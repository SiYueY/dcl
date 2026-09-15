# DCLCPP 设计文档

> 文档状态：Draft V0.2  
> 模块名称：DCLCPP — DDS Client Library for C++  
> 下层依赖：`dmw`  
> C++ 标准：C++17

---

## 1. 设计目标

`dclcpp` 是 DCL 面向 C++ 应用的高层 Client Library。

目标：

1. 提供现代 C++17 typed API；
2. 隐藏 `dmw` 的 type-erased 细节；
3. 隐藏 Fast DDS；
4. 提供 Topic / Service / Timer / Action；
5. 提供 WaitSet / Executor；
6. 提供 RAII、callback、future；
7. 支持 Native DDS 和 ROS 2 Humble/Jazzy compatibility；
8. 使用 Fast DDS-Gen 产物，不重新实现完整 codegen；
9. 允许一次性绑定 `Msg + MsgPubSubType`，后续只使用 `MsgType<Msg>`；
10. 复用 `dmw` 提供的 language-neutral runtime、protocol state、Graph authority 和 readiness，不在 C++ Client Library 重复实现与 `dclpy` 平行的底层状态机。

---

## 2. 非目标

DCLCPP V1 不负责：

- DDS entity 直接管理；
- Fast DDS Listener 用户 callback；
- Service request identity 底层实现；
- ROS 2 request/reply DDS mapping；
- Timer deadline/readiness 底层状态机；
- Action endpoint composition、transport correlation、Goal registry、cancel matching 和 Goal FSM 底层实现；
- 独立 Graph/discovery cache；
- common QoS preset 数值的第二套定义；
- Python API；
- 多 middleware；
- 完整 ROS Graph tooling compatibility；
- 自定义 IDL compiler。

这些分别属于 `dmw`、`dclpy` 或后续阶段。

---

## 3. 总体架构

```text
                    C++ Application
                          │
                          ▼
┌─────────────────────────────────────────────────┐
│                    dclcpp                       │
│                                                 │
│ Context / Node                                  │
│ MsgType / ServiceType / ActionType              │
│ Publisher<T> / Subscription<T>                  │
│ Client<S> / Service<S>                          │
│ Timer + callback                                │
│ ActionClient<A> / ActionServer<A>               │
│ typed GoalHandle                                │
│ QoS / Graph value wrappers                      │
│ WaitSet / Executor                              │
│ Callback / Future                               │
└───────────────────────┬─────────────────────────┘
                        ▼
                       dmw
                        │
                        ▼
                    Fast DDS
```

`dmw` 不只是 DDS entity wrapper，也承担 `dclcpp` 与 `dclpy` 必须共享的 language-neutral runtime semantics。DCLCPP 不复制 DMW 已经提供的 Timer、Graph、Action protocol、Goal FSM、Service correlation、WaitSet readiness 或 ROS 2 wire mapping。

---

## 4. API 设计原则

### 4.1 强类型

用户 API 基于：

```cpp
Publisher<MsgT>
Subscription<MsgT>
Client<ServiceT>
Service<ServiceT>
ActionClient<ActionT>
ActionServer<ActionT>
```

### 4.2 RAII

用户无需手动 delete DDS entities。

### 4.3 Fast DDS 隐藏

除 `create_msg_type<Msg, MsgPubSubType>()` 的显式绑定点外，普通业务代码不接触 Fast DDS 类型。

后续可通过接口包预绑定进一步隐藏 `MsgPubSubType`，但不作为 V1 前置条件。

### 4.4 Callback 不在 DDS/DMW 内部线程执行

所有 subscription/service/timer/action callback 通过 Executor 调度。

DMW 可以维护 readiness、protocol state 和内部 listener，但不得执行用户 callback；DCLCPP 负责把 ready runtime entity 转换为 C++ callback、promise/future completion 或 typed GoalHandle 操作。

### 4.5 只包装 Language-specific 能力

DCLCPP 允许和 DCLPY 存在以下合理重复：

