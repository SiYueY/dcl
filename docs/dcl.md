# DCL 系统架构设计

> 文档状态：V1 Architecture Frozen Candidate  
> 项目名称：DCL — DDS Client Libraries  
> 适用范围：`dmw`、`dclcpp`、`dclpy` 统一架构  
> 主要目标平台：Linux / Ubuntu 22.04、Ubuntu 24.04  
> C++ 标准：C++17  
> DDS 实现：Fast DDS

## 1. 文档目的

本文档定义 DCL 的系统分层、模块职责、运行时模型、类型边界、通信原语、Timer、Action、Graph、WaitSet、ROS 2 互操作、构建验证和演进原则。

DCL 的目标不是重新实现 ROS 2，也不是重新实现 DDS，而是在 Fast DDS 之上提供一组更轻量的 Client Library：

```text
DCL
├── dmw       DDS Middleware / Common Runtime Layer
├── dclcpp    DDS Client Library for C++
└── dclpy     DDS Client Library for Python
```

总体关系：

```text
                 Applications
              /               \
             ▼                 ▼
         C++ Application   Python Application
             │                 │
             ▼                 ▼
          dclcpp              dclpy
             │                 │
             │              _dclpy
             │                 │
             └────────┬────────┘
                      ▼
                     dmw
                      │
                      ▼
                  Fast DDS
                      │
                      ▼
                  DDSI-RTPS
```

## 2. 上游参考基线

### 2.1 DMW 主要参考

后续 DMW 设计与实现使用两个**平等**主要参考基线：

```text
Fast DDS 2.14.x ─────────────┐
                             ├── cross-audit ──> DMW
rmw_fastrtps Jazzy ──────────┘
```

二者不存在优先级。

Fast DDS 2.14.x 重点用于理解和验证：

- DDS public API；
- Entity lifecycle；
- QoS；
- Listener / StatusCondition / GuardCondition / WaitSet；
- discovery；
- resource/error/teardown semantics。

`rmw_fastrtps` Jazzy 重点用于理解和验证：

- Fast DDS 的生产级 ROS 2 用法；
- ROS Topic/Service naming；
- QoS translation；
- GID / MessageInfo；
- request/response correlation；
- service availability；
- discovery/wait race；
- teardown pattern。

当二者实现策略不同，DMW 同时评估 Fast DDS public guarantees、`rmw_fastrtps` interoperability 行为和自身 public contract，不机械复制任一实现。

### 2.2 Common Runtime 与 Client Library 参考

```text
rcl / rcl_action Jazzy
    -> language-neutral runtime / state machine / wait 参考

rclcpp / rclpy Jazzy
    -> C++ / Python typed API、Future、callback、Executor 边界参考
```

DCL 不依赖这些 ROS 2 runtime package，也不复制 ROS 2 package tree。

### 2.3 Humble 定位

ROS 2 Humble / Fast DDS 2.6.x 保留为重要兼容性验证目标，但不决定新的 DMW public design。

目标是：

```text
same DCL source
    ├── build/test with Jazzy / Fast DDS 2.14.x
    └── compatibility build/test with Humble / Fast DDS 2.6.x
```

不要求同一预编译 DCL binary 跨 Fast DDS minor ABI 使用。

## 3. 核心架构原则

### 3.1 只保留两层 Client/Middleware 结构

DCL 不新增独立 `rcl` 等价 package：

```text
Application
    │
    ▼
dclcpp / dclpy
    │
    ▼
dmw
    │
    ▼
Fast DDS
```

`dmw` 同时承担 Fast DDS binding 与 language-neutral common runtime。

### 3.2 dclcpp 与 dclpy 平级

禁止：

```text
dclpy -> dclcpp -> dmw
```

采用：

```text
             dmw
           ▲     ▲
          /       \
     dclcpp       _dclpy
                    ▲
                    │
                  dclpy
```

两种 Client Library 共享 DMW runtime semantics，但拥有各自语言友好的 API、Future、callback 和 Executor。

### 3.3 DMW non-template/type-erased

