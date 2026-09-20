# DMW Fast DDS 实现规格

| 属性 | 值 |
| --- | --- |
| 文档文件 | `dmw_fastdds.md` |
<<<<<<< Updated upstream
| 规范状态 | V1 Implementation Revision Frozen Candidate |
| 上位规范 | [`dmw.md`](dmw.md) |
| 主要 Fast DDS 参考 | eProsima Fast DDS `2.14.x` |
| 主要 ROS 2 Fast DDS 参考 | `ros2/rmw_fastrtps` `jazzy` |
| Common Runtime 参考 | ROS 2 Jazzy `rcl`、`rcl_action` |
| 兼容性验证 | Humble / Fast DDS 2.6.x；Jazzy / Fast DDS 2.14.x |

本文定义 `dmw.md` 在 Fast DDS backend 上的实现规则。Public API、错误优先级、生命周期和可观察语义以 `dmw.md` 为唯一 authority。

## 1. 实现基线与参考关系

### 1.1 平等参考基线

DMW 后续开发的两个主要实现参考为：

```text
Fast DDS 2.14.x ─────────────┐
                             ├── cross-audit ──> DMW Fast DDS implementation
rmw_fastrtps Jazzy ──────────┘
```

二者地位平等，不存在“Fast DDS 高于 rmw_fastrtps”或“rmw_fastrtps 高于 Fast DDS”的实现优先级。

关注重点不同：

Fast DDS 2.14.x：

- DDS API 与 ReturnCode；
- DomainParticipant / Publisher / Subscriber；
- DataWriter / DataReader；
- QoS；
- Listener / StatusCondition / GuardCondition / WaitSet；
- discovery；
- resource ownership 与 teardown。

`rmw_fastrtps` Jazzy：

- Fast DDS 在 ROS 2 中的生产使用方式；
- ROS Topic/Service naming；
- QoS mapping；
- SampleIdentity / MessageInfo；
- service request/response correlation；
- response-reader discovery workaround；
- service availability；
- discovery / wait race；
- listener 与 teardown pattern。

`rcl` / `rcl_action` Jazzy 主要用于 Timer、Action、Graph/Wait 等 language-neutral runtime 职责对照；`rclcpp` / `rclpy` 仅用于确认 Future/callback/typed API 等不应下沉的边界。

### 1.2 兼容性定位

ROS 2 Humble / Fast DDS 2.6.x 是兼容性验证目标，不再决定新的实现结构。

实施策略：

```text
new design
    -> 先在 Fast DDS 2.14.x + Jazzy rmw_fastrtps 上 cross-audit
    -> 使用稳定 DDS-PIM public API
    -> 再确认 2.6.x source compatibility
    -> 确有 API 差异时，仅在 private fastdds compatibility helper 中隔离
```

不得把 Fast DDS minor version 传播到普通 `dmw` public header。

### 1.3 CMake package 名称

Fast DDS 2.14.x 上游 CMake project/package 仍使用：

```text
fastrtps
```

因此 DMW 不应仅因为产品名称是 Fast DDS 就改成未经验证的：

```cmake
find_package(fastdds ...)
```

推荐构建策略：

```cmake
find_package(fastrtps CONFIG REQUIRED)
```

Primary CI 必须验证实际解析版本位于 2.14.x；若继续支持 Humble compatibility build，则 CMake minimum version 可以保留为 2.6.x 可满足的下限，而不是把 2.14 写成 source compatibility 的硬门槛。

换言之：

```text
reference baseline == 2.14.x
```

不等价于：

```text
CMake minimum must == 2.14
```

### 1.4 Fast CDR / generated type boundary

DMW core 不直接建立 Fast CDR public API。

```text
DMW public runtime
        ↓
MessageType
        ↓
dmw::fastdds binding
        ↓
Fast DDS TopicDataType / generated PubSubType
        ↓
Fast CDR
```

Fast CDR 1.x / 2.x 差异必须限制在 generated type / binding / integration test 层。

## 2. 内部边界与命名

### 2.1 public / implementation boundary

普通 public headers 不出现：

```text
eprosima::fastdds::*
eprosima::fastcdr::*
```

Fast DDS 专用 public integration 仅允许出现在明确命名的：

```text
dmw/fastdds/*
```

内部 Fast DDS helper 位于：

```cpp
namespace dmw::impl::fastdds
{
    // private only
}
```

不新增 middleware virtual interface 或 runtime backend dispatch。

### 2.2 实现结构原则

优先结构：

```text
public facade
    ↓
T::Impl
    ↓
small private Fast DDS helper/state
```

不要为了“未来可能支持其他 DDS”创建：

```text
ITransport
IMiddleware
BackendFactory
FastDDSBackend
```

等当前没有需求的抽象。

### 2.3 核心内部 state

允许存在：

```text
ContextState
NodeState
WaitableState
RegistrationState
DiscoveryGraph
EventSourceState
ReaderWaitState
ActionGoalRegistry
```

这些是 concurrency/lifetime authority，不是 public entity。

## 3. Context 与 DomainParticipant

### 3.1 DDS entity mapping
=======
| 规范状态 | V1 Implementation Convergence |
| 上位规范 | [`dmw.md`](dmw.md) |
| 主要 Fast DDS 参考 | eProsima Fast DDS 2.14.x |
| 主要 ROS 2 Fast DDS 参考 | `ros2/rmw_fastrtps` Jazzy |
| Common Runtime 参考 | ROS 2 Jazzy `rcl`、`rcl_action` |
| Graph metadata 参考 | `rmw_dds_common` Jazzy |
| 兼容性验证 | Humble / Fast DDS 2.6.x；Jazzy / Fast DDS 2.14.x |

本文定义 `dmw.md` 在 Fast DDS backend 上的实现规则。Public API、错误优先级、资源所有权、生命周期和可观察语义以 `dmw.md` 为唯一 authority；本文负责把这些 contract 映射为可验证的 Fast DDS / common-runtime 实现策略。

---

## 1. 实现基线、内部边界与工程原则

### 1.1 平等参考基线

DMW 的两个主要实现参考为：

```text
Fast DDS 2.14.x ─────────────┐
                             ├── cross-audit ──> DMW implementation
rmw_fastrtps Jazzy ──────────┘
```

二者地位平等。

Fast DDS 2.14.x 重点用于理解和验证：

- DDS public API / ReturnCode；
- DomainParticipant / Publisher / Subscriber；
- DataWriter / DataReader / Topic；
- QoS；
- Listener / StatusCondition / GuardCondition / WaitSet；
- discovery；
- entity ownership / delete semantics；
- resource and threading guarantees。

`rmw_fastrtps` Jazzy 重点用于：

- Fast DDS 的生产级 ROS 2 用法；
- ROS Topic/Service naming；
- QoS mapping；
- SampleIdentity / MessageInfo；
- service correlation；
- response-reader discovery workaround；
- graph metadata transport；
- discovery/wait race；
- teardown pattern。

`rcl` / `rcl_action` 用于 Clock/Timer/Wait/Graph/Action common-runtime职责对照；`rclcpp` / `rclpy` 只用于确认 typed/Future/callback/Executor 不应下沉的边界。

### 1.2 Humble compatibility

ROS 2 Humble / Fast DDS 2.6.x 是 source-compatibility 与 interoperability target，不决定新的实现结构。

策略：

```text
new implementation
    ↓
Fast DDS 2.14.x + Jazzy reference cross-audit
    ↓
优先稳定 DDS-PIM public API
    ↓
2.6.x source build验证
    ↓
确有API差异 -> private compatibility shim
```

Fast DDS minor version不得传播到普通 public header。

### 1.3 CMake package

Fast DDS 2.14.x CMake package仍通常通过：

```cmake
find_package(fastrtps CONFIG REQUIRED)
```

发现。

因此：

```text
reference baseline == 2.14.x
```

不等价于：

```text
CMake minimum must == 2.14
```

如果同一 source继续支持 Humble/Fast DDS 2.6.x，CMake minimum可以保留2.6.x可满足的下限；Primary CI再单独验证实际Fast DDS版本为2.14.x。

### 1.4 Public / Fast DDS boundary

普通 public headers 不出现：

```text
eprosima::fastdds::*
eprosima::fastrtps::*
eprosima::fastcdr::*
```

Fast DDS generated-type integration只允许出现在：

```text
include/dmw/fastdds/*
```

private implementation helper统一放在：

```cpp
namespace dmw::impl::fastdds
{
    // private only
}
```

不新增：

```text
ITransport
IMiddleware
BackendFactory
FastDDSBackend virtual hierarchy
```

等没有实际需求的通用backend abstraction。

### 1.5 Fast CDR / generated type boundary

```text
DMW runtime
    ↓
MessageType
    ↓
dmw::fastdds binding
    ↓
Fast DDS TopicDataType / generated PubSubType
    ↓
Fast CDR
```

Fast CDR 1.x/2.x差异限制在 generated type/binding/integration-test 层，不传播到 DMW core。

### 1.6 推荐内部结构

优先：

```text
public facade
    ↓
T::Impl
    ↓
small private state/helper
    ↓
Fast DDS
```

允许存在的真实 concurrency/lifetime state：

```text
ContextState
NodeState
ClockState
TypeRegistry
TopicRegistry
DiscoveryGraph
WaitableState
RegistrationState
ReaderWaitState
EventSourceState
ServerRequestState
ActionGoalRegistry
ParameterStoreState
```

这些不是 public entity，不应因为 private state存在就新增public framework layer。

---

## 2. Context、DDS Entity Mapping 与创建生命周期

### 2.1 Entity mapping
>>>>>>> Stashed changes

```text
Context
    -> DomainParticipant
    -> one DDS Publisher container
    -> one DDS Subscriber container
<<<<<<< Updated upstream
=======
    -> internal graph metadata writer/reader in ROS2 mode
>>>>>>> Stashed changes

Node
    -> no DDS entity
    -> local graph metadata record
    -> ParameterStoreState

Clock
    -> no DDS entity

Publisher
    -> DataWriter

Subscriber
    -> DataReader

Client
    -> request DataWriter
    -> response DataReader

Server
    -> request DataReader
    -> response DataWriter

Timer
    -> no DDS entity

WaitSet
    -> Fast DDS WaitSet
    -> private control GuardCondition

GuardCondition
<<<<<<< Updated upstream
    -> Fast DDS GuardCondition + logical generation

Timer
    -> no DDS entity

GraphEvent
    -> no dedicated DDS entity; driven by DiscoveryGraph revision

ActionClient
    -> 3 service-client endpoint pairs + 2 DataReaders

ActionServer
    -> 3 service-server endpoint pairs + 2 DataWriters
    -> logical Goal/result-expiry state
```

### 3.2 Context create transaction

目标创建顺序：

```text
allocate Context shared state
        ↓
create DiscoveryGraph / registry state
        ↓
create ParticipantListener backing
        ↓
construct explicit DomainParticipantQos
        ↓
DomainParticipantFactory::create_participant(
    domain_id,
    qos,
    listener,
    StatusMask::none())
        ↓
create DDS Publisher container
        ↓
create DDS Subscriber container
        ↓
commit Context Active
```

与当前“先创建 Participant，后 `set_listener()`”相比，新的实现应优先在 `create_participant()` 时传入 listener，避免 Participant creation 与 listener installation 之间的 discovery observation window。

### 3.3 Participant QoS 来源

DMW 的 public contract 不把 mutable XML/profile defaults 当作 RuntimeMode contract。因此默认实现：

- 不调用 ROS 2 RMW 的 XML override pipeline；
- 不读取 `RMW_FASTRTPS_USE_QOS_FROM_XML`；
- 不依赖用户调用前修改 Fast DDS factory mutable default；
- 从 DMW-owned baseline QoS 开始，再应用 RuntimeMode explicit override。

`rmw_fastrtps` 中的 XML/profile 支持是重要工程参考，但 DMW V1 没有对应 public feature，因此不应无条件复制。

### 3.4 ROS2 Participant baseline

ROS2 mode 至少显式保证 builtin discovery history 可动态扩展：

```cpp
PREALLOCATED_WITH_REALLOC_MEMORY_MODE
```

其它 Participant transport/discovery 配置使用 Fast DDS 2.14.x public default，除非 `dmw.md` 后续增加明确 public option。

### 3.5 Publisher/Subscriber container

一个 Context 创建一个 DDS Publisher container 和一个 DDS Subscriber container；DMW public `Publisher`/`Subscriber` endpoint 分别是其中的 DataWriter/DataReader。

container 创建失败时必须回滚 Participant，不提交 Context。

不要为每个 logical Node 创建独立 DDS Publisher/Subscriber container。

## 4. Listener 与 callback lifetime

### 4.1 listener 职责

Fast DDS listener 只做：

```text
capture discovery/status callback data
update synchronized DMW state
mark logical readiness
trigger wait notification
return
```

不得：

```text
invoke dclcpp callback
invoke Python callback
fulfill arbitrary user Future
block waiting for application work
```