```text
C++ typed template wrapper
std::function callback storage
std::promise / std::future
Pending Future registry
Executor callback dispatch
C++ exception mapping
```

但不允许重新实现：

```text
Timer scheduling state
Service availability state
Graph cache/revision
Action endpoint topology
Goal FSM / Goal registry
Cancel matching
Action availability
```

---

## 5. Context

`dclcpp::Context` 是 `dmw::Context` 的 C++ 高层 wrapper。

概念：

```cpp
class Context {
public:
    explicit Context(const ContextOptions& options = {});
    ~Context();

    void shutdown();
    bool ok() const noexcept;

private:
    std::unique_ptr<dmw::Context> context_;
};
```

ContextOptions 可包含：

- domain id；
- participant name；
- compatibility/runtime mode；
- discovery options；
- transport config（后续）。

ROS 2 compatibility 由底层 DMW Context 统一决定，不在每个 endpoint 重复实现独立 middleware mode。

Context 也提供 Graph snapshot/change wrapper，数据 authority 仍然是 `dmw::Context`。

---

## 6. Node

推荐用户接口：

```cpp
auto node = std::make_shared<dclcpp::Node>(
    context,
    "controller");
```

Node 负责创建：

- Publisher；
- Subscription；
- Client；
- Service；
- Timer；
- ActionClient；
- ActionServer。

概念：

```cpp
class Node {
public:
    Node(
        std::shared_ptr<Context> context,
        std::string name,
        NodeOptions options = {});
};
```

Node 不拥有独立 DomainParticipant；实际 participant 管理由 `dmw::Context` 完成。

Timer 在 DMW 中是 Context-scoped runtime primitive；`Node::create_timer()` 只负责把创建出的 DMW Timer 纳入该 Node 的高层 callback/executor registry，不在 DMW 重新制造 Node→Timer 的 DDS ownership 关系。

---

## 7. MsgType

## 7.1 设计目标

解决 Fast DDS-Gen 产生：

```text
Msg
MsgPubSubType
```

而应用希望后续只使用一个类型描述的问题。

## 7.2 类型定义

```cpp
template<class MsgT>
class MsgType {
public:
    using message_type = MsgT;

    MsgType(const MsgType&) = default;
    MsgType(MsgType&&) noexcept = default;

    bool valid() const noexcept;
    std::string_view type_name() const noexcept;

private:
    std::shared_ptr<const dmw::MessageType> type_;
};
```

## 7.3 创建

```cpp
template<class MsgT, class PubSubTypeT>
MsgType<MsgT> create_msg_type();
```

用户：

```cpp
auto joint_state_type =
    dclcpp::create_msg_type<
        JointState,
        JointStatePubSubType>();
```

内部：

```text
Msg + PubSubType
      ↓
dmw::fastdds::create_message_type<PubSubType>()
      ↓
dmw::MessageType
      ↓
MsgType<Msg>
```

## 7.4 设计约束

- `MsgType<Msg>` 保留强类型 Msg；
- PubSubType 被擦除；
- MessageType descriptor 可共享；
- 创建 MessageType 不等价于立即向所有 Participant 注册；注册由 DMW Context 按需完成。

---

## 8. Publisher

## 8.1 API

推荐：

```cpp
auto pub = node->create_publisher(
    joint_state_type,
    "/joint_states",
    qos);
```

模板推导得到：

```text
Publisher<JointState>
```

概念接口：

```cpp
template<class MsgT>
class Publisher {
public:
    using message_type = MsgT;

    void publish(const MsgT& message);

private:
    std::unique_ptr<dmw::Publisher> publisher_;
};
```

## 8.2 调用链

```text
Publisher<Msg>::publish(const Msg&)
        │
        ▼
dmw::Publisher::publish(&msg)
        │
        ▼
Fast DDS DataWriter
```

## 8.3 不推荐 API

禁止要求用户：

```cpp
create_publisher<Msg, MsgPubSubType>()
```

每次重复绑定。

