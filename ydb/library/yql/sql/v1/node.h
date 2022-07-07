#pragma once

#include <google/protobuf/message.h>
#include <ydb/library/yql/utils/resetable_setting.h>
#include <ydb/library/yql/parser/proto_ast/proto_ast.h>
#include <ydb/library/yql/public/udf/udf_data_type.h>
#include <ydb/library/yql/ast/yql_ast.h>
#include <ydb/library/yql/ast/yql_expr.h>
#include <util/generic/vector.h>
#include <util/generic/set.h>
#include <util/generic/map.h>
#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/generic/maybe.h>
#include <util/string/builder.h>

#include <library/cpp/enumbitset/enumbitset.h>

#include <array>
#include <functional>
#include <variant>

namespace NSQLTranslationV1 {
    constexpr const size_t SQL_MAX_INLINE_SCRIPT_LEN = 24;

    using NYql::TPosition;
    using NYql::TAstNode;

    enum class ENodeState {
        Begin,
        Precached = Begin,
        Initialized,
        CountHint,
        Const,
        MaybeConst,
        Aggregated,
        AggregationKey,
        OverWindow,
        Failed,
        End,
    };
    typedef TEnumBitSet<ENodeState, static_cast<int>(ENodeState::Begin), static_cast<int>(ENodeState::End)> TNodeState;

    enum class ESQLWriteColumnMode {
        InsertInto,
        InsertOrAbortInto,
        InsertOrIgnoreInto,
        InsertOrRevertInto,
        UpsertInto,
        ReplaceInto,
        InsertIntoWithTruncate,
        Update,
        Delete,
    };

    enum class EWriteColumnMode {
        Default,
        Insert,
        InsertOrAbort,
        InsertOrIgnore,
        InsertOrRevert,
        Upsert,
        Replace,
        Renew,
        Update,
        UpdateOn,
        Delete,
        DeleteOn,
    };

    enum class EAlterTableIntentnt {
        AddColumn,
        DropColumn
    };

    class TContext;
    class ITableKeys;
    class ISource;
    class IAggregation;
    typedef TIntrusivePtr<IAggregation> TAggregationPtr;

    struct TScopedState;
    typedef TIntrusivePtr<TScopedState> TScopedStatePtr;

    inline TString DotJoin(const TString& lhs, const TString& rhs) {
        TStringBuilder sb;
        sb << lhs << "." << rhs;
        return sb;
    }

    TString ErrorDistinctByGroupKey(const TString& column);
    TString ErrorDistinctWithoutCorrelation(const TString& column);

    class INode: public TSimpleRefCount<INode> {
    public:
        typedef TIntrusivePtr<INode> TPtr;

        struct TIdPart {
            TString Name;
            TPtr Expr;

            TIdPart(const TString& name)
                : Name(name)
            {
            }
            TIdPart(TPtr expr)
                : Expr(expr)
            {
            }
            TIdPart Clone() const {
                TIdPart res(Name);
                res.Expr = Expr ? Expr->Clone() : nullptr;
                return res;
            }
        };

    public:
        INode(TPosition pos);
        virtual ~INode();

        TPosition GetPos() const;
        const TString& GetLabel() const;
        TMaybe<TPosition> GetLabelPos() const;
        void SetLabel(const TString& label, TMaybe<TPosition> pos = {});
        bool IsImplicitLabel() const;
        void MarkImplicitLabel(bool isImplicitLabel);

        void SetCountHint(bool isCount);
        bool GetCountHint() const;
        bool Init(TContext& ctx, ISource* src);

        bool IsConstant() const;
        bool MaybeConstant() const;
        bool IsAggregated() const;
        bool IsAggregationKey() const;
        bool IsOverWindow() const;
        bool HasState(ENodeState state) const {
            PrecacheState();
            return State.Test(state);
        }

        virtual bool IsNull() const;
        virtual bool IsLiteral() const;
        virtual TString GetLiteralType() const;
        virtual TString GetLiteralValue() const;
        virtual bool IsIntegerLiteral() const;
        virtual TPtr ApplyUnaryOp(TContext& ctx, TPosition pos, const TString& opName) const;
        virtual bool IsAsterisk() const;
        virtual const TString* SubqueryAlias() const;
        virtual TString GetOpName() const;
        virtual const TString* GetLiteral(const TString& type) const;
        virtual const TString* GetColumnName() const;
        virtual void AssumeColumn();
        virtual const TString* GetSourceName() const;
        virtual const TString* GetAtomContent() const;
        virtual bool IsOptionalArg() const;
        virtual size_t GetTupleSize() const;
        virtual TPtr GetTupleElement(size_t index) const;
        virtual ITableKeys* GetTableKeys();
        virtual ISource* GetSource();
        virtual TVector<INode::TPtr>* ContentListPtr();
        virtual TAstNode* Translate(TContext& ctx) const = 0;
        virtual TAggregationPtr GetAggregation() const;
        virtual void CollectPreaggregateExprs(TContext& ctx, ISource& src, TVector<INode::TPtr>& exprs);
        virtual TPtr WindowSpecFunc(const TPtr& type) const;
        virtual bool SetViewName(TContext& ctx, TPosition pos, const TString& view);
        void UseAsInner();
        virtual bool UsedSubquery() const;
        virtual bool IsSelect() const;
        virtual const TString* FuncName() const;
        virtual const TString* ModuleName() const;

        using TVisitFunc = std::function<bool (const INode&)>;
        using TVisitNodeSet = std::unordered_set<const INode*>;

        void VisitTree(const TVisitFunc& func) const;
        void VisitTree(const TVisitFunc& func, TVisitNodeSet& visited) const;

        TPtr AstNode() const;
        TPtr AstNode(TAstNode* node) const;
        TPtr AstNode(TPtr node) const;
        TPtr AstNode(const TString& str) const;

        template <typename TVal, typename... TVals>
        void Add(TVal val, TVals... vals) {
            DoAdd(AstNode(val));
            Add(vals...);
        }

        void Add() {}

        // Y() Q() L()
        TPtr Y() const {
            return AstNode();
        }

        template <typename... TVals>
        TPtr Y(TVals... vals) const {
            TPtr node(AstNode());
            node->Add(vals...);
            return node;
        }

        template <typename T>
        TPtr Q(T a) const {
            return Y("quote", a);
        }

        template <typename... TVals>
        TPtr L(TPtr list, TVals... vals) const {
            Y_VERIFY_DEBUG(list);
            auto copy = list->ShallowCopy();
            copy->Add(vals...);
            return copy;
        }

        TPtr Clone() const;
    protected:
        virtual TPtr ShallowCopy() const;
        virtual void DoUpdateState() const;
        virtual TPtr DoClone() const = 0;
        void PrecacheState() const;

        virtual void DoVisitChildren(const TVisitFunc& func, TVisitNodeSet& visited) const;
    private:
        virtual bool DoInit(TContext& ctx, ISource* src);
        virtual void DoAdd(TPtr node);