### 4.2 listener backing lifetime

Listener backing 必须至少存活到：

1. 对应 DDS entity 不再可能开始新 callback；
2. 已进入 callback 全部退出。

建议每个 listener/state 使用：

```text
closing flag
+
in-flight callback counter / shared lifetime guard
```

teardown：

```text
mark closing
    ↓
detach/disable listener or delete DDS entity
    ↓
drain callbacks already entered
    ↓
release listener backing
```

### 4.3 Participant discovery listener

Participant listener 更新 Context `DiscoveryGraph`，至少处理：

- remote participant add/remove；
- DataWriter discovery add/change/remove；
- DataReader discovery add/change/remove。

callback 只提交 normalized discovery records，不直接执行 service/action availability callback。

### 4.4 delete failure

如果 Fast DDS delete 返回失败，且实现无法证明 middleware 不再引用 listener/state backing：

> memory safety 优先于强制 free。

允许把必要 backing 保留到 parent/Participant teardown；最终 Participant 删除仍失败时，可以进入 process-lifetime conservative retention，并记录 diagnostic。

不要在 delete failure 后立即释放 middleware 可能继续访问的 backing。

## 5. MessageType / TypeRegistry / TopicRegistry

### 5.1 MessageType binding

`dmw::fastdds::MessageTypeAdapter` 持有：

```text
DDS wire type name
Fast DDS TypeSupport
BindingIdentity (e.g. std::type_index)
```

普通 runtime 不直接依赖具体 generated `PubSubTypeT`。

### 5.2 TypeRegistry key

```text
wire_type_name
```

value：

```text
BindingIdentity
TypeSupport backing
registration state
endpoint/topic references
```

规则：

```text
same wire name + same binding identity
    -> reuse

same wire name + different binding identity
    -> TypeMismatch
```

### 5.3 registration lifecycle

第一引用：

```text
participant->register_type(...)
```

最后 endpoint/topic 引用释放后才允许 unregister。

不要在仍有 Topic/DataReader/DataWriter 依赖时 unregister type。

如果 unregister 失败且 Fast DDS 仍可能引用 TypeSupport backing，保留 backing 到 Participant final teardown。

### 5.4 TopicRegistry

Key：

```text
resolved DDS topic name
+
wire type name
```

同 DDS topic name 被不同 wire type 使用：

```text
TypeMismatch
```

同名同 type：reuse Fast DDS Topic。

Topic refcount 在 endpoint transaction commit 后增加，在 endpoint DDS entity 安全删除后减少。

### 5.5 endpoint create transaction

推荐统一 transaction：

```text
validate Context/Node/name/type/qos
        ↓
resolve logical name -> DDS name
        ↓
acquire/register TypeRegistry entry
        ↓
acquire/create TopicRegistry entry
        ↓
prepare listener/wait state
        ↓
create DataWriter/DataReader
        ↓
install internal state
        ↓
commit public endpoint
```

failure 按逆序 rollback。

## 6. QoS mapping

### 6.1 mapping authority

Public `dmw::Qos` 是唯一输入；Fast DDS `DataWriterQos` / `DataReaderQos` 是实现产物。

所有 mapping 集中在少量 private helper，例如：

```text
impl/fastdds/qos.hpp
```

不要在 Publisher/Subscriber/Client/Server/Action 各复制一套 policy switch。

### 6.2 DDS mode baseline

`RuntimeMode::DDS`：

- 从 DMW-owned Fast DDS baseline QoS 构造；
- 不读取 ROS-specific environment variable；
- public `SystemDefault` 解析到该 baseline；
- 显式 Qos policy 覆盖 baseline 对应字段。

### 6.3 ROS2 mode endpoint baseline

当 DMW 自己控制 endpoint QoS，而不是要求使用外部 XML override 时，参考 Jazzy `rmw_fastrtps` 的非-middleware-default path，ROS2 mode 对 DataWriter/DataReader 显式设置：

```text
history_memory_policy = PREALLOCATED_WITH_REALLOC_MEMORY_MODE
data_sharing = OFF
```

DataWriter publication mode 默认：

```text
SYNCHRONOUS_PUBLISH_MODE
```

这与 Jazzy `rmw_fastrtps` 未设置 publication-mode environment override 时的行为一致。

DMW V1 不读取 `RMW_FASTRTPS_PUBLICATION_MODE`；若未来需要 publication mode，新增 DMW 自己的 explicit option，而不是隐式继承 ROS environment contract。

### 6.4 public policy mapping
=======
    -> Fast DDS GuardCondition
    -> logical trigger generation

Event
    -> endpoint listener/EventSource cursor state

GraphEvent
    -> DiscoveryGraph revision cursor

ActionClient
    -> 3 service-client endpoint pairs
    -> Feedback DataReader
    -> Status DataReader
    -> aggregate WaitableState

ActionServer
    -> 3 service-server endpoint pairs
    -> Feedback DataWriter
    -> Status DataWriter
    -> GoalRegistry / expiry state
    -> aggregate WaitableState
```

### 2.2 Context create transaction

目标顺序：

```text
allocate Context shared state
        ↓
parse/capture Context Arguments
        ↓
create TypeRegistry / TopicRegistry / DiscoveryGraph
        ↓
create ParticipantListener backing
        ↓
construct explicit DomainParticipantQos baseline
        ↓
create_participant(domain_id, qos, listener, mask)
        ↓
create DDS Publisher container
        ↓
create DDS Subscriber container
        ↓
if ROS2 mode: create graph metadata transport
        ↓
commit Context Active
```

Participant listener优先在 `create_participant()` 时安装，避免“Participant创建成功但listener尚未安装”的 discovery observation window。

任一步失败按逆序rollback，不提交Active Context。

### 2.3 Participant QoS baseline

DMW public contract不把不可审计的 process-global mutable XML/profile state自动变成RuntimeMode contract。

V1默认：

- 不读取 `RMW_FASTRTPS_USE_QOS_FROM_XML`；
- 不读取 `RMW_FASTRTPS_PUBLICATION_MODE`；
- 不依赖caller在Context创建前修改Fast DDS factory global default；
- Context创建时从一个可审计的Fast DDS upstream baseline构造 ParticipantQos；
- 后续若支持XML/profile override，必须通过明确 DMW option启用并冻结precedence。

ROS2 mode至少保证 builtin discovery history/resource policy不会因固定preallocation过小而拒绝正常动态 discovery；参考Jazzy生产配置采用可扩展memory policy。

### 2.4 DDS Publisher / Subscriber container

每个 Context创建：

```text
1 DDS Publisher container
1 DDS Subscriber container
```

多个logical Node共享这些container。

不要为每个Node创建独立DDS Publisher/Subscriber。

container创建失败必须回滚Participant。

### 2.5 OperationGuard / shutdown

ContextState维护：

```text
lifecycle state
active operation count
shutdown generation/condition
child/backing references
```

public middleware operation进入Fast DDS前先取得OperationGuard。

shutdown：

```text
Active -> ShuttingDown linearization
    ↓
reject new operations
    ↓
trigger all runtime/control wake paths
    ↓
drain operations/callbacks needed for safety
    ↓
mark Shutdown
```

Context facade销毁不等于立即delete Participant；最终Participant teardown发生在最后一个child/backing安全释放后。

### 2.6 Node create/destroy

Node create：

```text
merge global + local Arguments
    ↓
resolve node-name/namespace remap
    ↓
validate normalized name/namespace
    ↓
allocate NodeState
    ↓
register local graph metadata
    ↓
install parameter override table / store policy
    ↓
commit Node facade
```

Node没有DDS entity。

Node facade析构：

- 从local graph metadata中移除Node public ownership；
- 已存在endpoint仍可持有NodeState，直到它们销毁；
- graph metadata中的Node->endpoint association只有在对应endpoint/Node ownership实际消失后才更新；
- 不允许为了Node facade析构提前使child endpoint无效。

---

## 3. Foundation Backend：Types、Arguments、Clock、QoS 与 Registries

### 3.1 MessageType binding

`dmw::fastdds` adapter保存：

```text
DDS wire type name
Fast DDS TypeSupport
BindingIdentity
```

runtime不依赖具体 `PubSubTypeT`。

TypeRegistry key：

```text
wire type name
```

value：

```text
BindingIdentity
TypeSupport backing
Fast DDS registration phase
Topic/endpoint reference state
```

same wire name + same BindingIdentity -> reuse。

same wire name + different BindingIdentity -> `TypeMismatch`。

### 3.2 Type registration lifecycle

第一引用需要时调用Participant type registration；最后Topic/endpoint引用释放后才允许unregister。

如果unregister失败且Fast DDS仍可能引用TypeSupport backing：

- 不释放backing；
- 延迟到Participant teardown；
- 必要时进入conservative retention并记录diagnostic。

### 3.3 TopicRegistry

Topic identity：

```text
resolved DDS topic name
+
wire type name
```

同DDS topic name不同wire type -> `TypeMismatch`。

同名同type -> reuse Fast DDS Topic。

Topic reference只在endpoint transaction commit后增加；endpoint DDS entity安全delete后减少。

### 3.4 Endpoint create transaction

统一模板：

```text
validate Context/Node/name/type/Qos
        ↓
resolve logical name/remap -> DDS name
        ↓
acquire/register TypeRegistry entry
        ↓
acquire/create TopicRegistry entry
        ↓
prepare listener / EventSource / WaitableState
        ↓
create DataWriter/DataReader
        ↓
install graph/local entity association
        ↓
commit public endpoint
```

failure逆序rollback。

### 3.5 Arguments / remapping implementation

Arguments parser属于 DMW common runtime，不依赖Fast DDS。

建议内部形成immutable canonical records：

```text
ParsedArguments
├── ordered RemapRule list
├── parameter override map/rules
└── unparsed arguments
```

规则应用必须deterministic，同一Context/Node输入在dclcpp和dclpy获得完全相同结果。

name resolver分成两层：

```text
common logical resolver
    namespace expansion + remap + validation

Fast DDS transport resolver
    RuntimeMode::DDS / ROS2 mapping
```

不要把 `rt/`、`rq/`、`rr/` prefix逻辑散布在Publisher/Client/Action实现中。

### 3.6 ClockState

Clock没有DDS entity。

System/Steady Clock可以直接使用标准平台time source：

```text
System -> std::chrono::system_clock or equivalent realtime source
Steady -> std::chrono::steady_clock or equivalent monotonic source
```

Ros ClockState至少保存：

```text
override_enabled
current_override_time
clock_generation
registered dependent Timer/WaitSet weak state
```

Ros override disabled：now跟随system time。

Ros override enabled：now只由`set_ros_time()`更新。

update：

```text
lock ClockState
    ↓
validate / commit new value + generation
    ↓
collect dependent WaitSet wake targets
    ↓
unlock
    ↓