---

## 9. Subscription

推荐：

```cpp
auto sub = node->create_subscription(
    joint_state_type,
    "/joint_states",
    qos,
    [](const JointState& msg) {
        // ...
    });
```

内部对象：

```text
Subscription<Msg>
├── dmw::Subscriber
├── MsgType<Msg>
└── callback
```

Executor readiness 后：

1. 创建/复用 Msg 对象；
2. 调用 `dmw::Subscriber::read(&msg, info)`；
3. 若成功，将 typed msg 交给 callback。

---

## 10. QoS

`dclcpp::QoS` 是用户友好的 wrapper：

```cpp
dclcpp::QoS qos(10);
qos.reliable();
qos.volatile_durability();
```

内部转换：

```text
dclcpp::QoS
    ↓
dmw::Qos
```

### 10.1 Common profile authority

DCLCPP 不独立硬编码与 DCLPY 重复的 profile 数值。以下 convenience wrapper 直接基于 DMW profile：

```text
SystemDefaultQoS          -> dmw::Qos::system_default()
DefaultQoS                -> dmw::Qos::ros2_default()
SensorDataQoS             -> dmw::Qos::ros2_sensor_data()
ServiceQoS                -> dmw::Qos::ros2_services_default()
ParametersQoS             -> dmw::Qos::ros2_parameters()
ParameterEventsQoS        -> dmw::Qos::ros2_parameter_events()
ActionStatusQoS           -> dmw::Qos::ros2_action_status_default()
```

DCLCPP 可以为这些 profile 提供 C++ class/alias 和 fluent API，但 profile 的实际数值由 DMW 单独冻结。

ROS 2-specific profile 应明确标记 compatibility，不让 generic QoS API 隐含 ROS 2 runtime dependency。

---

## 11. ServiceType

### 11.1 目标

把 request/response 两种 MsgType 组合为强类型 Service descriptor。

概念：

```cpp
template<class ServiceT>
class ServiceType;
```

ServiceT 推荐提供：

```cpp
using Request = ...;
using Response = ...;
```

创建：

```cpp
auto get_state_type =
    dclcpp::create_service_type<GetState>(
        request_msg_type,
        response_msg_type);
```

内部：

```text
dclcpp::ServiceType<GetState>
        │
        ▼
dmw::ServiceType
```

---

## 12. Client

推荐：

```cpp
auto client = node->create_client(
    get_state_type,
    "/get_state",
    qos);
```

接口：

```cpp
template<class ServiceT>
class Client {
public:
    using Request = typename ServiceT::Request;
    using Response = typename ServiceT::Response;

    std::future<Response> async_send_request(
        const Request& request);

    bool wait_for_service(Duration timeout);
};
```

### 12.1 Pending requests

Pending request registry 放在 `dclcpp::Client`：

```text
RequestId -> promise<Response>
```

原因：

- DMW 负责 transport identity、correlation 与 service availability/wait；
- DCLCPP 负责 future/callback completion。

Response ready：

```text
dmw::Client::read_response()
        ↓
RequestId
        ↓
PendingRequestRegistry
        ↓
promise.set_value(response)
```

Future registry 不下沉到 DMW。`std::promise` / `std::future`、callback completion 和 cancellation policy 属于 C++ Client Library semantics。

### 12.2 Service availability

`wait_for_service()` 不自行 sleep/poll。实现直接包装：

```text
dmw::Client::wait_for_service(WaitTimeout)
```

从而与 DCLPY 使用同一 discovery/wakeup semantics。

---

## 13. Service

推荐：

```cpp
auto service = node->create_service(
    get_state_type,
    "/get_state",
    [](const GetState::Request& req,
       GetState::Response& res) {
        // ...
    });
```

`dclcpp::Service` 不处理 DDS identity 细节。

Executor：

1. WaitSet 告知 service request ready；
2. typed request storage；
3. `dmw::Server::read_request()`；
4. 保存 `RequestId`；
5. 调用用户 callback；
6. `dmw::Server::write_response(request_id, &response)`。

