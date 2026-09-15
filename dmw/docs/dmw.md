# DMW 设计文档

| 属性 | 值 |
| --- | --- |
| 文档状态 | V1 Design Frozen Candidate |
| 模块名称 | DMW — DDS Middleware / Common Runtime Layer |
| 上层 | `dclcpp`、`dclpy` / `_dclpy` |
| 下层 | Fast DDS |
| 语言标准 | C++17 |
| 主要实现参考 | Fast DDS 2.14.x、`rmw_fastrtps` Jazzy（平等参考基线） |
| Common Runtime 参考 | ROS 2 Jazzy `rcl`、`rcl_action` |
| Client Library 边界参考 | ROS 2 Jazzy `rclcpp`、`rclpy` |
| 兼容性验证目标 | ROS 2 Humble / Fast DDS 2.6.x；ROS 2 Jazzy / Fast DDS 2.14.x |

## 1. 文档目的与规范层级

本文档是 DCL 项目中 `dmw` 模块 V1 的公共运行时规范，定义：

- 模块职责边界；
- public API 与对象模型；
- Context / Node / Topic / Service / Timer / Action / Graph / WaitSet；
- QoS、identity、discovery 与 ROS 2 wire compatibility；
- 生命周期、并发、错误与 teardown 语义；
- `dclcpp` / `dclpy` 必须共享的 language-neutral runtime contract。

Fast DDS 专用实现规则由 [`dmw_fastdds.md`](dmw_fastdds.md) 定义。规范层级固定为：

```text
本文件 dmw.md
    │
    │ public/runtime normative contract
    ▼
dmw_fastdds.md
    │
    │ Fast DDS implementation contract
    ▼
Fast DDS / DDSI-RTPS
```

`dmw_fastdds.md` 不得扩大、收窄或改写本文 public contract。

### 1.1 上游参考模型

后续 DMW 设计与实现采用以下参考关系：

```text
Fast DDS 2.14.x ─────────────┐
                             ├── 对照 / 交叉验证 ──> DMW
rmw_fastrtps Jazzy ──────────┘

rcl / rcl_action Jazzy
    └── language-neutral runtime / protocol / state-machine 参考

rclcpp / rclpy Jazzy
    └── typed/Python Client Library 边界参考
```

**Fast DDS 2.14.x 与 `rmw_fastrtps` Jazzy 是平等的主要参考基线，二者不存在优先级。**

当两者实现策略不同，DMW 必须同时评估：

1. Fast DDS public API semantics 与 guarantees；
2. `rmw_fastrtps` 的生产实践和 ROS 2 interoperability 要求；
3. DMW 自身 public contract、复杂度和生命周期约束；

然后选择适合 DMW 的实现，而不是机械复制任一上游。

ROS 2 Humble / Fast DDS 2.6.x 不再决定新的 DMW 设计；它作为重要兼容性验证目标保留。需要兼容 2.6.x 时，应优先选择 2.6.x 与 2.14.x 均存在的稳定 DDS-PIM 能力，仅在确有 API 差异时增加局部 compatibility shim。

### 1.2 DMW 的定位

DMW 是 Fast DDS binding 与 language-neutral common runtime 的统一核心层：

```text
             dclcpp                    _dclpy
                │                         │
                └───────────┬─────────────┘
                            ▼
                   ┌────────────────┐
                   │      dmw       │
                   └───────┬────────┘
                           ▼
                      Fast DDS
                           ▼
                       DDSI-RTPS
```

DMW：

- 不依赖 `rcl`、`rcl_action`、`rclcpp`、`rclpy` 或 ROS 2 runtime；
- 不提供多 middleware plugin abstraction；
- 不复制 ROS 2 的 package hierarchy；
- 直接承载适合跨语言共享的 `rcl` / `rcl_action` 类职责；
- 不执行 user callback；
- 不提供 C++ Future、Python Future、Executor scheduling 或 Python GIL/asyncio policy。

### 1.3 下沉判定规则

能力满足以下条件时，应由 DMW 实现一次：

```text
与 C++ / Python 类型系统无关
+
两种 Client Library 必须具有相同语义
+
涉及协议、identity、state machine、readiness、discovery 或 lifecycle
```

因此以下能力属于 DMW：

```text
Context / Node runtime
Topic / Service primitive
Service correlation and availability
QoS value and common profiles
WaitSet / GuardCondition / Event
Timer deadline/readiness state
Action endpoint composition
Goal identity / Goal FSM
Action cancel/result/status common state
Graph snapshot/change authority
ROS 2 Fast DDS wire mapping
```

以下能力不得为了减少表面重复而下沉：

```text
C++ template typed API
std::function / Python callable
std::promise / std::future
Python Future / asyncio Future
pending Future registry
Executor callback dispatch
CallbackGroup / ThreadPool policy
C++ exception presentation
Python exception / GIL / asyncio integration
```

## 2. V1 范围与功能矩阵

### 2.1 V1 public/runtime 能力

| 能力 | 归属 | V1 状态 |
| --- | --- | --- |
| Context / Node | DMW | 已实现 / 持续完善 |
| Publisher / Subscriber | DMW | 已实现 / 持续完善 |
| Client / Server | DMW | 已实现 / 持续完善 |
| Service availability / wait | DMW | 已实现 / 持续完善 |
| QoS mapping | DMW | 已实现 / 本文补全 profiles |
| WaitSet | DMW | 已实现 / 持续完善 |
| GuardCondition / Event | DMW | 已实现 / 持续完善 |
| Timer | DMW | **设计冻结，待实现** |
| Graph snapshot/change | DMW | **设计冻结，待实现** |
| ActionType | DMW | **设计冻结，待实现** |
| ActionClient / ActionServer | DMW | **设计冻结，待实现** |
| GoalId / Goal FSM | DMW | **设计冻结，待实现** |
| Action cancel/result/status common state | DMW | **设计冻结，待实现** |
| Future / Promise | Client Library | 不下沉 |
| pending Future registry | Client Library | 不下沉 |
| user callback | Client Library | 不下沉 |
| Executor callback dispatch | Client Library | 不下沉 |
| Python GIL / asyncio | dclpy | 不下沉 |
| C++ template typed API | dclcpp | 不下沉 |

“设计冻结，待实现”表示本文已经冻结 public contract 与职责，不表示当前源码已经具备该实现。

### 2.2 V1 非目标

V1 不实现：

- 完整 ROS GraphCache；
- ROS participant-to-node 元数据协议；
- `ros2 node list` / `ros2 node info` 的完整兼容；
- serialized message public API；
- loaned message；
- zero-copy public API；
- content filtered topic；
- message batch take；
- public actual-QoS query；
- `BestAvailable` QoS negotiation；
- wait-for-all-acked；
- public assert-liveliness；
- DDS Security public API；
- Executor；
- Future；
- Python asyncio；
- C API / stable C ABI；
- IDL parser / code generator；
- Cyclone DDS / Connext backend。

## 3. 核心对象模型

### 3.1 Resource / Entity

以下类型属于具有稳定 identity 的 Resource/Entity：

```text
Context
Node
Publisher
Subscriber
Client
Server
Timer
ActionClient
ActionServer
WaitSet
GuardCondition
Event
GraphEvent
```

统一规则：

```text
Factory
+
Result<std::unique_ptr<T>>
+
non-copyable
+
non-movable
+
stable public address
+
RAII
```

Public ownership 使用 `std::unique_ptr`。内部可以使用 `shared_ptr` / `weak_ptr` 维护 backing lifetime 和 teardown safety，但不得改变 public ownership model。

### 3.2 Value / Descriptor

以下类型属于 value/descriptor：

```text
ContextOptions / NodeOptions
PublisherOptions / SubscriberOptions
ClientOptions / ServerOptions
TimerOptions
ActionClientOptions / ActionServerOptions
WaitSetOptions / GuardConditionOptions
GraphEventOptions

MessageType / ServiceType / ActionType
Qos / QosDuration
Gid / RequestId / MessageInfo
GoalId / GoalInfo / GoalState / GoalEvent / GoalAcceptMode
GoalStatusInfo / CancelGoalCriteria
ActionClientReadySet / ActionServerReadySet
GraphRevision / GraphSnapshot / GraphChangeInfo
WaitTimeout / WaitableRegistration / WaitResult
EventType / EventInfo
Error / Result<T>
```

