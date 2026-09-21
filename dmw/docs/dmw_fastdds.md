# DMW Fast DDS 实现规格

| 属性 | 值 |
| --- | --- |
| 文档文件 | `dmw_fastdds.md` |
| 规范状态 | V1 Implementation Frozen |
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

```text
Context
    -> DomainParticipant
    -> one DDS Publisher container
    -> one DDS Subscriber container
    -> internal graph metadata writer/reader in ROS2 mode

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

History：

```text
KeepLast -> KEEP_LAST_HISTORY_QOS + checked depth
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

实现约束（实测得出，必须有测试覆盖）：

```text
1. Gid 线缆布局随 ROS 2 世代变化：
       Humble          : char[24]
       Rolling / Jazzy : char[16]
   DMW 不链接 ROS 2 runtime，无法在运行期识别对端，因此在构建期按 Fast DDS 世代选择：
       CMake 选项 DMW_RMW_GID_SIZE（空 = 自动：fastrtps < 2.13 取 24，否则取 16）
   同一次构建只讲一种布局；跨世代的 ROS 2 node graph metadata 本身即不互通，
   DMW 与 ROS 2 行为保持一致，不试图同时支持两种。
   前 16 octet 始终是 DDS GUID，因此 GUID 提取与布局无关。

2. listener 线程只允许“置位待处理标记”，不得在 Fast DDS 回调线程里 take sample：
   2.13 上这样做会在 reader 上阻塞。取样本、解析、更新图统一下沉到应用线程
   （graph_snapshot / graph_revision / availability 查询前无条件 drain 一次）。

3. drain 必须无条件执行且按“进入时的 unread 快照”限定上界：
   边沿触发的 DATA_AVAILABLE 可能早于样本入队被消费，门控式 drain 会永久漏读；
   无上界则可能被无法消费的样本卡死。

4. 远端 metadata 不触发本端快照 republish：远端快照不改变本端 Node/endpoint 集合，
   重发既无意义，也会把 metadata writer 带进 Fast DDS 回调线程。
```

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

Public GuardCondition 不直接暴露一个 Fast DDS `GuardCondition` 对象，而是保存：

```text
trigger_generation
consumed_generation
pending
```

多 trigger 可 coalesce，但 new trigger 不得被 concurrent reset 吞掉。

trigger transaction：

1. validate Context Active；
2. stage next generation（CAS 循环，generation 耗尽返回 `ResourceExhausted`）；
3. 发布 `pending`；
4. 通过注册的 wake callback 通知 WaitSet（`WaitSetWake::notify()` 内部才使用 Fast DDS
   `GuardCondition::set_trigger_value(true)`）；
5. wake 失败则本次 public `trigger()` 返回 `DDSError`，逻辑 trigger 已经 staged 但不会被消费成
   success。

这样做是因为 public GuardCondition 可能尚未注册到任何 WaitSet：native condition 是
notification mechanism，而不是 logical state 的 authority；WaitSet 报告 ready 时通过
`consume_trigger()` 推进 `consumed_generation`。

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

实现注记：`ResponseState` 的等待使用 `std::condition_variable` + 普通 `std::mutex`，而不是
`condition_variable_any`。后者内部自带一把隐藏 mutex，会在 ThreadSanitizer 下产生一个与
`TargetReader` 顺序无关的 lock-order 边；改用普通 mutex 后该路径不再持有任何隐藏锁，
TargetReader 顺序仍由调用顺序保证（见 `dmw.md` 阶段 6 的 TSan 收口）。

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

已在本环境执行完毕（全部通过）。环境为仓库内 Jazzy 容器 `osrf/ros:jazzy-desktop-full`
（Ubuntu 24.04 / GCC 13.3 / Fast DDS 2.14.6 / `rmw_fastrtps_cpp`），仓库 bind-mount 到
`/workspace/dcl`，`FASTDDS_BUILTIN_TRANSPORTS=UDPv4`：

```text
build                              ✓（含 32 个 header check TU；-Werror）
unit tests                         ✓
DMW integration tests              ✓（31 项 ctest 全过）
Topic bidirectional ROS interoperability      ✓
Service bidirectional ROS interoperability    ✓（AddTwoInts + std_srvs/SetBool）
Graph metadata/node discovery interoperability ✓
Action bidirectional ROS interoperability     ✓（example_interfaces/action/Fibonacci）
QoS golden/actual/compatibility tests         ✓
WaitSet race tests                 ✓
Clock/Timer tests                  ✓
Parameter common-state tests       ✓
shutdown/teardown tests            ✓
ASan/UBSan                         ✓（DMW 26/26；interop 5/5）
selected TSan                      ✓（DMW 26/26）
```

该行首次执行即暴露两个真实问题，均已修复并回灌到 Humble/Rolling：