---

## 14. Action 总体设计

Action 的 language-neutral protocol/runtime semantics 位于 DMW，DCLCPP 不直接用 3 Service + 2 Topic 自行维护完整 Action protocol。

底层由 ROS 2-compatible Action 组合构成：

```text
Action
├── SendGoal Service
├── CancelGoal Service
├── GetResult Service
├── Feedback Topic
└── Status Topic
```

但组合、Goal identity、Goal FSM、Goal registry、cancel matching、goal/result/cancel correlation、Action availability、status snapshot 和 aggregate WaitSet readiness 由 DMW 统一实现，使 `dclcpp` 与 `dclpy` 共用同一 runtime semantics。

```text
dclcpp::ActionClient<ActionT>
            │
            ▼
     dmw::ActionClient
            │
            ▼
      Service/Topic primitives


dclcpp::ActionServer<ActionT>
            │
            ▼
     dmw::ActionServer
```

DCLCPP 负责 typed API、typed message field adapter、GoalHandle wrapper、Future、callback 和 Executor dispatch；不复制 DMW 的 Action protocol state machine。

### 14.1 Typed message 与 common runtime 的边界

DMW 不引入 action message reflection/codegen，因此 DCLCPP 仍负责从 generated Action message 中读写：

```text
Goal UUID
accepted flag
acceptance stamp
result status/payload
CancelGoal criteria/response fields
Feedback payload
Status message fields
```

但这些字段一旦转换为语言无关值：

```text
dmw::GoalId
dmw::GoalInfo
dmw::GoalState
dmw::CancelGoalCriteria
dmw::GoalStatusInfo
```

其状态机、匹配、registry 和 lifecycle 由 DMW 负责。

---

## 15. Action type model

ActionT 推荐生成/定义：

```cpp
struct Move {
    using Goal = ...;
    using Result = ...;
    using Feedback = ...;

    using SendGoal = ...;
    using CancelGoal = ...;
    using GetResult = ...;

    using FeedbackMessage = ...;
    using StatusMessage = ...;
};
```

DCLCPP 使用这些强类型消息构造 typed `ActionType<ActionT>`，内部绑定一个 type-erased `dmw::ActionType`：

```text
ActionT + MsgType / ServiceType
            ↓
dclcpp::ActionType<ActionT>
            ↓
      dmw::ActionType
```

DCLCPP 保留 ActionT 的编译期类型信息；DMW 负责运行时 Action descriptor 与 endpoint composition。

---

## 16. Goal FSM

DMW Goal 状态采用：

```text
UNKNOWN
ACCEPTED
EXECUTING
CANCELING
SUCCEEDED
CANCELED
ABORTED
```

状态转换事件采用：

```text
EXECUTE
CANCEL_GOAL
SUCCEED
ABORT
CANCELED
```

DCLCPP 不维护第二套 `GoalStateMachine`。

DCLCPP 的 GoalHandle 只负责：

- typed Goal/Result/Feedback 访问；
- 保存 `dmw::GoalId` 与 Action runtime weak/shared reference；
- 将 C++ API 操作转发到 DMW Goal runtime；
- Future/callback 生命周期；
- 将 DMW state/error 映射为 C++ API。

非法状态转换由 DMW 返回明确错误，DCLCPP 负责转换为统一的 C++ status/exception model。

---

## 17. ActionServer

内部：

```text
ActionServer<ActionT>
├── dmw::ActionServer
├── typed ServerGoalHandle wrappers
├── goal/cancel/execute callbacks
└── typed result/feedback/status conversion
```

DMW `ActionServer` 负责公共 runtime：

```text
3 Service + 2 Topic composition
Goal identity / Goal FSM
Goal registry
Cancel criteria matching
Terminal/result lifecycle state
Pending result request transport state
Action availability/discovery authority
Aggregate WaitSet readiness
```

### 17.1 Goal request

流程：