Value type 不表示 DDS resource ownership。

### 3.3 Factory 归属

```text
Context::create()
      │
      ├── create_node()
      ├── create_wait_set()
      ├── create_guard_condition()
      ├── create_timer()
      └── create_graph_event()

Node
      ├── create_publisher()
      ├── create_subscriber()
      ├── create_client()
      ├── create_server()
      ├── create_action_client()
      └── create_action_server()
```

Timer 不表示网络 endpoint，因此归属 Context/runtime，而不是 DDS Node identity。`dclcpp::Node` / `dclpy.Node` 可以提供语言层 `create_timer()` convenience，并在内部调用其 Context。

ActionClient/ActionServer 需要 logical name resolution，因此归属 Node。

### 3.4 事务式创建

所有 Resource/Entity Factory 必须满足：

```text
validate
   ↓
reserve logical/internal state
   ↓
create middleware/native resources
   ↓
install listeners/conditions
   ↓
commit registry/public visibility
   ↓
return complete object
```

任一步失败：

- 使用 local RAII rollback；
- 不返回 half-valid object；
- 不留下对上层可见的 partial registration。

### 3.5 Type-erased 指针 contract

DMW runtime message path 使用 `void*` / `const void*`，例如：

```cpp
Result<void> Publisher::write(const void* message);
Result<bool> Subscriber::read(void* message, MessageInfo& info);
```

规则：

- `nullptr` 返回 `InvalidArgument`；
- concrete C++ object 必须与 endpoint 绑定的 MessageType 对应；
- DMW V1 不尝试从 `void*` 动态识别真实 C++ type；
- wrong concrete type 属于 programming contract violation；
- `TypeMismatch` 只用于 DMW 可以验证的 descriptor/registry 冲突。

对于 `read()` / `read_response()` / `read_request()` 以及 Action take API：

- `success + false` 时全部 caller output 保持不变；
- middleware sample 被消费前发生 Error，全部 output 保持不变；
- sample 已消费后发生 deserialize/conversion Error，只保证 output 仍可安全析构，内容允许 unspecified。

## 4. Error 与 Result

### 4.1 ErrorCode

V1 使用：

```cpp
enum class ErrorCode
{
    InvalidArgument,
    InvalidState,
    InvalidName,
    TypeMismatch,
    AlreadyExists,
    NotFound,
    AlreadyRegistered,
    NotRegistered,
    Busy,
    Timeout,
    Unsupported,
    IncompatibleQos,
    ParentDestroyed,
    ResourceExhausted,
    DDSError,
    ContextShutdown
};
```

`ErrorCode` 不包含 `Ok`。

### 4.2 Result contract

`Result<T>`：

- 明确为 success 或 failure；
- 不允许无意义 default state；
- 支持 move-only T；
- 提供 `value() & / const& / &&`；
- 提供 `error() & / const& / &&`；
- wrong-alternative access 调用 `std::terminate()`；
- expected middleware/runtime failure 不使用 C++ exception；
- `std::bad_alloc` 允许传播，不映射到 `ResourceExhausted`。

`Result<void>` 使用相同语义。

### 4.3 全局错误优先级

Public runtime operation 按以下优先级判断：

1. 可安全检测的 public 参数错误；
2. Context `ShuttingDown/Shutdown`；
3. Parent destroyed；
4. object-local state，例如 `Busy` / `NotFound` / `AlreadyRegistered`；
5. middleware/runtime error，例如 `DDSError` / `Timeout`。

Timer、Graph、Action 不建立第二套错误优先级。

## 5. Context 与 Node

### 5.1 ContextOptions

```cpp
struct ContextOptions
{
    std::uint32_t domain_id{0};
    std::string participant_name;
    RuntimeMode runtime_mode{RuntimeMode::DDS};
};
```

`RuntimeMode` 在 Context 创建成功后不可修改。

### 5.2 Context API

V1 目标 public surface：

```cpp
class Context
{
public:
    static Result<std::unique_ptr<Context>>
    create(const ContextOptions& options);

    ~Context() noexcept;

    std::uint32_t domain_id() const noexcept;
    bool is_shutdown() const noexcept;
    Result<void> shutdown();

    Result<std::unique_ptr<Node>>
    create_node(const NodeOptions& options);

    Result<std::unique_ptr<WaitSet>>
    create_wait_set(const WaitSetOptions& options = {});

    Result<std::unique_ptr<GuardCondition>>
    create_guard_condition(const GuardConditionOptions& options = {});

    Result<std::unique_ptr<Timer>>
    create_timer(const TimerOptions& options);

    Result<std::unique_ptr<GraphEvent>>
    create_graph_event(const GraphEventOptions& options = {});

    Result<GraphSnapshot> graph_snapshot() const;
    Result<GraphRevision> graph_revision() const;
};
```

现有源码尚未包含 Timer/Graph API；本文冻结的是后续实现目标。

### 5.3 Context state machine

```text
Active
   │
   │ shutdown linearization point
   ▼
ShuttingDown
   │
   │ stop new work + wake waiters + drain
   ▼
Shutdown
   │
   │ last internal reference
   ▼
Destroyed
```

`Active -> ShuttingDown` 是不可逆线性化点。

`is_shutdown()`：

```text
Active        -> false
ShuttingDown  -> true
Shutdown      -> true
```

Concurrent/repeated `shutdown()` 必须归并到同一 terminal result。

### 5.4 Shutdown propagation

进入 ShuttingDown 后必须：

- 禁止创建新 child/resource；
- 中断 Service availability wait；
- 中断 active WaitSet；
- 中断 Timer/Action/Graph wait；
- 阻止新的 middleware operation；
- 允许已有 public object 安全析构；
- 最终进入 Shutdown。

Context facade 可以早于 child facade 析构；内部 Context backing 必须存活到最后一个 child 安全释放。

### 5.5 Node

Node 是 logical entity，不等于 DDS DomainParticipant。

```cpp
struct NodeOptions
{
    std::string node_name;
    std::string node_namespace{"/"};
};
```

一个 Context 固定：

```text
1 DDS Domain ID
1 DomainParticipant
```

多个 Node 共享 Context Participant。

Node facade 析构后，已创建 endpoint 可以继续使用，只要 Context 仍 Active；NodeState 由 endpoint 内部引用保持。

## 6. 类型系统

### 6.1 Gid

```cpp
struct Gid
{
    static constexpr std::size_t Size = 16;
    std::array<std::uint8_t, Size> data{};
};
```

Fast DDS GUID 不进入普通 public API。

### 6.2 MessageType

`MessageType` 是 cheap-copy runtime descriptor handle：

- descriptor 内容 immutable；
- 不存在 invalid default object；
- `type_name()` 表示 DDS wire type name；
- Fast DDS TypeSupport integration 只出现在 `dmw/fastdds/message_type.hpp`。

Type identity：

```text
DDS wire type name
+
BindingIdentity
```

同 wire name + same binding identity 可 reuse；同 wire name + different binding identity 返回 `TypeMismatch`。

### 6.3 ServiceType

```cpp
class ServiceType
{
public:
    ServiceType(MessageType request_type,
                MessageType response_type) noexcept;

    const MessageType& request_type() const noexcept;
    const MessageType& response_type() const noexcept;
};
```

### 6.4 ActionType

ActionType 是 DMW 的 immutable type-erased descriptor：

```cpp
class ActionType
{
public:
    ActionType(
        ServiceType send_goal_type,
        ServiceType cancel_goal_type,
        ServiceType get_result_type,
        MessageType feedback_type,
        MessageType status_type) noexcept;

    const ServiceType& send_goal_type() const noexcept;
    const ServiceType& cancel_goal_type() const noexcept;
    const ServiceType& get_result_type() const noexcept;
    const MessageType& feedback_type() const noexcept;
    const MessageType& status_type() const noexcept;
};
```

它只描述 wire/runtime endpoint types，不保留 `ActionT` C++ template information。

```text
dclcpp::ActionType<ActionT>
            │
            ▼
      dmw::ActionType

Python Action type binding
            │
            ▼
      dmw::ActionType
```

