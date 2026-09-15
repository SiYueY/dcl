# DCLCPP 设计文档

> 文档状态：V1 Architecture Frozen Candidate  
> 模块名称：DCLCPP — DDS Client Library for C++  
> 下层依赖：`dmw`  
> C++ 标准：C++17

## 1. 设计目标

`dclcpp` 是 DCL 面向 C++ 应用的高层 Client Library。它把 DMW 的 non-template、type-erased common runtime 转换为现代 C++17 强类型 API。

目标：

1. 提供 typed Publisher/Subscription/Client/Service/Timer/Action；
2. 隐藏 DMW 的 `void*` boundary；
3. 隐藏普通 Fast DDS 类型；
4. 提供 callback、Future/Promise 和 Executor；
5. 提供 C++ GoalHandle；
6. 提供 C++ QoS convenience API；
7. 提供 Graph value wrapper；
8. 复用 DMW 的 Timer、Service availability、Graph、Action FSM 与 readiness；
9. 不在 C++ 层复制 DCLPY 也需要的一套 runtime protocol。

## 2. 非目标

DCLCPP V1 不负责：

- DDS entity 直接管理；
- Fast DDS listener 用户 callback；
- Service SampleIdentity/correlation；
- Service availability discovery；
- Timer deadline/FSM；
- Action 3 Service + 2 Topic 组合；
- Goal FSM；
- cancel candidate matching；
- Action availability；
- DMW Graph cache；
- Python runtime；
- 多 middleware；
- 完整 ROS Node Graph；
- 自定义 IDL compiler。

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
│ Timer + std::function                           │
│ ActionClient<A> / ActionServer<A>               │
│ typed GoalHandle / typed Result cache           │
│ QoS / Graph wrappers                            │
│ Future / Promise / Pending registries            │
│ WaitSet wrapper / SingleThreadedExecutor         │
└───────────────────────┬─────────────────────────┘
                        ▼
                       dmw
                        │
                        ▼
                    Fast DDS
```

DMW 是 middleware/common-runtime authority；DCLCPP 是 typed/language-runtime layer。

## 4. API 原则

### 4.1 强类型

用户 API 主要基于：

```cpp
Publisher<MsgT>
Subscription<MsgT>
Client<ServiceT>
Service<ServiceT>
ActionClient<ActionT>
ActionServer<ActionT>
ClientGoalHandle<ActionT>
ServerGoalHandle<ActionT>
```

### 4.2 RAII

用户不手动 delete DDS entity。DCLCPP wrapper 使用 RAII 持有对应 DMW resource。

### 4.3 普通业务代码不接触 Fast DDS

Fast DDS-Gen 绑定点可以显式使用 `MsgPubSubType`：

```cpp
auto type =
    dclcpp::create_msg_type<Msg, MsgPubSubType>();
```

绑定完成后 Publisher/Subscription/Service/Action 普通业务 API 不再传 `MsgPubSubType`。

### 4.4 callback 只由 Executor 调度

```text
DMW readiness
    ↓
DCLCPP Executor
    ↓
callback / Promise completion
```

DCLCPP 不把 user callback 注册给 Fast DDS listener 或 DMW internal notification thread。

## 5. Context

`dclcpp::Context` 包装 `dmw::Context`：

```cpp
class Context {
public:
    explicit Context(const ContextOptions& options = {});
    ~Context();

    void shutdown();
    bool ok() const noexcept;

    GraphSnapshot graph_snapshot() const;
    GraphEvent create_graph_event();

private:
    std::unique_ptr<dmw::Context> context_;
};
```

ContextOptions 可以提供：

- domain id；
- participant name；
- RuntimeMode/compatibility；
- 后续明确支持的 transport/discovery options。

DCLCPP 不维护独立 middleware RuntimeMode。

## 6. Node

```cpp
auto node = std::make_shared<dclcpp::Node>(
    context,
    "controller");
```

Node 创建语言层对象：

```text
Publisher
Subscription
Client
Service
Timer
ActionClient
ActionServer
```

Node 本身不拥有独立 DomainParticipant。

### 6.1 Timer ownership

DMW Timer 是 Context-scoped primitive；`Node::create_timer()` 是 C++ convenience：

1. 从 Node 关联的 Context 创建 `dmw::Timer`；
2. 包装 callback；
3. 把 Timer wrapper 加入 Node 的 Executor registry。

不因此在 DMW 引入 Node->Timer DDS ownership。

## 7. MsgType

### 7.1 目标

Fast DDS-Gen 通常生成：

```text
Msg
MsgPubSubType
```

DCLCPP 将二者一次绑定为：

```cpp
MsgType<Msg>
```

### 7.2 概念定义

```cpp
template<class MsgT>
class MsgType {
public:
    using message_type = MsgT;