```text
dmw ActionServer goal-request ready
        ↓
DCLCPP take typed SendGoal request
        ↓
extract GoalId
        ↓
user goal callback
        ↓
accepted?
   ┌────┴────┐
   │         │
  yes        no
   │         │
DMW accept   DMW keeps no accepted goal
   │         │
write typed SendGoal response
```

DMW 的 `accept_goal(GoalInfo)` 是 Goal registry/FSM commit；用户 callback 是否接受目标仍属于 DCLCPP policy。

### 17.2 GoalHandle

```text
DMW GoalId/state
        ↓
dclcpp::ServerGoalHandle<ActionT>
```

GoalHandle 不重新成为 Goal FSM authority。

### 17.3 Result lifecycle

DMW 维护：

- terminal Goal metadata；
- pending GetResult request identity；
- result available/retention/expiry protocol state。

DCLCPP 负责 typed Result message/object。若 DMW public API 使用 type-erased result retention，则 DCLCPP 只提供对应 `void*` backing；如果 DMW 只保存 protocol state，则 typed result ownership仍由 DCLCPP wrapper 管理。两者以 DMW public contract 为唯一准则，不允许 C++ 与 Python 出现不同 expiry semantics。

### 17.4 Cancellation

必须支持：

- 指定 GoalId；
- timestamp 之前的一组 goal；
- cancel all；
- unknown goal；
- already terminal；
- cancel callback 决策。

DMW 根据 `CancelGoalCriteria` 和 Goal registry 计算可取消 Goal selection；用户 cancel callback 仍由 DCLCPP Executor 调度。用户接受取消后，DCLCPP 请求 DMW 对对应 Goal 执行 `CANCEL_GOAL` transition。

---

## 18. ActionClient

内部：

```text
ActionClient<ActionT>
├── dmw::ActionClient
├── typed ClientGoalHandle wrappers
├── acceptance/result/cancel Future registries
└── feedback/status callbacks/views
```

### 18.1 Future registry 留在 C++ 层

DCLCPP 维护：

```text
Goal request RequestId   -> promise<GoalHandle>
Cancel request RequestId -> promise<CancelResponse>
Result request RequestId -> promise<ResultResponse>
GoalId                   -> feedback callback / weak GoalHandle
```

DMW 不知道 `std::promise` / `std::future`。

### 18.2 Aggregate readiness

一个 `dmw::ActionClient` 在 DMW WaitSet 中只占一个 logical registration。它内部可由以下任一子通道触发：

```text
Goal response
Cancel response
Result response
Feedback
Status
```

Executor 收到 ActionClient ready token 后查询/取得 DMW 的 `ActionClientReady` snapshot，再按 ready 子通道完成 typed take/Future/callback dispatch。DCLCPP 不把五个底层 endpoint 分别注册成自己的 Action transport topology。

### 18.3 Action server availability

```cpp
bool wait_for_action_server(Duration timeout);
```

直接包装 DMW `ActionClient::wait_for_server()`；不得在 C++ 层 polling 五个 endpoint。

---

## 19. Timer / WaitSet / Graph

### 19.1 Timer

Timer 的 period、deadline/readiness、cancel/reset 和 WaitSet integration 下沉到 `dmw::Timer`。DCLCPP Timer 只增加 C++ callback 和 typed/RAII convenience：

```text
dclcpp::Timer
    ├── dmw::Timer
    └── std::function callback
```

概念接口：

```cpp
class Timer {
public:
    void cancel();
    void reset();
    bool is_canceled() const;
    std::chrono::nanoseconds period() const;
    void period(std::chrono::nanoseconds value);
    std::chrono::nanoseconds time_until_trigger() const;
};
```

Timer 不创建自己的 callback thread。到期 readiness 由 DMW WaitSet 报告。Executor 在执行 callback 前先调用 DMW `Timer::take(TimerInfo&)`，只有成功消费本次触发才执行 C++ callback；`TimerInfo` 可映射为 C++ expected/actual call time。