DMW 不通过 ActionType 解析任意用户 message layout；GoalId、cancel criteria、GoalInfo 等 common metadata 通过独立 sidecar value 进入 common runtime，typed message field extraction/assembly 由 Client Library binding 完成。

## 7. RuntimeMode、Naming 与 QoS

### 7.1 RuntimeMode

```cpp
enum class RuntimeMode
{
    DDS,
    ROS2
};
```

RuntimeMode 属于 Context，不允许 endpoint override。

logical FQN `/a/b` 的基础 resolved naming：

| RuntimeMode | Topic | Service request | Service response |
| --- | --- | --- | --- |
| `DDS` | `a/b` | `a/b_Request` | `a/b_Reply` |
| `ROS2` | `rt/a/b` | `rq/a/bRequest` | `rr/a/bReply` |

Public observer 始终返回 normalized logical name，不返回 resolved DDS transport name。

### 7.2 基础 QoS policy

V1 public QoS 支持：

- History / Depth；
- Reliability；
- Durability；
- Deadline；
- Lifespan；
- Liveliness；
- Liveliness lease duration。

`QosDuration` 使用 `SystemDefault / Infinite / Finite`，不使用 magic duration sentinel。

`keep_last(depth)` 要求 `depth > 0`；`KeepAll` canonical depth 为 0。

### 7.3 公共 ROS 2 profiles

ROS 2 preset 数值只有 DMW 一个 authority。V1 提供：

```cpp
static Qos ros2_default();
static Qos ros2_services_default();
static Qos ros2_sensor_data();
static Qos ros2_parameters();
static Qos ros2_parameter_events();
static Qos ros2_action_status_default();
```

profile 固定为：

| Profile | History | Depth | Reliability | Durability |
| --- | --- | ---: | --- | --- |
| `ros2_default` | KeepLast | 10 | Reliable | Volatile |
| `ros2_services_default` | KeepLast | 10 | Reliable | Volatile |
| `ros2_sensor_data` | KeepLast | 5 | BestEffort | Volatile |
| `ros2_parameters` | KeepLast | 1000 | Reliable | Volatile |
| `ros2_parameter_events` | KeepLast | 1000 | Reliable | Volatile |
| `ros2_action_status_default` | KeepLast | 1 | Reliable | TransientLocal |

其余 Deadline/Lifespan/Liveliness/Lease 使用 ROS 2 对应 profile 的 default/system-default 语义。

`dclcpp::SensorDataQoS`、`dclcpp::ServicesQoS`、Python convenience profile 等只能包装这些 DMW profile，不得重新定义数值。

### 7.4 SystemDefault

`Qos::system_default()` 不等于 `ros2_default()`。

`SystemDefault` 最终解析由：

```text
Context RuntimeMode
+
Entity kind
+
DMW validated implementation baseline
```

决定。

V1 不把 XML mutable defaults、环境变量或 Fast DDS factory mutable defaults隐式变成 DMW public contract。

### 7.5 不在 V1 中引入 BestAvailable

ROS 2 Jazzy RMW 存在 BestAvailable policy，但它依赖 discovery-time endpoint 情况，并可能造成创建时 race。DMW V1 不将其加入 public QoS；需要时在后续 actual-QoS / compatibility API 设计中单独评审。

## 8. Topic：Publisher / Subscriber

### 8.1 Publisher

现有 public API 保持：

```cpp
Result<void> write(const void* message);
std::string_view topic_name() const noexcept;
const MessageType& message_type() const noexcept;
Result<std::size_t> matched_subscriber_count() const;
Result<std::unique_ptr<Event>> create_event(EventType type);
```

### 8.2 Subscriber

```cpp
Result<bool> read(void* message, MessageInfo& info);
std::string_view topic_name() const noexcept;
const MessageType& message_type() const noexcept;
Result<std::size_t> matched_publisher_count() const;
Result<std::unique_ptr<Event>> create_event(EventType type);
```

`success + false` 表示本次有限扫描没有取得 public sample，不是 Error。

### 8.3 MessageInfo

`MessageInfo` 标准化至少包含：

```text
writer_gid
writer/source timestamp
reader/reception timestamp
writer/sample sequence（可用时）
```

不可获得的 metadata 使用文档约定的 unknown value，不使用本地 `steady_clock` 伪造 DDS reception timestamp。

### 8.4 matched count

matched count 是 query-time compatible DDS match snapshot，不是 ROS Graph count。

至少满足：

```text
same domain
same resolved topic
same wire type
compatible QoS
discovery completed
```

才计入 count。

## 9. Service：Client / Server

### 9.1 组成

```text
Client
├── request DataWriter
└── response DataReader

Server
├── request DataReader
└── response DataWriter
```

DMW 对上层暴露整体 Client/Server，不允许 Client Library 重新拼装四个 endpoint。

### 9.2 RequestId

```cpp
struct RequestId
{
    Gid client_gid{};
    std::int64_t sequence_number{0};
};
```

`client_gid` 是 runtime-mode-neutral correlation identity；在 ROS2 service mapping 中通常表示 Client response reader identity。

### 9.3 Client

```cpp
Result<RequestId> write_request(const void* request);
Result<bool> read_response(void* response, RequestId& request_id);
Result<bool> service_is_available() const;
Result<bool> wait_for_service(WaitTimeout timeout) const;
```

DMW 不维护 Future/promise table。

`read_response()` 只保证：

- response 属于当前 Client identity；
- RequestId 正确标准化。

RequestId 是否仍存在于上层 pending Future registry 属于 `dclcpp` / `dclpy`。

### 9.4 Server pending request FSM

`ServerOptions::max_pending_requests` 必须大于 0。

```text
read_request success
      ↓
Pending
      ↓ write_response begins
Responding
      ├── success -> remove
      └── failure -> Pending
```

规则：

- unknown / already responded RequestId -> `NotFound`；
- concurrent response to same Responding request -> `Busy`；
- pending capacity 满时必须在 middleware take 前返回 `ResourceExhausted`；
- Pending/Responding 生命周期内 suppress duplicate RequestId；
- 不维护无限 responded tombstone。

### 9.5 Service availability

`service_is_available()` 的 authority 位于 DMW internal DiscoveryGraph。

禁止简单使用：

```text
request matched > 0
AND
response matched > 0
```

因为两侧可能来自不同 remote participant。

DMW 必须按 remote participant identity 配对：

```text
same remote participant
    has request DataReader
AND has response DataWriter
```

才形成一个 available server candidate。

`wait_for_service()`：

- 与同一 availability authority 绑定；
- 由 discovery revision / notification 唤醒；
- 不使用固定 sleep polling；
- timeout 返回 `success + false`；
- Context shutdown 返回 `ContextShutdown`。

## 10. Timer Common Runtime

### 10.1 设计目标

Timer 参考 ROS 2 Jazzy `rcl_timer_t` 的职责边界：

```text
DMW Timer
    ├── period
    ├── next deadline
    ├── ready/canceled state
    ├── reset/cancel
    ├── consume scheduling state
    └── WaitSet integration

Client Library
    └── callback / executor dispatch
```

DMW Timer **不得保存或执行 user callback**，不得为每个 Timer 创建 callback thread。

### 10.2 TimerOptions

```cpp
struct TimerOptions
{
    std::chrono::nanoseconds period{0};
    bool autostart{true};
};
```

规则：

- `period < 0` -> `InvalidArgument`；
- `period == 0` 合法，表示 always-ready timer；
- `autostart == false` 创建后为 canceled；
- Timer 使用 monotonic clock；V1 固定为 `std::chrono::steady_clock` 或等价平台 monotonic source。

### 10.3 TimerInfo

```cpp
struct TimerInfo
{
    std::chrono::steady_clock::time_point expected_call_time;
    std::chrono::steady_clock::time_point actual_call_time;
    std::chrono::nanoseconds elapsed_since_last_call{0};
};
```

这些 time point 只用于本进程 scheduling/diagnostic，不是 ROS message timestamp，不用于跨进程协议。

### 10.4 Timer API

```cpp
class Timer
{
public:
    ~Timer() noexcept;

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    std::chrono::nanoseconds period() const noexcept;

    Result<bool> is_canceled() const;
    Result<bool> is_ready() const;

    Result<void> cancel();
    Result<void> reset();

    Result<std::chrono::nanoseconds>
    exchange_period(std::chrono::nanoseconds new_period);

    Result<std::chrono::nanoseconds>
    time_until_next_call() const;

    Result<bool> consume(TimerInfo& info);
};
```

