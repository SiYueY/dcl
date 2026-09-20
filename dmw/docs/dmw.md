# DMW 设计文档

| 属性 | 值 |
| --- | --- |
| 文档状态 | V1 Architecture Convergence |
| 模块名称 | DMW — DDS Middleware / Language-Neutral Common Runtime |
| 上层 | `dclcpp`、`dclpy` / `_dclpy` |
| 下层 | Fast DDS |
| 语言标准 | C++17 |
| 主要实现参考 | Fast DDS 2.14.x、`rmw_fastrtps` Jazzy（平等参考基线） |
| Common Runtime 参考 | ROS 2 Jazzy `rcl`、`rcl_action` |
| Client Library 边界参考 | ROS 2 Jazzy `rclcpp`、`rclpy` |
| 兼容性验证目标 | ROS 2 Humble / Fast DDS 2.6.x；ROS 2 Jazzy / Fast DDS 2.14.x |

本文档定义 DCL 项目中 `dmw` 模块 V1 的 **public/runtime normative contract**。Fast DDS 专用实现规则由 [`dmw_fastdds.md`](dmw_fastdds.md) 定义；若二者冲突，以本文档定义的 public API、错误、生命周期、并发和可观察语义为准。

---

## 1. 定位、分层与 V1 边界

### 1.1 DMW 的定位

DCL 不复制 ROS 2 的 package hierarchy，也不新增独立 `rcl` 等价 package。DMW 同时承担：

1. Fast DDS binding / middleware primitive；
2. 适合 `dclcpp`、`dclpy` 共同复用的 language-neutral common runtime。

总体关系固定为：

```text
             C++ Application              Python Application
                    │                             │
                    ▼                             ▼
                 dclcpp                         dclpy
                    │                             │
                    │                           _dclpy
                    │                             │
                    └──────────────┬──────────────┘
                                   ▼
                                  DMW
                                   │
                                   ▼
                               Fast DDS
                                   │
                                   ▼
                               DDSI-RTPS
```

DMW：

- 不依赖 `rcl`、`rcl_action`、`rclcpp`、`rclpy` 或 ROS 2 runtime；
- Fast DDS-only；
- 不提供 middleware plugin abstraction；
- 普通 runtime public API 不暴露 Fast DDS / Fast CDR 类型；
- public runtime API 以 non-template、type-erased C++17 API 为主；
- 不执行 user callback；
- 不实现 Executor scheduling；
- 不提供 `std::future` / Python Future / asyncio / GIL policy；
- 不把 C++ template typed API 或 Python object model 下沉到 DMW。

DMW 的参考关系不是：

```text
DMW == rmw_fastrtps thin wrapper
```

而是：

```text
DMW
≈ rmw / rmw_fastrtps 的 Fast DDS runtime responsibilities
+ rcl 的 language-neutral runtime responsibilities
+ rcl_action 的 language-neutral protocol/state-machine responsibilities
+ 少量为避免 dclcpp/dclpy 重复而进一步下沉的 common state
```

### 1.2 下沉判定规则

能力满足以下条件时，应优先由 DMW 实现一次：

```text
与 C++ / Python 类型系统无关
+
两种 Client Library 必须具有相同语义
+
涉及 protocol / identity / state machine / readiness /
discovery / clock / naming / lifecycle / shared validation
```

因此下列能力属于 DMW：

```text
Context / Node runtime
Clock / Time common runtime
Arguments / Remapping / name resolution
Message / Service / Action runtime descriptors
QoS model / common profiles / compatibility
Topic / Service transport primitives
Service correlation / availability
Timer state / deadline / readiness
WaitSet / GuardCondition / Event
DiscoveryGraph / Graph snapshot / GraphEvent
Action endpoint composition / Goal FSM / cancel/result/status common state
Parameter value / descriptor / store / validation / override common state
ROS 2 Fast DDS wire mapping
```

下列能力不得仅为了减少表面代码重复而下沉：

```text
C++ templates
std::function / Python callable
std::promise / std::future
Python Future / Task / asyncio Future
Pending Future registry
Executor callback dispatch
CallbackGroup / ThreadPool policy
C++ exception presentation
Python exception mapping / GIL / asyncio integration
typed Action GoalHandle presentation
typed Action result payload cache
user parameter callbacks
```

### 1.3 上游参考模型

后续设计和实现采用：

```text
Fast DDS 2.14.x ─────────────┐
                             ├── cross-audit ──> DMW
rmw_fastrtps Jazzy ──────────┘

rcl / rcl_action Jazzy
    └── common runtime / clock / wait / graph / action state-machine 参考

rclcpp / rclpy Jazzy
    └── typed API / callback / Future / Executor / language-runtime 边界参考
```

Fast DDS 2.14.x 与 `rmw_fastrtps` Jazzy 是平等主要参考。实现策略不机械复制任一上游，而同时评估：

1. Fast DDS public API semantics 与 guarantees；
2. `rmw_fastrtps` 的生产实践和 ROS 2 interoperability 行为；
3. `rcl` / `rcl_action` 的 language-neutral runtime contract；
4. DMW 自身 public contract、复杂度与生命周期约束。

ROS 2 Humble / Fast DDS 2.6.x 是持续兼容性验证目标，不反向决定新的 public API。需要兼容 2.6.x 时，优先使用 2.6.x 与 2.14.x 均存在的稳定 DDS-PIM 能力；确有 API 差异时仅允许 private compatibility shim。

### 1.4 V1 基础能力矩阵

新的 V1 以“足以支撑 dclcpp/rclcpp-like 与 dclpy/rclpy-like 基础 Client Library，而不在两种语言层重复 common runtime”为标准。

| 能力 | 归属 | V1 状态 |
| --- | --- | --- |
| Error / Result | DMW | 已实现 / 需收敛异常映射 |
| Context / Node | DMW | 已实现 / 继续完善 |
| MessageType / ServiceType | DMW | 已实现 |
| ActionType | DMW | 设计冻结，待实现 |
| Arguments / Remapping / name resolution | DMW | **新增，待实现** |
| Clock / Time common runtime | DMW | **新增，待实现** |
| QoS mapping / common profiles | DMW | 已实现基础 / 需补齐 |
| actual QoS / compatibility / ACK / liveliness operation | DMW | **新增，待实现** |
| Publisher / Subscriber | DMW | 已实现 / 持续完善 |
| Client / Server | DMW | 已实现 / 持续完善 |
| Service correlation / availability / wait | DMW | 已实现 / 持续完善 |
| WaitSet | DMW | 已实现 foundation / 需扩展 |
| GuardCondition / Event | DMW | 已实现 / 持续完善 |
| Timer | DMW | 设计冻结，待实现；改为 Clock-aware |
| DiscoveryGraph | DMW | internal foundation 已实现 / 需收敛 revision |
| Graph public runtime | DMW | 设计冻结，待实现并扩展 Node/endpoint view |
| ActionClient / ActionServer | DMW | 设计冻结，待实现 |
| Goal FSM / cancel/result/status common state | DMW | 设计冻结，待实现 |
| Parameter common runtime | DMW | **新增，待实现** |
| Future / Promise / Task | Client Library | 不下沉 |
| Pending Future registry | Client Library | 不下沉 |
| user callback | Client Library | 不下沉 |
| Executor / CallbackGroup / ThreadPool | Client Library | 不下沉 |
| Python GIL / asyncio | dclpy | 不下沉 |
| C++ template typed API | dclcpp | 不下沉 |
| Component / Composition | 整个 DCL | **V1 明确不实现** |

“设计冻结，待实现”表示本文已经冻结职责和主要 public contract，不表示当前源码已经具备实现。

### 1.5 V1 非目标

V1 明确不实现：

- Component / Composition；
- `rclcpp_components` 等价机制；
- dynamic component loading / component manager；
- Lifecycle Node；
- 多 middleware backend；
- stable C ABI；
- IDL parser / compiler / code generator；
- loaned message public API；
- zero-copy public API；
- content filtered topic；
- message batch take；
- dynamic type public API；
- DDS Security public API；
- advanced network-flow endpoint API；
- Executor；
- CallbackGroup；
- Future / Promise；
- Python asyncio runtime；
- logger frontend / C++ logging macro / Python logging facade。

这些非目标不允许削弱 V1 已纳入的基础 runtime contract。例如不实现 Executor，不代表 WaitSet 可以缺 Timer/Action/Graph readiness；不实现 Component，也不影响 Node/Graph/Parameter 等基础能力完整性。

---

## 2. 对象模型、所有权与类型边界

### 2.1 Resource / Entity

以下类型具有稳定 identity 和独立 runtime backing：

```text
Context
Node
Clock
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

public ownership 使用 `std::unique_ptr`。内部允许 `shared_ptr` / `weak_ptr` 维护 backing lifetime、registered-waitable lifetime、Context facade 先于 child facade 析构等 teardown safety，但不得改变 public ownership model。

### 2.2 Value / Descriptor

以下类型属于 value / descriptor，不表示 DDS resource ownership：

```text
ContextOptions / NodeOptions
PublisherOptions / SubscriberOptions
ClientOptions / ServerOptions
TimerOptions
ActionClientOptions / ActionServerOptions

MessageType / ServiceType / ActionType
Qos / QosDuration / QosCompatibilityResult
Gid / RequestId / MessageInfo
ClockType / TimePoint / Duration
GoalId / GoalInfo / GoalState / GoalEvent / GoalAcceptMode
GoalStatusInfo / CancelGoalCriteria / GoalTransition
ActionClientReadySet / ActionServerReadySet
GraphRevision / GraphSnapshot / GraphChangeInfo
WaitTimeout / WaitableRegistration / WaitResult
EventType / EventInfo
ParameterType / ParameterValue / Parameter / ParameterDescriptor
ParameterChangeSet / ParameterListResult
Arguments / RemapRule
Error / Result<T>
```

如果某个 `*Options` 在 V1 没有任何真实字段，不应仅为“未来可能扩展”引入新的 empty public type；已经存在的 empty options 可以在 API 收敛时移除或保留兼容 overload，但新的设计不继续复制此模式。

### 2.3 Factory 归属

```text
Context::create()
      │
      ├── create_node()
      ├── create_clock()
      ├── create_wait_set()
      ├── create_guard_condition()
      ├── create_timer(clock, ...)
      └── create_graph_event()

Node
      ├── create_publisher()
      ├── create_subscriber()
      ├── create_client()
      ├── create_server()
      ├── create_action_client()
      └── create_action_server()
```

Timer 是 Context-scoped runtime primitive，不是 DDS Node entity；它依赖一个同 Context 的 Clock。`dclcpp::Node` / `dclpy.Node` 可以提供 `create_timer()` convenience，并选择其 Node clock，但不改变 DMW ownership。

ActionClient/ActionServer 需要 Node name/namespace resolution，因此归属 Node。

Parameter common state 归属 Node logical identity，不创建 DDS entity；参数服务/事件的 typed message assembly 属于 Client Library/standard-interface binding，底层 ParameterStore authority 仍在 DMW。

### 2.4 事务式创建

所有 Resource/Entity factory 必须满足：

```text
validate
   ↓
resolve names/options/common state
   ↓
reserve registry/internal state
   ↓