    protected:
        TPosition Pos;
        TString Label;
        TMaybe<TPosition> LabelPos;
        bool ImplicitLabel = false;
        mutable TNodeState State;
        bool AsInner = false;
    };
    typedef INode::TPtr TNodePtr;

    using TTableHints = TMap<TString, TVector<TNodePtr>>;
    void MergeHints(TTableHints& base, const TTableHints& overrides);

    template<class T>
    inline T SafeClone(const T& node) {
        return node ? node->Clone() : nullptr;
    }

    template<class T>
    inline TVector<T> CloneContainer(const TVector<T>& args) {
        TVector<T> cloneArgs;
        cloneArgs.reserve(args.size());
        for (const auto& arg: args) {
            cloneArgs.emplace_back(SafeClone(arg));
        }
        return cloneArgs;
    }

    class TAstAtomNode: public INode {
    public:
        TAstAtomNode(TPosition pos, const TString& content, ui32 flags, bool isOptionalArg);

        ~TAstAtomNode() override;

        TAstNode* Translate(TContext& ctx) const override;
        const TString& GetContent() const {
            return Content;
        }

        const TString* GetAtomContent() const override;
        bool IsOptionalArg() const override;

    protected:
        TString Content;
        ui32 Flags;
        bool IsOptionalArg_;

        void DoUpdateState() const override;
    };

    class TAstAtomNodeImpl final: public TAstAtomNode {
    public:
        TAstAtomNodeImpl(TPosition pos, const TString& content, ui32 flags, bool isOptionalArg = false)
            : TAstAtomNode(pos, content, flags, isOptionalArg)
        {}

        TNodePtr DoClone() const final {
            return new TAstAtomNodeImpl(Pos, Content, Flags, IsOptionalArg_);
        }
    };

    class TAstDirectNode final: public INode {
    public:
        TAstDirectNode(TAstNode* node);

        TAstNode* Translate(TContext& ctx) const override;

        TPtr DoClone() const final {
            return new TAstDirectNode(Node);
        }
    protected:
        TAstNode* Node;
    };

    class TAstListNode: public INode {
    public:
        TAstListNode(TPosition pos);
        virtual ~TAstListNode();

        TAstNode* Translate(TContext& ctx) const override;

    protected:
        explicit TAstListNode(const TAstListNode& node);
        explicit TAstListNode(TPosition pos, TVector<TNodePtr>&& nodes);
        TPtr ShallowCopy() const override;
        bool DoInit(TContext& ctx, ISource* src) override;
        void DoAdd(TNodePtr node) override;
        void DoVisitChildren(const TVisitFunc& func, TVisitNodeSet& visited) const override;

        void DoUpdateState() const override;

        void UpdateStateByListNodes(const TVector<TNodePtr>& Nodes) const;

    protected:
        TVector<TNodePtr> Nodes;
        mutable TMaybe<bool> CacheGroupKey;
    };

    class TAstListNodeImpl final: public TAstListNode {
    public:
        TAstListNodeImpl(TPosition pos);
        TAstListNodeImpl(TPosition pos, TVector<TNodePtr> nodes);
        void CollectPreaggregateExprs(TContext& ctx, ISource& src, TVector<INode::TPtr>& exprs) override;

    protected:
        TNodePtr DoClone() const final;
    };

    class TCallNode: public TAstListNode {
    public:
        TCallNode(TPosition pos, const TString& opName, i32 minArgs, i32 maxArgs, const TVector<TNodePtr>& args);
        TCallNode(TPosition pos, const TString& opName, const TVector<TNodePtr>& args)
            : TCallNode(pos, opName, args.size(), args.size(), args)
        {}

        TString GetOpName() const override;
        const TString* GetSourceName() const override;

        const TVector<TNodePtr>& GetArgs() const;

    protected:
        bool DoInit(TContext& ctx, ISource* src) override;
        bool ValidateArguments(TContext& ctx) const;
        TString GetCallExplain() const;
        void CollectPreaggregateExprs(TContext& ctx, ISource& src, TVector<INode::TPtr>& exprs) override;

    protected:
        TString OpName;
        i32 MinArgs;
        i32 MaxArgs;
        TVector<TNodePtr> Args;
        mutable TMaybe<bool> CacheGroupKey;

        void DoUpdateState() const override;
    };

    class TCallNodeImpl final: public TCallNode {
        TPtr DoClone() const final;
    public:
        TCallNodeImpl(TPosition pos, const TString& opName, i32 minArgs, i32 maxArgs, const TVector<TNodePtr>& args);
        TCallNodeImpl(TPosition pos, const TString& opName, const TVector<TNodePtr>& args);
    };

    class TFuncNodeImpl final : public TCallNode {
        TPtr DoClone() const final;
    public:
        TFuncNodeImpl(TPosition pos, const TString& opName);
        const TString* FuncName() const override;
    };

    class TCallNodeDepArgs final : public TCallNode {
        TPtr DoClone() const final;
    public:
        TCallNodeDepArgs(ui32 reqArgsCount, TPosition pos, const TString& opName, i32 minArgs, i32 maxArgs, const TVector<TNodePtr>& args);
        TCallNodeDepArgs(ui32 reqArgsCount, TPosition pos, const TString& opName, const TVector<TNodePtr>& args);
    protected:
        bool DoInit(TContext& ctx, ISource* src) override;

    private:
        const ui32 ReqArgsCount;
    };

    class TCallDirectRow final : public TCallNode {
        TPtr DoClone() const final;
    public:
        TCallDirectRow(TPosition pos, const TString& opName, const TVector<TNodePtr>& args);
    protected:
        bool DoInit(TContext& ctx, ISource* src) override;
        void DoUpdateState() const override;
    };

    class TWinAggrEmulation: public TCallNode {
    protected:
        void DoUpdateState() const override;
        bool DoInit(TContext& ctx, ISource* src) override;
        TPtr WindowSpecFunc(const TNodePtr& type) const override;
    public:
        TWinAggrEmulation(TPosition pos, const TString& opName, i32 minArgs, i32 maxArgs, const TVector<TNodePtr>& args);
    protected:
        template<class TNodeType>
        TPtr CallNodeClone() const {
            return new TNodeType(GetPos(), OpName, MinArgs, MaxArgs, CloneContainer(Args));
        }
        TString FuncAlias;
    };

    using TFunctionConfig = TMap<TString, TNodePtr>;

    class TExternalFunctionConfig final: public TAstListNode {
    public:
        TExternalFunctionConfig(TPosition pos, const TFunctionConfig& config)
        : TAstListNode(pos)
        , Config(config)
        {
        }

        bool DoInit(TContext& ctx, ISource* src) override;
        TPtr DoClone() const final;

    private:
        TFunctionConfig Config;
    };

    class TWinRowNumber final: public TWinAggrEmulation {
        TPtr DoClone() const final {
            return CallNodeClone<TWinRowNumber>();
        }
    public:
        TWinRowNumber(TPosition pos, const TString& opName, i32 minArgs, i32 maxArgs, const TVector<TNodePtr>& args);
    };