trigger control notifications
```

DMW不在ClockState内部执行user jump callback。

### 3.7 QoS mapping authority

public `dmw::Qos`是唯一输入；Fast DDS `DataWriterQos` / `DataReaderQos`是实现产物。

mapping集中在少量private helper，例如：

```text
src/impl/fastdds/qos.*
```

禁止Publisher/Subscriber/Client/Server/Action各复制policy switch。

### 3.8 SystemDefault baseline

`SystemDefault`不等于`ros2_default`。

实现应在Context/container初始化时capture一份明确、可审计的Fast DDS default entity baseline，后续endpoint creation从该baseline构造，再应用：

```text
RuntimeMode required override
+
explicit dmw::Qos fields
```

同一Context生命周期中，不应因为process-global default后来被其他代码修改而使`SystemDefault`语义漂移。

未来如支持XML/profile override，该override必须在Context creation时显式capture，并通过actual_qos可观察。

### 3.9 ROS2 endpoint baseline

参考Jazzy `rmw_fastrtps` validated path，ROS2 mode在DMW自己控制endpoint Qos时至少明确：

```text
history_memory_policy = PREALLOCATED_WITH_REALLOC_MEMORY_MODE
data_sharing = OFF
DataWriter publication mode = SYNCHRONOUS_PUBLISH_MODE
```

除非后续public option明确允许不同策略。

这些是implementation baseline，不应变成普通`dmw::Qos`字段。

### 3.10 Public QoS mapping
>>>>>>> Stashed changes

History：

```text
<<<<<<< Updated upstream
KeepLast -> KEEP_LAST_HISTORY_QOS
depth    -> history.depth
=======
KeepLast -> KEEP_LAST_HISTORY_QOS + checked depth
>>>>>>> Stashed changes
KeepAll  -> KEEP_ALL_HISTORY_QOS
```

Reliability：

```text
Reliable   -> RELIABLE_RELIABILITY_QOS
BestEffort -> BEST_EFFORT_RELIABILITY_QOS
```

Durability：

```text
Volatile       -> VOLATILE_DURABILITY_QOS
TransientLocal -> TRANSIENT_LOCAL_DURABILITY_QOS
```

<<<<<<< Updated upstream
Deadline/Lifespan/Liveliness/Lease 使用 checked duration conversion；禁止 integer wrap。

### 6.5 max_blocking_time

`reliability().max_blocking_time` 当前不是 DMW public Qos policy，但它是 Fast DDS effective writer QoS 的一部分。

Service response-reader matching workaround 必须读取 **实际 response DataWriter effective QoS**：

```cpp
writer->get_qos().reliability().max_blocking_time
```

并转换为 absolute steady deadline。

不再在 DMW contract 或实现中另写一份固定 `100 ms` timeout。

### 6.6 common profiles golden test

Fast DDS mapping 必须对以下 DMW profiles 建 golden test：

```text
ros2_default
ros2_services_default
ros2_sensor_data
ros2_parameters
ros2_parameter_events
ros2_action_status_default
```

## 7. Topic data path

### 7.1 Publisher::write

步骤：

```text
validate message != nullptr
validate Context Active
enter operation guard
DataWriter::write(message, optional WriteParams)
map return
leave operation guard
```

当前 Fast DDS 2.14 仍存在返回 `bool` 的常用 `write(void*)` overload，也有 ReturnCode/handle/timestamp overload。实现可以使用最稳定、最能取得所需 metadata 的 public overload；public DMW result 不暴露这个差异。

### 7.2 Subscriber::read

单次调用使用 call-start finite candidate budget：

1. snapshot 可扫描的 unread candidate 数；
2. `take_next_sample()`；
3. invalid data / filtered sample 消耗 budget；
4. valid sample 转换到 caller output；
5. budget 耗尽返回 false。

并发新 arrival 不得无限延长单次 `read()`。

### 7.3 MessageInfo

优先从 Fast DDS `SampleInfo` 提取：

```text
sample/source identity
publication handle/GUID fallback
source timestamp
reception timestamp
sequence number（可用时）
```

转换失败不得伪造 timestamp。

### 7.4 matched count

使用 Fast DDS publication/subscription matched status 或等价 thread-safe state。

matched count 不从 DiscoveryGraph 同名 endpoint 数量推导，因为 QoS incompatible endpoint 不应计入。

## 8. Service runtime

### 8.1 endpoint composition

Client：

```text
request writer
response reader
```

Server：

```text
request reader
response writer
```

create/rollback/destroy 均作为整体 transaction。

### 8.2 ROS2 request write

ROS2 mode：

```cpp
WriteParams params;
params.related_sample_identity().writer_guid() = response_reader_guid;
request_writer->write(request, params);
```

write 成功后使用 generated sample identity 的 sequence number 标准化：

```text
RequestId.client_gid = response_reader Gid
RequestId.sequence_number = request sample sequence
```

这与 Jazzy `rmw_fastrtps` request path 对齐。

### 8.3 Server take request

读取：

```text
SampleInfo.sample_identity
SampleInfo.related_sample_identity
```

若 related writer GUID 有效，则它表示 Client response reader target；否则使用 request sample writer GUID fallback。

标准化到 public RequestId。

### 8.4 pending request capacity

在调用 `take_next_sample()` 前，在 Server pending-state 同一同步域中预留 capacity slot。

如果：

```text
Pending + Responding + reserved >= max_pending_requests
```

返回 `ResourceExhausted`，不消费 DDS history sample。

### 8.5 response write

进入 Responding 后，ROS2 mode 首先确保 target Client response reader 已 matched。

Jazzy reference 行为：

```text
related identity indicates reader
    ↓
query response writer effective max_blocking_time
    ↓
wait target subscription GUID
```

DMW 实现：

```text
already matched -> write immediately
confirmed target gone -> success without write
matched before deadline -> write
Context shutdown -> ContextShutdown
deadline expires -> Timeout
```

所有 discovery wake 都使用同一个 absolute deadline，不重置 timeout。

write failure 后 Server RequestId 回到 Pending，允许应用 retry。

### 8.6 response write params

response：

```cpp
params.related_sample_identity(request_sample_identity);
```

或者等价构造，保证 response related sequence 与 Client filtering 兼容。

### 8.7 Client take response

Client 过滤不属于自己的 response。

兼容检查至少接受 Jazzy/Humble interoperability test 验证的 endpoint identity form，不把旧实现 workaround 写成 public API。

### 8.8 Service availability registry

DiscoveryGraph 为每个 remote endpoint 保存：

```text
participant prefix
endpoint GUID
reader/writer kind
resolved topic
wire type
QoS/match relevant metadata
```

Service candidate 要求同一 remote participant 同时具有 compatible request reader 和 response writer。

`wait_for_service()` 等待 DiscoveryGraph revision/condition，不使用 `sleep_for()` polling loop。

## 9. DiscoveryGraph 与 Graph public view

### 9.1 internal record

建议内部记录：

```text
RemoteParticipantRecord
RemoteEndpointRecord
ServiceCandidateRecord
ActionCandidateRecord
GraphRevision
```

不要求把 public `GraphSnapshot` 直接作为可变 cache 存储。

### 9.2 endpoint normalize

listener callback 收到 DDS topic/type 时，先保存原始 DDS identity，再尝试根据 RuntimeMode 解析 logical name：

ROS2 topic：

```text
rt/<path> -> /<path>
```

ROS2 service：

```text
rq/<path>Request
rr/<path>Reply
```

解析失败的普通 DDS endpoint 可以保留在 internal discovery state，但只在能够形成合法 public logical entry 时进入相应 GraphSnapshot view。

### 9.3 action composition recognition

识别 logical suffix：

```text
/_action/send_goal
/_action/cancel_goal
/_action/get_result
/_action/feedback
/_action/status
```

同 remote participant 五个所需 endpoint 组合完成后形成 ActionServer candidate。

不要跨 participant 拼接。

### 9.4 revision commit

Discovery callback under graph mutex：

```text
normalize callback
    ↓
compare old record
    ↓
no observable change -> no revision bump
    ↓
apply state change
    ↓
revision++
    ↓
collect graph-event/wait notifications
```

trigger notification 应在释放 graph mutex 后执行，避免 callback 内部锁反转。

### 9.5 GraphSnapshot

snapshot：

1. lock graph；
2. copy immutable public view；
3. record revision；
4. unlock；
5. return。

不得把内部 endpoint pointer/GUID container iterator 暴露给 caller。

### 9.6 GraphEvent

GraphEvent 保存 cursor revision。

DiscoveryGraph revision 变化时：

- 标记所有 relevant GraphEvent logically ready；
- 若 event 已注册 WaitSet，则触发该 WaitSet control guard；
- successful `GraphEvent::take()` 推进 cursor。

## 10. Timer implementation

### 10.1 no DDS entity / no worker thread

Timer 只保存：

```text
period
canceled
last_call_time
next_call_time
registered WaitSet state
```

不创建 Fast DDS entity，不创建 per-timer thread。

### 10.2 time source

使用：

```cpp
std::chrono::steady_clock
```

所有 add/compare 使用 checked/saturating arithmetic，避免 time_point overflow。

### 10.3 state synchronization

Timer state 使用一个小粒度 mutex 或等价 atomic state machine。

推荐优先 mutex，因为：

- period + next_call + last_call 是组合 state；
- reset/cancel/exchange/consume 需要原子更新多个字段；
- Timer operation 不是硬实时 fast path。

不要为了“lock-free”引入复杂多字段 CAS protocol。

### 10.4 consume algorithm

成功 consume：

```text
expected = next_call_time
actual = now
elapsed = now - last_call_time
last_call_time = now
```

period > 0：

```text
next = expected + period
if next <= now:
    missed = 1 + (now - next) / period
    next += missed * period
```

period == 0：

```text
next = now
```

所有乘加使用 overflow-safe arithmetic；必要时 saturate 到 `steady_clock::time_point::max()` 并返回可诊断错误，而不是 wrap。

### 10.5 WaitSet notification

reset/cancel/exchange_period 改变 earliest deadline 时，调用 registered WaitSet 的 topology/control notification。

Timer 到期本身不需要后台 thread trigger GuardCondition；Fast DDS WaitSet 的 timeout 被设置为 earliest Timer deadline，因此 deadline 到达自然唤醒 wait。

## 11. WaitSet implementation

### 11.1 目标

新的 Fast DDS baseline 不再把“每 100 ms 醒一次检查”作为正常 wait algorithm。

正常路径：

```text
logical pre-check
    ↓
attach/current native conditions
    ↓
compute absolute caller deadline
    ↓
compute earliest Timer/Action expiry
    ↓
Fast DDS WaitSet::wait(remaining minimum)
    ↓
detach/reconcile if topology changed
    ↓
logical re-check
    ↓
Ready or continue until original deadline
```

### 11.2 private control GuardCondition

每个 DMW WaitSet 拥有一个 private Fast DDS GuardCondition，用于：

- add/remove；
- waitable destruction/auto-detach；
- Timer reset/cancel/period change；
- GraphEvent revision；
- Action logical deadline/topology change；
- Context shutdown。

control guard 不进入 public WaitResult。

### 11.3 reader readiness condition

Subscriber / Client / Server / Action reader path 使用 DataReader StatusCondition 的：

```text
DATA_AVAILABLE_STATUS
```

或 Fast DDS 版本兼容 helper 能提供的更精确 public condition。

Jazzy `rmw_wait` 的关键模式必须保留：

1. wait 前 pre-check 数据是否已经 available；
2. 未 ready 才 attach/wait；
3. wake 后重新检查 entity readiness；
4. GuardCondition reset/consume 与 concurrent trigger 不丢 wake。

### 11.4 2.14 / 2.6 readiness adapter

若 Fast DDS 2.14.x 提供比 2.6.x 更适合的 unread-info query，例如能判断 first untaken valid sample 的 API，可以封装在：

```text
impl/fastdds/reader_readiness.hpp
```

2.6.x fallback 使用该版本稳定 public API。

版本差异不得泄漏到 `WaitSet` public API。

### 11.5 composite registration

`RegistrationState` 可以对应多个 native conditions：

```text
Subscriber -> 1 reader condition
Client -> response reader condition
Server -> request reader condition
ActionClient -> goal/cancel/result response + feedback + status reader conditions
ActionServer -> goal/cancel/result request conditions + logical expiry deadline
```

任意子 condition ready，只返回 composite public token 一次。

### 11.6 topology generation

WaitSet 保存单调 topology generation。

add/remove/auto-detach：

```text
mutate logical registrations
    ↓
generation++
    ↓
trigger control guard
```

active wait wake 后如果 generation 改变，重建 native condition snapshot，再使用**原始 caller deadline**继续。

### 11.7 finite/infinite wait

caller finite：

```text
user_deadline = now + timeout
```

caller infinite：

```text
user_deadline = none
```

native remaining：

```text
min(
    user deadline remaining if finite,
    earliest Timer deadline,
    earliest Action expiry deadline)
```

如果没有任何 deadline，则使用 Fast DDS infinite duration。

### 11.8 no fixed polling slice

正常有效 Fast DDS WaitSet/GuardCondition path 不使用：

```text
100 ms periodic slice
```

作为 lost-wakeup safety net。

如果 Fast DDS GuardCondition trigger 返回错误：

- 触发该 API 的 public operation按 `dmw.md` 返回 `DDSError` 或 Context failure；
- 不伪装为 success；
- implementation 记录 diagnostic；
- 不通过永久轮询隐藏 middleware error。

### 11.9 removal/destruction safety

WaitSet 不长期依赖 raw public facade pointer。

使用：

```text
WaitableState
RegistrationState
native condition hold
```

waitable destructor：

```text
mark Closing
    ↓
invalidate registration
    ↓
wake active WaitSet
    ↓
drain active native snapshot reference
    ↓
detach condition
    ↓