```text
1. GraphMetadataTransport::create 中从 const Result 取 .error() 后 std::move，
   GCC 13 -Wextra 报 -Werror=redundant-move（GCC 11 不报）；改为非 const 临时值。
2. TimerState::exchange_period 中 notify_wait_set() 结果同类问题，同样修复。
```

TSan 在容器内需要 ASLR 关闭（`setarch <arch> -R`），这要求放宽容器 seccomp
（`--security-opt seccomp=unconfined`）；ASan 下 ROS 2 interop 用例需
`ASAN_OPTIONS=new_delete_type_mismatch=0`（报告来自 Jazzy `librcutils`/`librclcpp`）。

### 10.2 Compatibility CI — Humble / Fast DDS 2.6.x

已在本环境执行完毕（全部通过）：

```text
source build
unit + integration tests（26 项，含 v1_stress / client_library_prototype）
Topic interoperability
Service interoperability
Graph metadata compatibility
Action interoperability（命名/类型/availability + 数据面：accept/execute/succeed、reject、abort、
cancel、feedback/status、GetResult 挂起与移交、双 client 并发 goal）
Clock/Timer common-runtime tests
ASan + UBSan（DMW 测试集；范围见下方 Sanitizer 说明）
targeted TSan（suppressions 见 cmake/tsan_suppressions.txt）
```

遇到 2.6-only limitation 先通过 private shim 评估，不修改 DMW public API 迎合旧 minor。

附加已验证栈（本仓库开发机同时装有 ROS 2 Rolling / Fast DDS 2.13.2，用于逼近 Jazzy 线）：

```text
source build（无需修改，仅补了测试文件缺失的 <thread> include）
unit + integration tests（26 项全部通过）
Topic interoperability（rolling rclcpp/rmw_fastrtps + Fast DDS 2.13）
Service interoperability（std_srvs/srv/SetBool 双向；该接口在每个 ROS 2 发行版都存在，
  因此同一用例在 Humble 与 Rolling 上都会运行）
Graph metadata interoperability（rolling 节点可被 DMW 看到，DMW 节点可被 rolling 看到）
Action interoperability（该发行版没有 action 接口包，因此用仓库内
  test/ros2_action_test_interfaces 生成 Fibonacci action，经
  test/build_action_test_interfaces.sh 安装到本地前缀后运行同一套 Action 用例）
```

该 Action 用例在 Rolling 上曾出现约 1/12 的 terminate，已定位并修复：

```text
现象：cancel/result 阶段偶发 terminate，异常来自 *rclcpp_action 客户端*
      （"Taking data from action client but nothing is ready"），
      抛出点经 backtrace 定位到 librclcpp_action.so 的 cancel-response take 路径，
      由 rclcpp::Executor::get_next_ready_executable_from_map 调用。
根因：测试用 MultiThreadedExecutor 旋转 ROS 节点，同时应用线程并发调用 action client；
      rclcpp_action 的 Client 不是线程安全的，新版本会因 is_ready 与 take 的竞态而 terminate。
修复：
      - 测试改为单线程 canonical 模式：用 rclcpp::spin_some 在发起调用的同一线程驱动 ROS 侧，
        两侧都在该线程推进（DMW 侧仍用 WaitSet + 小 timeout 交替）；
      - 同时按真实 ROS 2 server 语义重排 cancel 流程（CancelGoal 响应先于终态迁移，
        终态 status 先于 GetResult 回包）。
验证：Rolling 连续 15 次运行 0 失败；Humble 连续 8 次运行 0 失败。
```

该栈发现并已修复的真实缺陷（都属 2.13/新版本行为差异）：

```text
1. metadata listener 在 Fast DDS 回调线程里 take sample -> 在新版本上阻塞；
   现在 listener 只置位 data-pending，取样本统一由应用线程完成。
2. 应用线程 drain 若以“pending 标志”为门控，会漏掉边沿触发的通知；
   现在 drain 无条件执行（标志只作为唤醒提示）。
3. ingest 循环改为按“进入时的 unread 快照”限定上界，杜绝无法消费的样本造成死循环。
4. 远端 metadata 不再触发本端快照 republish（远端快照不改变本端 Node/endpoint 集合）。
5. rmw_dds_common 的 Gid 线缆布局在不同 ROS 2 世代不同：
   Humble char[24]，Rolling/Jazzy char[16]（见 §5.5 与 CMake 选项 DMW_RMW_GID_SIZE）。
```