    class TWinLeadLag final: public TWinAggrEmulation {
        TPtr DoClone() const final {
            return CallNodeClone<TWinLeadLag>();
        }
        bool DoInit(TContext& ctx, ISource* src) override;
    public:
        TWinLeadLag(TPosition pos, const TString& opName, i32 minArgs, i32 maxArgs, const TVector<TNodePtr>& args);
    };

    class TWinRank final: public TWinAggrEmulation {
        TPtr DoClone() const final {
            return CallNodeClone<TWinRank>();
        }
        bool DoInit(TContext& ctx, ISource* src) override;
    public:
        TWinRank(TPosition pos, const TString& opName, i32 minArgs, i32 maxArgs, const TVector<TNodePtr>& args);
    };

    class ITableKeys: public INode {
    public:
        enum class EBuildKeysMode {
            CREATE,
            DROP,
            INPUT,
            WRITE
        };

        ITableKeys(TPosition pos);
        virtual const TString* GetTableName() const;
        virtual TNodePtr BuildKeys(TContext& ctx, EBuildKeysMode mode) = 0;

    private:
        /// all TableKeys no clonnable
        TPtr DoClone() const final {
            return {};
        }

        ITableKeys* GetTableKeys() override;
        TAstNode* Translate(TContext& ctx) const override;
    };

    enum class ESampleMode {
        Auto,
        Bernoulli,
        System
    };

    class TDeferredAtom {
    public:
        TDeferredAtom();
        TDeferredAtom(TPosition pos, const TString& str);
        TDeferredAtom(TNodePtr node, TContext& ctx);
        const TString* GetLiteral() const;
        bool GetLiteral(TString& value, TContext& ctx) const;
        TNodePtr Build() const;
        TString GetRepr() const;
        bool Empty() const;

    private:
        TMaybe<TString> Explicit;
        TNodePtr Node; // atom or evaluation node
        TString Repr;
    };

    typedef TIntrusivePtr<ISource> TSourcePtr;

    struct TTableRef {
        TString RefName;
        TString Service;
        TDeferredAtom Cluster;
        TNodePtr Keys;
        TNodePtr Options;
        TSourcePtr Source;

        TTableRef() = default;
        TTableRef(const TString& refName, const TString& service, const TDeferredAtom& cluster, TNodePtr keys);
        TTableRef(const TTableRef&) = default;
        TTableRef& operator=(const TTableRef&) = default;

        TString ShortName() const;
    };

    struct TIdentifier {
        TPosition Pos;
        TString Name;

        TIdentifier(TPosition pos, const TString& name)
            : Pos(pos)
            , Name(name) {}
    };

    struct TColumnSchema {
        TPosition Pos;
        TString Name;
        TNodePtr Type;
        bool Nullable;
        TVector<TIdentifier> Families;

        TColumnSchema(TPosition pos, const TString& name, const TNodePtr& type, bool nullable,
            TVector<TIdentifier> families);
    };

    struct TColumns: public TSimpleRefCount<TColumns> {
        TSet<TString> Real;
        TSet<TString> Artificial;
        TVector<TString> List;
        TVector<bool> NamedColumns;
        bool All = false;
        bool QualifiedAll = false;
        bool HasUnreliable = false;

        bool Add(const TString* column, bool countHint, bool isArtificial = false, bool isReliable = true, bool hasName = true);
        void Merge(const TColumns& columns);
        void SetPrefix(const TString& prefix);
        void SetAll();
        bool IsColumnPossible(TContext& ctx, const TString& column);
    };

    struct TSortSpecification: public TSimpleRefCount<TSortSpecification> {
        TNodePtr OrderExpr;
        bool Ascending;
        TIntrusivePtr<TSortSpecification> Clone() const;
        ~TSortSpecification() {}
    };
    typedef TIntrusivePtr<TSortSpecification> TSortSpecificationPtr;

    enum EFrameType {
        FrameByRows,
        FrameByRange,
        FrameByGroups,
    };
    enum EFrameExclusions {
        FrameExclNone, // same as EXCLUDE NO OTHERS
        FrameExclCurRow,
        FrameExclGroup,
        FrameExclTies,
    };
    enum EFrameSettings {
        // keep order
        FrameUndefined,
        FramePreceding,
        FrameCurrentRow,
        FrameFollowing,
    };

    struct TFrameBound: public TSimpleRefCount<TFrameBound> {
        TPosition Pos;
        TNodePtr Bound;
        EFrameSettings Settings = FrameUndefined;

        TIntrusivePtr<TFrameBound> Clone() const;
        ~TFrameBound() {}
    };
    typedef TIntrusivePtr<TFrameBound> TFrameBoundPtr;


    struct TFrameSpecification: public TSimpleRefCount<TFrameSpecification> {
        EFrameType FrameType = FrameByRows;
        TFrameBoundPtr FrameBegin;
        TFrameBoundPtr FrameEnd;
        EFrameExclusions FrameExclusion = FrameExclNone;

        TIntrusivePtr<TFrameSpecification> Clone() const;
        ~TFrameSpecification() {}
    };
    typedef TIntrusivePtr<TFrameSpecification> TFrameSpecificationPtr;

    struct THoppingWindowSpec: public TSimpleRefCount<THoppingWindowSpec> {
        TNodePtr TimeExtractor;
        TNodePtr Hop;
        TNodePtr Interval;
        TNodePtr Delay;
        bool DataWatermarks;

        TIntrusivePtr<THoppingWindowSpec> Clone() const;
        ~THoppingWindowSpec() {}
    };
    typedef TIntrusivePtr<THoppingWindowSpec> THoppingWindowSpecPtr;

    struct TWindowSpecification: public TSimpleRefCount<TWindowSpecification> {
        TMaybe<TString> ExistingWindowName;
        TVector<TNodePtr> Partitions;
        bool IsCompact = false;
        TVector<TSortSpecificationPtr> OrderBy;
        TNodePtr Session;
        TFrameSpecificationPtr Frame;

        TIntrusivePtr<TWindowSpecification> Clone() const;
        ~TWindowSpecification() {}
    };
    typedef TIntrusivePtr<TWindowSpecification> TWindowSpecificationPtr;
    typedef TMap<TString, TWindowSpecificationPtr> TWinSpecs;

    typedef TVector<TTableRef> TTableList;

    void WarnIfAliasFromSelectIsUsedInGroupBy(TContext& ctx, const TVector<TNodePtr>& selectTerms, const TVector<TNodePtr>& groupByTerms,
        const TVector<TNodePtr>& groupByExprTerms);
    bool ValidateAllNodesForAggregation(TContext& ctx, const TVector<TNodePtr>& nodes);

    struct TWriteSettings {
        bool Discard = false;
        TDeferredAtom Label;
    };

    class TColumnNode final: public INode {
    public:
        TColumnNode(TPosition pos, const TString& column, const TString& source, bool maybeType);
        TColumnNode(TPosition pos, const TNodePtr& column, const TString& source);