    std::string_view type_name() const noexcept;

private:
    dmw::MessageType type_;
};
```

### 7.3 创建

```cpp
template<class MsgT, class PubSubTypeT>
MsgType<MsgT> create_msg_type();
```

内部：

```text
PubSubTypeT
    ↓
dmw::fastdds::create_message_type<PubSubTypeT>()
    ↓
dmw::MessageType
    ↓
MsgType<MsgT>
```

## 8. Publisher

用户：

```cpp
auto pub = node->create_publisher(
    joint_state_type,
    "/joint_states",
    qos);
```

概念：

```cpp
template<class MsgT>
class Publisher {
public:
    void publish(const MsgT& message);

private:
    std::unique_ptr<dmw::Publisher> publisher_;
};
```

调用链：

```text
dclcpp::Publisher<Msg>::publish(msg)
        ↓
dmw::Publisher::write(&msg)
        ↓
Fast DDS DataWriter
```

DCLCPP `publish()` 是用户友好命名；底层 DMW public method 是 `write()`。

## 9. Subscription

```cpp
auto sub = node->create_subscription(
    joint_state_type,
    "/joint_states",
    qos,
    [](const JointState& msg) {
        // ...
    });
```

内部：

```text
Subscription<Msg>
├── dmw::Subscriber
├── MsgType<Msg>
└── callback
```

Executor：

```text
Subscriber ready
    ↓
construct/reuse Msg
    ↓
dmw::Subscriber::read(&msg, info)
    ↓
callback(msg)
```

## 10. QoS

`dclcpp::QoS` 是 `dmw::Qos` 的 C++ convenience wrapper。

```cpp
dclcpp::QoS qos(10);
qos.reliable();
qos.volatile_durability();
```

### 10.1 Common profile authority

数值只由 DMW 定义：

```text
SystemDefaultQoS    -> dmw::Qos::system_default()
DefaultQoS          -> dmw::Qos::ros2_default()
SensorDataQoS       -> dmw::Qos::ros2_sensor_data()
ServicesQoS         -> dmw::Qos::ros2_services_default()
ParametersQoS       -> dmw::Qos::ros2_parameters()
ParameterEventsQoS  -> dmw::Qos::ros2_parameter_events()
ActionStatusQoS     -> dmw::Qos::ros2_action_status_default()
```

DCLCPP 不保存第二份 preset 数值表。

## 11. ServiceType

```cpp
template<class ServiceT>
class ServiceType;
```

其中 ServiceT 提供：

```cpp
using Request = ...;
using Response = ...;
```

内部绑定：

```text
dclcpp::ServiceType<ServiceT>
        ↓
dmw::ServiceType
```

## 12. Client

```cpp
auto client = node->create_client(
    get_state_type,
    "/get_state",
    qos);
```

概念接口：

```cpp
template<class ServiceT>
class Client {
public:
    using Request = typename ServiceT::Request;
    using Response = typename ServiceT::Response;

    std::future<Response>
    async_send_request(const Request& request);

    bool wait_for_service(Duration timeout);
};
```

### 12.1 Pending Future registry

DCLCPP 维护：

```text
dmw::RequestId -> std::promise<Response>
```

发送：

```text
typed Request
    ↓
dmw::Client::write_request(&request)
    ↓
RequestId
    ↓
insert Promise
```

接收：

```text
dmw::Client ready
    ↓
read_response(&response, request_id)
    ↓
Pending registry
    ↓
promise.set_value(response)
```

Future cancellation/exception/completion policy只属于 DCLCPP。

### 12.2 Service availability

```text
dclcpp::Client::wait_for_service()
        ↓
dmw::Client::wait_for_service(WaitTimeout)
```

DCLCPP 不 `sleep_for()` polling，也不维护独立 discovery cache。

## 13. Service

```cpp
auto service = node->create_service(
    type,
    "/get_state",
    [](const Request& req, Response& res) {
        // ...
    });