destroy DDS/native resource
```

WaitSet destructor 前置条件：自身没有 active `wait()`。

## 12. GuardCondition implementation

### 12.1 logical generation

GuardCondition 保存：

```text
trigger_generation
consumed_generation
```

多 trigger 可以 coalesce，但新 trigger 不得被 concurrent reset 丢失。

### 12.2 trigger transaction

推荐在 GuardCondition state mutex 下：

1. validate Context Active；
2. stage new generation；
3. `Fast DDS GuardCondition::set_trigger_value(true)`；
4. 成功后 commit generation；
5. 失败则不把本次 public trigger 标记为成功。

这样避免“logical success 但 native infinite wait 永远未被唤醒”的无法兑现 contract。

### 12.3 consume

WaitSet 确认 ready 后 snapshot generation，并在形成 Ready result 前推进 consumed generation。

如果 consume 期间发现新 generation，native trigger 保持 true；否则 reset false。

## 13. Event implementation

### 13.1 listener cumulative state

Publisher/Subscriber listener 保存 Fast DDS status cumulative/change information到 `EventSourceState`。

每个 public Event 保存自己的 cursor。

不要让多个 Event 直接竞争消费同一个 Fast DDS destructive status query，从而互相吞掉事件。

### 13.2 Event readiness

```text
source cumulative generation > event cursor
```

listener update 后，若 Event 已注册 WaitSet，触发对应 wait control notification。

WaitSet 不推进 cursor；`Event::take()` 才推进。

### 13.3 parent destruction

parent endpoint delete 前：

- mark EventSource closing；
- auto-detach child Event registrations；
- drain listener；
- delete DataWriter/DataReader；
- child Event 后续 operation 返回 `ParentDestroyed`（Context Active 时）。

## 14. Action implementation

### 14.1 ActionClient composition

ActionClient::Impl 内部拥有：

```text
SendGoal client endpoint pair
CancelGoal client endpoint pair
GetResult client endpoint pair
Feedback DataReader
Status DataReader
Action availability state
aggregate WaitableState
```

这些 endpoint 不向 Client Library 暴露为 public `Client`/`Subscriber`。

可以复用现有 Client/Subscriber private creation helper，但不要通过 public API 让内部 endpoint 具有独立 WaitSet ownership。

### 14.2 ActionServer composition

ActionServer::Impl：

```text
SendGoal server endpoint pair
CancelGoal server endpoint pair
GetResult server endpoint pair
Feedback DataWriter
Status DataWriter
GoalRegistry
result expiry scheduling state
aggregate WaitableState
```

### 14.3 create transaction

五个 endpoint 必须整体 transaction：

```text
resolve all five logical names
        ↓
validate all five descriptors/QoS
        ↓
reserve registry/topic/type references
        ↓
create services/topics/endpoints
        ↓
install aggregate state
        ↓
commit ActionClient/ActionServer
```

任何一步失败，逆序 rollback 全部已创建 endpoint。

不得返回“3 个 service 成功但 status topic 失败”的 partial Action object。

### 14.4 Action readiness

ActionClient native reader conditions：

```text
goal response
cancel response
result response
feedback
status
```

ActionServer：

```text
goal request
cancel request
result request
```

另有 logical：

```text
goal expiry deadline
```

WaitSet 任意子通道 ready -> public Action registration ready。

`ActionClient::readiness()` / `ActionServer::readiness()` 重新检查当前状态，不直接信任过时 native condition vector。

### 14.5 GoalRegistry

GoalRegistry 不是 Fast DDS object，使用 DMW mutex-protected map：

```text
GoalId -> GoalRecord
```

GoalRecord：

```text
GoalInfo
GoalState
terminal steady timestamp optional
pending result RequestIds
```

FSM transition 按 `dmw.md` 固定表执行。

不要复制一份 C++ FSM 和一份 Python FSM。

### 14.6 accept flow

因为 raw accepted response message 由上层 typed binding 构造，implementation 需要一个 private transaction helper，把：

```text
reserve/validate GoalId
write accepted response
commit GoalRecord Accepted
```

组合起来。

public基础 primitive 可以保持分离，但 dclcpp/_dclpy 应调用该 transaction path，避免 write success 后 registry commit failure。

### 14.7 cancel selection

`select_cancel_goals()` 只在 GoalRegistry snapshot 上执行纯 selection，不改变状态。

Client Library user cancel callback 返回接受后，DMW `update_goal_state(CancelGoal)` 才提交 Canceling。

### 14.8 pending result requests

GetResult request 到达后，typed binding 从 raw request 提取 GoalId，再调用：

```text
register_result_request(goal_id, RequestId)
```

active goal -> 保存 RequestId；terminal -> 立即由上层 typed result cache回复；unknown -> unknown-goal response。

terminal transition 后，DMW 把 pending RequestId list 移交给上层一次。

### 14.9 result payload cache boundary

DMW 不复制任意 action-specific result object。

禁止为了把 result payload cache 也下沉而新增：

- runtime reflection；
- generic object clone ABI；
- serialized-message public subsystem；
- language-dependent allocator callback。

Goal lifecycle 下沉，payload storage 留 typed layer，这是 V1 有意边界。

### 14.10 result expiry

terminal state commit：

```text
terminal_time = steady_clock::now()
expiry = terminal_time + result_timeout
```

不创建 background expiry thread。

ActionServer 注册 WaitSet 时，earliest expiry 参与 native wait deadline。

ActionServer 未注册 WaitSet 时，任何 GoalRegistry operation / `take_expired_goals()` 可以 lazy prune 已过期 goal。

`take_expired_goals()` 返回 GoalId 后，上层删除对应 typed result payload cache。

### 14.11 status snapshot

DMW 从 GoalRegistry 构造 stable `vector<GoalStatusInfo>` snapshot。

上层把它映射为实际 ROS/common status message，再调用 ActionServer status DataWriter。

这样 DMW 保持状态 authority，却不需要依赖 action-specific/generated object layout。

### 14.12 Action availability

DiscoveryGraph 按 remote participant 计算五个 endpoint composition。

至少要求：

```text
same participant:
    SendGoal server
    CancelGoal server
    GetResult server
    Feedback writer
    Status writer
```

才 available。

这比简单五个全局 count AND 更严格。

## 15. ROS 2 Action naming / QoS mapping

### 15.1 logical derived names

Action `/move`：

```text
/move/_action/send_goal
/move/_action/cancel_goal
/move/_action/get_result
/move/_action/feedback
/move/_action/status
```

### 15.2 DDS resolved names

通过已有 service/topic ROS2 resolver：

```text
send_goal request  -> rq/move/_action/send_goalRequest
send_goal response -> rr/move/_action/send_goalReply
cancel request     -> rq/move/_action/cancel_goalRequest
cancel response    -> rr/move/_action/cancel_goalReply
result request     -> rq/move/_action/get_resultRequest
result response    -> rr/move/_action/get_resultReply
feedback           -> rt/move/_action/feedback
status             -> rt/move/_action/status
```

不要在 Action 代码再写另一套 `rq/rr/rt` resolver。

### 15.3 default QoS

Action client/server default：

```text
goal service   -> ros2_services_default
cancel service -> ros2_services_default
result service -> ros2_services_default
feedback topic -> ros2_default
status topic   -> ros2_action_status_default
```

Status：

```text
KeepLast depth=1
Reliable
TransientLocal
```

### 15.4 result timeout

default 10 s，由 `ActionServerOptions` 定义。

这是 Action terminal Goal retention，不是 DDS response writer max-blocking-time；二者不得混淆。

## 16. Teardown protocol

### 16.1 endpoint destruction order

一般 endpoint：

```text
mark Closing
    ↓
prevent new operation
    ↓
auto-detach WaitSet registration
    ↓
detach listener / prevent new callback
    ↓
drain in-flight callback
    ↓
delete DataReader/DataWriter
    ↓
release Topic reference
    ↓
release TypeRegistry reference
    ↓
release backing
```

### 16.2 Client/Server

先使 composite public state Closing，再对两侧 endpoint执行 reverse-order cleanup；中间任何 delete failure 不阻止其余可安全 cleanup。

### 16.3 Action

先使 aggregate WaitableState Closing，再 detach public Action registration，然后逆创建顺序删除五个 endpoint。

GoalRegistry 是本地 state，在 transport endpoint不再可使用后清理。

### 16.4 Context final teardown

最后一个 child reference 释放后：

```text
stop discovery listener admission
    ↓
drain participant/endpoint callbacks
    ↓
delete remaining contained entities
    ↓
delete DDS Publisher/Subscriber containers
    ↓
unregister/release remaining types/topics where safe
    ↓