### 10.5 Timer readiness

Timer ready iff：

```text
Context Active
AND !canceled
AND now >= next_call_time
```

`is_ready()` 不消费 readiness。

Canceled timer：

- `is_ready()` -> `success + false`；
- `consume()` -> `success + false`；
- `time_until_next_call()` -> `InvalidState`。

### 10.6 reset

`reset()`：

```text
canceled = false
last_call_time = now
next_call_time = now + period
```

period 0 时 `next_call_time = now`，因此立即 ready。

### 10.7 cancel

`cancel()` 是幂等状态更新：

- canceled 设为 true；
- 触发注册 WaitSet 的 topology/control wake，使 active wait 重新计算 earliest Timer deadline；
- 不执行 callback。

### 10.8 exchange_period

`exchange_period(new_period)`：

- `new_period < 0` -> `InvalidArgument`；
- 返回旧 period；
- 不隐式 reset 当前 next deadline；
- 下一次 successful `consume()` 使用新 period 推进 schedule；
- 如需从“当前时间 + 新周期”重新开始，调用者应随后调用 `reset()`。

该语义与“period 与 next deadline 是两个独立 scheduling state”保持一致，避免 period 更新隐式改变当前 deadline。

### 10.9 consume 与 missed-period 规则

`consume(info)`：

1. canceled / not ready -> `success + false`，info unchanged；
2. ready -> snapshot current expected deadline 与 actual `now`；
3. 更新 last call；
4. `next_call_time += period`；
5. 如果新 next deadline 仍 `<= now`，一次性跳过已经错过的完整周期，使 next deadline 成为第一个严格晚于 `now` 的周期点；
6. period 0 时 next deadline 设为当前 now，因此持续 ready；
7. commit TimerInfo 并返回 true。

因此 DMW 不把 callback execution latency 累加到每个周期，也不会为已经错过的每个历史周期补发多个 callback readiness。

### 10.10 Timer 与 WaitSet

Timer 是 level-triggered waitable：

- 到期后，在 successful `consume()` 前持续 ready；
- WaitSet 返回 Timer token 本身不消费 deadline；
- reset/cancel/period update 必须唤醒 active WaitSet 重算 deadline；
- WaitSet 计算 native wait timeout 时使用 caller deadline 与 earliest Timer deadline 的最小值。

## 11. Action Common Runtime

### 11.1 架构边界

Action 底层由：

```text
SendGoal Service
CancelGoal Service
GetResult Service
Feedback Topic
Status Topic
```

组成。

DMW Action runtime 负责：

- 五个 endpoint 的事务式创建与销毁；
- Action logical naming；
- Action availability；
- aggregate readiness；
- GoalId / GoalInfo；
- Goal FSM；
- cancel candidate selection；
- terminal goal lifetime；
- pending GetResult RequestId state；
- status snapshot authority；
- ROS 2 Action endpoint QoS 与 wire mapping；
- Context shutdown / teardown。

DMW **不负责**：

- typed Goal/Result/Feedback C++ class；
- Python Action class；
- user goal/cancel/feedback/result callback；
- std::future / Python Future；
- typed terminal result payload copy/cache；
- Executor scheduling。

最后一项需要特别区分：DMW 维护“哪个 Goal 已 terminal、哪些 GetResult request 正在等待、何时过期”的公共状态；但任意 ActionT 的 result 对象如何复制和长期保存属于 typed Client Library。DMW 不为了缓存任意 `void*` 而引入通用 reflection/serialization framework。

### 11.2 Action endpoint logical names

给定 normalized Action FQN：

```text
/<action>
```

固定派生：

```text
/<action>/_action/send_goal
/<action>/_action/cancel_goal
/<action>/_action/get_result
/<action>/_action/feedback
/<action>/_action/status
```

前三项继续经过 Service RuntimeMode naming；后两项继续经过 Topic RuntimeMode naming。

因此 ROS2 模式最终自动得到 ROS 2-compatible `rq/rr/rt` DDS names；Client Library 不重复拼接 DDS prefix。

### 11.3 GoalId

```cpp
struct GoalId
{
    static constexpr std::size_t Size = 16;
    std::array<std::uint8_t, Size> data{};
};
```

必须提供 equality/hash。

DMW 不要求 GoalId 由 DMW 生成。dclcpp/dclpy 可以使用各自 UUID convenience；进入 DMW 后统一转换为 `GoalId`。

### 11.4 GoalInfo

```cpp
struct GoalInfo
{
    GoalId goal_id{};
    std::chrono::nanoseconds accepted_stamp{0};
};
```

`accepted_stamp` 表示 Action protocol time domain 中的接受时间，用于 cancel-before semantics。V1 DMW 不引入完整 ROS Clock/sim-time runtime，因此由 Client Library/clock binding 在 accept 时提供；DMW 只保存并按值比较。

该时间与 Timer 的 steady-clock deadline 完全分离。

### 11.5 GoalState / GoalEvent / GoalAcceptMode

```cpp
enum class GoalState
{
    Unknown,
    Accepted,
    Executing,
    Canceling,
    Succeeded,
    Canceled,
    Aborted
};

enum class GoalEvent
{
    Execute,
    CancelGoal,
    Succeed,
    Abort,
    Canceled
};

enum class GoalAcceptMode
{
    Defer,
    Execute
};
```

状态转换表固定为：

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

其它 transition -> `InvalidState`，state 不变。

Terminal states：

```text
Succeeded
Canceled
Aborted
```

`GoalAcceptMode::Defer` 表示 accepted 后保持 `Accepted`；`GoalAcceptMode::Execute` 表示 accepted transaction commit 后立即执行 `Accepted -> Executing`。

### 11.6 Goal registry authority

每个 ActionServer 唯一维护：

```text
GoalId
 -> GoalInfo
 -> GoalState
 -> terminal timestamp（若 terminal）
 -> pending GetResult RequestIds
```

同 GoalId 重复 accept -> `AlreadyExists`。

`dclcpp` / `dclpy` 不建立第二套 Goal FSM。

### 11.7 ActionClientOptions

```cpp
struct ActionClientOptions
{
    Qos goal_service_qos{Qos::ros2_services_default()};
    Qos cancel_service_qos{Qos::ros2_services_default()};
    Qos result_service_qos{Qos::ros2_services_default()};
    Qos feedback_topic_qos{Qos::ros2_default()};
    Qos status_topic_qos{Qos::ros2_action_status_default()};
};
```

### 11.8 ActionServerOptions

```cpp
struct ActionServerOptions
{
    Qos goal_service_qos{Qos::ros2_services_default()};
    Qos cancel_service_qos{Qos::ros2_services_default()};
    Qos result_service_qos{Qos::ros2_services_default()};
    Qos feedback_topic_qos{Qos::ros2_default()};
    Qos status_topic_qos{Qos::ros2_action_status_default()};

    std::chrono::nanoseconds result_timeout{
        std::chrono::seconds(10)};
};
```

`result_timeout < 0` -> `InvalidArgument`。

10 s 与 ROS 2 Jazzy `rcl_action` default 对齐，但数值 authority 位于 DMW options/default，不在 dclcpp/dclpy 重复定义。

### 11.9 ActionClient public API

DMW ActionClient 是一个 aggregate primitive，不向上暴露内部三个 `Client` 与两个 `Subscriber`。

```cpp
class ActionClient
{
public:
    ~ActionClient() noexcept;

    Result<RequestId> write_goal_request(const void* request);
    Result<bool> read_goal_response(void* response, RequestId& request_id);

    Result<RequestId> write_cancel_request(const void* request);
    Result<bool> read_cancel_response(void* response, RequestId& request_id);

    Result<RequestId> write_result_request(const void* request);
    Result<bool> read_result_response(void* response, RequestId& request_id);

    Result<bool> read_feedback(void* feedback, MessageInfo& info);
    Result<bool> read_status(void* status, MessageInfo& info);

    Result<bool> server_is_available() const;
    Result<bool> wait_for_server(WaitTimeout timeout) const;

    Result<ActionClientReadySet> readiness() const;

    std::string_view action_name() const noexcept;
};
```