```

Executor flow：

```text
Server ready
    ↓
dmw::Server::read_request()
    ↓
typed callback
    ↓
dmw::Server::write_response(RequestId, &response)
```

DDS identity 对 typed callback 不可见。

## 14. ActionType

DCLCPP typed ActionT 概念：

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

```text
ActionT + typed MsgType/ServiceType
        ↓
dclcpp::ActionType<ActionT>
        ↓
dmw::ActionType
```

DMW 只保存 wire/runtime descriptors，不保留 C++ template information。

## 15. Action 总体边界

底层：

```text
Action
├── SendGoal Service
├── CancelGoal Service
├── GetResult Service
├── Feedback Topic
└── Status Topic
```

DMW 负责五 endpoint composition、Goal FSM、cancel matching、result common lifecycle、availability 和 aggregate readiness。

DCLCPP 负责：

- Action message typed field access；
- GoalHandle presentation；
- user callback；
- Future/Promise；
- Pending Future registry；
- typed result payload cache；
- Executor dispatch。

## 16. Typed Action metadata adapter

Action message中的公共字段转换：

```text
Goal UUID             -> dmw::GoalId
acceptance timestamp  -> dmw::GoalInfo
CancelGoal request    -> dmw::CancelGoalCriteria
Goal status list      <-> vector<dmw::GoalStatusInfo>
```

这些转换属于 ActionT adapter，不等于重新实现 Goal FSM。

DMW 不引入 reflection/codegen 读取任意 ActionT object。

## 17. ActionServer

内部：

```text
ActionServer<ActionT>
├── dmw::ActionServer
├── typed ServerGoalHandle wrappers
├── typed result cache
├── goal/cancel/execute callbacks
└── typed feedback/status conversion
```

### 17.1 Goal request transaction

安全流程不是“先 `accept_goal()` 再写 response”的两个独立公共步骤，而是：

```text
DMW goal request ready
    ↓
take typed SendGoal request
    ↓
extract GoalId
    ↓
user goal callback
    ├── reject
    │    -> DMW reject/write response path
    │
    └── accept
         -> prepare typed accepted response
         -> DMW accept transaction:
              reserve GoalId
              write accepted response
              commit Accepted / optional Executing
              rollback reservation on write failure
```

DCLCPP 不自行实现 Goal registry rollback。

### 17.2 ServerGoalHandle

```text
dmw::GoalId + ActionServer runtime
        ↓
dclcpp::ServerGoalHandle<ActionT>
```

GoalHandle methods：

```text
execute
succeed
abort
canceled
```

最终都转发到 DMW Goal transition。

### 17.3 Cancel

流程：

```text
typed CancelGoal request
    ↓
convert to CancelGoalCriteria
    ↓
DMW select_cancel_goals()
    ↓
for each candidate:
    user cancel callback
       ├── reject -> no DMW state change
       └── accept -> DMW CancelGoal transition
    ↓
build typed CancelGoal response
    ↓
DMW write_cancel_response()
```

selection/FSM 只在 DMW。

### 17.4 Result lifecycle

DMW 保存：

```text
Goal state
terminal timestamp
pending GetResult RequestIds
result expiry
```

DCLCPP 保存：

```text
GoalId -> typed ActionT::Result response/payload
```

terminal transition：

1. DCLCPP 先准备/保存 typed result；
2. 请求 DMW terminal transition；
3. DMW 返回/允许取得 pending result RequestIds；
4. DCLCPP 为每个 RequestId 构造 typed GetResult response；
5. DMW 负责 transport response write；
6. result_timeout 到期，DMW 报告 expired GoalId；
7. DCLCPP 删除 typed cache。

不再存在“DMW 也许缓存 payload，也许不缓存”的双重设计。

### 17.5 status

DMW `status_snapshot()` 返回 `GoalStatusInfo` 列表；DCLCPP 构造 ActionT 对应 status message并调用 DMW publish。

状态 authority仍在 DMW。

## 18. ActionClient

内部：

```text
ActionClient<ActionT>
├── dmw::ActionClient
├── typed ClientGoalHandle wrappers
├── Goal/Cancel/Result Promise registries
└── feedback/status language state
```

### 18.1 Future registry

```text
Goal RequestId   -> promise<GoalResponse/GoalHandle>
Cancel RequestId -> promise<CancelResponse>
Result RequestId -> promise<ResultResponse>
GoalId           -> feedback callback / weak GoalHandle
```

DMW 不认识 `std::promise`。

### 18.2 Aggregate readiness

一个 DMW ActionClient registration 对应：

```cpp
dmw::ActionClientReadySet {
    goal_response,
    cancel_response,
    result_response,
    feedback,
    status
}
```

Executor 收到一个 ActionClient token 后查询 ReadySet，再执行 typed take/Future completion。

DCLCPP 不把内部 5 endpoint分开加入自己的 WaitSet topology。

### 18.3 availability

```text
wait_for_action_server()
    ↓