create middleware/native resources
   ↓
install listener/condition/wait backing
   ↓
commit registry/public visibility
   ↓
return complete object
```

任一步失败：

- local RAII rollback；
- 不返回 half-valid object；
- 不留下对上层可见的 partial registration；
- composite resource（Client/Server/Action）不得部分成功后暴露。

### 2.5 Type-erased message pointer contract

DMW runtime message path使用 `void*` / `const void*`，例如：

```cpp
Result<void> Publisher::write(const void* message);
Result<bool> Subscriber::read(void* message, MessageInfo& info);
```

规则：

- `nullptr` -> `InvalidArgument`；
- concrete C++ object 必须与 endpoint 绑定的 MessageType 对应；
- DMW V1 不从 `void*` 动态识别真实 C++ type；
- wrong concrete type 属于 programming contract violation；
- `TypeMismatch` 只用于 DMW 可验证的 descriptor/registry 冲突。

对于 `read()` / `read_response()` / `read_request()` 和 Action take API：

- `success + false` 时 caller output 保持不变；
- middleware sample 被消费前发生 Error，caller output 保持不变；
- sample 已消费后发生 deserialize/conversion Error，只保证 output 仍可安全析构，内容允许 unspecified；
- 不使用 `optional` 表达“本次没有 sample”。

---

## 3. Foundation：Error、Context、Clock、Arguments、Node 与类型系统

### 3.1 Error / Result

V1 `ErrorCode`：

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

`Result<T>` contract：

- 明确为 success 或 failure；
- 不允许无意义 default state；
- 支持 move-only T；
- 提供 `value() & / const& / &&`；
- 提供 `error() & / const& / &&`；
- wrong-alternative access 调用 `std::terminate()`；
- expected middleware/runtime failure 不使用 C++ exception；
- `std::bad_alloc` 允许传播，不映射为 `ResourceExhausted`；
- `Result<void>` 使用同一语义。

Public runtime operation 统一错误优先级：

1. 可安全检测的 public 参数错误；
2. Context `ShuttingDown/Shutdown`；
3. Parent destroyed；
4. object-local state，例如 `Busy` / `NotFound` / `AlreadyRegistered`；
5. middleware/runtime error，例如 `DDSError` / `Timeout`。

Clock、Parameter、Timer、Graph、Action 不建立第二套错误优先级。

### 3.2 ContextOptions 与 Context API

```cpp
struct ContextOptions
{
    std::uint32_t domain_id{0};
    std::string participant_name;
    RuntimeMode runtime_mode{RuntimeMode::DDS};
    Arguments arguments;
};
```

`RuntimeMode`、domain id、global arguments 在 Context 创建成功后不可修改。

V1 目标 surface：

```cpp
class Context
{
public:
    static Result<std::unique_ptr<Context>>
    create(const ContextOptions& options);

    ~Context() noexcept;

    std::uint32_t domain_id() const noexcept;
    RuntimeMode runtime_mode() const noexcept;
    bool is_shutdown() const noexcept;
    Result<void> shutdown();

    Result<std::unique_ptr<Node>>
    create_node(const NodeOptions& options);

    Result<std::unique_ptr<Clock>>
    create_clock(ClockType type);

    Result<std::unique_ptr<WaitSet>>
    create_wait_set();

    Result<std::unique_ptr<GuardCondition>>
    create_guard_condition();

    Result<std::unique_ptr<Timer>>
    create_timer(Clock& clock, const TimerOptions& options);

    Result<std::unique_ptr<GraphEvent>>
    create_graph_event();

    Result<GraphSnapshot> graph_snapshot() const;
    Result<GraphRevision> graph_revision() const;
};
```

现有源码尚未包含 Clock/Timer/Graph public API；本文冻结的是 V1 目标。

Context state machine：

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

进入 `ShuttingDown` 后必须：

- 禁止创建新 child/resource；
- 中断 Service/Action availability wait；
- 中断 active WaitSet；
- 中断 Clock/Timer/Graph logical wait；
- 阻止新的 middleware operation；
- 允许已有 public object 安全析构；
- 最终进入 Shutdown。

Context facade 可以早于 child facade 析构；内部 Context backing 必须存活到最后一个 child 安全释放。

### 3.3 Clock / Time common runtime

Clock 属于 DMW，而不是由 dclcpp/dclpy 各自实现一套 time state。

```cpp
enum class ClockType
{
    System,
    Steady,
    Ros
};

struct TimePoint
{
    std::int64_t nanoseconds{0};
    ClockType clock_type{ClockType::System};
};
```

Duration 使用 `std::chrono::nanoseconds` 表达；跨 clock type 的 TimePoint 不允许直接比较。

基础 API：

```cpp
class Clock
{
public:
    ~Clock() noexcept;

    ClockType type() const noexcept;
    Result<TimePoint> now() const;

    // 仅 Ros clock 合法。
    Result<bool> ros_time_override_enabled() const;
    Result<void> enable_ros_time_override(bool enabled);
    Result<void> set_ros_time(TimePoint time);
};
```

规则：

- `System` 使用 system/realtime time domain；
- `Steady` 使用 monotonic time domain，不受 wall-clock jump 影响；
- `Ros` 未启用 override 时跟随 system time；启用 override 后仅由显式 ROS-time update 推进；
- `set_ros_time()` 只接受 `ClockType::Ros` time point，其他 clock 调用 -> `InvalidState/InvalidArgument`；
- ROS-time update / enable-state change 必须唤醒依赖该 Clock 的 active Timer/WaitSet，使 readiness 重新计算；
- DMW 不执行 clock jump user callback；如未来增加 jump callback，仍通过 language-layer callback/Executor 分发。

Clock facade 可以早于其 Timer facade 析构；Timer 持有 Clock backing 的内部共享引用。Clock 与 Timer 必须属于同一 Context，否则创建返回 `InvalidArgument`。

### 3.4 Arguments / Remapping

DMW 是 global/node-local arguments 与 remapping 的唯一 parser/authority；dclcpp/dclpy 不各自实现 name resolution。

`Arguments` 是 immutable parsed descriptor，至少支持：

```text
node name remap
node namespace remap
topic remap
service remap
action remap
parameter override records
unparsed/non-DCL arguments preservation
```

推荐 public helper：

```cpp
Result<Arguments>
parse_arguments(const std::vector<std::string>& arguments);
```

NodeOptions：

```cpp
struct NodeOptions
{
    std::string node_name;
    std::string node_namespace{"/"};
    Arguments arguments;
    bool use_global_arguments{true};
    bool allow_undeclared_parameters{false};
};
```

解析/应用顺序固定为：

```text
Context global Arguments
        +
Node local Arguments
        ↓
select rules applicable to this Node
        ↓
node-name / namespace remap
        ↓
validate normalized Node identity
        ↓
entity logical name expansion
        ↓
topic/service/action remap
        ↓
normalized logical FQN
        ↓
RuntimeMode DDS wire mapping
```

同一规则集合中多条匹配的优先顺序必须由 DMW 文档化并保持 dclcpp/dclpy 一致。V1 应与 ROS 2 常见 remapping 语义兼容，但 DMW 不依赖 `rcl` parser 实现。

Parameter override 只形成 DMW common override state；“是否自动声明 override 中未显式声明的参数”是 Client Library Node policy，不由 argument parser擅自改变 parameter store。

### 3.5 Node

Node 是 logical entity，不等于 DDS DomainParticipant。

一个 Context 固定：

```text
1 DDS Domain ID
1 DomainParticipant
```

多个 Node 共享 Context Participant。

Node public identity 至少提供：

```cpp
std::string_view name() const noexcept;
std::string_view node_namespace() const noexcept;
std::string_view fully_qualified_name() const noexcept;
```

Node facade 析构后，已经创建的 communication endpoint 可以继续使用，只要 Context 仍 Active；endpoint 内部持有 NodeState 所需 name/namespace/remapping backing。

Node 创建/销毁必须同步更新 DMW local Graph metadata authority；不得通过 DDS Participant name 猜测 Node identity。

### 3.6 Gid / MessageType / ServiceType / ActionType

```cpp
struct Gid
{
    static constexpr std::size_t Size = 16;
    std::array<std::uint8_t, Size> data{};
};
```

Fast DDS GUID 不进入普通 public API。

`MessageType` 是 cheap-copy immutable runtime descriptor handle：

- 不存在 invalid default object；
- `type_name()` 表示 DDS wire type name；
- Fast DDS TypeSupport integration 只出现在 `dmw/fastdds/message_type.hpp`；
- type identity 使用 `DDS wire type name + BindingIdentity`；
- same wire name + same binding identity 可 reuse；
- same wire name + different binding identity -> `TypeMismatch`。

ServiceType：

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

ActionType：

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

ActionType 只描述 wire/runtime endpoint types，不保存 `ActionT` template/Python class information，不解析任意 action-specific object layout。

### 3.7 Node communication factory surface

V1 Node communication factory 保持 type-erased、transactional：

```cpp
Result<std::unique_ptr<Publisher>> create_publisher(
    const MessageType& type,
    std::string_view topic_name,
    const Qos& qos,
    const PublisherOptions& options = {});

Result<std::unique_ptr<Subscriber>> create_subscriber(
    const MessageType& type,
    std::string_view topic_name,
    const Qos& qos,
    const SubscriberOptions& options = {});

Result<std::unique_ptr<Client>> create_client(
    const ServiceType& type,
    std::string_view service_name,
    const Qos& qos,
    const ClientOptions& options = {});

Result<std::unique_ptr<Server>> create_server(
    const ServiceType& type,
    std::string_view service_name,
    const Qos& qos,
    const ServerOptions& options = {});

Result<std::unique_ptr<ActionClient>> create_action_client(
    const ActionType& type,
    std::string_view action_name,
    const ActionClientOptions& options = {});

Result<std::unique_ptr<ActionServer>> create_action_server(
    const ActionType& type,
    std::string_view action_name,
    const ActionServerOptions& options = {});
```

所有 name 参数先走 Node namespace expansion/remapping，再走 RuntimeMode transport resolver。Factory 不允许将半创建的 composite endpoint暴露给caller。


---

## 4. RuntimeMode、Naming 与 QoS

### 4.1 RuntimeMode 与 wire naming

```cpp
enum class RuntimeMode
{
    DDS,
    ROS2
};
```

RuntimeMode 属于 Context，不允许 endpoint override。

logical FQN `/a/b` 的基础 transport mapping：

| RuntimeMode | Topic | Service request | Service response |
| --- | --- | --- | --- |
| `DDS` | `a/b` | `a/b_Request` | `a/b_Reply` |
| `ROS2` | `rt/a/b` | `rq/a/bRequest` | `rr/a/bReply` |

Public observer 始终返回 normalized logical name，不返回 resolved DDS transport name。

完整 name pipeline 必须是：

```text
user name
  ↓
Node namespace expansion
  ↓
Arguments/Remapping
  ↓
normalized logical FQN
  ↓
RuntimeMode resolver
  ↓