DMW：

- C++17；
- runtime public API non-template；
- message path type-erased；
- RAII；
- 普通 public API 不暴露 Fast DDS type；
- Fast DDS-only；
- 不提供 middleware plugin abstraction。

### 3.4 用户 callback 不进入 Fast DDS/DMW internal thread

标准调度链：

```text
Fast DDS / DMW state
        │
        ▼
DMW WaitSet readiness
        │
        ▼
dclcpp / dclpy Executor
        │
        ▼
User callback / Future completion
```

Listener 只允许更新内部 state、discovery、event、readiness 和 wake notification。

### 3.5 下沉判定

以下能力应只有一个 DMW authority：

```text
protocol mapping
identity / correlation
runtime state machine
readiness / wait semantics
Timer deadline state
Action Goal FSM / cancel/result lifecycle
Graph snapshot/change authority
common QoS profile values
ROS 2 wire mapping
```

以下必须留在 Client Library：

```text
C++ templates / Python runtime types
std::function / Python callable
std::promise / std::future
Python Future / asyncio Future
Pending Future registry
Executor callback dispatch
CallbackGroup / ThreadPool policy
C++ exception presentation
Python exception / GIL / asyncio
```

## 4. ROS 2 分层到 DCL 的映射

| 能力 | ROS 2 主要位置 | DCL authority |
| --- | --- | --- |
| Context / Node | `rcl` | `dmw` |
| Pub/Sub / Service primitive | `rcl + rmw` | `dmw` |
| QoS mapping / common preset | RMW / Client Library | `dmw` |
| WaitSet | `rcl` | `dmw` |
| GuardCondition / Event | `rcl` | `dmw` |
| Service availability / wait | `rcl` + Client Library wrapper | `dmw` |
| Timer state/readiness | `rcl` | `dmw` |
| Action endpoint composition | `rcl_action` | `dmw` |
| Goal FSM | `rcl_action` | `dmw` |
| Action cancel/result/status common state | `rcl_action` | `dmw` |
| Graph snapshot/change | `rcl` | `dmw` |
| Future / Promise | `rclcpp/rclpy` | `dclcpp/dclpy` |
| Pending Future registry | `rclcpp/rclpy` | `dclcpp/dclpy` |
| Callback | `rclcpp/rclpy` | `dclcpp/dclpy` |
| Executor dispatch | `rclcpp/rclpy` | `dclcpp/dclpy` |
| CallbackGroup / ThreadPool | Client Library | Client Library；V1 不做复杂模型 |
| C++ typed API | `rclcpp` | `dclcpp` |
| Python GIL / asyncio | `rclpy` | `dclpy` |
| Exception mapping | Client Library | `dclcpp/dclpy` |

## 5. 模块职责

### 5.1 dmw

负责：

- Context / Node runtime；
- Fast DDS Participant 与 endpoint 生命周期；
- Publisher / Subscriber；
- Client / Server；
- Service identity/correlation/availability/wait；
- Timer；
- ActionType / ActionClient / ActionServer；
- GoalId / GoalInfo / Goal FSM；
- cancel candidate selection；
- pending result RequestId / terminal Goal / expiry common state；
- Action aggregate readiness；
- Qos 与 common profiles；
- WaitSet / GuardCondition / Event；
- GraphSnapshot / GraphEvent / graph revision；
- MessageType / ServiceType / ActionType；
- MessageInfo / RequestId；
- TypeRegistry / TopicRegistry；
- ROS 2 Fast DDS wire mapping。

不负责：

- typed Action result payload cache；
- Future/Promise；
- Pending Future registry；
- user callback；
- Executor；
- C++ template API；
- Python GIL/asyncio。

### 5.2 dclcpp

负责：

- C++17 typed API；
- Context / Node wrapper；
- `MsgType<MsgT>` / `ServiceType<S>` / `ActionType<A>`；
- Publisher/Subscription wrapper；
- Client/Service wrapper；
- Timer callback wrapper；
- typed ActionClient/ActionServer/GoalHandle；
- typed Action result payload cache；
- C++ Pending Future registry；
- QoS convenience wrapper；
- Graph value wrapper；
- WaitSet wrapper；
- SingleThreadedExecutor；
- callback/Future/exception mapping。