        virtual ~TColumnNode();
        bool IsAsterisk() const override;
        virtual bool IsArtificial() const;
        const TString* GetColumnName() const override;
        const TString* GetSourceName() const override;
        TAstNode* Translate(TContext& ctx) const override;
        void ResetColumn(const TString& column, const TString& source);
        void ResetColumn(const TNodePtr& column, const TString& source);

        void SetUseSourceAsColumn();
        void SetUseSource();
        void ResetAsReliable();
        void SetAsNotReliable();
        bool IsReliable() const;
        bool IsUseSourceAsColumn() const;
        bool CanBeType() const;

    private:
        bool DoInit(TContext& ctx, ISource* src) override;
        TPtr DoClone() const final;

        void DoUpdateState() const override;

    private:
        static const TString Empty;
        TNodePtr Node;
        TString ColumnName;
        TNodePtr ColumnExpr;
        TString Source;
        bool GroupKey = false;
        bool Artificial = false;
        bool Reliable = true;
        bool UseSource = false;
        bool UseSourceAsColumn = false;
        bool MaybeType = false;
    };

    class TArgPlaceholderNode final: public INode
    {
    public:
        TArgPlaceholderNode(TPosition pos, const TString &name);

        TAstNode* Translate(TContext& ctx) const override;

        TString GetName() const;
        TNodePtr DoClone() const final;

    protected:
        bool DoInit(TContext& ctx, ISource* src) override;

    private:
        TString Name;
    };

    enum class EAggregateMode {
        Normal,
        Distinct,
        OverWindow,
    };

    class TTupleNode: public TAstListNode {
    public:
        TTupleNode(TPosition pos, const TVector<TNodePtr>& exprs);

        bool IsEmpty() const;
        const TVector<TNodePtr>& Elements() const;
        bool DoInit(TContext& ctx, ISource* src) override;
        size_t GetTupleSize() const override;
        TPtr GetTupleElement(size_t index) const override;
        TNodePtr DoClone() const final;
    private:
        void CollectPreaggregateExprs(TContext& ctx, ISource& src, TVector<INode::TPtr>& exprs) override;
        const TString* GetSourceName() const override;

        const TVector<TNodePtr> Exprs;
    };

    class TStructNode: public TAstListNode {
    public:
        TStructNode(TPosition pos, const TVector<TNodePtr>& exprs, const TVector<TNodePtr>& labels, bool ordered);

        bool DoInit(TContext& ctx, ISource* src) override;
        TNodePtr DoClone() const final;
        const TVector<TNodePtr>& GetExprs() {
            return Exprs;
        }

    private:
        void CollectPreaggregateExprs(TContext& ctx, ISource& src, TVector<INode::TPtr>& exprs) override;
        const TString* GetSourceName() const override;

        const TVector<TNodePtr> Exprs;
        const TVector<TNodePtr> Labels;
        const bool Ordered;
    };

    class IAggregation: public INode {
    public:
        bool IsDistinct() const;

        void DoUpdateState() const override;

        virtual const TString* GetGenericKey() const;

        virtual bool InitAggr(TContext& ctx, bool isFactory, ISource* src, TAstListNode& node, const TVector<TNodePtr>& exprs) = 0;

        virtual TNodePtr AggregationTraits(const TNodePtr& type) const;

        virtual TNodePtr AggregationTraitsFactory() const = 0;

        virtual std::vector<ui32> GetFactoryColumnIndices() const;

        virtual void AddFactoryArguments(TNodePtr& apply) const;

        virtual TNodePtr WindowTraits(const TNodePtr& type) const;

        const TString& GetName() const;

        EAggregateMode GetAggregationMode() const;
        void MarkKeyColumnAsGenerated();

        virtual void Join(IAggregation* aggr);

    private:
        virtual TNodePtr GetApply(const TNodePtr& type) const = 0;

    protected:
        IAggregation(TPosition pos, const TString& name, const TString& func, EAggregateMode mode);
        TAstNode* Translate(TContext& ctx) const override;

        TString Name;
        TString Func;
        const EAggregateMode AggMode;
        TString DistinctKey;
        bool IsGeneratedKeyColumn = false;
    };

    enum class EExprSeat: int {
        Open = 0,
        FlattenByExpr,
        FlattenBy,
        GroupBy,
        DistinctAggr,
        WindowPartitionBy,
        Max
    };

    enum class EExprType: int {
        WithExpression,
        ColumnOnly,
    };

    enum class EOrderKind: int {
        None,
        Sort,
        Assume,
        Passthrough
    };

    class IJoin;
    class ISource: public INode {
    public:
        virtual ~ISource();

        virtual bool IsFake() const;
        virtual void AllColumns();
        virtual const TColumns* GetColumns() const;
        virtual void GetInputTables(TTableList& tableList) const;
        /// in case of error unfilled, flag show if ensure column name
        virtual TMaybe<bool> AddColumn(TContext& ctx, TColumnNode& column);
        virtual void FinishColumns();
        virtual bool AddExpressions(TContext& ctx, const TVector<TNodePtr>& columns, EExprSeat exprSeat);
        virtual void SetFlattenByMode(const TString& mode);
        virtual void MarkFlattenColumns();
        virtual bool IsFlattenColumns() const;
        virtual bool AddFilter(TContext& ctx, TNodePtr filter);
        virtual bool AddGroupKey(TContext& ctx, const TString& column);
        virtual void SetCompactGroupBy(bool compactGroupBy);
        virtual TString MakeLocalName(const TString& name);
        virtual bool AddAggregation(TContext& ctx, TAggregationPtr aggr);
        virtual bool AddFuncOverWindow(TContext& ctx, TNodePtr expr);
        virtual void AddTmpWindowColumn(const TString& column);
        virtual const TVector<TString>& GetTmpWindowColumns() const;
        virtual bool HasAggregations() const;
        virtual void AddWindowSpecs(TWinSpecs winSpecs);
        virtual bool AddAggregationOverWindow(TContext& ctx, const TString& windowName, TAggregationPtr func);
        virtual bool AddFuncOverWindow(TContext& ctx, const TString& windowName, TNodePtr func);
        virtual void SetHoppingWindowSpec(THoppingWindowSpecPtr spec);
        virtual THoppingWindowSpecPtr GetHoppingWindowSpec() const;
        virtual TNodePtr GetSessionWindowSpec() const;
        virtual bool IsCompositeSource() const;
        virtual bool IsGroupByColumn(const TString& column) const;
        virtual bool IsFlattenByColumns() const;
        virtual bool IsFlattenByExprs() const;
        virtual bool IsCalcOverWindow() const;
        virtual bool IsOverWindowSource() const;
        virtual bool IsStream() const;
        virtual EOrderKind GetOrderKind() const;
        virtual TWriteSettings GetWriteSettings() const;
        virtual bool SetSamplingOptions(TContext& ctx, TPosition pos, ESampleMode mode, TNodePtr samplingRate, TNodePtr samplingSeed);
        virtual bool SetTableHints(TContext& ctx, TPosition pos, const TTableHints& hints, const TTableHints& contextHints);
        virtual bool CalculateGroupingHint(TContext& ctx, const TVector<TString>& columns, ui64& hint) const;
        virtual TNodePtr BuildFilter(TContext& ctx, const TString& label);
        virtual TNodePtr BuildFilterLambda();
        virtual TNodePtr BuildFlattenByColumns(const TString& label);
        virtual TNodePtr BuildFlattenColumns(const TString& label);
        virtual TNodePtr BuildPreaggregatedMap(TContext& ctx);
        virtual TNodePtr BuildPreFlattenMap(TContext& ctx);
        virtual TNodePtr BuildPrewindowMap(TContext& ctx);
        virtual TNodePtr BuildAggregation(const TString& label);
        virtual TNodePtr BuildCalcOverWindow(TContext& ctx, const TString& label);
        virtual TNodePtr BuildSort(TContext& ctx, const TString& label);
        virtual TNodePtr BuildCleanupColumns(TContext& ctx, const TString& label);
        virtual bool BuildSamplingLambda(TNodePtr& node);
        virtual bool SetSamplingRate(TContext& ctx, TNodePtr samplingRate);
        virtual IJoin* GetJoin();
        virtual ISource* GetCompositeSource();
        virtual bool IsSelect() const;
        virtual bool IsTableSource() const;
        virtual bool ShouldUseSourceAsColumn(const TString& source) const;
        virtual bool IsJoinKeysInitializing() const;
        virtual const TString* GetWindowName() const;

