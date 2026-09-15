# DMW Fast DDS 实现规格

| 属性 | 值 |
| --- | --- |
| 文档文件 | `dmw_fastdds.md` |
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

```text
Context
    -> DomainParticipant
    -> one DDS Publisher container
    -> one DDS Subscriber container

Node
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

WaitSet
    -> Fast DDS WaitSet
    -> private control GuardCondition

GuardCondition
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

History：

```text
KeepLast -> KEEP_LAST_HISTORY_QOS
depth    -> history.depth
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