### 5.3 dclpy

负责：

- Pythonic Context/Node/Topic/Service/Timer/Action API；
- `_dclpy` native binding；
- Python message/type wrapper；
- Python typed Action payload cache；
- Python Future / Pending Future registry；
- Python callback；
- Python Executor / asyncio；
- Graph value conversion；
- GIL boundary；
- Python exception mapping。

## 6. 运行时对象模型

```text
Context
├── DomainParticipant
├── TypeRegistry
├── TopicRegistry
├── DiscoveryGraph
├── WaitSet / GuardCondition
├── Timer
├── GraphEvent
└── Node...
     ├── Publisher
     ├── Subscriber
     ├── Client
     ├── Server
     ├── ActionClient
     └── ActionServer
```

### 6.1 Context

一个 Context 固定：

```text
1 DDS Domain ID
1 DomainParticipant
1 immutable RuntimeMode
```

多个 Domain 使用多个 Context。

Context 是 shutdown、registry、discovery/graph 和 common wait coordination 的 runtime root。

### 6.2 Node

Node 是 logical communication identity，不等于 DomainParticipant。

多个 Node 可以共享一个 Context Participant。

Node facade 可以早于 endpoint facade 析构；DMW internal NodeState 保留 endpoint 所需 logical name/namespace backing。

### 6.3 Timer

Timer 是 Context-scoped runtime primitive，不是 DDS entity。

`dclcpp::Node::create_timer()` / `dclpy.Node.create_timer()` 是语言层 convenience：内部从该 Node 的 Context 创建 DMW Timer，并把 wrapper 加入该 Node 的 Executor registry。

### 6.4 Action

ActionClient/ActionServer 是 Node-scoped logical endpoint，因为 Action name 需要 Node namespace resolution。

DMW 内部实现仍由 3 Service + 2 Topic 组成，但这五个 endpoint 不暴露给 Client Library。

## 7. 类型系统

### 7.1 Message

DCL V1 不重新实现完整 IDL/compiler/codegen。

推荐：

```text
IDL
 ↓
Fast DDS-Gen
 ↓
Msg + MsgPubSubType
 ↓
dmw::MessageType
```

C++：

```cpp
auto type = dclcpp::create_msg_type<Msg, MsgPubSubType>();
```

后续业务 API 只使用 `MsgType<Msg>`。

### 7.2 ServiceType

```text
ServiceType
├── Request MessageType
└── Response MessageType
```

### 7.3 ActionType

```text
dmw::ActionType
├── SendGoal ServiceType
├── CancelGoal ServiceType
├── GetResult ServiceType
├── Feedback MessageType
└── Status MessageType
```

DMW 不解析任意 Action message 字段。Client Library/type adapter 负责把 typed message 的 UUID、timestamp 等公共字段转换为：

```text
GoalId
GoalInfo
CancelGoalCriteria
GoalStatusInfo
```

之后 FSM、匹配和 lifecycle 由 DMW 处理。

## 8. Topic 架构

发送：

```text
dclcpp::Publisher<Msg> / dclpy.Publisher
        ↓
dmw::Publisher::write()
        ↓
Fast DDS DataWriter
```

接收：

```text
Fast DDS DataReader
        ↓
dmw::Subscriber::read()
        ↓
Client Library Executor
        ↓
user callback
```

DMW 负责 MessageInfo、matched count、Event 和 QoS mapping。

## 9. Service 架构

```text
Client
├── Request DataWriter
└── Response DataReader

Server
├── Request DataReader
└── Response DataWriter
```

DMW 负责：

- RequestId；
- SampleIdentity / related_sample_identity；
- response routing；
- pending request state；
- capacity/backpressure；
- service availability；
- blocking availability wait。

Client Library 负责：

```text
RequestId -> language Future/Promise
```