### 19.2 WaitSet

`dclcpp::WaitSet` 是 `dmw::WaitSet` 的 C++ typed wrapper。

用户高级 API 可以：

```cpp
wait_set.add_subscription(sub);
wait_set.add_client(client);
wait_set.add_service(service);
wait_set.add_timer(timer);
wait_set.add_action_client(action_client);
wait_set.add_action_server(action_server);
wait_set.add_graph_event(graph_event);
```

内部最终只注册相应 `dmw` entity。DCLCPP 不自行展开 Action 的 3 Service + 2 Topic。

### 19.3 Graph

DCLCPP 不建立独立 discovery cache。公开的 C++ Graph API 是 DMW value 的容器/命名风格 wrapper，例如：

```cpp
GraphSnapshot graph = context->graph_snapshot();
auto event = context->create_graph_event();
```

DCLCPP 可以提供：

```text
get_node_names()
get_topic_names_and_types()
get_service_names_and_types()
count_publishers()
count_subscribers()
count_clients()
count_services()
```

但实现必须从一次 DMW `GraphSnapshot` 或 DMW query 获得数据，不得维护与 DMW DiscoveryGraph 平行的长期 cache。

GraphEvent 可以进入 Executor WaitSet，用于 endpoint topology 变化后重新构建高层 waitables/introspection view；GraphEvent readiness 本身不是用户 callback，是否向用户暴露 graph callback 由 DCLCPP API 决定。

---

## 20. Executor

## 20.1 V1 只实现 SingleThreadedExecutor

```cpp
class SingleThreadedExecutor {
public:
    void add_node(std::shared_ptr<Node> node);
    void remove_node(...);

    void spin();
    void spin_once(Duration timeout);
    void spin_some();
};
```

## 20.2 调度流程

```text
Executor
   │
   ▼
construct/update dmw::WaitSet
   │
   ▼
wait()
   │
   ▼
ReadySet
   │
   ├── Subscription → read → callback
   ├── Service      → read → callback → response
   ├── Client       → read → fulfill promise
   ├── Timer        → take → callback
   ├── ActionClient → ready snapshot → Future/callback dispatch
   ├── ActionServer → ready snapshot → typed request/callback/response
   └── GraphEvent   → consume revision → topology/introspection refresh
```

Executor 负责执行策略和语言层任务调度，不成为 Timer、Graph、Service correlation 或 Action Goal FSM authority。

## 20.3 不做 ROS 2 Executor 复杂度复制

V1 不引入：

- CallbackGroup；
- AnyExecutable；
- MemoryStrategy；
- multi executor hierarchy；
- intra-process manager。

有实际需求后再增加。

---

## 21. Node endpoint ownership

Node 内部维护 endpoint weak/shared registry，以支持 executor 构建 waitables。

建议：

```text
Node
├── publishers
├── subscriptions
├── clients
├── services
├── timers
└── actions
```

但不要把 Node 变成全局 middleware manager；endpoint 自身保持 RAII，Node registry 主要用于 C++ callback ownership 和 executor wiring。Discovery/Graph authority 位于 DMW Context。

---

## 22. Compatibility

DCLCPP 不自行实现 DDS naming/QoS/type/service/action wire mapping。Context 的 compatibility/runtime mode 传递给 DMW，并由 DMW 对所有 endpoint 保持一致。

概念上支持：

```cpp
Compatibility::NativeDDS
Compatibility::ROS2
```

实际 DDS naming/QoS/type mapping 交给 DMW。

DCLCPP Action 在 ROS2 模式下使用 DMW 提供的 ROS 2 Action endpoint naming 和 protocol semantics。Humble 与 Jazzy 的 wire/runtime compatibility 由 DMW 双环境验证矩阵保证，而不是在 DCLCPP 中维护 distro-specific Action implementation。

---

## 23. 错误处理

推荐策略：