RequestId 与 Future 的映射留在 Client Library：

```text
DMW RequestId
    ↓
dclcpp pending promise table
or
dclpy pending Future table
```

### 11.10 ActionClientReadySet

```cpp
struct ActionClientReadySet
{
    bool goal_response{false};
    bool cancel_response{false};
    bool result_response{false};
    bool feedback{false};
    bool status{false};

    bool any() const noexcept;
};
```

WaitSet 对一个 ActionClient 只返回一个 registration token；调用者通过 `readiness()` 决定具体 ready sub-channel。

### 11.11 Action availability

Action server available 要求同一 remote participant 下存在兼容的：

```text
SendGoal Server
CancelGoal Server
GetResult Server
Feedback Publisher
Status Publisher
```

DMW 使用 internal DiscoveryGraph 组合判断。

这比单纯把五个独立 count 做 AND 更严格，避免不同 participant 的 endpoint 被错误拼成一个 ActionServer。

`wait_for_server()` 与 Service wait 一样依赖 discovery revision，不固定 sleep polling。

### 11.12 ActionServer public transport API

```cpp
class ActionServer
{
public:
    ~ActionServer() noexcept;

    Result<bool> read_goal_request(void* request, RequestId& request_id);

    // Raw response path is for rejected goals only. Accepted responses must
    // use accept_goal() so the transport response and Goal registry commit
    // form one DMW transaction.
    Result<void> write_goal_response(
        const RequestId& request_id,
        const void* rejected_response);

    Result<bool> read_cancel_request(void* request, RequestId& request_id);
    Result<void> write_cancel_response(
        const RequestId& request_id,
        const void* response);

    Result<bool> read_result_request(void* request, RequestId& request_id);
    Result<void> write_result_response(
        const RequestId& request_id,
        const void* response);

    Result<void> publish_feedback(const void* feedback);
    Result<void> publish_status(const void* status);

    Result<ActionServerReadySet> readiness() const;

    std::string_view action_name() const noexcept;

    Result<GoalTransition> accept_goal(
        const RequestId& request_id,
        const GoalInfo& goal_info,
        const void* accepted_response,
        GoalAcceptMode mode);

    Result<GoalState> goal_state(const GoalId& goal_id) const;
    Result<GoalTransition> update_goal_state(
        const GoalId& goal_id,
        GoalEvent event);

    Result<CancelSelection> select_cancel_goals(
        const CancelGoalCriteria& criteria) const;

    Result<ResultRequestDisposition>
    register_result_request(
        const GoalId& goal_id,
        const RequestId& request_id);

    Result<std::vector<RequestId>>
    take_pending_result_requests(const GoalId& goal_id);

    Result<std::vector<GoalStatusInfo>>
    status_snapshot() const;

    Result<std::vector<GoalId>>
    take_expired_goals();
};
```

传输 message 与 Goal common metadata 分离是刻意设计：DMW 不解析任意 action-specific C++ object layout。

`write_goal_response()` 的 `rejected_response` 必须表示 rejected/non-accepted SendGoal response。由于 DMW 不解析 action-specific typed response layout，这一点是 Client Library/type-adapter 的 programming contract。任何 accepted response 都必须走 `accept_goal()`。

### 11.13 Goal accept transaction boundary

标准 server flow：

```text
read_goal_request(raw request, RequestId)
        ↓
Client Library 从 typed request 提取 GoalInfo
        ↓
user goal callback
        ├── reject
        │     ↓
        │  write_goal_response(request_id, rejected_response)
        │
        └── accept/defer-or-execute
              ↓
          accept_goal(
              request_id,
              goal_info,
              accepted_response,
              GoalAcceptMode)
```

`accept_goal()` 是 **DMW public transaction boundary**，不得由 dclcpp/dclpy 拆成“先写 accepted response，再调用另一个 Goal registry API”。事务语义固定为：

1. 参数、Context、ActionServer 和 `RequestId` state 校验；
2. 验证 GoalId 尚未存在；
3. 在 ActionServer state 同步域内预留 GoalId，并预先分配/准备 GoalRecord、status/expiry bookkeeping 等成功写响应后 commit 所需的全部本地资源；
4. 写 accepted SendGoal response；
5. response write 失败：撤销 Goal reservation，不产生 public GoalRecord，不改变 Goal FSM；底层 SendGoal request 的 retryability 按 Service response failure contract 处理；
6. response write 成功：以 **no-fail / no-allocation local commit** 把预留 GoalRecord 发布为 `Accepted`；
7. `GoalAcceptMode::Execute` 在同一 Action state transaction 中继续执行 `Accepted -> Executing`；`Defer` 保持 `Accepted`；
8. 返回最终 `GoalTransition`。

关键不变量：

```text
accepted response observable on wire
    =>
corresponding GoalRecord is committed locally
```

以及：

```text
accept_goal() returns failure before successful response write
    =>
no public GoalRecord remains
```

response 成功写出以后不得再执行可能失败的 heap allocation、registry insertion 或其它会使本地 commit 失败的步骤；这些资源必须在第 3 步完成 reservation/preparation。这样 DMW 不会形成“client 已观察 accepted，但本地没有 Goal”的 half-accepted state。

上层 user callback 只决定 Reject / AcceptAndDefer / AcceptAndExecute policy；accepted transport commit 与 Goal state commit 只有 DMW 一个 authority。

### 11.14 CancelGoalCriteria

```cpp
struct CancelGoalCriteria
{
    GoalId goal_id{};                  // all-zero = wildcard
    std::chrono::nanoseconds stamp{0}; // zero = no time bound for exact-id form
};
```

DMW selection 规则与 ROS 2 Action cancel semantics 对齐：

```text
GoalId != 0, stamp == 0
    -> exact goal

GoalId == 0, stamp == 0
    -> all cancelable goals

GoalId == 0, stamp != 0
    -> all cancelable goals accepted at/before stamp

GoalId != 0, stamp != 0
    -> exact goal OR all cancelable goals accepted at/before stamp
```

只选择当前 `Accepted` / `Executing` 状态的 cancelable goal。

`select_cancel_goals()` 不执行 user cancel callback，也不自动改变 state。

用户接受某个 candidate 后，Client Library 调用：

```text
update_goal_state(goal_id, GoalEvent::CancelGoal)
```

由 DMW 完成 `Accepted/Executing -> Canceling`。

### 11.15 ResultRequestDisposition

```cpp
enum class ResultRequestDisposition
{
    UnknownGoal,
    Pending,
    Terminal
};
```

`register_result_request(goal_id, request_id)`：

- goal 不存在 -> `UnknownGoal`，不保存 request；
- goal active -> 保存 RequestId，返回 `Pending`；
- goal terminal -> `Terminal`，不重复保存；

终止 transition 后 `take_pending_result_requests(goal_id)` 把此前等待该 goal 的 RequestId 一次性交给上层，以便用 typed result payload 回应。

### 11.16 GoalTransition

```cpp
struct GoalTransition
{
    GoalState previous{GoalState::Unknown};
    GoalState current{GoalState::Unknown};
    bool became_terminal{false};
};
```

进入 terminal state 时记录 terminal steady-clock timestamp，用于 result expiry scheduling。注意这与 `GoalInfo.accepted_stamp` 的 Action protocol clock domain 不同。

### 11.17 terminal result payload 与 result timeout

DMW 保存：

```text
terminal Goal state
terminal time
pending GetResult RequestIds
expiry deadline
```

Client Library 保存：

```text
GoalId -> typed ActionT::Result payload
```

当 Goal terminal：

1. Client Library 保存 typed result；
2. DMW 返回 pending result RequestIds；
3. Client Library 为这些 RequestId 构造 response 并通过 ActionServer write；
4. 新 GetResult request 到达 terminal goal 时，DMW 返回 `Terminal`，Client Library 从 typed cache 构造 response；
5. `result_timeout` 到期后 DMW 报告 expired GoalId；Client Library 删除对应 typed cache。

这样公共 Goal/result lifecycle 只在 DMW 一处，而 DMW 不需要通用对象 clone/serialization 框架。

### 11.18 status common state

```cpp
struct GoalStatusInfo
{
    GoalInfo goal_info;
    GoalState state{GoalState::Unknown};
};
```

`status_snapshot()` 返回当前未 expired Goal 的一致 snapshot。