因此 Future registry 不下沉。

## 10. Timer 架构

DMW Timer 负责：

- period；
- next deadline；
- autostart/cancel/reset；
- readiness；
- `exchange_period()`；
- `consume(TimerInfo&)`；
- missed-cycle skip/re-align；
- WaitSet integration。

```text
dmw::Timer
    ├── dclcpp::Timer + std::function
    └── dclpy.Timer + Python callable
```

Timer 使用 monotonic clock，不创建 callback thread。

WaitSet 报告 Timer ready 不消费触发；Executor 调用 `consume()` 成功后才执行 callback。

严重迟到时 DMW 跳过已经错过的完整周期，并把下一 deadline 对齐到周期网格，而不是逐个补发历史触发。

## 11. Action 架构

### 11.1 endpoint composition

```text
Action
├── SendGoal Service
├── CancelGoal Service
├── GetResult Service
├── Feedback Topic
└── Status Topic
```

DMW 对 Client Library 暴露一个 ActionClient 或 ActionServer，不暴露内部五个 endpoint。

### 11.2 Goal FSM

DMW 状态：

```text
Accepted
  ├── Execute    -> Executing
  └── CancelGoal -> Canceling

Executing
  ├── CancelGoal -> Canceling
  ├── Succeed    -> Succeeded
  └── Abort      -> Aborted

Canceling
  ├── Succeed    -> Succeeded
  ├── Abort      -> Aborted
  └── Canceled   -> Canceled
```

`Succeeded / Canceled / Aborted` 是 terminal state。

### 11.3 Goal accept transaction

accepted response 与 DMW Goal registry 必须通过一个 DMW transaction path 协调：

```text
validate/reserve GoalId
        ↓
write accepted response
        ↓
commit Accepted / optional Executing state
```

write failure -> rollback reservation，不产生 half-accepted Goal。

Client Library 负责用户 goal callback 和 typed response 构造；DMW 负责 transport write 与 Goal-state commit 的一致性。

### 11.4 Cancel

DMW 根据 `CancelGoalCriteria` 选择候选：

```text
exact GoalId
all cancelable goals
goals at/before timestamp
exact GoalId OR goals at/before timestamp
```

用户 cancel callback仍在 Client Library。接受后调用 DMW Goal transition进入 Canceling。

### 11.5 Result lifecycle

DMW 保存：

```text
terminal Goal state
terminal timestamp
pending GetResult RequestIds
expiry deadline
```

Client Library 保存：

```text
GoalId -> typed ActionT::Result payload
```

这样 DMW 不需要 reflection、generic object clone 或 serialized-message subsystem。

### 11.6 Aggregate readiness

一个 ActionClient WaitSet token 聚合：

```text
goal response
cancel response
result response
feedback
status
```

一个 ActionServer token 聚合：

```text
goal request
cancel request
result request
goal expiry
```

Executor 不注册内部 transport endpoints。

## 12. Graph 架构

DMW DiscoveryGraph 是唯一 authority：

```text
Fast DDS discovery
        ↓
DiscoveryGraph
        ├── Service availability
        ├── Action availability
        ├── GraphSnapshot
        └── GraphEvent / revision
```

V1 public Graph 只暴露 DDS discovery 能可靠支撑的内容：

- Topic names/types/counts；
- Service names/types/client/server-candidate counts；
- Action names/client/server-candidate counts；
- monotonic graph revision；
- GraphEvent。

**V1 不暴露 ROS Node graph。**

原因是普通 DDS participant/endpoint discovery 无法可靠恢复 ROS logical node name/namespace；DCL 不用 Participant name 猜测 Node identity。

完整 ROS Graph metadata、`ros2 node list`、`ros2 node info`、rqt_graph compatibility 后续单独实现。

## 13. QoS 架构

```text
dclcpp::QoS / dclpy.QoS
          ↓
       dmw::Qos
          ↓
Fast DDS Writer/Reader QoS
```

Common preset 数值只有 DMW 一个 authority：

