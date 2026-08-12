# DMW Fast DDS 实现规格

| 属性 | 值 |
| --- | --- |
| 文档文件 | `dmw_fastdds.md` |
| 规范状态 | V1 Implementation Frozen Candidate |
| Fast DDS 版本 | 2.6.12 |
| 上位规范 | [`dmw.md`](dmw.md) |

本文只定义 DMW 的 Fast DDS 实现规格。公共 API、错误优先级、输出保证和运行时语义均以同版本 `dmw.md` 为唯一 authority。只有完成[验证矩阵](#fastdds-verification)并关闭[公共契约前置条件](#fastdds-appendices)后，本文才能升级为 V1 Implementation Frozen。

## 1. 文档定位与实现边界

<a id="fastdds-document-authority"></a>

### 1.1 文档层级

DMW V1 的规范层级固定为：

```text
dmw.md
    │
    │ public/runtime normative contract
    ▼
dmw_fastdds.md
    │
    │ Fast DDS implementation specification
    ▼
Fast DDS 2.6.12
```

本文负责 DDS entity mapping、内部 ownership、Factory transaction、shutdown、listener/discovery、Registry、WaitSet、Service correlation、QoS mapping、retirement、teardown、lock ordering 和 Fast DDS 实现验证。

本文不得自行修改以下公共契约：

- public class 与 Factory API；
- `ErrorCode`、`EventType`、`RequestId`、`WaitToken` 和 `WaitResult`；
- public error priority 与 output guarantee。

若本文与 `dmw.md` 冲突，必须修改本文的实现规则以满足 `dmw.md`，不得用实现细节扩大、收窄或改写公共运行时契约。

#### 1.1.1 阅读路径

| 读者目标 | 建议章节 |
| --- | --- |
| Context、Factory 与 shutdown | [Context 与 shutdown](#fastdds-context-shutdown)、[DDS entity 生命周期](#fastdds-entity-lifecycle-model) |
| Binding、Registry、QoS 与 discovery | [Binding 与 Registry](#fastdds-binding-registry-qos)、[discovery](#fastdds-discovery) |
| endpoint、Service 与 WaitSet | [endpoint 数据路径](#fastdds-endpoint-data-path)、[Service runtime](#fastdds-service-runtime)、[WaitSet](#fastdds-waitset) |
| teardown、锁序和错误映射 | [teardown](#fastdds-teardown)、[锁序与错误映射](#fastdds-lock-error-model) |
| 验证、冻结审查和最终架构 | [验证与 invariant](#fastdds-verification)、[附录](#fastdds-appendices) |

#### 1.1.2 规范性语言与结构模板

本文沿用 `dmw.md` 对“必须、不得、可以、建议”的定义。状态机按照 authority、states、transitions、failure behavior、concurrency 和 tests 的顺序说明；资源按照 owner、DDS entity pointer、creation evidence、deletion evidence 和 retention barrier 的顺序说明；公共操作按照 validation、allocation/Fast DDS API boundary、commit、rollback 和 error priority 的顺序说明。

### 1.2 参考基线

V1 使用以下固定 baseline：

| Component | Version |
| --- | --- |
| Ubuntu | 22.04 |
| ROS 2 | Humble |
| `rmw` | 6.1.3 |
| `rmw_fastrtps_cpp` | 6.2.10 |
| `rosidl_typesupport_fastrtps_cpp` | 2.2.4 |
| Fast DDS | 2.6.12 |
| Fast CDR | 1.0.29 |

CI baseline manifest 还必须记录 OS image digest、APT repository snapshot、Debian package revision、Git tag/ref、resolved commit、compiler version 和 CPU architecture。升级任一 baseline 后，必须重新执行 Topic/Service interoperability、QoS golden、failure-injection 以及 ASan/UBSan/TSan 测试。

### 1.3 V1 不引入 WireCertification

V1 不新增以下 API：

```cpp
enum class WireCertification;
make_ros2_message_type<T>();
```

当前 `MessageType` integration contract 保持：

```cpp
template<class PubSubTypeT>
Result<MessageType>
make_message_type();

MessageTypeAccess::create(
    type_support,
    std::type_index);
```

`CompatibilityProfile::Ros2FastDdsHumble` 只冻结 Fast DDS API 可控制的 DDS naming、QoS、service identity、`SampleIdentity`、`related_sample_identity` 和 discovery/matching behavior。完整 ROS 2 wire compatibility 仍要求调用者提供正确的 DDS wire type name，以及与 frozen ROS 2 Humble `rosidl_typesupport_fastrtps_cpp` 一致的 CDR serializer。V1 不尝试在运行时证明任意自定义 `PubSubTypeT` 的 ROS compatibility。

### 1.4 Fast DDS 实现总体原则

Fast DDS 实现遵循以下原则：

1. **Ownership 唯一明确。** 每个 DDS resource 都必须明确 handle owner、delete authority、listener/Topic/TypeSupport backing owner、WaitSet Condition hold，以及 delete failure 后的接管者。
2. **Logical state 是 authority。** `RuntimeState`、`EndpointPhase`、Registry entry、WaitSet topology、Guard generation、Event cumulative state、Pending FSM 和 target reader state 由 DMW 定义；Fast DDS object 只负责通信、matching 和 wake。
3. **Wake 不等于状态。** Fast DDS GuardCondition、control condition、condition variable 和 listener callback 都只是 notification mechanism；wake failure 不得回滚已经提交的 logical state。
4. **Memory safety 优先于强制 cleanup。** 无法证明 Fast DDS object 已删除时，不得释放其仍可能引用的 backing；最终可以进入 process-lifetime terminal quarantine。

#### 1.4.1 内部命名空间与 `Info` 约定

Fast DDS 专用实现类型必须位于 `dmw::impl::fastdds`；`dmw::impl` 只保留 `ContextState`、`NodeState`、`WaitableState`、`RegistrationState` 等 runtime/concurrency authority。公开 binding API 继续位于 `dmw::fastdds`，`MessageTypeAccess` 继续位于 `dmw::detail`。

```cpp
namespace dmw::impl::fastdds
{

struct ParticipantInfo;
struct DataReaderInfo;
struct DataWriterInfo;
struct GuardConditionInfo;
struct ConditionInfo;
struct ControlGuardInfo;
struct WaitSetInfo;

enum class CreationStatus;
enum class EntityStatus;
enum class AttachmentStatus;

}  // namespace dmw::impl::fastdds
```

内部 `*Info` 是 DDS entity 的生命周期记录，不是 public value type。它可以拥有 DDS entity pointer、listener、`TopicLease`、`TypeLease`、WaitSet attachment 和延迟删除信息。本文代码片段中未限定的 Fast DDS 专用内部名称，均视为位于 `dmw::impl::fastdds`。

`Context::Impl` 等嵌套 PImpl 仍定义在 `namespace dmw`，可以组合或引用 `dmw::impl::fastdds` 中的类型。该命名空间只表达编译期依赖边界，不引入多 DDS 实现、多态接口或 runtime dispatch。

### 1.5 Vendor Liveness Assumptions

DMW 可以容忍：
- Fast DDS entity creation/delete 返回错误；
- attach/detach 返回错误；
- GuardCondition trigger 返回错误；
- listener late callback；
- partial `delete_contained_entities()` failure。

但 DMW 不能在纯用户态解决 Fast DDS 自身违反基本调用约定的情况。

V1 假设：
- finite `Fast DDS WaitSet::wait()` 最终会在 timeout 后返回；
- 已进入的 Fast DDS callback 最终会返回；
- Fast DDS API 不永久阻塞在违反其公开 contract 的内部死锁中；
- 成功 `delete_datareader()` / `delete_datawriter()` 后，Fast DDS 不再开始新的 endpoint listener callback，也不再访问对应 `DataReaderInfo` / `DataWriterInfo`；删除前已经进入的 callback 仍可能晚返回，因此仍必须执行 second drain；
- 成功 `DDS Subscriber::delete_contained_entities()` 后，该 Subscriber 当时 contained DataReaders 已删除，不再开始新的 endpoint callback，也不再访问对应 `DataReaderInfo`；删除前已进入 callback 仍需 drain；
- 成功 `DDS Publisher::delete_contained_entities()` 后，对 contained DataWriters 同理；
- 成功 `DomainParticipant::delete_contained_entities()` 后，该 Participant 当时 contained Publisher/Subscriber/Topic DDS entities 已删除；已进入的 listener callback 仍需按对应 drain protocol 完成；
- 成功删除 DDS Subscriber / DDS Publisher 后，Fast DDS 不再访问其 contained endpoint 对应的 `DataReaderInfo` / `DataWriterInfo`；
- 成功删除 DomainParticipant 后，Fast DDS 不再访问其 contained DDS entity graph、discovery listener、TypeSupport 或 TopicEntry。

这些 `delete_contained_entities()` success evidence 必须由 Fast DDS 2.6.12 targeted baseline test 锁定；若 targeted test 不能稳定支持某个 cleanup shortcut，实现必须退回更保守的 parent/container/Participant lifetime barrier，不能为了 cleanup rate 放宽 memory-safety evidence。

如果 vendor 违反这些基本 liveness/lifetime assumptions，DMW 不承诺 destructor 能够有限时间完成。这属于 middleware implementation failure，而不是 DMW 可以完全屏蔽的 runtime failure。

<a id="fastdds-guid-prefix-constraint"></a>

### 1.6 Remote Participant GuidPrefix Deployment Constraint

Fast DDS 2.6.12 将 `GuidPrefix_t` 作为 RTPSParticipant 的唯一标识，并允许应用显式配置该值；但本文 **不把“Participant 删除后该 GuidPrefix 在整个本地 Context observation lifetime 永不被新的 Participant incarnation 复用”声明为 vendor guarantee**。

V1 为了保持 `ParticipantRemoved` terminal tombstone、service fallback targeting 与 late-callback FSM 的可实现性，冻结以下 **DMW deployment/integration constraint**：

```text
在一个本地 DMW Context 的整个 observation lifetime 内，
同一个 remote Participant GuidPrefix 不得被不同的 remote Participant incarnation 复用。
```

这不是有限 targeted test 可以“证明”的 vendor property。targeted/interoperability test 只作为 regression evidence；部署方仍必须满足该 constraint。

因此 V1：
- `ParticipantRemoved` 可以作为 Context-lifetime terminal tombstone；
- tombstone 后同 prefix 的 endpoint add/change/remove 按 late callback 处理，不 resurrection；
- 如果部署确实需要在同一个本地 Context lifetime 内复用 GuidPrefix，则该部署 **不属于 V1 supported deployment**；必须先版本化引入可区分 incarnation 的 remote-participant identity，再允许该场景；
- Fast DDS 实现不尝试从时间、locator、participant name 或 endpoint set 猜测 incarnation，因为这些字段都不是 V1 冻结的可靠 incarnation token。

<a id="fastdds-context-shutdown"></a>

## 2. Context DDS Entity Mapping、Factory 与 Shutdown

本章依次定义 DDS entity mapping、process/context state authority、Factory transaction、operation gate 和 shutdown protocol。所有创建路径都必须先建立可回滚 ownership，再进入 Fast DDS API call，最后在 parent 与 Context 仍允许时提交 public entity。

### 2.1 DDS Entity 映射

```text
Context
    -> DomainParticipant

Context
    -> one DDS Publisher
    -> one DDS Subscriber

Node
    -> no DDS entity

Publisher
    -> DataWriter

Subscriber
    -> DataReader

Client
    -> response DataReader
    -> request DataWriter

Server
    -> request DataReader
    -> response DataWriter

WaitSet
    -> Fast DDS WaitSet

GuardCondition
    -> Fast DDS GuardCondition
    + logical generation

Event
    -> logical EventSource/EventState
```

### 2.2 Process Runtime

V1 内部使用 process-lifetime `DmwProcessRuntime`，统一持有：

- `ProcessTerminalQuarantine`；
- `ProcessBindingQuarantine`；
- `WaitSetIdAllocator`；
- `LocalEndpointIdAllocator`。

`DmwProcessRuntime` 必须使用 non-destructing process-lifetime storage，不得依赖普通 C++ static object destructor 在进程退出时清理 quarantine backing。建议使用 intentionally leaked function-local singleton backing。

`ProcessBindingQuarantine` 只用于 trusted `TopicDataType` integration contract 已被违反，且 `deleteData()` 抛异常后无法证明 sample allocation 已释放的极端路径；它不是正常错误恢复机制。

Process runtime 在第一个 Context DDS resource 或第一个 MessageType temporary backing 创建前完成初始化。

### 2.3 Process ID Allocator

Process-wide ID allocator 使用 `std::atomic<std::uint64_t>`，不使用 mutex。

```cpp
enum class IdAllocationStatus
{
    Ok,
    Exhausted
};
```

```cpp
struct IdAllocation
{
    IdAllocationStatus status;
    std::uint64_t value{0};
};
```

分配算法如下：

```text
initial next = 1

current = next.load()

current == 0
    -> Exhausted

current == UINT64_MAX
    -> CAS next to 0
    -> return UINT64_MAX

otherwise
    -> CAS next to current + 1
```

分配器必须满足以下不变量：

- `0` 永远表示 invalid；
- `1 ... UINT64_MAX` 中每个值最多分配一次；
- 耗尽后永久返回 `Exhausted`；
- ID 不得 wrap 或 reuse；
- Factory rollback 可以产生 ID gap。

<a id="fastdds-error-priority"></a>

### 2.4 全局错误优先级

所有 public runtime operation 必须保持 `dmw.md` 定义的错误优先级：

1. public argument；
2. Context state；
3. parent state；
4. object-local state；
5. middleware/runtime state。

实现不得为了提前获取 process ID 而改变用户可观察错误。Endpoint Factory 的顺序为：

```text
validate args
    ↓
OperationGuard
    ↓
validate NodeState
    ↓
allocate LocalEndpointId
```

WaitSet Factory 的顺序为：

```text
validate args
    ↓
OperationGuard
    ↓
allocate WaitSetId
```

Atomic allocator 不获取其它 DMW lock，因此不形成 process lock inversion。

### 2.5 ContextState

```cpp
enum class FinalTeardownState
{
    NotStarted,
    Running,
    Completed,
    Quarantined
};

enum class ShutdownExecutionState
{
    Idle,
    Running,
    Completed,
    Failed
};
```

`RuntimeState` 描述对 public runtime operation 的可用性；`ShutdownExecutionState` 独立描述“谁正在执行一次性 shutdown protocol，以及该 executor 是否已经 terminal”。二者不能互相代替。

```cpp
struct ContextState
{
    std::mutex runtime_mutex;
    std::condition_variable runtime_cv;

    RuntimeState state{
        RuntimeState::Active};

    ShutdownExecutionState shutdown_execution{
        ShutdownExecutionState::Idle};

    // 仅当 shutdown_execution == Failed 时非空。
    // 保存 ordinary explicit shutdown executor 的原始 exception channel。
    std::exception_ptr shutdown_failure;

    std::size_t operations_in_flight{0};

    std::uint32_t domain_id{0};
    std::string participant_name;
    CompatibilityProfile compatibility_profile{
        CompatibilityProfile::NativeDds};

    std::atomic<FinalTeardownState> final_teardown_state{
        FinalTeardownState::NotStarted};

    std::unique_ptr<ParticipantInfo>
        participant_info;

    std::unique_ptr<TypeRegistryState>
        types;

    std::unique_ptr<TopicRegistryState>
        topics;

    ChildRegistry children;

    // Discovery callbacks only keep weak_ptr to these states.
    // ContextState is their primary strong owner during normal runtime.
    std::shared_ptr<ParticipantObservationRegistryState>
        participants;

    std::shared_ptr<RemoteEndpointRegistryState>
        remote_endpoints;

    std::shared_ptr<ServiceMatchRegistryState>
        service_matches;

    std::shared_ptr<TargetReaderObservationRegistryState>
        target_readers;

    // Retirement registries are context-owned, non-callback registries.
    std::unique_ptr<OrphanedEndpointRegistryState>
        orphaned_endpoints;

    std::unique_ptr<RetiredWaitSetRegistryState>
        retired_waitsets;

    std::unique_ptr<TerminalContextNode>
        terminal_node;
};
```

Shutdown state consistency invariant：

```text
RuntimeState::Active
    <=> shutdown_execution == Idle

shutdown_execution == Running
    => RuntimeState::ShuttingDown

shutdown_execution == Completed
    <=> RuntimeState::Shutdown

shutdown_execution == Failed
    => RuntimeState::ShuttingDown
    => no second shutdown executor is allowed
```

`Failed` 是 terminal execution state。V1 **不重跑**一个已经部分执行、随后抛异常的 shutdown protocol；这样避免对 request-all / child acknowledgement / registry finalize 等可能已经部分提交的 phase 进行未经证明的重复执行。

<a id="fastdds-registry-ownership"></a>

#### 2.5.1 Registry Ownership Closure

V1 registry state ownership 固定如下：

```text
ContextState
    -> unique_ptr<TypeRegistryState>
    -> unique_ptr<TopicRegistryState>
    -> shared_ptr<ParticipantObservationRegistryState>
    -> shared_ptr<RemoteEndpointRegistryState>
    -> shared_ptr<ServiceMatchRegistryState>
    -> shared_ptr<TargetReaderObservationRegistryState>
    -> unique_ptr<OrphanedEndpointRegistryState>
    -> unique_ptr<RetiredWaitSetRegistryState>
```

`ParticipantObservationRegistryState` 是 remote Participant lifecycle/tombstone 的 **唯一 authority**。`RemoteEndpointRegistryState` 与 `TargetReaderObservationRegistryState` 不再各自维护第二份 participant tombstone table。

DiscoveryListenerState 只保存：

```text
weak_ptr<ParticipantObservationRegistryState>
weak_ptr<RemoteEndpointRegistryState>
weak_ptr<ServiceMatchRegistryState>
weak_ptr<TargetReaderObservationRegistryState>
```

因此 listener 不会形成：

```text
listener
    -> registry
    -> Context
    -> participant
    -> listener
```

循环。

正常 runtime：
`ContextState` 是 discovery-related RegistryState 的 primary strong owner。

进入 terminal transfer：
`ContextState` 必须把上述四个 shared_ptr 以 move 方式移入 `QuarantinedParticipantInfo`；
`DiscoveryListenerState` 中已有 weak_ptr 继续指向相同 RegistryState object，
control block 和 object 地址均保持稳定。

在 discovery listener：

```text
lock DiscoveryListenerState.callback_state.mutex
accepting=false
unlock
+
first callback drain via 5.2.1 zero-count drain protocol
```

完成前，不得释放这些 RegistryState 的最后一个 strong reference。

Participant delete success + second discovery callback drain 完成后，
允许释放 discovery listener 和 discovery-related RegistryState。

Participant delete failure：
discovery listener state + 所有仍可能被 callback/DDS entity graph 访问的 RegistryState
一并进入 `ProcessTerminalQuarantine`。

`TypeRegistryState` / `TopicRegistryState` 继续使用 `unique_ptr`，
因为 listener 不持有其 weak_ptr；
Lease 仅保存 stable raw State pointer，
terminal transfer 通过 unique_ptr move 保持 State object 地址不变。

为了提交 hidden-entity evidence，`TypeRegistryState` / `TopicRegistryState` /
`OrphanedEndpointRegistryState` 允许保存 non-owning `ParticipantInfo*`；
该 pointer 只用于当前 Context 的 contained-entity status，不形成 strong ownership。

`ContextState` 在这些 registry 之前创建 `ParticipantInfo`；
normal runtime 不移动 State object，
terminal transfer 又通过 unique_ptr move 保持对象地址不变，
因此该 non-owning pointer 在 registry lifetime 内稳定。

`OrphanedEndpointRegistryState` / `RetiredWaitSetRegistryState`：
不向 Fast DDS callback 暴露 weak_ptr；
由 ContextState unique ownership，
terminal transfer 使用 intrusive splice / unique_ptr move，
不得在 catastrophic path 分配内存。

### 2.6 Runtime 状态

```cpp
enum class RuntimeState
{
    Active,
    ShuttingDown,
    Shutdown
};
```

状态机：
Active
    │
    │ shutdown linearization
    ▼
ShuttingDown
    │
    │ wake / cancel / drain
    ▼
Shutdown
    │
    │ final Fast DDS teardown
    ▼
Destroyed
Destroyed 不是 public/runtime enum。

### 2.7 ContextOptions Mapping 与 Public API Closure

V1 正式选择 **Context-scoped `CompatibilityProfile`**。原因是一个 `Context` 只创建一个 `DomainParticipant`、一个 DDS `Publisher` 和一个 DDS `Subscriber`，而 profile 同时影响 Participant/DDS container QoS、Topic/Service naming、SystemDefault 与 interoperability scope；因此 endpoint-scoped mixed profile 会破坏一个 Context 对一个 DDS entity graph 的确定性。

冻结后的 public `ContextOptions` 语义必须等价于：

```cpp
struct ContextOptions
{
    std::uint32_t domain_id{0};
    std::string participant_name;
    CompatibilityProfile compatibility_profile{
        CompatibilityProfile::NativeDds};
};
```

同时必须从以下 public options 中删除 `CompatibilityProfile`：

```text
PublisherOptions
SubscriberOptions
ClientOptions
ServerOptions
```

这些 endpoint/service options 只描述 endpoint-local policy；profile 一律继承 parent `Context`。

这是 **public API / `dmw.md` / public header 的同步要求**，不是 Fast DDS 实现可以私下兼容的 implementation detail。旧 endpoint-scoped profile API 与本文不兼容；在 public headers 与 `dmw.md` 完成同步前，本文保持 `V1 Implementation Frozen Candidate`。

`ContextOptions::domain_id`：

```text
ContextState.domain_id
    -> create_participant(domain_id, ...)
```

`Context::domain_id()` 返回 stored domain id。

`ContextOptions::participant_name`：

```text
ContextState.participant_name
    -> explicit DomainParticipantQos.name
```

`ContextOptions::compatibility_profile`：

`ContextState.compatibility_profile`

Context construction commit 后 immutable。

Fast DDS 实现不自行扩大 `dmw.md` 对这些 public fields 的合法性检查。

<a id="fastdds-native-naming"></a>
<a id="fastdds-dds-naming"></a>

#### 2.7.1 CompatibilityProfile、Resolved DDS Naming 与 Error Priority

`CompatibilityProfile` 在 Context 创建时解析一次并保存到
`ContextState.compatibility_profile`。同一个 Context 内不存在 endpoint 自行覆盖 profile，也不存在“同 Context、同 logical name、不同 profile 共存”。

所有 public endpoint/service 对外保存并返回 **logical DMW name**；resolved DDS name 只属于 implementation-internal metadata、TopicRegistry、discovery 和 interoperability diagnostic。禁止 public `name()`/等价接口泄露 profile-specific DDS prefix。

名称处理必须拆为两个阶段，以保持[全局错误优先级](#fastdds-error-priority)。

**Phase 0 — allocation-free immutable profile snapshot**

在任何可能返回 public argument error 或取得 `OperationGuard` 之前，可以：

```cpp
const CompatibilityProfile profile_snapshot =
    context_state->compatibility_profile;
```

这是对 Context commit 后 immutable enum 的无分配、无失败读取：
- 不获取 mutex；
- 不调用 Fast DDS；
- 不产生 user-visible Error；
- 不改变任何 runtime state。

该 snapshot 是当前 Factory 整个 name-validation/materialization transaction 唯一使用的 profile value；后续不重新选择 profile。

**Phase A — allocation-free public argument validation**

在取得 `OperationGuard` 之前，只允许执行不会分配 heap memory 的 legality check。实现使用 `std::string_view` / index / checked length 等构造一个栈上 `NameValidationPlan`，至少完成：
- 字符与分隔符合法性；
- absolute/relative/name namespace 组合规则的合法性；
- checked 计算 normalized logical FQN 长度；
- 使用 `profile_snapshot` checked 计算最终 DDS Topic/Service name 长度；
- 所有能导致 `InvalidArgument` 的名称语义判定。

`NameValidationPlan`：
- 不拥有字符串；
- 不分配内存；
- 不构造 `std::string`；
- 不调用 Fast DDS；
- 保存 `profile_snapshot` 或由其推导的 fixed-size resolution kind，确保 Phase B 不重新读取/解释 profile。

**Phase B — allocating materialization**

只有在：

```text
Phase 0 immutable profile snapshot
    -> allocation-free argument validation succeeded
    -> OperationGuard acquired
    -> parent state valid
    -> object-local state valid
```

之后，才允许：
- materialize normalized logical FQN；
- materialize resolved DDS Topic/Service names；
- 构造 `ServiceKey` / Topic registry lookup value；
- 进行 registry/Fast DDS transaction。

因此 Context 已 Shutdown 时，不会因为 resolver 的 `std::string` allocation 先抛 `std::bad_alloc` 而覆盖高优先级的 `ContextShutdown`。

如果未来 public name grammar 新增一个 **无法 allocation-free 判定** 的 legality 条件，则必须先修改 `dmw.md` 的 error-priority contract 或提供 allocation-free validation representation；V1 不允许通过“先分配再验证”形成隐式例外。

allocating resolver 的 value object：

```cpp
struct ResolvedTopicName
{
    std::string logical_name;
    std::string dds_name;
};

struct ResolvedServiceName
{
    std::string logical_name;
    std::string request_dds_name;
    std::string response_dds_name;
};
```

设 normalized logical FQN 为 `/a/b`，`path = a/b`（只移除规范化 FQN 的一个 leading `/`，不再次做名称清洗）。

`CompatibilityProfile::NativeDds`：

```text
Topic:            dmw/t/<path>
Service request:  dmw/rq/<path>
Service response: dmw/rr/<path>
```

`CompatibilityProfile::Ros2FastDdsHumble`：

```text
Topic:            rt/<path>
Service request:  rq/<path>Request
Service response: rr/<path>Reply
```

**TopicRegistry primary key 只使用 resolved DDS topic name。** wire type 与 canonical TopicQos fingerprint 是 `TopicEntry` 的 name-exclusive invariant fields，不是允许同名 Topic 并存的 key components。具体规则见 [TopicRegistry](#fastdds-topic-registry) 及其后续事务章节。

#### 2.7.2 ServiceKey Exact Identity / Equality / Hash

V1 `ServiceKey` 字段集合固定为：

```cpp
struct ServiceKey
{
    // Equality/hash authority fields.
    std::string request_dds_name;
    std::string response_dds_name;
    std::string request_wire_type_name;
    std::string response_wire_type_name;

    // Diagnostic/invariant fields; not equality/hash authority.
    std::string logical_service_name;
    CompatibilityProfile compatibility_profile;
};
```

`ServiceKey::operator==` **只比较且必须比较**以下四个 authority fields，顺序不影响 equality：

```text
request_dds_name
response_dds_name
request_wire_type_name
response_wire_type_name
```

`logical_service_name` 与 `compatibility_profile` 不参与 equality/hash；原因是 Registry 是 Context-scoped，resolved DDS names 已经编码 profile-specific naming。它们只用于 diagnostic 与 invariant validation。

若两个 equality-authority fields 完全相同的 key 却具有不同 `logical_service_name` 或不同 `compatibility_profile`，表示 resolver/implementation invariant corruption：相关 registry capability -> `Degraded`，operation -> `DdsError`；不能把它们静默视为两个 service identity。

`ServiceKeyHash` 必须：
- 只 hash 与 equality 相同的四个 authority fields；
- 按固定字段顺序 `request_dds_name -> response_dds_name -> request_wire_type_name -> response_wire_type_name` combine；
- 满足 `a == b => hash(a) == hash(b)`；
- 不 hash struct raw bytes、padding、object address、allocator/control-block address；
- 不作为持久化格式或跨进程 wire identity，因此具体 process-local string hash mixing 常数不是 public/frozen ABI。

Factory 固定顺序：

```text
allocation-free snapshot immutable Context profile
    -> allocation-free validate / build NameValidationPlan
    -> OperationGuard
    -> parent state check
    -> object-local state check
    -> materialize normalized logical name using the same profile snapshot
    -> resolve DDS name(s)
    -> construct ServiceKey / Topic lookup identity
    -> registry transaction / Fast DDS entity creation
```

name resolver 不得读取 Fast DDS XML、环境变量或 endpoint-local hidden profile。

### 2.8 ParticipantInfo

```cpp
enum class CreationStatus
{
    NotStarted,
    NoSideEffect,
    HandleKnown,
    SideEffectIndeterminate
};
```

```cpp
enum class ContainedEntitiesStatus
{
    Exact,
    MayContainHiddenEntity
};
```

```cpp
enum class EntityStatus
{
    KnownAlive,
    KnownDeleted,
    Indeterminate
};
```

```cpp
struct ParticipantInfo
{
    DomainParticipant* participant{nullptr};
    CreationStatus participant_creation_status{
        CreationStatus::NotStarted};
    EntityStatus participant_entity_status{
        EntityStatus::KnownDeleted};

    DDS::Publisher* dds_publisher{nullptr};
    CreationStatus publisher_creation_status{
        CreationStatus::NotStarted};
    EntityStatus publisher_entity_status{
        EntityStatus::KnownDeleted};

    DDS::Subscriber* dds_subscriber{nullptr};
    CreationStatus subscriber_creation_status{
        CreationStatus::NotStarted};
    EntityStatus subscriber_entity_status{
        EntityStatus::KnownDeleted};

    // Participant-owned graph includes Publisher/Subscriber/Topic.
    std::atomic<ContainedEntitiesStatus> participant_entities_status{
        ContainedEntitiesStatus::Exact};

    // Container-owned endpoint graphs.
    std::atomic<ContainedEntitiesStatus> publisher_entities_status{
        ContainedEntitiesStatus::Exact};
    std::atomic<ContainedEntitiesStatus> subscriber_entities_status{
        ContainedEntitiesStatus::Exact};

    std::shared_ptr<DiscoveryListenerState>
        discovery_listener_state;

    std::unique_ptr<DiscoveryListener>
        discovery_listener;
};
```

`CreationStatus` 表示 DMW 对 create transaction 副作用的已知结果；`EntityStatus` 表示 DMW 对当前 DDS entity 生命周期的已知结果。两者记录可观察状态，不声称能读取 Fast DDS 内部真实状态。一旦 pointer 被成功取得，后续生命周期 authority 转为对应 `EntityStatus`：

```text
create returns valid handle -> HandleKnown + KnownAlive
delete returns proven success -> KnownDeleted + pointer=null
delete failure/exception with uncertain effect -> Indeterminate
```

不得使用 `participant_creation_status == HandleKnown` 推断一个经历过 delete attempt 的 Participant 仍为 KnownAlive。Publisher/Subscriber 同理。

### 2.9 Context Construction

固定顺序：
initialize DmwProcessRuntime

allocate ContextState

preallocate TerminalContextNode

allocate TypeRegistryState / TopicRegistryState

allocate shared ParticipantObservationRegistryState
allocate shared RemoteEndpointRegistryState
allocate shared ServiceMatchRegistryState
allocate shared TargetReaderObservationRegistryState

allocate OrphanedEndpointRegistryState
allocate RetiredWaitSetRegistryState

allocate DiscoveryListenerState
and bind weak_ptr views to discovery-related RegistryState

construct DiscoveryListener

construct explicit DomainParticipantQos

create Participant(
    domain_id,
    explicit qos,
    discovery listener)

create DDS Publisher
with explicit PublisherQos

create DDS Subscriber
with explicit SubscriberQos

Factory commit
Discovery listener 必须：
在 Participant 创建时已经安装
不得：
Participant create
    ↓
later install discovery listener
否则可能丢失早期 discovery。

### 2.10 NodeState

```cpp
enum class NodePhase
{
    Alive,
    Closing
};
```

```cpp
struct NodeState
{
    std::shared_ptr<ContextState>
        context;

    std::mutex mutex;

    NodePhase phase{
        NodePhase::Alive};

    std::string name;
    std::string ns;
};
```

Node facade destructor：
Alive -> Closing
Existing endpoint：
继续正常工作
新的 endpoint Factory：
必须在 Parent Commit
重新检查 NodePhase::Alive

### 2.11 EndpointRuntimeState

```cpp
enum class EndpointPhase
{
    Alive,
    Closing,
    Closed
};
```

```cpp
struct EndpointRuntimeState
{
    std::mutex mutex;

    EndpointPhase phase{
        EndpointPhase::Alive};
};
```

主要用于：
Event Factory
endpoint destruction
object-local lifecycle checks

### 2.12 Operation Gate

所有要求 Context Active 的 runtime operation：
必须取得 OperationGuard
Acquire：
lock runtime_mutex

if state != Active:
    ContextShutdown

++operations_in_flight

unlock
Release：
lock runtime_mutex

assert operations_in_flight > 0

--operations_in_flight

if state == ShuttingDown
AND operations_in_flight == 0:
    notify

unlock
必须保证：
Active 检查与 operations_in_flight 增量在同一次 runtime_mutex 临界区内 linearize；
OperationGuard 一旦成功取得，shutdown 在该 Guard release 前不能观察到对应 operation 已 drain；
OperationGuard release 必须 noexcept、exactly-once，并覆盖正常 return、Error return 与 std::bad_alloc exception unwind；
OperationGuard 只证明 operation 在取得时允许开始，不证明 Context 在 operation 整个执行期间持续 Active；
可能进入 blocking Fast DDS WaitSet wait 的 operation 在真正阻塞前还必须按各自协议重新观察 shutdown/cancellation state。

### 2.13 Operation Gate Coverage

`Context::create()` 是 OperationGuard 在创建阶段的唯一例外。此时 `ContextState` 尚未成为可用的 runtime object，因此 Context construction 本身不取得 OperationGuard，而是通过 Factory transaction 和 pre-commit failure rollback 保证原子性。

Context 创建完成后，以下 operation 必须使用 OperationGuard：

- Node、endpoint 和 Event Factory；
- publish、take 和 matched count；
- Client send/take、Server take/send 和 service availability；
- WaitSet add/remove/wait；
- GuardCondition trigger 和 Event take。

以下 cleanup operation 不得要求 Context 仍为 Active：

- destructor 和 private `destroy()`；
- listener drain 和 Lease release；
- ChildRegistry unregister；
- DDS entity retirement retry；
- final Context teardown。

### 2.14 Factory 提交

Runtime Commit
用于：
create_node
create_guard_condition
条件：
Context still Active
Parent Commit
用于：
create_publisher
create_subscriber
create_client
create_server
create_event
条件：
Context Active
AND
parent Alive
Shutdown-participant Commit
用于：
create_wait_set
条件：
Context Active
AND
ChildRegistry registration completed

### 2.15 Factory 事务

统一：
validate arguments

OperationGuard

validate parent

allocate process IDs

acquire Type/Topic Leases

allocate backing

preallocate retirement / hidden-entity ownership node

create DDS resources

perform commit

publish public facade
Fast DDS entity creation success：
不是 Factory linearization point。

#### 2.15.1 Fast DDS Entity Creation Exception 与 `CreationStatus`

“ordinary C++ exception 原样传播”只描述 public exception result，
不表示 Fast DDS 实现可以在 catch 后立即 rethrow。

任何可能创建/register Fast DDS state 的 API 在 exception 越过 ordinary runtime boundary 前，必须先保证：
所有可能已经产生的 Fast DDS side effect 都处于 lifetime-safe、可追踪状态。

统一 transaction rule：
1. 在进入 Fast DDS API call 前建立 logical ownership record；
2. 预分配异常后所需 retirement/orphan node；
3. Fast DDS API call 外不持有禁止的 DMW mutex；
4. Fast DDS API call 返回或抛异常后，先更新 `CreationStatus` 或 `TypeRegistrationStatus`；
5. 必要时把 backing/lease/listener 转移到 Orphaned/Terminal ownership；
6. 最后才 return Error 或 rethrow original exception。

禁止：
Fast DDS API call entered
    -> exception
    -> erase Creating record
    -> release backing
    -> rethrow。

CreationStatus：
- NotStarted：尚未进入 Fast DDS entity creation call；
- NoSideEffect：由 frozen Fast DDS 2.6.12 operation-specific baseline 明确证明未创建目标 DDS entity；
- HandleKnown：create 成功并获得唯一 DDS entity pointer；
- SideEffectIndeterminate：已经进入 Fast DDS API call，但 DMW 无法证明没有创建隐藏 DDS entity。

`nullptr` / ReturnCode failure 只有在[锁与错误模型](#fastdds-lock-error-model)针对该 API 明确冻结为 NoSideEffect 时才能释放预建 backing；
否则同样按 SideEffectIndeterminate 处理。

#### 2.15.2 Contained Hidden Entity Rule

对于有 parent container 的 create API：
DataReader -> DDS Subscriber
DataWriter -> DDS Publisher
Topic / Publisher / Subscriber -> DomainParticipant

如果 create call 进入后发生 exception 或无法证明 no-side-effect，且 caller 没有得到 handle：
- parent 对应 `ContainedEntitiesStatus` -> MayContainHiddenEntity；
- 预分配 backing/lease/listener ownership 不释放；
- backing 进入 Orphaned/retirement ownership；
- public operation 按原始 Error/exception 结束。

MayContainHiddenEntity 表示：
DMW 的显式 handle registry 不再声称完整枚举 parent 的 contained DDS entity graph。
因此后续 teardown 必须使用 container-level evidence barrier，
不能仅依据“所有已知 handle 已删除”释放隐藏 entity 可能仍引用的 backing。

DataReader/DataWriter create indeterminate：
对应 DDS Subscriber/Publisher graph 标记 MayContainHiddenEntity。

Topic/Publisher/Subscriber create indeterminate：
Participant graph 标记 MayContainHiddenEntity。

#### 2.15.3 Root DomainParticipant Creation

`DomainParticipantFactory::create_participant()` 没有更高层 DDS container 可用于恢复隐藏 Participant。
因此 Context Factory 在调用 create_participant() 前必须已经：
- 初始化 non-destructing DmwProcessRuntime；
- 预分配 TerminalContextNode；
- 建立 DiscoveryListener/Registry backing strong ownership；
- 建立可 no-allocation transfer 的 partial QuarantinedParticipantInfo ownership。

create_participant() 返回有效 handle：
participant_creation_status = HandleKnown；
participant_entity_status = KnownAlive。

create_participant() 返回 nullptr：
只有 frozen baseline test 明确证明 nullptr 表示 no Participant side effect 时：
participant_creation_status = NoSideEffect；
participant_entity_status = KnownDeleted；
否则：
participant_creation_status = SideEffectIndeterminate；
participant_entity_status = Indeterminate。

create_participant() 在 entered Fast DDS API call 后抛 exception：
participant_creation_status = SideEffectIndeterminate；
participant_entity_status = Indeterminate；
没有 handle 时不得尝试猜测隐藏 Participant 状态；
必须将 partial Context objects（至少 discovery listener/state 和其依赖 registry）
通过预分配 TerminalContextNode no-allocation adopt 到 ProcessTerminalQuarantine；
然后 rethrow original exception。

该极端路径允许 process-lifetime Fast DDS Participant/backing retention，
优先保证隐藏 Participant 即使仍持有 listener pointer 也不会造成 UAF。

#### 2.15.4 Context Construction Child Create Failure

Participant handle 已知后创建 DDS Publisher / DDS Subscriber：
- 有效 handle -> 对应 creation_status=HandleKnown，entity_status=KnownAlive；
- frozen no-side-effect failure -> creation_status=NoSideEffect，entity_status=KnownDeleted；
- entered Fast DDS API call 后 exception/ambiguous failure -> creation_status=SideEffectIndeterminate，entity_status=Indeterminate，participant_entities_status=MayContainHiddenEntity。

Context 尚未 public commit 时发生 error/exception：优先执行 best-effort contained cleanup + Participant delete。

若能够获得完整 KnownDeleted/NoSideEffect evidence：
释放 partial Context objects，并按 primary error/exception 返回。

若任一 child create 为 SideEffectIndeterminate、container cleanup 失败或 Participant delete 无法证明成功：
整个 partial Context 进入 TerminalContextNode / ProcessTerminalQuarantine；
不得只释放 caller 未拿到 handle 的 child backing；
之后再返回 primary Error 或 rethrow original exception。

### 2.16 Factory 与 Shutdown

需要注册 ChildRegistry 的 Factory：
lock runtime_mutex

state still Active?
    no:
        rollback
        ContextShutdown

yes:
    lock ChildRegistry
    register child
    commit
    unlock ChildRegistry

unlock runtime_mutex
因此只有：
Factory commits first
    -> shutdown sees it

or

shutdown linearizes first
    -> Factory cannot commit

### 2.17 Shutdown Linearization

lock runtime_mutex

Active -> ShuttingDown
        ↑
        linearization point

unlock
之后：
new OperationGuard
new Factory commit
new blocking operation registration
全部失败：
ContextShutdown

### 2.18 ChildRegistry

管理两类对象：
PersistentChild
EphemeralInterruptibleWait
Persistent：
WaitSet
Ephemeral：
Server response discovery wait
其它内部可取消 blocking wait

每个 child registration 在 commit 前预分配 intrusive link并取得
`child_registration_id`。该 ID 由 Context-local checked monotonic allocator产生，Context
lifetime 内不复用、不 wrap；耗尽时新 child Factory/ephemeral registration 返回
`ResourceExhausted` 且不修改 registry。ChildRegistry 支持按 ID 查找下一个 linked child并
复制现有 stable ownership handle，该 traversal 不分配内存，用于 2.21 shutdown Phase B/D。

### 2.19 InternalChildState

```cpp
struct InternalChildState
{
    std::mutex mutex;
    std::condition_variable cv;

    std::atomic<std::uint64_t>
        requested_generation{0};

    std::atomic<std::uint64_t>
        acknowledged_generation{0};

    ChildKind kind;

    std::weak_ptr<WaitSetState>
        waitset;

    std::weak_ptr<InterruptibleWaitState>
        interruptible_wait;

    ShutdownParticipantOps ops;
};
```

### 2.20 ShutdownParticipantOps

```cpp
struct ShutdownParticipantOps
{
    void (*request_shutdown)(
        InternalChildState&,
        std::uint64_t generation) noexcept;

    bool (*shutdown_complete)(
        const InternalChildState&,
        std::uint64_t generation) noexcept;
};
```

禁止：
std::function
capturing lambda
opaque dangling target

<a id="fastdds-child-ack"></a>

#### 2.20.1 Child acknowledgement CV publication discipline

`InternalChildState::cv` 只等待 acknowledgement/quiescence transition。虽然 `requested_generation` / `acknowledged_generation` 使用 atomic 便于 blocking path lock-free observation，**condition_variable 的 normal correctness path 仍必须让 acknowledgement publication 与 waiter通过同一 `InternalChildState::mutex` 建立 wait/notify handshake**。

唯一 normal acknowledge helper：

```text
publish_ack_noexcept(child, generation):
    try:
        unique_lock child.mutex
        checked/max publish acknowledged_generation >= generation
        unlock
        child.cv.notify_all()
    catch (...):
        // catastrophic mutex/system failure at a noexcept boundary
        atomic max-publish acknowledged_generation >= generation
        fixed diagnostic
        child.cv.notify_all() best effort
        // Phase-D waiter uses bounded slices and rechecks the atomic predicate.
```

规则：
- normal `acknowledged_generation` publication在 `child.mutex` 下完成；
- normal waiter持同一 mutex检查 predicate，因此不存在 notify-before-wait lost wake；
- notify不是 state authority，generation才是；
- catastrophic mutex/system failure不得越过 `noexcept` helper；
- Phase D **禁止无限 `cv.wait()`**，必须使用 bounded `wait_for` slice并重读 atomic acknowledgement / shutdown execution state；因此即使 catastrophic path丢失一次 notify，也不能永久等待已发生的 ack。

`ShutdownParticipantOps::request_shutdown()` 作为
`request_shutdown_signal_only()` 调用，是 signal-only：
- `requested_generation` 已由 2.21 Phase B1 发布，本 helper 不再次修改该 authority；
- 发起必要 control wake / Target dependency signal；
- 不等待 ack；
- 调用时不持有任何 DMW mutex，本 helper 自身也不得取得
  `ChildRegistry`/`InternalChildState::mutex`。

`shutdown_complete()` 只执行 bounded/no-allocation quiescence observation；不能持有 WaitSet/Target 等高 rank mutex后反向取得 child mutex。若 executor确认 child已 quiescent但 ack尚未发布，必须先释放所有高 rank locks，再调用 `publish_ack_noexcept()`。

### 2.21 Shutdown Protocol

Context 生命周期只有一次 shutdown execution。V1 使用：

`shutdown_generation = 1`

一次 successful executor 的固定 phase：

**Phase A — Mark dependent runtime state**
- `PendingRequestRegistry.begin_shutdown()`；
- mark discovery/runtime cancellation；
- 影响 `send_response()` predicate 的 shutdown state 通过 [target dependency protocol](#fastdds-target-dependency) 的 `signal_target_dependency_change()` 发布，不使用裸 atomic store + notify。

**Phase B — Request All Children**

必须先向全部 linked child 发布 logical request，再执行任何 Fast DDS/control wake；不得
request-one/wait-one。Phase B 固定为两个 pass：

```text
B1 — publish all logical requests
    lock ChildRegistry
    for every linked child:
        requested_generation = 1  // atomic publish
    unlock ChildRegistry

B2 — signal without registry locks
    cursor = invalid child_registration_id
    loop:
        lock ChildRegistry
        child = next linked stable child handle after cursor
        unlock ChildRegistry

        if no child:
            break

        cursor = child.child_registration_id
        request_shutdown_signal_only(child, 1)
        release stable child handle
```

`child_registration_id` 在 child 注册时由 checked monotonic allocator 分配，Context
lifetime 内不复用、不 wrap。`ChildRegistry` 按该 ID 提供 no-allocation 的 next lookup；
复制现有 stable handle 必须 `noexcept`。shutdown linearization 后不再允许新 child commit，
因此 B2 的 cursor traversal 不会漏掉新注册对象。若 child 在 B1 后、B2 snapshot 前
unlink，2.22 保证它已经对 generation 1 quiesce 并 ack，所以无需再 signal。

`request_shutdown_signal_only()`：
- `noexcept`；
- 不获取 `InternalChildState::mutex`；
- 不 unregister child；
- 不再发布 `requested_generation`；该 authority 已由 B1 对全部 child 提交；
- 只执行允许的 cancellation control wake / Target dependency signal；
- 调用时不得持有 `ChildRegistry`、Context runtime mutex 或任何其它 DMW mutex。

因此 Phase B 不形成 `ChildRegistry(rank 2)` 与 `InternalChildState(rank 2)` same-rank overlap，
也不违反 10.4 对 `GuardCondition::set_trigger_value()` 的 Fast DDS API call unlock rule。

**Phase C — Drain Operations**

```text
unique_lock runtime_mutex
runtime_cv.wait(lock, operations_in_flight == 0)
unlock
```

`OperationGuard` release在同一 `runtime_mutex` 下递减并在归零时 notify，因此该 drain使用标准 CV handshake。

**Phase D — Acknowledge All**

Phase B 已释放 `ChildRegistry`。对每个当前仍 linked child：
1. 在 `ChildRegistry` 下取得 stable child handle/shared ownership，立即 unlock；
2. `shutdown_complete(child, 1)` 只做 nonblocking quiescence observation；若已 quiescent但尚未 ack，在零高-rank-lock状态调用 `publish_ack_noexcept(child, 1)`；
3. 使用 bounded CV slice：

```text
for (;;) {
    if acknowledged_generation >= 1:
        break

    unique_lock child.mutex
    child.cv.wait_for(lock, <= 100ms, [&] {
        return acknowledged_generation >= 1;
    })
    unlock

    if acknowledged_generation >= 1:
        break

    if shutdown_complete(child, 1):
        publish_ack_noexcept(child, 1)
}
```

4. release stable child handle，再处理下一个。

Child若在 Phase D snapshot前已 unlink，2.22保证它在 unlink前已经发布当前 generation ack。

**Phase E — Finalize Runtime**
- `PendingRequestRegistry.finalize_shutdown()`；
- 在 `runtime_mutex` 下原子提交：
  - `state = RuntimeState::Shutdown`；
  - `shutdown_execution = ShutdownExecutionState::Completed`；
  - clear `shutdown_failure`；
- unlock 后 `runtime_cv.notify_all()`。

Phase E 是 success 的唯一 terminal commit；此前任何 unexpected C++ exception必须进入2.24 `Failed` protocol，不能留下无人负责的 `Running` executor。

### 2.22 Child Unregister

Child 从 `ChildRegistry` unlink前若：

`requested_generation > acknowledged_generation`

固定为：
1. 先进入 quiescent state；
2. 释放 WaitSet/Target/Registration/其它高 rank state mutex；
3. 调用 `publish_ack_noexcept(child, requested_generation)`；
4. 最后取得 `ChildRegistry` 并 unlink。

因此不存在 high-rank child-state -> rank-2 child mutex 的反向锁，也不存在“已 unlink 但仍欠当前 shutdown acknowledgement”的状态。

### 2.23 WaitSet Shutdown Ack

Idle WaitSet：

```text
request shutdown
    -> active_wait == false
    -> no topology/reconciliation/registration/waitable lock held
    -> publish_ack_noexcept(shutdown_child, requested_generation)
```

Active WaitSet：

```text
record shutdown request
    -> trigger private control GuardCondition

Wait 真正结束:
    finish Fast DDS result interpretation
    release active_wait_count
    release reconciliation/topology/registration/waitable locks
    ActiveWaitGuard destructor
    -> active_wait = false
    -> publish_ack_noexcept(shutdown_child, requested_generation)
```

Ack表示指定 generation 已 quiescent：不存在仍执行的 blocking Fast DDS WaitSet wait；shutdown linearization前取得 OperationGuard但尚未真正阻塞的 operation，在进入 wait前必须重新观察 cancellation并退出。successful wake本身不等价于 ack。

### 2.24 Runtime Shutdown Execution、Result 与 Executor Failure

`RuntimeState` 和 `ShutdownExecutionState` 的职责分离：

```text
RuntimeState:
    controls whether new public runtime operations may start/commit

ShutdownExecutionState:
    elects exactly one shutdown executor
    records Completed or Failed terminal execution outcome
```

#### 2.24.1 Executor election

所有 explicit `Context::shutdown()` 和 internal `shutdown_noexcept()` 都进入同一 helper/state machine。

在 `runtime_mutex` 下：

```text
shutdown_execution == Completed
    -> success

shutdown_execution == Failed
    -> explicit caller snapshots shutdown_failure and rethrows after unlock
    -> noexcept caller records diagnostic and returns

shutdown_execution == Running
    -> wait runtime_cv until execution != Running
    -> re-evaluate terminal state

shutdown_execution == Idle
    -> require RuntimeState == Active
    -> state = ShuttingDown
    -> shutdown_execution = Running
    -> this caller becomes the only executor
    -> unlock
    -> execute 2.21 phases
```

`Active -> ShuttingDown` 与 `Idle -> Running` 必须在同一个 `runtime_mutex` critical section 内提交，因此不存在 `state == ShuttingDown && shutdown_execution == Idle` 的合法中间状态。

#### 2.24.2 Successful executor

Phase A～E 完成后，在 `runtime_mutex` 下：

```text
state = Shutdown
shutdown_execution = Completed
shutdown_failure = nullptr
```

然后 `runtime_cv.notify_all()`。

所有等待 caller 被唤醒并观察 `Completed`。

#### 2.24.3 Unexpected executor exception

如果 ordinary shutdown executor 在 Phase A～E 任意位置遇到允许传播的 unexpected C++ exception：

```text
catch (...)
    ep = std::current_exception()   // noexcept capture
    lock runtime_mutex
    assert shutdown_execution == Running
    state remains ShuttingDown
    shutdown_failure = ep
    shutdown_execution = Failed
    unlock
    best-effort cancellation/notify that is itself noexcept
    runtime_cv.notify_all()
```

然后：
- explicit ordinary caller：`std::rethrow_exception(ep)`；
- `shutdown_noexcept()` caller：不传播，fixed-size diagnostic 后返回。

**禁止**在 exception 后仅保持 `ShuttingDown + Running` 或 `ShuttingDown + Idle`；
否则原 executor 已退出后，后续 waiter 会永久等待。

#### 2.24.4 Failed 是 terminal，不 retry partial phases

`shutdown_execution == Failed` 后：
- 不再 elect 第二 executor；
- 不重新执行已经部分完成的 Phase A～E；
- explicit 后续 `shutdown()` 重复传播同一个 stored exception；
- noexcept/destructor path 不传播；
- surviving child operation 因 `RuntimeState::ShuttingDown` 始终返回 `ContextShutdown`；
- 最后 ContextState destruction 进入 conservative terminal-retention/quarantine，不执行要求 `RuntimeState::Shutdown` 的正常 final Fast DDS teardown。

如果未来希望 retry Failed shutdown，必须给每个 Phase 增加独立 durable progress marker 和 idempotence evidence；V1 不定义该复杂度。

#### 2.24.5 Normal Fast DDS V1 shutdown result subset

在本文 vendor liveness assumptions 成立、且没有 unexpected C++ exception 的正常 protocol 中，runtime shutdown terminal result 恒为 success。

private control wake failure不直接导致 shutdown failure，因为 logical cancellation 已提交，并有 bounded wait slice fallback。

以下 future final-cleanup failure 不属于已经返回的 runtime shutdown result：
- endpoint Fast DDS entity deletion failure；
- Topic delete failure；
- Participant delete failure；
- terminal quarantine。

这些只进入 Fast DDS cleanup diagnostics，不能 retroactively 改变 shutdown result。

Fast DDS V1 是 `dmw.md` 所允许 shutdown result capability 的严格子集：
- Fast DDS 实现不为了 final cleanup failure 人为返回一个 terminal `DdsError`；
- unexpected ordinary C++ exception 仍按统一 exception boundary 原样传播，并由 `ShutdownExecutionState::Failed + exception_ptr` 使并发/后续 caller 得到一致 terminal observation；
- 若未来新增可观察的 non-exception shutdown Error result，再同步 `dmw.md` 与本文。

### 2.25 Context Facade Destruction 与 Implicit Shutdown

同版本 `dmw.md` 规定 public Context handle 析构执行 best-effort implicit shutdown。本文必须把该 public rule 映射为 **与显式 `Context::shutdown()` 相同的 runtime shutdown execution state machine**，不能等到 `ContextState` 最后一个 owner 才第一次提交 shutdown。

正常 facade/Impl ownership 边界固定为：

```cpp
Context::Impl::~Impl() noexcept
{
    if (state)
    {
        state->shutdown_noexcept();
    }
}
```

如果具体 public facade 实现不是独立 `Context::Impl`，等价要求仍是：
销毁最后一个 public Context facade representation 时，在释放其 `ContextState` strong reference 之前调用 `ContextState::shutdown_noexcept()`。

`shutdown_noexcept()`：
- 复用 2.24 executor election；
- `Idle` caller 可以成为唯一 executor；
- `Running` caller 等待现有 executor进入 `Completed/Failed`；
- `Completed` 立即返回；
- `Failed` 不重试，记录 diagnostic 后返回；
- 不允许 exception 越过 `noexcept` boundary。

关键 public effect：

```text
Context facade destroyed first
    -> shutdown linearization is committed immediately
    -> surviving Node/endpoint/WaitSet may still strong-own ContextState
    -> but every new runtime operation observes ContextShutdown
    -> surviving child can no longer observe/re-enter Active
```

`ContextState::~ContextState() noexcept` 是最后一道 defensive barrier：

```text
if shutdown_execution == Idle:
    call shutdown_noexcept()

if shutdown_execution == Running:
    wait until Completed or Failed

if shutdown_execution == Completed:
    assert RuntimeState == Shutdown
    run_final_teardown_once()

if shutdown_execution == Failed:
    assert RuntimeState == ShuttingDown
    normal Fast DDS final teardown is forbidden
    transfer retained DDS entity graph and Info objects to terminal quarantine
```

因此[正常 final teardown](#fastdds-teardown)的前置条件仍是 `RuntimeState::Shutdown + shutdown_execution::Completed`。

该规则不改变 public facade lifetime rule：**同一个 public Context object 的普通方法与该 object 析构不得无同步并发。** shutdown executor concurrency tests 必须通过不同合法 internal state holders 或经过 public lifetime synchronization 构造，不能制造 C++ object-lifetime UB。

<a id="fastdds-native-proof-model"></a>
<a id="fastdds-entity-lifecycle-model"></a>

## 3. DDS Entity Ownership 与 Operation Status

本章提供 DDS entity 生命周期的阅读入口，不重复后续章节的规范条款。实现必须从以下五个维度审查每类 DDS entity：owner、DDS entity pointer、creation status、entity status 和 retention barrier。

| 维度 | 规范问题 | 主要定义位置 |
| --- | --- | --- |
| Owner | 谁持有 DDS entity pointer 及对应 `*Info`？ | Context、Registry 与 Endpoint Info 章节 |
| Creation status | Fast DDS entity creation 返回或抛异常后，`CreationStatus` 应如何更新？ | Factory transaction 与 creation-status matrix |
| Entity status | delete 返回后，`EntityStatus` 是 KnownAlive、KnownDeleted 还是 Indeterminate？ | Retirement 与 Fast DDS ReturnCode mapping |
| Retention barrier | 无法证明删除成功时，哪些 listener、TypeSupport、Topic 或 Condition 必须继续保活？ | Contained graph、retirement 与 terminal quarantine |
| Final authority | 谁执行 exactly-once final teardown？ | ContextState final teardown protocol |

详细协议分别由后续资源章节定义；本章仅建立统一阅读模型，不引入第二份 normative state machine。

<a id="fastdds-binding-registry-qos"></a>

## 4. Message Binding、Type/Topic Registry 与 QoS

本章以 canonical binding 为起点，依次定义 temporary sample、Type/Topic Registry transaction 和 Fast DDS QoS materialization。Caller 提供的 descriptor 只参与首次 canonicalization；成功取得 TypeLease 后，runtime 只使用 registry 中的 canonical authority。

### 4.1 MessageType

MessageType 内部保存：
exact DDS wire type name
BindingIdentity
Fast DDS TypeSupport
descriptor 构造后 immutable。

<a id="fastdds-binding-identity"></a>

### 4.2 BindingIdentity

V1：
binding implementation ID
+
std::type_index(typeid(PubSubTypeT))
要求 RTTI。
TypeRegistry equality：
same wire type name
AND
same BindingIdentity
不同 BindingIdentity：
TypeMismatch

<a id="fastdds-message-binding-contract"></a>

#### 4.2.1 Message Binding Integration Contract

make_message_type<PubSubTypeT>() 接受的是 trusted integration binding，
而不是 DMW 可以完全运行时验证的任意 C++ 类型。

V1 对 PubSubTypeT / TopicDataType binding 冻结以下要求。

Canonical binding capability：

```cpp
enum class BindingCapabilityState
{
    Healthy,
    Degraded
};
```

```cpp
struct CanonicalTypeBinding
{
    std::shared_ptr<const MessageType::Impl>
        descriptor;

    std::atomic<BindingCapabilityState>
        capability{BindingCapabilityState::Healthy};
};
```

`MessageType::Impl` 只是 immutable descriptor candidate，保存：
exact wire type name
BindingIdentity
TypeSupport / TopicDataType integration object。

`BindingCapabilityState` 不属于独立 descriptor instance；
它属于当前 Context 的 canonical TypeEntry runtime binding。

同一 Context 内：
same wire type name
AND
same BindingIdentity
必须解析到同一个 `CanonicalTypeBinding`。

第一次成功创建该 TypeEntry 的 descriptor 成为：
canonical descriptor。
后续独立 `make_message_type<PubSubTypeT>()` 即使拥有不同 `MessageType::Impl`，
只要 BindingIdentity 相同，也只是等价 candidate；
`TypeRegistry::acquire()` 成功后不得继续使用 caller candidate 执行 TypeSupport serialization hook，
必须使用 `TypeLease::canonical_binding()`。

因此同一 Active TypeEntry 的硬性 invariant 是：
registered TypeSupport
== canonical_binding.descriptor 的 TypeSupport
== endpoint runtime binding
== TemporarySample binding
== publish/take binding
== Service request/response binding
== BindingCapability authority。

若两个 descriptor 的 wire type name + BindingIdentity 相同，
但实际 serializer/lifecycle behavior 不可互换，属于 integration contract violation；
调用者必须使用不同 BindingIdentity，而不是依赖 descriptor instance identity 绕过 TypeRegistry deduplication。

Canonical binding scope：
Context × TypeEntry。
不同 Context 可以各自建立独立 canonical TypeEntry/capability；
同一 Context 内相同 TypeEntry 的所有 lease 必须共享 degradation。

Healthy -> Degraded 单向；
不自动恢复。

普通 runtime 在完成：
public arguments
Context state
parent state
object-local state
优先级检查后，若下一步必须调用 canonical binding 且 capability == Degraded：
    -> DdsError

必须检查 canonical binding capability 的路径至少包括：
endpoint Factory 的 Fast DDS type integration step
TemporarySample::create(TypeLease)
publish/write serialization path
take/read deserialization path
Temporary -> Caller commit serialization/deserialization
Service request/response serialization/deserialization

teardown / delete / retirement 不得因为 canonical binding 已 Degraded 而跳过；
它们继续按 conservative lifetime protocol 完成。

对象生命周期：
createData()
    -> 成功时返回与该 TopicDataType 匹配的、已构造且可由 deleteData() 释放的 sample object
    -> nullptr 表示 binding/Fast DDS allocation failure

deleteData(void*)
    -> 必须接受由同一 TopicDataType::createData() 返回的对象
    -> integration contract 要求不向调用者传播异常

`AllocatedSample` 表示由 TypeSupport 创建、等待提交或删除的 sample allocation：

```cpp
class AllocatedSample
{
public:
    IntrusiveBindingQuarantineNode node;
    std::shared_ptr<CanonicalTypeBinding> binding;
    void* data{nullptr};
};
```

TemporarySample 在调用 createData() 前先分配并拥有：
std::unique_ptr<AllocatedSample>

如果该 `AllocatedSample` 分配抛 `std::bad_alloc`：
    -> 原样传播
    -> 尚未调用 createData()

`createData()` 成功后把返回 pointer 写入 `allocated_sample->data`。

TemporarySample::~TemporarySample() noexcept：
try:
    binding->descriptor->deleteData(allocated_sample->data)
    allocated_sample->data = nullptr
catch (...):
    binding->capability.store(BindingCapabilityState::Degraded)
    将已经预分配 intrusive node 的 AllocatedSample
    以 no-allocation 方式 transfer 到 DmwProcessRuntime::ProcessBindingQuarantine
    记录固定大小 diagnostic
    不允许 exception 离开 destructor

ProcessBindingQuarantine：
process lifetime
non-destructing
独立 mutex + intrusive list
adoption API 必须 noexcept
adoption 正常路径不分配内存
adoption 时不得持有其它 DMW mutex
adoption 必须复用 [terminal quarantine](#fastdds-terminal-quarantine) 的 `std::unique_lock` + `noexcept intrusive splice` protocol；禁止手工 lock/unlock

如果 quarantine mutex acquisition 或任何 adoption bookkeeping 自身异常：
catch inside noexcept adoption
release raw AllocatedSample ownership intentionally
形成 process-lifetime intentional leak
不得让 `unique_ptr` 在栈展开时析构该 allocation
不得 terminate
并使用预分配/atomic diagnostic bit 记录 quarantine-adoption failure。

正常 adoption 永久 strong-own AllocatedSample，
从而同时保活：
sample pointer
CanonicalTypeBinding
canonical MessageType::Impl / TypeSupport / binding capability

因此 deleteData 抛异常属于 binding contract violation；
DMW 保证 memory safety / noexcept destructor，
但允许故意 process-lifetime retention，而不伪造“已经释放”。

C++ exception boundary：
createData()
getSerializedSizeProvider()
serialize()
deserialize()
以及普通 Fast DDS C++ API 调用，遵循 `dmw.md` public exception contract：

std::bad_alloc：
    ordinary runtime -> 原样传播

其它未预期 C++ exception：
    ordinary runtime -> 原样传播
    不得转换或伪装成 DdsError / Unsupported / ResourceExhausted

noexcept Fast DDS listener callback：
    catch (...)
    -> affected capability Degraded / NeedsRebuild
    -> fixed-size diagnostic

noexcept destructor / retirement / rollback cleanup：
    catch (...)
    -> conservative retention / Indeterminate evidence / diagnostic
    -> 不允许 exception 越过 noexcept boundary

已经完成 logical commit 的 best-effort notification path，
例如 public GuardCondition logical trigger commit 之后的 Fast DDS wake，
属于 operation 的 post-commit notification，不再代表 public operation 成败；
该 path 捕获 Fast DDS wake exception、进入 degraded state 并记录 diagnostic，
不能在 logical effect 已提交以后向 caller 抛出一个看似“operation failed”的异常。

返回 false：
serialize()/deserialize() -> DdsError
因为这是 binding/TypeSupport hook 自己报告的正常 failure result，
不是未预期 C++ exception。

size provider：
必须给出与该 sample serializer 一致的 upper bound；
实现必须对以下情况执行 checked validation：
size overflow
size > Fast DDS payload representable range
不一致的负/非法 representation
执行 checked validation。
无法表示：
DdsError
真正 heap allocation failure：
std::bad_alloc 原样传播。

Deserialize basic guarantee：
对于实现传入的已经构造 caller object，
TopicDataType::deserialize() 的每一个失败出口都必须保证 caller object 仍：
valid
destructible

覆盖：
returns false
throws std::bad_alloc
throws other C++ exception

字段值允许 unspecified。
异常是否传播与 caller object 是否保持 basic guarantee 是两个独立 contract。
DMW V1 不尝试通过 runtime reflection 证明该性质；
违反该要求属于 binding integration contract violation。

线程安全：
同一 MessageType binding 可能被多个 DataWriter/DataReader、
Fast DDS 内部线程以及 DMW operation 并发使用。
PubSubTypeT 必须满足 Fast DDS 2.6.12 对 TopicDataType 并发调用的要求。
DMW 不在所有 TopicDataType hook 外增加全局 serialization mutex，
因为该 mutex 无法覆盖 Fast DDS 内部直接调用同一 TypeSupport 的路径。
不满足该线程安全要求属于 binding contract violation。

ROS compatibility：
Ros2FastDdsHumble profile 下用于 interoperability test 的 binding
必须来自 frozen ROS 2 Humble rosidl_typesupport_fastrtps_cpp，
或提供逐字节等价的 wire type name 与 CDR behavior。
任意 custom binding 不因选择 Ros2FastDdsHumble profile 而自动获得认证。

##### 4.2.1.1 MessageTypeAccess::create() Bridge Contract

`MessageTypeAccess::create()` 是 public template binding 与 implementation-private `MessageType::Impl` 之间唯一 construction bridge。

固定输入/输出协议：

```cpp
static Result<MessageType> create(
    TypeSupportHandle type_support,
    std::type_index binding_type);
```

实现顺序：
1. 验证 `type_support` 非空；
2. 取得 `TopicDataType*` stable target；
3. 调用 frozen Fast DDS binding 的 `getName()`/等价 API 取得 exact wire type name；
4. wire type name 为空 -> `InvalidArgument`；
5. checked copy immutable wire type name；
6. 构造 `BindingIdentity{binding_implementation_id, binding_type}`；
7. allocate immutable `MessageType::Impl`；
8. Impl strong-own TypeSupport integration object；
9. commit `MessageType` facade。

异常规则：
- allocation `std::bad_alloc` 原样传播；
- `getName()`、TypeSupport wrapper 或其它 binding construction 抛未预期 C++ exception：完成已建立的局部 RAII rollback 后原样传播；
- Fast DDS/TypeSupport exception 不转换成 `DdsError`；
- 在 facade commit 前失败不得留下 TypeRegistry entry 或 Fast DDS registration。

`wire_type_name` 在 Impl 构造后 immutable；后续不得再次调用可变化的 `getName()` 决定 Registry identity。

Binding DSO lifetime：
只要以下任一对象仍存活：
- `MessageType` / `MessageType::Impl`；
- `CanonicalTypeBinding` / `TypeLease`；
- endpoint/service runtime backing；
- `ProcessBindingQuarantine` / `ProcessTerminalQuarantine` 中保留的 CanonicalTypeBinding；
定义 `PubSubTypeT` 的 `type_info`、vtable 和 hook code 所在 DSO **不得 unload/dlclose**。
DMW V1 不提供 DSO pinning API；这是 integration/deployment contract。

当前 public API 中 `BindingIdentity` 的 type component 固定为 `typeid(PubSubTypeT)`。
因此如果两个 serializer/lifecycle implementation 不可互换，调用者必须使用 **不同的 C++ `PubSubTypeT` wrapper type**；V1 没有“显式传入任意 BindingIdentity token”的 public API。

<a id="fastdds-identity-conversion"></a>

#### 4.2.2 GUID / Sequence / Timestamp Conversion Helpers

所有 wire identity 转换集中在单一 internal module：
fastdds_identity_conversion.hpp
或等价实现。

禁止在 Client、Server、Subscriber、Discovery 各自复制 bit conversion 代码。

编译期 layout assumptions 必须显式冻结：
static_assert(sizeof(std::int32_t) == 4);
static_assert(sizeof(std::uint32_t) == 4);
static_assert(sizeof(std::int64_t) == 8);
static_assert(sizeof(std::uint64_t) == 8);
static_assert(sizeof(SequenceNumber_t{}.high) == sizeof(std::int32_t));
static_assert(sizeof(SequenceNumber_t{}.low) == sizeof(std::uint32_t));
static_assert(sizeof(RequestId{}.sequence_number) == sizeof(std::int64_t));

如果 future Fast DDS baseline 改变这些 representation assumptions：
compile-time failure
而不是静默改变 wire identity conversion。

RequestId：SequenceNumber_t -> int64_t
Fast DDS SequenceNumber_t 的 high/low 视为 64-bit bit pattern：

uint32_t high_bits;
memcpy(
    &high_bits,
    &sequence.high,
    sizeof(high_bits));

uint64_t bits =
    (uint64_t{high_bits} << 32) |
    uint64_t{sequence.low};

int64_t value;
static_assert(sizeof(value) == sizeof(bits));
memcpy(&value, &bits, sizeof(value));

return value;

禁止：
对负 signed high 执行 signed left shift；
依赖 uint32_t -> int32_t 超范围转换；
依赖 implementation-defined arithmetic shift；
通过十进制字符串进行 sequence 转换。

RequestId：int64_t -> SequenceNumber_t
uint64_t bits;
memcpy(&bits, &value, sizeof(bits));

uint32_t high_bits =
    static_cast<uint32_t>(bits >> 32);

int32_t high;
memcpy(&high, &high_bits, sizeof(high));

SequenceNumber_t result;
result.high = high;
result.low =
    static_cast<uint32_t>(bits & UINT32_MAX);

要求：
to_fastdds_sequence(from_fastdds_sequence(x))
保持 x 的 high/low bit pattern；
from_fastdds_sequence(to_fastdds_sequence(v))
保持 v 的 int64_t bit pattern。

RequestId unknown sequence：
必须通过 Fast DDS 2.6.12 frozen `c_SequenceNumber_Unknown`
或等价 baseline helper 明确检测，
不能把 unknown sentinel 当成普通 RequestId sequence。

send_request()：
Fast DDS write 成功但返回 unknown request sample sequence
    -> DdsError
    -> 不发布新的 public RequestId

Server::take_request()：
已经消费的 sample identity sequence unknown
    -> DdsError
    -> public output 按 post-consumption error guarantee 处理

Client::take_response()：
related sequence unknown
    -> 该 response 不可关联
    -> consume/filter
    -> continue scanning
不能返回伪造 RequestId。

MessageInfo publication sequence 使用独立 helper：
std::optional<std::uint64_t>
publication_sequence_from_fastdds(
    const SequenceNumber_t& sequence) noexcept;

唯一算法：
if sequence == frozen Fast DDS c_SequenceNumber_Unknown:
    return std::nullopt

uint32_t high_bits;
memcpy(&high_bits, &sequence.high, sizeof(high_bits));

uint64_t bits =
    (uint64_t{high_bits} << 32) |
    uint64_t{sequence.low};

return bits;

因此：
unknown sentinel -> nullopt
zero 若不是 frozen unknown sentinel -> uint64_t{0}
high < 0 的非 unknown bit pattern -> 仍按 bit-preserving uint64_t 返回
所有非 unknown high/low pattern 都可由 uint64_t 完整表示
不执行 signed numeric interpretation
不隐式复用 RequestId 的 int64_t public semantics。

该 helper 的目的不是判断 RTPS sequence 是否“业务上合理”，
而是把 middleware 已提供且非 unknown 的 64-bit publication identity
确定性地暴露为 MessageInfo::publication_sequence_number。

GUID：
Gid 与 Fast DDS GUID 的转换必须为固定 16-byte bit-preserving copy，
并 static_assert 两侧内部转换 buffer 长度。
unknown/invalid GUID 不通过字符串 parse 猜测恢复。
publication_handle fallback 必须通过单一 frozen Fast DDS identity converter。

Timestamp：
Fast DDS Duration/Time_t 转 public nanoseconds 必须：
验证 seconds >= 0
验证 nanosec 位于 frozen Fast DDS 合法范围
checked 计算：
seconds * 1'000'000'000 + nanosec
若 unavailable / invalid / overflow：
public timestamp = 0
不能 wrap。
reception_sequence_number 不使用 publication sequence 伪造。

### 4.3 TemporarySample

为了保持：
NoData
和 Fast DDS take 前 error
不修改 caller output，
take 不能直接写 caller object。

内部：

```cpp
class TemporarySample
{
public:
    static Result<TemporarySample>
    create(
        const TypeLease& type);

    TemporarySample(const TemporarySample&) = delete;
    TemporarySample& operator=(const TemporarySample&) = delete;

    TemporarySample(TemporarySample&&) noexcept = default;
    TemporarySample& operator=(TemporarySample&& other) noexcept;

    ~TemporarySample() noexcept;

    void* data() noexcept;

private:
    std::unique_ptr<AllocatedSample>
        allocated_sample_;
};
```

创建顺序：
1. 从 TypeLease 取得 `canonical_binding`；
2. 检查 canonical_binding.capability == Healthy；
3. allocate AllocatedSample；
4. `AllocatedSample` strong-own `CanonicalTypeBinding`；
5. 调用 canonical_binding.descriptor 对应 TopicDataType::createData()；
6. 成功后写入 `allocated_sample->data`。

禁止 TemporarySample 从 caller-provided MessageType candidate 重新选择 binding；
TypeLease acquire 后 canonical binding 是唯一 authority。

AllocatedSample allocation 抛 std::bad_alloc：
    -> 原样传播
    -> 不调用 createData()

createData() 返回 nullptr：
    -> DdsError

createData() 抛 std::bad_alloc：
    -> 原样传播

createData() 抛其它未预期 exception：
    -> 原样传播

Move contract：
- moved-from `TemporarySample.backing_ == nullptr`；
- moved-from destructor 是 no-op；
- `backing_ == nullptr` 或 `backing_->data == nullptr` 时 destructor 也是 no-op；
- move assignment 必须先按同一 noexcept/quarantine contract 处理当前 backing，再接管 source backing；不得泄露一个正常可释放的旧 sample。

析构：
调用同 binding deleteData()；
异常不得离开 noexcept destructor，
按 [Message Binding Integration Contract](#fastdds-message-binding-contract)：
CanonicalTypeBinding.capability -> Degraded
AllocatedSample -> ProcessBindingQuarantine。

### 4.4 Temporary → Caller Commit

V1 不增加新的 public binding copy hook。
因此：
temporary typed sample
    ↓
serialize through the same TopicDataType
    ↓
implementation-owned SerializedPayload
    ↓
deserialize into caller object

Payload storage 由：
Fast DDS 2.6.12 specific
SerializationScratch helper
负责。

该 helper：
根据 TypeSupport 的 serialized-size/type-size 能力
checked 计算 payload capacity
禁止：
写死任意固定 payload size

在调用 binding hook 前必须重新检查：
TypeLease canonical binding capability == Healthy。
如果已经 Degraded：
    -> DdsError
    -> caller object unchanged

Serialize returns false：
    -> DdsError
    -> caller object unchanged

Serialize throws std::bad_alloc：
    -> 原样传播
    -> caller object unchanged

Serialize throws other unexpected C++ exception：
    -> 原样传播
    -> caller object unchanged

Payload heap allocation failure：
    -> std::bad_alloc 原样传播
    -> caller object unchanged

Deserialize returns false：
    -> DdsError
    -> DDS sample 已消费
    -> caller object valid/destructible
    -> fields unspecified

Deserialize throws std::bad_alloc：
    -> std::bad_alloc 原样传播
    -> DDS sample 已消费
    -> caller object valid/destructible
    -> fields unspecified

Deserialize throws other unexpected C++ exception：
    -> 原样传播
    -> DDS sample 已消费
    -> caller object valid/destructible
    -> fields unspecified

因此 post-consumption output guarantee 与 exception propagation 是独立保证。
TemporarySample / SerializationScratch 的销毁与 ProcessBindingQuarantine adoption
不得发生在持有其它 DMW mutex 的临界区内。

### 4.5 Registry State

```cpp
enum class RegistryEntryState
{
    Creating,
    Active,
    Retiring,
    Orphaned
};
```

### 4.6 Registry Waiter Lifetime 与 CV Publication Discipline

TypeRegistry / TopicRegistry 使用 registry-level：

```cpp
std::mutex mutex;
std::condition_variable cv;
```

waiter 必须使用与 registry state publication 相同的 mutex 建立 predicate handshake：

```text
unique_lock registry.mutex
while target key is Creating/Retiring:
    registry.cv.wait(lock)
    relookup key/state under the same mutex
```

任何会使 waiter predicate 从 false 变 true 的 state transition（例如 Creating -> Active/Orphaned/erase，Retiring -> erase/Orphaned）都必须：

```text
lock registry.mutex
commit state/map mutation
unlock registry.mutex
registry.cv.notify_all()
```

notify 可以放在 unlock 后，但 predicate authority 的 mutation 必须发生在同一 registry mutex 下；禁止仅靠 atomic state + naked notify 形成 notify-before-wait race。

不使用：

```text
Entry-local condition_variable
+ erase Entry
```

模型，因为 Entry 可以在 waiter 睡眠期间被 erase。

### 4.7 TypeEntry、TypeLease 与 Canonical Binding

```cpp
enum class TypeRegistrationStatus
{
    NotStarted,
    NotRegistered,
    Registered,
    Indeterminate
};
```

```cpp
struct TypeEntry
{
    RegistryEntryState state;

    std::string wire_type_name;
    BindingIdentity binding_identity;

    std::shared_ptr<CanonicalTypeBinding>
        canonical_binding;

    TypeRegistrationStatus
        registration_status{
            TypeRegistrationStatus::NotStarted};

    std::size_t ref_count{0};
};
```

`TypeRegistryState` strong-own Active/Creating/Retiring/Orphaned TypeEntry；
`TypeEntry` strong-own canonical binding；
`CanonicalTypeBinding` strong-own canonical `MessageType::Impl`，
因此 registered TypeSupport 的 lifetime 不依赖第一个 caller descriptor facade 是否仍存在。

`TypeLease` 至少保存：
stable TypeRegistryState pointer
entry key / identity
std::shared_ptr<CanonicalTypeBinding> canonical_binding。

`TypeLease::canonical_binding()` 是 endpoint runtime 唯一允许使用的 binding authority。
Factory 在 acquire TypeLease 后：
不得继续把 caller-provided MessageType::Impl 当作 serializer/registered TypeSupport authority；
可以只保留 caller descriptor 作为不可操作的 metadata/debug reference，
但所有 TypeSupport hook 必须经 TypeLease canonical binding。

### 4.8 TypeRegistry Acquire Transaction

Absent：
1. 在进入任何 Fast DDS API call 前完成：
   - allocate TypeEntry；
   - allocate CanonicalTypeBinding；
   - canonical_binding.descriptor = caller descriptor；
   - registration_status = NotStarted；
   - insert Creating；
2. unlock TypeRegistry；
3. 将本次 Fast DDS registration 标记为 entered Fast DDS API call；
4. 调用 canonical descriptor 对应 TypeSupport::register_type(participant)；
5. 无论 ReturnCode 还是 C++ exception，都先完成 `TypeRegistrationStatus` 和 registry state 更新，再对 caller 返回或传播。

register_type() == OK：
relock TypeRegistry
registration_status = Registered
state = Active
ref_count = 1
construct TypeLease with canonical_binding
notify
unlock
return lease。

register_type() 返回非 OK：
必须通过[operation-specific status matrix](#fastdds-lock-error-model)得到 TypeRegistrationStatus。

若 baseline 明确证明：
该 ReturnCode 表示本次调用没有建立目标 registration：
    registration_status = NotRegistered
    erase Creating
    notify
    unlock
    return mapped Error。

若无法证明没有 Fast DDS registration side effect：
    registration_status = Indeterminate
    state = Orphaned
    ref_count = 0
    retain canonical_binding / TypeSupport
    notify
    unlock
    return mapped Error。

register_type() 在进入 Fast DDS API call 后抛任意 C++ exception：
catch internally
    relock TypeRegistry
    registration_status = Indeterminate
    state = Orphaned
    ref_count = 0
    retain canonical_binding / TypeSupport
    notify
    unlock
    record fixed-size diagnostic
    rethrow original exception。

禁止：
catch exception
    -> erase Creating
    -> release canonical TypeSupport
    -> rethrow。

因为 exception propagation 是 public result；
`Indeterminate/Orphaned` ownership commit 必须先完成。

Creating：
wait + relookup。

Active same wire type name + same BindingIdentity：
    ++ref_count
    return TypeLease referencing existing canonical_binding。
caller descriptor 本身不替换 canonical binding，
其独立 capability 也不存在。

Active same wire type name + different BindingIdentity：
    -> TypeMismatch。

Retiring：
wait + relookup。

Orphaned：
    -> DdsError。
不得建立第二个同 key Fast DDS registration 与 indeterminate historical registration 竞争。

#### 4.8.1 Binding Degradation Propagation

任何通过该 TypeEntry canonical binding 发生的 lifecycle/serialization contract violation：
canonical_binding.capability -> Degraded。

之后同一 Context / TypeEntry 的：
existing Publisher/Subscriber/Client/Server
new endpoint Factory
TemporarySample
service serialization path
全部观察同一 Degraded state。

独立 `make_message_type()` descriptor 不能通过重新 acquire 同 key 绕过 degradation；
它只得到同一个 canonical TypeLease。

#### 4.8.2 Type Release

ref_count > 1：
    -> --ref_count。

ref_count == 1：
在 TypeRegistry mutex 下：
Active -> Retiring
snapshot canonical binding / registration_status
unlock TypeRegistry
调用 unregister_type()。

unregister_type() == OK：
relock
registration_status = NotRegistered
erase
notify
unlock。

unregister_type() 返回非 OK：
根据[operation-specific status matrix](#fastdds-lock-error-model)：
- 如果 baseline 明确证明目标已经 NotRegistered：erase；
- 否则 registration_status = Indeterminate，state = Orphaned，retain canonical binding。

unregister_type() 抛 exception：
noexcept cleanup/retirement boundary 内 catch：
registration_status = Indeterminate
state = Orphaned
retain canonical binding
record diagnostic
不得释放 TypeSupport。

所有 register_type()/unregister_type() Fast DDS API calls 都发生在 TypeRegistry mutex 外。

<a id="fastdds-topic-registry"></a>

### 4.9 TopicEntry 与 TopicRegistry Primary Key

Topic identity 在一个 Context 内对 **resolved DDS topic name** 是 exclusive 的。不能把 wire type 或 TopicQos fingerprint 放入 primary map key 后再期待同名冲突产生 `TypeMismatch`。

```cpp
enum class TopicRegistryCapability
{
    Healthy,
    Degraded
};

struct TopicEntry
{
    RegistryEntryState state;

    // Primary key stable copy.
    std::string topic_name;

    // Name-exclusive invariant fields.
    std::string wire_type_name;
    TopicQosFingerprint canonical_qos_fingerprint;

    // Identifies the current Creating transaction.
    // Non-zero only while state == Creating.
    std::uint64_t creation_transaction_id{0};

    Topic* topic{nullptr};

    CreationStatus creation_status{
        CreationStatus::NotStarted};

    EntityStatus entity_status{
        EntityStatus::KnownDeleted};

    // Creating placeholder is intentionally allowed to exist without a TypeLease.
    // Active/Retiring/Orphaned entries that may own/reference a DDS Topic must have it engaged.
    std::optional<TypeLease> type_dependency;

    std::size_t ref_count{0};
};

struct TopicRegistryState
{
    std::mutex mutex;
    std::condition_variable cv;

    TopicRegistryCapability capability{
        TopicRegistryCapability::Healthy};

    // primary key = resolved DDS topic name only
    TopicTable by_resolved_name;

    // monotonic transaction token allocator, mutex-protected
    std::uint64_t next_creation_transaction_id{1};
    bool creation_transaction_id_exhausted{false};

    ParticipantInfo* participant_info{nullptr};
};
```

`TopicRegistryState::participant_info` 是同 Context 的 stable non-owning evidence pointer，ownership/lifetime 规则见 [Registry Ownership Closure](#fastdds-registry-ownership)。

Topic creation transaction ID：
- 0 invalid；
- 1…`UINT64_MAX` 单调分配；
- never reuse / never wrap；
- 分配 `UINT64_MAX` 后设置 exhausted；
- exhausted 只阻止新的 Absent Topic transaction，已有 Active Topic reuse仍可工作；
- absent acquire 需要新 token但 allocator exhausted -> `ResourceExhausted`，不伪造 registry corruption；
- transaction ID 是 implementation-internal evidence token，不是 public WaitToken；若 ID 已分配而随后 Creating map-node insertion 抛 `std::bad_alloc`，允许留下 ID gap，但不得留下 placeholder/Fast DDS side effect。

`TopicRegistryCapability::Degraded` 表示 name-exclusive registry invariant 已无法可信维护；之后依赖 TopicRegistry 的新 Factory 在完成更高优先级 public/Context/parent/local 检查后返回 `DdsError`。已有 TopicEntry 和 endpoint Info 仍按 conservative teardown 保活/清理。

Topic Fast DDS lifetime 必须独立保活 TypeSupport。

硬性 lock invariant：

```text
TopicRegistry mutex(rank 4)
    -> NEVER acquire/release TypeLease
    -> NEVER enter TypeRegistry(rank 3)
```

所有 `TypeLease` acquire/release 都发生在 TopicRegistry mutex 外。

### 4.10 双 TypeLease

首次 endpoint 的 ownership 固定为：

```text
DataReaderInfo / DataWriterInfo
    -> one endpoint TypeLease

TopicEntry
    -> one independent Topic-owned TypeLease
```

两者是两个独立 TypeRegistry 引用。

禁止把 endpoint TypeLease move 到 TopicEntry，也禁止 endpoint facade/Impl 再持第三个冗余 TypeLease。

### 4.11 Topic Acquire

V1 DDS TopicQos 不从 Publisher/Subscriber/Client/Server endpoint QoS 派生。所有 Topic 使用 4.17 canonical TopicQos baseline；因此不存在 first-creator-wins TopicQos。

Acquire primary lookup：

```text
key = resolved DDS topic name
lock TopicRegistry
lookup key
```

若 registry capability 已 Degraded：在 public/Context/parent/local 优先级检查之后返回 `DdsError`。

Existing entry：

```text
same resolved name + different wire type
    -> TypeMismatch

same name/type + different canonical TopicQos fingerprint
    -> TopicRegistry capability = Degraded
    -> DdsError

same name/type/fingerprint + Active
    -> assert type_dependency engaged
    -> ref_count++
    -> reuse exactly the same DDS Topic

Creating/Retiring
    -> wait registry CV
    -> wake 后 relookup primary name

Orphaned
    -> DdsError
    -> 不得创建第二个同名 DDS Topic 来绕过未知旧状态
```

fingerprint mismatch 是 implementation invariant violation；fingerprint 不是 composite-key 分叉条件。

#### 4.11.1 Absent Topic 三阶段事务：禁止 Topic -> Type lock inversion

**Stage A — reserve name-exclusive Creating placeholder**

```text
lock TopicRegistry

recheck capability/key absent

allocate monotonic creation_transaction_id
    exhausted -> unlock -> ResourceExhausted

insert Creating TopicEntry:
    topic_name/type/fingerprint fixed
    creation_transaction_id = tx
    type_dependency = nullopt
    creation_status = NotStarted
    entity_status = KnownDeleted
    ref_count = 0

unlock TopicRegistry
```

`TopicEntry` 的字符串/fingerprint materialization必须在进入该 critical section 前已经完成；placeholder insertion 本身可能分配 map node，异常按 ordinary allocation channel传播且不留下 entry。

**Stage B — acquire independent Topic-owned TypeLease with no Topic lock held**

`local Result<TypeLease> = TypeRegistry::acquire(...)`

此时 **不得持有 TopicRegistry mutex**。

TypeLease acquire 返回 Error：

```text
lock TopicRegistry
if same key still == Creating(tx):
    erase placeholder
    notify
else:
    registry invariant violation -> capability Degraded
unlock
return original TypeLease acquire Error
```

TypeLease acquire 抛 C++ exception：
执行完全相同 placeholder rollback，然后 rethrow original exception。

**Stage C — publish TypeLease into same Creating transaction**

```text
lock TopicRegistry

lookup key
must be same Creating(tx)
must have type_dependency == nullopt

move local TypeLease -> entry.type_dependency

unlock TopicRegistry
```

如果 same transaction 校验失败：
- 不得把 local TypeLease move 到未知 entry；
- TopicRegistry capability -> `Degraded`；
- unlock；
- release local TypeLease outside Topic mutex；
- return `DdsError`。

从 Stage C commit 起，Creating entry 已拥有 DDS Topic hidden-create path 所需 canonical TypeSupport lifetime。

#### 4.11.2 Fast DDS entity creation

Stage C 成功后，不持有任何 Registry mutex调用：

`create_topic()`

返回有效 `Topic*`：

```text
relock TopicRegistry
verify same Creating(tx)
entry.topic = returned pointer
entry.creation_status = HandleKnown
entry.entity_status = KnownAlive
entry.state = Active
entry.creation_transaction_id = 0
entry.ref_count = 1
notify
unlock
```

explicit failure/nullptr 且[错误与 status matrix](#fastdds-lock-error-model)的 frozen baseline 明确证明无 Topic side effect：

```text
relock
verify same Creating(tx)
creation_status = NoSideEffect
entity_status = KnownDeleted
move engaged Topic-owned TypeLease to local
erase Creating
notify/unlock
release local TypeLease outside Topic mutex
return mapped Error
```

不能证明 no-side-effect，或 entered Fast DDS API call 后抛 C++ exception：

```text
relock
verify same Creating(tx)
creation_status = SideEffectIndeterminate
entity_status = Indeterminate
state = Orphaned
creation_transaction_id = 0
topic = nullptr
retain engaged TypeLease / canonical binding
participant_entities_status = MayContainHiddenEntity
notify/unlock
```

ReturnCode failure 返回 operation-specific Error；C++ exception 在上述 evidence/ownership commit 后原样 rethrow。

禁止：
- 持 TopicRegistry mutex acquire/release TypeLease；
- exception path erase Orphaned entry + release TypeLease；
- Creating waiter自行执行 Type acquire/Fast DDS entity creation；只有持 `tx` 的 transaction owner执行 Stage B/C/Fast DDS entity creation。

### 4.12 Topic Release 与 `EntityStatus`

最后一个 Topic reference：

```text
lock TopicRegistry
Active -> Retiring
snapshot topic + entity_status
unlock
```

只有 `entity_status == KnownAlive` 才允许调用 `delete_topic(topic)`。

删除结果分为 public/Fast DDS result 与 lifetime evidence 两个维度：

```text
proven success / baseline-proven already deleted:
    entity_status = KnownDeleted
    topic = nullptr
    assert type_dependency engaged
    move *type_dependency to local
    type_dependency.reset()
    erase entry
    notify/unlock
    release local TypeLease outside TopicRegistry mutex

failure with frozen baseline evidence target definitely remains alive:
    entity_status = KnownAlive
    state = Orphaned
    retain topic + TypeLease
    allow a later individual retry

ambiguous failure / exception / unknown result:
    entity_status = Indeterminate
    state = Orphaned
    retain backing + TypeLease
    participant_info.participant_entities_status = MayContainHiddenEntity
    NEVER call delete_topic(old topic pointer) again
    rely on Participant contained-graph barrier or Participant delete success
```

`RegistryEntryState::Orphaned` 只是 registry lifecycle state，**不能**替代 `EntityStatus`。后续第二轮 Topic cleanup 只允许选择 `state == Orphaned && entity_status == KnownAlive` 的 entry。

任何 TypeLease release 都不得在 TopicRegistry mutex 内执行，因为 TypeLease release 可能取得 TypeRegistry mutex，而固定 lock rank 为 TypeRegistry < TopicRegistry。

### 4.13 Fast DDS QoS Authority

所有 DMW DDS entity：
DomainParticipant
DDS Publisher
DDS Subscriber
Topic
DataWriter
DataReader
均使用本文规定的 explicit Fast DDS QoS value object。

不得使用：
*_QOS_DEFAULT sentinel
DomainParticipant::get_default_*_qos()
DomainParticipantFactory::get_default_participant_qos()
named default XML endpoint profile
RMW_FASTRTPS_USE_QOS_FROM_XML
RMW_FASTRTPS_PUBLICATION_MODE
FASTDDS_DEFAULT_PROFILES_FILE
DEFAULT_FASTDDS_PROFILES.xml
来决定 DMW entity behavior。

允许进程其它模块加载 XML；
DMW 自己创建的 DDS entity 仍必须使用本文规定的 explicit QoS object。

### 4.14 Canonical QoS Construction Rule

“frozen baseline”不是一句“使用 Fast DDS 默认值”的模糊描述。

V1 的唯一构造算法是：
1. 使用 Fast DDS 2.6.12 对应 QoS class 的 value/default constructor 构造本地 value object；
2. 禁止调用任何 factory/participant 的 get_default_*_qos()；
3. 应用本章定义的 canonical baseline 与 mandatory override；
4. 应用 resolved DMW Qos 中非 SystemDefault policy；
5. 执行 consistency normalization；
6. 生成 QoS fingerprint / golden snapshot；
7. 把该完整 value object 直接传给 create DDS entity。

因此未列为 DMW override 的字段，其 normative value 定义为：
“Fast DDS 2.6.12 对该 QoS class 的 value/default constructor 所产生的字段值”。

这是一条 executable normative definition，
不是“运行时读取 middleware default”。
由于 baseline 严格冻结为 Fast DDS 2.6.12，
两套 conforming implementation 不允许选择不同 XML/profile/factory default。

代码必须集中在：
fastdds_qos_baseline.hpp
fastdds_qos_resolver.cpp
或等价单一 internal module。

禁止各 Factory 自己复制 magic values。

CI golden snapshot 必须完整记录实际创建前的：
DomainParticipantQos
PublisherQos
SubscriberQos
TopicQos
DataWriterQos
DataReaderQos
中 DMW 使用和 Fast DDS equality/consistency 相关的全部字段。
Golden test 是对本节算法的验证，不是规范定义的替代品。

### 4.15 Canonical Entity Baseline

DomainParticipantQos：
base = DomainParticipantQos{}
mandatory：
name = ContextState.participant_name
entity_factory.autoenable_created_entities = true

PublisherQos：
base = PublisherQos{}
mandatory：
entity_factory.autoenable_created_entities = true

SubscriberQos：
base = SubscriberQos{}
mandatory：
entity_factory.autoenable_created_entities = true

TopicQos：
base = TopicQos{}
V1 不从 endpoint Qos 复制：
history
reliability
durability
deadline
lifespan
liveliness
到 TopicQos。
TopicQos 是与 endpoint 无关的 canonical baseline，
解决同 topic/type 不同 endpoint QoS 的确定性问题。

DataWriterQos：
base = DataWriterQos{}
然后应用 CompatibilityProfile override
+
resolved DMW endpoint Qos。

DataReaderQos：
base = DataReaderQos{}
然后应用 CompatibilityProfile override
+
resolved DMW endpoint Qos。

任何新增 Fast DDS policy 如果未来会影响 DMW observable behavior：
必须先进入 baseline module
+
golden snapshot
+
本文版本升级，
不能悄悄继承新的 library default。

### 4.16 Ros2FastDdsHumble Mandatory Overrides

Ros2FastDdsHumble 下必须设置：

DomainParticipantQos：
wire_protocol.builtin.readerHistoryMemoryPolicy
    = PREALLOCATED_WITH_REALLOC_MEMORY_MODE

wire_protocol.builtin.writerHistoryMemoryPolicy
    = PREALLOCATED_WITH_REALLOC_MEMORY_MODE

原因：
frozen Humble rmw_fastrtps baseline 显式允许 built-in discovery history
对超过约 5000-byte discovery payload 进行 reallocation。

DataWriter：
history memory policy
    = PREALLOCATED_WITH_REALLOC_MEMORY_MODE

publish mode
    = SYNCHRONOUS_PUBLISH_MODE

data sharing
    = OFF

DataReader：
history memory policy
    = PREALLOCATED_WITH_REALLOC_MEMORY_MODE

data sharing
    = OFF

Reliable writer：
reliability.max_blocking_time
    = 100 ms
除非 public DMW Qos 将来显式增加该 policy；
V1 public Qos 不暴露该字段，所以该值属于 frozen Fast DDS profile。

这些 override 不读取：
RMW_FASTRTPS_USE_QOS_FROM_XML
RMW_FASTRTPS_PUBLICATION_MODE。

NativeDds profile：
不应用上述 ROS-specific override，
只使用 4.14/4.15 canonical constructor baseline
+
resolved DMW Qos。

### 4.17 SystemDefault Resolution

Public Qos policy == SystemDefault 时：
不查询进程环境，
不查询 XML，
不查询 participant/factory default。

最终值由：
CompatibilityProfile
+
EntityKind
+
Fast DDS 2.6.12 canonical constructor baseline
+
mandatory profile override
确定。

普通 ROS Topic endpoint default：
上层必须显式请求 Qos::ros2_default()。

Service endpoint default：
上层必须显式请求 Qos::ros2_services_default()。

SystemDefault 与 ros2_default()/ros2_services_default() 是不同概念。

### 4.18 TopicQosFingerprint Exact Semantic Equality

`TopicQosFingerprint` 只用于同一 Context 内 Topic canonical invariant 与 golden/debug validation；它不是 public ABI，也不是跨 Fast DDS version 的持久格式。

Fast DDS 2.6.12 `TopicQos` equality authority 固定覆盖以下 **完整 policy 集合**：

```text
topic_data
durability
durability_service
deadline
latency_budget
liveliness
reliability
destination_order
history
resource_limits
transport_priority
lifespan
ownership
```

不得使用“至少覆盖”或“其它 equality-relevant policy”留给实现自行判断。

Fingerprint 构造必须先把每个 policy 转成 DMW 自己的 canonical semantic representation：
- enum -> 明确枚举值；
- duration -> checked `(sec, nanosec)` canonical form，infinite 使用唯一 sentinel；
- history -> `{kind, depth}`；
- resource_limits -> `{max_samples, max_instances, max_samples_per_instance, allocated_samples, extra_samples}`，unlimited 使用唯一 canonical sentinel；
- durability_service -> `{service_cleanup_delay, history_kind, history_depth, max_samples, max_instances, max_samples_per_instance}`；
- topic_data -> exact octet sequence content；
- liveliness -> `{kind, lease_duration, announcement_period}`；
- reliability -> `{kind, max_blocking_time}`；
- transport_priority -> semantic integer value；
- 其它单值 policy -> 对应明确 semantic field。

`TopicQosFingerprint` equality 必须逐 policy/逐 semantic field 比较 canonical representation。

Hash 只允许对同一 canonical representation 按上述固定 policy 顺序进行 semantic field hashing，并满足：

`fingerprint_equal(a, b) => fingerprint_hash(a) == fingerprint_hash(b)`

禁止：
- `memcmp(TopicQos)`；
- hash `TopicQos` raw object bytes；
- hash padding / vptr / allocator state / object address；
- 依赖未正规化的 duration/unlimited 多种内部表示；
- 把 process-local hash 数值写入持久文件并作为未来版本 identity。

TopicRegistry 中保存 canonical representation 或其 collision-safe fingerprint object；如果实现只缓存 hash value，发生 hash collision 时仍必须回到 semantic equality，不能把 hash 相等当作 QoS 相等。

同一 resolved DDS topic name + same wire type 的 canonical TopicQos semantic equality 必须唯一；检测到不同 canonical fingerprint 表示 implementation invariant violation -> TopicRegistry capability `Degraded` + `DdsError`。

### 4.19 KeepLast

Public：
先完全按照同版本 `dmw.md` 校验 KeepLast depth。
例如 `dmw.md` 规定 depth 必须 > 0 时：
depth == 0
    -> InvalidArgument

public argument validation 通过后，
再 checked 转换到：
int32_t dds_depth。

如果一个已经被 `dmw.md` 判定为合法的 public depth
无法表示为 Fast DDS 2.6.12 Fast DDS int32 depth：
    -> Unsupported

Fast DDS 实现不得仅因为 Fast DDS representation 较窄，
自行把一个合法 public value 重新定义成 InvalidArgument。

Fast DDS：
history.kind = KEEP_LAST_HISTORY_QOS
history.depth = dds_depth

ResourceLimits consistency normalization 固定为：

normalize_limit(limit, required):
    if limit == LENGTH_UNLIMITED:
        return LENGTH_UNLIMITED
    if limit < 0:
        -> DdsError  // impossible/corrupt canonical baseline
    return max(limit, required)

resource_limits.max_samples_per_instance =
    normalize_limit(
        baseline_or_resolved.max_samples_per_instance,
        dds_depth)

resource_limits.max_samples =
    normalize_limit(
        baseline_or_resolved.max_samples,
        dds_depth)

max_instances：
保持 canonical/resolved value；
V1 不因 KeepLast depth 人为缩小或扩大 instance 数。

随后执行 implementation consistency validation。
错误分类固定为：

违反 `dmw.md` public KeepLast argument contract：
    -> InvalidArgument

DMW public QoS 合法，
但 Fast DDS 2.6.12 / frozen profile 根本无法表示该 policy
（包括合法 public depth 无法转换为 Fast DDS int32_t）：
    -> Unsupported

本文 deterministic resolver 根据自身 frozen baseline / normalization
产生内部自相矛盾 policy：
    -> DdsError
    -> internal invariant diagnostic

resolver 已生成本文认为合法的 Fast DDS QoS，
但最终 Fast DDS entity creation/set_qos 返回 INCONSISTENT_POLICY：
    -> IncompatibleQos

因此 IncompatibleQos 不得用于表示“当前 Fast DDS version/profile 根本不能表达合法 public policy”。

所有 integer 运算 checked；
禁止 unsigned/signed wrap。

### 4.20 KeepAll

Fast DDS：
history.kind = KEEP_ALL_HISTORY_QOS

Public：
depth = 0

Fast DDS `history.depth`：
保持 canonical baseline value，
因为 KEEP_ALL 下 public depth 不参与 history semantics。

resource_limits：
保持 canonical/resolved baseline，
不得因为 public depth==0 改写成 0。

如果 resource limit 本身导致 KEEP_ALL 无法满足某应用负载，
这是运行时 middleware resource ceiling，
不是把 public KeepAll 翻译为“无限内存承诺”。

### 4.21 Reliability

Reliable：
reliability.kind = RELIABLE_RELIABILITY_QOS

BestEffort：
reliability.kind = BEST_EFFORT_RELIABILITY_QOS

SystemDefault：
保持 profile-resolved canonical baseline。

Ros2FastDdsHumble Reliable writer：
max_blocking_time = 100 ms。

### 4.22 Durability

Volatile：
durability.kind = VOLATILE_DURABILITY_QOS

TransientLocal：
durability.kind = TRANSIENT_LOCAL_DURABILITY_QOS

SystemDefault：
保持 profile-resolved canonical baseline。

### 4.23 Deadline / Lifespan

Deadline：
DataReader
DataWriter
都映射。

Finite duration：
使用 checked Fast DDS duration conversion。

Infinite：
使用 frozen Fast DDS infinite duration constant。

Lifespan：
NativeDds profile：
DataWriter
DataReader
都映射 public lifespan。

Ros2FastDdsHumble profile：
DataWriter
DataReader
都映射 public lifespan，
以跟随 frozen Humble rmw_fastrtps endpoint QoS helper 的 observable endpoint behavior。

TopicQos：
仍保持 4.15 / 4.18 定义的 canonical、endpoint-independent TopicQos；
不得从某个 endpoint 的 lifespan/deadline 动态派生。

这意味着 V1 有意不复制 Humble reference implementation
在 Topic 创建阶段使用 endpoint/profile-derived TopicQos 的 first-creator-wins 细节；
lifespan/deadline 是其中需要特别冻结的两个 policy。
原因是同一 Context 内 TopicRegistry 必须提供：
deterministic canonical TopicQos
与 endpoint creation order 无关
同名同类型 Topic 不因第一个 endpoint 的 lifespan 而改变 DDS Topic identity。

因此 Ros2FastDdsHumble compatibility scope 在这里冻结为：
Reader/Writer endpoint QoS 与 Humble baseline 对齐；
Topic object 使用 DMW canonical TopicQos，可能与 Humble helper 为首个 endpoint 构造的 TopicQos 不同；
这是明确记录的 implementation-internal divergence，而不是遗漏。

该 divergence 必须由 interoperability/golden test 验证：
ROS 2 Humble Publisher/Subscriber 与 DMW 双向通信、matching、sample expiry observable behavior
不得因为 canonical TopicQos 而产生不兼容。
如果实际 baseline test 证明该 divergence 影响要求的 interoperability：
V1 specification 必须在 Frozen 前重新设计 Topic ownership/key，
不能静默恢复 first-creator-wins。

Subscriber options 中合法 lifespan：
映射到 DataReader lifespan；
不得 silently ignore，
也不得因为它是 reader side 而返回 Unsupported。

### 4.24 Liveliness

DataReader：
kind
lease_duration

DataWriter：
kind
lease_duration
announcement_period

Infinite：
lease_duration = Fast DDS infinite
announcement_period =
    canonical profile infinite value

Automatic + Finite(L > 0)：
先 checked 转换 L 为 uint64 integer nanoseconds。

A =
    (L / 3) * 2
    +
    ((L % 3) * 2) / 3

即：
floor(2L/3)

禁止计算：
2 * L
避免 overflow。

Automatic + Finite(0)：
public QosDuration::finite(0) 仍是合法 public value。
如果 Fast DDS 2.6.12 无法形成合法 automatic liveliness：
    -> Unsupported
不是 InvalidArgument。

ManualByTopic：
kind = MANUAL_BY_TOPIC_LIVELINESS_QOS
lease_duration =
    resolved public value

announcement_period =
    canonical profile value

禁止对 ManualByTopic 自动套用 2/3 announcement 算法；
该 period 不作为 DMW 的 manual assertion cadence。

### 4.25 Data Sharing / History Memory / Publication Mode

这些不是 DMW public Qos policy，
V1 由 CompatibilityProfile 决定。

Ros2FastDdsHumble：
writer history memory = PREALLOCATED_WITH_REALLOC_MEMORY_MODE
reader history memory = PREALLOCATED_WITH_REALLOC_MEMORY_MODE
writer publication mode = SYNCHRONOUS_PUBLISH_MODE
writer data sharing = OFF
reader data sharing = OFF

NativeDds：
保持 Fast DDS 2.6.12 canonical constructor baseline。

### 4.26 QoS Golden Tests

每个 profile/entity kind 的 golden test 必须在 Fast DDS entity creation 前比较 resolved object。

至少覆盖：
Participant built-in discovery reader/writer history memory policy
Publisher/Subscriber entity_factory
Topic canonical fingerprint
Reader/Writer history
depth
resource limits
history memory policy
reliability
reliability max_blocking_time
durability
deadline
lifespan
liveliness kind
lease
announcement
publication mode
data sharing

测试必须先加载一个故意冲突的 Fast DDS XML default profile，
再验证 resolved DMW QoS 完全不变。

<a id="fastdds-discovery"></a>

## 5. Listener、Discovery、Matched State 与 EventSource

本章把 listener lifetime、remote observation、service matching、target reader state 和 EventSource 组织为同一 discovery 数据流。Listener callback 只更新内部 authority 和 readiness，不执行用户 callback，也不承担可能阻塞的 rebuild。

### 5.1 ListenerState

```cpp
struct ListenerState
{
    std::mutex mutex;
    std::condition_variable cv;

    bool accepting{true};
    bool degraded{false};

    std::size_t callbacks_in_flight{0};
};
```

### 5.2 CallbackInFlightGuard

Callback 进入：
lock ListenerState

++callbacks_in_flight

accepted = accepting

unlock
然后：
if !accepted:
    return
退出：

```text
lock ListenerState.mutex
--callbacks_in_flight
zero = (callbacks_in_flight == 0)
unlock
if zero:
    cv.notify_all()
```

callback drain 必须使用同一 mutex/predicate handshake：

```text
unique_lock ListenerState.mutex
cv.wait(lock, [&] {
    return callbacks_in_flight == 0;
})
```

`accepting=false` 同样在 `ListenerState.mutex` 下提交。这样 callback entry accounting、first/second drain 与 notify 不存在 lost wake；禁止只对 `callbacks_in_flight` 做无锁读取再裸 `notify_all()`。

必须先完成 in-flight accounting，再根据 `accepted` 决定 callback body 是否 no-op；否则 teardown 期间 late callback 可能绕过 drain accounting。

#### 5.2.1 Listener drain CV discipline

`callbacks_in_flight` 与 `accepting` 的 authority 是 `ListenerState::mutex`。

Callback exit：

```text
lock ListenerState.mutex
assert callbacks_in_flight > 0
--callbacks_in_flight
became_zero = (callbacks_in_flight == 0)
unlock
if became_zero:
    cv.notify_all()
```

Teardown提交 `accepting=false` 同样必须在该 mutex 下完成。

`drain_listener_callbacks_noexcept()`：

```text
try:
    unique_lock ListenerState.mutex
    cv.wait(lock, callbacks_in_flight == 0)
    return true
catch (...):
    fixed diagnostic
    return false
```

若 destructor/noexcept cleanup无法取得可靠 zero evidence：不得释放 listener、ListenerState 或对应 endpoint Info；转入 retirement/terminal retention。normal callback decrement与drain waiter共享同一mutex predicate，因此无lost wake。

### 5.3 Callback Boundary

所有 listener：
void on_xxx(...) noexcept override;
内部：
CallbackInFlightGuard

try:
    bounded implementation state update

catch (...):
    mark affected capability Degraded
异常不得离开 Fast DDS callback boundary。

### 5.4 Callback Lock Rule

Listener callback：
不得持有 ListenerState mutex
再取得 Registry/Event/Endpoint mutex
正确：
enter in-flight
unlock ListenerState

update target implementation state

leave callback

### 5.5 Degraded State

Healthy -> Degraded
单向。
后续成功 callback：
不能自动恢复 Healthy
原因：
已经丢失的 historical change
exact match edge
event cumulative state
无法可靠重建。

### 5.6 Listener Conservative Teardown

Endpoint：
phase -> Closing

close WaitSet hold gate

mark Waitable closing

invalidate EventSource parent

ListenerState.accepting=false

set_listener(nullptr)
outside DMW locks
best effort

first callback drain

remove LocalEndpoint identity
and service/match edges

complete logical WaitSet detach

attempt Fast DDS entity deletion if safe
Fast DDS entity deletion success
DDS entity pointer = nullptr

second callback drain

release listener

release TopicLease

release TypeLease
Fast DDS entity deletion failure/deferred
retain:
DDS entity pointer
listener
ListenerState
TopicLease
TypeLease
进入 retirement。

### 5.7 为什么需要 Second Drain

set_listener(nullptr) 不作为：
绝无 late callback
的 lifetime evidence。
第一次 drain：
清空此前已经进入的 accepted callback
Fast DDS entity deletion 成功后：
第二次 drain
清理 detach/delete 期间已经进入、但已经：
accepting == false
的 no-op callback。
根据 1.5 vendor assumption，DDS endpoint delete 成功后不会再开始新的 callback；
因此 second drain 归零后才构成 listener 可释放的 lifetime evidence。
如果 Fast DDS entity deletion 失败或结果无法证明 Deleted：
不得依赖 drain 释放 listener，必须连同对应 endpoint Info 一起进入 retirement / quarantine。
之后才允许释放 listener。

### 5.8 Discovery Listener

DiscoveryListenerState：

```cpp
struct DiscoveryListenerState
{
    ListenerState callback_state;

    std::weak_ptr<
        ParticipantObservationRegistryState>
        participants;

    std::weak_ptr<
        RemoteEndpointRegistryState>
        remote_endpoints;

    std::weak_ptr<
        ServiceMatchRegistryState>
        service_matches;

    std::weak_ptr<
        TargetReaderObservationRegistryState>
        target_readers;
};
```

不强持有：
ContextState
避免：
listener
 -> Context
 -> participant
 -> listener
cycle。

### 5.9 Discovery Final Teardown

在 DiscoveryListenerState callback mutex 下提交 `accepting=false`。

participant.set_listener(nullptr)

first callback drain 使用5.2.1 zero-count drain protocol；失败则保留 listener/registry backing并进入 conservative terminal path。

continue Fast DDS Context teardown
Participant 删除成功：
second callback drain 同样必须取得 zero evidence
release discovery listener
Participant 删除失败：
listener
+
ListenerState
进入 terminal quarantine。

### 5.10 Final Context Teardown、Implicit Shutdown 与 Callback-stack Barrier

Listener 不允许成为 `ContextState` 最后一个 strong owner；listener/discovery callback只持需要的 stable state/backing，不在 callback entry中临时 strong-own ContextState。因此最后一个 public/internal Context owner的释放不能由 Fast DDS callback自动触发 Participant destruction。

正常最终入口固定为：

```cpp
ContextState::~ContextState() noexcept
{
    const auto shutdown_terminal =
        ensure_shutdown_terminal_for_destruction_noexcept();

    if (shutdown_terminal == ShutdownExecutionState::Completed)
    {
        run_final_teardown_once();
    }
    else
    {
        assert(shutdown_terminal == ShutdownExecutionState::Failed);
        quarantine_without_normal_teardown_noexcept();
    }
}
```

`ensure_shutdown_terminal_for_destruction_noexcept()` 只复用 2.24/2.25：
- `Idle` -> 调用 `shutdown_noexcept()` 竞争唯一 executor；
- `Running` -> 在 `runtime_cv` 上等待已有 executor进入 `Completed/Failed`；
- `Completed` -> 返回；
- `Failed` -> **不得 retry partial shutdown phases**，返回 Failed；
- 不允许 exception越过 noexcept boundary。

正常情况下，2.25 已经在 Context facade/Impl析构时提交 implicit shutdown，因此 endpoint/Node 等 surviving owners只延长 `ContextState` lifetime，不会延迟 shutdown linearization。这里是最后的 defensive barrier。

`run_final_teardown_once()` 使用 `final_teardown_state` CAS：

```text
NotStarted -> Running -> Completed
                     \-> Quarantined
```

规则：
- 只有 `NotStarted -> Running` winner执行[normal final Fast DDS teardown](#fastdds-teardown)；
- winner进入 teardown 前必须再次确认：
  - `RuntimeState == Shutdown`
  - `shutdown_execution == Completed`
- `shutdown_execution == Failed` 永远不允许进入 normal child/Fast DDS entity deletion sequence；
- runtime quiescence未被证明 -> terminal retention/quarantine；
- retirement retry线程不得直接执行 Participant final teardown；
- `Running` 不允许第二执行者并发进入；
- successful final teardown -> `Completed`；
- ownership转移到 `ProcessTerminalQuarantine` -> `Quarantined`；
- `Completed/Quarantined` terminal，不回退；
- 进入函数必须断言当前线程不在任何 DMW/Fast DDS listener callback stack；
- final teardown可以发生在任意释放最后一个合法 `ContextState` strong owner的非-callback thread，不绑定特定用户线程。

### 5.11 Ordinary Endpoint StatusMask

Publisher：
publication_matched
offered_deadline_missed
offered_incompatible_qos
liveliness_lost
Subscriber：
subscription_matched
requested_deadline_missed
requested_incompatible_qos
liveliness_changed
sample_lost

### 5.12 Service StatusMask

Client request Writer：
publication_matched
Client response Reader：
subscription_matched
Server request Reader：
subscription_matched
Server response Writer：
publication_matched
Data readiness：
Reader StatusCondition(DATA_AVAILABLE)
Client/Server 不维护没有 public API 消费者的普通 Event 状态。

### 5.13 Ordinary Matched Count

普通 endpoint 保存：
std::atomic<std::size_t>
    matched_count;
Matched callback：
Fast DDS status.current_count
    ↓
checked convert
    ↓
publish atomic snapshot
非法 Fast DDS count：
matched-count capability
    -> Degraded
后续 query：
DdsError

### 5.14 ParticipantObservationRegistry 与 RemoteEndpoint State

#### 5.14.1 唯一 Participant lifecycle/tombstone authority

`ParticipantObservationRegistryState` 是 Context 内 remote Participant incarnation/lifecycle 的唯一 authority。

```cpp
enum class DiscoveryRegistryCapability
{
    Healthy,
    Degraded
};

enum class ParticipantLifecycle
{
    Active,
    Removed
};

struct ParticipantObservationEntry
{
    GuidPrefix participant_prefix;

    std::atomic<ParticipantLifecycle> lifecycle{
        ParticipantLifecycle::Active};

    std::atomic<std::uint64_t> generation{0};
};

struct ParticipantObservationRegistryState
{
    std::mutex mutex;

    // Atomic so Target predicate can observe degradation without taking rank-5 mutex.
    std::atomic<DiscoveryRegistryCapability> capability{
        DiscoveryRegistryCapability::Healthy};

    std::uint64_t registry_generation{0};

    // Entries/tombstones are retained for Context lifetime.
    ParticipantObservationTable entries;
};
```

`ParticipantObservationTable` value 是
`std::shared_ptr<ParticipantObservationEntry>` 或等价 stable ownership。
entry 一旦发布：
- Context lifetime 内不 erase；
- object address/control block 稳定；
- RemoteEndpointEntry / TargetReaderKey / target entries 可以保存 shared participant observation handle；
- `lifecycle/generation` 通过 atomic read 支持 Target predicate 无锁观察 participant terminal state。

Participant registry update：
- first observation of absent prefix -> materialize Active entry；
- participant remove of absent prefix -> **materialize Removed tombstone**；
- Active -> Removed checked increment generation；
- duplicate remove -> idempotent；
- Removed terminal，不回退；
- registry generation checked monotonic、never wrap；
- allocation/generation/invariant failure -> capability `Degraded`，callback boundary 内不传播 exception。

[GuidPrefix deployment constraint](#fastdds-guid-prefix-constraint) 使“Removed 后不同 incarnation 复用相同 prefix”不属于 supported V1 deployment；因此 Removed tombstone 可以作为 Context-lifetime terminal state。

ordinary data path 需要 participant handle 时，唯一 helper 固定为：

```text
get_or_create_participant_observation(prefix):
    lock ParticipantObservationRegistry                 // rank 5
    if capability != Healthy:
        unlock
        return DdsError

    if entries contains prefix:
        snapshot existing stable handle
        unlock
        return existing handle                          // Active or Removed

    unlock
    allocate candidate ParticipantObservationEntry(Active)

    lock ParticipantObservationRegistry                 // rank 5
    if capability != Healthy:
        unlock
        destroy candidate outside lock
        return DdsError

    if entries now contains prefix:
        snapshot existing stable handle
        unlock
        destroy candidate outside lock
        return existing handle                          // Active or Removed

    checked registry_generation + 1
        exhausted:
            capability = Degraded
            unlock
            destroy candidate outside lock
            signal_target_dependency_change() with no registry lock
            return ResourceExhausted

    insert candidate as Active using strong-guarantee table insertion
        allocation failure:
            registry/table/generation remain unchanged
            unlock by RAII
            propagate std::bad_alloc

    commit registry_generation
    snapshot inserted stable handle
    unlock
    return handle
```

该 helper 不读取 Remote/Target registry，也不在 rank 5 下调用 Fast DDS API 或 dependency
wake。与 participant remove 的竞争由 rank-5 commit 顺序线性化：helper 先 commit 时，remove
随后把同一 canonical entry 转成 `Removed`；remove 先 commit 时，helper 返回已有 Removed
tombstone，绝不创建第二个 entry 或复活为 Active。callback/noexcept path 使用同一 table
authority，但在 allocation/exception failure 时按本节规则降级并在 unlock 后 signal，不能让
exception 越过 callback boundary。

<a id="fastdds-discovery-commit-order"></a>

#### 5.14.2 Discovery callback commit order

Participant tombstone只有一个 authority，并且 dependent registry更新必须遵守单调 lock rank。

```text
A. Participant authority
   lock ParticipantObservationRegistry            // rank 5
   materialize/update participant entry/capability
   snapshot stable shared ParticipantObservationEntry
   unlock

B. Remote + service dirty
   lock RemoteEndpointRegistry                    // rank 6
   update remote endpoint/tombstone
   if remote change must dirty service graph:
       lock ServiceMatchRegistry                  // rank 7; only 6 -> 7 nesting
       mark Active local entries NeedsRebuild / advance generation
       unlock ServiceMatchRegistry
   unlock RemoteEndpointRegistry

C. Target-specific update
   lock TargetReaderObservationRegistry           // rank 8; no 5/6/7 held
   update exact/participant-response state
   advance target generation
   unlock
   cv.notify_all()

D. External dependency wake
   participant/shutdown/capability changes call
   signal_target_dependency_change() with no other registry lock held
```

禁止 Participant与Remote/Service/Target同时持锁；禁止 Target(8)->Participant(5)/Remote(6)/Service(7)；禁止 Target update后持Target mutex再dirty Service。

Partial failure：
- Participant commit失败 -> participant capability `Degraded`；不伪造 dependent success；调用 `signal_target_dependency_change()`；
- Remote update失败 -> Remote capability `Degraded`；若丢失response-reader target correctness信息，则调用 `degrade_target_and_signal()`；
- Target update失败 -> `degrade_target_and_signal()`；
- Service dirty failure/exhaustion -> ServiceMatch capability `Degraded`；不回滚已提交 Participant/Remote state；
- Participant `Removed`一旦提交永不因后续partial failure回滚。

#### 5.14.3 Remote endpoint lifecycle

```cpp
enum class RemoteEndpointLifecycle
{
    Active,
    Removed,
    ParticipantRemoved
};

struct RemoteEndpointEntry
{
    RemoteEndpointGuid guid;
    GuidPrefix participant_prefix;

    std::shared_ptr<const ParticipantObservationEntry>
        participant;

    EndpointKind kind;
    std::string dds_topic_name;
    std::string wire_type_name;

    RemoteEndpointLifecycle lifecycle{
        RemoteEndpointLifecycle::Active};
};

struct RemoteEndpointRegistryState
{
    std::mutex mutex;
    DiscoveryRegistryCapability capability{
        DiscoveryRegistryCapability::Healthy};

    std::uint64_t registry_generation{0};

    RemoteEndpointTable endpoints;
    ParticipantEndpointIndex by_participant;
};
```

`RemoteEndpointRegistryState` **不再拥有 ParticipantObservationTable**。

每次 lookup/rebuild/availability 使用 remote entry 前必须先检查：
- ParticipantObservationRegistry global capability Healthy；
- `entry.participant->lifecycle != Removed`。

Participant Removed 时，即使 endpoint-specific `lifecycle` 尚未被 best-effort 改成 `ParticipantRemoved`，shared participant tombstone仍是 correctness authority。

Remote endpoint callback FSM：

```text
add for absent GUID + participant Active
    -> insert Active

duplicate add with identical immutable identity while Active
    -> idempotent no-op

change for Active GUID
    -> immutable identity must stay identical
    -> otherwise registry Degraded

remove for Active GUID
    -> Removed

duplicate remove for Removed
    -> idempotent no-op

remove for absent GUID
    -> materialize Removed endpoint tombstone
       using callback GUID/prefix/kind/topic/type identity available from Fast DDS API callback

add/change for GUID already Removed while participant still Active
    -> registry Degraded
    -> cannot distinguish late callback from unsupported endpoint GUID incarnation reuse

participant lifecycle Removed
    -> any add/change/remove is late no-op/diagnostic
    -> effective lifecycle is ParticipantRemoved

ParticipantRemoved / Removed
    -> never resurrect
```

如果 absent-remove callback 无法提供足够 immutable identity来 materialize一个可验证 tombstone：
`RemoteEndpointRegistryState.capability -> Degraded`；
不得把 remove 静默丢掉，否则 later add 会产生不同 service availability。

Remote endpoint tombstone Context-lifetime保留；V1 接受 extreme churn 下无界增长。未来 hard bound必须版本化，不能通过 correctness-breaking eviction实现。

`registry_generation` checked monotonic、never wrap；若仍需要记录 update 而已到 `UINT64_MAX`，registry -> Degraded。

callback 中任何需要新增 map/index entry 的 allocation若抛 `std::bad_alloc`：
异常不得离开 callback；
Remote capability -> Degraded；
若丢失的是 target correctness 所需 response-reader lifecycle 信息，还必须按 [discovery commit order](#fastdds-discovery-commit-order) 单向 degrade Target capability 并 notify。

### 5.15 Per-local Service Match Entry

```cpp
enum class LocalServiceEntryPhase
{
    Active,
    Closing,
    Removed
};
```

```cpp
enum class MatchRebuildState
{
    Clean,
    NeedsRebuild
};
```

```cpp
struct LocalServiceMatchEntry
{
    LocalEndpointId local_id;
    LocalServiceEntryPhase phase{LocalServiceEntryPhase::Active};
    MatchRebuildState rebuild_state{MatchRebuildState::Clean};
    std::uint64_t match_generation{0};
    std::uint64_t observed_remote_registry_generation{0};
    set<RemoteEndpointGuid> matched;
};
```

```cpp
struct ServiceMatchRegistryState
{
    std::mutex mutex;
    DiscoveryRegistryCapability capability{DiscoveryRegistryCapability::Healthy};
    LocalServiceMatchTable entries;
};
```

`NeedsRebuild` 是 **per-entry dirty state**，不是全局 capability。一个 local service endpoint 的 transient ordering/match delta 不得把其它 service endpoint 一起标脏。
`ServiceMatchRegistryState.capability == Degraded` 只表示 registry structure/invariant 已无法可信维护。

`match_generation` checked monotonic、never wrap。若某 Active/Closing entry 的一次新 logical edge/phase mutation 需要再次 increment，而当前值已经是 `UINT64_MAX`：

`ServiceMatchRegistryState.capability = Degraded`

V1 不定义 per-entry Degraded，也不把 generation 归零；这是避免 stale rebuild ABA 的唯一 terminal 处理。之后所有依赖 exact service graph 的 operation -> `DdsError`，普通 matched-count query 仍可独立工作。

local endpoint close/unregister：
1. 在 ServiceMatchRegistry mutex 下 `phase Active -> Closing`；
2. checked advance 该 entry `match_generation`；
3. 从可 rebuild/availability candidate 中排除；
4. Fast DDS listener/match teardown 完成后 `Closing -> Removed`；
5. late callback/rebuild result 看到非 Active phase 必须丢弃，不能重建已关闭 endpoint。

### 5.16 Match / Discovery Update Notification

Fast DDS matched callback 是 QoS compatibility authority；DMW 不重新实现 DDS QoS evaluator。

Matched callback：
- 先独立更新 ordinary matched count；
- lock ServiceMatchRegistry；
- lookup local Active entry；不存在/Closing/Removed -> late callback no-op；
- checked increment `match_generation`；若已为 `UINT64_MAX` 且仍需 increment，则 global ServiceMatchRegistry capability -> Degraded，停止 exact graph mutation；
- 如果 `current_count_change` 与 last remote identity 可证明 exactly one add/remove，允许无分配/受控分配 fast-path 更新 matched set；
- unexpected delta、identity 暂不可解释、fast-path allocation failure -> 仅该 entry `NeedsRebuild`；callback 不做 Fast DDS discovery enumeration；
- registry structure corruption -> global capability Degraded。

RemoteEndpointRegistry successful add/change/remove/participant-remove commit 后：
- checked advance `registry_generation`；
- 按 lock rank RemoteEndpointRegistry -> ServiceMatchRegistry；
- 对所有 Active local service entries 设置 `NeedsRebuild` 并 checked increment 各自 `match_generation`；任一 entry generation 已耗尽且仍需 increment -> global ServiceMatchRegistry capability Degraded。
V1 允许 O(number_of_local_service_entries) 的 bounded iteration；不得在 callback 中为 dirty notification 新分配容器。
这样无需 correctness-sensitive reverse index，也不会漏掉一个 remote discovery change 对 service pairing 的影响。

### 5.17 Exact Match Rebuild 与 Service Availability

普通 runtime operation（`service_is_available()` 及需要 exact graph 的内部路径）发现 target entry `NeedsRebuild`：

```text
OperationGuard

atomic check ParticipantObservationRegistry capability == Healthy

lock ServiceMatchRegistry
verify global capability Healthy
verify entry Active
snapshot local_id + match_generation + stable DDS endpoint Info
unlock

lock RemoteEndpointRegistry
verify capability Healthy
snapshot remote registry_generation / required remote entries
snapshot each remote entry stable ParticipantObservationEntry handle
unlock

no Registry mutex held
DataWriter -> get_matched_subscriptions()
DataReader -> get_matched_publications()
construct candidate matched GUID set

for every candidate remote entry:
    recheck participant capability atomically
    recheck participant lifecycle/generation atomically
    Removed/changed -> candidate snapshot stale; leave NeedsRebuild/retry

relock RemoteEndpointRegistry
verify capability/generation still compatible
unlock

relock ServiceMatchRegistry
verify entry still Active
verify match_generation unchanged
if stale -> discard and retry/leave NeedsRebuild
else atomic replace matched set; rebuild_state=Clean
```

如果 Fast DDS matched snapshot 中 remote endpoint 尚未出现在 RemoteEndpointRegistry：视为 discovery ordering transient；保持该 entry `NeedsRebuild`，`service_is_available()` 保守返回 false。

`std::bad_alloc` 在普通 rebuild 路径原样传播，entry 保持 NeedsRebuild。
Fast DDS discovery enumeration Unsupported/不可恢复 error、identity 无法可靠转换、registry invariant corruption -> 相应 registry capability Degraded，后续 exact graph operation -> DdsError。

Service availability pairing：RemoteEndpointRegistry 按 remote Participant GuidPrefix 分组；只有同一 remote Participant 同时具有 compatible request/response counterpart 才返回 true，禁止跨 Participant 拼接。

#### 5.17.1 RemoteEndpointRegistry Stored Fields

至少保存：RemoteEndpointGuid、Participant GuidPrefix、EndpointKind、resolved DDS topic name、wire type name、lifecycle。
这些字段用于 identity/removal/participant lifecycle/target-reader history；不自行计算 DDS QoS compatibility。

<a id="fastdds-target-dependency"></a>

### 5.18 TargetReaderObservationRegistry 与共享 Participant Authority

`TargetReaderObservationRegistry` 是 `Server::send_response()` 的 target-specific live observation authority；Participant terminal lifecycle不在这里复制，而通过 `TargetReaderKey`/entry strong-own 的 shared `ParticipantObservationEntry` 读取。

目标状态：

```cpp
enum class TargetReaderState
{
    NeverObserved,
    Matched,
    KnownUnmatched,
    Removed
};
```

`ParticipantRemoved` 不再作为 target-local stored enum value；它是：

`participant->lifecycle == ParticipantLifecycle::Removed`

推导出的更高优先级 effective state。

目标 key：

```cpp
enum class TargetReaderKeyKind
{
    ExactReader,
    ParticipantResponseSet
};

struct TargetReaderKey
{
    TargetReaderKeyKind kind;
    ServiceKey service;
    GuidPrefix participant_prefix;

    std::shared_ptr<const ParticipantObservationEntry>
        participant;

    std::optional<EndpointGuid> exact_reader_guid;
};
```

exact 与 participant-level 使用两个索引：

```cpp
struct ExactTargetReaderEntry
{
    EndpointGuid reader_guid;
    GuidPrefix participant_prefix;
    ServiceKey service;

    std::shared_ptr<const ParticipantObservationEntry>
        participant;

    TargetReaderState state{
        TargetReaderState::NeverObserved};

    std::uint64_t observation_generation{0};
};

struct ParticipantResponseEntry
{
    GuidPrefix participant_prefix;
    ServiceKey service;

    std::shared_ptr<const ParticipantObservationEntry>
        participant;

    TargetReaderState state{
        TargetReaderState::NeverObserved};

    std::uint64_t observation_generation{0};

    set<EndpointGuid> matched_response_readers;
};

struct TargetReaderObservationRegistryState
{
    std::mutex mutex;
    std::condition_variable cv;

    DiscoveryRegistryCapability capability{
        DiscoveryRegistryCapability::Healthy};

    std::uint64_t registry_generation{0};

    // Target mutex-protected external dependency/wake stamp.
    // Participant lifecycle/capability and shutdown handlers advance this before notify.
    std::uint64_t dependency_generation{0};

    ExactTargetReaderTable exact_readers;
    ParticipantResponseTable participant_responses;
};
```

`TargetReaderObservationRegistryState` **不含 ParticipantObservationTable**。

`ParticipantResponseTable` exact key 是 `{ServiceKey, GuidPrefix}`；
`ExactTargetReaderTable` exact key 是 response Reader GUID，并校验 stored `ServiceKey` / participant identity。
禁止“至少是”式 key 定义。

所有 target generation checked monotonic、never wrap。
global/per-entry/dependency generation耗尽 -> Target capability `Degraded`；
后续 target resolution -> `DdsError`。

Target entry/materialization 必须先在 **不持 Target mutex** 的情况下从 ParticipantObservationRegistry获取 stable participant handle，然后再 lock Target registry publish target-specific state；禁止 Target(rank 8) -> Participant(rank 5)。

外部 dependency change 的 lost-wake-free bridge 固定为：

```text
signal_target_dependency_change() noexcept:
    try:
        lock TargetReaderObservationRegistry.mutex
        checked advance dependency_generation
            exhausted -> capability = Degraded
        unlock
        cv.notify_all()
    catch (...):
        // callback/noexcept boundary
        atomic/fixed diagnostic
        // 100ms absolute-deadline fallback remains the terminal progress backstop
```

以下 commit 后必须调用该 bridge：
- Participant lifecycle/generation change；
- ParticipantObservationRegistry capability -> Degraded；
- Remote failure 按 [discovery commit order](#fastdds-discovery-commit-order) 单向传播到 Target capability；
- EphemeralInterruptibleWait shutdown request。

这样 normal path 中 notifier会取得与 `cv.wait_until` 相同的 Target mutex，不存在“predicate刚检查false、notify先发生、随后才睡眠”的裸 atomic notify race。

`degrade_target_and_signal()` 固定为：

```text
try:
    unique_lock TargetReaderObservationRegistry.mutex
    capability = Degraded
    checked advance dependency_generation if representable
    unlock
    cv.notify_all()
catch (...):
    fixed diagnostic
    // 100ms absolute deadline remains progress backstop
```

该 helper同样只能在零其它registry lock时调用。

### 5.19 TargetReader State 与 Update FSM

Effective participant lifecycle具有最高优先级：

```text
target_key.participant == null
    -> invariant corruption -> Target capability Degraded

ParticipantObservationRegistry capability != Healthy
    -> DdsError

target_key.participant->lifecycle == Removed
    -> effective ParticipantRemoved
    -> do not reinterpret absent target-specific entry as NeverObserved
```

Exact entry target-local FSM：

```text
exact entry absent                     -> NeverObserved
response Reader discovered             -> KnownUnmatched
actual matched add                     -> Matched
actual matched remove                  -> KnownUnmatched
endpoint remove                        -> Removed
Removed                                -> terminal for that exact Reader GUID
```

Participant-response target-local FSM：

```text
service-specific entry absent          -> NeverObserved
matched_response_readers non-empty     -> Matched
participant observed but set empty     -> KnownUnmatched
```

Participant-response entry 不使用 exact `Removed` terminal；一个 response Reader 删除不能证明同 Participant 的其它 response Reader 不存在。

无论 exact 还是 fallback，只要 shared participant lifecycle变成 `Removed`，effective state立即是 `ParticipantRemoved`，允许对应 7.10/7.11 terminal no-write success。

Fast DDS discovery callback：
- 先按 [discovery commit order](#fastdds-discovery-commit-order) 获取并提交 shared participant observation；
- response Reader add/change/remove 更新 target-specific entry；
- participant remove只需在 ParticipantObservationRegistry commit `Removed` 即已完成 correctness terminal commit；
- 可以 best-effort 更新已有 target entries/diagnostic，但**没有 service-specific entry也不影响 terminal lookup**；
- immutable target identity mutation/impossible transition -> Target capability `Degraded`；
- callback allocation failure catch inside callback -> Target capability `Degraded`，exception不越过 callback boundary；
- commit state/generation 后 unlock，再 `cv.notify_all()`。

Fast DDS matched callback：
- actual response-reader match 是 compatibility authority；
- 首先取得/验证 stable participant observation handle，且不持 Target mutex；
- participant 已 Removed -> late callback no-op/diagnostic + notify不是必需；
- 否则 exact entry `Matched <-> KnownUnmatched`；
- participant-response entry insert/erase matched reader GUID，并据 set emptiness发布 `Matched/KnownUnmatched`；
- update成功后 checked advance entry/global generation；
- unlock mutex 后 `cv.notify_all()`。

Remote endpoint lifecycle update 与 matched update可以不同顺序到达；target-specific transition必须 idempotent；participant Removed authority永不 resurrection。

<a id="fastdds-target-predicate"></a>

### 5.20 Target Predicate Wait、Ownership 与 Lost-wake-free Lookup

`PendingEntry` strong-own `std::shared_ptr<const TargetReaderKey>`。该 immutable key在 `take_request()` 中、零 Pending/Target mutex状态下取得 stable ParticipantObservationEntry并完成所有字符串/key allocation，然后才插入 Pending map；`send_response()` 只做 noexcept shared_ptr copy。

predicate不获取 ParticipantObservationRegistry mutex。为了同时避免 lock inversion 与 lost wake，participant atomic authority必须在取得 Target mutex后再做一次最终recheck：

```text
// optional fast precheck
if participant capability atomic != Healthy -> DdsError
if target_key.participant == null -> invariant -> Target Degraded/DdsError
if participant.lifecycle atomic == Removed -> ParticipantRemoved

unique_lock TargetReaderObservationRegistry.mutex

// mandatory under-Target-mutex recheck
if participant capability atomic != Healthy:
    unlock -> DdsError
if participant.lifecycle atomic == Removed:
    unlock -> ParticipantRemoved
if Target capability != Healthy:
    unlock -> DdsError

lookup exact/service-specific state
snapshot target registry/entry generation
snapshot dependency_generation
check Context/ephemeral shutdown request

if matched/terminal -> unlock and decide
else cv.wait_until(lock, absolute_deadline, full predicate)
```

完整 predicate在 Target mutex下重读：participant capability、participant lifecycle/generation、`dependency_generation`、Target capability/registry/entry generation、Context/ephemeral shutdown和deadline。

Lost-wake evidence：
- external participant/shutdown commit先发生 -> notifier随后取得 Target mutex推进 dependency_generation；waiter之后取得mutex时mandatory recheck直接看到新authority；
- waiter先取得 Target mutex -> notifier不能越过；`cv.wait_until`原子释放mutex并进入wait后，notifier推进generation+notify；
- target-local update在Target mutex下提交state/generation后unlock+notify；
- signal helper catastrophic failure时，send_response固定100ms absolute deadline保证最终重读authority，不能无界阻塞。

Exact：participant Removed或exact Removed -> terminal no-write success；Matched -> write；NeverObserved/KnownUnmatched -> wait。
Fallback：participant Removed -> terminal no-write success；matched set非空 -> write；其它 -> wait。

Target terminal observations 不做 correctness-breaking eviction；GuidPrefix terminal 语义边界见 [deployment constraint](#fastdds-guid-prefix-constraint)。

### 5.21 EventSource

一个 endpoint：
one EventSourceState
其下：
many EventState instances
概念：

```cpp
struct EventSourceState
{
    std::mutex mutex;

    std::atomic<ParentState> parent;

    EventSourceTable source;

    EventRegistrationTable events;
};
```

`EventRegistrationTable` 以 checked monotonic、EventSource-lifetime never-reuse 的
`event_registration_id` 为 key，value 是 `std::weak_ptr<EventState>`；它必须支持按 ID 的
next-live traversal。Event Factory 在 publish 前完成 EventState、table node 与所需 ownership
allocation；ID exhaustion返回 `ResourceExhausted`，不 wrap。

Event Factory commit：
在 EventSource mutex 下注册 EventState。
Event destruction：
在 EventSource mutex 下移除自身 weak entry；
同时允许 opportunistic erase 已 expired entry。
因此正常 create/destroy Event 不允许让 stale weak entries 无界增长。
callback path 不承担 registration-table compaction，避免把 cleanup work 放入 Fast DDS listener callback。

### 5.22 Event Callback Fan-out

Listener：
lock EventSource

update cumulative state

snapshot weak EventState targets into a temporary vector

unlock

notify registered WaitSets
如果 snapshot allocation 抛：
catch in listener
mark EventType Degraded
即使通知失败：
bounded WaitSet slice
+
logical readiness precheck
仍最终观察 Degraded state。
V1 明确接受 Event fan-out snapshot 可能分配内存，这是 correctness-first tradeoff；
callback allocation failure 不能传播，也不能悄悄丢失 event history，因此 capability 必须 Degraded。
未来若优化为 intrusive/preallocated fan-out，只能改变内部性能，不能改变 Event public semantics。

<a id="fastdds-endpoint-data-path"></a>

## 6. DDS Endpoint Info 与数据路径

本章按照 backing ownership、Fast DDS entity creation evidence、wait hold、public operation 和 endpoint teardown 的顺序定义 Publisher/Subscriber 数据路径。所有 take 操作都区分 sample consumption 前后的输出保证。

### 6.1 WaitSetHoldState

用于：
DataReaderInfo
GuardConditionInfo

```cpp
struct WaitSetHoldState
{
    std::atomic<bool>
        accepting{true};

    std::atomic<std::uint64_t>
        refs{0};
};
```

### 6.2 Hold Acquire

if accepting == false:
    Closed

CAS refs + 1
with checked overflow

increment success
    ↓
recheck accepting

if now false:
    release acquired ref
    return Closed

else:
    Acquired
如果 refs 无法继续表示：
internal build failure
不得 wrap。
该情况不会新增 public WaitSet token：
WaitSetInfo construction fails
    ->
DdsError

### 6.3 Hold Release

atomic refs--
不得在此操作中获取：
DataReaderInfo mutex
OrphanRegistry mutex
WaitSet mutex
这样避免：
WaitSet reconciliation
    ->
lower-rank endpoint mutex
逆序。
Hold 归零以后：
在 reconciliation mutex 外
触发 deferred reader cleanup retry。

### 6.4 DataReaderInfo

```cpp
enum class ReaderDeleteState
{
    Alive,
    DeleteDeferredByWaitSet,
    Deleting,
    Deleted,
    Orphaned,
    Indeterminate
};
```

```cpp
struct DataReaderInfo
{
    std::mutex mutex;

    ReaderDeleteState state{
        ReaderDeleteState::Alive};

    DataReader* reader{nullptr};

    CreationStatus creation_status{
        CreationStatus::NotStarted};

    ListenerOwner listener;

    std::shared_ptr<ListenerState>
        listener_state;

    TopicLease topic;
    TypeLease type;

    WaitSetHoldState waitset_holds;
};
```

### 6.5 DataWriterInfo

```cpp
enum class WriterDeleteState
{
    Alive,
    Deleting,
    Deleted,
    Orphaned,
    Indeterminate
};
```

```cpp
struct DataWriterInfo
{
    std::mutex mutex;

    WriterDeleteState state{
        WriterDeleteState::Alive};

    DataWriter* writer{nullptr};

    CreationStatus creation_status{
        CreationStatus::NotStarted};

    ListenerOwner listener;

    std::shared_ptr<ListenerState>
        listener_state;

    TopicLease topic;
    TypeLease type;
};
```

### 6.6 GuardConditionInfo

```cpp
enum class GuardConditionStatus
{
    Healthy,
    LogicalOnlyDegraded
};
```

```cpp
struct GuardConditionInfo
{
    GuardConditionHandle guard;

    std::atomic<GuardConditionStatus>
        status{GuardConditionStatus::Healthy};

    WaitSetHoldState
        waitset_holds;
};
```

Historical Fast DDS WaitSet generation：
继续持有 shared_ptr<GuardConditionInfo>
因此 public GuardCondition facade 可以先析构。

### 6.7 ConditionInfo

```cpp
enum class ConditionKind
{
    ReaderStatusCondition,
    PublicGuardCondition,
    PrivateControlGuard
};
```

```cpp
struct ConditionInfo
{
    ConditionKind kind;

    Condition* condition{nullptr};

    std::shared_ptr<DataReaderInfo>
        reader;

    std::shared_ptr<GuardConditionInfo>
        public_guard;

    std::shared_ptr<ControlGuardInfo>
        control_guard;
};
```

按 kind 只使用对应 owner。

### 6.8 Reader Endpoint Destruction

phase -> Closing

reader.waitset_holds.accepting=false

mark Waitable closing

invalidate Events

在 ListenerState mutex 下提交 `listener.accepting=false`

set_listener(nullptr)
best effort

first callback drain 使用5.2.1 zero-count drain protocol；失败则retain backing/retirement

remove LocalEndpointId
and discovery/match contribution

auto-detach WaitSet registration

drain Registration active_wait_count
之后：
reader.waitset_holds.refs == 0 ?
Zero
允许：
delete_datareader()
Non-zero
严格禁止：
delete_datareader()
改为：
state =
DeleteDeferredByWaitSet

transfer DataReaderInfo
to OrphanedEndpointRegistry
适用于：
Subscriber
Client response Reader
Server request Reader

### 6.9 Deferred Reader Retry

WaitSetInfo 最终安全 retire：
release historical reader holds
然后：
unlock WaitSet reconciliation
再调用：
retry_ready_deferred_readers()
只有：
accepting == false
refs == 0
state == DeleteDeferredByWaitSet
才可以 retry：
delete_datareader()

### 6.10 Publisher / Subscriber Internal State

Publisher：
NodeState
EndpointRuntimeState
EventSourceState
DataWriterInfo
LocalEndpointId
preallocated retirement / hidden-entity node
canonical binding is reached through DataWriterInfo.type
Subscriber：
NodeState
EndpointRuntimeState
EventSourceState
WaitableState
DataReaderInfo
ConditionInfo
LocalEndpointId
preallocated retirement / hidden-entity node
canonical binding is reached through DataReaderInfo.type

Endpoint TypeLease ownership 唯一规则：
- `DataWriterInfo::type` / `DataReaderInfo::type` 是 endpoint 唯一 TypeLease owner；
- `TopicEntry` 额外持有一个 independent Topic-owned TypeLease；
- endpoint facade/Impl、EventSource、WaitableState 不再重复持有第三个 TypeLease；
- public endpoint state 可以保存 caller `MessageType` metadata/facade，但该 metadata 不增加 TypeRegistry lease refcount，也不是 runtime hook authority。

Public endpoint state 可以保留 caller MessageType metadata，
但 runtime TypeSupport hook authority 必须来自 `TypeLease::canonical_binding()`；
不得直接回到 Factory caller descriptor 调用 serialize/deserialize/createData/deleteData。

<a id="fastdds-endpoint-create-transaction"></a>

#### 6.10.1 DataReader / DataWriter Creation Transaction

在调用 create_datareader()/create_datawriter() 前必须已经完成：
- TypeLease acquired；
- TopicLease acquired；
- backing allocated；
- listener/listener_state allocated and bound；
- WaitSet hold state initialized（Reader）；
- retirement/hidden-entity intrusive node preallocated；
- info.creation_status = NotStarted。

create_datareader()/create_datawriter() 返回有效 handle：
info DDS entity pointer = returned handle
info.creation_status = HandleKnown
然后才允许继续 endpoint Factory commit。

返回 nullptr：
只有[错误与 status matrix](#fastdds-lock-error-model)的 targeted Fast DDS 2.6.12 baseline 明确证明：
nullptr 表示该调用没有创建隐藏 target entity，
才可：
creation_status = NoSideEffect
rollback listener/TopicLease/TypeLease/backing
return DdsError。

如果该 no-side-effect evidence 未被冻结：
creation_status = SideEffectIndeterminate
DDS entity pointer 保持 nullptr
info.entity_status = Indeterminate
parent Subscriber/Publisher graph = MayContainHiddenEntity
把完整 backing 通过预分配 node adopt 到 OrphanedEndpointRegistry
return DdsError。

entered Fast DDS API call 后抛 C++ exception：
catch internally
creation_status = SideEffectIndeterminate
DDS entity pointer = nullptr
info.entity_status = Indeterminate
parent Subscriber/Publisher graph = MayContainHiddenEntity
adopt backing to OrphanedEndpointRegistry
record diagnostic
rethrow original exception。

该 orphan backing 必须继续 strong-own：
listener / listener_state
TopicLease
TypeLease / canonical binding
Reader WaitSet backing state（如适用）。

如果隐藏 DDS entity 实际存在，以上 ownership 保证其 Fast DDS listener / Topic / TypeSupport 引用不会悬空。

#### 6.10.2 Client / Server Partial Entity Creation

Client 固定：
response DataReader first
request DataWriter second。

Server 固定：
request DataReader
response DataWriter。

aggregate Factory 在第一个 Fast DDS child 成功、第二个 child error/exception 时仍未 public commit。
必须：
- 对已知 handle child 尝试 ordinary rollback delete；
- 对 delete failure 保留 KnownAlive/Indeterminate backing；
- 对第二个可能隐藏 child 按 [endpoint Fast DDS entity creation transaction](#fastdds-endpoint-create-transaction) 标记 container graph `MayContainHiddenEntity`；
- aggregate preallocated retirement node 接管所有尚未获得 safe-release evidence 的 backing；
- 不创建 public facade；
- 最后返回 primary Error 或 rethrow primary exception。

rollback cleanup failure/exception 不覆盖 primary result；
但必须改变 lifetime evidence 并保留 backing。

#### 6.10.3 Hidden Endpoint Runtime Consequence

一旦 DDS Subscriber/Publisher graph = MayContainHiddenEntity：
DMW 不再宣称该 container 的 known endpoint registry 完整。

已有 public endpoints 可以继续按其自身 handle/lifetime contract 工作；
但 hidden entity 可能影响 DDS matching/resource usage，
因此：
- fixed-size diagnostic 必须可观测；
- 后续同一 container 的 endpoint Factory 在完成正常 public/Context/parent 检查后返回 DdsError；
- 不继续制造新的 Fast DDS children，避免扩大未知 graph；
- final teardown 必须使用 container/Participant-level evidence barrier。

### 6.11 Publish

validate arguments

OperationGuard

endpoint Alive

TypeLease canonical binding capability == Healthy ?
    no -> DdsError

Fast DDS DataWriter::write()
ReturnCode 根据[错误映射](#fastdds-lock-error-model)处理；未预期 C++ exception 按 [binding contract](#fastdds-message-binding-contract) 原样传播。

#### 6.11.1 Bounded Filter Scan Contract

任何单次 public `take()` 都不得因其它线程持续写入 invalid/foreign/duplicate sample 而无限扫描。

Fast DDS V1 实现 在调用开始、进入第一个 `take_next_sample()` 前，在不持有 DMW mutex 时 snapshot：

`scan_budget = DataReader::get_unread_count(false)`

Fast DDS 2.6.12 baseline 必须验证该 API 返回当前 reader history 中未读 sample 数的有限 non-negative snapshot。
checked conversion 失败/负值/invariant violation -> `DdsError`；Fast DDS C++ exception 按 ordinary exception boundary 传播。

本次 public call 最多成功消费/过滤 `scan_budget` 个 DDS samples。
新并发到达的 sample 不增加本次 budget。

每消费一个 invalid/foreign/duplicate sample：`--remaining_scan_budget`。
当 remaining==0 且尚未取得符合 public 条件的 sample：

`return TakeStatus::NoData`

这不表示 reader history 绝对为空，只表示“本次调用开始时 snapshot 所覆盖的候选已经扫描完且未得到可返回 sample”。后续 public take 可取得新 snapshot 继续处理。

若 Fast DDS 在 remaining>0 时提前返回 NO_DATA，也立即返回 NoData。
该 bounded behavior 属于 public liveness/output contract，必须在同版本 `dmw.md` 具有 middleware-neutral 等价条款。

### 6.12 Subscriber Take

remaining = begin_take_scan_budget(reader)
while remaining > 0:

    TypeLease canonical binding capability == Healthy ?
        no -> DdsError

    create/reset TemporarySample

    Fast DDS take into temporary

    NO_DATA:
        return NoData

    Fast DDS error:
        return Error

    valid_data == false:
        discard temporary
        --remaining
        continue

    build MessageInfo temporary

    commit payload to caller

    assign MessageInfo

    return Taken
Metadata-only DDS sample：
consume/filter
--remaining
continue scanning
不能让一个 invalid sample 遮挡后续 valid sample。

while 因 remaining==0 退出：
return NoData。

### 6.13 MessageInfo Mapping

source_timestamp_ns
SampleInfo.source_timestamp
    ->
checked nanoseconds
不可用：
0
received_timestamp_ns
SampleInfo.reception_timestamp
    ->
checked nanoseconds
不可用：
0
不得使用：
steady_clock
填 public timestamp。
publisher_gid
优先：
sample_identity.writer_guid
若 unknown：
尝试 publication_handle
通过 Fast DDS identity converter
恢复 GUID
无法可靠转换：
Gid{}
publication_sequence_number
publication_sequence_from_fastdds(
    sample_identity.sequence_number)
    -> optional<uint64_t>
unknown sentinel：
nullopt
其它 high/low bit pattern：
按 [identity conversion helpers](#fastdds-identity-conversion) 的 bit-preserving `uint64_t` helper 返回
reception_sequence_number
Fast DDS V1 没有 DMW 所需独立 reception sequence：
nullopt
禁止：
publication sequence
冒充 reception sequence。

<a id="fastdds-service-runtime"></a>

## 7. Client / Server 与 Service Runtime

本章按照 endpoint composition、request/reply identity、Pending capacity、target discovery 和 `send_response()` transaction 的顺序定义 Service runtime。Client 和 Server 的两个 DDS endpoint 始终作为一个 aggregate resource 创建、回滚和销毁。

### 7.1 CompatibilityProfile 与 Service Wire Contract

两种 Fast DDS profile 都使用 DMW V1 的 `SampleIdentity` / `related_sample_identity` correlation model；区别主要在 resolved DDS naming/QoS/interoperability target。

`NativeDds`：
- request/response topic name 使用 [resolved DDS naming contract](#fastdds-dds-naming) 的 `dmw/rq/...` / `dmw/rr/...`；
- 仍使用 response Reader GUID + request sequence 形成 DMW `RequestId` 和 related identity；
- exact target/fallback、PendingRequestRegistry、response filtering 语义与本章一致；
- 100 ms response discovery wait 仍作为 Fast DDS correctness workaround 使用，不声称是 ROS compatibility requirement。

`Ros2FastDdsHumble`：
- 使用 `rq/<path>Request` / `rr/<path>Reply`；
- 同样使用本章 SampleIdentity correlation；
- 100 ms response discovery behavior 同时属于 frozen Humble interoperability behavior。

因此除“resolved name/QoS/golden interoperability expectations”外，本章 correlation state machine 默认适用于 **两个** CompatibilityProfile；任何只适用于 Humble 的差异必须在具体小节显式标注。

### 7.2 Client Composition

response DataReader
    ├── Reader listener
    ├── StatusCondition
    ├── DataReaderInfo
    └── LocalEndpointId

request DataWriter
    ├── Writer listener
    ├── DataWriterInfo
    └── LocalEndpointId
创建顺序：
response Reader
    ↓
request Writer
保证 request Writer 被 discovery 观察前：
response Reader 已经存在

### 7.3 Server Composition

request DataReader
    ├── Reader listener
    ├── StatusCondition
    ├── DataReaderInfo
    └── LocalEndpointId

response DataWriter
    ├── Writer listener
    ├── DataWriterInfo
    └── LocalEndpointId
创建顺序：
request Reader
    ↓
response Writer

### 7.4 Aggregate Retirement

Client：

```cpp
struct ClientRetirement
{
    WriterSideRetirement request;
    ReaderSideRetirement response;
};
```

Server：

```cpp
struct ServerRetirement
{
    ReaderSideRetirement request;
    WriterSideRetirement response;
};
```

两侧：
独立 CreationStatus/EntityStatus
独立 listener
独立 TopicLease
独立 TypeLease
第一侧 delete failure：
不阻止仍然安全的第二侧 cleanup

### 7.5 Client Send Request

Request write 前：
related_sample_identity.writer_guid
    =
response_reader_gid
Successful write：
RequestId.client_gid
    =
response_reader_gid

Fast DDS request sample sequence 必须为 known。

RequestId.sequence_number
    =
from_fastdds_sequence(request sample sequence)

转换算法唯一采用 4.2.2；
unknown / conversion invariant failure：
DdsError
且不发布新的 RequestId。

### 7.6 Server Take Request

读取：
sample_identity
related_sample_identity
如果：
related writer GUID known
则：
RequestId.client_gid
    =
related writer GUID
并保存 internal：
ExactResponseReaderGuid
否则：
RequestId.client_gid
    =
request sample writer GUID
并保存：
RequestWriterGuidFallback
Public RequestId 不改变。

### 7.7 ClientCorrelation

```cpp
enum class ClientCorrelationKind
{
    ExactResponseReaderGuid,
    RequestWriterGuidFallback
};
```

```cpp
struct ClientCorrelation
{
    ClientCorrelationKind kind;

    EndpointGuid correlation_guid;

    GuidPrefix participant_prefix;
};
```

### 7.8 PendingEntry

```cpp
enum class PendingState
{
    Pending,
    Responding
};

struct PendingEntry
{
    PendingState state{
        PendingState::Pending};

    RequestId request_id;
    ClientCorrelation correlation;

    // Immutable stable target identity.
    std::shared_ptr<const TargetReaderKey> target_key;
};

using PendingEntryHandle =
    std::shared_ptr<PendingEntry>;
```

`PendingRequestRegistry` map value 必须是 `PendingEntryHandle`（或具有相同 stable-identity/noexcept-copy 语义的 handle），而不是会在 map erase 后立即失效的裸 value reference。

原因：
`send_response()` 在第一阶段 public object-local lookup 后需要 unlock并执行可能分配的 preallocation；stable handle用于第二阶段无 ABA revalidation。

创建 PendingEntry：
- `take_request()` 在 sample 已消费后、不持 Pending mutex时完成 correlation/ParticipantObservation handle/TargetReaderKey 的所有 allocation；
- allocate `PendingEntryHandle` control block；
- lock Pending registry；
- 插入 map并把 `CapacityReservation` 原子转换为 Pending capacity；
- insertion/allocation failure 按 [Pending commit/output rollback](#fastdds-pending-output-rollback) 处理。

map erase 不立即销毁仍被并发 `send_response()` precheck snapshot strong-own 的 PendingEntry；这种 snapshot只用于 revalidation/Busy classification，不能在 map外修改 `state`。

### 7.9 Server Response Wire Identity

无论 correlation mode：
response.related_sample_identity.writer_guid
    =
request_id.client_gid

response.related_sample_identity.sequence_number
    =
to_fastdds_sequence(request_id.sequence_number)

转换算法唯一采用 4.2.2，
禁止直接依赖 signed shift/high-low cast。
保持 public Humble wire contract。

### 7.10 Exact Target Mode

若 correlation kind 为 `ExactResponseReaderGuid`：

```text
RequestId.client_gid = Client response DataReader GUID
build immutable TargetReaderKey {
    kind = ExactReader
    service = current ServiceKey
    participant_prefix = response Reader participant
    participant = stable ParticipantObservationEntry handle for that prefix
    exact_reader_guid = that response Reader GUID
}
PendingEntry.target_key = shared_ptr<const TargetReaderKey>
```

`send_response()` 不读取 `PendingEntry` 中的旧 observation；按 [target predicate](#fastdds-target-predicate) 以 key 查询 live exact entry：

```text
Matched
    -> unlock registry -> Fast DDS write

Removed / ParticipantRemoved
    -> success without write

NeverObserved / KnownUnmatched
    -> predicate wait
```

### 7.11 Fallback Target Mode

若 correlation kind 为 `RequestWriterGuidFallback`：

```text
RequestId.client_gid = Client request DataWriter GUID
build immutable TargetReaderKey {
    kind = ParticipantResponseSet
    service = current ServiceKey
    participant_prefix = request writer participant
    participant = stable ParticipantObservationEntry handle for that prefix
    exact_reader_guid = nullopt
}
PendingEntry.target_key = shared_ptr<const TargetReaderKey>
```

绝不能把 request Writer GUID 当成 response Reader GUID。

fallback target 是：

```text
same remote Participant
+ same service response resolved DDS topic/type
+ at least one actual matched response Reader
```

participant-response live observation `Matched` -> write response。

request Writer endpoint 自身被删除不足以证明 response Reader 消失；participant-level `KnownUnmatched/NeverObserved` 继续 wait。只有 `ParticipantRemoved` 才允许 success without write；否则到 absolute deadline -> `Timeout`。

<a id="fastdds-response-discovery"></a>

### 7.12 100 ms Response Discovery 与 Lost-wake-free Predicate

`deadline = steady_clock::now() + 100ms`

absolute deadline只计算一次。

EphemeralInterruptibleWait registration与shutdown linearization互斥：runtime mutex下确认Active，再在ChildRegistry注册预分配child，然后依次unlock。shutdown commits first则注册失败；child commits first则request-all一定看见它。

Ephemeral shutdown request atomic publish cancellation，并在不持 `ChildRegistry` 时调用 `signal_target_dependency_change()`；operation 退出 quiescent 后按 [child acknowledgement discipline](#fastdds-child-ack) 发布 ack。

等待只使用5.20 Target mutex/CV protocol：

```text
Loop:
    Context/ephemeral shutdown? -> ContextShutdown
    participant capability Degraded? -> DdsError
    participant Removed? -> terminal no-write success

    lock Target mutex
    // mandatory recheck while holding Target mutex
    recheck shutdown
    recheck participant capability/lifecycle
    check Target capability/state

    Matched -> unlock -> Fast DDS write
    exact Removed -> unlock -> terminal no-write success
    deadline expired -> unlock -> Timeout

    cv.wait_until(lock, deadline, full predicate from 5.20)
```

进入任何实际wait前必须在已经持有Target mutex时重读 participant capability/lifecycle与shutdown predicate；只在拿Target mutex前做atomic precheck是不充分的。5.18 dependency-generation handshake + 5.20 under-lock recheck共同构成lost-wake evidence。

### 7.13 PendingRequestRegistry

```cpp
enum class PendingRegistryState
{
    Active,
    Shutdown
};

struct PendingRequestRegistryState
{
    std::mutex mutex;
    PendingRegistryState state{
        PendingRegistryState::Active};

    PendingRequestTable<RequestId, PendingEntryHandle> entries;

    std::size_t pending_count{0};
    std::size_t responding_count{0};
    std::size_t reservations{0};
    std::size_t max_pending_requests{0};
};
```

容量：

```text
Pending
+
Responding
+
reservations
<
max_pending_requests
```

否则：
`ResourceExhausted`
并且不得 Fast DDS take request。

Pending/Responding counters必须与 map中 same-state entry数量一致；所有转换在 Pending mutex 下提交。

Stable-handle invariant：
- map lookup成功返回 `PendingEntryHandle`；
- handle copy noexcept；
- map erase后其它线程持有的 snapshot handle只延长 entry object lifetime，不延长其 registry membership；
- map membership是 authoritative pending ownership；
- snapshot handle不能自行把已 erase entry重新插回 map。

### 7.14 CapacityReservation

使用 move-only RAII：
reserve
    ↓
Fast DDS take
失败/NoData/duplicate：
automatic release
成功新 request：
reservation
    ->
PendingEntry
不使用容易下溢的手工 ++/--。

### 7.15 Server take_request()：Capacity-before-scan Transaction

Server capacity 是 object-local resource state，且 contract 规定 capacity full 时不得执行 Fast DDS request take。因此固定错误/操作顺序为：

```text
validate public arguments
OperationGuard
validate parent/object-local state

reserve one Pending capacity
    failure -> ResourceExhausted
               no unread-count query
               no Fast DDS take

snapshot call-start unread scan budget
    exception/error -> reservation RAII release; propagate/map normally
    zero            -> reservation RAII release; NoData

enter bounded scan
```

这意味着 `capacity full + unread_count == 0` 的唯一结果是 `ResourceExhausted`，不是 `NoData`。

每一次 Fast DDS request take 必须由一个 live `CapacityReservation` 覆盖：

```text
remaining = call-start scan budget
reservation already held

while remaining > 0:
    Fast DDS take into TemporarySample

    NO_DATA/error:
        reservation releases
        return corresponding result

    invalid sample:
        --remaining
        reservation releases
        if remaining == 0 -> NoData
        reserve again; failure -> ResourceExhausted
        continue

    duplicate request:
        --remaining
        reservation releases
        if remaining == 0 -> NoData
        reserve again; failure -> ResourceExhausted
        continue

    new valid request:
        no Pending/Target mutex held
        obtain stable ParticipantObservationEntry handle
            participant capability Degraded -> reservation release; DdsError
            allocation bad_alloc while first materializing participant entry -> reservation release; rethrow
        build RequestId/correlation + immutable `std::shared_ptr<const TargetReaderKey>`
        allocate PendingEntryHandle
        all string/key/control-block allocation completes before Pending map insertion
        insert PendingEntryHandle by consuming current reservation
        continue to output commit transaction in 7.16
```

Duplicate/invalid 后必须重新 reserve；不能让一次 reservation 覆盖多个 DDS samples。并发期间 capacity 可能在重新 reserve 前被其它线程占满，此时返回 `ResourceExhausted` 是合法且唯一的结果。

<a id="fastdds-pending-output-rollback"></a>

### 7.16 Server take_request() Pending Commit / Output Rollback

在 caller output commit 前必须先成功建立 PendingEntry；否则无法保证 request 已消费后 response correlation/capacity bookkeeping 不丢失。

`PendingEntry` insertion 使用当前 `CapacityReservation`。为避免“map 已 commit，但 rollback guard 构造抛异常”的 throwing gap，rollback guard 必须 **在 map insertion 前以 disarmed 状态预构造**：

```cpp
PendingEntryRollbackGuard rollback{
    pending_registry,
    request_id,
    pending_entry_handle,
    DisarmedTag{}};
```

该构造必须 `noexcept` / no-allocation；guard 只保存 stable pointer/RequestId/`PendingEntryHandle`/armed bit。

随后：

```text
lock PendingRequestRegistry
insert map[request_id] = pending_entry_handle
    duplicate-key race -> leave reservation owned; unlock; classify/filter duplicate and continue bounded scan
    allocation/other exception -> unlock via RAII; reservation releases; guard remains disarmed; rethrow

convert CapacityReservation -> Pending count
rollback.arm()          // noexcept; no allocation
unlock PendingRequestRegistry
```

只有 map insertion + reservation conversion + `rollback.arm()` 全部完成后，PendingEntry 才算 bookkeeping commit。`arm()` 与 counter conversion 不允许抛异常。

若 insertion 抛 `std::bad_alloc` 或其它允许传播的 C++ exception：

```text
reservation RAII releases
rollback remains disarmed
no PendingEntry remains
caller request/RequestId unchanged
DDS sample may already be consumed
exception propagates unchanged
```

rollback guard 在任何未完成 caller output commit 的退出路径中只 erase `map[request_id] == pending_entry_handle` 的 newly inserted entry，并正确偿还 pending capacity；若 map identity 不匹配则记录 invariant diagnostic，不删除其它 entry。destructor `noexcept`，cleanup failure按 conservative internal invariant/diagnostic处理，不覆盖原始 exception/result。

然后执行 Temporary -> caller payload commit：

```text
deserialize returns false:
    rollback guard erases PendingEntry
    -> DdsError

deserialize throws std::bad_alloc:
    rollback guard erases PendingEntry
    -> rethrow std::bad_alloc

deserialize throws other C++ exception:
    rollback guard erases PendingEntry
    -> rethrow original exception

payload commit success:
    assign caller RequestId / metadata
    dismiss rollback guard
    -> Taken
```

post-consumption failure 时 sample 可以已从 DDS history 消失；caller object 按 public basic guarantee保持 valid/destructible，字段可 unspecified。exception channel 与 output guarantee 是独立 contract。

### 7.17 Client take_response()

remaining = begin_take_scan_budget(response_reader)
while remaining > 0:

take into TemporarySample

NO_DATA
    -> NoData

invalid_data
    -> discard, --remaining, continue

foreign related identity
    -> discard, --remaining, continue

legal response
    -> canonicalize RequestId
    -> commit response
    -> return Taken
remaining == 0：
    -> NoData
Foreign response：
不得修改 caller outputs

### 7.18 Client RequestId Canonicalization

Wire related GUID 可为：
response_reader_gid
request_writer_gid
Public 返回统一：
RequestId.client_gid
    =
response_reader_gid
Sequence：
related sequence 必须 known，
并通过 4.2.2 from_fastdds_sequence() 转为 public int64_t。
unknown related sequence：
consume/filter response
--remaining
continue scanning
不得返回伪造 RequestId。

### 7.19 Server `send_response()` Full Transaction 与 ResponseRollbackGuard

本节是 `send_response()` 的唯一 implementation order。
7.10～7.12 定义 target semantics；本节定义 public error priority、two-phase claim、locks、rollback 与 exception ordering。

#### 7.19.1 Phase A：object-local lookup 必须先于后续 allocation

固定顺序：

```text
validate public arguments
    -> OperationGuard
    -> validate Server parent/object-local lifecycle

lock PendingRequestRegistry          // rank 16
verify registry Active
lookup RequestId

absent:
    unlock
    -> NotFound

entry.state == Responding:
    unlock
    -> Busy

entry.state == Pending:
    snapshot PendingEntryHandle candidate   // noexcept shared_ptr copy
unlock
```

因此：
- unknown RequestId + injected later preallocation OOM -> `NotFound`；
- already Responding RequestId + injected later preallocation OOM -> `Busy`；
- NotFound/Busy 作为 object-local errors不会被后续 heap allocation抢占。

第一阶段只 snapshot stable handle，不修改 `PendingState`。

#### 7.19.2 Phase B：preallocate before claim

只有第一阶段确认 candidate Pending 后才允许：

```text
preallocate EphemeralInterruptibleWait backing + intrusive ChildRegistry link
construct disarmed ResponseRollbackGuard
```

`std::bad_alloc` / other allowed C++ exception：
- Pending map/state未修改；
- original exception原样传播。

`ResponseRollbackGuard` 构造必须 no-allocation/noexcept，只保存：
- non-owning stable `PendingRequestRegistryState*`；
- `RequestId`；
- `PendingEntryHandle candidate`；
- armed flag。

#### 7.19.3 Phase C：revalidate same stable entry and claim

```text
lock PendingRequestRegistry          // rank 16

if registry Shutdown:
    unlock
    -> ContextShutdown

lookup RequestId

if map contains same candidate handle:
    if candidate.state == Responding:
        unlock
        -> Busy

    assert candidate.state == Pending
    candidate.state = Responding
    pending_count--
    responding_count++
    rollback_guard.arm()             // noexcept
    unlock
    -> claim success

else:
    // First lookup once proved this RequestId existed.
    // Same RequestId entry replacement is forbidden by registry invariant.
    if candidate.state == Responding:
        unlock
        -> Busy   // another sender claimed/completed while this caller preallocated
    else:
        registry invariant diagnostic
        unlock
        -> DdsError
```

两个并发 `send_response()`：
- 两者第一阶段都可能 snapshot同一个 Pending handle；
- 只有一个 Phase C 可以 `Pending -> Responding`；
- 另一个看到 same handle `Responding` 或 terminally erased-but-still-Responding snapshot -> `Busy`。

在 `Pending -> Responding` 后、rollback guard arm 前不得执行任何可能分配/抛异常的 operation。

#### 7.19.4 Phase D：Ephemeral child registration

claim 成功后，预分配 `EphemeralInterruptibleWait` 按 [response discovery protocol](#fastdds-response-discovery) 与 shutdown linearization 互斥注册。

ChildRegistry insertion使用预建 intrusive/link storage，不分配。

registration：
- success -> checked commit next `child_registration_id` + intrusive link，并安装
  `EphemeralWaitRegistrationGuard`；
- child ID exhausted -> registry保持不变，rollback Responding，返回 `ResourceExhausted`；
- Context shutdown wins -> rollback Responding；返回 `ContextShutdown`；
- unexpected C++ exception -> rollback bookkeeping first，然后 rethrow original exception。

Rollback不覆盖 primary Error/exception。

#### 7.19.5 Phase E：Predicate wait and lock separation

注册成功后：

```text
no PendingRegistry mutex held
no ChildRegistry mutex held
no ParticipantObservationRegistry mutex held
TargetReaderObservationRegistry only through 5.20 predicate protocol
```

`PendingRegistry(rank 16)` 与 `TargetReader(rank 8)` **永不同时持有**。

predicate：
- participant authority/Target capability Degraded -> rollback -> `DdsError`；
- `Matched` -> release Target mutex -> Fast DDS write；
- exact target `Removed` -> terminal no-write success；
- participant `Removed` -> terminal no-write success；
- timeout -> rollback -> `Timeout`；
- shutdown -> rollback -> `ContextShutdown`；
- wait exception -> rollback then rethrow。

#### 7.19.6 Phase F：Fast DDS write

Fast DDS write时不持有 Pending / Target / Participant / ChildRegistry / Context runtime mutex。

```text
write OK
    -> terminal success commit

write ReturnCode failure
    -> unregister ephemeral child
    -> rollback Responding according to registry state
    -> mapped Error

write throws:
    -> unregister/settle ephemeral child
    -> rollback bookkeeping
    -> rethrow original exception
```

#### 7.19.7 Terminal success commit

Fast DDS write success和 terminal no-write success使用 **同一个** Pending commit：

```text
settle/unregister EphemeralInterruptibleWait

lock PendingRegistry
if registry Active:
    require map[RequestId] == candidate
    require candidate.state == Responding
    erase map entry
    responding_count--
else if registry Shutdown:
    map may already be cleared by shutdown finalize
    do not reinsert
unlock

rollback_guard.disarm()
return success
```

对 exact target：
- Reader `Removed` 或 participant Removed -> terminal no-write success。

对 fallback：
- 只有 participant Removed -> terminal no-write success。

terminal success之后 candidate handle可能仍被其它 precheck thread暂时 strong-own；它仍保持 `Responding`，使那些 second-phase caller稳定得到 `Busy`，但它已不属于 registry，绝不能再次发送 response。

#### 7.19.8 Rollback semantics

`ResponseRollbackGuard::~ResponseRollbackGuard() noexcept`：

```text
if !armed:
    return

lock PendingRegistry

if registry Active
AND map contains same candidate handle
AND candidate.state == Responding:
    candidate.state = Pending
    responding_count--
    pending_count++

else if registry Shutdown:
    if map contains same candidate handle:
        erase
        adjust responding count exactly once
    // never reinsert after shutdown

else:
    fixed invariant diagnostic
    conservative no-duplicate mutation

unlock
```

Rollback guard不持有 Target/Participant/ChildRegistry lock。

任何 write/wait/registration C++ exception：
**先完成 rollback/child bookkeeping，再传播原异常。**

#### 7.19.9 Shutdown race

如果 shutdown在 Phase A 与 Phase C 之间提交：
- Phase C观察 Pending registry Shutdown -> `ContextShutdown`；
- preallocated ephemeral backing由 local RAII释放；
- candidate仍由 shutdown protocol/finalize拥有或已清除；
- caller不 claim Responding。

如果 shutdown在 Phase C 后提交：
- Responding已计入 capacity；
- request-all看见已注册/即将按 commit protocol注册的 ephemeral child；
- operation最终 rollback时 registry Shutdown -> erase/no-reinsert；
- `finalize_shutdown()` 不得与 live OperationGuard completion产生 counter underflow。

### 7.20 Pending Shutdown

Phase A：
state = Shutdown

reject:
new reservation
new Pending -> Responding

retain existing counters/state
Context operation drain 完成后：
assert reservations == 0

clear Pending
clear Responding
避免：
reservation--
after shutdown force reset
造成下溢。

<a id="fastdds-waitset"></a>

## 8. WaitSet、GuardCondition 与 Event

本章统一定义 logical registration、WaitSetInfo、control wake、Guard generation 和 Event cursor。Logical topology/readiness 是 authority；Fast DDS Condition 和 GuardCondition 只提供 Fast DDS WaitSet wait integration 与 notification。

### 8.1 WaitableState

```cpp
enum class WaitableCloseReason
{
    None,
    SelfClosing,
    ParentDestroyed
};

struct WaitableState
{
    std::shared_ptr<ContextState>
        context;

    std::mutex mutex;

    WaitableCloseReason close_reason{
        WaitableCloseReason::None};

    std::weak_ptr<RegistrationState>
        registration;

    WaitableAdapter adapter;
};
```

`close_reason != None` 等价于旧的 generic `closing=true`，但保留 public error-priority 所需的原因：
- Event parent endpoint destruction -> `ParentDestroyed`；
- waitable 自身 public destruction/close -> `SelfClosing`；
- Context shutdown不写该字段，由更高优先级 `OperationGuard`处理。

Parent endpoint destruction在启动 Event auto-detach 前，必须先在 Event `WaitableState.mutex` 下提交 `ParentDestroyed`，因此后续 `WaitSet::add()` 无需在持 topology/waitable高 rank mutex时反向取得 parent Endpoint mutex。

#### 8.1.1 Waitable Registration Ownership

一个 Waitable 最多属于一个 WaitSet registration。

`WaitableState::registration` 是 `weak_ptr`；`RegistrationState` 由 WaitSet logical registration table 或 active operation 的 strong ownership 保活。

跨对象 lock order 固定为：

```text
WaitSet topology
    ->
Waitable local state
```

任何路径都禁止：

```text
Waitable local state
    ->
WaitSet topology
```

因此 endpoint/Event/Guard destructor 不能持有 Waitable mutex 调用 WaitSet detach。

#### 8.1.2 Bidirectional Registration Detach Transaction

Waitable-side close 适用于 Subscriber、Client、Server、Event 和 GuardCondition destructor，以及 parent endpoint destruction 导致的 Event auto-detach。WaitSet-side close 仅适用于 WaitSet destructor。

两侧共享同一个 `RegistrationState` CAS protocol，但 WaitSet 本身不是 `WaitableState`，不得假设 WaitSet 具有 `WaitableState::registration`。

Waitable-side close 使用以下事务：

```text
lock WaitableState.mutex

if close_reason != None:
    snapshot registration if needed
else:
    close_reason = SelfClosing
    reg = registration.lock()
    registration.reset()

unlock WaitableState.mutex

之后才允许调用：
auto_detach_registration(reg)

因此 destructor 从不形成：
waitable -> topology
lock inversion。

auto_detach_registration(reg)：

if !reg:
    return

ws = reg->waitset.lock()

if !ws:
    // WaitSetState lifetime invariant:
    // WaitSetState 的最后一个 strong owner 释放前，
    // 必须已经把其全部 RegistrationState 推进到 Detached。
    assert reg.phase == Detached
    return

lock ws.topology_mutex

phase = reg.phase

if phase == Attached:
    CAS Attached -> Detaching
    remove reg from desired logical topology
    checked advance topology_generation
    if generation exhausted:
        ws.wait_set_status = Poisoned
    snapshot need_wake = true

if phase == Detaching:
    logical topology must already exclude reg

if phase == Detached:
    unlock
    return

unlock ws.topology_mutex

if need_wake:
    best-effort control wake
```

然后由当前 active wait 的 reconciliation 或 auto-detach caller 竞争 reconciliation ownership，完成 Fast DDS detach。

`Detaching -> Detached` 必须同时满足：

- registration 已不在 current/future desired topology；
- `registration.active_wait_count == 0`；
- 没有 current published generation 仍把该 registration 当作 active binding。

历史 Unresolved WaitSetInfo 可以继续持有 `ConditionInfo` 和 Reader/Guard WaitSet hold，但不阻止 public `RegistrationPhase` 进入 `Detached`；Info lifetime 由 retirement evidence 单独控制。

完成时提交：

```text
phase = Detached
notify drain_cv
```

WaitSet destructor 的固定顺序为：

1. `wait_set_status = Closing`；
2. 在 `topology_mutex` 下 snapshot 全部 registration；
3. 将 `Attached` registration 统一推进到 `Detaching` 并从 desired topology 删除；
4. checked advance `topology_generation` 一次，或提交一个 batch generation stamp；
5. unlock topology；
6. 逐个锁 `WaitableState`，仅当其 weak registration 仍指向同一 `RegistrationState` 时才 reset；
7. 不持有 Waitable mutex 进入 reconciliation；
8. 完成所有 logical detach；
9. active Fast DDS WaitSet wait 退出并完成 shutdown ack；
10. unresolved historical WaitSetInfo 进入 `RetiredWaitSetRegistry`。

Waitable 与 WaitSet 同时析构时，双方都只能通过 CAS claim `Attached -> Detaching`。只有一个 caller 成为 logical unlink owner；另一个 caller 观察 `Detaching/Detached` 并协助或等待，不得重复删除 registration、递增 generation 或释放 hold。

Control wake failure 不得回滚 `Detaching`。只要 vendor liveness assumption 成立，active Fast DDS WaitSet wait 最迟通过 100 ms bounded slice 退出并重新 reconciliation，因此 destructor 不依赖单次 Fast DDS GuardCondition trigger 成功。

### 8.2 Same-context Rule

WaitSet 只能注册属于同一 Context 的 waitable。跨 Context 注册返回 `InvalidArgument`，且不得创建 `RegistrationState`、消耗 `registration_id` 或修改 topology。该规则来自同版本 `dmw.md`，Fast DDS 实现不在本文新增公共契约。

### 8.3 WaitableMechanism

```cpp
enum class WaitableMechanism
{
    DdsCondition,
    LogicalReadiness,
    Hybrid
};
```

WaitableMode
SubscriberCondition
ClientCondition
ServerHybrid
EventLogicalReadiness
GuardConditionHybrid

### 8.4 `WaitSetStatus`

```cpp
enum class WaitSetStatus
{
    Healthy,
    Poisoned,
    Closing
};
```

```cpp
enum class ControlGuardReplacementState
{
    Idle,
    Required,
    Building,
    Published
};
```

Poisoned：
对该 WaitSet 永久
不恢复 Healthy。

### 8.5 WaitSetState

```cpp
struct WaitSetState
{
    std::shared_ptr<ContextState>
        context;

    std::mutex topology_mutex;

    std::mutex reconciliation_mutex;

    std::atomic<std::uint64_t>
        topology_generation{0};

    std::atomic<bool>
        active_wait{false};

    std::atomic<WaitSetStatus>
        wait_set_status{
            WaitSetStatus::Healthy};

    std::uint64_t wait_set_id{0};

    // topology_mutex-protected per-WaitSet token allocator.
    std::uint64_t next_registration_id{1};
    bool registration_id_exhausted{false};

    RegistrationTable registrations;

    std::unique_ptr<
        WaitSetInfo>
        current_wait_set;

    std::shared_ptr<
        ControlGuardInfo>
        control_guard;

    std::atomic<ControlGuardReplacementState>
        control_guard_replacement{
            ControlGuardReplacementState::Idle};

    std::shared_ptr<
        InternalChildState>
        shutdown_child;
};
```

### 8.6 RegistrationState

```cpp
enum class RegistrationPhase
{
    Attached,
    Detaching,
    Detached
};
```

```cpp
struct RegistrationState
{
    std::atomic<RegistrationPhase>
        phase{
            RegistrationPhase::Attached};

    // backing for no-allocation logical table insertion after registration_id commit.
    IntrusiveRegistrationNode topology_node;

    WaitToken token;

    std::weak_ptr<WaitableState>
        waitable;

    std::weak_ptr<WaitSetState>
        waitset;

    std::atomic<std::uint32_t>
        active_wait_count{0};

    std::mutex drain_mutex;
    std::condition_variable drain_cv;
};
```

active_wait_count 的唯一语义：
“当前 active `WaitSet::wait()` Fast DDS WaitSet wait slice是否仍可能根据 WaitSetInfo attachment snapshot把该 RegistrationState解释为 ready token”。

`drain_cv` predicate authority包括 `active_wait_count` 和 `RegistrationPhase` terminal publication。虽然字段可 atomic fast-read，**任何用于唤醒 drain waiter的transition必须在 `drain_mutex` 下发布**。

V1单个 RegistrationState：

`active_wait_count ∈ {0,1}`

进入Fast DDS WaitSet wait前 CAS `0 -> 1`。Fast DDS WaitSet wait返回并完成 frozen mapping/ready snapshot/token revalidation后：

```text
release_wait_reference(reg) noexcept:
    try:
        unique_lock reg.drain_mutex
        require active_wait_count == 1
        active_wait_count = 0
        unlock
        drain_cv.notify_all()
    catch (...):
        fixed diagnostic
        retain registration and corresponding ConditionInfo/endpoint Info conservatively
```

`Detaching -> Detached`：

```text
unique_lock drain_mutex
verify active_wait_count == 0
phase = Detached
unlock
drain_cv.notify_all()
```

waiter必须先释放 topology/reconciliation/waitable mutex，再持 `drain_mutex` 使用 predicate wait。禁止 `active_wait_count.store(0); notify_all()` 这种裸 atomic notify。

remove/auto-detach success要求refs==0，只提供 Fast DDS Condition interpretation lifetime safety；允许 wait先形成Ready snapshot、再publish refs=0、remove返回、最后wait返回旧snapshot。WaitReferenceGuard覆盖success/timeout/error/exception所有退出路径。historical unresolved WaitSetInfo不保持active_wait_count，而由 ConditionAttachment+WaitSet hold保活。

WaitSetState lifetime invariant：
WaitSetState 最后一个 strong owner 释放以前，
所有属于它的 RegistrationState.phase 必须已经 == Detached。
因此 waitable auto-detach 中 `reg->waitset.lock()` 失败
可以断言 registration 已 Detached。

### 8.7 add() Semantics 与跨对象错误优先级

固定 public ordering：

```text
1. validate ordinary public arguments
2. validate same Context
3. OperationGuard / Context state
4. waitable parent/close state
5. waitable registration state
6. WaitSet local logical state (Poisoned / topology or token exhaustion)
7. Fast DDS reconciliation state
```

因此不得使用 `WaitSet wait_set_status == Poisoned` 的 early fast-path抢占更高优先级的 `ParentDestroyed` / waitable-local registration error。

具体 transaction：

```text
validate ordinary public arguments

validate same Context using stable ContextState identity
    no:
        -> InvalidArgument
        -> no RegistrationState
        -> no registration_id consumption
        -> no topology / Waitable mutation

OperationGuard
```

随后进入 topology/waitable ordered critical section：

```text
lock WaitSet.topology_mutex          // rank 12
lock WaitableState.mutex             // rank 15

switch waitable.close_reason:
    ParentDestroyed:
        unlock both
        -> ParentDestroyed

    SelfClosing:
        unlock both
        -> object-local closed error according to dmw.md

    None:
        continue

if waitable.registration still live:
    unlock both
    -> AlreadyRegistered
```

只有上述更高优先级 state通过后，才检查 WaitSet local：

```text
wait_set_status != Healthy:
    unlock both
    -> DdsError

topology_generation == UINT64_MAX:
    wait_set_status = Poisoned
    unlock both
    -> ResourceExhausted
    -> no registration_id consumption
    -> no topology change

registration_id allocator exhausted:
    unlock both
    -> ResourceExhausted
    -> no topology change
```

然后固定为：

```text
verify registration_id allocator has an available token, but do not consume it

allocate/construct RegistrationState backing with provisional token=0
preallocate RegistrationTable insertion backing/node
    bad_alloc -> unlock via RAII -> rethrow
    registration_id not consumed

allocate/commit registration ID under topology mutex
assign token to prebuilt RegistrationState/table node using noexcept field assignment
insert preallocated registration node into WaitSet logical table using noexcept/no-allocation operation
bind WaitableState.registration to the same RegistrationState using noexcept weak/shared handle assignment
checked advance topology_generation already proven representable
unlock Waitable
unlock topology
best-effort control wake
```

`RegistrationTable` 的 V1 implementation contract 是：**final token allocation 以后，logical table insertion/bidirectional bind 不得再分配或抛异常**。实现可以使用嵌入 `RegistrationState` 的 intrusive table node，或其它在 token commit 前已经预分配完成的等价结构。

因为 topology mutex从availability check保持到 token/table commit，不存在其它 add线程偷走该 token 的 race。这样既满足 no-reuse，又满足“logical commit 前 allocation failure 不消耗 registration ID”。

Success不要求 Fast DDS Condition 已经 attach。
后续 Fast DDS reconciliation失败：
`wait() -> DdsError`，registration保留并可 `remove()`。

#### 8.7.1 Race note

Event parent destruction与 add并发：
- parent destruction先提交 `WaitableCloseReason::ParentDestroyed` -> add返回 ParentDestroyed；
- add先在 topology+waitable critical section commit registration -> parent destruction通过 bidirectional detach protocol处理该 registration；
- 不存在 `Poisoned` 抢先隐藏 ParentDestroyed 的路径。

WaitSet Poisoned与 already-registered waitable：
`AlreadyRegistered` 作为 waitable registration-local error先于 WaitSet local `DdsError`。

### 8.8 Registration ID

每个 WaitSet 的 allocator state 直接位于 `WaitSetState`，并仅在 `topology_mutex` 下访问：

```cpp
std::uint64_t next_registration_id{1};
bool registration_id_exhausted{false};
```

分配算法：

```text
if registration_id_exhausted:
    -> ResourceExhausted

id = next_registration_id

if id == UINT64_MAX:
    registration_id_exhausted = true
else:
    next_registration_id = id + 1

return id
```

规则：
- 0 永远 invalid；
- 1 ... UINT64_MAX 每个值最多分配一次；
- `UINT64_MAX` 本身允许作为最后一个成功分配 ID；
- 下一次 add 永久 `ResourceExhausted`；
- Factory/add 在真正 logical commit 前若因其它原因失败，不消耗 registration ID；因此 ID allocation 必须位于所有可预检失败之后、RegistrationState commit transaction 内；
- allocator 永不 wrap、永不复用。

### 8.9 remove()

validate token

OperationGuard

lock topology

lookup token -> RegistrationState

if Attached:
    CAS Attached -> Detaching
    remove from desired logical topology
    checked advance topology_generation
    on exhaustion:
        wait_set_status = Poisoned

snapshot waitable weak_ptr

unlock topology

if waitable exists:
    lock WaitableState
    if WaitableState.registration still refers to this RegistrationState:
        WaitableState.registration.reset()
    unlock WaitableState

best-effort wake

如果当前没有 active wait：
remove caller 自己竞争 reconciliation ownership，
不能依赖另一个线程未来调用 wait()。
成功返回要求：
phase == Detached

active_wait_count == 0

current/future generation
不会重新 attach
历史 unresolved WaitSetInfo：
允许继续持有对应 endpoint Info
因此：
public logical detach
和：
historical Info lifetime
是两个概念。

### 8.10 remove 与 Active Wait

若 active wait 因 Fast DDS error 返回，而 registration 仍：
Detaching
remove thread 观察：
active_wait == false
后自己竞争：
reconciliation ownership
完成 detach。
不能出现：
wait 返回 DdsError
+
remove 永久阻塞

### 8.11 topology_generation

std::atomic<std::uint64_t>
    topology_generation;
0 为初始 generation。
1 ... UINT64_MAX 每个值最多用于一次成功 logical topology version。
禁止 unsigned wrap。

普通 logical topology mutation：
lock topology

current = topology_generation.load()

if current == UINT64_MAX:
    public add():
        不修改 topology
        不消耗 registration_id
        wait_set_status = Poisoned
        return ResourceExhausted

    remove()/destruction/internal capacity transition:
        允许完成必要的 logical detach/state mutation
        wait_set_status = Poisoned
        标记 current WaitSetInfo 必须 retire
        不再发布新的 WaitSetInfo

else:
    modify topology
    topology_generation.store(current + 1)

unlock

Reconciliation：
atomic load
因此禁止：
普通 uint64_t
跨 topology/reconciliation locks 读取
也禁止：
fetch_add 导致 generation 从 UINT64_MAX wrap 到 0。
Poisoned 后 remove 仍必须可以完成 logical detach/retirement；
这是 teardown exception，不允许借此重新恢复 Healthy。

### 8.12 WaitSetInfo

`WaitSetInfo` 表示一个实际 Fast DDS `WaitSet` 及其完整 Condition attachment 集合。WaitSet topology 发生变化时，reconciliation 构造新的 `WaitSetInfo`；旧 `WaitSetInfo` 按 `WaitSetPhase` 进入 retirement。它是 DDS object 的生命周期记录，不是 logical topology authority；topology authority 仍属于 `WaitSetState`。

```cpp
enum class WaitSetPhase
{
    Building,
    Current,
    Retiring,
    Unresolved,
    Retired
};
```

```cpp
enum class AttachmentStatus
{
    NotAttached,
    Attached,
    Indeterminate
};
```

```cpp
struct ConditionAttachment
{
    std::shared_ptr<
        ConditionInfo>
        condition_info;

    std::shared_ptr<
        RegistrationState>
        registration;

    AttachmentStatus status{
        AttachmentStatus::NotAttached};

    bool waitset_hold_acquired{false};
};
```

### 8.13 `WaitSetInfo` Preallocation

这是硬性要求。
在：
第一个 Fast DDS attach_condition()
之前完成：
allocate WaitSetInfo

allocate Fast DDS WaitSet

snapshot all desired registrations

attachments.reserve(all conditions)

condition lookup table reserve/build

active-condition scratch reserve

acquire all needed
Reader/Guard WaitSet holds
第一个 Fast DDS attach 后：
这样发生 std::bad_alloc 时不会留下：
Fast DDS Condition 已 attach
但 DMW 没有 ownership record

### 8.14 Condition Lookup

`WaitSetInfo` 内 Condition→Registration mapping 必须：
在 first attach 前完全构造
推荐：
preallocated vector<ConditionAttachment>
或其它：
等待期间不分配
的数据结构。
Fast DDS WaitSet wait 返回 Condition pointer 后：
只查询 frozen `WaitSetInfo` attachment snapshot
不得查询已经变化的 logical registration container 来解释旧 Fast DDS wake。

### 8.15 Condition Attach Transaction

每个 attachment：
slot already exists

acquire WaitSet hold

call attach_condition()
Success：
status = Attached
Fast DDS error/exception：
status = Indeterminate
然后：
rollback/retire building `WaitSetInfo`
Indeterminate 的意义：
Fast DDS API call 已经发生，DMW 无法证明 Condition 没有被 attach；
因此该 attachment 必须视为“可能仍被 Fast DDS WaitSet 引用”；
对应 ConditionInfo 与 Reader/Guard WaitSet hold 必须继续保留；
retirement 必须尝试 `detach_condition()` 获取 `NotAttached` status；
在得到明确 status 前，禁止释放 `ConditionInfo` 或允许对应 Reader 被删除。

### 8.16 Topology Recheck

全部 attach 完成后同时检查：
atomic topology_generation
==
snapshot_generation
AND
atomic wait_set_status == Healthy ?
No：
new `WaitSetInfo` 不 publish

retire it

如果 wait_set_status == Healthy：
    retry
否则：
    return DdsError / teardown path
Yes：
publish new current `WaitSetInfo`

retire old `WaitSetInfo`

### 8.17 Reconciliation Lock

topology_mutex
与：
reconciliation_mutex
永不同时持有。
reconciliation_mutex 是单个 WaitSet DDS entity graph 的 serialization mutex；
它保护：
current_wait_set
WaitSetInfo publication/retirement
Fast DDS WaitSet attach/detach/wait 的互斥访问。

流程：
lock topology
build logical snapshot
G = topology_generation
unlock topology

lock reconciliation

if topology_generation != G:
    unlock
    retry

perform WaitSetInfo reconciliation

unlock

这是第 10.4 Fast DDS API call unlock rule 的明确且唯一一类 WaitSet 例外：
attach_condition()
detach_condition()
Fast DDS WaitSet::wait()
wait/reconciliation path 的 private control GuardCondition set_trigger_value(false/true)
允许在持有该 WaitSet reconciliation_mutex 时调用，
因为该 mutex 本身就是 Fast DDS WaitSet 的 exclusive-access authority。
调用这些 Fast DDS WaitSet API 时不得同时持有其它 DMW mutex，尤其不得持有 topology_mutex、RegistrationState、Waitable/Event/Guard 或 Registry mutex。
其它 Fast DDS create/delete/write/take/register/topic operation 仍遵循 [Fast DDS API call unlock rule](#fastdds-api-call-rule)。

### 8.18 `WaitSetInfo` Retirement

对：
Attached
Indeterminate
Condition：
detach_condition()
结果：
OK
status = NotAttached
release WaitSet hold
Fast DDS baseline 明确定义的 not-attached precondition
status = NotAttached
release hold
其它错误或 exception
retain:
status
ConditionInfo
hold

WaitSetInfo -> Unresolved

### 8.19 Poisoned WaitSet

如果某个 `WaitSetInfo`：
无法证明全部 Conditions 已 detach
则：
WaitSetStatus = Poisoned
并：
停止创建新的 WaitSetInfos

停止新的 Fast DDS WaitSet wait
Unresolved `WaitSetInfo`：
RetiredWaitSetRegistry
这使：
一个 WaitSet 不会无限不断制造
新的 unresolved `WaitSetInfo` instances

### 8.20 Poisoned Public Behavior

仍先遵循：
arguments
Context state
之后：
wait()
DdsError
不消费：
Guard
Event
logical readiness。
add()
DdsError
且无 logical registration。
remove()
仍允许：
logical detach
因为 teardown 必须可以继续。

### 8.21 Historical Reader Hold

Reader StatusCondition 成功或可能 attach：
DataReaderInfo.waitset_holds.refs++
Generation 被证明 detach：
refs--
Unresolved：
hold 保留
这直接控制：
DataReader 是否允许 delete

### 8.22 Historical Guard Hold

Public GuardCondition 同样：
generation
    ->
shared GuardConditionInfo
+
waitset hold
Guard facade destruction：
accepting=false
old unresolved WaitSetInfo：
仍可以安全保存 Fast DDS GuardCondition pointer

### 8.23 RetiredWaitSetRegistry

Unresolved WaitSetInfo 使用：
pre-existing node
+
intrusive no-allocation adoption
Retirement 过程中不能：
hold reconciliation_mutex
    ->
lock RetiredWaitSetRegistry
正确：
produce unresolved node

unlock reconciliation

adopt
Reader deferred cleanup retry 同理：
reconciliation unlock 后执行

### 8.24 Private Control Guard

每 WaitSet 有：
one private Fast DDS GuardCondition
只用于：
wake Fast DDS WaitSet wait

```cpp
enum class ControlGuardState
{
    Healthy,
    Broken
};
```

```cpp
struct ControlGuardInfo
{
    GuardConditionHandle guard;

    std::atomic<ControlGuardState>
        state{ControlGuardState::Healthy};

    std::atomic<std::uint64_t>
        control_generation{0};
};
```

WaitSetInfo 通过 shared_ptr<ControlGuardInfo> 保活 historical private control condition；
replacement 不能释放仍被旧 generation 引用的 ControlGuardInfo。
每个 ControlGuardInfo 保存自己的 generation，replacement generation 从 0 重新开始。
control_generation 只是 wake change stamp；
logical authority 仍然是：
Context/shutdown state
WaitSet topology_generation
Registration phase
Guard/Event logical state
Server capacity state。

Topology/cancellation mutation：
先 commit logical state
    ↓
release topology/child/local state mutex
    ↓
checked advance current ControlGuardInfo.control_generation
    ↓
set_trigger_value(true)
Fast DDS GuardCondition trigger failure：
logical state 不回滚
bounded wait slice 是最后 fallback。

control_generation 禁止 unsigned wrap。
如果 checked advance 发现 UINT64_MAX：
当前 ControlGuardInfo -> Broken
不再增加该 generation
logical mutation 仍然有效
best-effort 保持/设置 Fast DDS GuardCondition trigger true
reconciliation 创建 replacement ControlGuardInfo，replacement generation 从 0 开始。
如果旧 control condition 无法安全 detach：
WaitSet -> Poisoned。

### 8.25 Control Guard Reset

Wait thread：
取得 current ControlGuardInfo stable reference
snapshot = control_info.control_generation

Fast DDS set false

re-read same control_info.control_generation
并确认 control_info 仍为 current control authority
如果 generation 改变或 backing 已被替换：
对 current ControlGuardInfo set true again
避免 lost wake。
如果 ControlGuardInfo 已 Broken：
不再依赖 reset result；进入 replacement/reconciliation path。

### 8.26 Broken Control Guard

`WaitSetState::control_guard_replacement` 是 replacement transaction 的唯一 public-independent internal state authority。

如果：
set_trigger_value(false)
失败，旧 private GuardCondition 可能永久保持 true，造成 busy loop。
因此：
ControlGuardInfo
    -> Broken
WaitSet reconciliation 必须：
detach broken control guard

retire current WaitSetInfo

create replacement control guard

build next generation
如果 broken control condition：
无法被证明 detach
则：
WaitSet -> Poisoned
不能继续让 stuck-true Fast DDS condition 空转。

Replacement transaction：
1. Broken 被确认后 `Required`；旧 `ControlGuardInfo` 继续由 historical `WaitSetInfo` strong-own，不作为新的 wake authority；
2. 普通 wait/reconciliation owner CAS/claim `Required -> Building`；
3. 在任何 Fast DDS attach 前完成 replacement backing、generation attachment record 和所需容器预分配；
4. allocation `std::bad_alloc`：当前普通 `wait()` 原样传播，state 回到 `Required`，后续调用可重试；
5. Fast DDS Guard construction/initialization 抛异常或返回无法建立可用 backing 的失败：`WaitSet -> Poisoned`，不得保留无 authority 的 Building 状态；
6. replacement attach failure/exception 按 AttachmentStatus 规则处理；只要无法取得 safe attachment evidence，`WaitSet -> Poisoned`；
7. build 期间 topology_generation 改变：未 publish replacement generation 正常 retire，state 回 `Required`/重新 reconciliation；
8. topology stable 且 replacement generation publish 成功：`Published -> Idle`，新的 `control_guard` 成为 wake authority；
9. 在 `Required/Building` 窗口内仍依靠 bounded 100 ms Fast DDS WaitSet wait slice + logical readiness precheck 保证最终 progress，不允许无限阻塞等待一次 control wake。

### 8.27 Internal Wait Slice

constexpr auto kWaitControlSlice =
    std::chrono::milliseconds{100};
Public Poll：
Fast DDS timeout = 0
Public Finite：
min(
    remaining public deadline,
    100ms)
Public Infinite：
repeated 100ms
Internal slice timeout：
不对用户可见
V1 将 100 ms slice 作为 control-wake failure 的 correctness fallback；
因此典型部署假设 WaitSet 数量较少，主要对应 Executor/dispatcher 级 wait loop，而不是每个 endpoint 一个独立 WaitSet。
性能测试必须覆盖 idle WaitSet 数量增长时的 wakeup/CPU 成本；
性能优化不得取消 bounded fallback，除非能提供等价 cancellation evidence。

### 8.28 Wait Main Loop

validate timeout

OperationGuard

claim active_wait：
expected = false
active_wait.compare_exchange_strong(expected, true)
失败：
    Busy

ActiveWaitGuard

在第一次 Fast DDS WaitSet wait 前：
重新检查 Context shutdown / shutdown_child requested_generation；
如果 cancellation 已经请求：
直接退出并由 ActiveWaitGuard 完成 ack，不能进入 Fast DDS WaitSet wait。

compute absolute deadline once
Loop：
1. Context shutdown?
       -> ContextShutdown

2. WaitSet Poisoned?
       -> DdsError

3. reconcile topology

4. logical readiness precheck

5. if candidates:
       allocate WaitResult storage
       revalidate
       consume GuardCondition readiness only
       DO NOT advance any Event cursor
       return Ready

6. construct WaitReferenceGuard for current WaitSetInfo

7. Fast DDS WaitSet wait

8. interpret active Conditions only through current WaitSetInfo attachment mapping

9. release WaitReferenceGuard before publishing final Ready/Timeout/Error

### 8.29 Public Timeout Final Precheck

Public timeout 前：
再执行一次 nonblocking readiness precheck
若此时 ready：
Ready
否则：
Timeout
避免 deadline 边界的 obvious lost readiness。

### 8.30 WaitResult Allocation

如果：
ready vector allocation
抛：
std::bad_alloc
则 exception 传播。
且：
Guard trigger 未消费
Event cursor 本来就不由 WaitSet 推进
不能转换：
ResourceExhausted

### 8.31 Server Capacity 与 WaitSet Topology

Server capacity：
available -> full
以及：
full -> available
都是：
WaitSet desired topology mutation
所以：
在 topology_mutex 下 checked advance topology_generation；
如果 generation 已经 UINT64_MAX：
    commit 必要的 capacity logical state
    wait_set_status = Poisoned
    retire current WaitSetInfo
    不再创建新 generation
否则：
    topology_generation = current + 1
control wake
Full：
request DataReader StatusCondition
不加入 blocking Fast DDS WaitSet topology
Available：
重新 attach
避免：
request unread
+
capacity full
+
StatusCondition permanently true
造成 WaitSet busy loop。
PendingRegistry mutex 必须先释放，再通知 WaitSet。

### 8.32 GuardCondition Logical Counters

```cpp
struct GuardState
{
    std::mutex mutex;

    std::uint64_t
        trigger_generation{0};

    std::uint64_t
        consumed_generation{0};

    std::shared_ptr<
        GuardConditionInfo>
        guard_info;

    std::shared_ptr<
        WaitableState>
        waitable;
};
```

Ready：
trigger_generation
!=
consumed_generation

### 8.33 Guard Counter Exhaustion

Guard 是 coalescing trigger，不是 counting semaphore。
因此不需要把 counter exhaustion 暴露成新的 public error。
如果：
trigger_generation == UINT64_MAX
AND
consumed_generation < UINT64_MAX
已经 pending。
新的 trigger：
保持 UINT64_MAX
return success
仍然只是：
one pending readiness
如果：
trigger_generation
==
consumed_generation
==
UINT64_MAX
说明当前无 pending。
在 Guard mutex 下 renormalize：
trigger_generation = 0
consumed_generation = 0
然后新 trigger：
trigger_generation = 1
不 wrap，不丢 readiness。

### 8.34 GuardCondition Trigger

Public `GuardCondition::trigger()` 的 public success linearization 是：
logical trigger generation 在 Guard mutex 下成功 commit。

顺序：
validate public arguments
OperationGuard
validate parent/object state
lock Guard mutex
checked/coalescing logical generation commit
unlock Guard mutex
    ↓
从这一点开始 public trigger 已经成功产生 effect
    ↓
if GuardConditionStatus == Healthy:
    best-effort Fast DDS set_trigger_value(true)
else:
    skip public Fast DDS GuardCondition wake
    best-effort private control wake
    return success

Fast DDS set_trigger_value(true) == OK：
    -> return success

Fast DDS ReturnCode failure：
    -> GuardConditionStatus = LogicalOnlyDegraded
    -> logical pending trigger 保留
    -> fixed-size diagnostic
    -> best-effort trigger WaitSet private control GuardCondition
    -> return success

Fast DDS unexpected C++ exception：
    -> catch inside this post-commit notification path
    -> GuardConditionStatus = LogicalOnlyDegraded
    -> logical pending trigger 保留
    -> fixed-size diagnostic
    -> best-effort private control wake
    -> return success

private control wake 在该 post-commit path 中同样是 best-effort：
ReturnCode failure / C++ exception
    -> catch / diagnostic / ControlGuardInfo health update
    -> 不改变 public success
bounded 100 ms slice 仍是最终 correctness fallback。

这里 catch Fast DDS wake exception 不违反 4.2.1 ordinary exception boundary，
因为 logical public operation 已经 commit；
该 Fast DDS API call 只是 notification optimization，
不能在 effect 已经成功提交以后向 caller 报告一个“trigger failed”。

如果 public/Context/parent/object validation 在 logical commit 前失败：
按普通 public error priority 返回对应 Error；
此时不得修改 trigger_generation。

如果已注册 WaitSet：
private control GuardCondition + bounded Fast DDS WaitSet wait slice
保证 logical readiness 最终重新被观察；
一次 Fast DDS public Guard wake failure 不是 public trigger failure。

### 8.35 Guard Logical-only Degradation

一旦 public Fast DDS GuardCondition：
trigger/reset
出现 unrecoverable Fast DDS failure：
GuardConditionStatus =
LogicalOnlyDegraded
之后该 public GuardCondition：
不再进入新 WaitSetInfo attachment set
WaitSet readiness：
完全由 logical generation precheck
并依赖：
private control GuardCondition
+
bounded wait slice
进行 wake。
如果旧 generation 仍 attach Fast DDS Guard：
按正常 historical WaitSetInfo
detach/retire
若 detach 无法证明：
WaitSet Poisoned

### 8.36 Guard Consume / Reset

成功构造 WaitResult storage 后：
lock Guard

snapshot =
trigger_generation

consumed_generation =
snapshot

unlock
如果 Fast DDS Guard 仍 Healthy：
set false
然后：
lock

new_pending =
trigger_generation
!=
consumed_generation

unlock
若 new pending：
set true
Reset failure：
Fast DDS state -> LogicalOnlyDegraded
Logical consumed state：
仍然有效

#### 8.36.1 WaitSet 对 Guard 与 Event 的消费边界

`WaitSet::wait()` 对 readiness 的消费语义固定为：

- `GuardCondition`：成功构造并 commit Ready `WaitResult` 时消费本次 coalesced logical trigger；
- `Event`：**level-triggered**；`wait()` 只返回 Event token，不推进 `EventState.cursor`，也不构造/消费 `EventInfo`；
- 只有 `Event::take(EventInfo&)` 成功取得 EventInfo 后才推进该 Event 自己的 cursor。

因此：

```text
event update
  -> wait() returns Event token
  -> wait() again before Event::take()
  -> still Ready with Event token
  -> Event::take() returns Taken + EventInfo
  -> cursor advances
  -> next wait is not ready unless a new change exists
```

一个 Event token 被 wait 返回不表示 event data 已被消费。
多个 Event 实例继续各自维护独立 cursor。

### 8.37 Event 多实例

一个：
EventSourceState
可以拥有：
Event A
Event B
Event C
同一 EventType 也允许。
每个 Event：
独立 cursor
因此：
A.take()
不能消费：
B
的 change。

### 8.38 Event Creation Cursor

Event Factory commit 时：
lock EventSource

EventState.cursor =
    source current cumulative state

register EventState

unlock
所以 V1 语义：
source update before Event Factory commit
    -> new Event 不 replay 该历史 change
Event Factory commit before source update
    -> new Event 可以观察该 update。
Event 的创建 linearization point 是：
在 EventSource mutex 下设置 cursor 并把 EventState 注册到 source 的 commit。

### 8.39 Event 累计模型

Deadline/LivelinessLost/IncompatibleQos/MessageLost：
source:
latest Fast DDS total
+
checked cumulative change
每个 Event：
reported change =
source cumulative change
-
Event cursor
successful take()：
cursor =
source current cumulative

### 8.40 LivelinessChanged

必须独立维护：
latest alive_count
latest not_alive_count

cumulative alive_count_change
cumulative not_alive_count_change
因为：
alive_count
not_alive_count
是 gauge。
不能通过：
current total - old total
统一处理。

### 8.41 IncompatibleQos

维护：
latest total_count

cumulative total_count_change

latest last_policy
Event take：
last_policy
=
当前 source latest last_policy
cursor 只消费 change，不消费 global latest policy state。

### 8.42 Event Exhaustion

内部累计使用：
checked wider integer
任何：
internal arithmetic overflow

或

无法转换到 public EventInfo field
导致：
EventSource[type]
    -> Exhausted
此后：
Event logically ready

Event::take()
    -> ResourceExhausted

cursor unchanged
所有该 source/type Event 都观察 Exhausted。

### 8.43 Event Degraded

Callback 无法保证 cumulative state 正确：
EventSource[type]
    -> Degraded
之后：
Event logically ready

Event::take()
    -> DdsError
永久不恢复。

<a id="fastdds-event-parent-destruction"></a>

### 8.44 Event Parent Destruction

`EventState` strong-own `EventSourceState`；`EventSourceState::parent` 是 atomic
`Alive/Destroyed` authority，Event registration 使用 checked monotonic、EventSource-lifetime
不复用的 `event_registration_id`。source 中每个 weak registration 都按该 ID 可稳定遍历。

Parent endpoint `Alive -> Closing` 后执行以下 no-throw、no-allocation 两阶段 fan-out：

```text
Phase A — publish close reason for every Event
    lock EventSource.mutex
    atomic store EventSource.parent = Destroyed
    unlock EventSource.mutex
    cursor = invalid event_registration_id
    repeatedly:
        lock EventSource.mutex
        event = next live stable EventState handle after cursor
        unlock EventSource.mutex
        if no event: break
        cursor = event.event_registration_id
        lock event.WaitableState.mutex
        if close_reason == None:
            close_reason = ParentDestroyed
        unlock

Phase B — logical auto-detach and wake
    traverse the same stable IDs again
    for each still-live EventState:
        run the common two-phase auto-detach protocol
        perform best-effort control wake only after releasing
            EventSource / topology / registration / waitable mutexes
```

Event Factory 在 `EventSource.mutex` 下同时检查 `parent == Alive`、初始化 cursor、分配
registration ID并注册 EventState；因此 parent close 与 Factory commit 有唯一顺序。Phase A
开始后不能再注册新 Event。Event 自身析构可以并发移除 weak registration；若它在 fan-out
snapshot 前消失，则其 public lifetime 已结束，无需 detach。stable handle 已取得时，Event
析构与 parent fan-out 仍由 `Attached -> Detaching` CAS 决定唯一 detach winner。

必须先完成全部 Phase A publication，才能对任何 Event 启动 Phase B；所以某 Event 的
auto-detach/wake不会早于另一 Event 的 `ParentDestroyed` authority commit。普通 fan-out 不
分配内存、不嵌套 same-rank Event/Waitable mutex，也不在任何 DMW mutex下调用 Fast DDS
`set_trigger_value()`。单个 Fast DDS wake失败只记录 fixed diagnostic；logical close/detach已
提交，bounded WaitSet slice保证最终重新计算。

之后：

```text
Context Active   -> ParentDestroyed
Context shutdown -> ContextShutdown
```

保持全局错误优先级。`WaitSet::add()` 可无锁读取 stable EventSource parent authority，并在
topology -> waitable锁序下重读 `WaitableCloseReason`；不得为了判断 parent 状态反向取得
endpoint/EventSource mutex。

<a id="fastdds-teardown"></a>

## 9. Retirement、Final Teardown 与 Terminal Quarantine

本章按照 endpoint retirement、container evidence barrier、Participant final authority 和 process-lifetime quarantine 的顺序定义失败安全 teardown。无法证明 DDS entity graph 已删除时，必须保留 backing，不能以 cleanup 完整性换取 UAF 风险。

### 9.1 Private Destroy

每个 public Resource/Entity：
void Impl::destroy() noexcept;
必须：
idempotent
noexcept
best-effort
Repeated call：
不能 double delete
不能 double release Lease
不能 double adopt retirement

### 9.2 Retirement Preallocation

Endpoint Factory 在 DDS entity 创建前：
预分配 retirement node
Client/Server：
预分配 aggregate retirement node
因此 public destructor delete failure：
不需要分配内存

### 9.3 Retirement Diagnostic

Correctness 不依赖：
dynamic std::string
固定：

```cpp
struct CleanupDiagnostic
{
    OperationKind operation;

    EntityKind entity_kind;

    std::int32_t return_code;

    std::uint64_t logical_instance_id;
};
```

完整日志 message：
best-effort only

### 9.4 OrphanedEndpointRegistry

Orphaned endpoint retirement node 必须区分：

```cpp
enum class OrphanedEndpointKind
{
    KnownHandle,
    HiddenContainedEntity
};
```

KnownHandle：
DMW 拥有 DataReader*/DataWriter*，但 individual delete 当前失败/deferred。

HiddenContainedEntity：
create call 已 entered Fast DDS，`CreationStatus::SideEffectIndeterminate`，
但 DMW 没有可靠 child handle；backing 只为 lifetime safety 保留。

Adopt：
move already-existing backing
into preallocated retirement node
intrusive insert
No allocation。

Retry KnownHandle：
lock registry
unlink candidate
unlock
Fast DDS cleanup
success -> release resources
failure -> relink same node。

Retry HiddenContainedEntity：
禁止 individual delete，因为没有可靠 handle；
保持 intrusive node，直到 parent container evidence barrier：
Subscriber/Publisher delete_contained_entities or delete success，
或最终 Participant contained-graph/delete success。

parent barrier success 后：
在 registry mutex 下 unlink hidden node；
unlock；
执行必要 callback drain；
release listener/TopicLease/TypeLease/backing。

barrier failure/exception：
保留 hidden node；
不得把 null DDS entity pointer 误解释成 NoSideEffect/KnownDeleted。

### 9.5 Indeterminate DDS Entity Pointer

Individual endpoint delete success：
KnownDeleted
DDS entity pointer = nullptr

### 9.6 delete_contained_entities() Failure

Failure 可能表示：
部分 DDS entity 已删除
部分未删除
因此所有无法 individual evidence 的 old pointers：
Indeterminate
此后：
否则可能 double-delete。
只能继续：
container-level teardown

### 9.7 Final WaitSet Barrier

Context Fast DDS teardown 前必须：
RetiredWaitSetRegistry
不存在 Unresolved WaitSetInfo
如果仍存在：
STOP
禁止：
delete_datareader

delete_contained_entities

delete DDS Subscriber

delete DDS Publisher

delete Participant
整个 DDS entity graph：
进入 TerminalContextNode
原因：
old Fast DDS WaitSet
可能仍然保存 Condition*

### 9.8 Final Teardown Order

final teardown 使用 local：

`participant_barrier_succeeded = false`

固定顺序：

1. verify `RuntimeState == Shutdown && shutdown_execution == Completed`。若 execution 为 `Idle/Running`，只能通过 [Context shutdown](#fastdds-context-shutdown) 的 terminal helper 先得到 `Completed/Failed`；若为 `Failed` 或无法证明 runtime quiescence，则 normal final teardown 禁止继续，直接 terminal quarantine，STOP；绝不 retry partial Failed shutdown phases。
2. resolve/retire all WaitSet generations。
3. unresolved WaitSet generation exists -> terminal quarantine，STOP。
4. 在 DiscoveryListenerState callback mutex 下提交 discovery `accepting=false`。
5. `participant.set_listener(nullptr)` best effort。
6. first discovery callback drain 使用5.2.1 zero-count drain protocol；无法证明zero则 terminal quarantine，STOP。
7. retry individual endpoint retirements。
8. 若 Subscriber `entity_status == KnownAlive`，调用 Subscriber::delete_contained_entities()；failure/exception -> `subscriber_entities_status=MayContainHiddenEntity`，相关 raw child evidence -> Indeterminate。
9. Publisher 对称。
10. 根据 individual/container evidence 标记 unresolved endpoint handles。
11. first Topic/Type cleanup retry；Topic individual delete 只允许 `TopicEntry.entity_status == KnownAlive`。
12. delete DDS Subscriber only if `subscriber_entity_status == KnownAlive` 且 individual/container state允许；success -> KnownDeleted + pointer=null；ambiguous failure/exception -> Indeterminate。
13. Subscriber contained cleanup/delete success 后，reconcile remaining DataReaderInfo；second listener drain；release endpoint leases/backing；graph=Exact。
14. Publisher 对称。
15. 计算是否需要 Participant contained-graph recovery barrier：

```text
participant_entities_status == MayContainHiddenEntity
OR publisher_entities_status == MayContainHiddenEntity
OR subscriber_entities_status == MayContainHiddenEntity
OR publisher/subscriber handle evidence == Indeterminate
OR any endpoint handle evidence == Indeterminate
OR any TopicEntry creation_status == SideEffectIndeterminate
OR any TopicEntry entity_status == Indeterminate
OR any contained cleanup partial/ambiguous result
OR Context construction recorded unknown participant-contained child
```

16. 若需要 barrier：

```text
participant_entity_status != KnownAlive
    -> cannot safely call Participant API
    -> terminal quarantine

otherwise
    -> call DomainParticipant::delete_contained_entities() outside all DMW mutex
```

**Barrier success 是所有 prior Participant child raw pointers 的 invalidation/lifetime barrier。** success 后必须在任何后续 individual child delete 前统一 reconciliation：

```text
participant_barrier_succeeded = true
participant_entities_status = Exact
publisher_entities_status = Exact
subscriber_entities_status = Exact

dds_publisher = nullptr
publisher_entity_status = KnownDeleted

dds_subscriber = nullptr
subscriber_entity_status = KnownDeleted

for every TopicEntry:
    topic = nullptr
    entity_status = KnownDeleted

for every DataReaderInfo:
    reader = nullptr
    state = Deleted

for every DataWriterInfo:
    writer = nullptr
    state = Deleted
```

然后：
- 禁止再调用 `delete_datareader/delete_datawriter/delete_topic/delete_subscriber/delete_publisher` 使用 barrier 前 raw pointer；
- 对所有 endpoint listener 执行必要 second drain，之后释放 listener/TopicLease/TypeLease/backing；
- TopicRegistry 在 mutex 下把可释放 TopicEntry/lease 移出到 local cleanup list，unlock 后释放 TypeLease；
- hidden/known TopicEntry 都按 KnownDeleted evidence reconciliation，不只处理 hidden entry；
- Type registration 仍由 `TypeRegistrationStatus` 管理；Participant contained barrier success 不自动伪造 unregister_type success。

Barrier failure/exception：

```text
participant_barrier_succeeded = false
participant_entities_status = MayContainHiddenEntity
retain all affected backing/leases/listeners
all ambiguous prior child pointers remain Indeterminate
```

不得猜测 individual deletion state。

17. **只有 `participant_barrier_succeeded == false`** 时，才执行 second Topic cleanup；并且只 retry `TopicEntry.entity_status == KnownAlive` 的 Topic。`Indeterminate` Topic pointer 永不再次 individual delete。
18. second Type cleanup retry；Registered entry可正常 unregister；Indeterminate registration retain canonical binding if evidence cannot be obtained。
19. delete DomainParticipant only if `participant_entity_status == KnownAlive`；failure/exception -> participant handle Indeterminate + terminal quarantine。
20. DomainParticipant delete success -> participant KnownDeleted/null；作为所有 Participant-owned Fast DDS reference 的最终 authority，final callback drain 后释放剩余 defensive backing。
21. DomainParticipant delete failure/Indeterminate -> transfer unresolved graph to ProcessTerminalQuarantine。

### 9.9 为什么有第二轮 Topic/Type Cleanup

例如：
first delete_topic
    -> PRECONDITION failure

delete DDS Subscriber
    -> succeeds
Reader 已不存在。
此时：
second delete_topic
可能成功，但前提是 Participant contained-graph barrier 尚未成功，且 TopicEntry.entity_status 仍明确为 KnownAlive。
如果 barrier 已成功，prior Topic* 全部视为 stale/KnownDeleted，严禁第二轮 individual delete。
如果没有该条件化第二轮，KnownAlive remaining Topic 可能导致 Participant delete unnecessary failure；如果对 Indeterminate/stale pointer盲目重试，则可能 double delete/UAF。

### 9.10 DDS Subscriber Success

成功删除 DDS Subscriber：
证明该 Subscriber-contained
remaining DataReaders 已不存在
所以相关：
DataReaderInfo
可以：
state = Deleted
reader = nullptr
然后：
second listener drain
release listener
release TopicLease
release TypeLease

### 9.11 DDS Publisher Success

对 Writer 完全对称。

#### 9.11.1 Participant Contained-Graph Recovery Barrier

`ParticipantInfo::participant_entities_status == MayContainHiddenEntity` 表示：
至少一次 Participant-contained graph operation 使 DMW 无法仅靠 individual handle 安全证明 graph clean。
来源包括 hidden Topic/child create，也包括 Subscriber/Publisher contained cleanup partial failure、container delete evidence Indeterminate、以及因此失去 individual authority 的 endpoint raw pointer。

此时最终 teardown 不能仅凭已知 Topic/Publisher/Subscriber map 为空得出 graph clean。
必须在不持有 DMW mutex 时调用：
DomainParticipant::delete_contained_entities()。

该 API 在 frozen Fast DDS 2.6.12 baseline 中被用作 Participant-contained entity cleanup barrier；
targeted baseline test 必须验证 success 后：
- remaining Publisher/Subscriber/Topic 不再 Fast DDS-live；
- hidden create 预留 listener/TopicLease/TypeLease backing 可以在 callback drain 后释放；
- 已知 raw pointer 不再执行 individual delete，避免 double delete。

failure/exception：
不提供“部分对象已删除”的 individual evidence；
所有受影响 pointer/backing 按 Indeterminate 保留，
最终只允许依赖 DomainParticipant delete success 或 terminal quarantine。

该 barrier 不自动证明 TypeSupport registration 已经通过 unregister_type() 移除；
TypeRegistry 仍按 TypeRegistrationStatus 管理 canonical binding，
但 DomainParticipant delete success 是所有 participant-owned Fast DDS references 的最终 lifetime barrier。

### 9.12 Participant Success

成功：
delete DomainParticipant
作为最终 authority：
Participant-contained DDS entity graph gone
剩余 defensive backing：
可以安全释放

### 9.13 Participant Failure

若：
delete Participant
仍失败：
所有无法证明 safe-to-release
的 DDS entity pointers and Info objects
必须进入：
ProcessTerminalQuarantine

### 9.14 QuarantinedParticipantInfo

```cpp
struct QuarantinedParticipantInfo
{
    std::unique_ptr<ParticipantInfo>
        participant_info;

    std::unique_ptr<TypeRegistryState>
        types;

    std::unique_ptr<TopicRegistryState>
        topics;

    IntrusiveEndpointRetirementList
        endpoint_retirements;

    IntrusiveWaitSetList
        waitset_retirements;

    std::shared_ptr<ParticipantObservationRegistryState>
        participants;

    std::shared_ptr<RemoteEndpointRegistryState>
        remote_endpoints;

    std::shared_ptr<ServiceMatchRegistryState>
        service_matches;

    std::shared_ptr<TargetReaderObservationRegistryState>
        target_readers;

    std::shared_ptr<
        DiscoveryListenerState>
        discovery_listener_state;
};
```

### 9.15 Retirement 不得强持 ContextState

TopicLease / TypeLease 内部指向 stable RegistryState，而不是 `shared_ptr<ContextState>`，否则：

```text
Context
 -> OrphanRegistry
 -> Retirement
 -> Lease
 -> Context
```

形成循环。

`TypeRegistryState` / `TopicRegistryState`：
由 unique_ptr ownership；
移动到 QuarantinedParticipantInfo 后 State object 地址不变；
TypeLease / TopicLease 保存的 stable State pointer 仍有效。

以下 discovery-related state 由 shared_ptr ownership：
- `ParticipantObservationRegistryState`
- `RemoteEndpointRegistryState`
- `ServiceMatchRegistryState`
- `TargetReaderObservationRegistryState`

terminal transfer 通过 shared_ptr move 保持同一 control block / object；
`DiscoveryListenerState` 的 weak_ptr 仍可安全 lock，直到 final listener drain 完成。

Participant observation entries本身 Context-lifetime stable；Remote/Target state中保存的 shared participant handles不会反向 strong-own ContextState。

Retirement Info 不得反向 strong-own ContextState。

### 9.16 TerminalContextNode

在：
first Fast DDS Participant create
之前
预分配。
Terminal transfer 只允许：
unique_ptr move
shared_ptr move
intrusive splice
pointer assignment
禁止：
vector grow
map rebuild
string construction
new
确保 catastrophic cleanup 路径：
allocation-free

<a id="fastdds-terminal-quarantine"></a>

### 9.17 ProcessTerminalQuarantine

ProcessTerminalQuarantine 是：
process-lifetime
thread-safe
allocation-free-on-adoption
non-destructing
terminal fail-safe storage。

它接收两类 TerminalContextNode：
1. normal/final teardown 无法证明 Participant graph 可安全释放；
2. Context Factory root/child create 在 public commit 前发生 SideEffectIndeterminate，
   包括 create_participant() exception 后没有 participant handle 的 partial Context。

因此 QuarantinedParticipantInfo 允许是 partial bundle；
`ParticipantInfo::participant` 可以为 nullptr，
同时 `participant_creation_status == SideEffectIndeterminate`。
这种 null handle 绝不等价于“没有 Participant”；
quarantine 必须继续保活 discovery listener/state 和所有可能被隐藏 DDS entity graph 引用的 backing。

它不是：
正常 resource pool
可恢复 cache
后续 Context 可复用的 DDS entity owner。

初始化：
DmwProcessRuntime 在第一个 Context DDS resource 创建前完成 process runtime 初始化。

推荐唯一模型：

```cpp
class ProcessTerminalQuarantine
{
public:
    void adopt_released_node(TerminalContextNode*) noexcept;

private:
    std::mutex mutex_;
    IntrusiveTerminalContextList contexts_;
    std::atomic<std::uint64_t> adopted_count_{0};
};
```

ProcessTerminalQuarantine 本体必须在 runtime bootstrap 阶段构造完成。

non-destructing storage：
允许使用一次性 heap allocation：

static ProcessTerminalQuarantine& quarantine()
{
    static ProcessTerminalQuarantine* q =
        new ProcessTerminalQuarantine;
    return *q;
}

该 `new` 只允许发生在：
DmwProcessRuntime 初始化阶段
且必须早于第一个 Participant create。

catastrophic terminal adoption path 本身不得执行：
new
vector grow
map insertion
string construction
shared ownership control-block allocation。

并发 adoption：
多个 Context 可以同时进入 terminal quarantine。
每个 Context 已经拥有预分配 TerminalContextNode。

terminal adoption 的 ownership handoff 必须先于可能抛异常的 mutex bookkeeping：

```text
raw = ContextState.terminal_node.release()
// 从此 ContextState 不再析构 raw
adopt_released_node(raw) noexcept
```

`adopt_released_node(raw)` 必须使用 RAII lock；禁止手写 `lock()/unlock()` 跨越 catch boundary：

```cpp
void ProcessTerminalQuarantine::adopt_released_node(
    TerminalContextNode* raw) noexcept
{
    assert(raw != nullptr);
    assert(no_other_dmw_mutex_held());

    try
    {
        std::unique_lock<std::mutex> lock{mutex_};

        // Both operations are frozen noexcept operations.
        contexts_.splice_intrusive_noexcept(raw);
        update_diagnostic_counters_noexcept();

        // lock destructor/unlock owns mutex release on every later path.
    }
    catch (...)
    {
        // If mutex acquisition failed, raw was never inserted: intentional leak.
        // Once splice_intrusive_noexcept() ran, no later operation in the try block may throw,
        // so there is no ambiguous "inserted and leaked" ownership state.
        intentionally_forget_unowned_raw(raw);
    }
}
```

冻结要求：
- `std::unique_lock<std::mutex>` 负责所有成功 lock 后的 unlock；
- intrusive splice **必须 `noexcept`**；
- diagnostic counter update **必须 `noexcept`**（checked/saturating fixed-size atomic/counter only）；
- splice 之后不存在任何允许抛异常的 operation；
- mutex acquisition 抛 `std::system_error` 时，released raw intentional process-lifetime leak，不运行 destructor；
- 正常 splice 后 raw ownership 永久属于 quarantine intrusive list。

`ProcessBindingQuarantine` 使用完全相同的 RAII/noexcept intrusive adoption helper 与 ownership rule；不得维护第二套手动 lock/unlock pseudo-protocol。

Quarantine mutex 不参与普通 lock rank：
规则是只能在“零其它 DMW locks”状态取得，
因此不会形成 rank inversion。

Intrusive sentinel：
由 non-destructing ProcessTerminalQuarantine 本体拥有。
TerminalContextNode：
由每个 Context 在 Participant create 前预分配。

Diagnostic：
correctness path 只使用固定大小字段：
OperationKind
EntityKind
Fast DDS return code
logical instance id
fixed counters/flags

完整日志字符串：
best-effort only
不能作为 adoption 成功条件。

进程退出：
禁止注册 atexit cleanup 去释放 quarantine；
禁止 static object destructor 间接析构 QuarantinedParticipantInfo；
最终虚拟内存和 OS DDS resources 由进程退出回收。

这是一种：
以 process-lifetime resource retention 换取 memory safety 的 terminal fail-safe。

<a id="fastdds-lock-error-model"></a>

## 10. Lock Model 与 Fast DDS ReturnCode Mapping

本章集中定义 lock rank、允许的嵌套边、Fast DDS API call unlock rule，以及 operation-specific ReturnCode/status matrix。其它章节描述具体事务，但不得创造新的隐式锁序或通用化错误映射。

### 10.1 Lock Rank

Process IDs：
atomic，无 mutex。

ProcessTerminalQuarantine / ProcessBindingQuarantine：
只允许在没有其它 DMW mutex 时进入；
各自使用 process-lifetime non-destructing quarantine mutex；
adoption path 必须 no-allocation。

普通 lock rank：

```text
1   Context runtime/shutdown
2   ChildRegistry
3   TypeRegistry
4   TopicRegistry
5   ParticipantObservationRegistry
6   RemoteEndpointRegistry
7   ServiceMatchRegistry
8   TargetReaderObservationRegistry
9   OrphanedEndpointRegistry
10  RetiredWaitSetRegistry
11  NodeState / EndpointRuntimeState / DataReaderInfo / DataWriterInfo / GuardConditionInfo
12  WaitSet topology
13  WaitSet reconciliation
14  RegistrationState
15  Waitable / Event / Guard local state
16  PendingRequestRegistry
17  ListenerState / DiscoveryListenerState
```

补充规则：
- rank 数值只描述**允许的嵌套方向：低 -> 高**；
- ParticipantObservation callback handoff 按 [discovery commit order](#fastdds-discovery-commit-order) 通常选择“unlock rank 5，再进入 rank 6/8”，而不是依赖嵌套；
- 唯一允许的 discovery cross-registry nested edge 是 [discovery commit order](#fastdds-discovery-commit-order) 定义的
  `RemoteEndpointRegistry(rank 6) -> ServiceMatchRegistry(rank 7)`；该 edge用于把 remote
  mutation与 affected local service entries 的 `NeedsRebuild` publication放在同一 commit
  boundary；除此之外 discovery registry之间均先 unlock 再 handoff；
- Target predicate绝不从 rank 8 反向进入 Participant(rank 5) 或 Remote(rank 6)；participant lifecycle/capability通过 stable entry atomic snapshot读取；
- Pending(rank 16) 与 Target(rank 8) 在 `send_response()` 中永不同时持有；
- Topic(rank 4) 永不调用 TypeLease acquire/release，因此不存在 Topic -> Type(rank 3)。

### 10.2 Same-rank Rule

默认：
不同实例的 same-rank mutex
不同时持有
确实需要：
stable logical instance_id
ascending
禁止：
pointer address
作为 lock ordering。

### 10.3 WaitSet 特殊锁规则

topology_mutex
和
reconciliation_mutex
永不嵌套。
WaitSet 关键字段同步规则固定为：
registrations / logical topology
    -> topology_mutex
current_wait_set / WaitSetInfo publication-retirement
    -> reconciliation_mutex
topology_generation
    -> atomic，mutation 仍在 topology_mutex 下完成
wait_set_status
    -> atomic
active_wait
    -> atomic CAS claim / atomic release
RegistrationState.phase / active_wait_count
    -> atomic；drain_cv 只用于等待 refs 归零
shutdown requested/ack generation
    -> InternalChildState atomics + child mutex/cv
不得让实现自行选择另一把 mutex 保护同一字段，否则 lock evidence 不再成立。

active_wait 的 false -> true 只能由 WaitSet::wait() CAS claim；
true -> false 只能由对应 ActiveWaitGuard release。
shutdown 观察 active_wait == false 只能立即 ack 当前 generation，
但任何已经取得 OperationGuard、尚未进入 Fast DDS WaitSet wait 的 wait operation 仍必须在真正阻塞前重新检查 shutdown request；
因此不会在 ack 后新进入一个未观察 cancellation 的 Fast DDS WaitSet wait。

<a id="fastdds-native-call-rule"></a>
<a id="fastdds-api-call-rule"></a>

### 10.4 Fast DDS API Call Rule

以下 Fast DDS operations：
create/delete entity

write

take/read

set_listener

GuardCondition::set_trigger_value

attach/detach condition

WaitSet::wait

register/unregister type

create/delete topic

delete_contained_entities

delete Participant
原则：
除下述 WaitSet 例外外，流程应采用：
logical reservation

unlock all DMW mutex

Fast DDS operation

relock

commit/rollback

WaitSet 例外：
Fast DDS WaitSet::attach_condition()
Fast DDS WaitSet::detach_condition()
Fast DDS WaitSet::wait()
以及 wait/reconciliation path 对 private ControlGuardInfo 执行的 set_trigger_value(false/true)
由该 WaitSet 的 reconciliation_mutex 提供 exclusive DDS entity graph ownership，
允许在持有 reconciliation_mutex 时调用；
但此时不得持有其它 DMW mutex。
该例外不能推广到：
create/delete DataReader/DataWriter
set_listener
write/take
register/unregister type
create/delete topic
delete_contained_entities
delete Participant。

### 10.5 Fast DDS ReturnCode Mapping 必须 Operation-specific

禁止：
RETCODE_X 永远映射 ErrorCode_Y。

必须结合：
Fast DDS result
+
current operation
+
logical state
+
lifetime evidence state。

普通 public ErrorCode 与 cleanup evidence 是两个维度。

例如 detach failure：
public operation 可能返回 DdsError，
同时 AttachmentStatus 必须为 Indeterminate，
不能因为已经有 ErrorCode 就丢掉 lifetime evidence。

### 10.6 Common Runtime Return Mapping

| Fast DDS result | Ordinary runtime meaning | DMW result |
|---|---|---|
| OK | operation success | success |
| create returns nullptr | public factory failure; lifetime evidence API-specific | DdsError unless higher-priority/public-specific rule applies |
| BAD_PARAMETER | public args 已由 DMW 验证，说明 implementation/Fast DDS API mismatch | DdsError |
| NOT_ENABLED | unexpected Fast DDS state | DdsError |
| IMMUTABLE_POLICY | operation 不允许修改 immutable policy | DdsError |
| ILLEGAL_OPERATION | operation/Fast DDS state mismatch | DdsError |
| UNSUPPORTED | frozen Fast DDS capability unavailable | Unsupported |
| INCONSISTENT_POLICY | resolved QoS Fast DDS consistency failure | IncompatibleQos |
| OUT_OF_RESOURCES | middleware/DDS resource ceiling | ResourceExhausted |
| NO_DATA on take | no valid sample | TakeStatus::NoData |
| TIMEOUT on internal wait slice | internal retry | not public error |
| TIMEOUT at public deadline | user-visible timeout | WaitStatus::Timeout |
| unknown return | unspecified Fast DDS failure | DdsError |

std::bad_alloc：
不是 OUT_OF_RESOURCES。
C++ heap `std::bad_alloc` 原样传播，
除非当前位于 noexcept callback/destructor boundary，
此时按照对应 capability degradation/retention protocol 处理。

Fast DDS / binding C++ non-bad_alloc exception：
ordinary runtime -> 先完成必要的 logical rollback / lifetime-evidence update，然后原样 rethrow
callback noexcept boundary -> catch + affected capability Degraded/NeedsRebuild + diagnostic
destructor/retirement/rollback-cleanup noexcept boundary -> catch + conservative evidence/retention + diagnostic

禁止把未预期 C++ exception 转换成普通 DMW DdsError。
ReturnCode_t failure 与 C++ exception 是两个独立 error channels。

#### 10.6.1 `CreationStatus` Matrix

Create API public result 与 Fast DDS lifetime evidence 必须分开记录。

| Fast DDS entity creation outcome | CreationStatus | Ownership action | Public result |
|---|---|---|---|
| valid handle returned | HandleKnown | backing records handle | continue transaction |
| call not entered because local/precheck failed | NotStarted | normal local rollback | primary local error |
| nullptr / failure with frozen no-side-effect evidence | NoSideEffect | safe to release prebuilt backing | mapped Error |
| nullptr / failure without no-side-effect evidence | SideEffectIndeterminate | retain backing; parent graph MayContainHiddenEntity | mapped Error |
| exception after entering contained-entity create | SideEffectIndeterminate | retain/adopt backing; parent graph MayContainHiddenEntity | rethrow original exception |
| exception after entering root Participant create | SideEffectIndeterminate | partial Context -> ProcessTerminalQuarantine | rethrow original exception |

NoSideEffect 不是由“返回 nullptr”字面自动推出；
必须由 Fast DDS 2.6.12 API contract + targeted baseline/fault test 冻结。
Fast DDS 文档把 create_datareader/create_datawriter 等错误路径表示为返回 nullptr，
但 DMW 仍用 targeted test 锁定“nullptr 不留下 hidden entity”的 lifetime assumption。

任何 create exception 的处理顺序固定：
catch internally
    -> record evidence
    -> transfer backing / graph degradation
    -> release DMW locks
    -> rethrow。

#### 10.6.2 `TypeRegistrationStatus` Matrix

| register_type outcome | TypeRegistrationStatus | TypeEntry action | Public result |
|---|---|---|---|
| OK | Registered | Active + canonical binding | success |
| call not entered | NotStarted | erase local Creating if safe | local error |
| frozen ReturnCode proves target not registered | NotRegistered | erase Creating / release candidate | mapped Error |
| failure without no-registration evidence | Indeterminate | Orphaned + retain canonical binding | mapped Error |
| exception after entered Fast DDS API call | Indeterminate | Orphaned + retain canonical binding | rethrow original exception |

`PRECONDITION_NOT_MET` 等 ReturnCode 的 evidence 解释必须基于 frozen Fast DDS 2.6.12 semantics；
ErrorCode mapping 不能替代 `TypeRegistrationStatus`。

unregister_type() 同样：
只有 OK 或 frozen baseline 明确证明 NotRegistered 的结果允许释放 canonical TypeSupport；
其它 failure/exception -> Indeterminate/Orphaned retention。

#### 10.6.3 `ContainedEntitiesStatus` Matrix

| Parent | Child create | Hidden-child evidence authority |
|---|---|---|
| DDS Subscriber | DataReader | Subscriber::delete_contained_entities() success or Subscriber deletion success |
| DDS Publisher | DataWriter | Publisher::delete_contained_entities() success or Publisher deletion success |
| DomainParticipant | Topic/Publisher/Subscriber | DomainParticipant::delete_contained_entities() success or DomainParticipant deletion success |
| process root | DomainParticipant | only obtained participant handle + successful delete, or process-lifetime terminal quarantine when handle unavailable |

container-level cleanup failure/exception 可能是 partial success；
不得据此为单个 unknown child 伪造 KnownDeleted evidence。

### 10.7 `AttachmentStatus` Matrix

attach_condition(condition)：

| 结果 | AttachmentStatus | public/internal outcome |
|---|---|---|
| OK | Attached | success |
| 明确在 Fast DDS API call 前未调用 | NotAttached | local failure |
| PRECONDITION / ALREADY_DELETED / BAD_PARAMETER | Indeterminate | generation rollback/retire |
| exception after entering Fast DDS API call | Indeterminate | ordinary runtime 完成 rollback/retire 后 rethrow；noexcept teardown retain/diagnostic |
| unknown ReturnCode/error | Indeterminate | generation rollback/retire；按 operation-specific ErrorCode |

除 OK 外，
V1 不从 attach error 猜测“肯定没有 attach”。

任何 successful 或 possibly-successful attach
在 Fast DDS API call 前都必须已有 ConditionAttachment record 和 hold。

### 10.8 Detach Status Matrix

detach_condition(condition)：

| 结果 | AttachmentStatus | hold action |
|---|---|---|
| OK | NotAttached | release WaitSet hold |
| frozen Fast DDS 2.6.12 明确表示“该 condition 不属于此 WaitSet”的 PRECONDITION_NOT_MET | NotAttached | release hold |
| ALREADY_DELETED | Indeterminate | retain hold/backing |
| BAD_PARAMETER | Indeterminate | retain |
| exception | Indeterminate | retain；ordinary runtime 在 evidence 固化后 rethrow，noexcept teardown 只 diagnostic |
| unknown ReturnCode/error | Indeterminate | retain |

“PRECONDITION means not attached”的解释必须：
由 Fast DDS 2.6.12 targeted unit/fault test 锁定。
如果 baseline test 无法证明该语义：
PRECONDITION 同样按 Indeterminate 处理。

该 evidence 不能推广到：
publish
take
delete entity
其它 operation。

### 10.9 `set_listener(nullptr)` Status

set_listener(nullptr) == OK：
只证明 Fast DDS API 接受 listener detach 请求；
不证明：
没有 callback 已经进入
也不替代 first/second callback drain。

set_listener failure/exception：
listener backing 必须继续保活。
如果后续 DDS endpoint delete 成功：
仍执行 second drain 后才能释放 listener。
如果 delete 失败：
listener 与对应 endpoint Info 一起 retirement/quarantine。

### 10.10 Individual Endpoint Delete Matrix

delete_datareader()/delete_datawriter()：

| 结果 | EntityStatus | 后续 |
|---|---|---|
| OK | KnownDeleted | pointer=null；second drain；release leases |
| frozen baseline 明确可证明 target 已删除的 ALREADY_DELETED | KnownDeleted | pointer=null；second drain；release |
| PRECONDITION_NOT_MET | KnownAlive 或 Indeterminate，依 operation context | retirement/retry |
| OUT_OF_RESOURCES / BAD_PARAMETER / NOT_ENABLED / ILLEGAL_OPERATION | Indeterminate | retirement |
| exception | Indeterminate | retirement |
| unknown error | Indeterminate | retirement |

V1 默认保守：
除 OK 和经过 baseline test 明确证明的 ALREADY_DELETED 外，
不把 error 当作 KnownDeleted。

如果 WaitSet historical hold > 0：
严格禁止调用 reader delete；
state = DeleteDeferredByWaitSet，
不产生 delete evidence。

#### 10.10.1 Topic Deletion Status Matrix

`delete_topic()` 只有在 `TopicEntry.entity_status == KnownAlive` 时可调用。

| Fast DDS result | Topic handle evidence | 后续 |
|---|---|---|
| OK / baseline-proven already deleted | KnownDeleted | `topic=null`；erase/release entry |
| baseline-proven failure且 target definitely still alive | KnownAlive | Orphaned；允许条件化第二轮 retry |
| PRECONDITION/unknown error，无法证明仍活 | Indeterminate | Orphaned；禁止再次 individual delete；Participant barrier |
| C++ exception | Indeterminate | 先提交 evidence/retention，再按 cleanup boundary处理；禁止重试旧 pointer |

任何 ambiguous Topic delete 都必须把 `participant_entities_status` 标为 `MayContainHiddenEntity`，确保 final teardown 会进入 Participant-level recovery evidence，而不是把 `Topic*` 的非空误作 KnownAlive。

### 10.11 delete_contained_entities Matrix

DDS Subscriber/Publisher delete_contained_entities()：

OK：
证明该 container 当时 contained endpoint 已被删除；
相关 known-handle endpoint 与 `HiddenContainedEntity` backing
可以在 callback drain / registry reconciliation 后转为 safe-to-release。

DomainParticipant::delete_contained_entities()：

OK：
在 frozen baseline evidence 成立时，作为 Participant-contained Publisher/Subscriber/Topic graph barrier；
可解决没有 individual handle 的 hidden Topic/contained-child creation record。
在该 success evidence 后：
不得再使用 prior raw child pointer 做 individual delete；
先统一 reconciliation，再释放 backing。

任何 delete_contained_entities failure/exception：
可能是 partial success。
所有无法 individual evidence 的旧 raw endpoint/topic handle：
EntityStatus = Indeterminate；
所有 hidden-null-handle backing 继续保留。

之后禁止：
再次使用这些 Indeterminate raw pointer 进行 endpoint-level delete/read/write。

只允许继续：
higher-level container/Participant teardown
+
最终 Participant delete success 或 terminal quarantine。

### 10.12 Container Delete Matrix

delete_subscriber()/delete_publisher()：

OK：
container itself KnownDeleted
并作为 authority 证明其 remaining contained endpoint 已不存在。
然后：
reconcile endpoint Info
second listener drain
release endpoint Topic/Type leases。

failure/exception：
container evidence = Indeterminate。
不能释放可能被其 contained graph 引用的 backing。
继续更高层 Participant teardown 或 terminal quarantine。

### 10.13 Participant Delete Matrix

delete_participant()：

OK：
作为最终 authority：
Participant-contained DDS entity graph gone。
剩余 defensive backing 可以释放。

任何 failure/exception：
无法证明 contained graph 已删除。
所有仍可能被 DDS entity graph 访问的 backing：
QuarantinedParticipantInfo
    ->
ProcessTerminalQuarantine。

禁止对 Indeterminate old raw handles 再做 endpoint-level delete。

### 10.14 GuardCondition set_trigger_value

Public Guard logical generation 是 authority。Public trigger 的结果矩阵为：

| 阶段或 Fast DDS result | Logical effect | Public result / internal action |
| --- | --- | --- |
| logical commit 前 validation/Fast DDS-independent failure | 无 | 返回对应 public Error |
| commit 后 `set_trigger_value(true) == OK` | 已提交 | success |
| commit 后 ReturnCode failure | 保留 | `LogicalOnlyDegraded` + diagnostic + success |
| commit 后 C++ exception | 保留 | notification boundary 内 catch，随后 degradation + diagnostic + success |

因此 V1 不允许：
public trigger() 返回 Error
但 logical pending trigger 已经成功提交。

Public/Fast DDS Guard reset 属于 WaitSet consume 后的 notification maintenance；failure 或 exception 不回滚 consumed logical state，而是进入 `LogicalOnlyDegraded` 或 replacement path。

Private control Guard 的 set-true failure/exception 不回滚 topology/cancellation logical state；set-false failure/exception 进入 `Broken` 并触发 replacement。这些状态只表达 internal notification health，不改变已经提交的 logical mutation。

### 10.15 Take / Wait / Service Timeout

| 场景 | 结果 |
| --- | --- |
| Take `NO_DATA` | `TakeStatus::NoData`，不是 Error |
| Internal WaitSet 100 ms slice timeout | internal retry |
| Public finite WaitSet deadline reached | final readiness precheck 后返回 `WaitStatus::Timeout` |
| Server response discovery 100 ms deadline | `ErrorCode::Timeout`；Pending rollback 遵循 Pending FSM |

### 10.16 Factory Primary Error

```text
Factory DDS resources created
+
Context shutdown wins commit race
+
rollback delete fails

public Factory：
必须返回 ContextShutdown。
```

Cleanup failure 进入 retirement 并记录 diagnostic，不得覆盖 primary result。

```text
second endpoint create failed
+
first endpoint rollback delete failed：
返回 original second-create error。

second endpoint create 抛 C++ exception
+
first endpoint rollback delete fails：
先把 first backing 和可能 hidden second backing 提交到 retirement/Indeterminate ownership，
然后 rethrow original second-create exception；
cleanup failure 只记录 diagnostic，不覆盖 primary exception。
```

register_type/create_topic/create_datareader/create_datawriter exception：
同样先提交 registration/create evidence + backing ownership，
再 rethrow original exception。

### 10.17 Fast DDS Error/Status Baseline Tests

每个 evidence shortcut 必须有 Fast DDS 2.6.12 frozen test。

至少：
detach PRECONDITION semantic
delete endpoint ALREADY_DELETED semantic（如果启用 KnownDeleted shortcut）
delete_contained_entities partial failure
set_listener failure
attach exception-after-call
detach exception-after-call
register_type ReturnCode no-registration evidence
register_type exception-after-call -> Indeterminate
create_datareader nullptr no-side-effect evidence
create_datawriter nullptr no-side-effect evidence
create_topic nullptr/no-side-effect evidence
create_datareader/create_datawriter/create_topic exception-after-call hidden-entity path
Subscriber/Publisher delete_contained_entities success as hidden endpoint evidence barrier
DomainParticipant delete_contained_entities success as Participant child/Topic evidence barrier
create_participant nullptr no-side-effect assumption（若实现启用该 shortcut）
create_participant exception/no-handle partial Context quarantine
Participant delete failure

如果测试无法稳定证明某个 shortcut：
implementation 必须退回更保守的 Indeterminate，
不能为了 cleanup rate 放宽 memory-safety evidence。

<a id="fastdds-verification"></a>

## 11. Verification Matrix 与 Frozen Invariants

本章只定义实现顺序、可观察测试场景和审查索引。第 2～10 章是 Fast DDS 行为的 normative authority；测试描述和 Frozen checklist 不得重新定义正文语义。测试应优先引用[稳定 invariant ID](#fastdds-stable-invariant-ids)，并链接到相应正文。

| 验证区域 | Stable invariant IDs |
| --- | --- |
| Context、Factory 与 shutdown | `CTX-*`、`NAME-*` |
| Binding、Type 与 Topic | `TYPE-*`、`TOPIC-*` |
| Discovery 与 target reader | `DISC-*`、`TARGET-*` |
| Listener 与 WaitSet | `LIST-*`、`WS-*` |
| Guard 与 Event | `GUARD-*`、`EVENT-*` |
| Take 与 Service | `TAKE-*`、`SVC-*` |
| Retirement 与 quarantine | `RET-*`、`TERM-*` |
| 全局 closure | `AUDIT-001`、`PUB-*` |

### 11.1 开发阶段

Phase 1 — 上位 Contract Closure
直接审查并修改同版本 `dmw.md` 与 public headers；不得把旁路 sync/merge 文件作为长期 normative source。

Phase 1 **不再维护第二份文字清单**。唯一 checklist authority 是 [Public Contract Closure Requirement](#fastdds-public-closure) 中的 `PUB-001 ... PUB-015`。Phase 1 必须逐 ID closure；缺少任意一项都保持 Frozen Candidate。

合并后，本文只规定这些 public behavior 的 Fast DDS 实现，不再自行扩大 public guarantee。
Phase 2 — Process / Context
实现：
DmwProcessRuntime

lock-free monotonic IDs

ContextState

NodeState

OperationGuard

Factory commit

ChildRegistry

request-all/drain/ack-all shutdown
Phase 3 — Type / Topic / QoS
实现：
TemporarySample
SerializationScratch

TypeRegistry
TopicRegistry

dual TypeLease

explicit Fast DDS QoS

QoS golden baseline
Phase 4 — Endpoint backing
实现：
ListenerState
CallbackInFlightGuard

DataReaderInfo
DataWriterInfo
GuardConditionInfo

WaitSetHoldState

retirement nodes
Phase 5 — Discovery
实现：
DiscoveryListener

RemoteEndpointRegistry

ordinary matched count

ServiceMatchRegistry

TargetReaderObservationRegistry
Phase 6 — WaitSet
实现：
WaitableState
RegistrationState

ConditionInfo

WaitSetInfo

preallocation-before-attach

hold accounting

Poisoned WaitSet

private control Guard recovery

wait/remove/add
shutdown ack
Phase 7 — Guard / Event
实现：
Guard generation
counter renormalization
logical-only degradation

EventSource
per-Event cursors
LivelinessChanged model
overflow/degraded state
Phase 8 — Client / Server
实现：
dual DDS endpoints

exact/fallback correlation

TemporarySample request/response

CapacityReservation

Pending FSM

100ms response discovery

aggregate teardown
Phase 9 — Final Teardown
实现：
RetiredWaitSetRegistry

OrphanedEndpointRegistry

Indeterminate handles

container reconciliation

second Topic/Type retry

QuarantinedParticipantInfo

ProcessTerminalQuarantine
Phase 10 — Verification
执行：
Debug
Release

ASan
UBSan
TSan

failure injection

QoS golden

XML isolation

ROS 2 Topic interop

ROS 2 Service interop

<a id="fastdds-public-closure"></a>

#### 11.1.1 Public Contract Closure Requirement IDs / Frozen Preflight

本表是 **唯一 Frozen-closure public-contract checklist authority**。Phase 1、CI、附录 A 都只引用这些 ID，不再各自复制一套可能漂移的列表。

| ID | `dmw.md` / public header requirement |
|---|---|
| PUB-001 | `CompatibilityProfile` public ownership = Context-scoped immutable；`ContextOptions` 含 profile；Publisher/Subscriber/Client/Server options 无 profile override |
| PUB-002 | NativeDds / Ros2FastDdsHumble logical-to-DDS Topic/Service naming observable contract；public `name()` 保持 logical name |
| PUB-003 | Cross-context WaitSet add -> `InvalidArgument`，且 public argument priority 高于 Context state |
| PUB-004 | Event creation history / no pre-creation replay |
| PUB-005 | Event readiness level-triggered；WaitSet 不消费 Event cursor；successful Event::take 才消费 |
| PUB-006 | Event exhaustion semantics |
| PUB-007 | Service take pre/post-consumption output guarantee，包括 exception 后 valid/destructible basic guarantee |
| PUB-008 | Bounded take/filter liveness；call-start finite candidate budget |
| PUB-009 | MessageInfo middleware-neutral semantics |
| PUB-010 | Result / `std::bad_alloc` / other C++ exception boundary |
| PUB-011 | `SystemDefault` semantics |
| PUB-012 | `Ros2FastDdsHumble` compatibility scope / custom binding limitation |
| PUB-013 | WaitResult stale/snapshot token semantics |
| PUB-014 | Generic shutdown contract允许 Fast DDS V1 shutdown-success subset；final Fast DDS cleanup failure不 retroactively 改变 runtime shutdown result |
| PUB-015 | `~Context()` best-effort implicit shutdown；Context facade destruction提交 shutdown，即使 children 仍保活 implementation state |

Frozen preflight 必须：
1. 直接读取同 revision 的 `dmw.md` 与 public headers，逐项证明 `PUB-001...PUB-015` 已 closure；
2. 确认 `ContextOptions` / endpoint option C++ declarations 与 `dmw.md` 一致；
3. 确认不存在 staged/active 的旧 `dmw_public_contract_sync.md`；
4. merge patch 若存在只能是 temporary non-normative review aid；正式 merge 后删除或移入明确 historical archive；
5. CI/review 不能只检查 merge patch；
6. 任意 PUB ID 未满足 -> status 必须保持 `V1 Implementation Frozen Candidate`。

#### 11.1.2 Fast DDS Implementation Global Closure Gate

每次修改 `dmw_fastdds.md` 后，在交付下一版 Frozen Candidate/Frozen review 前必须重新生成并审查以下六张全局表；不能只回归本轮修改章节。

1. **Lock-edge graph**
   - 枚举所有 `mutex A -> mutex B` 嵌套边；
   - 验证 rank严格单调或命中明确 same-rank/WaitSet例外；
   - 扫描 callback、rollback、destructor、registry acquire/release；
   - 当前 V1 特别断言：
     - no Topic(4) -> Type(3)；
     - no Target(8) -> Participant(5)/Remote(6)；
     - no Pending(16) + Target(8) overlap；
     - WaitSet add only topology(12) -> waitable(15)；
     - discovery唯一 cross-registry nested edge是 Remote(6) -> Service(7)；
     - Fast DDS/private control wake时不持有其它 DMW mutex。

2. **Public operation error-stage table**
   - 对每个 public API展开：
     `argument -> Context -> parent -> object-local -> middleware`；
   - 标注每个 heap allocation/Fast DDS API call发生在哪个 stage之后；
   - 任一可能抛异常的 operation若提前于高优先级 public error，review fail。

3. **FSM transition completeness table**
   - `RuntimeState × ShutdownExecutionState`；
   - Type/Topic Registry；
   - Remote/Participant/Target discovery；
   - Pending/Responding；
   - WaitSet/Registration/ControlGuard/Event；
   - 对 success/Error/bad_alloc/other exception/shutdown race逐项确认唯一 transition；
   - 不允许“executor退出但 state仍等待它”的非 terminal状态。

4. **Authority / ownership / evidence table**
   - 每个 logical fact只能有一个 authority；
   - Participant tombstone authority唯一是 `ParticipantObservationRegistryState`；
   - 每个 DDS entity pointer列 owner、create evidence、delete evidence、terminal barrier；
   - duplicate cache必须标明 source authority、更新方向和 partial-failure degradation。

5. **CV / wake handshake audit**
   - 每个 `condition_variable` 列 predicate authority、wait mutex、state publisher、notify path；
   - atomic predicate也不能用裸 notify替代共同mutex/generation handshake；
   - 当前特别检查 Child ack、Registration drain、Target participant/shutdown dependency wake。

6. **Throw-point / rollback table**
   - 枚举每个 allocation和可能抛 C++ exception的 Fast DDS/TypeSupport call；
   - 记录 throw前已提交状态、RAII owner、rollback、exception/result channel；
   - 特别检查“logical transition -> guard arm”之间不存在 throwing gap。

此外执行 mechanical scan：
- duplicate heading/ID；
- stale section references；
- old field/state names；
- contradictory Frozen invariant/test wording；
- code fence pairing；
- trailing whitespace。

任何一项失败：
本文状态必须保持 `V1 Implementation Frozen Candidate`，并且不得宣称该 revision 已通过 implementation closure review。

当前 revision 的逐表审查证据记录在
[`dmw_fastdds_closure_audit.md`](dmw_fastdds_closure_audit.md)。audit 通过 source SHA-256
绑定到本文与 `dmw.md`；digest 不匹配时记录自动失效，必须重新生成全部六张表。

### 11.2 Operation Gate Tests

必须覆盖：
acquire before shutdown linearization

acquire after shutdown linearization

many in-flight operations

shutdown waits operations

Factory Fast DDS entity creation success
then shutdown commit race

rollback failure
does not overwrite ContextShutdown

#### 11.2.1 Context Shutdown Executor / Implicit Shutdown Tests

Public lifetime-legal tests：
- destroy Context facade while Node/Publisher/Subscriber/Client/Server/WaitSet still strong-own `ContextState`；
- facade destructor commits `Active -> ShuttingDown` before releasing its strong state owner；
- surviving child operation -> `ContextShutdown`，never observes Active again；
- explicit `shutdown()` completed under legal synchronization, then facade destruction -> no second executor/no duplicate request-all；
- last `ContextState` owner destruction with execution `Idle` -> defensive `shutdown_noexcept()`；
- last owner sees `Completed` -> normal final teardown；
- last owner sees `Failed` -> no retry of partial phases, terminal retention/quarantine。

**禁止**测试“同一个 public Context object 的 `shutdown()` 与其 destructor 无同步并发”；该场景违反 public facade lifetime contract并可能构成 C++ UB。

Internal state-machine concurrency tests：
- two legal internal holders concurrently enter `ContextState::shutdown()` / `shutdown_noexcept()` -> exactly one `Idle -> Running` executor；
- waiter sees Running and waits；
- executor success -> `Completed + RuntimeState::Shutdown`，all waiters wake；
- inject unexpected exception after each Phase A/B/C/D boundary -> executor commits `Failed + stored exception_ptr + notify_all`；
- executor returns/throws after Failed -> no waiter remains blocked on a vanished Running executor；
- later explicit shutdown observes/rethrows same stored exception；
- later noexcept shutdown observes Failed, does not retry, does not throw；
- Phase B instrumentation证明 B1 在 ChildRegistry 下只发布全部 logical request，且不调用
  `request_shutdown_signal_only()`；
- B2 每次 private control wake / Target dependency signal 时
  ChildRegistry/runtime/InternalChildState/WaitSet/Target mutex均未持有；
- child 在 B1 后、B2 snapshot 前 unlink时必须已 ack generation 1，shutdown不漏等也不访问
  stale child；
- state `ShuttingDown + Idle` and `Shutdown + Running/Failed` are asserted impossible。

#### 11.2.2 Name Resolution Error-priority Tests

- immutable profile snapshot before Phase A performs no allocation/no lock/no failure；
- Phase A uses that exact snapshot for profile-specific DDS length checks；
- invalid logical name detectable by allocation-free validation + Context Shutdown -> `InvalidArgument` wins；
- valid logical name + Context Shutdown + injected resolver allocation OOM -> `ContextShutdown`, resolver allocation is never reached；
- valid name + Active Context + parent destroyed -> parent error before allocating resolved names；
- only after Context/parent/object-local checks pass may normalized/resolved `std::string` construction throw `std::bad_alloc`；
- `NameValidationPlan` itself performs no heap allocation under allocation instrumentation；
- no later Phase B re-read may select a different profile.

### 11.3 Child Shutdown Tests

many active WaitSets

many ephemeral waits

all children receive request
before shutdown waits first ack

idle WaitSet immediate ack

active WaitSet ack only after wait exits

private control trigger failure

100ms finite-slice fallback

child unregister vs shutdown request

#### 11.3.1 Child CV / executor-loss scheduling tests

- ack commit immediately before waiter wait -> no lost wake；
- waiter holds child mutex first -> publisher waits until cv atomically releases mutex, then publish+notify；
- publish_ack_noexcept mutex/system failure -> no exception escape；bounded Phase-D recheck observes atomic ack or executor exception enters Failed；
- child unregister publishes pending ack only after releasing high-rank child state locks；
- instrumentation rejects ChildRegistry+InternalChildState mutex overlap。

### 11.4 Listener Tests

callback active during destructor

set_listener(nullptr) failure

late callback after accepting=false

callback between first drain
and Fast DDS entity deletion

callback entered before endpoint delete success
returns after delete success
second drain retains listener until callback exits

Fast DDS entity deletion success 后禁止新的 callback entry（vendor assumption contract test / baseline audit）

Fast DDS entity deletion failure

Fast DDS entity deletion success

second drain
要求：
no listener UAF
no callback exception escape
no premature release of any Info object

#### 11.4.1 Listener drain CV tests

- callback decrement and drain predicate share ListenerState mutex；
- accepting=false commits under same mutex；
- forced notify-before-wait scheduling cannot hang；
- injected mutex/CV failure in noexcept teardown -> no zero evidence, listener/backing retained。

### 11.5 Registry / Lock-order Tests

TypeRegistry：
- Creating waiter；
- creation success；
- creation failure + waiter retry；
- Retiring waiter；
- retire success + recreate；
- retire failure -> Orphaned；
- Orphaned acquire -> DdsError；
- Fast DDS API calls outside Registry mutex。

TopicRegistry three-stage transaction：
- Absent insert Creating(tx) under Topic lock；
- unlock Topic before TypeLease acquire；
- instrumentation asserts **TypeRegistry mutex is never acquired while TopicRegistry mutex is held**；
- TypeLease acquire Error -> relock same tx, erase placeholder, notify, release outside lock；
- TypeLease acquire exception -> same rollback then rethrow；
- concurrent waiter observes Creating and does not run a second Type acquire/create_topic；
- Stage C tx mismatch -> Topic capability Degraded + local TypeLease released outside Topic lock；
- create_topic hidden-side-effect exception -> Orphaned retains engaged TypeLease；
- creation transaction ID never reuse/wrap；exhausted absent acquire -> ResourceExhausted；
- TopicEntry erase/move TypeLease under Topic mutex, TypeLease destruction/release only after unlock；
- generated lock-edge graph contains no `TopicRegistry(4) -> TypeRegistry(3)` edge。

Participant/Remote authority：
- only ParticipantObservationRegistry owns ParticipantObservationTable；
- Remote/Target contain no second participant tombstone table；
- participant remove absent -> canonical Removed tombstone materialized；
- Participant registry commit happens before dependent commits and rank 5 is released before
  Remote/Service/Target；Target is entered only after rank 6/7 are released；
- callback instrumentation observes registry commit order
  `Participant(5) -> Remote(6) -> Service(7) -> Target(8)`；唯一允许的 cross-registry
  overlap 是正文 5.14.2 冻结的 `Remote(6) -> Service(7)`，不得观察其它 nested edge；
- participant commit failure -> participant capability Degraded + Target CV notify；
- Remote/Target partial failure after participant Removed never rolls participant lifecycle back；
- Remote absent remove -> Removed tombstone；
- Removed GUID + later add while participant Active -> Remote capability Degraded；
- participant Removed + late endpoint add -> late no-op/diagnostic；
- ordinary get-or-create absent + participant remove race线性化为 Active->Removed或直接返回
  existing Removed，不创建第二 entry、不 resurrection；
- ordinary get-or-create allocation failure保持 table/generation不变；若发生在 sample consumption
  后，public take遵守 7.16 basic output guarantee；
- remote tombstone Context-lifetime retention。

Target dependency wake：
- participant Removed commits between precheck and Target mutex -> under-mutex recheck sees Removed, no sleep；
- waiter holds Target mutex first -> notifier advances dependency_generation only after cv releases mutex, then notify；
- shutdown uses same bridge；
- signal-helper failure cannot extend send_response beyond absolute100ms；
- no Target(8)->Participant(5)/Remote(6)/Service(7) lock edge。

### 11.6 TemporarySample Tests

createData success

createData failure

heap bad_alloc

invalid_data then valid_data

all invalid then NoData

serialize failure

payload allocation bad_alloc

deserialize failure
验证 output contract。

### 11.7 Reader Hold Tests

并发：
generation tries to acquire hold

Reader destructor closes hold gate
必须保证：
either generation gets a valid hold

or acquire fails
不存在：
generation obtains Condition pointer
after Reader is allowed to delete
TSan required。

### 11.8 Reader Delete Barrier

场景：
historical unresolved WaitSet
+
Subscriber destructor
必须断言：
delete_datareader NOT CALLED
直到：
all historical holds released

### 11.9 WaitSetInfo Transaction

Fault inject：
before allocation

during reserve

before first attach

control attach

first Reader attach

middle Reader attach

last Reader attach

exception after Fast DDS API call

topology change after attach

topology_generation reaches UINT64_MAX
next public add -> ResourceExhausted + no mutation
mandatory remove/internal mutation at exhaustion -> logical detach + Poisoned
no wrap to zero

rollback detach failure
任何：
successful or possibly-successful Fast DDS attach
必须始终拥有对应 backing。

### 11.10 Poisoned WaitSet

detach permanently fails

WaitSet -> Poisoned

future wait -> DdsError

future add -> DdsError

remove still completes logically

no new WaitSetInfo

unresolved WaitSetInfo count
does not grow indefinitely

#### 11.10.1 Waitable Auto-detach / add Error-priority Races

Auto-detach：
- WaitSet destructor vs Waitable destructor；
- Event parent destructor vs Event destructor；
- Event parent fan-out先对全部 live Event commit `ParentDestroyed`，再开始任一 logical detach；
- parent fan-out使用 pre-existing stable registration ID逐项遍历，不分配 snapshot vector；
- Attached -> Detaching CAS exactly one winner；
- no duplicate topology generation increment；
- no duplicate hold release；
- no waitable -> topology lock inversion；
- historical unresolved WaitSetInfo may retain ConditionInfo and endpoint Info after public registration Detached。

`WaitSet::add()` priority：
- cross-context + Context Shutdown -> `InvalidArgument`；
- Event parent destroyed + WaitSet Poisoned -> `ParentDestroyed`，not DdsError；
- already-registered waitable + WaitSet Poisoned -> `AlreadyRegistered`；
- clean waitable + Poisoned WaitSet -> `DdsError`；
- `WaitableCloseReason::ParentDestroyed` is committed before parent-driven auto-detach；
- add/parent-destroy race linearizes either successful registration followed by detach, or ParentDestroyed；never returns lower-priority Poisoned while parent destruction was already committed；
- lock instrumentation confirms add acquires topology(rank 12) -> waitable(rank 15) only and never obtains lower-rank parent mutex while holding them；
- Fast DDS control wake instrumentation confirms EventSource/topology/registration/waitable mutex均已释放；
- injected `RegistrationState` / registration-table-node allocation `bad_alloc` -> no registration ID consumed, no topology/Waitable mutation；
- after registration ID commit, table insertion + bidirectional bind are instrumented no-allocation/noexcept；
- final token `UINT64_MAX` can commit once; next add returns `ResourceExhausted` without wrap/reuse。

### 11.11 Control Guard Failure

测试：
set true failure
    -> logical control generation retained

set false failure
    -> control guard Broken

control_generation reaches UINT64_MAX
    -> no wrap
    -> ControlGuardInfo Broken
    -> logical mutation retained

replace control guard

old control detach success

old control detach failure
    -> WaitSet Poisoned
特别验证：
stuck Fast DDS true
不会导致无限 CPU busy loop

### 11.12 Guard Tests

trigger before registration

multiple trigger coalescing

trigger/consume race

Fast DDS true ReturnCode failure
    -> trigger() success
    -> logical pending retained
    -> LogicalOnlyDegraded

Fast DDS true C++ exception
    -> caught post-commit
    -> trigger() success
    -> logical pending retained
    -> LogicalOnlyDegraded

Fast DDS false failure/exception
    -> consumed logical state retained
    -> LogicalOnlyDegraded

future trigger while LogicalOnlyDegraded
    -> logical commit
    -> skip public Fast DDS Guard wake
    -> best-effort private control wake
    -> success

LogicalOnlyDegraded

MAX generation pending

MAX generation consumed
then renormalize

old WaitSet generation
outlives public Guard facade

### 11.13 Event Tests

event update
    -> wait returns Event token
    -> Event::take returns Taken + correct EventInfo

event update
    -> wait returns Event token
    -> wait again before take remains Ready
    -> cursor unchanged until successful Event::take

event source change
before Event creation

Event creation
then no readiness

source change after creation
then readiness

same type Event A/B

A take does not consume B

repeated Event create/destroy
    -> stale weak registration does not grow without bound

LivelinessChanged positive changes

negative changes

overflow

Degraded Event

parent destruction

### 11.14 Service Match Tests

request side only

response side only

same remote Participant pair

cross Participant false pairing

remote endpoint removal

participant removal

unexpected matched delta
    -> affected local entry rebuild_state = NeedsRebuild
    -> unrelated entry remains unchanged
    -> callback 不执行 Fast DDS discovery enumeration

service_is_available triggers affected-entry rebuild
rebuild success
    -> entry Clean; global registry capability remains Healthy

rebuild concurrent match_generation change
    -> stale rebuild discarded/retried

remote endpoint temporarily absent from RemoteEndpointRegistry
    -> affected entry remains NeedsRebuild
    -> availability false

Fast DDS matched enumeration failure/Unsupported
    -> exact graph Degraded

fast-path set allocation failure in callback
    -> affected entry NeedsRebuild

rebuild heap allocation failure
    -> std::bad_alloc propagates
    -> affected entry remains NeedsRebuild

Graph corruption / impossible identity conversion：
service availability -> DdsError
corresponding RemoteEndpointRegistry/ServiceMatchRegistry global capability -> Degraded
普通 matched query可以保持独立。

### 11.15 Target Reader History

response Reader discovered

response Reader removed

request later taken

PendingEntry sees Removed
以及：
KnownUnmatched -> Matched

Removed + late callback

ParticipantRemoved + late callback
Terminal 不回退。

### 11.16 Service Correlation

Exact mode
related response Reader GUID

matched
    -> write

Removed
    -> success no write
Fallback mode
related unknown

RequestId contains request Writer GUID

must not exact-query response Reader

same Participant response Reader matched
    -> write

request Writer removed
    -> still not terminal

ParticipantRemoved
    -> success no write

### 11.17 Pending Capacity

many concurrent take_request()

capacity exact maximum

one extra
    -> ResourceExhausted
    -> no Fast DDS take

duplicate request
    -> releases reservation
    -> must reserve again

shutdown during reservation
    -> no counter underflow

capacity full + unread_count == 0
    -> ResourceExhausted
    -> no unread-count query / no Fast DDS take
filtered request needs next scan iteration but concurrent capacity becomes full
    -> re-reserve fails ResourceExhausted
    -> no reservation leak
PendingEntry rollback guard preconstruction
    -> disarmed constructor is no-allocation/noexcept before map insertion
PendingEntry insertion bad_alloc
    -> guard remains disarmed; reservation released; no pending leak; outputs unchanged; exception propagates
duplicate-key insertion race after sample consumption
    -> no second PendingEntry; current reservation released/re-reserved according to bounded duplicate filtering; no counter leak
map insertion + reservation conversion + rollback.arm()
    -> arm is noexcept before unlock; no throwing gap exists between bookkeeping commit and rollback ownership
payload commit deserialize false
    -> PendingEntryRollbackGuard erases pending; DdsError
payload commit bad_alloc
    -> rollback; exception propagates; no capacity leak
payload commit non-bad exception
    -> rollback; original exception propagates

#### 11.17.1 Bounded Take / Filter Liveness Tests

Subscriber invalid-data stream concurrent producer：
    -> one take scans no more than initial unread snapshot budget
    -> returns NoData when budget exhausted
Client foreign-response stream：
    -> bounded; no infinite call
Server duplicate-request stream：
    -> bounded; every consumed duplicate releases reservation and decrements scan budget
Samples arriving after call-start snapshot：
    -> do not extend current scan budget
    -> visible to later take call
get_unread_count exception/invariant failure：
    -> exception/DdsError according to 6.11.1

### 11.18 MessageInfo Tests

覆盖：
source_timestamp

reception_timestamp

sample writer GUID

publication_handle fallback

publication sequence

unknown publication sequence

reception sequence == nullopt

#### 11.18.1 Binding / Identity Tests

MessageTypeAccess::create：
empty wire name -> InvalidArgument
getName non-bad_alloc exception -> propagates unchanged
allocation bad_alloc -> propagates
wire name copied once and remains immutable
wrapper PubSubTypeT provides distinct BindingIdentity from base type
DSO unload while binding alive is documented invalid deployment

TemporarySample move：
Result<TemporarySample> return/move works in C++17
moved-from destructor no-op
move assignment safely releases/quarantines old backing before ownership transfer

Canonical binding capability：
first descriptor for absent TypeEntry becomes canonical
same MessageType cheap-copies in one Context acquire same CanonicalTypeBinding
independent make_message_type() descriptors with same wire name + BindingIdentity in same Context
    -> acquire same TypeEntry
    -> receive same canonical binding
    -> caller candidate is not used for endpoint serialization after acquire
first descriptor facade destroyed
    -> TypeEntry still strong-owns canonical MessageType::Impl / TypeSupport
contract violation through endpoint created from descriptor A
    -> canonical capability -> Degraded
endpoint/factory using independent descriptor B with same key
    -> observes same Degraded capability
    -> cannot bypass degradation by reacquire
different Contexts
    -> independent TypeEntry / canonical capability scope
same BindingIdentity but deliberately non-substitutable custom descriptor
    -> documented integration contract violation; conformance binding must not rely on descriptor-instance behavior

deleteData exception
    -> destructor does not throw
    -> CanonicalTypeBinding capability -> Degraded
    -> AllocatedSample adopted by ProcessBindingQuarantine
    -> data pointer / canonical MessageType::Impl remain alive
    -> later binding-dependent runtime operation returns DdsError after higher-priority checks

createData nullptr
createData non-bad_alloc exception
    -> propagates unchanged
createData bad_alloc
    -> propagates std::bad_alloc
serialize false
    -> DdsError
serialize non-bad_alloc exception
    -> propagates unchanged
deserialize false
    -> DdsError + caller valid/destructible
deserialize non-bad_alloc exception
    -> exception propagates + caller valid/destructible
deserialize bad_alloc
    -> std::bad_alloc propagates + caller valid/destructible
size provider non-bad_alloc exception
    -> propagates unchanged
size provider overflow/inconsistent size
    -> DdsError

`TypeRegistrationStatus`：
register_type OK
    -> Registered + Active canonical TypeEntry
register_type frozen no-registration ReturnCode
    -> NotRegistered + Creating erased
register_type ambiguous failure
    -> Indeterminate + Orphaned + canonical TypeSupport retained
register_type exception after entered Fast DDS API call
    -> Indeterminate + Orphaned committed before exception rethrow
reacquire same key while Orphaned
    -> DdsError; no second Fast DDS registration
unregister_type exception
    -> Orphaned retention; no TypeSupport release
Participant delete success
    -> remaining orphaned registration backing may finally release

Fast DDS endpoint/topic create evidence：
create_datareader valid handle -> HandleKnown
create_datawriter valid handle -> HandleKnown
create_topic valid handle -> HandleKnown
nullptr with frozen no-side-effect evidence -> NoSideEffect rollback
ambiguous nullptr/failure -> SideEffectIndeterminate + parent graph MayContainHiddenEntity
create_datareader exception after entered Fast DDS API call
    -> orphan backing retains listener/TopicLease/TypeLease
    -> Subscriber graph MayContainHiddenEntity
    -> original exception rethrown after adoption
create_datawriter exception -> symmetric Publisher behavior
create_topic exception
    -> TopicEntry Orphaned with null handle
    -> TypeLease retained
    -> Participant graph MayContainHiddenEntity
    -> original exception rethrown
Client/Server second-child create exception after first child success
    -> no public commit
    -> known child rollback attempted
    -> hidden child/container evidence retained

Root create evidence：
create_participant exception after entered Fast DDS API call and no handle
    -> partial Context objects adopted to preallocated TerminalContextNode
    -> ProcessTerminalQuarantine
    -> discovery listener/registry remain process-lifetime safe
    -> original exception rethrown
Context Publisher/Subscriber create exception after Participant known
    -> best-effort contained cleanup
    -> unresolved evidence quarantines entire partial Context before returning/rethrowing

Contained graph barrier：
Subscriber hidden reader + delete_contained_entities success
    -> hidden DataReaderInfo safe to release
Publisher hidden writer + delete_contained_entities success
    -> hidden DataWriterInfo safe to release
Participant hidden Topic + participant.delete_contained_entities success
    -> hidden TopicEntry safe to release; no later stale delete_topic
container cleanup failure/exception
    -> no per-child KnownDeleted inference
    -> final Participant success or terminal quarantine required

SequenceNumber RequestId：
positive high/low
negative high bit-pattern
0
UINT64 boundary patterns
unknown sentinel
round-trip bit equality
no signed shift UB
compile-time high/low/RequestId size assertions

MessageInfo publication sequence：
unknown sentinel -> nullopt
zero non-unknown pattern -> 0
positive high/low -> exact uint64 bits
high < 0 non-unknown -> exact bit-preserving uint64
helper does not use RequestId signed numeric interpretation

GUID：
16-byte round-trip
invalid fallback

Timestamp：
zero
finite
negative invalid
overflow -> 0

ProcessBindingQuarantine concurrency：
multiple deleteData contract violations from different threads
no allocation in adoption
no double delete
quarantine storage is not destroyed by ordinary static teardown

#### 11.18.2 WaitSet Remove / Snapshot Tests

Fast DDS WaitSet wait returns ready condition
forms/revalidates Ready snapshot
active_wait_count -> 0
concurrent remove() returns success
wait() subsequently returns snapshot containing old token
    -> allowed by dmw.md stale snapshot semantics
    -> no UAF / no RegistrationState access after ref release

remove() while Fast DDS Condition interpretation still needs RegistrationState
    -> waits until active_wait_count == 0
    -> lifetime-safe completion

#### 11.18.3 Resolved DDS Naming / Profile API Tests

NativeDds logical `/a/b` -> `dmw/t/a/b`
NativeDds service `/srv` -> `dmw/rq/srv` + `dmw/rr/srv`
Ros2FastDdsHumble logical `/a/b` -> `rt/a/b`
Ros2FastDdsHumble service `/srv` -> `rq/srvRequest` + `rr/srvReply`
public endpoint/service name remains logical FQN
ContextOptions owns CompatibilityProfile; endpoint/service options contain no profile override
same Context cannot mix CompatibilityProfile
TopicRegistry primary key uses resolved DDS name only
same resolved name + different wire type -> TypeMismatch
same name/type + fingerprint mismatch -> registry Degraded + DdsError

### 11.19 QoS Tests

每个 profile golden snapshot：
ParticipantQos
PublisherQos
SubscriberQos
TopicQos
DataWriterQos
DataReaderQos
覆盖：
History
Depth
ResourceLimits
HistoryMemoryPolicy

Reliability
Durability

Deadline
Lifespan

Liveliness
Lease
Announcement

PublicationMode

DataSharing
特别验证 KeepLast classification：
违反 dmw.md public depth contract -> InvalidArgument
若 dmw.md 允许某个 depth 但其 > INT32_MAX / 无法转 Fast DDS int32_t -> Unsupported
valid public QoS otherwise not representable by Fast DDS 2.6.12 -> Unsupported
resolver internal contradiction -> DdsError + invariant diagnostic
Fast DDS entity creation INCONSISTENT_POLICY -> IncompatibleQos

特别验证 Lifespan：
NativeDds DataWriter lifespan mapped
NativeDds DataReader lifespan mapped
Ros2FastDdsHumble DataWriter lifespan mapped
Ros2FastDdsHumble DataReader lifespan mapped
canonical TopicQos 不随 endpoint lifespan/deadline 或 creation order 改变
两个同名同类型 endpoint 使用不同 lifespan 时 Topic fingerprint 不 first-creator-wins
ROS 2 Humble 双向 interoperability / sample expiry observable behavior 仍通过

#### 11.19.1 ServiceKey / TopicQosFingerprint Identity Tests

ServiceKey：
- equality/hash authority exactly four fields: request/response DDS name + request/response wire type；
- logical name/profile differences with same authority identity -> invariant violation, not distinct registry key；
- equal keys always same hash；
- raw-byte/padding/address hashing prohibited。

TopicQosFingerprint：
- field-by-field canonical equality covers exact 13 TopicQos policies frozen in 4.18；
- includes `topic_data` and `durability_service`；
- duration infinite and resource unlimited normalize to unique semantic sentinels；
- semantic equality unaffected by struct padding/object address；
- forced hash collision still performs semantic equality and does not merge unequal QoS。

### 11.20 XML Isolation Tests

先让外部代码：
加载 Fast DDS XML profiles
再创建 DMW entity。
验证：
effective DMW QoS
与无 XML 时相同。

### 11.21 Final Teardown Tests

Fault inject：
endpoint delete failure

delete_contained_entities partial failure

DDS Subscriber delete failure/success

DDS Publisher delete failure/success

hidden DataReader create + Subscriber contained cleanup success/failure

hidden DataWriter create + Publisher contained cleanup success/failure

hidden Topic create + Participant delete_contained_entities success/failure

container cleanup success 后不再 individual-delete stale hidden/known child pointer

container cleanup failure 不伪造 per-child KnownDeleted evidence

Topic delete success -> KnownDeleted + topic=null
Topic delete baseline-proven alive failure -> KnownAlive + conditional retry allowed
Topic delete ambiguous failure/exception -> Indeterminate + no second individual delete
Participant contained barrier success -> publisher/subscriber/Topic handle proofs KnownDeleted + pointers null；Reader/Writer state Deleted + pointers null
Participant barrier success -> second Topic cleanup skipped; no stale child delete

first Topic delete baseline-proven target-alive failure -> KnownAlive

second Topic delete retry -> success -> KnownDeleted

Type cleanup retry

register_type Indeterminate survives until unregister/container/Participant lifetime barrier

create_participant exception with no handle
    -> partial TerminalContextNode adoption
    -> discovery listener/Registry objects retained

Context Publisher/Subscriber create exception after Participant handle known
    -> cleanup success releases partial Context
    -> cleanup/Participant delete uncertainty quarantines partial Context

Participant failure

Participant success

terminal quarantine adoption uses preallocated/no-allocation path
ProcessTerminalQuarantine is not destroyed by ordinary C++ static teardown
必须验证：
no double delete

no dangling listener

no dangling TypeSupport

no Reader deletion while old WaitSet holds Condition

#### 11.21.1 Terminal Quarantine Concurrency

并发创建多个 Context，
fault inject Participant permanent delete failure，
同时进入 terminal adoption。

验证：
quarantine singleton 在 first Participant 前已初始化
adopt path no allocation
intrusive list integrity
quarantine mutex serialization
adopt while holding any DMW mutex -> debug assertion
TerminalContextNode unique adoption
no static/atexit destruction
fixed diagnostic counters thread-safe

#### 11.21.2 Final Teardown / Container / Topic Status Tests

last ContextState owner released from user thread -> exactly one NotStarted->Running winner
retirement retry concurrent with final owner release -> no second final teardown
listener callback cannot become last ContextState strong owner
publisher/subscriber delete failure -> EntityStatus Indeterminate
participant barrier is invoked for publisher/subscriber graph uncertainty and endpoint Indeterminate handles
ProcessTerminalQuarantine mutex lock injected exception -> no terminate; released node intentionally leaked

#### 11.21.3 Discovery / Participant / TargetReader FSM Tests

Participant authority：
- exactly one `ParticipantObservationRegistryState` owns ParticipantObservationTable；
- participant remove before any endpoint/TargetReaderKey exists -> canonical Removed tombstone；
- later target lookup through stable participant handle -> effective ParticipantRemoved；
- participant with no compatible response reader removed -> fallback terminal no-write；
- Participant registry allocation/generation failure -> capability Degraded + Target CV notify；
- no Target predicate takes Participant/Remote lower-rank mutex；
- Participant Removed commit remains terminal even if later Remote/Target update fails。

Remote endpoint FSM：
- duplicate remote add identical identity -> idempotent；
- remove-before-add absent GUID -> materialize Removed tombstone；
- duplicate remove -> idempotent；
- Removed GUID + later add while participant Active -> Remote capability Degraded；
- participant Removed + late add/change/remove -> late no-op/diagnostic；
- impossible immutable identity mutation -> Remote capability Degraded；
- endpoint tombstones retained Context lifetime。

Service match：
- CHANGED_QOS with stable identity increments registry generation and dirties active local entries；
- one local entry NeedsRebuild does not dirty unrelated entry except intentional remote-registry dependency update；
- local endpoint Closing/Removed + late callback/rebuild -> discarded；
- remote/service registry generation change during rebuild -> stale result discarded；
- callback allocation failure -> affected capability Degraded, no exception escape；
- `match_generation == UINT64_MAX` + new mutation -> global ServiceMatchRegistry capability Degraded；exact operation -> DdsError。

TargetReader：
- PendingEntry strong-owns immutable `TargetReaderKey` carrying stable participant observation handle；
- Exact target NeverObserved -> Matched wakes predicate wait without lost wake；
- Exact target Matched -> exact Removed -> no-write success；
- Fallback response set empty/non-empty transitions KnownUnmatched/Matched；
- request Writer removal alone is not terminal；
- shared participant lifecycle Removed is terminal for exact/fallback；
- Target generation exhaustion -> Target capability Degraded；
- late matched/discovery callback after participant tombstone -> no resurrection；
- GuidPrefix reuse remains documented V1 deployment constraint, not claimed as test-proven vendor guarantee。

#### 11.21.4 Server send_response Transaction / Error-priority Tests

Phase A priority：
- unknown RequestId + injected EphemeralWait preallocation OOM -> `NotFound`；
- already Responding RequestId + injected preallocation OOM -> `Busy`；
- first lookup Pending -> snapshot handle -> unlock；
- preallocation failure -> Pending remains Pending, exception channel preserved。

Two-phase claim：
- two concurrent callers snapshot same Pending handle；
- first Phase C claims `Pending -> Responding`；
- second Phase C sees same handle Responding -> `Busy`；
- first sender terminal success erases map while second still holds snapshot handle -> second remains `Busy`, never sends again；
- same RequestId map replacement is treated as invariant failure, not silent new claim。

Rollback/locks：
- `Pending -> Responding` uses only noexcept operations and arms rollback before unlock；
- child registration loses shutdown race -> Responding rollback/erase according to registry state；
- PendingRegistry mutex(rank 16) and TargetReader mutex(rank 8) are never held together；
- target predicate never obtains Participant(rank 5) or Remote(rank 6) mutex；
- target Timeout/Degraded/ContextShutdown -> Active registry restores Pending；Shutdown registry erases/no-reinsert；
- terminal no-write success and Fast DDS write success both use same Responding erase commit；
- Fast DDS write ReturnCode failure -> rollback then mapped Error；
- Fast DDS write `std::bad_alloc` / other C++ exception -> bookkeeping rollback before original exception escapes；
- rollback/ephemeral unregister exactly-once；no pending/responding/reservation counter underflow。

#### 11.21.5 Control Guard Replacement Tests

replacement backing allocation bad_alloc -> propagates from wait, state returns Required, later retry allowed
Fast DDS replacement creation exception -> WaitSet Poisoned
replacement attach Indeterminate -> WaitSet Poisoned
topology changes during Building -> uncommitted build retired/retried
Required/Building still makes progress through bounded wait slice

### 11.22 Sanitizers

必须：
ASan
UBSan
TSan
TSan 重点：
Factory/shutdown

Node destroy/Factory

listener drain

matched callbacks

WaitSet hold gate

topology_generation

add/remove/wait

capacity transition

Pending request/response

### 11.23 ROS 2 Interoperability

Topic：
DMW Publisher
↔
ROS 2 Humble Subscriber

ROS 2 Humble Publisher
↔
DMW Subscriber
Service：
DMW Client
↔
ROS 2 Humble Server

ROS 2 Humble Client
↔
DMW Server
必须覆盖：
resolved DDS names

wire type name

CDR

Fast DDS QoS

SampleIdentity

related_sample_identity

RequestId

multi-client

response filtering

service availability

discovery race
测试 MessageType 必须来自：
frozen ROS-generated Fast DDS binding

### 11.24 V1 Frozen Invariants

本节是正文的压缩审查清单，不是第二份 normative definition。若清单与第 2～10 章冲突，以正文为准，并必须在同一 revision 修复清单。

Contract
dmw.md 是唯一 public normative specification。
dmw_fastdds.md 不扩大 public guarantees。
V1 不引入 WireCertification。
custom PubSubType ROS compatibility 不由 runtime 自动证明。
std::bad_alloc 不翻译成普通 DMW allocation error。
DDS/DDS resource ceiling 可以映射 ResourceExhausted。
Context
一个 Context 一个 Participant。
一个 Context 一个 DDS Publisher。
一个 Context 一个 DDS Subscriber。
Node 不对应 DDS entity。
Context 保存 domain ID。
ContextOptions 是 CompatibilityProfile 唯一 public owner；Context 保存 immutable profile；Publisher/Subscriber/Client/Server options 不含 profile override。
logical name 与 resolved DDS name 分离；Phase 0可无分配读取immutable profile，Phase A allocation-free validation使用该snapshot，Phase B在Context/parent/local检查后materialize resolved DDS name。
participant name 显式映射 Fast DDS QoS。
Factory
Factory 全事务化。
Fast DDS entity creation success 不是 commit。
Context state check 与 in-flight increment 同步。
shutdown linearization 后不允许新 runtime commit。
Node parent commit 检查 NodePhase。
Event parent commit 检查 EndpointPhase。
WaitSet commit 包含 ChildRegistry registration。
rollback cleanup failure不覆盖 primary Factory error。
Shutdown
Context facade/Impl destructor 先执行 implicit shutdown；ShutdownExecutionState exactly-one executor；executor exception -> Failed + stored exception + notify；Failed不重跑 partial phases；Completed后 final teardown由 FinalTeardownState CAS once执行，callback stack不得成为执行者。
shutdown 使用 request-all。
request-all 后 drain operations。
最后等待全部 child acknowledgement。
WaitSet ack 表示 active Fast DDS WaitSet wait 真正退出。
Child unregister 前偿还 pending shutdown request。
shutdown result 不包含 future final teardown failure。
Type / Topic
Type identity = wire name + BindingIdentity。
Type Registry 是 Context mandatory。
Topic Registry 是 Context mandatory；primary key = resolved DDS topic name only，wire type/fingerprint 是 name-exclusive invariant fields。
discovery-related RegistryState 由 ContextState shared_ptr strong-own，listener 只 weak-own。
Topic Fast DDS QoS 使用 canonical TopicQos，与 endpoint Qos 解耦。
同名同 wire type Topic 不允许 first-creator-wins QoS。
Topic 自己持 TypeLease。
Endpoint TypeLease 与 Topic TypeLease 独立。
Topic Acquire 使用 Creating(tx) placeholder -> unlock Topic -> TypeLease acquire -> relock same tx；Topic-owned TypeLease acquire/release永远发生在 TopicRegistry mutex外，禁止 Topic(4) -> Type(3)。
Registry Fast DDS API call不在 Registry mutex内。
register_type/create_topic/DDS endpoint create 在进入 Fast DDS API call 前必须已有 ownership record 和预分配异常接管路径。
entered Fast DDS API call 后 exception 必须先提交 Indeterminate/SideEffectIndeterminate evidence 与 backing ownership，再向外传播。
Type registration Indeterminate -> Orphaned 且 canonical TypeSupport retained。
TopicEntry 同时维护 CreationStatus + EntityStatus；Indeterminate Topic* 禁止再次 individual delete。
Topic/DataReader/DataWriter hidden-create evidence 通过 parent graph MayContainHiddenEntity 保留到 container-level barrier。
root Participant create 无 handle 且 SideEffectIndeterminate -> partial Context process terminal quarantine。
Context participant/publisher/subscriber 同时维护 create evidence 与 delete/handle evidence；create evidence 不替代删除后的 KnownAlive/KnownDeleted/Indeterminate state。
Creating/Retiring waiter重新 lookup。
Orphaned entry不重新复用。
Message binding / data
custom PubSubTypeT 必须满足 4.2.1 lifecycle/exception/thread-safety contract。
BindingCapabilityState = Healthy/Degraded，按 Context × canonical TypeEntry 共享且 degradation 单向。
同一 Context 内相同 wire name + BindingIdentity 只能有一个 canonical binding authority。
TypeLease acquire 后 endpoint/TemporarySample/TypeSupport hook 只使用 canonical binding，caller descriptor 不再是 runtime authority。
deleteData exception 不能逃出 noexcept destructor，并把 AllocatedSample adoption 到 ProcessBindingQuarantine。
ordinary non-bad_alloc C++ exception 不转换为 DdsError；完成必要 lifetime evidence 后原样传播。
deserialize false/bad_alloc/其它 exception 所有失败出口均保证 caller object valid/destructible。
RequestId SequenceNumber 与 MessageInfo publication sequence 使用不同 frozen helper。
SequenceNumber/GUID/timestamp 只通过 4.2.2 frozen helper 转换。
take 使用 implementation TemporarySample；TemporarySample C++17 move-only 且 moved-from destructor no-op。
NoData 不修改 output。
pre-take error不修改 output。
post-take conversion failure只保证 output 合法可析构。
invalid/foreign/duplicate sample filtering 受 call-start unread snapshot scan budget 限制，单次 public take 不可活锁。
temporary→user V1通过 CDR round-trip。
payload heap OOM传播 std::bad_alloc。
Listener
callback noexcept。
callback先进入 in-flight accounting。
accepting=false 后 callback no-op。
set_listener failure不释放 listener。
Fast DDS entity deletion success后 second drain。
delete failure/deferred保留 listener。
degraded state 不恢复。
listener不成为 Context 最后 strong owner。
endpoint Fast DDS entity deletion success 后不再允许新的 callback entry；second drain 清空 delete 前已经进入的 late callback。
Discovery
RemoteEndpointRegistry capability 仅 Healthy/Degraded；service NeedsRebuild 是 per-local-entry state。
Remote endpoint lifecycle Active/Removed/ParticipantRemoved terminal rules、registry generation 和 late-callback policy 已冻结。
ParticipantObservationRegistryState 是唯一 participant lifecycle/tombstone authority；RemoteEndpointRegistry/TargetReaderRegistry只保存 stable participant observation handle，不复制 tombstone table。
ContextState strong-owns discovery RegistryState；
DiscoveryListenerState 只保存 weak_ptr；
terminal transfer 后 strong ownership 由 QuarantinedParticipantInfo 保持。
actual Fast DDS matching 是 QoS compatibility authority。
ordinary matched count与 exact service graph分离。
Service pairing必须 same Participant。
unexpected matched delta 只把 affected local service entry 标记为 NeedsRebuild，不在 listener callback 中执行 Fast DDS discovery enumeration。
普通 runtime operation 在 callback stack 外按 entry 执行 Fast DDS matched-endpoint rebuild。
transient discovery ordering 让 affected entry 保持 NeedsRebuild 并保守返回 availability false；registry/Fast DDS discovery enumeration/identity correctness 无法可靠维护时 global capability 永久 Degraded。
TargetReaderObservationRegistry 具有 mutex/CV/capability/generation + exact/participant-response indexes；TargetReaderKey strong-own canonical ParticipantObservationEntry；PendingEntry使用 stable shared handle。
target predicate先 atomic读取 Participant authority/capability，再在 Target mutex下重读 target-specific live state；不获取 Participant/Remote低rank mutex；participant/target/shutdown commit后notify，避免 lost wake。
participant tombstone由canonical ParticipantObservationRegistry Context-lifetime保留，独立于任何 service-specific target entry；Target不复制/evict participant authority；ParticipantRemoved永久性依赖V1 GuidPrefix不复用 deployment constraint。
target history allocation failure使 capability Degraded。
Reader lifetime
DataReaderInfo 是 DataReader lifecycle authority。
WaitSet hold gate控制是否允许新 WaitSetInfo引用 Reader。
historical hold非零时绝不 delete DataReader。
Reader delete deferred直到 hold==0。
hold release本身不获取 Reader mutex。
deferred cleanup在 reconciliation lock外执行。
WaitSet
per-WaitSet registration ID allocator state 位于 WaitSetState，monotonic/never reuse/never wrap。
Waitable cross-lock 顺序固定 topology -> waitable；
waitable destructor 使用 two-phase auto-detach，禁止 waitable -> topology。
Attached -> Detaching 由 CAS 唯一 claim，WaitSet/waitable concurrent teardown 不重复 unlink。
topology generation 是 atomic，且 checked monotonic、never wrap。
wait_set_status / active_wait 是 atomic。
topology/reconciliation mutex不嵌套。
reconciliation_mutex 是 Fast DDS WaitSet serialization mutex，并且是第 10.4 Fast DDS API call unlock rule 的显式例外。
generation所有必要 allocation在 first attach前完成。
possible attach 必须已有 attachment record。
attach异常按 Indeterminate处理。
unresolved WaitSetInfo保留所有 ConditionInfo。
unresolved WaitSetInfo使 WaitSet永久 Poisoned。
Poisoned WaitSet不再创建 WaitSetInfo。
Poisoned wait返回DdsError；add在返回WaitSet local DdsError前必须先完成更高优先级waitable parent/registration检查。
Poisoned remove仍允许 logical detach。
old WaitSetInfo使用 frozen Condition→Registration mapping。
active_wait_count 只保证 Fast DDS Condition interpretation lifetime safety，不禁止 remove 返回后到达已形成的 stale WaitResult snapshot。
public Infinite由有限 Fast DDS slices组成。
finite deadline只计算一次。
public timeout前 final readiness precheck。
WaitResult OOM不消费 logical readiness。
Control Guard
private ControlGuardInfo 使用 checked generation change stamp，never wrap；logical authority 仍是 runtime/topology/readiness state。
Fast DDS GuardCondition trigger failure不回滚 logical state。
control generation exhaustion使当前 ControlGuardInfo Broken 并触发 replacement。
reset使用 post-reset recheck。
reset failure触发 replacement。
stuck control condition不能持续留在 blocking topology。
无法安全 detach broken control 则 WaitSet Poisoned。
Public Guard
Public Guard有 logical generation。
多次 pending trigger coalesce。
trigger counter不发生 unsigned wrap。
fully-consumed MAX 可 renormalize。
logical trigger commit 是 GuardCondition::trigger() success linearization。
Fast DDS wake ReturnCode failure/exception 保留 logical pending、退化到 LogicalOnlyDegraded，但 public trigger() 返回 success。
LogicalOnlyDegraded Guard不进入新 WaitSetInfo。
historical WaitSetInfo继续保活 GuardConditionInfo。
Event
EventSource 与 EventState 分离。
Event destroy 必须从 EventSource 移除自身 registration，stale weak entries 不允许正常无界增长。
同 EventType允许多个 Event。
Event cursor独立。
WaitSet 只报告 Event level readiness，不推进 Event cursor；只有 successful Event::take() 消费 event data。
Event创建时 cursor=current cumulative。
不 replay pre-creation history。
Liveliness gauge/change分开。
Event arithmetic checked。
Exhausted Event保持 ready。
Exhausted Event take返回 ResourceExhausted。
Exhausted cursor不推进。
Degraded Event take返回 DdsError。
ContextShutdown优先于 ParentDestroyed。
Service
NativeDds 与 Ros2FastDdsHumble 均使用本章 SampleIdentity correlation；profile-specific difference 必须显式列出。
Client response Reader先于 request Writer。
Service endpoint具有 matched listener。
PendingEntry保存 internal correlation kind + immutable shared TargetReaderKey；不保存 live observation snapshot。
fallback request Writer GUID不能作为 response Reader GUID。
Exact mode可根据 Reader Removed no-write success。
Fallback mode只有 ParticipantRemoved足以 no-write success。
Response deadline基于 steady_clock。
100ms deadline只计算一次。
CapacityReservation是 RAII。
Server take 先 reserve capacity 再 snapshot scan budget；capacity full 优先 ResourceExhausted，且不 Fast DDS take。
duplicate/invalid 后必须重新 reserve。
PendingEntry 成功后用 PendingEntryRollbackGuard 覆盖 payload commit；false->DdsError，bad_alloc/其它 C++ exception 原样传播且无 pending/capacity 泄漏。
EphemeralInterruptibleWait registration commit 与 shutdown linearization 互斥。
capacity两个方向都修改 WaitSet topology。
send_response先 Pending object-local lookup得到NotFound/Busy，再做preallocation；随后two-phase stable-handle revalidation claim；failure在Active registry恢复Pending，shutdown后erase/no-reinsert。
Context shutdown后 rollback不重新插入 Pending。
Client response canonicalizes public RequestId。
QoS
所有 DDS entities使用 explicit QoS。
canonical baseline = Fast DDS 2.6.12 QoS value/default constructor +本文 mandatory override；
禁止读取 factory/participant/XML default。
Participant built-in discovery reader/writer history memory 在 Ros2FastDdsHumble 下为 PREALLOCATED_WITH_REALLOC。
TopicQos 与 endpoint Qos 解耦并使用 canonical TopicQos，禁止 first-creator-wins。
SystemDefault由 profile + entity kind + frozen baseline决定。
ROS Topic default必须显式 ros2_default()。
Service default必须显式 ros2_services_default()。
Ros2FastDdsHumble publication synchronous。
Ros2FastDdsHumble history realloc。
Ros2FastDdsHumble Data Sharing OFF。
KeepLast resource limit deterministic，且 InvalidArgument/Unsupported/DdsError/IncompatibleQos 分类固定。
Reader/Writer lifespan 均映射；Ros2FastDdsHumble endpoint lifespan 跟 Humble baseline。
TopicQos lifespan/deadline 保持 canonical、endpoint-independent，不复制 reference first-creator-wins。
Liveliness duration conversion checked。
announcement period计算不发生乘法 overflow。
Retirement
Endpoint retirement adoption no-allocation。
retirement Info不强持 ContextState。
WaitSet unresolved WaitSetInfo阻止 Context Fast DDS teardown。
delete_contained failure产生 Indeterminate handles。
Indeterminate raw pointer不再 endpoint-level delete。
DDS Subscriber success证明 contained Readers gone。
DDS Publisher success证明 contained Writers gone。
container success后释放对应 endpoint leases。
Topic/Type cleanup具有条件化第二轮；Participant contained barrier success 后 prior child raw pointer全部 KnownDeleted/null，禁止任何第二轮 individual child delete。
Participant contained barrier success会废止所有 prior Publisher/Subscriber/Topic/Reader/Writer raw child pointer；Participant delete success是最终 contained-graph evidence。
Participant failure进入 process terminal quarantine。
terminal transfer no-allocation。
terminal quarantine intentionally process-lifetime。
ProcessTerminalQuarantine 本身 non-destructing，禁止 static destruction 释放 terminally backings。
ProcessTerminalQuarantine 具有独立 mutex + intrusive list，多个 Context 并发 adoption thread-safe；adoption使用 `std::unique_lock`，splice/counter update均 noexcept。
terminal node 在进入 noexcept adoption 前先从 Context ownership release；adoption mutex/bookkeeping 自身抛异常时 intentional raw leak，不 terminate。
terminal adoption 只允许在不持有其它 DMW mutex 时执行，且 adoption path 不分配内存。
ProcessBindingQuarantine 同样 process-lifetime/non-destructing/thread-safe/no-allocation，并复用 `std::unique_lock` + noexcept intrusive splice protocol；
其 noexcept adoption 在 lock acquisition 失败时允许 intentional raw leak，优先保证不析构状态未知的 sample allocation。
Lock / Error
Stable lock rank固定。
same-rank peer默认不得一起持有。
必要时使用 stable ID 排序。
pointer address不是 lock-order key。
除第 8.17 / 10.4 明确允许的 WaitSet reconciliation_mutex Fast DDS-graph serialization 例外外，
blocking/DDS entity graph-changing Fast DDS API 不在其它 DMW mutex 内调用。
ReturnCode mapping operation-specific。
OUT_OF_RESOURCES可映射 ResourceExhausted。
heap bad_alloc仍传播。
ordinary non-bad_alloc C++ exception 原样传播；callback/destructor/noexcept cleanup 才 catch + degrade/retain。
detach PRECONDITION不能推广到其它 operations。
cleanup diagnostic不能改变 primary public result。

<a id="fastdds-stable-invariant-ids"></a>

### 11.25 Stable Frozen Invariant IDs

以下 ID 是 review/test 的稳定引用名。正文语义仍是唯一 authority；测试与 Global Closure Gate 必须优先引用 ID 和正文锚点，避免复制长文本而产生漂移。

| ID | Invariant |
|---|---|
| CTX-001 | OperationGuard 与 shutdown linearization 后禁止新 runtime commit |
| CTX-002 | ShutdownExecutionState exactly-one executor；Running必须terminal到Completed或Failed并notify；Failed保存原异常且V1不重跑partial phases |
| CTX-003 | Child acknowledgement normal publication与waiter共享InternalChildState mutex；Phase-D bounded slice避免noexcept ack failure造成永久lost wake |
| CTX-004 | Context facade/Impl destructor执行implicit shutdown；Completed才允许normal final teardown；Failed走terminal retention |
| CTX-005 | Shutdown request-all分B1 logical publish与B2 unlocked signal；任何 private/Fast DDS control wake时不持ChildRegistry/runtime/其它DMW mutex |
| NAME-001 | immutable CompatibilityProfile可在Phase 0无分配snapshot；Phase A allocation-free legality validation先于OperationGuard；Phase B allocation在Context/parent/local检查后 |
| NAME-002 | ServiceKey equality/hash authority字段精确冻结；diagnostic logical/profile字段不参与identity |
| TYPE-001 | 同 Context 同 wire name + BindingIdentity 只有一个 canonical binding |
| TYPE-002 | entered Fast DDS API call exception 先提交 lifecycle status/ownership 再传播 |
| TYPE-003 | endpoint TypeLease 只由 DataReaderInfo/DataWriterInfo 持有；TopicEntry 另持 independent lease |
| TOPIC-001 | TopicRegistry primary key = resolved DDS topic name only；wire type mismatch -> TypeMismatch |
| TOPIC-002 | TopicEntry 的 CreationStatus 与 EntityStatus 分离；Indeterminate Topic* 永不 individual retry |
| TOPIC-003 | `TopicQosFingerprint` 按其 canonical policy 集合执行 semantic equality/hash；禁止 raw-byte/padding hash |
| TOPIC-004 | Absent Topic使用Creating(tx)三阶段事务；Topic mutex下绝不acquire/release TypeLease；transaction token never reuse/wrap |
| DISC-001 | ParticipantObservationRegistryState 是唯一 Participant lifecycle/tombstone authority；Remote/Target不复制participant table |
| DISC-002 | Remote endpoint absent-remove materializes tombstone；Removed+later add while participant Active -> Degraded；terminal不resurrection |
| DISC-003 | Service NeedsRebuild 是 per-local entry；rebuild用generation防stale commit |
| DISC-004 | match/participant/remote/target generation never wrap；exhaustion -> corresponding capability Degraded |
| DISC-005 | GuidPrefix incarnation uniqueness 是 V1 deployment constraint，不是 test-proven vendor guarantee |
| DISC-006 | ordinary data path通过唯一 get-or-create helper取得canonical ParticipantObservationEntry；与remove按rank-5 commit线性化且Removed不复活 |
| TARGET-001 | TargetReaderKey strong-own stable ParticipantObservationEntry；PendingEntry strong-own stable TargetReaderKey；predicate不做Target->Participant/Remote lock inversion |
| TARGET-002 | participant/shutdown external dependency通过Target mutex-protected dependency_generation唤醒；进入wait前在Target mutex下重读participant authority |
| WS-001 | WaitSetInfo preallocation-before-first-attach |
| WS-002 | unresolved attachment -> Poisoned + historical WaitSetInfo/ConditionInfo retained |
| WS-003 | registration ID monotonic/never reuse/exhaustion permanent |
| WS-004 | active_wait_count 只保证 lifetime safety，允许已形成 stale snapshot |
| WS-006 | active_wait_count归零与Detached publication使用Registration drain_mutex/CV handshake；禁止naked atomic store+notify |
| WS-005 | WaitSet::add错误优先级 = args/same-context -> Context -> waitable parent -> registration -> WaitSet local -> Fast DDS reconciliation |
| GUARD-001 | Guard logical commit 是 trigger success linearization |
| GUARD-002 | Broken control guard replacement failure 有 Required/Building/Poisoned 唯一协议 |
| LIST-001 | Listener callbacks_in_flight/accepting由ListenerState mutex authority；drain使用同mutex CV predicate，zero-count evidence 不足则 retain |
| EVENT-001 | Event level-triggered；WaitSet 不消费 cursor；successful take 才消费 |
| EVENT-002 | parent destruction先对全部live Event发布ParentDestroyed，再逐项auto-detach；fan-out no-allocation且Fast DDS wake在零DMW mutex下执行 |
| TAKE-001 | filter loop 受 call-start unread snapshot budget 限制，单次 take 不活锁 |
| SVC-001 | same-Participant service pairing + SampleIdentity correlation |
| SVC-002 | Server capacity reservation precedes unread scan；full -> ResourceExhausted before Fast DDS take |
| SVC-003 | take_request PendingEntryRollbackGuard preserves exception channel and capacity bookkeeping |
| SVC-004 | send_response先lookup NotFound/Busy，再preallocate，再stable-handle revalidate claim；Pending/Target locks不重叠 |
| SVC-005 | send_response terminal write/no-write共用erase commit；failure/exception先rollback bookkeeping；shutdown后不reinsert |
| RET-001 | delete/create public result 与 creation/entity status 是两个独立维度 |
| RET-002 | participant contained-graph barrier 覆盖 hidden create、partial container cleanup 和 Indeterminate child status |
| RET-003 | Participant barrier success invalidates/nuls every prior child raw pointer and suppresses later individual delete |
| TERM-001 | terminal quarantine handoff先release ownership；adoption使用unique_lock + noexcept intrusive splice/counters；lock failure intentional leak |
| AUDIT-001 | 每次revision交付前必须通过11.1.2 lock/error/FSM/authority/throw-point Global Closure Gate |

[验证矩阵](#fastdds-verification)中的 failure-injection/golden tests 必须引用这些 ID 或未来新增的稳定 ID。

<a id="fastdds-appendices"></a>

## 12. Public Contract Preconditions 与最终架构

### 12.1 Public Contract Preconditions

本文不新增 public/runtime contract，`dmw.md` 是唯一 public normative specification。

Public contract closure 的 **唯一条目集合** 是 [Public Contract Closure Requirement](#fastdds-public-closure) 中的 `PUB-001 ... PUB-015`。本附录不再复制条款正文，以避免 Phase 1、Preflight 和 Appendix 三处漂移。

Frozen Candidate -> Frozen 的 public closure 条件：
- 同 revision `dmw.md` 已逐项满足 `PUB-001...PUB-015`；
- public headers 与 `dmw.md` 完全一致，特别是 Context-scoped `CompatibilityProfile` API；
- `~Context()` implicit shutdown contract 与 2.25 Fast DDS mapping 一致；
- `dmw_public_contract_sync.md` 不存在 active/staged normative copy；
- `dmw_public_contract_merge_patch.md` 已合入并删除，或只位于明确 historical/non-normative archive；
- Frozen closure review 只把 `dmw.md` + `dmw_fastdds.md` 作为 normative sources。

Fast DDS-specific mapping仍只存在于本文，例如：
- `SampleInfo.source_timestamp` / reception timestamp；
- SampleIdentity / publication_handle；
- Fast DDS SequenceNumber_t conversion；
- explicit Fast DDS QoS construction；
- DDS Topic/Service resolved-name registry representation。

只要任一 `PUB-*` 未 closure，本文规范状态必须保持 `V1 Implementation Frozen Candidate`。

### 12.2 最终内部架构

下图中 `ContextState`、`NodeState` 和 `RegistrationState` 属于 `dmw::impl`；`ParticipantInfo`、`DataReaderInfo`、`DataWriterInfo`、`ConditionInfo` 和 `WaitSetInfo` 属于 `dmw::impl::fastdds`。

```text
                         DmwProcessRuntime
                                │
                ┌───────────────┴──────────────┐
                │                              │
                ▼                              ▼
        Atomic ID Allocators          ProcessTerminalQuarantine
                │                              │
                └──── ProcessBindingQuarantine┘
                                                ▲
                                                │
                                             failure
                                                │
                                             Context
                                                │
                                      shared ContextState
                                                │
        ┌───────────────────────────────────────┼─────────────────────────┐
        │                                       │                         │
        ▼                                       ▼                         ▼
 ParticipantInfo                       TypeRegistry              TopicRegistry
        │                                       │                         │
        │                                       ▼                         └── TopicEntry
        │                                   TypeEntry                          │
        │                                       │                              └── TypeLease
        │                                       ▼
        │                             CanonicalTypeBinding
        │                                       │
        │                                       ├── canonical MessageType::Impl / TypeSupport
        │                                       └── BindingCapabilityState
        │
        ├── Participant
        ├── DDS Publisher
        ├── DDS Subscriber
        ├── participant/publisher/subscriber contained-entity status
        └── DiscoveryListener
        │
        ├── ChildRegistry
        ├── ParticipantObservationRegistry  (canonical participant tombstone authority)
        ├── RemoteEndpointRegistry
        ├── ServiceMatchRegistry
        ├── TargetReaderObservationRegistry
        ├── OrphanedEndpointRegistry
        └── RetiredWaitSetRegistry

                             NodeState
                                │
            ┌───────────────────┼───────────────────┐
            ▼                   ▼                   ▼
        Publisher           Subscriber          Client/Server
            │                   │                   │
        DataWriterInfo      DataReaderInfo          │
                                │                   │
                                └────────┬──────────┘
                                         │
                                 WaitSetHoldState
                                         ▲
                                         │
                               ConditionInfo
                                         │
                                         ▼
                                  RegistrationState
                                         │
                                         ▼
                                     WaitSet
                                         │
                                         ▼
                              WaitSetInfo
```

Reader safety chain 如下：

```text
WaitSetInfo
        │
        │ attached Reader StatusCondition
        ▼
Reader WaitSet hold > 0
        │
        ▼
Subscriber/Client/Server Reader
public destructor
        │
        ▼
close hold gate
        │
        ▼
hold > 0 ?
        │
       yes
        │
        ▼
DO NOT delete_datareader
        │
        ▼
DataReaderInfo
moves to retirement
        │
        ▼
old WaitSetInfo
finally detached
        │
        ▼
hold -> 0
        │
        ▼
retry delete_datareader
```

Catastrophic failure chain 如下：

```text
Fast DDS WaitSet condition
cannot be safely detached
        │
        ▼
WaitSet -> Poisoned
        │
        ▼
WaitSetInfo -> RetiredWaitSetRegistry
        │
        ▼
Context final teardown
detects unresolved WaitSetInfo
        │
        ▼
STOP Fast DDS deletion
        │
        ▼
TerminalContextNode
        │
        ▼
ProcessTerminalQuarantine
```

因此，当 vendor 连续失败时，实现必须优先保留 DDS entity pointers and Info objects，不得引入 dangling Condition、dangling Listener、dangling Topic、dangling TypeSupport 或 DMW use-after-free。

当且仅当以下条件全部满足时，本实现规格才可从 Frozen Candidate 升级为 `V1 Implementation Frozen`：

1. 同版本 dmw.md 已直接包含本文依赖的全部 public/runtime contract，且不存在第二 normative public source；
2. 本文所有 P0/P1 frozen invariants 已实现；
3. [验证矩阵](#fastdds-verification)中的 golden/failure-injection/interop/sanitizer 验证通过；

升级后，实现阶段不得重新决定以下已具有唯一协议的核心行为：

- Factory 与 shutdown 的 linearization；
- Node destructor 与 endpoint Factory race；
- DataReader 的可删除时点，以及 old WaitSet 对 Reader/Guard 的保活机制；
- Fast DDS attach 部分成功的 rollback、detach 失败后的 WaitSet Poisoning、control Guard stuck-true 处理与 infinite wait cancellation；
- listener late callback drain；
- Service fallback GUID 的含义、response-reader removal history 的保留与 duplicate request 的重新 reserve；
- Event 对 pre-creation history 的 replay 语义、WaitSet 对 Event cursor 的消费语义（V1 不消费，Event 为 level-triggered）、多 Event 独立消费与 counter overflow；
- `NoData` 与 post-take conversion 的 output guarantee；
- Topic 与 endpoint 的 TypeLease ownership，以及同一 Context 内 TypeRegistry 的 canonical binding 选择；
- 独立 descriptor 不得绕过 BindingCapability degradation；
- `register_type`/`create_topic`/`create_datareader`/`create_datawriter` 进入 Fast DDS API call 后发生异常时，必须先记录 `Indeterminate`/`SideEffectIndeterminate` status 再传播；
- 无 DDS entity pointer 的 hidden contained entity 由哪个 parent container deletion barrier 最终释放；
- root `create_participant` exception/no-handle 后 partial Context 进入 ProcessTerminalQuarantine 的条件；
- `delete_contained_entities` partial failure 后 raw pointer 的可用性；
- CompatibilityProfile 唯一解析 Fast DDS topic/service name，且 allocating resolver 不得抢占 `ContextShutdown`；
- Context facade 析构先提交 implicit shutdown，surviving child 不得继续观察 Active；
- service-specific target entry 不存在时，Participant remove 通过 canonical ParticipantObservationRegistry 保留 terminal state；
- `send_response` 的 Pending -> Responding / child registration / target wait / write / rollback 完整 transaction；
- 单次 take/filter 的 bounded progress；
- Discovery registry 的 per-entry rebuild/generation/late-callback FSM；
- Context final teardown 的 exactly-once executor；
- Participant permanent delete failure 后 backing 的保活者。