        virtual bool DoInit(TContext& ctx, ISource* src);
        virtual TNodePtr Build(TContext& ctx) = 0;

        virtual TMaybe<TString> FindColumnMistype(const TString& name) const;

        virtual bool InitFilters(TContext& ctx);
        void AddDependentSource(ISource* usedSource);
        bool IsAlias(EExprSeat exprSeat, const TString& label) const;
        bool IsExprAlias(const TString& label) const;
        bool IsExprSeat(EExprSeat exprSeat, EExprType type = EExprType::WithExpression) const;
        TString GetGroupByColumnAlias(const TString& column) const;
        const TVector<TNodePtr>& Expressions(EExprSeat exprSeat) const;

        virtual TWindowSpecificationPtr FindWindowSpecification(TContext& ctx, const TString& windowName) const;

        TIntrusivePtr<ISource> CloneSource() const;

    protected:
        ISource(TPosition pos);
        virtual TAstNode* Translate(TContext& ctx) const;

        void FillSortParts(const TVector<TSortSpecificationPtr>& orderBy, TNodePtr& sortKeySelector, TNodePtr& sortDirection);
        TNodePtr BuildSortSpec(const TVector<TSortSpecificationPtr>& orderBy, const TString& label, bool traits, bool assume);

        TVector<TNodePtr>& Expressions(EExprSeat exprSeat);
        TNodePtr AliasOrColumn(const TNodePtr& node, bool withSource);

        TNodePtr BuildWindowFrame(const TFrameSpecification& spec, bool isCompact);

        THashSet<TString> ExprAliases;
        THashSet<TString> FlattenByAliases;
        THashMap<TString, TString> GroupByColumnAliases;
        TVector<TNodePtr> Filters;
        bool CompactGroupBy = false;
        TSet<TString> GroupKeys;
        TVector<TString> OrderedGroupKeys;
        std::array<TVector<TNodePtr>, static_cast<unsigned>(EExprSeat::Max)> NamedExprs;
        TVector<TAggregationPtr> Aggregations;
        TMap<TString, TVector<TAggregationPtr>> AggregationOverWindow;
        TMap<TString, TVector<TNodePtr>> FuncOverWindow;
        TWinSpecs WinSpecs;
        THoppingWindowSpecPtr HoppingWindowSpec;
        TNodePtr SessionWindow;
        TVector<ISource*> UsedSources;
        TString FlattenMode;
        bool FlattenColumns = false;
        THashMap<TString, ui32> GenIndexes;
        TVector<TString> TmpWindowColumns;
        TNodePtr SamplingRate;
    };

    template<>
    inline TVector<TSourcePtr> CloneContainer<TSourcePtr>(const TVector<TSourcePtr>& args) {
        TVector<TSourcePtr> cloneArgs;
        cloneArgs.reserve(args.size());
        for (const auto& arg: args) {
            cloneArgs.emplace_back(arg ? arg->CloneSource() : nullptr);
        }
        return cloneArgs;
    }

    struct TJoinLinkSettings {
        bool ForceSortedMerge = false;
    };

    class IJoin: public ISource {
    public:
        virtual ~IJoin();

        virtual IJoin* GetJoin();
        virtual TNodePtr BuildJoinKeys(TContext& ctx, const TVector<TDeferredAtom>& names) = 0;
        virtual void SetupJoin(const TString& joinOp, TNodePtr joinExpr, const TJoinLinkSettings& linkSettings) = 0;
        virtual const THashMap<TString, THashSet<TString>>& GetSameKeysMap() const = 0;
        virtual TVector<TString> GetJoinLabels() const = 0;

    protected:
        IJoin(TPosition pos);
    };

    class TListOfNamedNodes final: public INode {
    public:
        TListOfNamedNodes(TPosition pos, TVector<TNodePtr>&& exprs);

        TVector<TNodePtr>* ContentListPtr() override;
        TAstNode* Translate(TContext& ctx) const override;
        TPtr DoClone() const final;
        void DoVisitChildren(const TVisitFunc& func, TVisitNodeSet& visited) const final;
    private:
        TVector<TNodePtr> Exprs;
        TString Meaning;
    };

    class TLiteralNode: public TAstListNode {
    public:
        TLiteralNode(TPosition pos, bool isNull);
        TLiteralNode(TPosition pos, const TString& type, const TString& value);
        TLiteralNode(TPosition pos, const TString& value, ui32 nodeFlags);
        TLiteralNode(TPosition pos, const TString& value, ui32 nodeFlags, const TString& type);
        bool IsNull() const override;
        const TString* GetLiteral(const TString& type) const override;
        void DoUpdateState() const override;
        TPtr DoClone() const override;
        bool IsLiteral() const override;
        TString GetLiteralType() const override;
        TString GetLiteralValue() const override;
    protected:
        bool Null;
        bool Void;
        TString Type;
        TString Value;
    };

    class TAsteriskNode: public INode {
    public:
        TAsteriskNode(TPosition pos);
        bool IsAsterisk() const override;
        TPtr DoClone() const override;
        TAstNode* Translate(TContext& ctx) const override;
    };

