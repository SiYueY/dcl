# DCLCPP 设计文档

> 文档状态：Draft V0.1  
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
10. 复用 `dmw` 提供的 language-neutral runtime、protocol state 和 readiness，不在 C++ Client Library 重复实现与 `dclpy` 平行的底层状态机。

---

## 2. 非目标

DCLCPP V1 不负责：

- DDS entity 直接管理；
- Fast DDS Listener 用户 callback；
- Service request identity 底层实现；
- ROS 2 request/reply DDS mapping；
- Timer deadline/readiness 底层状态机；
- Action endpoint composition、transport correlation 和 Goal FSM 底层实现；
- Python API；
- 多 middleware；
- 完整 ROS Graph；
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
│ QoS                                             │
│ WaitSet / Executor                              │
│ Callback / Future                               │
└───────────────────────┬─────────────────────────┘
                        ▼
                       dmw
                        │
                        ▼
                    Fast DDS
```

`dmw` 不只是 DDS entity wrapper，也承担 `dclcpp` 与 `dclpy` 必须共享的 language-neutral runtime semantics。DCLCPP 不复制 DMW 已经提供的 Timer、Action protocol、Goal FSM、Service correlation、WaitSet readiness 或 ROS 2 wire mapping。

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

### 4.4 Callback 不在 DDS 内部线程执行

所有 subscription/service/timer/action callback 通过 Executor 调度。

DMW 可以维护 readiness、protocol state 和内部 listener，但不得执行用户 callback；DCLCPP 负责把 ready runtime entity 转换为 C++ callback、promise/future completion 或 typed GoalHandle 操作。

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
dmw::fastdds::make_message_type<PubSubType>()
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
2. 调用 `dmw::Subscriber::take(&msg, info)`；
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

可提供 convenience profiles：

```text
DefaultQoS
SensorDataQoS
ServiceQoS
```

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

### Pending requests

Pending request registry 可以放在 `dclcpp::Client`：

```text
RequestId -> promise<Response>
```

原因：

- DMW 负责 transport identity、correlation 与 service availability/wait；
- DCLCPP 负责 future/callback completion。

Response ready：

```text
dmw::Client::take_response()
        ↓
RequestId
        ↓
PendingRequestRegistry
        ↓
promise.set_value(response)
```

Future registry 不下沉到 DMW。`std::promise` / `std::future`、callback completion 和 cancellation policy 属于 C++ Client Library semantics。

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
3. `dmw::Server::take_request()`；
4. 保存 `RequestId`；
5. 调用用户 callback；
6. `dmw::Server::send_response(request_id, &response)`。

---

## 14. Action 总体设计

Action 的 language-neutral protocol/runtime semantics 位于 DMW，DCLCPP 不再直接用 3 Service + 2 Topic 自行维护完整 Action protocol。

底层仍然由 ROS 2-compatible Action 组合构成：

```text
Action
├── SendGoal Service
├── CancelGoal Service
├── GetResult Service
├── Feedback Topic
└── Status Topic
```

但组合、Goal identity、Goal FSM、goal/result/cancel correlation、Action availability、status bookkeeping 和 WaitSet readiness 由 DMW 统一实现，使 `dclcpp` 与 `dclpy` 共用同一 runtime semantics。

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

DCLCPP 负责 typed API、GoalHandle wrapper、Future、callback 和 Executor dispatch；不复制 DMW 的 Action protocol state machine。

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

Action Goal 状态仍采用：

```text
ACCEPTED
EXECUTING
CANCELING
SUCCEEDED
CANCELED
ABORTED
```

状态转换事件仍包括：

```text
accept
execute
request_cancel
cancel
succeed
abort
```

但 Goal FSM 属于 language-neutral protocol state，统一由 DMW 实现和验证。DCLCPP 不维护第二套 `GoalStateMachine`。

DCLCPP 的 GoalHandle 只负责：

- typed Goal/Result/Feedback 访问；
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
└── Future/callback-facing state
```

DMW `ActionServer` 内部负责公共 runtime：

```text
3 Service + 2 Topic composition
Goal identity / Goal FSM
Goal registry protocol state
Result cache protocol state
Pending result requests
Cancel matching
Status bookkeeping
```

### GoalHandle

```text
DMW Goal identity/state
        ↓
dclcpp::ServerGoalHandle<ActionT>
```

GoalHandle 不重新成为 Goal FSM authority。

### ResultCache

terminal goal result 的协议级保存、过期和 GetResult correlation 由 DMW 统一维护；DCLCPP 只持有 typed result/future wrapper 所需的语言层引用。

### Cancellation

必须支持：

- 指定 goal；
- 多 goal；
- unknown goal；
- already terminal；
- cancel callback 决策。

其中匹配和状态转换由 DMW 执行；用户 cancel callback 仍由 DCLCPP Executor 调度并把决策返回 DMW。

---

## 18. ActionClient

内部：

```text
ActionClient<ActionT>
├── dmw::ActionClient
├── typed ClientGoalHandle wrappers
├── acceptance/result Future registry
└── feedback/result callbacks
```

ClientGoalHandle：

- Goal UUID/identity 的 typed wrapper；
- acceptance future；
- result future；
- feedback callback；
- latest status view。