另有一起跨越两条栈的**测试隔离**缺陷（不是 DMW 运行时缺陷）：`dmw.lifecycle_stress` 固定占用
domain 100–119 且 topic/service 名固定，当 Humble 与 Rolling 两套测试栈同时运行时两个进程
互为对端，对端 sample 会被本进程 reader 正常接收，使 shutdown 期间的 infinite
`WaitSet::wait()` 合法地返回 `Ready`，表现为偶发断言失败。修复为按 pid 分配互不重叠的
21-wide domain slot（`domain = 1 + (pid % 11) * 21 + iteration`，见
`test/lifecycle_stress_test.cpp`、`test/v1_stress_test.cpp`）。修复后 4 实例并发 ×12 轮、
Humble+Rolling 跨栈并发 ×6 轮、`ctest --repeat until-fail:60` 均 0 失败。

Sanitizer 范围：DMW 测试集（26 项）在 ASan+UBSan 与 targeted TSan 下全绿；5 项 ROS 2 interop
用例在 ASan 下会 abort，报告位于 ROS 2 Humble 自带 `librcutils` / `librclcpp` 的
`new-delete-type-mismatch`（非 DMW 代码），因此 interop 在非 sanitizer 构建下验证，
或在 ASan 下显式关闭 `new_delete_type_mismatch`。

### 10.2.1 Jazzy / Fast DDS 2.14.x — 已在本环境执行

本节内容已被 §10.1 取代：primary 行已在本机 Jazzy 容器
（`osrf/ros:jazzy-desktop-full`，Fast DDS 2.14.6）执行通过。仍保留 2.13.2 / 2.14.6 的
头文件实测结论，因为它决定 DMW 是否需要 compatibility shim。

Fast DDS 2.13.2 与 2.14.6 头文件实测结果（取代先前仅凭记忆的猜测）：

```text
DomainParticipantListener discovery callbacks
    2.6  : on_participant_discovery(DomainParticipant*, ParticipantDiscoveryInfo&&)
    2.13 : 同时保留上述 2 参形式，并新增
           on_participant_discovery(DomainParticipant*, ParticipantDiscoveryInfo&&,
                                    bool& should_be_ignored)
    2.14 : 2 参形式仍然存在，但已标注 FASTDDS_TODO_BEFORE(3, 0, "Remove this overload")；
           3 参形式同样存在。
    结论 : DMW 现有 override 在 2.13 与 2.14 上均生效，无需 shim（已实测编译并通过互操作）。
    后续 : 2 参形式计划在 Fast DDS 3.0 移除；届时按本规范把差异收敛到
           src/impl/fastdds/compat/* 或极小 private header。
```

`dmw.md` 阶段 6 全部条目（含 ASan/UBSan 与 targeted TSan）与四类 ROS 2 互操作均已在
2.14.6 上执行通过；结果见 §10.1。

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
write/read                        ✓ test/message_type_test.cpp
invalid sample filtering          ✓ 由 `sample_info.valid_data` 跳过实现；`temporary_sample_test`
                                   覆盖 commit 语义（invalid 样本不触碰 caller output）
finite read candidate budget      ✓ 实现事实：`Subscriber::Impl::read` 在调用开始处快照一次
                                   `get_unread_count()`，循环上限即该快照，因此并发 arrival 无法
                                   延长单次 read()；黑盒无法构造「全部 invalid 样本」场景，故不作断言
TemporarySample transactional commit ✓ test/temporary_sample_test.cpp
MessageInfo                       ✓ test/message_type_test.cpp（writer/reader timestamp 与 writer gid）
matched count                     ✓ test/message_type_test.cpp / test/qos_operation_test.cpp
QoS incompatible endpoint         ✓ test/qos_operation_test.cpp（BestEffort writer + Reliable reader
                                   在兼容 reader 已匹配的前提下仍保持 0 匹配）
EventSource multi-cursor          ✓ test/event_parent_state_test.cpp
listener teardown                 ✓ test/message_type_test.cpp / test/lifecycle_stress_test.cpp
```

### 10.6 Service tests

```text
request identity                          ✓ test/message_type_test.cpp
response identity                         ✓ test/message_type_test.cpp
multi-client routing                      ✓ test/message_type_test.cpp
pending capacity reservation before take  ✓ test/message_type_test.cpp（max_pending_requests=1 +
                                            ResourceExhausted 且未消费 DDS sample）
unknown / already-responded RequestId -> NotFound ✓ test/server_fsm_test.cpp
duplicate suppression                     ✓ 由同一 pending entry 的 phase FSM 保证；
                                            test/server_fsm_test.cpp 覆盖「已回应 RequestId 再回应 -> NotFound」
concurrent same RequestId -> Busy         ✓ 实现于 Server::Impl::write_response 的
                                            Pending->Responding 迁移；该分支只在 response-target wait
                                            处于 in-flight（discovery 未收敛）时可观察，进程内黑盒测试
                                            无法稳定复现，因此不作为断言，见下方说明