    template<typename T>
    class TLiteralNumberNode: public TLiteralNode {
    public:
        TLiteralNumberNode(TPosition pos, const TString& type, const TString& value, bool implicitType = false);
        TPtr DoClone() const override final;
        bool DoInit(TContext& ctx, ISource* src) override;
        bool IsIntegerLiteral() const override;
        TPtr ApplyUnaryOp(TContext& ctx, TPosition pos, const TString& opName) const override;
    private:
        const bool ImplicitType;
    };

    struct TTableArg {
        bool HasAt = false;
        TNodePtr Expr;
        TDeferredAtom Id;
        TString View;
    };

    class TTableRows final : public INode {
    public:
        TTableRows(TPosition pos, const TVector<TNodePtr>& args);
        TTableRows(TPosition pos, ui32 argsCount);

        bool DoInit(TContext& ctx, ISource* src) override;

        void DoUpdateState() const override;

        TNodePtr DoClone() const final;
        TAstNode* Translate(TContext& ctx) const override;

    private:
        ui32 ArgsCount;
        TNodePtr Node;
    };

    class TSessionWindow final : public INode {
    public:
        TSessionWindow(TPosition pos, const TVector<TNodePtr>& args);
        void MarkValid();
        TNodePtr BuildTraits(const TString& label) const;
    private:
        bool DoInit(TContext& ctx, ISource* src) override;
        TAstNode* Translate(TContext&) const override;
        void DoUpdateState() const override;
        TNodePtr DoClone() const override;
        TString GetOpName() const override;

        TVector<TNodePtr> Args;
        TSourcePtr FakeSource;
        TNodePtr Node;
        bool Valid;
    };

    struct TStringContent {
        TString Content;
        NYql::NUdf::EDataSlot Type = NYql::NUdf::EDataSlot::String;
        TMaybe<TString> PgType;
        ui32 Flags = NYql::TNodeFlags::Default;
    };

    TMaybe<TStringContent> StringContent(TContext& ctx, TPosition pos, const TString& input);
    TMaybe<TStringContent> StringContentOrIdContent(TContext& ctx, TPosition pos, const TString& input);

    struct TTtlSettings {
        TIdentifier ColumnName;
        TNodePtr Expr;

        TTtlSettings(const TIdentifier& columnName, const TNodePtr& expr);
    };

    struct TTableSettings {
        TNodePtr CompactionPolicy;
        TMaybe<TIdentifier> AutoPartitioningBySize;
        TNodePtr PartitionSizeMb;
        TMaybe<TIdentifier> AutoPartitioningByLoad;
        TNodePtr MinPartitions;
        TNodePtr MaxPartitions;
        TNodePtr UniformPartitions;
        TVector<TVector<TNodePtr>> PartitionAtKeys;
        TMaybe<TIdentifier> KeyBloomFilter;
        TNodePtr ReadReplicasSettings;
        NYql::TResetableSetting<TTtlSettings, void> TtlSettings;

        bool IsSet() const {
            return CompactionPolicy || AutoPartitioningBySize || PartitionSizeMb || AutoPartitioningByLoad
                || MinPartitions || MaxPartitions || UniformPartitions || PartitionAtKeys || KeyBloomFilter
                || ReadReplicasSettings || TtlSettings;
        }
    };

    struct TFamilyEntry {
        TFamilyEntry(const TIdentifier& name)
            :Name(name)
        {}

        TIdentifier Name;
        TNodePtr Data;
        TNodePtr Compression;
    };

    struct TIndexDescription {
        enum class EType {
            GlobalSync,
            GlobalAsync,
        };

        TIndexDescription(const TIdentifier& name, EType type = EType::GlobalSync)
            : Name(name)
            , Type(type)
        {}

        TIdentifier Name;
        EType Type;
        TVector<TIdentifier> IndexColumns;
        TVector<TIdentifier> DataColumns;
    };

    struct TChangefeedSettings {
        struct TLocalSinkSettings {
            // no special settings
        };

        TNodePtr Mode;
        TNodePtr Format;
        std::optional<std::variant<TLocalSinkSettings>> SinkSettings;
    };

    struct TChangefeedDescription {
        TChangefeedDescription(const TIdentifier& name)
            : Name(name)
            , Disable(false)
        {}

        TIdentifier Name;
        TChangefeedSettings Settings;
        bool Disable;
    };

    struct TCreateTableParameters {
        TVector<TColumnSchema> Columns;
        TVector<TIdentifier> PkColumns;
        TVector<TIdentifier> PartitionByColumns;
        TVector<std::pair<TIdentifier, bool>> OrderByColumns;
        TVector<TIndexDescription> Indexes;
        TVector<TFamilyEntry> ColumnFamilies;
        TVector<TChangefeedDescription> Changefeeds;
        TTableSettings TableSettings;
    };

    struct TAlterTableParameters {
        TVector<TColumnSchema> AddColumns;
        TVector<TString> DropColumns;
        TVector<TColumnSchema> AlterColumns;
        TVector<TFamilyEntry> AddColumnFamilies;
        TVector<TFamilyEntry> AlterColumnFamilies;
        TTableSettings TableSettings;
        TVector<TIndexDescription> AddIndexes;
        TVector<TIdentifier> DropIndexes;
        TMaybe<TIdentifier> RenameTo;
        TVector<TChangefeedDescription> AddChangefeeds;
        TVector<TChangefeedDescription> AlterChangefeeds;
        TVector<TIdentifier> DropChangefeeds;
        TMaybe<std::pair<TIdentifier, TIdentifier>> RenameIndexTo;

        bool IsEmpty() const {
            return AddColumns.empty() && DropColumns.empty() && AlterColumns.empty()
                && AddColumnFamilies.empty() && AlterColumnFamilies.empty()
                && !TableSettings.IsSet()
                && AddIndexes.empty() && DropIndexes.empty()
                && !RenameTo.Defined()
                && AddChangefeeds.empty() && AlterChangefeeds.empty() && DropChangefeeds.empty()
                && !RenameIndexTo.Defined();
        }
    };

    struct TRoleParameters {
        TMaybe<TDeferredAtom> Password;
        bool IsPasswordEncrypted = false;
    };

    TString IdContent(TContext& ctx, const TString& str);
    TString IdContentFromString(TContext& ctx, const TString& str);
    TTableHints GetContextHints(TContext& ctx);

    TString TypeByAlias(const TString& alias, bool normalize = true);

    TNodePtr BuildAtom(TPosition pos, const TString& content, ui32 flags = NYql::TNodeFlags::ArbitraryContent,
        bool isOptionalArg = false);
    TNodePtr BuildQuotedAtom(TPosition pos, const TString& content, ui32 flags = NYql::TNodeFlags::ArbitraryContent);

    TNodePtr BuildLiteralNull(TPosition pos);
    TNodePtr BuildLiteralVoid(TPosition pos);
    /// String is checked as quotable, support escaping and multiline
    TNodePtr BuildLiteralSmartString(TContext& ctx, const TString& value);

    struct TExprOrIdent {
        TNodePtr Expr;
        TString Ident;
    };
    TMaybe<TExprOrIdent> BuildLiteralTypedSmartStringOrId(TContext& ctx, const TString& value);