DMW 是 GoalState authority；Client Library 只把 snapshot 装配为实际 status message，然后调用 `publish_status()`。

状态改变后上层应发布 status；Executor/API wrapper 可以自动执行这一步，但不能复制 Goal FSM。

### 11.19 ActionServerReadySet

```cpp
struct ActionServerReadySet
{
    bool goal_request{false};
    bool cancel_request{false};
    bool result_request{false};
    bool goal_expired{false};

    bool any() const noexcept;
};
```

ActionServer WaitSet registration 同样只产生一个 token。

Goal expiry readiness 来自 ActionServer 内部 result-expiry deadline，不创建 user callback thread。

## 12. Graph Snapshot / Change

### 12.1 设计目标

DMW 已经必须维护 discovery authority 用于：

- matched state；
- Service availability；
- Action availability。

因此 Graph snapshot/change 不应由 `dclcpp` 和 `dclpy` 各自再建立 cache。

V1 只公开 DDS discovery 可以可靠支撑的 graph 信息，不宣称完整 ROS Node Graph compatibility。

### 12.2 GraphRevision

```cpp
using GraphRevision = std::uint64_t;
```

Context 内每次 **可观察 graph state 实际变化** 后 revision 单调递增。

重复/无效 discovery callback 不增加 revision。

### 12.3 TopicGraphInfo

```cpp
struct TopicGraphInfo
{
    std::string topic_name;
    std::vector<std::string> wire_types;
    std::size_t publisher_count{0};
    std::size_t subscriber_count{0};
};
```

name 是 normalized logical DMW name；不向普通 public API 暴露 DDS `rt/` transport name。

### 12.4 ServiceGraphInfo

```cpp
struct ServiceGraphInfo
{
    std::string service_name;
    std::vector<std::string> request_wire_types;
    std::vector<std::string> response_wire_types;
    std::size_t client_count{0};
    std::size_t server_candidate_count{0};
};
```

`server_candidate_count` 使用与 `service_is_available()` 相同的 participant-paired authority。

### 12.5 ActionGraphInfo

```cpp
struct ActionGraphInfo
{
    std::string action_name;
    std::size_t client_candidate_count{0};
    std::size_t server_candidate_count{0};
};
```

只有 DMW 能从五个 endpoint 可靠识别为同一 Action composition 时才纳入 snapshot。

### 12.6 GraphSnapshot

```cpp
struct GraphSnapshot
{
    GraphRevision revision{0};
    std::vector<TopicGraphInfo> topics;
    std::vector<ServiceGraphInfo> services;
    std::vector<ActionGraphInfo> actions;
};
```

V1 不提供 `NodeGraphInfo`，原因是普通 DDS participant/endpoint discovery 无法在不引入 ROS graph discovery metadata 协议的情况下可靠恢复 ROS logical node name/namespace。

未来实现 ROS Graph metadata 时可以扩展新的 API，但不得用 participant name 猜测 ROS Node identity。

### 12.7 GraphEvent

```cpp
struct GraphEventOptions {};

struct GraphChangeInfo
{
    GraphRevision previous_revision{0};
    GraphRevision current_revision{0};
};

class GraphEvent
{
public:
    ~GraphEvent() noexcept;

    Result<bool> take(GraphChangeInfo& info);
};
```

GraphEvent 创建时 cursor 初始化为当前 revision，不 replay 之前的 discovery history。

GraphEvent readiness：

```text
current graph revision > event cursor
```

WaitSet 报告 GraphEvent ready 不推进 cursor；只有 successful `take()` 推进 cursor。

因此 GraphEvent 是 level-triggered，与 Event 一致，不是计数 semaphore。

## 13. WaitSet / GuardCondition / Event

### 13.1 Waitable kinds

V1 完整 WaitSet 支持：

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

Publisher 不直接 waitable。

### 13.2 WaitableKind

```cpp
enum class WaitableKind
{
    Subscriber,
    Client,
    Server,
    Timer,
    ActionClient,
    ActionServer,
    Event,
    GraphEvent,
    GuardCondition
};
```

### 13.3 WaitSet API

```cpp
Result<WaitableRegistration> add(Subscriber&);
Result<WaitableRegistration> add(Client&);
Result<WaitableRegistration> add(Server&);
Result<WaitableRegistration> add(Timer&);
Result<WaitableRegistration> add(ActionClient&);
Result<WaitableRegistration> add(ActionServer&);
Result<WaitableRegistration> add(Event&);
Result<WaitableRegistration> add(GraphEvent&);
Result<WaitableRegistration> add(GuardCondition&);

Result<void> remove(WaitableRegistration);
Result<WaitResult> wait(WaitTimeout timeout);
```

### 13.4 WaitTimeout

WaitTimeout 只允许：

```text
Poll
Finite (> 0)
Infinite
```

不使用 `-1` 或 `nanoseconds::max()` sentinel。

Finite wait deadline 在进入 wait 时计算一次：

```text
deadline = steady_clock::now() + timeout
```

add/remove/topology/graph/timer/control wake 均不得重新开始完整 timeout。

### 13.5 Timer-aware wait deadline

有 Timer/Action expiry 时：

```text
native_deadline = min(
    caller finite deadline if any,
    earliest ready-producing timer deadline,
    earliest action result-expiry deadline)
```

native wait 返回后统一 re-evaluate logical readiness。

Infinite user wait 也会在最早 Timer/Action deadline 到达时返回 Ready。

### 13.6 Registration ownership

规则保持：

- 一个 waitable 同一时刻最多属于一个 WaitSet；
- 第二次 add -> `AlreadyRegistered`；
- WaitSet 不拥有 waitable；
- waitable destructor 自动 detach；
- Registration token 绑定创建它的 WaitSet；
- stale token -> `NotRegistered`；
- wrong WaitSet token -> `InvalidArgument`；
- 同一 WaitSet 同时最多一个 active `wait()`；第二个 -> `Busy`；
- add/remove 可以与 active wait 并发；
- WaitSet 自身 destructor 不允许与其 active wait 无同步并发。

### 13.7 WaitResult

WaitResult 是 readiness snapshot：

```text
Ready   -> ready() 非空
Timeout -> ready() 为空
```

remove/destroy 可以在 snapshot 形成后使 token stale；WaitResult 不持有 public entity pointer。

Client Library Executor 必须维护 token -> weak/high-level entity 映射并处理 stale token。

### 13.8 GuardCondition

GuardCondition 使用 coalesced pending-trigger semantics：

- trigger before registration 可观察；
- 多次 trigger 在消费前可合并为一次 readiness；
- WaitSet 报告 ready 时消费该次 logical trigger；
- concurrent trigger/consume 不得丢新 trigger。

Fast DDS/native wake 是 notification mechanism；实现必须先确保 logical state 与 native trigger 的提交顺序不会产生 lost wakeup。若 native GuardCondition trigger API 返回失败，`trigger()` 返回 `DDSError`，本次 trigger 不得伪装成 success。DMW 不使用固定周期 polling 来掩盖有效 Fast DDS GuardCondition 的错误。

### 13.9 Event

Event 仍是 endpoint-bound persistent entity。

EventType：

```text
Subscriber:
    LivelinessChanged
    RequestedDeadlineMissed
    RequestedIncompatibleQos
    MessageLost

Publisher:
    LivelinessLost
    OfferedDeadlineMissed
    OfferedIncompatibleQos
```

Event：

- Factory commit 前 history 不 replay；
- 每个 Event 有独立 cursor；
- WaitSet ready 不消费；
- successful `Event::take()` 消费；
- parent destroyed -> `ParentDestroyed`；
- Context shutdown 优先。

## 14. Discovery 与 Registry

### 14.1 TypeRegistry

每个 Context 必须有 TypeRegistry。

Key：

```text
DDS wire type name
```

Entry 至少保存：

```text
BindingIdentity
Fast DDS TypeSupport backing
reference count / phase
```

### 14.2 TopicRegistry

每个 Context 必须有 TopicRegistry。

同 resolved DDS topic name 不允许绑定不同 wire type。

### 14.3 DiscoveryGraph

Context 内只有一个 discovery authority，服务：

```text
matched endpoint state
Service availability
Action availability
GraphSnapshot
GraphEvent revision
```

