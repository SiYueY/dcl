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
4. 提供 Topic / Service / Action；
5. 提供 WaitSet / Executor；
6. 提供 RAII、callback、future；
7. 支持 Native DDS 和 ROS 2 Humble compatibility；
8. 使用 Fast DDS-Gen 产物，不重新实现完整 codegen；
9. 允许一次性绑定 `Msg + MsgPubSubType`，后续只使用 `MsgType<Msg>`。

---

## 2. 非目标

DCLCPP V1 不负责：

- DDS entity 直接管理；
- Fast DDS Listener 用户 callback；
- Service request identity 底层实现；
- ROS 2 request/reply DDS mapping；
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
│ MsgType / ServiceType                           │
│ Publisher<T> / Subscription<T>                  │
│ Client<S> / Service<S>                          │
│ ActionClient<A> / ActionServer<A>               │
│ GoalHandle / Goal FSM                           │
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

所有 subscription/service/action callback 通过 Executor 调度。

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
- compatibility default；
- discovery options；
- transport config（后续）。

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

- DMW 负责 transport identity；
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

Action 仅位于 DCLCPP：

```text
Action
├── SendGoal Service
├── CancelGoal Service
├── GetResult Service
├── Feedback Topic
└── Status Topic
```

不新增 DMW Action primitive。

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

DCLCPP Action 使用这些已有 MsgType/ServiceType。

---

## 16. Goal FSM

状态：

```text
ACCEPTED
EXECUTING
CANCELING
SUCCEEDED
CANCELED
ABORTED
```

GoalStateMachine 必须是纯逻辑组件：

- 不依赖 Fast DDS；
- 不依赖 Executor；
- 不依赖 callback；
- 易单元测试。

状态转换由事件驱动：

```text
accept
execute
request_cancel
cancel
succeed
abort
```

非法转换返回明确错误。

---

## 17. ActionServer

内部：

```text
ActionServer<ActionT>
├── Service<SendGoal>
├── Service<CancelGoal>
├── Service<GetResult>
├── Publisher<Feedback>
├── Publisher<Status>
├── GoalRegistry
├── GoalStateMachine
├── ResultCache
└── PendingResultRequests
```

### GoalRegistry

```text
Goal UUID -> ServerGoalHandle
```

### ResultCache

保存 terminal goals 的 result，直到 timeout/cleanup policy 到期。

### Cancellation

必须支持：

- 指定 goal；
- 多 goal；
- unknown goal；
- already terminal；
- cancel callback 决策。

---

## 18. ActionClient

内部：

```text
ActionClient<ActionT>
├── Client<SendGoal>
├── Client<CancelGoal>
├── Client<GetResult>
├── Subscription<Feedback>
├── Subscription<Status>
└── GoalRegistry
```

ClientGoalHandle：

- Goal UUID；
- acceptance future；
- result future；
- feedback callback；
- latest status。

---

## 19. WaitSet

`dclcpp::WaitSet` 是 `dmw::WaitSet` 的 C++ typed wrapper。

用户高级 API 可以：

```cpp
wait_set.add_subscription(sub);
wait_set.add_client(client);
wait_set.add_service(service);
```

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
   └── Action       → composed endpoint dispatch
```

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
└── actions
```

但不要把 Node 变成全局 manager；endpoint 自身保持 RAII，Node registry 主要用于 introspection/executor wiring。

---

## 22. Compatibility

`NodeOptions` 或 endpoint options 指定：

```cpp
Compatibility::NativeDDS
Compatibility::Ros2Humble
```

默认 compatibility 可在 Context/Node 设置，endpoint 可覆盖。

实际 DDS naming/QoS/type mapping 交给 DMW。

DCLCPP Action 在 ROS2Humble 模式下采用 ROS 2 Action endpoint naming 和 semantics。

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

### Subscription/Service callback

SingleThreadedExecutor 下顺序执行。

### Client futures

pending registry 必须线程安全，以支持发送线程与 executor response thread 分离。

### Action

GoalRegistry/ResultCache 必须定义 executor-thread-only 或显式 mutex policy，优先选择清晰的单 executor ownership，避免过早引入复杂锁。

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
│       ├── publisher.hpp
│       ├── subscription.hpp
│       ├── client.hpp
│       ├── service.hpp
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
    ├── wait_set.cpp
    ├── executor.cpp
    └── action/
        ├── goal_state_machine.cpp
        ├── goal_registry.cpp
        └── result_cache.cpp
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

### Action

- accept/reject；
- feedback；
- succeed；
- abort；
- cancel；
- multiple goals；
- multiple clients；
- result before completion；
- result cache；
- shutdown。

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
5. WaitSet / SingleThreadedExecutor；
6. ServiceType；
7. Client / Service；
8. Future/pending request；
9. Goal FSM；
10. ActionServer/ActionClient；
11. ROS 2 interoperability；
12. MultiThreadedExecutor/advanced features。

---

## 29. V1 冻结项

1. C++17；
2. typed templates only in DCLCPP；
3. `MsgType<MsgT>` 是 C++ 类型描述；
4. `create_msg_type<Msg, MsgPubSubType>()` 一次绑定；
5. 后续 endpoint API 不重复传 PubSubType；
6. `Publisher<T>` / `Subscription<T>`；
7. `Client<S>` / `Service<S>`；
8. Action 只存在于 DCLCPP；
9. Action = 3 Service + 2 Topic + FSM；
10. Executor 在 DCLCPP；
11. V1 只实现 SingleThreadedExecutor；
12. callbacks 不在 Fast DDS thread；
13. ROS 2 low-level mapping 交给 DMW；
14. Fast DDS public types不进入普通 DCLCPP API。

---

## 30. 总结

DCLCPP 的核心职责可以概括为：

```text
把 DMW 的非模板、type-erased middleware primitives，
转换成现代、强类型、RAII、callback/future 友好的 C++ API。
```

其最重要的边界是：`dclcpp` 负责类型安全和高层语义，`dmw` 负责 DDS 语义；Action 保留在 `dclcpp`，Service 下沉到 `dmw`；类型绑定通过 `MsgType<Msg>` 一次完成，从而避免用户在整个应用中反复操作 `MsgPubSubType`。