dmw::ActionClient::wait_for_server()
```

不轮询五个 endpoint。

## 19. Timer

```text
dclcpp::Timer
├── std::unique_ptr<dmw::Timer>
└── callback
```

建议 C++ wrapper：

```cpp
class Timer {
public:
    void cancel();
    void reset();
    bool is_canceled() const;
    bool is_ready() const;

    std::chrono::nanoseconds period() const;
    std::chrono::nanoseconds exchange_period(
        std::chrono::nanoseconds new_period);

    std::chrono::nanoseconds time_until_next_call() const;
};
```

若提供 fluent `period(new_period)` convenience，它必须内部调用 DMW `exchange_period()`，不得重新定义 deadline semantics。

Executor：

```text
Timer token ready
    ↓
dmw::Timer::consume(TimerInfo&)
    ├── false -> stale readiness, no callback
    └── true  -> callback
```

DCLCPP 不计算 next deadline 或 missed cycles。

## 20. Graph

DCLCPP 不建立长期 discovery cache。

V1 wrapper 基于 DMW：

```text
GraphSnapshot
GraphRevision
GraphEvent
TopicGraphInfo
ServiceGraphInfo
ActionGraphInfo
```

可以提供 C++ convenience：

```text
get_topic_names_and_types()
get_service_names_and_types()
get_action_names()
count_publishers()
count_subscribers()
count_clients()
count_server_candidates()
```

**V1 不提供 `get_node_names()` 作为 DMW Graph wrapper。**

完整 ROS Node identity 需要后续 ROS Graph metadata protocol，不能从 DDS Participant name 猜测。

## 21. WaitSet

`dclcpp::WaitSet` 包装 `dmw::WaitSet`。

用户 convenience：

```cpp
wait_set.add_subscription(sub);
wait_set.add_client(client);
wait_set.add_service(service);
wait_set.add_timer(timer);
wait_set.add_action_client(action_client);
wait_set.add_action_server(action_server);
wait_set.add_graph_event(graph_event);
```

内部只注册一个对应 DMW resource。

DCLCPP 不维护第二个 native waiting implementation。

## 22. SingleThreadedExecutor

V1：

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

调度：

```text
DMW WaitSet
    ↓
Ready registrations
    ├── Subscription -> read -> callback
    ├── Server -> read request -> callback -> response
    ├── Client -> read response -> fulfill promise
    ├── Timer -> consume -> callback
    ├── ActionClient -> ReadySet -> Future/callback
    ├── ActionServer -> ReadySet -> typed callback/protocol write
    └── GraphEvent -> take revision -> refresh high-level view
```

V1 不引入 ROS 2 Executor 的完整 CallbackGroup/AnyExecutable/MemoryStrategy hierarchy。

## 23. Node registry

Node 可维护 weak/high-level registry，用于 Executor 构建 waitable view：

```text
publishers
subscriptions
clients
services
timers
action clients/servers
```

该 registry 是语言层 object registry，不是 discovery graph authority。

Graph authority仍是 DMW Context。

## 24. Error mapping

DMW：

```text
Result<T> / ErrorCode
```

DCLCPP 集中映射为 C++ API policy，例如：

```text
DclcppError
├── InvalidArgumentError
├── InvalidStateError
├── TimeoutError
├── MiddlewareError
└── TypeError
```

不允许 Timer/Action/Graph 各自发明另一套 exception mapping。

## 25. 线程安全

### 25.1 Publisher

`publish()` 可以并发，具体以 DMW contract 为准。

### 25.2 Pending Future registry

必须同步，因为发送线程与 Executor response thread 可以不同。

### 25.3 callbacks

SingleThreadedExecutor 下用户 callback 串行执行。

### 25.4 Action

Goal FSM、cancel/result common state由 DMW同步；DCLCPP只同步 typed cache、GoalHandle wrapper和 Future registry。

### 25.5 Graph

GraphSnapshot 是 value snapshot；DCLCPP不保护第二份 cache。

## 26. 目录结构

```text
dclcpp/
├── CMakeLists.txt
├── include/dclcpp/
│   ├── context.hpp
│   ├── node.hpp
│   ├── msg_type.hpp
│   ├── create_msg_type.hpp
│   ├── service_type.hpp
│   ├── action_type.hpp
│   ├── publisher.hpp
│   ├── subscription.hpp
│   ├── client.hpp
│   ├── service.hpp
│   ├── timer.hpp
│   ├── action_client.hpp
│   ├── action_server.hpp
│   ├── client_goal_handle.hpp
│   ├── server_goal_handle.hpp
│   ├── graph.hpp
│   ├── qos.hpp
│   ├── wait_set.hpp
│   ├── executor.hpp
│   ├── single_threaded_executor.hpp
│   └── dclcpp.hpp
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

