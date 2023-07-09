#include "base_compute_actor.h"

#include <ydb/core/fq/libs/common/util.h>
#include <ydb/core/fq/libs/compute/common/metrics.h>
#include <ydb/core/fq/libs/compute/common/run_actor_params.h>
#include <ydb/core/fq/libs/compute/ydb/events/events.h>
#include <ydb/core/fq/libs/ydb/ydb.h>
#include <ydb/library/services/services.pb.h>

#include <ydb/library/yql/providers/common/metrics/service_counters.h>

#include <ydb/public/sdk/cpp/client/draft/ydb_query/client.h>
#include <ydb/public/sdk/cpp/client/ydb_operation/operation.h>

#include <library/cpp/actors/core/actor.h>
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/actorsystem.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/log.h>


#define LOG_E(stream) LOG_ERROR_S(*TlsActivationContext, NKikimrServices::FQ_RUN_ACTOR, "[ydb] [Finalizer] QueryId: " << Params.QueryId << " " << stream)
#define LOG_W(stream) LOG_WARN_S( *TlsActivationContext, NKikimrServices::FQ_RUN_ACTOR, "[ydb] [Finalizer] QueryId: " << Params.QueryId << " " << stream)
#define LOG_I(stream) LOG_INFO_S( *TlsActivationContext, NKikimrServices::FQ_RUN_ACTOR, "[ydb] [Finalizer] QueryId: " << Params.QueryId << " " << stream)
#define LOG_D(stream) LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::FQ_RUN_ACTOR, "[ydb] [Finalizer] QueryId: " << Params.QueryId << " " << stream)
#define LOG_T(stream) LOG_TRACE_S(*TlsActivationContext, NKikimrServices::FQ_RUN_ACTOR, "[ydb] [Finalizer] QueryId: " << Params.QueryId << " " << stream)

namespace NFq {

using namespace NActors;
using namespace NFq;

class TFinalizerActor : public TBaseComputeActor<TFinalizerActor> {
public:
    enum ERequestType {
        RT_PING,
        RT_MAX
    };

    class TCounters: public virtual TThrRefBase {
        std::array<TComputeRequestCountersPtr, RT_MAX> Requests = CreateArray<RT_MAX, TComputeRequestCountersPtr>({
            { MakeIntrusive<TComputeRequestCounters>("Ping") }
        });

        ::NMonitoring::TDynamicCounterPtr Counters;

    public:
        explicit TCounters(const ::NMonitoring::TDynamicCounterPtr& counters)
            : Counters(counters)
        {
            for (auto& request: Requests) {
                request->Register(Counters);
            }
        }

        TComputeRequestCountersPtr GetCounters(ERequestType type) {
            return Requests[type];
        }
    };

    TFinalizerActor(const TRunActorParams& params, const TActorId& parent, const TActorId& pinger, NYdb::NQuery::EExecStatus execStatus, const ::NYql::NCommon::TServiceCounters& queryCounters)
        : TBaseComputeActor(queryCounters, "Finalizer")
        , Params(params)
        , Parent(parent)
        , Pinger(pinger)
        , ExecStatus(execStatus)
        , Counters(GetStepCountersSubgroup())
        , StartTime(TInstant::Now())
    {}

    static constexpr char ActorName[] = "FQ_FINALIZER_ACTOR";

    void Start() {
        LOG_I("Start finalizer actor. Compute state: " << FederatedQuery::QueryMeta::ComputeStatus_Name(Params.Status));
        auto pingCounters = Counters.GetCounters(ERequestType::RT_PING);
        pingCounters->InFly->Inc();
        Become(&TFinalizerActor::StateFunc);
        Fq::Private::PingTaskRequest pingTaskRequest;
        if (ExecStatus == NYdb::NQuery::EExecStatus::Completed || Params.Status == FederatedQuery::QueryMeta::COMPLETING) {
            pingTaskRequest.mutable_result_id()->set_value(Params.ResultId);
        }
        pingTaskRequest.set_status(ExecStatus == NYdb::NQuery::EExecStatus::Completed || Params.Status == FederatedQuery::QueryMeta::COMPLETING ? ::FederatedQuery::QueryMeta::COMPLETED : ::FederatedQuery::QueryMeta::FAILED);
        Send(Pinger, new TEvents::TEvForwardPingRequest(pingTaskRequest, true));
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvents::TEvForwardPingResponse, Handle);
    )

    void Handle(const TEvents::TEvForwardPingResponse::TPtr& ev) {
        auto pingCounters = Counters.GetCounters(ERequestType::RT_PING);
        pingCounters->InFly->Dec();
        pingCounters->LatencyMs->Collect((TInstant::Now() - StartTime).MilliSeconds());
        if (ev.Get()->Get()->Success) {
            pingCounters->Ok->Inc();
            LOG_I("Query moved to terminal state ");
            Send(Parent, new TEvYdbCompute::TEvFinalizerResponse({}, NYdb::EStatus::SUCCESS));
            CompleteAndPassAway();
        } else {
            pingCounters->Error->Inc();
            LOG_E("Error moving the query to the terminal state");
            Send(Parent, new TEvYdbCompute::TEvFinalizerResponse(NYql::TIssues{NYql::TIssue{TStringBuilder{} << "Error moving the query to the terminal state"}}, NYdb::EStatus::INTERNAL_ERROR));
            FailedAndPassAway();
        }
    }

private:
    TRunActorParams Params;
    TActorId Parent;
    TActorId Pinger;
    NYdb::NQuery::EExecStatus ExecStatus;
    TCounters Counters;
    TInstant StartTime;
};

std::unique_ptr<NActors::IActor> CreateFinalizerActor(const TRunActorParams& params,
                                                      const TActorId& parent,
                                                      const TActorId& pinger,
                                                      NYdb::NQuery::EExecStatus execStatus,
                                                      const ::NYql::NCommon::TServiceCounters& queryCounters) {
    return std::make_unique<TFinalizerActor>(params, parent, pinger, execStatus, queryCounters);
}

}