| DMW profile | History | Depth | Reliability | Durability |
| --- | --- | ---: | --- | --- |
| `ros2_default` | KeepLast | 10 | Reliable | Volatile |
| `ros2_services_default` | KeepLast | 10 | Reliable | Volatile |
| `ros2_sensor_data` | KeepLast | 5 | BestEffort | Volatile |
| `ros2_parameters` | KeepLast | 1000 | Reliable | Volatile |
| `ros2_parameter_events` | KeepLast | 1000 | Reliable | Volatile |
| `ros2_action_status_default` | KeepLast | 1 | Reliable | TransientLocal |

`dclcpp` / `dclpy` 只提供语言友好的 wrapper/alias。

## 14. WaitSet / Executor 架构

DMW WaitSet 支持：

```text
Subscriber
Client
Server
Timer
ActionClient
ActionServer
Event
GraphEvent
GuardCondition
```

DMW 不提供 Executor。

`dclcpp` V1 提供 SingleThreadedExecutor；`dclpy` 独立提供 Python Executor/asyncio integration。

### 14.1 deadline model

Finite wait 使用一次性 absolute steady deadline。

native wait deadline：

```text
min(
    caller finite deadline,
    earliest Timer deadline,
    earliest Action result-expiry deadline)
```

add/remove/control wake 不重置 caller timeout。

正常路径不使用固定 100 ms polling slice。

## 15. ROS 2 wire compatibility

### 15.1 Topic

logical `/foo`：

```text
rt/foo
```

### 15.2 Service

logical `/add_two_ints`：

```text
rq/add_two_intsRequest
rr/add_two_intsReply
```

correlation 使用 Fast DDS SampleIdentity / related_sample_identity。

Service response writer等待目标 response reader 时，timeout 由 effective writer QoS `reliability().max_blocking_time` 派生，不是 DMW public 100 ms 常量。

### 15.3 Action

logical `/move` 派生：

```text
/move/_action/send_goal
/move/_action/cancel_goal
/move/_action/get_result
/move/_action/feedback
/move/_action/status
```

随后复用 Service/Topic ROS2 mapping 产生 `rq/rr/rt` DDS names。

默认 QoS：

```text
Goal/Cancel/Result service -> ros2_services_default
Feedback -> ros2_default
Status -> ros2_action_status_default
```

ActionServer result retention default：10 s。

### 15.4 Wire != full Graph

DCL V1 的首要 interoperability guarantee：

```text
Topic data
Service request/reply
Action protocol/data
```

不因没有完整 ROS Node graph metadata而否定 wire interoperability。

## 16. 生命周期与线程安全

### 16.1 RAII

所有 DMW Resource 使用 RAII；Factory 创建成功即完整有效。

Context facade 可以先于 child facade析构；internal shared state 保证 child安全 teardown。

### 16.2 listener boundary

Fast DDS listener 不执行用户 callback。

### 16.3 shutdown

Context shutdown：

- irreversible；
- 唤醒 WaitSet；
- 中断 Service/Action wait；
- 阻止新 resource；
- 已有 child可安全析构。

### 16.4 Client Library state

Future/typed payload/callback registry 的线程安全由对应 Client Library 负责，不把语言 runtime 锁塞入 DMW。

## 17. 构建依赖

```text
Fast DDS / Fast CDR
        ▲
        │
       dmw
      ▲   ▲
     /     \
 dclcpp   _dclpy
            ▲
            │
          dclpy
```

约束：

```text
dmw     !-> dclcpp
dmw     !-> dclpy
dclcpp  -> dmw
_dclpy  -> dmw
dclpy   -> _dclpy
dclcpp  !<-> dclpy
```

## 18. 目录与 target

核心 target：

```text
dmw::dmw
dmw::fastdds_binding
dclcpp::_target_as_defined_by_dclcpp
_dclpy
```

Timer、Action、Graph 属于 `dmw::dmw`，不新增独立 `rcl`、`action_runtime` 或 `graph_runtime` package。

## 19. 测试架构

测试分四层：