不要为了与 ROS 2 package 数量一致拆出额外 `dclcpp_action` package，除非未来编译依赖或发布需求证明有必要。

## 27. 测试计划

### 27.1 Type/Topic

- create_msg_type；
- binding lifetime；
- pub/sub；
- QoS；
- MessageInfo；
- Executor callback。

### 27.2 Service

- typed request/response；
- Promise completion；
- timeout；
- multiple clients；
- unavailable service。

Transport identity/availability pairing的完整 tests属于 DMW。

### 27.3 Timer

- C++ wrapper；
- `consume()` before callback；
- stale ready token；
- cancel/reset/exchange；
- callback exception policy。

Scheduling/missed-cycle tests属于 DMW。

### 27.4 Action

- ActionT adapter；
- GoalHandle；
- goal accept transaction wrapper；
- Goal/Cancel/Result Future registry；
- typed result payload cache；
- feedback/status mapping；
- callback decisions；
- expiry removes typed cache；
- aggregate ReadySet dispatch。

Goal FSM/cancel selection/pending RequestId/expiry protocol tests属于 DMW，不在 C++ 重复完整状态机矩阵。

### 27.5 Graph

- DMW GraphSnapshot -> C++ containers；
- GraphEvent Executor integration；
- no persistent duplicate cache；
- no Node graph claim in V1。

## 28. 实现顺序

建议压缩为四组：

1. Foundation wrapper：Context/Node/Message/Topic/QoS；
2. Service/Timer/WaitSet/SingleThreadedExecutor；
3. Graph wrapper + Action typed adapters/GoalHandle/Future/result cache；
4. ROS 2 interoperability 与 API polish。

前提是对应 DMW public contract 已实现。

## 29. V1 冻结项

1. typed templates只在 DCLCPP。
2. DCLCPP 不直接管理 DDS entity。
3. `Publisher<T>::publish()` 调用 DMW `Publisher::write()`。
4. `Subscription<T>` 调用 DMW `Subscriber::read()`。
5. Service Future registry留 C++ 层。
6. Service availability wait使用 DMW。
7. Timer deadline/readiness在 DMW。
8. Executor调用 DMW `Timer::consume()` 后才执行 callback。
9. Action 3 Service + 2 Topic、Goal FSM、cancel/result common state在 DMW。
10. typed Action result payload cache在 DCLCPP。
11. Goal accept使用 DMW transaction path，不由 DCLCPP拼不安全的两步操作。
12. ActionClientReadySet/ActionServerReadySet由 DMW提供。
13. Graph authority在 DMW；V1不宣称 Node graph。
14. QoS preset数值只由 DMW定义。
15. Executor在 DCLCPP；V1仅 SingleThreadedExecutor。
16. callbacks不运行在 Fast DDS/DMW internal thread。
17. C++ exception mapping留 DCLCPP。

## 30. 总结

DCLCPP 的职责可以概括为：

> **把 DMW 的稳定、type-erased middleware/common-runtime 能力转换为现代 C++17 强类型 API，并负责所有 C++ 语言层 Future、callback、typed payload 与 Executor 语义。**

它不再重复实现 Timer scheduler、Service discovery、Graph cache、Goal FSM 或 Action transport protocol，因此 C++ 与 Python 可以保持同一底层行为，同时各自拥有自然的语言 API。