禁止：

```text
dclcpp GraphCache
+
dclpy GraphCache
+
DMW DiscoveryGraph
```

形成三个互相可能不同步的 authority。

### 14.4 Eventual consistency

DDS discovery 是异步的。因此：

- endpoint create/destroy 后 graph snapshot 可以短暂观察旧 state；
- service/action availability 可以短暂观察旧 snapshot；
- 但单次 snapshot 内部必须自洽；
- 不允许把不同 remote participant endpoint 拼接成 false-positive service/action candidate。

## 15. 并发与生命周期

### 15.1 Fast DDS listener boundary

Fast DDS listener 只允许：

```text
update synchronized runtime state
update discovery graph
update matched/event state
mark readiness
trigger native/internal condition
record diagnostic
```

不得：

```text
invoke user callback
invoke Python
fulfill std::promise directly as user scheduling mechanism
run Executor callback
```

### 15.2 public operation concurrency

以下 operation 设计为可以从不同应用线程调用：

```text
Publisher::write
Subscriber::read
matched count
Client::write_request/read_response
Client::service_is_available/wait_for_service
Server::read_request
Server::write_response (different RequestId)
Timer observers/reset/cancel/consume
ActionClient independent transport operations
ActionServer Goal operations under internal synchronization
GuardCondition::trigger
Event::take
GraphEvent::take
Context graph snapshot/query
```

同一 public facade 的 destructor 与普通 operation 无同步并发仍由调用者禁止，WaitSet 注册实体销毁是专门例外。

### 15.3 Timer concurrency

Timer state 更新具有单一同步域。

并发 `consume()`：同一 deadline 最多一个调用返回 true；其它调用重新检查 state。

`reset/cancel/exchange_period` 与 `consume` 线性化，不允许 torn scheduling state。

### 15.4 Action concurrency

ActionServer Goal registry / pending result table / expiry state 使用同一逻辑同步域。

同 GoalId 的并发 transition：只有第一个合法 transition 成功；后续按新 state 验证。

`accept_goal()` 与同一 ActionServer 的其它 Goal-registry / SendGoal-response operation 必须在线性化的 Action state transaction 中执行；Goal reservation 对其它线程不可见为 accepted Goal，直到 response write 成功并完成 no-fail commit。

status snapshot 必须来自一致 state snapshot。

### 15.5 shutdown concurrency

允许 Context shutdown 与 publish/take/service/timer/action/graph/wait 并发。

每个 operation：

- 要么在线性化点前完成；
- 要么观察 Context 非 Active 并返回 `ContextShutdown`；
- 不得 UAF/deadlock/访问 half-destroyed middleware entity。

## 16. ROS 2 Fast DDS Wire Contract

### 16.1 主要参考与兼容验证

DMW 新设计参考：

```text
Fast DDS 2.14.x
rmw_fastrtps Jazzy
```

二者地位平等。

`rcl` / `rcl_action` Jazzy 用于 common-runtime 行为对照；`rclcpp` / `rclpy` 用于上层职责边界对照。

兼容性测试至少覆盖：

```text
Jazzy / Fast DDS 2.14.x
Humble / Fast DDS 2.6.x
```

Humble 兼容性不反向限制 public API 设计；仅在实现层使用必要的兼容 shim。

### 16.2 ROS Topic naming

logical FQN：

```text
/foo
```

DDS topic：

```text
rt/foo
```

### 16.3 ROS Service naming

Service `/add_two_ints`：

```text
request: rq/add_two_intsRequest
reply:   rr/add_two_intsReply
```

### 16.4 Service correlation

ROS2 mode 使用 Fast DDS：

```text
SampleIdentity
related_sample_identity
```

完成 request/response correlation。

Client request 写入前把 response reader GUID 放入 request related identity；Server take 后标准化为 `RequestId`；response 使用 RequestId 作为 related identity；Client 根据自己的 endpoint identity 过滤 response。

### 16.5 Response-reader match workaround

当 response target identity 表示 Client response reader 时，Server response writer 必须等待对应 reader matched 或确认目标已消失。

该等待时长不是 DMW public hard-coded constant。

Fast DDS 实现应参考 `rmw_fastrtps` Jazzy：从 effective response-writer reliability QoS 的 `max_blocking_time` 派生 absolute steady deadline。Humble/2.6.x path 必须通过 interoperability regression 验证。

因此 public contract 只定义：

```text
matched -> write
confirmed gone -> success without write
deadline expires -> Timeout
Context shutdown -> ContextShutdown
```

### 16.6 ROS Action naming

Action logical FQN `/move` 派生：

```text
/move/_action/send_goal
/move/_action/cancel_goal
/move/_action/get_result
/move/_action/feedback
/move/_action/status
```

这些 logical names 再通过现有 Service/Topic ROS2 RuntimeMode mapping 进入 `rq/rr/rt` DDS names。

### 16.7 ROS Action QoS

默认：

```text
SendGoal service -> ros2_services_default
CancelGoal service -> ros2_services_default
GetResult service -> ros2_services_default
Feedback topic -> ros2_default
Status topic -> ros2_action_status_default
```

### 16.8 ROS Action interoperability

必须验证：

```text
DMW ActionClient -> ROS 2 ActionServer
ROS 2 ActionClient -> DMW ActionServer
Jazzy DMW ActionClient/Server
Humble DMW ActionClient/Server compatibility path
cross-version wire probes where supported
```

至少覆盖：

- 5 endpoint naming/type/QoS；
- goal accept/reject；
- execute/succeed/abort/cancel；
- cancel selection semantics；
- feedback；
- status；
- result-before-terminal deferred response；
- terminal result response；
- result expiry；
- multiple clients/goals；
- Action availability。

### 16.9 Wire compatibility != ROS Graph compatibility

V1 保证的是：

```text
Topic / Service / Action data-plane wire interoperability
```

GraphSnapshot 是 DMW DDS discovery view，不等于完整 ROS graph protocol。

## 17. 实现边界

### 17.1 targets

Runtime target：

```text
dmw::dmw
```

Fast DDS type binding：

```text
dmw::fastdds_binding
```

Timer/Graph/Action common runtime 均属于 `dmw::dmw`，不新增 `rcl`、`runtime_core`、`action_runtime` 独立 library。

### 17.2 public headers

普通 public header 不出现：

```text
eprosima::fastdds::*
eprosima::fastcdr::*
```

Fast DDS TypeSupport integration 只存在于明确的 `dmw/fastdds/*` boundary。

### 17.3 推荐新增文件

实现 Timer/Graph/Action 时，建议保持与现有 public object 一致的扁平组织：

```text
include/dmw/
├── timer.hpp
├── timer_info.hpp
├── action_type.hpp
├── action_client.hpp
├── action_server.hpp
├── action_common.hpp
├── graph.hpp
└── graph_event.hpp

src/
├── timer.cpp
├── action_client.cpp
├── action_server.cpp
├── graph_event.cpp
└── impl/
    ├── timer_impl.*
    ├── action_client_impl.*
    ├── action_server_impl.*
    └── discovery_graph.*
```

不新增没有独立职责的顶层 package 或 framework layer。

## 18. 测试与验收

### 18.1 Foundation regression

必须继续覆盖：

- Result/Error；
- Context create/shutdown/concurrent shutdown；
- MessageType/TypeRegistry；
- Topic pub/sub、matched count、QoS；
- Service request/response/multi-client；
- Service availability pairing/wait；
- WaitSet add/remove/wait/teardown；
- GuardCondition race；
- Event cursor/readiness；
- ASan/UBSan；
- targeted TSan。

### 18.2 QoS profiles

为所有 DMW common profiles 建立 golden tests：

```text
ros2_default
ros2_services_default
ros2_sensor_data
ros2_parameters
ros2_parameter_events
ros2_action_status_default
```

禁止 dclcpp/dclpy 再保存独立 preset 数值。

### 18.3 Timer tests

至少覆盖：

```text
period < 0 -> InvalidArgument
period == 0 always-ready
autostart true/false
ready before/after deadline
consume output unchanged on false
periodic consume
missed cycles skip and re-align
cancel
reset
exchange_period
concurrent consume exactly-once per deadline
Timer + finite WaitSet
Timer + infinite WaitSet
Timer reset/cancel while WaitSet blocking
Timer destruction while registered
Context shutdown
no callback thread
```