- DMW 返回 `Result/Error`；
- DCLCPP 对构造/配置失败可抛 `dclcpp::Exception`；
- 高频路径如 `publish()` 可返回轻量 status 或使用统一异常策略；
- V1 需要统一，不应混用 `bool + log`。

建议异常层级：

```text
DclcppError
├── InvalidArgumentError
├── InvalidStateError
├── TimeoutError
├── MiddlewareError
└── TypeError
```

Exception mapping 属于 C++ Client Library，不下沉到 DMW；同一个 DMW `ErrorCode` 到 C++ exception 的规则必须集中实现，Timer/Action/Graph 不各自发明异常体系。

---

## 24. 线程安全

### Publisher

`publish()` 设计为可并发。

### Subscription/Service/Timer callback

SingleThreadedExecutor 下顺序执行。

### Client futures

pending registry 必须线程安全，以支持发送线程与 executor response thread 分离。

### Action

Goal protocol state、Goal FSM、Goal registry、cancel selection 和 common result lifecycle 的并发规则由 DMW 定义。DCLCPP 只为 typed GoalHandle、Future/callback registry 以及 Executor dispatch 定义线程安全策略，优先选择清晰的单 executor ownership，避免过早引入复杂锁。

### Graph

GraphSnapshot 是不可变 value snapshot；GraphEvent 的 cursor/revision 由 DMW 管理。DCLCPP 不用自己的锁保护第二份 discovery cache。

---

## 25. 目录结构

```text
dclcpp/
├── CMakeLists.txt
├── include/
│   └── dclcpp/
│       ├── context.hpp
│       ├── node.hpp
│       ├── msg_type.hpp
│       ├── create_msg_type.hpp
│       ├── service_type.hpp
│       ├── create_service_type.hpp
│       ├── action_type.hpp
│       ├── publisher.hpp
│       ├── subscription.hpp
│       ├── client.hpp
│       ├── service.hpp
│       ├── timer.hpp
│       ├── action_client.hpp
│       ├── action_server.hpp
│       ├── client_goal_handle.hpp
│       ├── server_goal_handle.hpp
│       ├── graph.hpp
│       ├── qos.hpp
│       ├── message_info.hpp
│       ├── wait_set.hpp
│       ├── executor.hpp
│       ├── single_threaded_executor.hpp
│       └── dclcpp.hpp
└── src/
    ├── context.cpp
    ├── node.cpp
    ├── qos.cpp
    ├── graph.cpp
    ├── timer.cpp
    ├── wait_set.cpp
    ├── executor.cpp
    └── action/
        ├── action_client.cpp
        ├── action_server.cpp
        └── goal_handle.cpp
```

模板实现建议：

- 简单模板直接在 `.hpp`；
- 大型模板可拆 `.ipp` / `detail/*.hpp`；
- 不创建无实际内容的 `.cpp`。

---

## 26. 示例 API

### Topic

```cpp
auto context = std::make_shared<dclcpp::Context>();
auto node = std::make_shared<dclcpp::Node>(context, "demo");

auto type = dclcpp::create_msg_type<MyMsg, MyMsgPubSubType>();

auto pub = node->create_publisher(type, "/demo", dclcpp::QoS(10));

auto sub = node->create_subscription(
    type,
    "/demo",
    dclcpp::QoS(10),
    [](const MyMsg& msg) {
        // ...
    });

dclcpp::SingleThreadedExecutor executor;
executor.add_node(node);
executor.spin();
```

### Service

```cpp
auto service_type = dclcpp::create_service_type<MyService>(
    request_type,
    response_type);

auto server = node->create_service(
    service_type,
    "/compute",
    [](const MyService::Request& request,
       MyService::Response& response) {
        // ...
    });
```

---

## 27. 测试计划

### Type system

- create_msg_type；
- MessageType reuse；
- invalid type binding；
- type lifetime。

### Topic

- pub/sub；
- multiple pubs/subs；
- QoS；
- shutdown；
- executor delivery。

### Service

- request/response；
- async future；
- timeout；
- multiple clients；
- unavailable service；
- `wait_for_service()` 使用 DMW blocking wait，不 polling。