    TNodePtr BuildLiteralRawString(TPosition pos, const TString& value, bool isUtf8 = false);
    TNodePtr BuildLiteralBool(TPosition pos, bool value);
    TNodePtr BuildEmptyAction(TPosition pos);

    TNodePtr BuildTuple(TPosition pos, const TVector<TNodePtr>& exprs);

    TNodePtr BuildStructure(TPosition pos, const TVector<TNodePtr>& exprs);
    TNodePtr BuildStructure(TPosition pos, const TVector<TNodePtr>& exprsUnlabeled, const TVector<TNodePtr>& labels);
    TNodePtr BuildOrderedStructure(TPosition pos, const TVector<TNodePtr>& exprsUnlabeled, const TVector<TNodePtr>& labels);

    TNodePtr BuildListOfNamedNodes(TPosition pos, TVector<TNodePtr>&& exprs);

    TNodePtr BuildArgPlaceholder(TPosition pos, const TString& name);

    TNodePtr BuildColumn(TPosition pos, const TString& column = TString(), const TString& source = TString());
    TNodePtr BuildColumn(TPosition pos, const TNodePtr& column, const TString& source = TString());
    TNodePtr BuildColumn(TPosition pos, const TDeferredAtom& column, const TString& source = TString());
    TNodePtr BuildColumnOrType(TPosition pos, const TString& column = TString());
    TNodePtr BuildAccess(TPosition pos, const TVector<INode::TIdPart>& ids, bool isLookup);
    TNodePtr BuildBind(TPosition pos, const TString& module, const TString& alias);
    TNodePtr BuildLambda(TPosition pos, TNodePtr params, TNodePtr body, const TString& resName = TString());
    TNodePtr BuildLambda(TPosition pos, TNodePtr params, const TVector<TNodePtr>& bodies);
    TNodePtr BuildDataType(TPosition pos, const TString& typeName);
    TMaybe<TString> LookupSimpleType(const TStringBuf& alias, bool flexibleTypes, bool isPgType);
    TNodePtr BuildSimpleType(TContext& ctx, TPosition pos, const TString& typeName, bool dataOnly);
    TNodePtr BuildIsNullOp(TPosition pos, TNodePtr a);
    TNodePtr BuildBinaryOp(TContext& ctx, TPosition pos, const TString& opName, TNodePtr a, TNodePtr b);

    TNodePtr BuildCalcOverWindow(TPosition pos, const TString& windowName, TNodePtr call);
    TNodePtr BuildYsonOptionsNode(TPosition pos, bool autoConvert, bool strict, bool fastYson);

    TNodePtr BuildDoCall(TPosition pos, const TNodePtr& node);
    TNodePtr BuildTupleResult(TNodePtr tuple, int ensureTupleSize);

    // Implemented in aggregation.cpp
    TAggregationPtr BuildFactoryAggregation(TPosition pos, const TString& name, const TString& func, EAggregateMode aggMode, bool multi = false);
    TAggregationPtr BuildKeyPayloadFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    TAggregationPtr BuildPayloadPredicateFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    TAggregationPtr BuildTwoArgsFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    TAggregationPtr BuildHistogramFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    TAggregationPtr BuildLinearHistogramFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    template <bool HasKey>
    TAggregationPtr BuildTopFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    TAggregationPtr BuildTopFreqFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    TAggregationPtr BuildCountDistinctEstimateFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    TAggregationPtr BuildListFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    TAggregationPtr BuildPercentileFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    TAggregationPtr BuildCountAggregation(TPosition pos, const TString& name, const TString& func, EAggregateMode aggMode);
    TAggregationPtr BuildUserDefinedFactoryAggregation(TPosition pos, const TString& name, const TString& factory, EAggregateMode aggMode);
    TAggregationPtr BuildPGFactoryAggregation(TPosition pos, const TString& name, EAggregateMode aggMode);


    // Implemented in builtin.cpp
    TNodePtr BuildCallable(TPosition pos, const TString& module, const TString& name, const TVector<TNodePtr>& args, bool forReduce = false);
    TNodePtr BuildUdf(TContext& ctx, TPosition pos, const TString& module, const TString& name, const TVector<TNodePtr>& args);
    TNodePtr BuildBuiltinFunc(
        TContext& ctx,
        TPosition pos,
        TString name,
        const TVector<TNodePtr>& args,
        const TString& nameSpace = TString(),
        EAggregateMode aggMode = EAggregateMode::Normal,
        bool* mustUseNamed = nullptr,
        bool warnOnYqlNameSpace = true
    );

    // Implemented in join.cpp
    TString NormalizeJoinOp(const TString& joinOp);
    TSourcePtr BuildEquiJoin(TPosition pos, TVector<TSourcePtr>&& sources, TVector<bool>&& anyFlags, bool strictJoinKeyTypes);

    // Implemented in select.cpp
    TNodePtr BuildSubquery(TSourcePtr source, const TString& alias, bool inSubquery, int ensureTupleSize, TScopedStatePtr scoped);
    TNodePtr BuildSubqueryRef(TNodePtr subquery, const TString& alias, int tupleIndex = -1);
    TNodePtr BuildSourceNode(TPosition pos, TSourcePtr source, bool checkExist = false);
    TSourcePtr BuildMuxSource(TPosition pos, TVector<TSourcePtr>&& sources);
    TSourcePtr BuildFakeSource(TPosition pos, bool missingFrom = false);
    TSourcePtr BuildNodeSource(TPosition pos, const TNodePtr& node, bool wrapToList = false);
    TSourcePtr BuildTableSource(TPosition pos, const TTableRef& table, const TString& label = TString());
    TSourcePtr BuildInnerSource(TPosition pos, TNodePtr node, const TString& service, const TDeferredAtom& cluster, const TString& label = TString());
    TSourcePtr BuildRefColumnSource(TPosition pos, const TString& partExpression);
    TSourcePtr BuildUnionAll(TPosition pos, TVector<TSourcePtr>&& sources, const TWriteSettings& settings);
    TSourcePtr BuildOverWindowSource(TPosition pos, const TString& windowName, ISource* origSource);

    TNodePtr BuildOrderBy(TPosition pos, const TVector<TNodePtr>& keys, const TVector<bool>& order);
    TNodePtr BuildSkipTake(TPosition pos, const TNodePtr& skip, const TNodePtr& take);


    TSourcePtr BuildSelectCore(
        TContext& ctx,
        TPosition pos,
        TSourcePtr source,
        const TVector<TNodePtr>& groupByExpr,
        const TVector<TNodePtr>& groupBy,
        bool compactGroupBy,
        bool assumeSorted,
        const TVector<TSortSpecificationPtr>& orderBy,
        TNodePtr having,
        TWinSpecs&& windowSpec,
        THoppingWindowSpecPtr hoppingWindowSpec,
        TVector<TNodePtr>&& terms,
        bool distinct,
        TVector<TNodePtr>&& without,
        bool selectStream,
        const TWriteSettings& settings
    );
    TSourcePtr BuildSelect(TPosition pos, TSourcePtr source, TNodePtr skipTake);