DDS transport name
```

Action 的五个 logical name 先按 Action 规则派生，再分别经过 Service/Topic resolver；禁止 Action 内部复制第二套 `rq/rr/rt` 逻辑。

### 4.2 基础 QoS policy

V1 public QoS 支持：

- History / Depth；
- Reliability；
- Durability；
- Deadline；
- Lifespan；
- Liveliness；
- Liveliness lease duration。

`QosDuration` 使用：

```text
SystemDefault
Infinite
Finite
```

不使用 magic duration sentinel。

规则：

- `keep_last(depth)` 要求 `depth > 0`；
- `KeepAll` canonical depth 为 0；
- finite duration 不得为负；
- conversion 到 DDS duration 必须 overflow-safe；
- QoS value 本身不暴露 Fast DDS policy type。

### 4.3 common profiles

ROS 2/common profile 数值只有 DMW 一个 authority。V1 提供：

```cpp
static Qos ros2_default();
static Qos ros2_services_default();
static Qos ros2_sensor_data();
static Qos ros2_parameters();
static Qos ros2_parameter_events();
static Qos ros2_action_status_default();
```

profile：

| Profile | History | Depth | Reliability | Durability |
| --- | --- | ---: | --- | --- |
| `ros2_default` | KeepLast | 10 | Reliable | Volatile |
| `ros2_services_default` | KeepLast | 10 | Reliable | Volatile |
| `ros2_sensor_data` | KeepLast | 5 | BestEffort | Volatile |
| `ros2_parameters` | KeepLast | 1000 | Reliable | Volatile |
| `ros2_parameter_events` | KeepLast | 1000 | Reliable | Volatile |
| `ros2_action_status_default` | KeepLast | 1 | Reliable | TransientLocal |

其余 Deadline/Lifespan/Liveliness/Lease 使用对应 profile 的 default/system-default 语义。

`dclcpp::SensorDataQoS`、`dclcpp::ServicesQoS`、Python convenience profile 等只能包装这些 DMW profile，不得保存第二份 preset 数值表。

### 4.4 SystemDefault

`Qos::system_default()` 不等于 `ros2_default()`。

`SystemDefault` 的含义固定为：

> 该字段没有被 DMW public Qos 显式指定，由当前 RuntimeMode 和实际 Fast DDS entity baseline 解析为 effective middleware value。

因此：

- `SystemDefault` 不应被静态重写成一组“看起来像默认值”的 DMW 常量；
- DMW 自己必须控制其默认 entity baseline，不允许不可审计的全局 mutable state 随机改变行为；
- 如果未来显式支持 Fast DDS XML/profile override，必须通过清晰 option 启用，而不是隐式读取环境状态；
- caller 使用 `actual_qos()` 观察最终 effective value。

`ros2_default()` 则是一组确定的 DMW public profile，语义不同。

### 4.5 actual QoS

基础 Client Library 需要查询实际 middleware QoS，因此 V1 增加：

```cpp
Result<Qos> Publisher::actual_qos() const;
Result<Qos> Subscriber::actual_qos() const;
Result<Qos> Client::request_actual_qos() const;
Result<Qos> Client::response_actual_qos() const;
Result<Qos> Server::request_actual_qos() const;
Result<Qos> Server::response_actual_qos() const;
```

Action 的 internal endpoint actual QoS 默认不单独暴露五个 public endpoint；如需诊断，可由 Action-specific aggregate query 后续扩展。V1 首先保证 common profiles 与实际 endpoint mapping 可验证。

`actual_qos()` 返回的是当前 entity 的 effective QoS snapshot，不返回 Fast DDS-native object。

### 4.6 QoS compatibility

QoS compatibility 逻辑属于 DMW，不允许 dclcpp/dclpy 各写一套。

```cpp
enum class QosCompatibility
{
    Compatible,
    Warning,
    Incompatible
};

struct QosCompatibilityResult
{
    QosCompatibility compatibility{QosCompatibility::Compatible};
    std::string reason;
};

Result<QosCompatibilityResult>
check_qos_compatibility(const Qos& publisher_qos,
                        const Qos& subscriber_qos);
```

规则：

- 只评价 DMW public QoS 可表达的 compatibility；
- `Incompatible` 用于明确不会匹配的 policy combination；
- `Warning` 用于可能依赖 effective/system-default state、无法仅凭给定 descriptor 完全确定的情况；
- runtime actual endpoint matching 仍以 Fast DDS discovery/matching 为最终事实；
- 不把 compatibility helper 当成 endpoint discovery 替代品。

### 4.7 Writer-side 基础 operation

V1 Publisher 补齐：

```cpp
Result<bool> wait_for_all_acked(WaitTimeout timeout);
Result<void> assert_liveliness();
```

语义：

- `wait_for_all_acked()` timeout -> `success + false`；
- Context shutdown -> `ContextShutdown`；
- 对不支持/无意义的 QoS 组合按 Fast DDS capability 返回 `Unsupported` / `InvalidState`，不得伪造成功；
- `assert_liveliness()` 用于 ManualByTopic 等需要 application assertion 的 liveliness；
- Automatic liveliness 上调用可返回 success 或 `InvalidState`，最终语义必须固定并通过测试覆盖，不能由不同语言 wrapper 自行决定。

### 4.8 V1 不引入 BestAvailable

ROS 2 Jazzy RMW 存在 BestAvailable policy，但它依赖 discovery-time endpoint 情况，并可能造成创建时 race。DMW V1 不将其加入 public QoS；需要时在后续 design revision 中单独评审。

---

## 5. Topic 与 Service Communication Runtime

### 5.1 Publisher

基础 public API：

```cpp
class Publisher
{
public:
    Result<void> write(const void* message);

    std::string_view topic_name() const noexcept;
    const MessageType& message_type() const noexcept;

    Result<Qos> actual_qos() const;
    Result<std::size_t> matched_subscriber_count() const;
    Result<bool> wait_for_all_acked(WaitTimeout timeout);
    Result<void> assert_liveliness();

    Result<std::unique_ptr<Event>> create_event(EventType type);
};
```

`write()` 只负责 middleware write；user callback、typed message presentation、intra-process callback dispatch 均不在 DMW。

### 5.2 Subscriber

```cpp
class Subscriber
{
public:
    Result<bool> read(void* message, MessageInfo& info);

    std::string_view topic_name() const noexcept;
    const MessageType& message_type() const noexcept;

    Result<Qos> actual_qos() const;
    Result<std::size_t> matched_publisher_count() const;

    Result<std::unique_ptr<Event>> create_event(EventType type);
};
```

`success + false` 表示本次 finite scan 没有取得 public sample，不是 Error。

单次 read 必须有有限 candidate budget；并发新 arrival 不得无限延长一个 public `read()` 调用。

### 5.3 MessageInfo

`MessageInfo` 至少标准化：

```text
writer_gid
source/writer timestamp
reception/reader timestamp
writer/sample sequence（可用时）
```

不可获得的 metadata 使用明确 unknown value；禁止用本地 `steady_clock` 伪造 DDS reception/source timestamp。

`MessageInfo` 是 middleware-neutral value，不暴露 Fast DDS `SampleInfo`。

### 5.4 matched count

matched count 是 query-time compatible DDS match snapshot，不是 Graph 中“同名 endpoint 数”。

至少满足：

```text
same domain
same resolved topic
same wire type
compatible QoS
discovery/matching completed
```

才计入 matched count。

### 5.5 Service 组成

```text
Client
├── request DataWriter
└── response DataReader

Server
├── request DataReader
└── response DataWriter
```

DMW 对上层暴露整体 Client/Server；Client Library 不允许重新拼装四个 DDS endpoint 建第二套 service protocol。

### 5.6 RequestId

```cpp
struct RequestId
{
    Gid client_gid{};
    std::int64_t sequence_number{0};
};
```

`client_gid` 是 runtime-mode-neutral correlation identity；在 ROS2 Fast DDS service mapping 中通常表示 Client response reader identity。

### 5.7 Client

```cpp
class Client
{
public:
    Result<RequestId> write_request(const void* request);
    Result<bool> read_response(void* response, RequestId& request_id);

    Result<bool> service_is_available() const;
    Result<bool> wait_for_service(WaitTimeout timeout) const;

    Result<Qos> request_actual_qos() const;
    Result<Qos> response_actual_qos() const;

    std::string_view service_name() const noexcept;
};
```

DMW 不维护 language Future/promise table。

`read_response()` 只保证：

- response 属于当前 Client identity；
- RequestId 正确标准化；
- `success + false` 不修改 output。

RequestId 是否仍存在于上层 pending Future registry 属于 `dclcpp` / `dclpy`。

### 5.8 Server pending request FSM

现有 Server bounded pending-request design 保留在 V1，因为它定义了明确 backpressure、duplicate suppression 与 response retry 行为。

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
- concurrent response to same `Responding` request -> `Busy`；
- pending capacity 满时必须在 middleware destructive take 前返回 `ResourceExhausted`；
- Pending/Responding 生命周期内 suppress duplicate RequestId；
- 不维护无限 responded tombstone；
- write failure 后 RequestId 回到 Pending，允许 application retry；
- Context shutdown 优先于 object-local pending state。

基础 API：

```cpp
class Server
{
public:
    Result<bool> read_request(void* request, RequestId& request_id);
    Result<void> write_response(const RequestId& request_id,
                                const void* response);

    Result<Qos> request_actual_qos() const;
    Result<Qos> response_actual_qos() const;

    std::string_view service_name() const noexcept;
};
```

### 5.9 Service availability

`service_is_available()` authority 位于 DMW DiscoveryGraph。

禁止只使用：

```text
request matched > 0
AND
response matched > 0
```

因为两侧可能来自不同 remote participant。

DMW 至少要求同一 remote participant 中存在 name/type/QoS-compatible：

```text
request DataReader
+
response DataWriter
```

形成一个 **participant-consistent server candidate**。

必须注意：在没有额外 endpoint-group identity metadata 的情况下，“same participant”只能证明该 participant 中存在可组合的 endpoint candidate，不能严格证明这些 endpoint 来自唯一具体 remote Service object。因此 public contract 不宣称发现了一个可唯一标识的 remote Server instance。

`wait_for_service()`：

- 与同一 availability authority 绑定；
- 由 discovery revision / notification 唤醒；
- 不使用固定 sleep polling；
- timeout -> `success + false`；
- Context shutdown -> `ContextShutdown`；
- topology/discovery wake 不重新开始原始 timeout。

### 5.10 ROS 2 Fast DDS Service wire contract

ROS2 mode 使用 Fast DDS：

```text
SampleIdentity
related_sample_identity
```

完成 request/response correlation。

Client request write：

- response reader GUID 放入 request related identity；
- successful write 生成 request sample identity；
- 标准化为 RequestId。

Server take：

- 从 sample/related identity 恢复 response target 与 sequence；
- 标准化为 RequestId。

Server response：

- response 使用 RequestId 相关 identity；
- Client 根据自己的 endpoint identity 过滤 response。

当 response target identity 表示 Client response reader 时，Server response writer 必须等待对应 reader matched 或确认目标已消失。

该等待时长不是 DMW hard-coded public constant。Fast DDS 实现从 effective response-writer reliability QoS 的 `max_blocking_time` 派生单一 absolute steady deadline。

public observable contract：

```text
already matched -> write
matched before deadline -> write
confirmed target gone -> success without write
deadline expires -> Timeout
Context shutdown -> ContextShutdown
```

所有 discovery/recheck 使用同一个 absolute deadline，不因 wake/retry 重置 timeout。

---

## 6. Clock-aware Timer、WaitSet、GuardCondition 与 Event

### 6.1 Timer 职责边界

Timer 参考 `rcl_timer_t` 的职责，但 DMW 不保存或执行 user callback：

```text
DMW Timer
    ├── Clock binding
    ├── period
    ├── next call point
    ├── ready / canceled state
    ├── reset / cancel / period exchange
    ├── consume scheduling state
    └── WaitSet integration