底层 goal/result/cancel request correlation、feedback/status endpoint readiness 和 Action availability 由 DMW 负责；DCLCPP 不重新组合三个 Client 和两个 Subscription 来实现 transport protocol。

---

## 19. Timer / WaitSet

### 19.1 Timer

Timer 的 period、deadline/readiness、cancel/reset 和 WaitSet integration 下沉到 `dmw::Timer`。DCLCPP Timer 只增加 C++ callback 和 typed/RAII convenience：

```text
dclcpp::Timer
    ├── dmw::Timer
    └── std::function callback
```

Timer 不创建自己的 callback thread。到期 readiness 由 DMW WaitSet 报告，callback 由 Executor 执行。

### 19.2 WaitSet

`dclcpp::WaitSet` 是 `dmw::WaitSet` 的 C++ typed wrapper。

用户高级 API 可以：

```cpp
wait_set.add_subscription(sub);
wait_set.add_client(client);
wait_set.add_service(service);
wait_set.add_timer(timer);
```

Action common runtime 是否以 Action registration 或其内部 ready token 暴露，由 DMW Action public contract 冻结；DCLCPP 不自行轮询 Action 的 3 Service + 2 Topic。

内部最终注册相应 `dmw` entity。

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
   ├── Subscription → take → callback
   ├── Service      → take → callback → response
   ├── Client       → take → fulfill promise
   ├── Timer        → consume → callback
   └── Action       → dmw runtime event → typed Future/callback dispatch
```

Executor 负责执行策略和语言层任务调度，不成为 Timer、Service correlation 或 Action Goal FSM authority。

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

但不要把 Node 变成全局 manager；endpoint 自身保持 RAII，Node registry 主要用于 introspection/executor wiring。

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
- 高频路径如 `publish()` 可返回轻量 status 或在确定无 recoverable error 时抛异常；
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

---

## 24. 线程安全

### Publisher

`publish()` 设计为可并发。

### Subscription/Service/Timer callback

SingleThreadedExecutor 下顺序执行。

### Client futures

pending registry 必须线程安全，以支持发送线程与 executor response thread 分离。

### Action

Goal protocol state、Goal FSM 和 ResultCache 的并发规则由 DMW 定义。DCLCPP 只需为 typed GoalHandle、Future/callback registry 以及 Executor dispatch 定义清晰的线程安全策略，优先选择清晰的单 executor ownership，避免过早引入复杂锁。

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
- unavailable service。

### Timer

- periodic readiness；
- cancel/reset；
- Executor callback；
- shutdown；
- DMW Timer wrapper behavior。

### Action

- typed ActionType binding；
- GoalHandle wrapper；
- acceptance/result Future completion；
- feedback callback；
- cancel callback integration；
- multiple goals；
- multiple clients；
- shutdown；
- DMW Action runtime error/state mapping。

Goal FSM、result cache、endpoint composition 和 transport correlation 的完整 protocol tests 属于 DMW；DCLCPP 不重复建立第二套同语义测试矩阵。

### ROS 2 interoperability

- Topic 双向；
- Service 双向；
- Action 双向。

---

## 28. 开发顺序

1. Context / Node wrapper；
2. MsgType / create_msg_type；
3. Publisher / Subscription；
4. QoS；
5. Timer / WaitSet / SingleThreadedExecutor；
6. ServiceType；
7. Client / Service；
8. Future/pending request；
9. ActionType / GoalHandle wrapper；
10. ActionServer/ActionClient wrapper；
11. ROS 2 interoperability；
12. MultiThreadedExecutor/advanced features。

其中 Timer 与 Action wrapper 的开发以前置 `dmw::Timer` 和 DMW Action common runtime public contract 稳定为条件。

---

## 29. V1 冻结项

1. C++17；
2. typed templates only in DCLCPP；
3. `MsgType<MsgT>` 是 C++ 类型描述；
4. `create_msg_type<Msg, MsgPubSubType>()` 一次绑定；
5. 后续 endpoint API 不重复传 PubSubType；
6. `Publisher<T>` / `Subscription<T>`；
7. `Client<S>` / `Service<S>`；
8. Timer 的 runtime/readiness 位于 DMW，DCLCPP 只增加 C++ callback wrapper；
9. Action 的 protocol/runtime state 和 Goal FSM 位于 DMW；
10. DCLCPP 提供 typed `ActionType`、GoalHandle、Future 和 callback wrapper；
11. Executor 在 DCLCPP；
12. V1 只实现 SingleThreadedExecutor；
13. callbacks 不在 Fast DDS/DMW internal thread；
14. ROS 2 low-level mapping 交给 DMW；
15. Fast DDS public types不进入普通 DCLCPP API。

---

## 30. 总结

DCLCPP 的核心职责可以概括为：

```text
把 DMW 的非模板、type-erased communication/runtime primitives，
转换成现代、强类型、RAII、callback/future 友好的 C++ API。
```

其最重要的边界是：`dclcpp` 负责类型安全、C++ callback/Future 和 Executor 调度，`dmw` 负责 DDS 语义以及跨语言必须一致的 Timer、Service、Action protocol/runtime state；类型绑定通过 `MsgType<Msg>` 一次完成，从而避免用户在整个应用中反复操作 `MsgPubSubType`。