DomainParticipantFactory::delete_participant
```

若 `delete_contained_entities()` 或 `delete_participant()` 返回 failure：

- 记录 diagnostic；
- 继续执行仍可证明安全的 cleanup；
- 不能证明不再被 middleware 引用的 backing 进入 conservative retention；
- destructor 不抛异常。

## 17. Locking 与 deadlock 规则

### 17.1 原则

不要建立一个覆盖全部 DMW 的 global mutex。

主要同步域独立：

```text
Context lifecycle
Type/Topic registry
DiscoveryGraph
WaitSet topology
endpoint operation state
Server pending requests
Timer state
Action GoalRegistry
EventSource
```

### 17.2 listener rule

Fast DDS listener callback：

- 持有 internal state lock 的时间尽可能短；
- 不在持 graph/endpoint mutex 时调用 user code；
- 不在持 graph mutex 时触发可能获取 WaitSet topology mutex 的长路径；
- 先提交 state、收集通知 target，释放 lock 后再 wake。

### 17.3 delete rule

避免在持有 DiscoveryGraph/WaitSet topology mutex 时调用 Fast DDS delete API；Fast DDS delete 可能同步触发内部 callback/drain，从而造成锁反转。

### 17.4 operation vs destructor

Public contract 不支持 caller 无同步地对同一 facade执行普通 operation 与 destructor，因此实现无需为这种 misuse 建立重型引用计数 fast path。

WaitSet registered waitable destruction是例外，必须由 WaitableState/RegistrationState解决。

## 18. ReturnCode / exception mapping

### 18.1 ReturnCode

集中 helper：

```text
impl/fastdds/return_code.hpp
```

常见 mapping：

```text
RETCODE_OK -> success
RETCODE_TIMEOUT -> Timeout
RETCODE_OUT_OF_RESOURCES -> ResourceExhausted
BAD_PARAMETER/PRECONDITION_NOT_MET -> InvalidArgument/InvalidState where contract can distinguish
other middleware failures -> DDSError
```

不要在每个 endpoint 手工写一套不同 mapping。

### 18.2 bool Fast DDS API

某些 Fast DDS API 仍返回 `bool`。false 统一转成与 operation 对应的 `DDSError`，除非 public contract 明确定义 false 为 non-error no-data。

### 18.3 C++ exception

DMW expected failure 通过 Result；`std::bad_alloc` 允许传播。

析构路径捕获/记录不得传播的 unexpected exception，并保持 noexcept。

## 19. Source compatibility strategy

### 19.1 primary path

生产实现优先使用 Fast DDS 2.14.x public DDS-PIM API。

### 19.2 Humble compatibility shim

只在实际 2.6 build 证明存在 API 差异时添加：

```text
impl/fastdds/compat/*
```

或者极小的 header helper。

禁止在 public header 或业务实现到处散布：

```cpp
#if FASTDDS_VERSION_MINOR ...
```

### 19.3 不追求跨 minor ABI

同一 DMW binary 不要求同时加载 Fast DDS 2.6 与 2.14。

目标是：

```text
same DMW source
    -> build against target Fast DDS
```

而不是跨 Fast DDS minor binary ABI。

## 20. Verification Matrix

### 20.1 primary CI — Jazzy / Fast DDS 2.14.x

必须运行：

```text
build
unit tests
DMW integration tests
ROS Topic bidirectional interoperability
ROS Service bidirectional interoperability
ROS Action bidirectional interoperability
QoS golden tests
WaitSet race tests
shutdown/teardown tests
ASan/UBSan
selected TSan
```

### 20.2 compatibility CI — Humble / Fast DDS 2.6.x

如果项目继续声明 Humble compatibility，则运行：

```text
source build
foundation unit tests
Topic interoperability
Service interoperability
Action compatibility once Action implementation lands
```

遇到 2.6-only limitation时，先判断是否可通过 private shim 解决；不要修改 DMW public API 以迎合旧 minor。

### 20.3 Context tests

```text
participant create rollback
listener installed at creation
publisher/subscriber container rollback
multiple Context / domains
shutdown with active WaitSet
Context facade destroyed before children
participant delete failure conservative retention
```

### 20.4 QoS tests

验证 public Qos -> Fast DDS writer/reader fields，包括：

```text
history/depth
reliability
durability
deadline/lifespan
liveliness/lease
ROS2 memory policy
ROS2 synchronous publish mode
ROS2 data_sharing off
all common profiles
```

### 20.5 Topic tests

```text
write/read
invalid data filtering
MessageInfo
matched count
QoS incompatible endpoint
listener teardown
```

### 20.6 Service tests

```text
request identity
response identity
multi-client routing
pending capacity reservation
duplicate suppression
response writer target already matched
match before effective deadline
target confirmed gone
max_blocking deadline timeout
Context shutdown during wait
participant-paired availability
```

不得继续以“固定 100 ms”作为测试 oracle；应从 response DataWriter effective QoS 取得期望 timeout。

### 20.7 WaitSet tests

```text
poll / finite / infinite
no fixed periodic slice in normal path
pre-existing data ready before native wait
control wake
add/remove while waiting
original finite deadline preserved
Timer deadline wakes infinite wait
Action expiry deadline wakes wait
GraphEvent wakes wait
GuardCondition trigger/consume race
waitable destroy while waiting
second concurrent wait -> Busy
```

特别验证 Jazzy `rmw_wait` 已暴露过的问题类型：

- GuardCondition untrigger race；
- ready entity存在时 return status；
- attach/detach 后 recheck；
- data arrives around attach boundary。

### 20.8 Timer tests

按 `dmw.md` Timer contract：

```text
zero period
cancel/reset
exchange period
missed-cycle re-align
concurrent consume
WaitSet deadline integration
shutdown
```

### 20.9 Graph tests

用 synthetic discovery record 与真实 Fast DDS discovery 两套测试：

```text
participant add/remove
reader/writer add/change/remove
revision no-op suppression
service same-participant pairing
action five-endpoint composition
GraphSnapshot normalized names
GraphEvent cursor
```

### 20.10 Action tests

分两层：

**pure common-state tests**：

```text
Goal FSM
cancel selection
pending result requests
terminal expiry
status snapshot
```

不启动 DDS 即可测试。

**Fast DDS integration tests**：

```text
five-endpoint transaction
aggregate readiness
request/response correlation
feedback/status data path
availability
rollback/teardown
ROS 2 interop
```

## 21. 当前实现相对本规格的重点迁移项

本节用于开发审查，不改变 public contract。

当前代码已具有较完整的 Foundation，但对新基线仍有以下重点工作：

1. **构建/验证基线**：Primary CI 从 Humble/2.6 导向 Jazzy/2.14；CMake package 仍使用 `fastrtps`。
2. **Participant creation**：从 create 后 `set_listener()` 优化为 listener 随 `create_participant()` 一次安装，并重新审计 Publisher/Subscriber container QoS 来源。
3. **QoS**：保留 PREALLOCATED_WITH_REALLOC、SYNCHRONOUS、data sharing off 的 ROS2 validated behavior；新增统一 common profiles；response wait 改为读取 writer effective max_blocking_time。
4. **WaitSet**：由固定 100 ms slice 改为 native Fast DDS WaitSet + control GuardCondition + logical pre/re-check + Timer/Action absolute deadline。
5. **Graph**：把已有 internal DiscoveryGraph 提升为唯一 service/action availability 与 public snapshot/change authority。
6. **Timer**：新增纯 runtime state，不增加 thread。
7. **Action**：新增 3 Service + 2 Topic aggregate、Goal FSM、cancel/result/expiry common state；Future与 typed payload cache保持上层。
8. **teardown**：保留现有 conservative lifetime safety，但删除只为旧 2.6 假设存在、且 2.14/Jazzy cross-audit 无依据的过度机制。

## 22. Frozen Implementation Invariants

1. Fast DDS 2.14.x 与 `rmw_fastrtps` Jazzy 是平等主要参考。
2. DMW 不依赖 ROS 2 runtime package。
3. 普通 public API 不暴露 Fast DDS 类型。
4. Participant listener 应在 Participant 创建时安装，避免 discovery gap。
5. TypeRegistry/TopicRegistry 是 Context mandatory authority。
6. ROS2 endpoint baseline 使用 PREALLOCATED_WITH_REALLOC memory policy。
7. ROS2 endpoint baseline关闭 data sharing。
8. ROS2 writer baseline默认 synchronous publication，与 Jazzy `rmw_fastrtps` default behavior 对齐。
9. Service response-reader wait 从 effective writer QoS `max_blocking_time` 派生。
10. Service availability按 remote participant 配对。
11. GraphSnapshot/GraphEvent复用同一个 DiscoveryGraph authority。
12. Timer 没有 DDS entity、没有 per-timer worker thread。
13. WaitSet 正常路径没有固定 100 ms periodic polling slice。
14. WaitSet 使用 Fast DDS native WaitSet/Condition + control GuardCondition + logical readiness recheck。
15. Timer/Action expiry deadline直接参与 native wait remaining calculation。
16. ActionClient/Server 对上层各只表现为一个 aggregate waitable。
17. Goal FSM/cancel selection/result RequestId/expiry state只有 DMW 一个 authority。
18. typed Action result payload cache不强行下沉。
19. listener 不执行 user callback。
20. delete failure 时 memory safety 优先于强制 free。
21. Fast DDS minor compatibility差异只存在 private implementation helper。
22. 不追求单一 binary 跨 Fast DDS minor ABI。

## 23. 结论

新的 Fast DDS 实现方向不再是“把旧 Humble/Fast DDS 2.6 规格逐项补绿”，而是：

```text
Fast DDS 2.14.x semantics
           +
rmw_fastrtps Jazzy production patterns
           +
rcl / rcl_action common-runtime boundaries
           +
DMW existing public/lifecycle invariants
           ↓
      DMW implementation
```

这意味着保留已经证明有价值的 Type/Topic registry、service identity、participant-paired availability、RAII 和 teardown safety，同时主动替换旧实现中不再合适的固定 timeout、固定 polling slice 和版本专属假设。

最终 DMW Fast DDS backend 应提供一套稳定、可测试、跨 `dclcpp` / `dclpy` 共用的 Topic / Service / Timer / Action / Graph / Wait runtime，而不是成为 `rmw_fastrtps` 的复制品，也不在上层重复构建第二套协议状态。
=======
Deadline/Lifespan/Liveliness/Lease使用checked duration conversion；禁止integer wrap。

### 3.11 actual_qos reverse mapping

`actual_qos()`必须从真实entity：

```cpp
DataWriter::get_qos()
DataReader::get_qos()
```

或2.6兼容等价API读取effective Qos，再reverse-map到`dmw::Qos`。

Fast DDS-only implementation字段不进入`dmw::Qos`。

如果某public field无法从当前Fast DDS API可靠恢复：

- 使用明确SystemDefault/unknown contract；
- 不根据创建时requested value伪造“actual”。

### 3.12 QoS compatibility

`check_qos_compatibility()`优先实现为DMW-owned pure common helper，覆盖public可表达policy。

可参考`rmw_dds_common`兼容性规则，但不依赖其runtime。

结果应区分：

```text
Compatible
Warning
Incompatible
```

并提供稳定reason，不把Fast DDS内部枚举字符串直接当public contract。

### 3.13 wait_for_all_acked / assert_liveliness

Publisher：

```text
wait_for_all_acked
    -> DataWriter wait_for_acknowledgments / compatible API

assert_liveliness
    -> DataWriter assert_liveliness / compatible API
```

finite timeout先转换为checked Fast DDS duration；Context shutdown如需要中断不可取消的middleware blocking API，优先采用可分段但由absolute deadline控制的private loop，不能重新引入固定100 ms public polling语义。

---

## 4. Topic 与 Service Fast DDS Data Path

### 4.1 Publisher::write

```text
validate message != nullptr
validate Context Active
enter OperationGuard
build WriteParams if protocol requires
DataWriter::write(...)
map return
leave OperationGuard
```

实现可以选择Fast DDS版本中最稳定、最能取得所需metadata的public overload；public DMW API不暴露`bool`/ReturnCode overload差异。

### 4.2 Subscriber::read

单次调用使用call-start finite candidate budget：

1. snapshot可扫描unread candidate数量；
2. take sample；
3. invalid/filtered sample消耗budget；
4. valid sample先落到temporary sample；
5. deserialize/conversion成功后transactional commit caller output；
6. budget耗尽 -> `success + false`。

并发arrival不得无限延长一个`read()`。

现有`TemporarySample`思路保留：`success + false`不修改caller output；sample consume前Error不修改output。

### 4.3 MessageInfo

优先从Fast DDS SampleInfo/identity提取：

```text
writer/sample identity
publication handle/GUID fallback
source timestamp
reception timestamp
sequence number（可用时）
```

不可获得时使用public unknown contract；禁止以本地clock伪造DDS timestamp。

### 4.4 matched count

使用Fast DDS publication/subscription matched status或等价synchronized state。

不要从DiscoveryGraph同名endpoint数量推导matched count，因为QoS-incompatible endpoint不应计入。

### 4.5 Service endpoint composition

Client：

```text
request writer
response reader
```

Server：

```text
request reader
response writer
```

create/rollback/destroy作为整体transaction。

内部endpoint可以复用Topic/Writer/Reader private factory，但不能以public Publisher/Subscriber形式注册独立WaitSet ownership。

### 4.6 ROS2 request write

ROS2 mode：

```cpp
WriteParams params;
params.related_sample_identity().writer_guid() = response_reader_guid;
request_writer->write(request, params);
```

successful write后使用生成的sample identity sequence标准化：

```text
RequestId.client_gid = response_reader Gid
RequestId.sequence_number = request sample sequence
```

与Jazzy `rmw_fastrtps` request correlation保持互操作。

### 4.7 Server take request / pending reservation

在destructive take前，先在Server pending-state同步域预留capacity：

```text
Pending + Responding + Reserved >= max_pending_requests
    -> ResourceExhausted
    -> 不消费DDS sample
```

成功take后读取：

```text
SampleInfo.sample_identity
SampleInfo.related_sample_identity
```

如果related writer GUID表示Client response reader，则作为response target；否则使用经过compatibility验证的fallback identity form。

预留slot在成功注册Pending RequestId后commit；take/deserialize失败必须释放reservation。

### 4.8 Response target match deadline

ROS2 response path参考Jazzy `rmw_fastrtps`：

```text
RequestId target reader
    ↓
response_writer->get_qos()
    ↓
reliability.max_blocking_time
    ↓
convert to one absolute steady deadline
    ↓
wait until target matched / gone / deadline / shutdown
```

禁止：

```text
const auto deadline = now + 100ms;
```

把当前默认Qos数值重新hard-code进Server实现。

目标状态：

```text
already matched          -> write
matched before deadline  -> write
confirmed gone           -> success without write
deadline expires         -> Timeout
Context shutdown         -> ContextShutdown
```

所有discovery wake/recheck共享同一个absolute deadline。

### 4.9 response write params

response关联到request identity：

```text
related_sample_identity = original request sample identity
```

或经Jazzy/Humble interoperability验证的等价构造。

write success -> Server pending entry移除。

write failure -> Responding回Pending，允许应用retry。

### 4.10 Client take response

Client过滤不属于自身response reader identity的response。

兼容性检查必须覆盖Jazzy与Humble实际identity form，不把某个版本私有workaround固化为public API。

`success + false`保持response和RequestId output不变。

### 4.11 Service availability

DiscoveryGraph remote endpoint record至少保存：

```text
participant prefix/Gid
endpoint GUID/Gid
reader/writer kind
resolved DDS topic
logical service name（可解析时）
wire type
QoS/match-relevant metadata
Node/entity metadata association（可用时）
```

candidate至少要求same participant下 compatible request reader + response writer。

如果graph metadata提供Node/entity association，进一步使用它减少错误composition。

`wait_for_service()`基于DiscoveryGraph revision/condition；不使用`sleep_for()`轮询。


---

## 5. DiscoveryGraph 与 ROS2 Graph Metadata

### 5.1 Internal records

DiscoveryGraph 推荐内部记录：

```text
RemoteParticipantRecord
RemoteEndpointRecord
RemoteNodeRecord
LocalNodeRecord
ServiceCandidateRecord
ActionCandidateRecord
GraphRevision
```

不要求直接把 public `GraphSnapshot` 当 mutable cache 存储；public snapshot应由内部authority构造。

### 5.2 ParticipantListener discovery

Participant listener至少处理：

```text
remote participant add/remove
DataWriter discovery add/change/remove
DataReader discovery add/change/remove
```

callback只提交normalized discovery record，不直接执行：

```text
service available callback
action available callback
GraphEvent user callback
Python/C++ application callback
```

listener callback的 graph mutation遵循：

```text
normalize
    ↓
compare old/new
    ↓
no public-observable change -> no-op
    ↓
commit state
    ↓
revision++
    ↓
collect notifications
    ↓
unlock
    ↓
wake
```

### 5.3 DDS name normalization

listener保存原始DDS identity，再尝试按RuntimeMode解析logical name。

ROS2 Topic：

```text
rt/<path> -> /<path>
```

ROS2 Service：

```text
rq/<path>Request
rr/<path>Reply
```

ROS2 Action基于：

```text
/_action/send_goal
/_action/cancel_goal
/_action/get_result
/_action/feedback
/_action/status
```

识别。

解析失败的普通DDS endpoint可以保留internal discovery state，但只有能够形成合法public logical entry时才进入对应GraphSnapshot view。

### 5.4 Local Node/entity metadata authority

每个Context内部维护：

```text
Participant Gid
    ↓
Node(name, namespace)
    ↓
local publisher/subscriber endpoint Gids
```

Service/Action内部endpoint仍作为reader/writer存在于entity set；metadata的public Node association由其owner NodeState提供。

local endpoint transaction commit后更新metadata；endpoint真正销毁/rollback后再移除。

metadata更新与GraphRevision commit必须一致，不能出现“public endpoint已commit但metadata永远缺失”的永久split-brain。

### 5.5 ROS2 `ros_discovery_info` transport

在 `RuntimeMode::ROS2` 下，为获得Node Graph interoperability，DMW创建internal graph metadata writer/reader，wire behavior参考Jazzy `rmw_fastrtps` / `rmw_dds_common`：

```text
topic: ros_discovery_info
avoid ROS namespace prefix remapping
wire payload compatible with ParticipantEntitiesInfo semantics
```

推荐QoS参考Jazzy：

**writer**：

```text
KeepLast(1)
Reliable
TransientLocal
```

**reader**：

```text
KeepAll
Reliable
TransientLocal
```

reader应忽略本地metadata publication，避免把自己的snapshot作为remote update回灌。

若generated keyed-topic support不足，兼容Jazzy behavior使用KeepAll reader，并通过participant Gid在common state中覆盖同一participant最新snapshot。

### 5.6 Internal graph metadata type boundary

DMW不依赖ROS 2 runtime package，但可以在private/generated-integration层提供与`rmw_dds_common::msg::ParticipantEntitiesInfo` wire-compatible的internal type。

普通public header不出现：

```text
rmw_dds_common::*
rosidl::*
```

实现可以：

- vendoring/生成一份只用于internal wire compatibility的IDL/type；或
- 通过项目统一standard-interface生成链路提供compatible type；

但最终binary不要求链接`rmw_dds_common` runtime。

### 5.7 Graph metadata publish transaction

local Node/endpoint graph变化后：

```text
commit local graph state
    ↓
revision++
    ↓
build complete ParticipantEntitiesInfo-style snapshot
    ↓
publish metadata
    ↓
wake local GraphEvent / availability wait
```

metadata publish failure：

- 不回滚已经成功创建/删除的本地communication entity；
- 标记graph metadata transport degraded并记录diagnostic；
- public GraphSnapshot仍反映本地真实state；
- ROS2 remote graph interoperability可能暂时stale；
- 后续metadata change / retry opportunity可以重新发布complete snapshot。

因为payload是participant complete state，不需要可靠重放每一个delta。

### 5.8 Remote graph metadata receive

internal reader收到remote participant snapshot后：

1. validate participant Gid / message structure；
2. 忽略local participant；
3. normalize Node name/namespace；
4. replace/update该participant的NodeEntitiesInfo snapshot；
5. compare public-observable graph state；
6. real change才revision++；
7. wake GraphEvent/service/action waiters。

重复相同snapshot不得增加revision。

### 5.9 Topic endpoint info

RemoteEndpointRecord至少保留：

```text
endpoint Gid
participant Gid
reader/writer kind
DDS topic name
logical topic name
wire type name
discovery-visible Qos
optional Node association
```

GraphSnapshot生成`TopicEndpointInfo`时：

- Node association存在 -> 填node_name/namespace；
- 无法可靠关联 -> 使用empty/unknown contract，不猜Participant name；
- Qos字段从DDS discovery/effective metadata reverse map；
- 不暴露Fast DDS locator/entity pointer。

### 5.10 Service candidate composition

Service candidate要求same participant的：

```text
request reader
response writer
```

并验证：

```text
logical service name一致
request/response wire type对应
QoS/matching条件满足当前availability定义
```

如果两endpoint都有Node association，优先要求Node association一致。

如果没有足够metadata证明具体remote object identity，只形成candidate，不生成虚构Server Gid。

### 5.11 Action candidate composition

Action server candidate要求same participant：

```text
SendGoal request reader + response writer
CancelGoal request reader + response writer
GetResult request reader + response writer
Feedback writer
Status writer
```

至少保证：

- normalized action logical name一致；
- wire descriptors compatible；
- same participant；
- Node metadata可用时保持同Node association。

不跨participant拼接。

### 5.12 GraphSnapshot

snapshot步骤：

```text
lock DiscoveryGraph
    ↓
copy one consistent immutable public view
    ↓
record revision
    ↓
unlock
    ↓
return
```

如果snapshot构造需要heap allocation，`std::bad_alloc`允许传播；不得在持graph mutex时catch并伪装为DDSError。

### 5.13 GraphEvent

GraphEvent保存：

```text
cursor_revision
registered WaitSet weak state
closing state
```

revision change后：

- event logical ready iff current_revision > cursor；
- registered WaitSet触发control guard；
- WaitSet返回ready不推进cursor；
- `take()`成功才推进cursor。

GraphEvent创建时cursor=current revision，不重放之前history。

---

## 6. Clock、Timer、WaitSet、GuardCondition 与 Event 实现

### 6.1 Clock implementation

Clock facade持有shared ClockState。

**System**：

- `now()`使用system/realtime source；
- wall-clock jump允许；
- Timer计算时以该clock当前time point为事实。

**Steady**：

- `now()`使用monotonic source；
- 不受wall-clock jump影响。

**Ros**：

```text
override disabled -> follow System now
override enabled  -> atomic/synchronized override value
```

Ros Clock update本身没有DDS dependency。

如果未来添加DMW-owned `/clock` transport，可以直接调用同一ClockState update primitive；V1不允许另建第二套RosClock state。

### 6.2 Clock dependent Timer registration

Timer创建时把ClockState保存为shared backing，并向ClockState登记weak Timer/WaitSet wake target。

不要让ClockState持有强public Timer ownership。

Timer销毁时解除registration；Clock facade先销毁不影响Timer，因为backing由Timer保持。

### 6.3 Timer state

Timer只保存：

```text
ClockState backing
period
canceled
last_call_time
next_call_time
registered WaitSet state
```

不创建DDS entity，不创建per-timer thread。

Timer state优先使用小粒度mutex，而不是多字段lock-free CAS protocol：

- period/next/last/canceled是组合state；
- reset/cancel/exchange/consume需要原子更新多个字段；
- Timer不是硬实时fast path。

### 6.4 Timer create/reset

创建/autostart：

```text
now = clock.now()
period = options.period
last_call = now
next_call = now + period
canceled = !autostart
```

所有time arithmetic checked。

reset同样以clock current time作为新的schedule origin。

### 6.5 Timer consume

successful consume：

```text
expected = next_call_time
actual = clock.now()
elapsed = actual - last_call_time
last_call_time = actual
```

period > 0：

```text
next = expected + period
if next <= actual:
    missed = 1 + (actual - next) / period
    next += missed * period
```

period == 0：

```text
next = actual
```

保持period grid，不用`actual + period`造成callback-latency drift。

### 6.6 ROS-time Timer semantics

Ros override active时不能把future ROS deadline简单映射为一个steady_clock sleep duration后就假设ready，因为ROS time可能pause/backward/forward jump。

WaitSet策略：

- pre-check所有Timer logical readiness；
- steady/system timer可贡献native remaining deadline；
- Ros override timer如果当前未ready，不根据steady elapsed自行推进；
- Clock update触发WaitSet control GuardCondition；
- wake后重新调用Clock now / Timer readiness；
- forward jump可以立即使多个Timer ready；
- backward jump只改变time_until_next_call，不回滚已经successful consume的history。

### 6.7 WaitSet normal algorithm

正常路径不使用固定`100 ms` polling slice：

```text
logical pre-check
    ↓
snapshot topology generation
    ↓
attach native conditions
    ↓
compute original user absolute deadline
    ↓
compute earliest steady-convertible runtime deadline
    ↓
Fast DDS WaitSet::wait(remaining minimum)
    ↓
reconcile topology/control generation
    ↓
logical re-check
    ↓
Ready / Timeout / continue with original deadline
```

### 6.8 Private control GuardCondition

每个DMW WaitSet持有private Fast DDS GuardCondition，用于：

```text
add/remove
auto-detach/waitable destroy
Timer reset/cancel/period change
Ros Clock update
Graph revision
Action expiry/topology change
Context shutdown
```

control guard不进入public WaitResult。

### 6.9 Native reader conditions

Subscriber/Client/Server/Action reader path使用DataReader StatusCondition的`DATA_AVAILABLE_STATUS`或版本兼容helper提供的更精确public readiness API。

关键模式保留：

1. wait前pre-check；
2. 未ready才attach/wait；
3. native wake后重新check；
4. data arrives around attach boundary不得丢wake；
5. GuardCondition reset与concurrent trigger不得丢新trigger。

Fast DDS 2.14.x若提供更合适unread-info query，可封装在：

```text
src/impl/fastdds/reader_readiness.*
```

2.6.x使用stable fallback；差异不得泄漏public API。

### 6.10 Composite registration

一个RegistrationState可对应多个native condition：

```text
Subscriber -> data reader condition
Client -> response reader
Server -> request reader
ActionClient -> goal/cancel/result response + feedback + status
ActionServer -> goal/cancel/result request + logical expiry
```

任意子condition ready，只返回public composite token一次，并生成detail mask snapshot。

### 6.11 Topology generation

WaitSet保存monotonic topology generation。

add/remove/auto-detach：

```text
mutate logical registration
    ↓
generation++
    ↓
trigger control guard
```

active wait发现generation改变时重建native snapshot，但继续使用原始caller deadline。

### 6.12 Deadline calculation

caller finite：

```text
user_deadline = steady_now + timeout
```

caller infinite：无user deadline。

native wait remaining：

```text
min(
    user deadline remaining if finite,
    earliest steady/system timer remaining,
    earliest Action expiry remaining)
```

Ros override Timer依赖clock update wake而不是steady deadline。

如果没有deadline，使用Fast DDS infinite wait。

### 6.13 WaitResult detail snapshot

native wake后必须重新evaluate所有logical registration，并生成：

```text
WaitableRegistration
WaitableKind
readiness detail mask
```

Action sub-channel bits在形成WaitResult时snapshot，避免Executor稍后调用`readiness()`才知道“当时为何ready”。

`ActionClient::readiness()`/`ActionServer::readiness()`仍可以重新检查current state，但与既有WaitResult不是同一snapshot概念。

### 6.14 Waitable destruction safety

WaitSet不长期依赖raw public facade pointer。

使用：

```text
WaitableState
RegistrationState
native condition hold/reference
```

waitable destructor：

```text
mark Closing
    ↓
invalidate registration
    ↓
wake active WaitSet
    ↓
drain active native snapshot reference
    ↓
detach native condition
    ↓
destroy native/DDS resource
```

WaitSet destructor前置条件：自身没有active `wait()`。

### 6.15 Public GuardCondition logical generation

GuardCondition保存：

```text
trigger_generation
consumed_generation
```

多trigger可coalesce，但new trigger不得被concurrent reset吞掉。

trigger transaction推荐：

1. validate Context Active；
2. under state mutex stage next generation；
3. `Fast DDS GuardCondition::set_trigger_value(true)`；
4. success后commit logical generation；
5. failure则本次public trigger失败，不commit logical success。

这样满足：native wake失败时`trigger()`直接返回`DDSError`。

### 6.16 GuardCondition consume

WaitSet recheck确认ready后snapshot generation；形成Ready result时推进相应consumed generation。

如果consume/reset过程中又出现新generation：

- native trigger保持true；
- 不允许把新trigger reset掉。

否则可以reset native condition false。

### 6.17 EventSource cumulative state

Publisher/Subscriber listener把Fast DDS status变化转换为`EventSourceState` cumulative generation/value。

每个public Event有独立cursor。

不要让多个Event直接竞争消费同一个Fast DDS destructive status query。

listener update：

```text
update cumulative source state
    ↓
collect Event/WaitSet notifications
    ↓
wake after releasing source lock
```

Event ready：

```text
source generation > event cursor
```

WaitSet不推进cursor；`Event::take()`推进。

### 6.18 Event parent teardown

endpoint delete前：

- mark EventSource closing；
- auto-detach child Event registrations；
- prevent new listener update；
- drain listener；
- delete DataWriter/DataReader；
- child Event后续operation在Context Active时返回`ParentDestroyed`。

---

## 7. Action Fast DDS 与 Common-State 实现

### 7.1 ActionClient composition

ActionClient::Impl拥有：

```text
SendGoal client pair
CancelGoal client pair
GetResult client pair
Feedback DataReader
Status DataReader
availability state
aggregate WaitableState
```

这些internal endpoint不向Client Library暴露成public `Client` / `Subscriber`，也不具有独立public WaitSet registration ownership。

### 7.2 ActionServer composition

ActionServer::Impl拥有：

```text
SendGoal server pair
CancelGoal server pair
GetResult server pair
Feedback DataWriter
Status DataWriter
GoalRegistry
result expiry state
aggregate WaitableState
```

### 7.3 Five-endpoint create transaction

```text
resolve action name/remap
    ↓
derive five logical names
    ↓
validate five descriptors/Qos
    ↓
reserve Type/Topic registry refs
    ↓
prepare listeners/wait states
    ↓
create all service/topic endpoints
    ↓
install graph/local metadata association
    ↓
commit aggregate Action object
```

任何一步失败逆序rollback全部资源。

不得返回“3个service创建成功但status writer失败”的partial Action。

### 7.4 Action native readiness

ActionClient native reader channels：

```text
goal response
cancel response
result response
feedback
status
```

ActionServer native reader channels：

```text
goal request
cancel request
result request
```

ActionServer另有logical：

```text
goal expiry deadline
```

WaitSet recheck时把这些状态编码为detail mask。

### 7.5 GoalRegistry

GoalRegistry不是Fast DDS object。

建议：

```text
unordered_map<GoalId, GoalRecord>
```

或等价container；public contract不绑定具体container。

GoalRecord至少保存：

```text
GoalInfo
GoalState
terminal steady timestamp optional
pending result RequestIds
expiry state
```

GoalRegistry使用dedicated mutex/synchronization domain。

### 7.6 Accept transaction implementation

`dmw.md`只冻结observable atomicity；本实现规格冻结推荐实现步骤：

```text
validate Context/ActionServer/RequestId/GoalInfo
    ↓
lock Action state transaction domain
    ↓
verify RequestId belongs to pending SendGoal request
    ↓
verify GoalId not committed/reserved
    ↓
reserve GoalId
    ↓
preallocate/prepare GoalRecord
status bookkeeping
expiry bookkeeping
container capacity/insertion node
all local resources needed after wire write
    ↓
build no-fail local commit handle
    ↓
unlock only if implementation can preserve transaction state safely,
or keep narrow transaction lock through write according to deadlock audit
    ↓
write accepted SendGoal response
    ├── failure -> rollback reservation/prepared state
    │             preserve/recover SendGoal request retry state
    └── success -> no-allocation/no-failable local publish of GoalRecord
                  state = Accepted
                  optional Execute transition
                  return GoalTransition
```

关键实现不变量：

```text
response write success之后
不得再执行可能返回recoverable failure的heap allocation、map insertion、registry acquisition
```

如果容器实现无法提供预allocation/no-fail publish路径，应改变private storage strategy，而不是削弱public atomicity contract。

### 7.7 Accept transaction lock/deadlock rule

不能因为atomicity要求就盲目在持GoalRegistry mutex时调用所有Fast DDS operation。

实现必须审计：

- response writer write是否同步触发可能反向获取Action/graph lock的listener；
- participant discovery callback lock order；
- Server pending RequestId lock与GoalRegistry lock顺序。

推荐固定lock order并把“reservation token”作为private transaction state，使wire write与local commit保持atomic observable semantics，同时避免跨middleware callback路径长期持有高层mutex。

### 7.8 Cancel selection

`select_cancel_goals()`在GoalRegistry stable snapshot/lock下执行pure selection：

```text
exact id
all cancelable
accepted at/before timestamp
exact OR time-bound
```

不调用user callback，不改变GoalState。

Client Library user callback接受后调用`update_goal_state(CancelGoal)`提交Canceling。

### 7.9 Result request state

GetResult transport take后，typed binding从raw request提取GoalId，再调用DMW common state：

```text
register_result_request(goal_id, RequestId)
```

active -> 保存RequestId。

terminal -> 不保存，返回Terminal。

unknown -> UnknownGoal。

terminal transition把pending RequestId一次性move/take给上层typed result responder。

### 7.10 Typed result cache boundary

DMW禁止为了缓存arbitrary result object而新增：

```text
runtime reflection
generic object clone ABI
serialized-message public subsystem
language-dependent allocator callback
```

Goal lifecycle/expiry属于DMW；typed payload storage留dclcpp/dclpy。

### 7.11 Result expiry

terminal commit：

```text
terminal_time = steady_clock::now()
expiry = terminal_time + result_timeout
```

result_timeout是local retention duration，与GoalInfo.accepted protocol timestamp无关。

不创建background expiry thread。

ActionServer注册WaitSet时earliest expiry参与native deadline；未注册时GoalRegistry operation / `take_expired_goals()`可lazy prune。

expiry状态变化需要触发registered WaitSet control wake。

### 7.12 Status snapshot

在GoalRegistry同步域内构造stable `vector<GoalStatusInfo>`，然后释放lock返回。

不在持GoalRegistry lock时调用typed status message builder或Fast DDS publish。

上层构造typed status message后调用ActionServer status publish path。

### 7.13 Action availability

DiscoveryGraph按`dmw.md` candidate semantics组合五类endpoint。

实现优先级：

1. same participant；
2. action logical name一致；
3. endpoint wire type/Qos compatible；
4. ROS graph metadata可用时same Node association；
5. 不跨participant拼接。

不生成DDS无法证明的“唯一ActionServer object id”。

### 7.14 Action naming/Qos

Action `/move`：

```text
send_goal request  -> rq/move/_action/send_goalRequest
send_goal response -> rr/move/_action/send_goalReply
cancel request     -> rq/move/_action/cancel_goalRequest
cancel response    -> rr/move/_action/cancel_goalReply
result request     -> rq/move/_action/get_resultRequest
result response    -> rr/move/_action/get_resultReply
feedback           -> rt/move/_action/feedback
status             -> rt/move/_action/status
```

复用统一Service/Topic resolver，不在Action模块复制prefix mapping。

默认：

```text
goal/cancel/result service -> ros2_services_default
feedback                   -> ros2_default
status                     -> ros2_action_status_default
```

`result_timeout` default 10 s；不得与response writer reliability `max_blocking_time`混淆。


---

## 8. Parameter Common-State 与 Remapping 实现

### 8.1 ParameterStoreState

每个NodeState拥有唯一ParameterStoreState：

```text
parameter map
parameter descriptor map
initial override map
store revision
allow_undeclared policy
mutex
```

ParameterStore本身不创建DDS entity。

不要在dclcpp和dclpy各自保存“真实参数值”副本；language wrapper只做view/conversion/callback bookkeeping。

### 8.2 ParameterValue storage

内部可以使用适合C++17的type-erased/value storage，但普通public API不暴露`std::variant`依赖。

内部storage必须满足：

- copy/move行为明确；
- array/string allocation failure允许`std::bad_alloc`传播；
- wrong public typed accessor不通过silent conversion修正类型；
- integer统一使用`std::int64_t`；
- double不自动与integer互转；
- byte array与integer array保持不同type identity。

### 8.3 Parameter name validation

Parameter name validation集中在common helper，不在Node methods重复。

至少检查：

- empty name；
- illegal separator/token；
- namespace-like dotted name canonical rule；
- list-prefix/depth operation的一致matching semantics。

如果DCL选择与ROS 2 parameter name规则兼容，应通过golden tests锁定，不调用rcl parser runtime。

### 8.4 Descriptor validation

declare/set common validator处理：

```text
read_only
dynamic_typing
fixed type
integer range + step
floating range + step
```

floating step比较应使用稳定epsilon/rounding strategy并有tests，不把编译器偶然浮点结果当contract。

同一descriptor多个range如何解释必须固定；V1建议与ROS 2常用语义一致：一个parameter value需要匹配descriptor声明的有效range约束，非法descriptor在declare时直接`InvalidArgument`。

### 8.5 Override resolution

Node create时从global/local Arguments解析出applicable override table并freeze到NodeState。

`declare_parameter(ignore_override=false)`：

```text
lookup canonical name
    ↓
override exists ? override value : caller default
    ↓
validate descriptor/type/range
    ↓
insert declared state
```

`ignore_override=true`跳过override lookup但不修改stored override table。

不要在每次declare时重新parse raw argv/YAML。

### 8.6 Atomic set

`set_parameters_atomically()`：

```text
validate input names/duplicates
    ↓
lock store
    ↓
validate current declared/read-only/type/range state
    ↓
prepare all new values + ParameterChangeSet
    ↓
all allocation complete
    ↓
commit all entries as one store revision
    ↓
unlock
    ↓
return change set
```

任一参数失败：

```text
no value changed
store revision unchanged
no partial ParameterChangeSet
```

### 8.7 validate-then-callback-then-commit race

由于user callback不进入DMW，Client Library可能执行：

```text
validate_parameters
    ↓
user callbacks
    ↓
set_parameters_atomically
```

DMW必须在最终commit重新验证current store state。

如果callback期间另一个mutation改变了相关state：

- 不使用stale pre-validation；
- final set返回`Busy`或对应validation Error；
- 不自动重复执行user callback。

Client Library建议以Node-local language callback mutex串行化parameter user mutation；DMW不关心该mutex的语言实现。

### 8.8 ParameterChangeSet

commit前准备：

```text
new parameters
changed parameters
deleted parameters
```

commit成功后返回stable value copies。

Client Library使用它构造typed ParameterEvent；DMW不直接include ROS parameter generated message。

### 8.9 Parameter service transport

当前DMW V1不把ROS parameter service generated types硬编码进common runtime，因此不创建：

```text
GetParameters
SetParameters
ListParameters
DescribeParameters
```

等typed service endpoint。

但是：

- service callback中的所有store/validation/override语义必须直接调用DMW Node Parameter API；
- dclcpp/dclpy不得各自重写range/type/atomic set逻辑；
- 如果后续引入DCL统一standard-interface native binding，应优先把这组固定service endpoint composition下沉到一个DMW aggregate，而不是长期复制两份protocol orchestration。

---

## 9. Teardown、Locking、ReturnCode 与异常边界

### 9.1 Listener backing lifetime

Listener backing至少存活到：

1. 对应DDS entity不再可能开始新callback；
2. 已进入callback全部退出。

建议：

```text
closing flag
+
in-flight callback counter/shared lifetime guard
```

teardown：

```text
mark closing
    ↓
detach/disable listener or delete entity
    ↓
drain already-entered callbacks
    ↓
release listener backing
```

### 9.2 General endpoint destruction order

```text
mark public/Impl Closing
    ↓
prevent new OperationGuard admission
    ↓
auto-detach WaitSet registration
    ↓
remove local graph metadata association
    ↓
detach listener / prevent new callback
    ↓
drain callback
    ↓
delete DataReader/DataWriter
    ↓
release TopicRegistry reference
    ↓
release TypeRegistry reference
    ↓
release backing
```

Graph metadata publish可以在local state commit后异步/best-effort完成；不能因为metadata publish failure把DDS entity resurrection回来。

### 9.3 Client / Server teardown

先把composite public state置Closing，再逆创建顺序清理request/response两侧endpoint。

Server Pending/Responding map：

- teardown开始后不允许新read/write admission；
- 已进入writer operation按OperationGuard规则完成/被shutdown打断；
- 最终本地pending state直接销毁，不产生额外wire response。

某一DDS delete failure不阻止其余可证明safe的cleanup。

### 9.4 Action teardown

顺序：

```text
mark aggregate Closing
    ↓
detach public Action WaitSet registration
    ↓
stop new Goal/result operation admission
    ↓
remove local graph metadata associations
    ↓
reverse-delete five endpoint groups
    ↓
drain listeners
    ↓
clear GoalRegistry/local expiry state
```

GoalRegistry是local state，不需要wire tombstone。

### 9.5 Graph metadata transport teardown

Context final teardown前停止internal `ros_discovery_info` admission：

- metadata reader不再更新DiscoveryGraph；
- writer不再发布new local snapshot；
- drain internal reader/listener；
- delete metadata reader/writer；
- metadata type/topic references最后释放。

内部metadata endpoints不参与public WaitSet teardown。

### 9.6 Context final teardown

最后一个child backing释放后：

```text
stop new discovery/listener admission
    ↓
wake all waits
    ↓
drain active operations
    ↓
drain participant/endpoint callbacks
    ↓
delete remaining contained entities
    ↓
delete internal graph metadata endpoints
    ↓
delete DDS Publisher/Subscriber containers
    ↓
unregister/release types/topics where safe
    ↓
delete DomainParticipant
```

如果`delete_contained_entities()` / `delete_participant()`失败：

- 记录diagnostic；
- 继续仍可证明safe的cleanup；
- 不能证明middleware已放弃引用的backing进入conservative retention；
- destructor不抛异常。

### 9.7 Process-lifetime conservative retention

ProcessLifetime只能作为“Fast DDS teardown失败且引用安全无法证明”的最后安全网，不能成为普通路径资源管理方式。

必须记录至少：

```text
retained participant count
retained reader/writer listener count
retained type/topic backing count
last retention reason / error category
```

长期压力测试应能观测异常retention growth。

### 9.8 Lock domains

禁止一个覆盖所有DMW的global mutex。

推荐logical domains：

```text
Context lifecycle
ClockState
TypeRegistry
TopicRegistry
DiscoveryGraph
WaitSet topology
endpoint operation state
Server pending request state
Timer state
Action GoalRegistry
ParameterStore
EventSource
```

### 9.9 Lock order / listener rules

统一原则：

- Fast DDS listener持internal lock尽可能短；
- 不在持graph/endpoint/action/parameter lock时执行user code；
- graph mutation先commit，收集wake target，release graph lock后trigger；
- 不在持DiscoveryGraph/WaitSet topology锁时调用Fast DDS delete；
- 不在持GoalRegistry锁时调用可能同步进入反向lock path的未知Fast DDS operation，除非经过明确deadlock audit；
- Server pending lock与Action GoalRegistry lock顺序必须固定并文档化。

可继续使用private lock-rank debug helper检测违反顺序的路径，但lock rank不进入public API。

### 9.10 ReturnCode mapping

集中helper：

```text
src/impl/fastdds/return_code.*
```

common mapping intent：

```text
RETCODE_OK                  -> success
RETCODE_TIMEOUT             -> Timeout
RETCODE_OUT_OF_RESOURCES    -> ResourceExhausted
BAD_PARAMETER               -> InvalidArgument（可准确映射时）
PRECONDITION_NOT_MET        -> InvalidState（可准确映射时）
other Fast DDS failure      -> DDSError
```

不要在各endpoint复制不同switch。

Fast DDS版本差异通过compat helper先归一，再进入同一DMW mapping。

### 9.11 bool APIs

Fast DDS某些API仍返回bool。

`false`只有在public contract定义为ordinary “no data/no change/not matched yet”时才能返回`success + false`；否则必须转成对应Error。

### 9.12 `std::bad_alloc`

`std::bad_alloc`遵循`dmw.md`：允许传播。

禁止：

```text
catch bad_alloc -> ResourceExhausted
catch(...) -> DDSError
```

把allocation failure吞进普通middleware Error。

如果需要rollback：

```text
catch bad_alloc
    -> rollback local/native resources
    -> rethrow
```

### 9.13 Other C++ exceptions

DMW expected runtime failure使用Result。

private standard-library operation的unexpected exception：

- 如果public contract允许传播（例如bad_alloc）则原样传播；
- 其他unexpected exception应被视为programming/internal failure并保持diagnostic；
- 不要用`catch(...)`把所有异常不加区分地映射成DDSError。

noexcept destructor内部必须捕获不能传播的异常并继续safe teardown。

### 9.14 Source compatibility shim

生产path优先Fast DDS 2.14.x public API。

只有实际2.6 build证明存在差异时新增：

```text
src/impl/fastdds/compat/*
```

或极小private header。

禁止在业务实现和public header到处散布：

```cpp
#if FASTDDS_VERSION_MINOR ...
```

不追求单一binary同时兼容Fast DDS 2.6与2.14 ABI；目标是同一DMW source分别针对目标版本构建。

---

## 10. Verification、当前迁移项与 Frozen Implementation Invariants

### 10.1 Primary CI — Jazzy / Fast DDS 2.14.x

必须运行：

```text
build
unit tests
DMW integration tests
Topic bidirectional ROS interoperability
Service bidirectional ROS interoperability
Graph metadata/node discovery interoperability
Action bidirectional ROS interoperability
QoS golden/actual/compatibility tests
WaitSet race tests
Clock/Timer tests
Parameter common-state tests
shutdown/teardown tests
ASan/UBSan
selected TSan
```

### 10.2 Compatibility CI — Humble / Fast DDS 2.6.x

如果继续声明Humble compatibility：

```text
source build
foundation unit tests
Topic interoperability
Service interoperability
Graph metadata compatibility
Clock/Timer common-runtime tests
Action compatibility once Action lands
```

遇到2.6-only limitation先通过private shim评估，不修改DMW public API迎合旧minor。

### 10.3 Context / lifecycle tests

```text
Participant create rollback
listener installed at creation
Publisher/Subscriber container rollback
ROS2 graph metadata transport rollback
multiple Context/domain
Context facade destroyed before children
shutdown with active WaitSet
shutdown with active service availability wait
shutdown with Ros Clock/Timer
participant delete failure conservative retention
retention diagnostic counters
```

### 10.4 Type / Topic / QoS tests

```text
same wire name + same binding reuse
same wire name + different binding -> TypeMismatch
Topic same name different type -> TypeMismatch
unregister after last dependency only
SystemDefault baseline capture
history/depth
reliability
durability
deadline/lifespan
liveliness/lease
ROS2 memory policy
ROS2 synchronous publish
ROS2 data_sharing off
all common profiles
actual_qos reverse mapping
compatibility helper
wait_for_all_acked
assert_liveliness
```

### 10.5 Topic tests

```text
write/read
invalid sample filtering
finite read candidate budget
TemporarySample transactional commit
MessageInfo
matched count
QoS incompatible endpoint
EventSource multi-cursor
listener teardown
```

### 10.6 Service tests

```text
request identity
response identity
multi-client routing
pending capacity reservation before take
duplicate suppression
concurrent same RequestId -> Busy
write failure returns request to Pending
response writer target already matched
match before effective QoS deadline
target confirmed gone
max_blocking deadline timeout
Context shutdown during target wait
participant-consistent availability
metadata-consistent availability whenNode metadata exists
```

不得继续以固定`100 ms`作为test oracle；expected response-target wait来自writer effective Qos。

### 10.7 Arguments / Clock / Timer tests

Arguments：

```text
global/local rule merge
node name/namespace remap
topic/service/action remap
invalid name/rule
unparsed preservation
parameter override canonicalization
```

Clock：

```text
System/Steady/Ros now
Ros override enable/disable
set Ros time
invalid cross-clock TimePoint
Clock facade destroyed before Timer
Clock update generation
```

Timer：

```text
negative/zero period
autostart
cancel/reset/exchange
missed-cycle grid realign
concurrent consume
System/Steady deadlines
Ros pause/backward/forward update
Clock update wakes WaitSet
no per-timer thread
shutdown
```

### 10.8 WaitSet race tests

```text
poll/finite/infinite
pre-existing data before attach
data arrives around attach boundary
control GuardCondition wake
add/remove while waiting
topology generation rebuild
original user deadline preserved
Timer deadline wakes infinite wait
Ros Clock update wakes wait
Action expiry wakes wait
GraphEvent wakes wait
GuardCondition trigger/reset race
waitable destroy while waiting
second concurrent wait -> Busy
Action detail mask snapshot
```

重点复现/覆盖Jazzy `rmw_wait`历史暴露过的race类型，而不是假设native WaitSet自动解决所有lost-wakeup问题。

### 10.9 Graph tests

synthetic + real Fast DDS两层：

```text
participant add/remove
reader/writer add/change/remove
no-op callback no revision bump
local Node metadata
local endpoint association
ros_discovery_info publish/receive
ignore local metadata publication
remote ParticipantEntitiesInfo equivalent update
same snapshot no revision bump
Node names/namespaces
topic names/types
TopicEndpointInfo + QoS
service candidate same-participant
service candidate same-node when metadata available
action five-endpoint composition
GraphSnapshot one-revision consistency
GraphEvent cursor
metadata publisher failure degraded/retry behavior
```

### 10.10 Action tests

**pure common-state tests**无需启动DDS：

```text
Goal FSM all legal/illegal transitions
cancel selection
pending result requests
terminal expiry
status snapshot
concurrent same-goal transition
```

**accept transaction tests**：

```text
reject response no GoalRecord
duplicate GoalId
reservation rollback on response write failure
no public half-accepted Goal
response write success -> no-fail local commit
Defer/Execute
concurrent same GoalId exactly one success
```

**Fast DDS integration**：

```text
five-endpoint create rollback
aggregate readiness/detail mask
service correlation
feedback/status
availability
result expiry WaitSet integration
teardown
ROS2 Action interop
```

### 10.11 Parameter tests

```text
ParameterValue all types
declare / duplicate
undeclare
name validation
fixed/dynamic type
read-only
integer range/step
floating range/step
override apply/ignore
override invalid type/range
atomic set all-or-nothing
concurrent store revision change
ParameterChangeSet
deleted/new/changed classification
list prefix/depth
allow_undeclared
```

### 10.12 Current implementation migration priorities

当前`main`已经具有较完整的Foundation/Topic/Service/Event/basic WaitSet，但与本规格相比重点工作为：

1. **Foundation contract fixes**
   - `Server` response-reader wait仍存在hard-coded `100 ms`，改为effective response-writer `max_blocking_time`；
   - GuardCondition native wake failure必须由`trigger()`返回；
   - `std::bad_alloc` mapping统一为propagate；
   - DiscoveryGraph duplicate/no-op callback不得推进revision；
   - RuntimeMode注释去掉“只验证Humble”的旧描述。

2. **QoS completion**
   - 补`ros2_sensor_data`、`ros2_parameters`、`ros2_parameter_events`、`ros2_action_status_default`；
   - actual_qos reverse mapping；
   - compatibility helper；
   - writer ACK/liveliness operation。

3. **Foundation expansion**
   - Arguments/Remapping；
   - Clock/Time；
   - ParameterStore；
   - Node FQN；
   - Clock-aware Timer。

4. **Graph / Wait completion**
   - local/remote Node metadata；
   - ROS2 graph metadata transport；
   - GraphSnapshot/GraphEvent；
   - WaitResult detail snapshot；
   - Timer/Clock/Graph-aware WaitSet。

5. **Action runtime**
   - five-endpoint aggregate；
   - GoalRegistry/FSM；
   - accept transaction；
   - cancel/result/status/expiry；
   - Action interop。

这些迁移项按`dmw.md`五阶段执行，不新增独立framework package。

### 10.13 Frozen Implementation Invariants

1. Fast DDS 2.14.x 与`rmw_fastrtps` Jazzy是平等主要参考。
2. DMW不依赖ROS 2 runtime package。
3. 普通public API不暴露Fast DDS/Fast CDR类型。
4. Participant listener优先在Participant创建时安装。
5. 一个Context使用一个Participant、一个DDS Publisher container、一个DDS Subscriber container。
6. TypeRegistry/TopicRegistry是Context mandatory authority。
7. RuntimeMode/name resolver集中实现，不在Action/Service重复`rt/rq/rr`逻辑。
8. Arguments/remapping parser属于DMW common runtime。
9. ClockState属于DMW；Timer绑定ClockState。
10. Ros Clock update通过control wake使Timer/WaitSet重新判断，不用steady elapsed擅自推进ROS time。
11. ROS2 endpoint baseline使用PREALLOCATED_WITH_REALLOC memory policy。
12. ROS2 endpoint baseline关闭data sharing。
13. ROS2 writer baseline默认synchronous publication。
14. SystemDefault在Context生命周期内基于captured effective baseline，不随global mutable default漂移。
15. actual_qos从真实Fast DDS entity读取并reverse-map。
16. Service response-reader wait从effective response-writer QoS `max_blocking_time`派生。
17. Server capacity在destructive take前reservation。
18. response write failure恢复Pending request state。
19. DiscoveryGraph revision仅在public-observable state真实改变时增长。
20. ROS2 Node identity通过显式graph metadata传播，不从Participant name猜测。
21. `ros_discovery_info` internal transport不暴露成用户Topic。
22. Service/Action availability使用participant-consistent candidate semantics；metadata可用时进一步约束Node association。
23. Timer没有DDS entity、没有per-timer worker thread。
24. WaitSet正常路径没有fixed periodic polling slice。
25. WaitSet使用native WaitSet/Condition + private control GuardCondition + logical pre/re-check。
26. Timer/Action steady deadline参与native wait remaining；Ros override Timer依赖Clock update wake。
27. WaitResult形成Action aggregate detail snapshot。
28. Public GuardCondition native trigger failure不能伪装成success。
29. Event使用cumulative source state + per-Event cursor，多个Event不得互相吞状态。
30. ActionClient/ActionServer各只表现为一个public aggregate waitable。
31. Goal FSM/cancel selection/result RequestId/expiry state只有DMW一个authority。
32. accepted response write成功后的Goal local commit不得再发生recoverable allocation/registry failure。
33. typed Action result payload cache不强行下沉。
34. Parameter value/descriptor/store/validation/override只有DMW一个common authority。
35. DMW不执行parameter user callback。
36. listener不执行任何user callback。
37. delete failure时memory safety优先于强制free。
38. conservative retention只用于无法证明middleware释放引用的failure path，并必须可diagnose。
39. `std::bad_alloc`允许传播，不映射为ordinary DMW Error。
40. Fast DDS minor compatibility差异只存在private helper。
41. 不追求单一binary跨Fast DDS minor ABI。
42. Component/Composition不进入DMW V1，也不影响Foundation/Graph/Action完整性。

### 10.14 结论

DMW Fast DDS backend最终应实现：

```text
Fast DDS 2.14.x public semantics
        +
rmw_fastrtps Jazzy production patterns
        +
rmw_dds_common graph metadata behavior
        +
rcl / rcl_action common-runtime semantics
        +
DMW existing lifecycle / Result / ownership contract
        ↓
Context / Clock / Naming / QoS / Parameter
Topic / Service / Timer / Graph / Action / Wait
```

实现重点不是复制`rmw_fastrtps`文件结构，而是保留已经验证有价值的Type/Topic registry、service identity、transactional receive、participant-consistent availability、RAII与teardown safety，同时去掉：

```text
hard-coded timeout
fixed periodic wait slice
duplicate graph revision
hidden GuardCondition wake failure
language-layer重复的clock/remap/parameter state machine
```

完成本规范列出的Foundation convergence、Graph/Clock/Parameter expansion和Action implementation后，`dmw_fastdds.md`才能从`V1 Implementation Convergence`恢复为`V1 Implementation Revision Frozen Candidate`。
>>>>>>> Stashed changes