Client Library
    └── callback / callback-group / Executor dispatch
```

Timer 不创建 callback thread，也不使用 Fast DDS entity。

### 6.2 TimerOptions / TimerInfo

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
- `autostart == false` 创建后 canceled；
- Timer 使用其绑定 Clock；
- Clock 和 Context 不匹配 -> `InvalidArgument`。

TimerInfo 不使用 `std::chrono::steady_clock::time_point` 固定死 clock domain，改为：

```cpp
struct TimerInfo
{
    TimePoint expected_call_time;
    TimePoint actual_call_time;
    std::chrono::nanoseconds elapsed_since_last_call{0};
};
```

`expected_call_time`、`actual_call_time` 的 `clock_type` 必须等于 Timer Clock type。

### 6.3 Timer API

```cpp
class Timer
{
public:
    ~Timer() noexcept;

    std::chrono::nanoseconds period() const noexcept;
    ClockType clock_type() const noexcept;

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

Timer ready iff：

```text
Context Active
AND !canceled
AND clock.now() >= next_call_time
```

`is_ready()` 不消费 readiness。

Canceled timer：

- `is_ready()` -> `success + false`；
- `consume()` -> `success + false`；
- `time_until_next_call()` -> `InvalidState`。

### 6.4 reset / cancel / exchange_period

`reset()`：

```text
canceled = false
last_call_time = clock.now()
next_call_time = now + period
```

period 0 时立即 ready。

`cancel()`：

- 幂等；
- canceled = true；
- 唤醒注册 WaitSet 重算 earliest deadline；
- 不执行 callback。

`exchange_period(new_period)`：

- negative -> `InvalidArgument`；
- 返回旧 period；
- 不隐式 reset current next call point；
- 下一次 successful `consume()` 使用新 period 推进 schedule；
- 如需从当前时间重新起算，caller 随后显式 `reset()`；
- period change 必须唤醒 active WaitSet 重新计算 deadline。

### 6.5 consume / missed-period 规则

`consume(info)`：

1. canceled/not ready -> `success + false`，info unchanged；
2. ready -> snapshot expected 与 actual now；
3. 更新 last call；
4. `next += period`；
5. 如果新 next 仍 `<= now`，一次跳过已经错过的完整周期，使 next 成为第一个严格晚于 `now` 的周期点；
6. period 0 时 `next = now`，因此持续 ready；
7. commit TimerInfo 并返回 true。

因此 callback execution latency 不会累积为 period drift，也不会为历史 missed cycle 补发 N 次 readiness。

所有 period/time arithmetic 必须 overflow-safe；不得 silent wrap。

### 6.6 ROS-time Timer

Ros Clock override 启用时，Timer deadline 不可简单转换为固定 steady timeout，因为 ROS time 可能暂停或跳变。

规则：

- WaitSet 对 Ros Clock Timer 先计算 logical readiness；
- ROS time override update / enable-state change必须通过 Clock backing 唤醒相关 WaitSet；
- override active 且没有可证明的 steady-time deadline 时，不以 wall/steady elapsed time擅自推进 ROS Timer；
- wake 后重新基于 Clock current value判断 Timer ready；
- time jump 不允许产生 torn Timer scheduling state。

### 6.7 Waitable kinds

V1 WaitSet 支持：

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

### 6.8 WaitSet API

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

`WaitTimeout` 只允许：

```text
Poll
Finite (> 0)
Infinite
```

不使用 `-1` 或 `nanoseconds::max()` sentinel。

Finite wait deadline 在进入 wait 时计算一次：

```text
user_deadline = steady_clock::now() + timeout
```

add/remove/topology/graph/clock/timer/control wake 均不得重新开始完整 timeout。

### 6.9 effective wait deadline

对可转换为 steady remaining duration 的 runtime deadline：

```text
native_deadline = min(
    user finite deadline if any,
    earliest Timer steady/system-derived deadline,
    earliest Action result-expiry deadline)
```

ROS-time override Timer 没有可靠 steady deadline时依赖 Clock update control wake。

native wait 返回后统一 re-evaluate logical readiness。

Infinite user wait 也必须在：

- Timer 到期；
- Action goal expiry；
- Graph revision；
- GuardCondition trigger；
- topology change；
- Context shutdown；
- ROS-time update使 Timer ready；

发生时及时返回/重评估，而不是依赖固定周期 polling。

### 6.10 Registration ownership

规则：

- 一个 waitable 同一时刻最多属于一个 WaitSet；
- 第二次 add -> `AlreadyRegistered`；
- WaitSet 不拥有 waitable；
- waitable destructor 自动 detach；
- Registration token 绑定创建它的 WaitSet；
- stale token -> `NotRegistered`；
- wrong WaitSet token -> `InvalidArgument`；
- 同一 WaitSet 同时最多一个 active `wait()`；第二个 -> `Busy`；
- add/remove 可以与 active wait 并发；
- WaitSet destructor 不允许与自身 active wait 无同步并发。

### 6.11 WaitResult

WaitResult 是 readiness snapshot，不持有 public entity pointer。

基础：

```text
Ready   -> ready() 非空
Timeout -> ready() 为空
```

为避免 Action aggregate token 与后续 `readiness()` 之间出现 snapshot 漂移，V1 WaitResult 应允许保存 registration 对应的 readiness detail mask。普通 waitable 只使用通用 Ready bit；Action 使用定义好的子通道 bit。

概念：

```cpp
struct ReadyWaitable
{
    WaitableRegistration registration;
    WaitableKind kind;
    std::uint32_t detail_mask{0};
};
```

`ActionClient::readiness()` / `ActionServer::readiness()` 可以保留作为 current-state query，但 Executor 对一次 `wait()` 的处理应优先使用 WaitResult snapshot，而不是依赖稍后重新查询才能知道当次 ready sub-channel。

remove/destroy 可以在 snapshot 形成后使 token stale；Client Library Executor 必须维护 token -> weak/high-level entity mapping 并处理 stale token。

### 6.12 GuardCondition

GuardCondition 使用 coalesced pending-trigger semantics：

- trigger before registration 可观察；
- 多次 trigger 在消费前可合并为一次 readiness；
- WaitSet 报告 ready 时消费该次 logical trigger；
- concurrent trigger/consume 不得丢新 trigger。

Fast DDS/native wake 只是 notification mechanism。实现必须保证 logical state 与 native trigger 的提交顺序不会产生 lost wakeup。

若 native GuardCondition trigger API 返回失败：

- `trigger()` 返回 `DDSError`；
- 本次 trigger 不得伪装成 success；
- 不通过固定周期 polling 隐藏 middleware error。

### 6.13 Event

Event 是 endpoint-bound persistent entity。

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

Event contract：

- Factory commit 前的 history 不 replay；
- 每个 Event 有独立 cursor；
- WaitSet ready 不消费；
- successful `Event::take()` 消费并推进 cursor；
- parent destroyed -> `ParentDestroyed`；
- Context shutdown 优先；
- 多个 Event 不得因为 Fast DDS destructive status query 而互相吞事件。


---

## 7. Discovery、Registry 与 Graph Public Runtime

### 7.1 TypeRegistry / TopicRegistry

每个 Context 必须有唯一 TypeRegistry。

TypeRegistry key：

```text
DDS wire type name
```

entry 至少保存：

```text
BindingIdentity
Fast DDS TypeSupport backing
registration phase
endpoint/topic reference state
```

规则：

```text
same wire name + same binding identity -> reuse
same wire name + different binding identity -> TypeMismatch
```

每个 Context 必须有唯一 TopicRegistry。

Topic identity：

```text
resolved DDS topic name
+
wire type name
```

同 resolved DDS topic name 不允许绑定不同 wire type；同名同 type 可以 reuse Topic backing。

Type/Topic unregister 只能在没有活跃 Topic/DataReader/DataWriter 依赖后进行；middleware delete/unregister failure 时以 memory safety 为优先，不提前释放仍可能被 Fast DDS 引用的 backing。

### 7.2 DiscoveryGraph authority

Context 内只有一个 DiscoveryGraph authority，统一服务：

```text
remote participant / endpoint discovery
matched/discovery metadata
Service availability
Action availability
GraphSnapshot
GraphEvent revision
Node/entity graph metadata
```

禁止：

```text
dclcpp GraphCache
+
dclpy GraphCache
+
DMW DiscoveryGraph
```

形成多个可能不同步的 graph authority。

DDS discovery 是异步的，因此 endpoint create/destroy 后 snapshot 可以短暂观察旧 state；但一个成功返回的 snapshot 本身必须内部自洽。

### 7.3 GraphRevision

```cpp
using GraphRevision = std::uint64_t;
```

只有 **public observable graph state 实际变化** 才允许 revision++。

以下不得增加 revision：

- duplicate `Added` callback；
- remove 不存在的 endpoint；
- received metadata 与现有 public view 完全相同；
- Fast DDS discovery callback 顺序变化但没有可观察 state change。

revision overflow 属于 terminal runtime error，不 silent wrap。

### 7.4 ROS2 Node graph metadata

仅凭 DDS Participant/Reader/Writer discovery 无法在“一 Context/Participant、多 logical Node”模型下可靠恢复 Node identity。因此：

- 禁止从 Participant name 推测 Node name/namespace；
- DMW 必须维护 local Node -> endpoint association；
- DCL peer 之间必须通过显式 participant/node/entity metadata protocol同步该 association；
- `RuntimeMode::ROS2` 若声明 ROS 2 Graph interoperability，则该 metadata wire contract 应与 ROS 2 `rmw_dds_common` 的 `ParticipantEntitiesInfo` / `ros_discovery_info` 路径兼容或通过互操作测试证明等价；
- DMW 可以自行实现所需 internal type/transport，不依赖 `rmw_dds_common` runtime package；
- metadata endpoint 对普通 public API 不可见为用户 Publisher/Subscriber，也不进入用户 Topic Graph 列表。

本条只规定可观察结果和 interoperability，具体 wire helper 在 `dmw_fastdds.md` 冻结。

### 7.5 Graph public values

```cpp
struct NodeGraphInfo
{
    std::string node_name;
    std::string node_namespace;
};
```

Topic endpoint：

```cpp
enum class EndpointKind
{
    Publisher,
    Subscriber
};

struct TopicEndpointInfo
{
    Gid endpoint_gid{};
    EndpointKind kind{EndpointKind::Publisher};
    std::string node_name;
    std::string node_namespace;
    std::string topic_name;
    std::string wire_type;
    Qos qos;
};
```

`TopicEndpointInfo::qos` 是 discovery-visible/effective QoS snapshot；无法可靠获得的字段使用 `SystemDefault`/unknown 约定，不伪造实际值。

Topic aggregate：

```cpp
struct TopicGraphInfo
{
    std::string topic_name;
    std::vector<std::string> wire_types;
    std::size_t publisher_count{0};
    std::size_t subscriber_count{0};
};
```

Service aggregate：

```cpp
struct ServiceGraphInfo
{
    std::string service_name;
    std::vector<std::string> request_wire_types;
    std::vector<std::string> response_wire_types;
    std::size_t client_candidate_count{0};
    std::size_t server_candidate_count{0};
};

enum class ServiceEndpointKind
{
    Client,
    Server
};

struct ServiceEndpointInfo
{
    ServiceEndpointKind kind{ServiceEndpointKind::Client};
    std::string node_name;
    std::string node_namespace;
    std::string service_name;
    Gid request_endpoint_gid{};
    Gid response_endpoint_gid{};
    std::string request_wire_type;
    std::string response_wire_type;
};
```

Action aggregate：

```cpp
struct ActionGraphInfo
{
    std::string action_name;
    std::size_t client_candidate_count{0};
    std::size_t server_candidate_count{0};
};

enum class ActionEndpointKind
{
    Client,
    Server
};

struct ActionEndpointInfo
{
    ActionEndpointKind kind{ActionEndpointKind::Client};
    std::string node_name;
    std::string node_namespace;
    std::string action_name;
    std::vector<Gid> endpoint_gids;
};
```

`endpoint_gids` 只用于表示一个已由 DMW 组合出的 logical Action candidate 关联的 constituent endpoint identities，不意味着 DDS 原生存在单一 Action object Gid。

candidate count 表示当前 discovery/metadata snapshot 中可组成对应 logical protocol 的 compatible candidate，不承诺存在一个可由 DDS 原生 discovery 唯一证明的 remote object identity。

### 7.6 GraphSnapshot

```cpp
struct GraphSnapshot
{
    GraphRevision revision{0};
    std::vector<NodeGraphInfo> nodes;
    std::vector<TopicGraphInfo> topics;
    std::vector<TopicEndpointInfo> topic_endpoints;
    std::vector<ServiceGraphInfo> services;
    std::vector<ServiceEndpointInfo> service_endpoints;
    std::vector<ActionGraphInfo> actions;
    std::vector<ActionEndpointInfo> action_endpoints;
};
```

单次 snapshot 必须来自一致 logical revision。实现不得：

- 一边复制 topics，一边放锁，再复制 services；
- 暴露内部 endpoint pointer / container iterator；
- 将 unresolved DDS transport name 当成 normalized DMW logical name；
- 跨 participant 拼接 Service/Action candidate。

`GraphSnapshot` 足以让 dclcpp/dclpy 提供：

```text
node names
node namespaces
topic names/types
service names/types
action names/types/candidates
publisher/subscriber count
topic endpoint info
by-node graph query
```

常见 query 的 filtering/normalization rules 应在 DMW 公共 helper 或 GraphSnapshot method 中共享，避免 C++/Python 产生不同结果；语言 wrapper 只负责容器/类型转换。

### 7.7 GraphEvent

```cpp
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

GraphEvent 创建时 cursor 初始化为当前 revision，不 replay 之前 discovery history。

readiness：

```text
current graph revision > event cursor
```

WaitSet 报告 GraphEvent ready 不推进 cursor；只有 successful `take()` 推进 cursor。

因此 GraphEvent 是 level-triggered change notification，不是计数 semaphore。

### 7.8 Service/Action candidate consistency

Service candidate 至少要求同一 remote participant 存在 compatible：

```text
request reader
response writer
```

Action server candidate 至少要求同一 remote participant 存在 compatible：

```text
SendGoal server endpoints
CancelGoal server endpoints
GetResult server endpoints
Feedback writer
Status writer
```

如果 ROS2 graph metadata 能提供 endpoint-to-node association，candidate composition 应进一步保持 metadata-consistent；若无法证明 endpoint 属于同一具体 service/action instance，public API 使用 candidate 语义，不夸大为唯一 remote object identification。

---

## 8. Action Common Runtime

### 8.1 架构边界

Action wire/runtime 由：

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
- Action logical naming / remapping；
- Action availability；
- aggregate readiness；
- GoalId / GoalInfo；
- Goal FSM；
- duplicate GoalId protection；
- cancel candidate selection；
- terminal goal lifetime；
- pending GetResult RequestId state；
- result expiry deadline；
- status snapshot authority；
- ROS 2 Action endpoint QoS / wire mapping；
- Context shutdown / teardown。

DMW 不负责：

- typed Goal/Result/Feedback C++ class；
- Python Action class；
- user goal/cancel/feedback/result callback；
- `std::future` / Python Future；
- typed terminal result payload copy/cache；
- Executor scheduling。

DMW 保存“哪个 Goal 是什么 common state、哪些 result request 正在等待、何时过期”；任意 ActionT 的 typed result object 如何构造和长期保存仍属于 Client Library。

### 8.2 Action endpoint logical names

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

ROS2 mode 最终得到 compatible `rq/rr/rt` DDS names；Client Library 不重复拼 transport prefix。

### 8.3 GoalId / GoalInfo

```cpp
struct GoalId
{
    static constexpr std::size_t Size = 16;
    std::array<std::uint8_t, Size> data{};
};
```

必须提供 equality/hash。

DMW 不要求 GoalId 由 DMW 生成；dclcpp/dclpy 可以提供 UUID convenience，进入 DMW 后统一转换为 GoalId。

```cpp
struct GoalInfo
{
    GoalId goal_id{};
    std::chrono::nanoseconds accepted_stamp{0};
};
```

`accepted_stamp` 属于 Action protocol time domain，用于 cancel-before semantics。它不是 result-expiry steady deadline。

因为 accepted SendGoal response 本身包含 typed timestamp，而 DMW 不解析任意 generated response layout，所以 Client Library/type adapter 仍负责构造 GoalInfo 与 accepted response；但是时间来源必须使用 DMW Clock common runtime，而不是 dclcpp/dclpy 各自实现一套 clock semantics。

### 8.4 GoalState / GoalEvent / GoalAcceptMode

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

状态转换：

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

Terminal：

```text
Succeeded
Canceled
Aborted
```

`GoalAcceptMode::Defer`：commit accepted 后保持 Accepted。

`GoalAcceptMode::Execute`：同一 accept transaction commit 后继续 `Accepted -> Executing`。

### 8.5 Goal registry authority

每个 ActionServer 唯一维护：

```text
GoalId
 -> GoalInfo
 -> GoalState
 -> terminal runtime timestamp（若 terminal）
 -> pending GetResult RequestIds
```

规则：

- duplicate GoalId accept -> `AlreadyExists`；
- dclcpp/dclpy 不建立第二套 Goal FSM；
- 同 GoalId 并发 transition 线性化；
- terminal state 之后非法 transition -> `InvalidState`；
- status snapshot 来自一致 registry snapshot。

### 8.6 ActionClientOptions / ActionServerOptions

```cpp
struct ActionClientOptions
{
    Qos goal_service_qos{Qos::ros2_services_default()};
    Qos cancel_service_qos{Qos::ros2_services_default()};
    Qos result_service_qos{Qos::ros2_services_default()};
    Qos feedback_topic_qos{Qos::ros2_default()};
    Qos status_topic_qos{Qos::ros2_action_status_default()};
};

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

10 s 与 ROS 2 Jazzy `rcl_action` default 对齐；authority 位于 DMW options/default，不在 dclcpp/dclpy 再定义。

### 8.7 ActionClient public API

ActionClient 是 aggregate primitive，不向上暴露内部三个 Client 与两个 Subscriber：

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

RequestId -> Future mapping留 Client Library：

```text
Goal RequestId   -> std::promise / Python Future
Cancel RequestId -> std::promise / Python Future
Result RequestId -> std::promise / Python Future
```

DMW 不尝试统一语言 Future abstraction。

### 8.8 Action readiness

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

ActionServer：

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

一个 ActionClient/ActionServer 在 public WaitSet 中各只占一个 registration token。

WaitResult 应保存当次 aggregate sub-channel readiness snapshot；`readiness()` 是 current-state observer，不作为 Executor 唯一依据。

### 8.9 Action availability

Action server available 表示当前 DiscoveryGraph 中存在一个 participant-consistent、name/type/QoS-compatible endpoint composition candidate：

```text
SendGoal Server
CancelGoal Server
GetResult Server
Feedback Publisher
Status Publisher
```

DMW 不跨 participant 拼接五个 endpoint。

在 ROS2 graph metadata 可用时，composition 应进一步保持 node/entity metadata consistent。

`wait_for_server()`：

- 与 DiscoveryGraph 同一 authority；
- 不轮询五个 count；
- timeout -> `success + false`；
- Context shutdown -> `ContextShutdown`。

### 8.10 ActionServer transport API

```cpp
class ActionServer
{
public:
    ~ActionServer() noexcept;

    Result<bool> read_goal_request(void* request, RequestId& request_id);

    // raw path 只用于 rejected/non-accepted response。
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

传输 message 与 common Goal metadata 分离是有意设计；DMW 不解析任意 ActionT field layout。

### 8.11 Goal accept transaction boundary

标准 flow：

```text
read_goal_request(raw request, RequestId)
        ↓
Client Library 从 typed request 提取 GoalId/GoalInfo
并使用 DMW Clock 构造 protocol timestamp
        ↓
user goal callback
        ├── Reject
        │     ↓
        │  write_goal_response(request_id, rejected_response)
        │
        └── AcceptAndDefer / AcceptAndExecute
              ↓
          accept_goal(
              request_id,
              goal_info,
              accepted_response,
              GoalAcceptMode)
```

`accept_goal()` 是 DMW public transaction boundary，不允许 dclcpp/dclpy 拆成“先发送 accepted response，再单独注册 Goal”。

public atomicity contract：

```text
accepted response becomes observable on wire
    =>
corresponding GoalRecord is committed locally
```

以及：

```text
accept_goal() fails before successful response write
    =>
no public accepted GoalRecord remains
```

write failure 后不得留下 half-accepted Goal；同 GoalId concurrent accept exactly one transaction succeeds。

为实现上述 contract，Fast DDS backend 必须在 response write 前准备完成成功后 local commit 所需资源，使 response write 成功后的本地 publish/commit 路径不再发生可恢复 allocation/registry failure。具体 reservation、lock、rollback 与 no-fail commit 算法属于 `dmw_fastdds.md` implementation contract，不在本 public 文档绑定具体容器实现。

### 8.12 CancelGoalCriteria

```cpp
struct CancelGoalCriteria
{
    GoalId goal_id{};                  // all-zero = wildcard
    std::chrono::nanoseconds stamp{0}; // zero = no time bound for exact-id form
};
```

selection 与 ROS 2 Action cancel semantics 对齐：

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

只选择当前 Accepted / Executing 的 cancelable goal。

`select_cancel_goals()` 不执行 user cancel callback，也不自动改变 state。

用户 callback 接受某 candidate 后，Client Library 调用：

```text
update_goal_state(goal_id, GoalEvent::CancelGoal)
```

由 DMW 完成 `Accepted/Executing -> Canceling`。

### 8.13 Result request lifecycle

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
- goal active -> 保存 RequestId，返回 Pending；
- goal terminal -> Terminal，不重复保存。

GoalTransition：

```cpp
struct GoalTransition
{
    GoalState previous{GoalState::Unknown};
    GoalState current{GoalState::Unknown};
    bool became_terminal{false};
};
```

进入 terminal state 时，DMW 记录内部 monotonic runtime timestamp，用于 result expiry scheduling。它与 GoalInfo.accepted_stamp 的 protocol clock domain 分离。

terminal flow：

1. Client Library 保存 typed result payload；
2. DMW transition 返回 terminal information；
3. `take_pending_result_requests()` 移交此前等待 RequestId；
4. Client Library 以 typed result构造 response 并调用 DMW write；
5. terminal 后新 GetResult request -> Terminal，Client Library从 typed cache构造 response；
6. result_timeout 到期后 DMW `take_expired_goals()` 返回 GoalId；
7. Client Library 删除对应 typed result cache。

DMW 不为了缓存 arbitrary `void*` result object 引入 reflection、generic clone ABI 或 serialized-message public subsystem。

### 8.14 status common state

```cpp
struct GoalStatusInfo
{
    GoalInfo goal_info;
    GoalState state{GoalState::Unknown};
};
```

`status_snapshot()` 返回当前未 expired Goal 的一致 snapshot。

DMW 是 GoalState authority；Client Library 只负责把 snapshot 装配为实际 typed status message，再调用 `publish_status()`。

状态变化后何时触发 user-facing publish policy 可以由 Client Library wrapper自动安排，但不得复制第二套 Goal FSM。

### 8.15 Action result expiry 与 WaitSet

result expiry 使用 internal monotonic/steady runtime deadline，不使用 GoalInfo accepted protocol stamp。

- 不创建 background expiry thread；
- ActionServer 注册 WaitSet 时 earliest expiry 参与 wait deadline；
- 未注册 WaitSet 时 Goal registry operation / `take_expired_goals()` 可以 lazy prune；
- expiry ready 通过 ActionServer aggregate waitable 的 `goal_expired` detail 报告。


---

## 9. Parameter Common Runtime

### 9.1 设计目标

Parameters 是典型的 language-neutral state/validation capability。ROS 2 中大量 parameter store、descriptor validation、range checking、override handling 在 rclcpp/rclpy 分别实现；DCL 不复制这一结构。

DMW V1 负责：

```text
Parameter type/value representation
Parameter descriptor
Node-local parameter store
name/type/range validation
declare / undeclare
get / has / list
atomic store update
parameter override resolution
change-set/event delta generation
```

Client Library 负责：

```text
C++/Python user parameter callbacks
callback handle/lifetime
callback invocation order
language exception mapping
typed standard parameter service/message assembly
parameter event typed message publish wrapper
```

DMW 不执行 pre/on/post user callback。

### 9.2 ParameterType / ParameterValue

V1 common types与 ROS 2 基础 parameter value set 对齐：

```cpp
enum class ParameterType
{
    NotSet,
    Bool,
    Integer,
    Double,
    String,
    ByteArray,
    BoolArray,
    IntegerArray,
    DoubleArray,
    StringArray
};
```

`ParameterValue` 不要求 public 暴露 `std::variant`。推荐使用显式 type + checked accessor：

```cpp
class ParameterValue
{
public:
    ParameterType type() const noexcept;