    enum class ReduceMode {
        ByPartition,
        ByAll,
    };
    TSourcePtr BuildReduce(TPosition pos, ReduceMode mode, TSourcePtr source, TVector<TSortSpecificationPtr>&& orderBy,
        TVector<TNodePtr>&& keys, TVector<TNodePtr>&& args, TNodePtr udf, TNodePtr having, const TWriteSettings& settings,
        const TVector<TSortSpecificationPtr>& assumeOrderBy, bool listCall);
    TSourcePtr BuildProcess(TPosition pos, TSourcePtr source, TNodePtr with, bool withExtFunction, TVector<TNodePtr>&& terms, bool listCall,
        bool prcessStream, const TWriteSettings& settings, const TVector<TSortSpecificationPtr>& assumeOrderBy);

    TNodePtr BuildSelectResult(TPosition pos, TSourcePtr source, bool writeResult, bool inSubquery, TScopedStatePtr scoped);

    // Implemented in insert.cpp
    TSourcePtr BuildWriteValues(TPosition pos, const TString& opertationHumanName, const TVector<TString>& columnsHint, const TVector<TVector<TNodePtr>>& values);
    TSourcePtr BuildWriteValues(TPosition pos, const TString& opertationHumanName, const TVector<TString>& columnsHint, TSourcePtr source);
    TSourcePtr BuildUpdateValues(TPosition pos, const TVector<TString>& columnsHint, const TVector<TNodePtr>& values);

    EWriteColumnMode ToWriteColumnsMode(ESQLWriteColumnMode sqlWriteColumnMode);
    TNodePtr BuildEraseColumns(TPosition pos, const TVector<TString>& columns);
    TNodePtr BuildIntoTableOptions(TPosition pos, const TVector<TString>& eraseColumns, const TTableHints& hints);
    TNodePtr BuildWriteColumns(TPosition pos, TScopedStatePtr scoped, const TTableRef& table, EWriteColumnMode mode, TSourcePtr values, TNodePtr options = nullptr);
    TNodePtr BuildUpdateColumns(TPosition pos, TScopedStatePtr scoped, const TTableRef& table, TSourcePtr values, TSourcePtr source);
    TNodePtr BuildDelete(TPosition pos, TScopedStatePtr scoped, const TTableRef& table, TSourcePtr source);

    // Implemented in query.cpp
    TNodePtr BuildTableKey(TPosition pos, const TString& service, const TDeferredAtom& cluster, const TDeferredAtom& name, const TString& view);
    TNodePtr BuildTableKeys(TPosition pos, const TString& service, const TDeferredAtom& cluster, const TString& func, const TVector<TTableArg>& args);
    TNodePtr BuildInputOptions(TPosition pos, const TTableHints& hints);
    TNodePtr BuildInputTables(TPosition pos, const TTableList& tables, bool inSubquery, TScopedStatePtr scoped);
    TNodePtr BuildCreateTable(TPosition pos, const TTableRef& tr, const TCreateTableParameters& params, TScopedStatePtr scoped);
    TNodePtr BuildAlterTable(TPosition pos, const TTableRef& tr, const TAlterTableParameters& params, TScopedStatePtr scoped);
    TNodePtr BuildDropTable(TPosition pos, const TTableRef& table, TScopedStatePtr scoped);
    TNodePtr BuildCreateUser(TPosition pos, const TString& service, const TDeferredAtom& cluster, const TDeferredAtom& name, const TMaybe<TRoleParameters>& params, TScopedStatePtr scoped);
    TNodePtr BuildCreateGroup(TPosition pos, const TString& service, const TDeferredAtom& cluster, const TDeferredAtom& name, TScopedStatePtr scoped);
    TNodePtr BuildAlterUser(TPosition pos, const TString& service, const TDeferredAtom& cluster, const TDeferredAtom& name, const TRoleParameters& params, TScopedStatePtr scoped);
    TNodePtr BuildRenameUser(TPosition pos, const TString& service, const TDeferredAtom& cluster, const TDeferredAtom& name, const TDeferredAtom& newName, TScopedStatePtr scoped);
    TNodePtr BuildAlterGroup(TPosition pos, const TString& service, const TDeferredAtom& cluster, const TDeferredAtom& name, const TVector<TDeferredAtom>& toChange, bool isDrop,
        TScopedStatePtr scoped);
    TNodePtr BuildRenameGroup(TPosition pos, const TString& service, const TDeferredAtom& cluster, const TDeferredAtom& name, const TDeferredAtom& newName, TScopedStatePtr scoped);
    TNodePtr BuildDropRoles(TPosition pos, const TString& service, const TDeferredAtom& cluster, const TVector<TDeferredAtom>& toDrop, bool isUser, bool force, TScopedStatePtr scoped);
    TNodePtr BuildWriteTable(TPosition pos, const TString& label, const TTableRef& table, EWriteColumnMode mode, TNodePtr options,
        TScopedStatePtr scoped);
    TNodePtr BuildWriteResult(TPosition pos, const TString& label, TNodePtr settings);
    TNodePtr BuildCommitClusters(TPosition pos);
    TNodePtr BuildRollbackClusters(TPosition pos);
    TNodePtr BuildQuery(TPosition pos, const TVector<TNodePtr>& blocks, bool topLevel, TScopedStatePtr scoped);
    TNodePtr BuildPragma(TPosition pos, const TString& prefix, const TString& name, const TVector<TDeferredAtom>& values, bool valueDefault);
    TNodePtr BuildSqlLambda(TPosition pos, TVector<TString>&& args, TVector<TNodePtr>&& exprSeq);
    TNodePtr BuildWorldIfNode(TPosition pos, TNodePtr predicate, TNodePtr thenNode, TNodePtr elseNode, bool isEvaluate);
    TNodePtr BuildWorldForNode(TPosition pos, TNodePtr list, TNodePtr bodyNode, TNodePtr elseNode, bool isEvaluate);

    template<class TContainer>
    TMaybe<TString> FindMistypeIn(const TContainer& container, const TString& name) {
        for (auto& item: container) {
            if (NLevenshtein::Distance(name, item) < NYql::DefaultMistypeDistance) {
                return item;
            }
        }
        return {};
    }

    bool Parseui32(TNodePtr from, ui32& to);
    TNodePtr GroundWithExpr(const TNodePtr& ground, const TNodePtr& expr);
    TSourcePtr TryMakeSourceFromExpression(TContext& ctx, const TString& currService, const TDeferredAtom& currCluster,
        TNodePtr node, const TString& view = {});
    void MakeTableFromExpression(TContext& ctx, TNodePtr node, TDeferredAtom& table);
    TDeferredAtom MakeAtomFromExpression(TContext& ctx, TNodePtr node);
    TString NormalizeTypeString(const TString& str);
}  // namespace NSQLTranslationV1