### QoS

- DCLCPP profile 与 DMW profile 完全一致；
- wrapper mutation 不改变 DMW profile authority。

### Timer

- periodic readiness；
- cancel/reset；
- period change；
- TimerInfo；
- Executor callback；
- shutdown；
- 不存在额外 timer thread。

### Graph

- snapshot conversion；
- GraphEvent wake；
- counts/name/type query；
- 不建立独立 cache；
- Context shutdown。

### Action

- typed ActionType binding；
- typed field ↔ DMW protocol-value conversion；
- GoalHandle wrapper；
- acceptance/result/cancel Future completion；
- feedback/status callback；
- cancel callback integration；
- aggregate readiness；
- multiple goals；
- multiple clients；
- shutdown；
- DMW Action runtime error/state mapping。

Goal FSM、Goal registry、cancel matching、result lifecycle、endpoint composition 和 transport correlation 的完整 protocol tests 属于 DMW；DCLCPP 不重复建立第二套同语义测试矩阵。

### ROS 2 interoperability

- Topic 双向；
- Service 双向；
- Action 双向；
- Humble/Jazzy 均由 DMW runtime contract 验证。

---

## 28. 开发顺序

1. Context / Node wrapper；
2. MsgType / create_msg_type；
3. Publisher / Subscription；
4. QoS/profile wrapper；
5. Timer / Graph / WaitSet / SingleThreadedExecutor；
6. ServiceType；
7. Client / Service；
8. Future/pending request；
9. ActionType / protocol-value adapter / GoalHandle wrapper；
10. ActionServer/ActionClient wrapper；
11. ROS 2 interoperability；
12. MultiThreadedExecutor/CallbackGroup/advanced features。

Timer、Graph 与 Action wrapper 的开发以前置 DMW public contract 稳定为条件。

---

## 29. V1 冻结项

1. C++17；
2. typed templates only in DCLCPP；
3. `MsgType<MsgT>` 是 C++ 类型描述；
4. `create_msg_type<Msg, MsgPubSubType>()` 一次绑定；
5. 后续 endpoint API 不重复传 PubSubType；
6. `Publisher<T>` / `Subscription<T>`；
7. `Client<S>` / `Service<S>`；
8. Service availability/wait 直接复用 DMW；
9. common QoS profile 数值来自 DMW，DCLCPP 只包装；
10. Timer 的 runtime/readiness 位于 DMW，DCLCPP 只增加 C++ callback wrapper；
11. Graph authority/cache/revision 位于 DMW，DCLCPP 只提供 value/query wrapper；
12. Action 的 endpoint composition、protocol/runtime state、Goal FSM/registry、cancel matching和 availability 位于 DMW；
13. DCLCPP 提供 typed `ActionType`、typed field adapter、GoalHandle、Future 和 callback wrapper；
14. Pending Future registry 保留 DCLCPP；
15. Executor 在 DCLCPP；
16. V1 只实现 SingleThreadedExecutor；
17. CallbackGroup/ThreadPool 不作为 V1 前提；
18. callbacks 不在 Fast DDS/DMW internal thread；
19. C++ exception mapping 保留 DCLCPP；
20. ROS 2 low-level mapping 交给 DMW；
21. Fast DDS public types 不进入普通 DCLCPP API。

---

## 30. 总结

DCLCPP 的核心职责可以概括为：

```text
把 DMW 的非模板、type-erased communication/runtime primitives，
转换成现代、强类型、RAII、callback/future 友好的 C++ API。
```

其最重要的边界是：`dclcpp` 负责类型安全、typed message 字段访问、C++ callback/Future、exception 和 Executor 调度；`dmw` 负责 DDS 语义以及跨语言必须一致的 QoS profile、Timer、Graph、Service、Action protocol/runtime state。类型绑定通过 `MsgType<Msg>` 一次完成，从而避免用户在整个应用中反复操作 `MsgPubSubType`。