    bool as_bool() const;
    std::int64_t as_integer() const;
    double as_double() const;
    std::string_view as_string() const;

    const std::vector<std::uint8_t>& as_byte_array() const;
    const std::vector<bool>& as_bool_array() const;
    const std::vector<std::int64_t>& as_integer_array() const;
    const std::vector<double>& as_double_array() const;
    const std::vector<std::string>& as_string_array() const;
};
```

wrong-type accessor 属于 programming precondition violation；实现可以 `std::terminate()` 或使用与项目其他 value type 一致的强 precondition 机制，不把它伪装成 runtime middleware Error。

Parameter：

```cpp
struct Parameter
{
    std::string name;
    ParameterValue value;
};
```

### 9.3 ParameterDescriptor

至少支持：

```cpp
struct IntegerRange
{
    std::int64_t from_value{0};
    std::int64_t to_value{0};
    std::uint64_t step{0};
};

struct FloatingPointRange
{
    double from_value{0.0};
    double to_value{0.0};
    double step{0.0};
};

struct ParameterDescriptor
{
    std::string description;
    std::string additional_constraints;
    bool read_only{false};
    bool dynamic_typing{false};
    std::vector<IntegerRange> integer_ranges;
    std::vector<FloatingPointRange> floating_point_ranges;
};
```

V1 validation authority 位于 DMW：

- parameter name validation；
- already declared / not declared；
- fixed-type vs dynamic-type；
- read-only；
- integer range/step；
- floating range/step；
- duplicate names in one atomic request；
- undeclared policy。

Client Library 不复制这些检查。

### 9.4 Node parameter policy

`NodeOptions::allow_undeclared_parameters` 决定 common ParameterStore 是否允许隐式出现未声明参数。

`automatically_declare_parameters_from_overrides` 不进入 DMW NodeOptions，因为它涉及 Client Library 用户 API 的 declare timing/presentation；Client Library 可以读取 DMW override table并按语言层 policy显式 declare。

DMW ParameterStore 始终维护真实 declared-state authority。

### 9.5 Parameter API

Parameter common state直接归 Node，不创建第二个 network entity。

概念 API：

```cpp
Result<Parameter>
Node::declare_parameter(
    std::string_view name,
    const ParameterValue& default_value,
    const ParameterDescriptor& descriptor,
    bool ignore_override = false);

Result<void>
Node::undeclare_parameter(std::string_view name);

Result<bool>
Node::has_parameter(std::string_view name) const;

Result<Parameter>
Node::get_parameter(std::string_view name) const;

Result<std::vector<Parameter>>
Node::get_parameters(const std::vector<std::string>& names) const;

Result<ParameterDescriptor>
Node::describe_parameter(std::string_view name) const;

Result<ParameterListResult>
Node::list_parameters(
    const std::vector<std::string>& prefixes,
    std::size_t depth) const;
```

Mutation：

```cpp
Result<void>
Node::validate_parameters(
    const std::vector<Parameter>& parameters) const;

Result<ParameterChangeSet>
Node::set_parameters_atomically(
    const std::vector<Parameter>& parameters);
```

`set_parameters_atomically()` 不调用 user callback。它只对 common store执行完整 DMW validation 与 atomic commit。

### 9.6 Client Library callback boundary

典型 dclcpp/dclpy flow：

```text
prepare typed Parameter list
        ↓
DMW validate_parameters()
        ↓
Client Library pre/on-set user callbacks
        ↓
callbacks reject -> no DMW state change
        │
        └── accept
              ↓
DMW set_parameters_atomically()
              ↓
ParameterChangeSet
              ↓
Client Library post-set callback
+ typed parameter-event publish
```

`set_parameters_atomically()` 在 commit 时必须重新检查 store state；如果 callback期间发生 concurrent mutation 使之前 validation 不再成立，则返回明确 `Busy` / `InvalidState`，不得基于 stale validation 强行 commit。

Client Library 可以在同一 Node 上串行化 parameter mutation，以获得更简单、稳定的 callback semantics；这种 callback mutex 属于语言层，不进入 DMW。

### 9.7 Parameter overrides

Arguments parser 解析得到的 override 由 NodeState 保存为 immutable initial override table。

`declare_parameter(..., ignore_override=false)`：

```text
matching override exists -> effective initial value = override
otherwise                -> effective initial value = default
```

override value 仍必须通过 descriptor/type/range validation。

DMW 提供只读 override query，供 dclcpp/dclpy 实现“automatically declare parameters from overrides”等 convenience，但上层不得重新解析 parameter CLI/YAML 语义形成第二份 authority。

如果 V1 支持 parameter file，文件解析后的 canonical override map必须在 DMW/共享 parser 层形成；dclcpp/dclpy 不各自实现 YAML interpretation。

### 9.8 ParameterChangeSet

```cpp
struct ParameterChangeSet
{
    std::vector<Parameter> new_parameters;
    std::vector<Parameter> changed_parameters;
    std::vector<Parameter> deleted_parameters;
};
```

它描述一次成功 atomic commit 的 change delta，供语言层：

- post-set callback；
- typed parameter event message construction；
- diagnostics。

DMW 不直接依赖 `rcl_interfaces/msg/ParameterEvent` generated type。

### 9.9 Parameter services / event transport boundary

DCL V1 需要能够在 dclcpp/dclpy 提供熟悉的 parameter service/event surface，但 DMW 不把 generated standard-interface object layout写入 common runtime。

因此职责固定为：

```text
DMW
    ParameterStore + validation + override + delta authority

Client Library / standard-interface adapter
    Get/Set/List/Describe service typed messages
    user parameter callbacks
    ParameterEvent typed message
```

如果后续 DCL 建立统一 standard-interface native binding，可再将 parameter service endpoint composition下沉成 DMW aggregate runtime；在此之前不得复制 parameter validation/store本身。

---

## 10. 并发、生命周期与 teardown

### 10.1 Fast DDS listener boundary

Fast DDS listener 只允许：

```text
capture callback data
update synchronized runtime state
update DiscoveryGraph
update matched/event state
mark readiness
trigger native/internal wake
record diagnostic
return
```

不得：

```text
invoke dclcpp user callback
invoke Python
acquire Python GIL for user work
fulfill arbitrary user promise as scheduling mechanism
run Executor callback
block waiting for application work
```

### 10.2 Public operation concurrency

以下 operation 设计为可以从不同 application threads调用：

```text
Publisher::write / actual_qos / matched count
Subscriber::read / actual_qos / matched count
Client::write_request / read_response
Client::service_is_available / wait_for_service
Server::read_request
Server::write_response(different RequestId)
Clock observers / ROS-time update
Timer observers / reset / cancel / exchange / consume
ActionClient independent transport operations
ActionServer Goal operations under internal synchronization
GuardCondition::trigger
Event::take
GraphEvent::take
Context graph snapshot/query
Node parameter reads
Node parameter atomic mutation under store synchronization
```

同一 public facade 的 destructor 与 ordinary operation 无同步并发由 caller 禁止；WaitSet registered waitable destruction是专门例外。

### 10.3 Context operation guard

Context Active operation应通过统一 OperationGuard/active-operation accounting实现：

```text
enter operation
    ↓
verify Active
    ↓
active_operation_count++
    ↓
perform runtime/middleware work
    ↓
active_operation_count--
```

shutdown linearization 后：

- 新 operation不得进入 middleware；
- already entered operation要么正常完成，要么被显式 shutdown wake打断；
- shutdown等待必须 drain 需要保证 teardown safety 的 operation；
- destructor不得 UAF/访问 half-destroyed Participant/entity。

### 10.4 Clock / Timer concurrency

Clock state更新具有自己的同步域。

Ros Clock update必须保证：

```text
clock value/state commit
    ↓
collect dependent Timer/WaitSet wake targets
    ↓
release clock lock
    ↓
trigger wake
```

Timer scheduling state使用一个同步域；并发 `consume()` 对同一 deadline最多一个返回 true。

`reset/cancel/exchange_period/consume` 线性化，不允许 torn：

```text
period
last_call
next_call
canceled
```

state。

### 10.5 Server concurrency

Server pending state至少区分：

```text
reserved
Pending
Responding
```

并发规则：

- capacity reservation 与 DDS destructive take同一 logical transaction；
- same RequestId只能有一个 response进入 Responding；
- different RequestId可以并发 write，只要 Fast DDS writer/thread-safety contract满足；
- response write failure回 Pending；
- destroy/shutdown必须中止新的 response admission，并安全 drain已进入 operation。

### 10.6 Action concurrency

ActionServer GoalRegistry / pending result table / expiry state使用同一逻辑 synchronization domain，或提供可证明等价的细粒度协议。

同 GoalId：

- concurrent accept exactly one成功；
- concurrent state transition按一个线性化序列执行；
- terminal后 transition重新按新 state验证；
- accept transaction与同一 ActionServer的 SendGoal response / Goal registry mutation保持 atomic observable semantics。

status snapshot来自一致 state snapshot。

### 10.7 Graph concurrency

DiscoveryGraph callback/update：

```text
normalize incoming record
    ↓
lock graph state
    ↓
compare old/new public-observable state
    ↓
commit real change + revision++
    ↓
collect notification targets
    ↓
unlock
    ↓
trigger wait/graph notification
```

不得在持有 graph mutex时执行可能获取 WaitSet topology mutex的长 notification path，也不得在持 graph lock时调用 user code。

GraphSnapshot 与 discovery update并发时：

- snapshot要么观察 change前完整 state；
- 要么观察 change后完整 state；
- 不允许 mixed partial snapshot。

### 10.8 Parameter concurrency

ParameterStore mutation atomic；read operation得到一个完整 committed state。

同 Node concurrent mutation：

- DMW store保证线性化；
- dclcpp/dclpy可以额外串行化 callback transaction；
- DMW不持有 unknown language user callback；
- callback后 commit发现store revision变化时不得 silent overwrite stale state。

### 10.9 WaitSet concurrency

WaitSet：

- 同一实例最多一个 active `wait()`；
- add/remove/auto-detach可以与 active wait并发；
- topology generation改变必须wake active wait；
- wake后继续使用原始 finite deadline；
- public waitable destruction自动detach；
- WaitSet不长期依赖raw public facade pointer。

### 10.10 Listener / backing lifetime

Listener backing至少存活到：

1. 对应 DDS entity不再可能开始新 callback；
2. 已进入 callback全部退出。

建议：

```text
closing flag
+
in-flight callback count / shared lifetime guard
```

teardown：

```text
mark closing
    ↓
prevent/detach new callback admission
    ↓
delete/detach DDS entity/listener
    ↓
drain callbacks already entered
    ↓
release listener backing
```

### 10.11 Endpoint teardown

一般 endpoint：

```text
mark Closing
    ↓
prevent new public operation
    ↓
auto-detach WaitSet registration
    ↓
detach/disable listener
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

Client/Server/Action 先关闭 aggregate public state，再逆创建顺序清理 internal endpoints；中间某个 delete failure不得阻止其余可证明安全的 cleanup。

### 10.12 Context final teardown

最后一个 child backing引用释放后：

```text
stop discovery/listener admission
    ↓
wake/drain runtime waits
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

如果 Fast DDS delete 返回 failure，且实现无法证明 middleware 不再引用某 backing：

> memory safety 优先于强制 free。

允许把必要 backing保留到 parent/Participant teardown；最终 Participant teardown仍失败时，可以进入 process-lifetime conservative retention并记录 diagnostic。

不得为了“无 leak”在 uncertain ownership 下制造 UAF。

Conservative retention 应具有 diagnostic counters / reason，便于发现长期 churn 导致的 RSS growth。

### 10.13 Locking / deadlock 原则

不要建立覆盖全部 DMW 的 global mutex。

主要 synchronization domains：

```text
Context lifecycle
Clock state
Arguments/immutable config
Type/Topic registry
DiscoveryGraph
WaitSet topology
endpoint operation state
Server pending requests
Timer state
Action GoalRegistry
ParameterStore
EventSource
```

Fast DDS listener callback：

- internal lock持有时间尽可能短；
- 不持 graph/endpoint lock调用 user code；
- state commit后释放lock，再wake其他 domain。

避免在持有 DiscoveryGraph / WaitSet topology / GoalRegistry mutex时调用可能同步触发 listener/drain 的 Fast DDS delete API。

### 10.14 Runtime error / exception boundary

Fast DDS ReturnCode -> DMW Error必须集中转换，不允许不同 endpoint各写一套不一致 mapping。

common intent：

```text
RETCODE_TIMEOUT          -> Timeout
RETCODE_OUT_OF_RESOURCES -> ResourceExhausted
invalid/precondition     -> InvalidArgument / InvalidState（可区分时）
other middleware failure -> DDSError
```

某些 Fast DDS API返回 bool；`false` 只有在 public contract明确表示“no data / no change”的情况下才是 non-error，否则映射为对应 middleware failure。

`std::bad_alloc` 允许传播；不得 catch 后随意映射为 `ResourceExhausted` / `DDSError`。

析构路径 `noexcept`，不得传播 exception；unexpected exception只能记录 diagnostic并继续 best-effort safe teardown。


---

## 11. ROS 2 Compatibility、实现边界、验收与 Frozen Invariants

### 11.1 ROS 2 Fast DDS compatibility scope

DMW `RuntimeMode::ROS2` 的 V1 兼容性分成两层：

```text
Data-plane compatibility
    Topic / Service / Action wire interoperability

Graph/control-plane compatibility
    Node/entity graph metadata interoperability
    name/type/endpoint introspection where V1 claims support
```

不允许再用“wire compatibility != graph compatibility”作为长期缺失 Node Graph 的理由。新的 V1 要求基础 Graph capability完整，因此 ROS2 mode应实现并验证其声明的 graph metadata compatibility。

仍然不保证：

- DCL完全复制 ROS 2 daemon/tooling internals；
- 所有 ROS 2 distribution私有实现细节；
- component/lifecycle/type-description等未纳入V1的高层 protocol。

### 11.2 Topic / Service / Action naming

ROS Topic：

```text
logical /foo
    -> rt/foo
```

ROS Service `/add_two_ints`：

```text
request -> rq/add_two_intsRequest
reply   -> rr/add_two_intsReply
```

ROS Action `/move`：

```text
logical:
/move/_action/send_goal
/move/_action/cancel_goal
/move/_action/get_result
/move/_action/feedback
/move/_action/status
```

resolved：

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

### 11.3 ROS Action QoS

默认：

```text
SendGoal service -> ros2_services_default
CancelGoal service -> ros2_services_default
GetResult service -> ros2_services_default
Feedback topic -> ros2_default
Status topic -> ros2_action_status_default
```

Status profile：

```text
KeepLast(1)
Reliable
TransientLocal
```

### 11.4 ROS Graph metadata

`RuntimeMode::ROS2` 的 local Node/entity metadata必须能表达：

```text
Participant identity
Node name/namespace
Node -> publisher GIDs
Node -> subscriber GIDs
```

Service/Action最终仍由其 underlying endpoint identity和logical naming组合得到。

Jazzy参考路径是 `rmw_dds_common::ParticipantEntitiesInfo` 在 `ros_discovery_info` 上传播。DMW不依赖 `rmw_dds_common` runtime library，但ROS2 mode实现应以互操作测试确认：

- ROS 2 peer能够观察DCL Node/entity association；
- DMW能够接收/解释ROS 2 peer graph metadata；
- local GraphSnapshot不从Participant name猜Node identity；
- duplicate metadata不产生无意义GraphRevision bump。

### 11.5 Public target / headers

Runtime target：

```text
dmw::dmw
```

Fast DDS generated-type binding：

```text
dmw::fastdds_binding
```

Clock/Arguments/Timer/Graph/Action/Parameter common runtime均属于 `dmw::dmw`；不新增：

```text
rcl-equivalent package
runtime_core library
action_runtime library
parameter_runtime library
generic middleware backend framework
```

普通 public header不得出现：

```text
eprosima::fastdds::*
eprosima::fastcdr::*
```

Fast DDS TypeSupport integration只允许出现在明确 `dmw/fastdds/*` boundary。

推荐新增/整理 public files：

```text
include/dmw/
├── arguments.hpp
├── clock.hpp
├── time.hpp
├── timer.hpp
├── timer_info.hpp
├── action_type.hpp
├── action_client.hpp
├── action_server.hpp
├── action_common.hpp
├── graph.hpp
├── graph_event.hpp
├── parameter.hpp
├── parameter_descriptor.hpp
├── parameter_change_set.hpp
└── qos_compatibility.hpp
```

保持扁平、按真实职责拆分；不要为每个小 value创建深层framework目录。

### 11.6 当前实现到 V1 的收敛顺序

为了避免同时大规模改动，建议按以下五个阶段实现：

#### 阶段 1：Foundation contract convergence

先修复已经实现部分与规范冲突：

- Result/Error `std::bad_alloc` 分类统一；
- Server response-reader deadline删除 hard-coded `100 ms`，从 effective response-writer QoS 派生；
- GuardCondition native trigger failure从 `trigger()` 正确返回；
- DiscoveryGraph revision仅在真实 observable state change时增长；
- 补齐 QoS common profiles；
- 补 actual QoS / compatibility / ACK / liveliness基础API；
- RuntimeMode public注释去掉只指向 Humble 的旧表述。

#### 阶段 2：Foundation expansion

实现：

- Arguments / Remapping；
- Clock / Time；
- Node FQN / resolved naming；
- Timer改为Clock-aware；
- Parameter common store / validation / overrides。

#### 阶段 3：Graph runtime

- DiscoveryGraph record/revision收敛；
- DCL node/entity metadata；
- ROS2 `ros_discovery_info` compatibility path；
- Node/Topic/Endpoint/Service/Action GraphSnapshot；
- GraphEvent；
- by-node/common graph query helper。

#### 阶段 4：WaitSet completion

- Timer/Clock-aware deadline；
- GraphEvent；
- Action aggregate registration skeleton；
- WaitResult detail snapshot；
- native wait/control-guard race regression。

#### 阶段 5：Action common runtime + interoperability

- ActionType；
- ActionClient/Server五endpoint transaction；
- Goal FSM；
- accept transaction；
- cancel selection；
- result lifecycle/expiry；
- status snapshot；
- ROS2 Action bidirectional interoperability。

Parameters可以在阶段2完成common store后由 dclcpp/dclpy并行构建typed user API，不阻塞Action。

### 11.7 Foundation regression

必须持续覆盖：

```text
Result/Error
Context create/shutdown/concurrent shutdown
Node name/namespace/FQN/remapping
MessageType/TypeRegistry
TopicRegistry
QoS mapping/actual/compatibility
Topic pub/sub/matched count/events
Service request/response/multi-client
Service availability/wait
WaitSet add/remove/wait/teardown
GuardCondition race
Event cursor/readiness
ASan/UBSan
targeted TSan
```

### 11.8 Clock / Arguments / Parameter tests

Clock：

```text
System/Steady/Ros now
Ros override enable/disable
Ros time set
invalid clock-type operation
clock facade destroyed before Timer
clock update wakes Timer wait
backward/forward ROS time changes do not tear Timer state
Context shutdown
```

Arguments/Remapping：

```text
node name remap
namespace remap
topic/service/action remap
global + node-local precedence
rule scoped to node
invalid remap
unparsed arguments preservation
parameter override parsing
same result through dclcpp and dclpy bindings
```

Parameter：

```text
declare / duplicate declare
undeclare
fixed/dynamic typing
read-only
integer/floating range
step validation
override value
ignore_override
atomic multi-parameter set
failed set leaves all values unchanged
concurrent mutation detection
ParameterChangeSet new/changed/deleted
list depth/prefix
allow_undeclared policy
```

### 11.9 QoS tests

为所有 common profiles建立 golden tests：

```text
ros2_default
ros2_services_default
ros2_sensor_data
ros2_parameters
ros2_parameter_events
ros2_action_status_default
```

并覆盖：

```text
SystemDefault -> effective actual QoS
actual_qos writer/reader
compatibility Compatible/Warning/Incompatible
wait_for_all_acked success/timeout/shutdown
assert_liveliness supported/invalid cases
```

禁止 dclcpp/dclpy 再保存独立 preset 数值。

### 11.10 Timer / WaitSet tests

Timer：

```text
period < 0 -> InvalidArgument
period == 0 always-ready
autostart true/false
ready before/after deadline
consume false leaves output unchanged
periodic consume
missed cycles skip/re-align
cancel
reset
exchange_period
concurrent consume exactly-once per deadline
System/Steady Clock
Ros Clock override/pause/jump
Timer + finite WaitSet
Timer + infinite WaitSet
Clock update while WaitSet blocking
Timer reset/cancel while WaitSet blocking
Timer destruction while registered
Context shutdown
no callback thread
```

WaitSet：

```text
poll/finite/infinite
no fixed periodic slice
pre-existing data ready before native wait
control wake
add/remove while waiting
original finite deadline preserved
Timer deadline wakes infinite wait
Action expiry wakes wait
GraphEvent wakes wait
GuardCondition trigger/consume race
waitable destroy while waiting
second concurrent wait -> Busy
Action readiness detail snapshot
```

### 11.11 Graph tests

synthetic discovery + real Fast DDS discovery两层覆盖：

```text
participant add/remove
reader/writer add/change/remove
no-op revision suppression
local Node add/remove
Node -> endpoint association
remote ROS2 ParticipantEntitiesInfo compatibility
topic names/types
endpoint info + QoS
service participant-consistent candidate
action five-endpoint candidate
GraphSnapshot consistency
GraphEvent no pre-creation replay
GraphEvent level-triggered until take
concurrent snapshot + discovery update
Context shutdown
GraphEvent destruction while registered
```

### 11.12 Action tests

Goal FSM / accept transaction：

```text
reject response does not create GoalRecord
duplicate GoalId -> AlreadyExists
accepted response write failure -> no public GoalRecord
accepted response write success -> GoalRecord committed
Defer -> Accepted
Execute -> Executing
Accepted -> Executing
Accepted -> Canceling
Executing -> Canceling
Executing -> Succeeded/Aborted
Canceling -> Succeeded/Aborted/Canceled
terminal transition rejection
multiple goals
concurrent accept same GoalId -> exactly one succeeds
concurrent transition same GoalId
status snapshot consistency
```

Cancel：

```text
four GoalId/timestamp selection forms
only cancelable goals selected
unknown exact id
terminal goal
all-goal cancel
timestamp boundary equal
user reject leaves state unchanged
accepted cancel -> Canceling
```

Result：

```text
unknown goal -> UnknownGoal
active goal -> pending RequestId
multiple result requests same goal
terminal transition exposes pending requests
new request after terminal -> Terminal
result timeout expiry
expired goal removed
typed result payload remains language-layer authority
```

Aggregate readiness：

```text
ActionClient one registration -> five sub-channel detail bits
ActionServer one registration -> goal/cancel/result/expiry detail bits
```

### 11.13 Interoperability matrix

Jazzy / Fast DDS 2.14.x：

```text
build
unit/integration tests
Topic bidirectional interop
Service bidirectional interop
Action bidirectional interop
Graph metadata/node discovery interop
QoS golden/actual tests
shutdown/teardown
ASan/UBSan
selected TSan
```

Humble / Fast DDS 2.6.x：

```text
source build
foundation tests
Topic interop
Service interop
Graph compatibility path
Action compatibility once implementation lands
```

测试 network path建议显式使用 UDPv4，避免同机 SHM隐藏 wire compatibility问题。

### 11.14 Frozen Architecture Invariants

以下条目是 V1 architecture review index。若简述与正文冲突，以正文为准并修正文档。

**Architecture**

1. DMW 使用 C++17。
2. DMW Fast DDS-only。
3. 普通 runtime public API non-template/type-erased。
4. 普通 public header 不暴露 Fast DDS / Fast CDR type。
5. DMW 不提供 C API、多-middleware plugin或独立 `rcl` package。
6. Fast DDS 2.14.x 与 `rmw_fastrtps` Jazzy是平等主要参考。
7. `rcl` / `rcl_action`用于 common-runtime职责参考。
8. `rclcpp` / `rclpy`用于语言层边界参考。
9. Humble / Fast DDS 2.6.x是兼容性验证目标，不决定新 public design。
10. Component / Composition 不进入整个 DCL V1。

**Ownership / lifecycle**

11. Resource factory返回 `Result<std::unique_ptr<T>>`。
12. Resource non-copyable/non-movable。
13. 创建transactional，不返回half-valid object。
14. Context是runtime root。
15. Node是logical entity，不等于Participant。
16. Context facade可以先于child facade析构，internal backing保证teardown safety。
17. Listener不执行user callback。
18. delete failure时memory safety优先于强制free。

**Client-Library boundary**

19. Future/Promise/Task不下沉。
20. Pending Future registry不下沉。
21. user callback不下沉。
22. Executor/CallbackGroup/ThreadPool不下沉。
23. Python GIL/asyncio留dclpy。
24. C++ typed template API留dclcpp。
25. typed Action result payload cache留Client Library。
26. user parameter callbacks留Client Library。

**Foundation**

27. Clock/Time common semantics只有DMW一个authority。
28. Arguments/Remapping/name resolution只有DMW一个authority。
29. dclcpp/dclpy不得各自解析第二套remap/parameter override semantics。
30. TypeRegistry/TopicRegistry属于Context mandatory authority。
31. `std::bad_alloc`允许传播，不映射成ordinary Error。

**QoS / Topic / Service**

32. QoS common preset数值只有DMW一个authority。
33. Service default = KeepLast(10)/Reliable/Volatile。
34. SensorData = KeepLast(5)/BestEffort/Volatile。
35. Parameter/ParameterEvent = KeepLast(1000)/Reliable/Volatile。
36. Action Status = KeepLast(1)/Reliable/TransientLocal。
37. SystemDefault与ros2_default语义不同。
38. actual QoS与compatibility属于DMW。
39. Service correlation属于DMW。
40. Server bounded pending RequestId FSM属于DMW。
41. Service availability/wait属于DMW。
42. Service candidate不跨participant组合。
43. Future completion属于Client Library。
44. response-reader wait deadline从effective writer QoS派生，不hard-code固定100 ms。

**Timer / Wait**

45. Timer属于DMW。
46. Timer绑定DMW Clock，不固定为单一steady clock public model。
47. Timer不执行user callback。
48. Timer readiness level-triggered直到consume。
49. missed cycles跳过并重新对齐周期网格。
50. Timer直接进入DMW WaitSet。
51. WaitSet在DMW；Executor在Client Library。
52. Finite timeout使用单一steady absolute user deadline。
53. topology/control wake不重置user timeout。
54. 正常路径不使用固定周期polling slice。
55. WaitSet不拥有registered waitable。
56. registered waitable destructor自动detach。
57. Action aggregate readiness detail属于WaitResult snapshot。

**Graph**

58. DiscoveryGraph是Context唯一graph authority。
59. revision只在observable state真实变化时递增。
60. GraphSnapshot/GraphEvent不在dclcpp/dclpy复制cache。
61. Node identity不从Participant name猜测。
62. Node/entity association通过显式metadata维护。
63. ROS2 mode graph metadata通过interop test验证。
64. Service/Action availability只承诺participant/metadata-consistent candidate，不夸大为DDS无法证明的唯一remote object identity。

**Action**

65. Action endpoint composition属于DMW。
66. GoalId/Goal FSM属于DMW。
67. Cancel selection common semantics属于DMW。
68. terminal Goal/pending result RequestId/expiry state属于DMW。
69. ActionClient/Server作为aggregate waitable，各使用一个public registration token。
70. Action availability使用同一DiscoveryGraph authority。
71. dclcpp/dclpy不重新组合3 Service + 2 Topic建立第二套Action protocol。
72. accepted SendGoal response与Goal registry commit通过`accept_goal()`形成单一DMW observable transaction。
73. raw `write_goal_response()`只用于rejected/non-accepted response。
74. accepted response write failure不得留下public accepted Goal。
75. GoalInfo protocol timestamp由Client Library typed adapter构造，但clock semantics必须来自DMW Clock。

**Parameters**

76. Parameter value/descriptor/store/validation/override只有DMW一个common authority。
77. Parameter user callback不进入DMW。
78. Atomic store failure不得partial apply。
79. Parameter override由DMW Arguments/parser形成canonical state。
80. ParameterChangeSet描述一次成功commit delta，typed event message由Client Library组装。

**ROS 2 compatibility**

81. Topic使用ROS2 `rt/` mapping。
82. Service使用`rq/...Request` / `rr/...Reply` mapping。
83. Service correlation使用SampleIdentity/related_sample_identity。
84. Action logical suffix固定使用`/_action/send_goal`、`cancel_goal`、`get_result`、`feedback`、`status`。
85. ROS2 mode基础Graph不再依赖Participant-name heuristic。
86. Jazzy/2.14与Humble/2.6均进入持续compatibility matrix。

### 11.15 结论

DMW V1 的目标不是复制 ROS 2 package tree，而是把两个 Client Library 真正需要共享的 language-neutral foundation/runtime集中到一个 authority：

```text
                 dclcpp / dclpy
      typed API / Future / callback / Executor
                        │
                        ▼
                       DMW
 Context / Clock / Naming / QoS / Parameter
 Topic / Service / Timer / Graph / Action / Wait
                        │
                        ▼
                    Fast DDS
```

在该边界下：

- Clock/Timer语义不在C++/Python重复；
- name/remapping/parameter override不在C++/Python重复；
- Parameter validation/store不在C++/Python重复；
- Action不在C++/Python各自实现Goal FSM/cancel/result lifecycle；
- Graph不允许两个Client Library各建cache；
- QoS profile/actual/compatibility只有一个authority；
- Future、callback、Executor、typed presentation、GIL/asyncio仍严格留在语言层；
- Component/Composition明确不进入DCL V1。

完成本文新增基础能力和现有Foundation contract convergence后，文档状态才应从 `V1 Architecture Convergence` 恢复为 `V1 Design Frozen Candidate`。