### 18.4 Graph tests

```text
snapshot initial
publisher/subscriber add/remove
service candidate pairing
action candidate composition
revision increments only on actual state change
GraphEvent no pre-creation replay
GraphEvent level-triggered until take
concurrent snapshot + discovery update
Context shutdown
GraphEvent destruction while registered
```

### 18.5 Action Goal FSM / accept-transaction tests

完整验证 transition table，包括所有非法 transition。

必须覆盖：

```text
reject response does not create GoalRecord
accept_goal duplicate GoalId -> AlreadyExists
accepted response write failure -> Goal reservation rolled back
accepted response write success -> GoalRecord always committed
no allocation/failable local step after accepted response write
GoalAcceptMode::Defer -> Accepted
GoalAcceptMode::Execute -> Executing
Accepted -> Executing
Accepted -> Canceling
Executing -> Canceling
Executing -> Succeeded/Aborted
Canceling -> Succeeded/Aborted/Canceled
terminal transition rejection
multiple goals
concurrent accept same GoalId -> exactly one transaction succeeds
concurrent transition same GoalId
status snapshot consistency
```

### 18.6 Action cancel tests

四种 GoalId/timestamp selection form 全覆盖，并验证：

- only cancelable goal selected；
- unknown exact id；
- terminal goal；
- all-goal cancel；
- timestamp boundary equal；
- user reject 后 state 不改变；
- accepted cancel -> Canceling。

### 18.7 Action result tests

```text
unknown goal -> UnknownGoal
active goal -> pending RequestId
multiple result requests same goal
terminal transition returns pending requests
new request after terminal -> Terminal
result timeout expiry
expired goal no longer exists
Goal expiry WaitSet readiness
```

Typed result payload cache behavior在 dclcpp/dclpy tests 验证，但 Goal lifecycle/expiry 只在 DMW 测一套 authority。

### 18.8 Aggregate WaitSet tests

ActionClient 一个 registration 必须正确报告五种子通道 readiness；ActionServer 一个 registration 必须正确报告 goal/cancel/result/expiry readiness。

不得要求 Executor 注册内部五/三个 primitive。

### 18.9 ROS interoperability regression

Jazzy/Fast DDS 2.14.x 用于验证当前 peer reference line；Humble/Fast DDS 2.6.x 用于验证持续兼容性。两套环境都属于 DMW 的持续验证矩阵，不用 CI 命名表达上游参考优先级。

分别覆盖 Topic / Service / Action 双向 interoperability。

测试网络路径建议显式使用 UDPv4，避免同机 SHM 把 wire compatibility 问题隐藏。

## 19. Frozen Invariants

以下为 V1 审查索引；若简述与正文冲突，以正文为准并修正文档。

### 19.1 Architecture

1. DMW 使用 C++17。
2. Fast DDS-only。
3. 普通 runtime public API non-template/type-erased。
4. Fast DDS public type 不泄漏到普通 public headers。
5. DMW 不提供 C API、多 middleware plugin 或独立 `rcl` package。
6. Fast DDS 2.14.x 与 `rmw_fastrtps` Jazzy 是平等主要参考基线。
7. `rcl` / `rcl_action` 用于 common-runtime 职责参考；`rclcpp` / `rclpy` 用于语言层边界参考。
8. Humble / Fast DDS 2.6.x 是兼容性验证目标，不决定新的 public design。

### 19.2 Ownership

9. Resource Factory 返回 `Result<std::unique_ptr<T>>`。
10. Resource non-copyable/non-movable。
11. 创建事务化，不返回 half-valid object。
12. Context 是 runtime root。
13. Node 是 logical entity，不等于 Participant。
14. Context facade 可以先于 child facade 析构，内部 backing 保证 teardown safety。

### 19.3 Client-Library boundary

15. Future/Promise 不下沉。
16. pending Future registry 不下沉。
17. user callback 不下沉。
18. Executor/CallbackGroup/ThreadPool 不下沉。
19. Python GIL/asyncio 留在 dclpy。
20. C++ typed template API 留在 dclcpp。

### 19.4 QoS

21. QoS common preset 数值只有 DMW 一个 authority。
22. Service default = KeepLast(10)/Reliable/Volatile。
23. SensorData = KeepLast(5)/BestEffort/Volatile。
24. Parameter/ParameterEvent = KeepLast(1000)/Reliable/Volatile。
25. Action Status = KeepLast(1)/Reliable/TransientLocal。
26. BestAvailable 不进入 V1 public QoS。

### 19.5 Service

27. Service correlation 归 DMW。
28. Server pending RequestId FSM 归 DMW。
29. Service availability/wait 归 DMW。
30. availability 不允许跨 participant 拼接。
31. Future completion 归 Client Library。

### 19.6 Timer

32. Timer 归 DMW。
33. Timer 使用 monotonic clock。
34. Timer 不执行 user callback。
35. readiness level-triggered直到 consume。
36. missed cycles 跳过并重新对齐周期网格。
37. Timer 直接进入 DMW WaitSet。

### 19.7 Action

38. Action endpoint composition 归 DMW。
39. GoalId/Goal FSM 归 DMW。
40. Cancel selection common semantics 归 DMW。
41. terminal Goal/pending result RequestId/expiry state 归 DMW。
42. typed result payload cache 留 Client Library。
43. ActionClient/Server 作为 aggregate waitable，各使用一个 registration token。
44. Action availability 由同一 DiscoveryGraph authority 计算。
45. dclcpp/dclpy 不重新组合 3 Service + 2 Topic 建第二套协议。
46. accepted SendGoal response 与 Goal registry commit 必须通过 `ActionServer::accept_goal()` 形成单一 DMW transaction；raw `write_goal_response()` 只用于 rejected response。
47. accepted response 写出后的本地 Goal commit 必须 no-fail/no-allocation；write 失败必须 rollback reservation。

### 19.8 Graph

48. GraphSnapshot/GraphEvent authority 位于 DMW DiscoveryGraph。
49. dclcpp/dclpy 不建立独立 discovery cache。
50. V1 Graph 只公开可可靠从 DDS discovery 获得/组合的信息。
51. V1 不从 Participant name 猜测 ROS Node identity。

### 19.9 WaitSet

52. WaitSet 在 DMW；Executor 在 Client Library。
53. Timer/Action/GraphEvent 使用同一 WaitSet authority。
54. Finite timeout 使用 steady-clock absolute deadline。
55. topology wake 不重置 timeout。
56. 正常路径不使用固定周期 polling slice。
57. WaitSet 不拥有 registered waitable。
58. registered waitable destructor 自动 detach。

### 19.10 ROS 2 compatibility

59. Topic 使用 ROS 2 `rt/` mapping。
60. Service 使用 `rq/...Request` / `rr/...Reply` mapping。
61. Service correlation 使用 SampleIdentity/related_sample_identity。
62. response-reader wait timeout 从 effective response-writer QoS 派生，不是 public hard-coded constant。
63. Action logical suffix 使用 `/_action/send_goal`、`cancel_goal`、`get_result`、`feedback`、`status`。
64. Wire compatibility 与完整 ROS Graph compatibility 分离。

## 20. 结论

DMW V1 的目标不是复制 ROS 2 RMW/RCL package tree，而是把 DCL 两种 Client Library 真正需要共享的语言无关能力集中到一个 runtime authority：

```text
                 dclcpp / dclpy
          typed API / Future / callback
                       │
                       ▼
                     DMW
 Context / Topic / Service / Timer / Action / Graph / Wait
                       │
                       ▼
                   Fast DDS
```

本轮设计冻结后：

- Timer 不再由 dclcpp/dclpy 各自维护 deadline；
- Action 不再由 dclcpp/dclpy 各自实现 3 Service + 2 Topic、Goal FSM 和 cancel/result lifecycle；
- Graph 不再允许两个上层各建 discovery cache；
- QoS common profile 数值只在 DMW 定义；
- Future、callback、Executor、GIL/asyncio 仍严格留在语言层。

这使 DMW 成为 DCL 的完整 language-neutral middleware/common-runtime 层，同时保持结构轻量，不引入新的 `rcl` package、通用 HAL、middleware plugin 或不必要的 framework abstraction。