```text
DMW pure/common-state unit tests
DMW Fast DDS integration tests
Client Library integration tests
ROS 2 interoperability tests
```

### 19.1 DMW pure tests

无需 DDS 即可覆盖：

- Result/Error；
- Timer scheduling；
- Goal FSM；
- cancel selection；
- result pending/expiry；
- Graph snapshot conversion；
- common QoS profile values。

### 19.2 Fast DDS integration

覆盖：

- Context/entity lifecycle；
- pub/sub；
- service correlation/availability；
- WaitSet race；
- Graph discovery；
- Action 5-endpoint composition；
- teardown/shutdown。

### 19.3 Client Library tests

重点覆盖语言层：

- typed/Python mapping；
- Future registry；
- callback/Executor；
- typed Action result cache；
- exception/GIL/asyncio。

不得在 dclcpp 和 dclpy 各重复证明完整 Goal FSM。

### 19.4 ROS 2 interoperability

Jazzy / Fast DDS 2.14.x 是主要新设计验证环境；Humble / 2.6.x 是兼容性环境。

至少双向验证：

```text
Topic
Service
Action
```

跨版本 wire probe 使用 UDPv4 以减少 SHM 路径干扰。

## 20. 开发阶段

将实现压缩为四个阶段：

### Phase 1 — Baseline 与 DMW Foundation 对齐

- primary baseline切到 Jazzy/Fast DDS 2.14.x；
- 保留 Humble compatibility build；
- Context/Participant/QoS/WaitSet/Service cross-audit；
- 移除固定 100 ms wait/polling 等旧假设；
- Foundation regression。

### Phase 2 — DMW Common Runtime 补齐

- Timer；
- GraphSnapshot/GraphEvent；
- ActionType/ActionClient/ActionServer；
- Goal FSM/cancel/result expiry；
- aggregate WaitSet readiness；
- Action interoperability。

### Phase 3 — dclcpp

- typed wrappers；
- Future/callback；
- typed result payload cache；
- SingleThreadedExecutor；
- C++ API tests。

### Phase 4 — dclpy

- `_dclpy`；
- Python API；
- Future/asyncio/GIL；
- Python typed result cache；
- cross-language tests。

## 21. 架构冻结项

1. `dclcpp` 与 `dclpy` 平级。
2. 两者都直接建立在 `dmw` 上。
3. DMW 是 non-template C++17 common runtime。
4. Fast DDS 是唯一 backend。
5. Fast DDS 2.14.x 与 `rmw_fastrtps` Jazzy 是平等主要参考基线。
6. Humble/2.6.x 是兼容性验证目标，不决定新 public design。
7. Topic/Service primitive在 DMW。
8. Service availability/wait在 DMW。
9. Timer在 DMW，callback在 Client Library。
10. Action endpoint composition/Goal FSM/cancel/result lifecycle common state在 DMW。
11. typed Action result payload cache在 Client Library。
12. Graph snapshot/change authority在 DMW，完整 ROS Node graph后续。
13. common QoS preset数值只有 DMW 一处。
14. WaitSet在 DMW，Executor在 Client Library。
15. Future/Pending Future registry不下沉。
16. callback不在 Fast DDS/DMW internal thread执行。
17. Python GIL/asyncio留在 Python 层。
18. C++ template typed API留在 C++ 层。
19. DMW 不新增 `rcl` 等价 package。
20. wire compatibility与完整 ROS Graph compatibility分离。

## 22. 总结

DCL 的核心结构最终为：

```text
C++ / Python typed Client Library
 Future / callback / Executor / language runtime
                    │
                    ▼
                  DMW
 Context / Topic / Service / Timer / Action / Graph / Wait
                    │
                    ▼
                Fast DDS
```

DMW 的价值不只是“薄 DDS wrapper”，而是成为 C++ 与 Python 共用的语言无关通信/runtime authority；同时明确拒绝把 Future、callback、Executor、GIL、typed payload 等语言职责下沉，从而在消除协议重复的同时保持架构轻量。