write failure returns request to Pending  ✓ 实现路径 + test/server_fsm_test.cpp 的
                                            null-response 不改状态断言（InvalidArgument 不迁移 phase）
response writer target already matched    ✓ test/message_type_test.cpp（含 capacity detach/reattach 交接）
match before effective QoS deadline       ✓ test/request_response_test.cpp（ResponseState 等待语义）
target confirmed gone                     ✓ test/request_response_test.cpp（Removed -> success 不写）
max_blocking deadline timeout             ✓ test/request_response_test.cpp（TimedOut via absolute deadline）
Context shutdown during target wait       ✓ test/server_fsm_test.cpp / test/lifecycle_stress_test.cpp
participant-consistent availability       ✓ test/message_type_test.cpp
metadata-consistent availability          ✓ test/ros2_graph_interop_test.cpp（Node 关联）
```

不得继续以固定`100 ms`作为test oracle；expected response-target wait来自writer effective Qos。

关于 `concurrent same RequestId -> Busy`：该分支要求第一次 `write_response` 仍停留在
response-target wait 中（即目标 reader 既未 matched 也未确认消失）。在一进程内，请求到达时目标
reader 必然已 matched，因此该窗口不可稳定构造；回归改为覆盖它依赖的等待语义
（`test/request_response_test.cpp` 的 Ready/Removed/TimedOut 三态）与相邻的
`NotFound` 分支（`test/server_fsm_test.cpp`）。

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

### 10.12 实现进度与收口项

`dmw.md` §11.6 的六个阶段在 Fast DDS backend 上均已完成，对应实现要点：

1. **Foundation contract（已完成）**
   - `Server` response-reader wait 从 effective response-writer `max_blocking_time` 派生单一 absolute deadline；
   - GuardCondition trigger 走逻辑 generation + WaitSet 控制唤醒，native 失败不伪装成 success；
   - `std::bad_alloc` mapping 统一为 propagate；
   - DiscoveryGraph duplicate/no-op callback 不推进 revision；
   - RuntimeMode 注释不再绑定 Humble。

2. **QoS（已完成）**
   - 六个 common profile；actual_qos reverse mapping；compatibility helper；writer ACK/liveliness。

3. **Foundation expansion（已完成）**
   - Arguments/Remapping；Clock/Time；ParameterStore；Node FQN；Clock-aware Timer。

4. **Graph / Wait（已完成）**
   - local Node/endpoint metadata 与远端 Node 关联；
   - ROS2 graph metadata transport（`ros_discovery_info`，wire-compatible 内部类型）；
   - GraphSnapshot/GraphEvent；
   - WaitResult detail snapshot；
   - Timer/Clock/Graph/Action-aware WaitSet（runtime deadline 折入 native wait）。

5. **Action（已完成）**
   - five-endpoint aggregate + 事务式创建；
   - GoalRegistry/FSM；accept transaction；cancel/result/status/expiry；
   - ROS2 Action 双向互操作。

阶段 6 收口（对应`dmw.md`阶段 6）已完成：并发/生命周期压力测试、Humble/Jazzy matrix 执行与
记录、API 审计复核、文档冻结。其中 Jazzy primary 行执行时额外修复了两个 GCC 13
`-Werror=redundant-move`（`src/impl/graph_metadata.cpp`、`src/timer.cpp`）；另修复了
`test/lifecycle_stress_test.cpp` / `test/v1_stress_test.cpp` 的跨进程 domain 隔离缺陷。
这些都不新增独立 framework package。

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

本规范列出的 Foundation convergence、Graph/Clock/Parameter expansion 和 Action implementation
均已完成，ROS 2 兼容性矩阵两条线均已执行通过，因此 `dmw_fastdds.md` 已冻结为
`V1 Implementation Frozen`。

已在本环境验证：

```text
Humble / Fast DDS 2.6.x
    build、unit/integration tests、四类 ROS 2 互操作、
    ASan+UBSan、targeted TSan（见 cmake/tsan_suppressions.txt）

Jazzy / Fast DDS 2.14.6（容器 osrf/ros:jazzy-desktop-full，Ubuntu 24.04 / GCC 13.3）
    build（-Werror）、31 项 unit/integration（含 5 项 ROS 2 互操作）、
    ASan+UBSan、targeted TSan 全部通过

Rolling / Fast DDS 2.13.2（辅助逼近栈）
    30 项 unit/integration、四类 ROS 2 互操作全部通过
```

已知的非 DMW 限制：ASan 下 ROS 2 interop 用例需关闭 `new_delete_type_mismatch`（报告来自
`librcutils` / `librclcpp`）；容器内 TSan 需关闭 ASLR（`setarch -R`，配合
`--security-opt seccomp=unconfined`）。
