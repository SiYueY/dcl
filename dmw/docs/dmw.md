# DMW 设计文档

| 属性 | 值 |
| --- | --- |
| 文档状态 | V1 Freeze Candidate |
| 模块名称 | DMW — DDS Middleware Layer |
| 上层 | `dclcpp`、`dclpy` |
| 下层 | Fast DDS |
| 语言标准 | C++17 |
| ROS 2 兼容基线 | ROS 2 Humble + `rmw_fastrtps_cpp` |

## 1. 文档概述

<a id="dmw-document-authority"></a>

### 1.1 文档目的

本文档是 DCL 项目中 `dmw` 模块 V1 的唯一公共运行时规范，定义公共 API、对象与所有权模型、生命周期、错误与并发语义，以及 ROS 2 Fast DDS wire interoperability contract。Fast DDS 专用实现规则由 [`dmw_fastdds.md`](dmw_fastdds.md) 定义；后者不得扩大、收窄或改写本文的公共契约。

本文档不仅描述架构方向，还给出：

> **DMW V1 可以直接进入实现阶段所需要的公共契约、生命周期规则、并发语义和 wire compatibility 规则。**

#### 1.1.1 阅读路径

| 读者目标 | 建议章节 |
| --- | --- |
| 理解模块边界和对象关系 | [公共对象模型](#dmw-public-object-model) |
| 实现公共 API 与生命周期 | [Factory 与 RAII](#dmw-public-object-model)、[Context runtime](#dmw-context-runtime) |
| 实现 Topic、Service 和同步原语 | [Topic](#dmw-topic)、[Service](#dmw-service)、[WaitSet](#dmw-waitset) |
| 理解 discovery、并发和 ROS 2 互操作 | [Registry 与 discovery](#dmw-registry-discovery)、[并发](#dmw-concurrency)、[ROS 2 互操作](#dmw-ros2-interop) |
| 执行实现验收或冻结审查 | [验收](#dmw-verification)、[Frozen invariant](#dmw-frozen-invariants) |

#### 1.1.2 规范性语言

本文统一使用以下关键词：

| 关键词 | 含义 |
| --- | --- |
| **必须** | 实现必须满足的强制要求 |
| **不得** | 实现禁止出现的行为 |
| **可以** | 允许但不要求采用的行为 |
| **建议** | 非规范性的实现建议 |

未使用上述关键词的原理、原因和示例用于解释规范，不独立引入新要求。代码中的类型名、函数名和枚举值保持其原始英文拼写。

#### 1.1.3 术语约定

| 术语 | 本文含义 |
| --- | --- |
| public entity | 暴露给调用者、具有稳定身份的资源对象 |
| value type | 不表示 middleware resource identity 的值、描述符或结果类型 |
| Factory | 返回完整有效对象或 Error 的归属型创建接口 |
| parent | 创建并定义子实体逻辑归属的 Context、Node 或 endpoint |
| backing/state | 用于并发安全和延迟 teardown 的内部共享状态，不改变 public ownership |
| DDS entity | Fast DDS 创建的 Participant、Reader、Writer、Topic 或 Condition |
| wire compatibility | DDS naming、type、QoS 和 identity 足以实现数据面互操作 |

### 1.2 DMW 定位

DMW 是 DCL 的 middleware 核心层：

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

DMW 具有以下边界：

- 不依赖 `rcl`、`rclcpp`、ROS 2 runtime 或 ament runtime；
- 不支持在运行时或构建时切换到其他 DDS 实现；
- 不额外引入 Runtime Core、实现选择层或 Protocol 等顶层抽象层。

DMW 直接使用 Fast DDS 提供 middleware primitives，但普通公共 API 不暴露 Fast DDS 类型。

### 1.3 V1 核心设计原则

DMW V1 冻结以下基础原则：

1. 使用 C++17；
2. Fast DDS-only；
3. runtime public API non-template；
4. 普通 public API 隐藏 Fast DDS 类型；
5. Resource / Entity 使用 RAII；
6. public entity 地址和 identity 稳定；
7. public ownership 使用 `std::unique_ptr`；
8. public entity non-copyable、non-movable；
9. Factory 创建成功即得到完整有效对象；
10. 不使用两阶段初始化；
11. 不提供 public `destroy()`；
12. middleware resource construction 事务化；
13. DMW listener 不执行 user callback；
14. DMW 提供 WaitSet，但不提供 Executor；
15. DMW 提供 Topic 和 Service primitive；
16. Action 不作为 DMW primitive；
17. V1 支持 RMW 基础通信能力；
18. V1 支持 ROS 2 Humble + Fast DDS wire interoperability；
19. V1 不要求完整 ROS Graph compatibility。

## 2. V1 范围与能力边界

### 2.1 Runtime

V1 支持：

- `Context`
- `Node`
- DDS Domain ID
- DomainParticipant
- shutdown
- 一个进程创建多个 Context
- 多 DDS Domain

### 2.2 Topic

V1 支持：

- `Publisher`
- `Subscriber`
- Fast DDS C++ message publish
- Fast DDS C++ message take
- `MessageInfo`
- matched Subscriber count
- matched Publisher count
- 基础 Topic QoS
- Publisher / Subscriber Event

### 2.3 Service

V1 支持：

- `Client`
- `Server`
- send request
- take request
- send response
- take response
- `RequestId`
- multi-client correlation
- service availability
- WaitSet readiness
- ROS 2 Humble Fast DDS Service interoperability

### 2.4 Wait

V1 支持：

- `WaitSet`
- `WaitableRegistration`
- `WaitResult`
- poll
- finite timeout
- infinite wait
- `GuardCondition`
- `Event`
- Subscriber readiness
- Client readiness
- Server readiness
- topology wakeup
- Context shutdown wakeup

### 2.5 Type

V1 支持：

- `MessageType`
- `ServiceType`
- Fast DDS TypeSupport binding
- TypeRegistry
- TopicRegistry

### 2.6 QoS

V1 支持：

- History
- Depth
- Reliability
- Durability
- Deadline
- Lifespan
- Liveliness
- Liveliness lease duration

### 2.7 ROS 2 Compatibility

V1 的 ROS2 RuntimeMode 精确限定为：

```text
ROS 2 Humble
+
rmw_fastrtps_cpp
+
Fast DDS
```

支持：

- ROS Topic DDS naming；
- ROS Service DDS naming；
- DDS type name；
- ROS-compatible CDR；
- Topic QoS；
- Service QoS；
- request SampleIdentity；
- related_sample_identity；
- Client/Server correlation；
- Service availability。

### 2.8 V1 非目标

V1 不实现：

- ROS GraphCache；
- ROS Graph public API；
- topic names/types introspection；
- service names/types introspection；
- serialized message API；
- public serialize / deserialize API；
- loaned message；
- zero-copy API；
- content filtered topic；
- message sequence take；
- actual QoS query；
- endpoint information；
- public QoS compatibility query；
- wait-for-all-acked；
- public assert-liveliness；
- network flow endpoint；
- DDS Security public API；
- Action；
- callback；
- Executor；
- Future；
- Python asyncio；
- IDL parser；
- code generator；
- C API；
- stable C ABI；
- Cyclone DDS；
- Connext；
- middleware plugin abstraction。

### 2.9 V1 RMW 基础功能矩阵

| 能力                               | V1 |
| -------------------------------- | -: |
| Context                          |  ✅ |
| Node                             |  ✅ |
| shutdown                         |  ✅ |
| Multi-domain                     |  ✅ |
| MessageType                      |  ✅ |
| ServiceType                      |  ✅ |
| Publisher                        |  ✅ |
| Subscriber                       |  ✅ |
| publish                          |  ✅ |
| take                             |  ✅ |
| MessageInfo                      |  ✅ |
| matched Subscriber count         |  ✅ |
| matched Publisher count          |  ✅ |
| Client                           |  ✅ |
| Server                           |  ✅ |
| send request                     |  ✅ |
| take request                     |  ✅ |
| send response                    |  ✅ |
| take response                    |  ✅ |
| RequestId                        |  ✅ |
| multi-client correlation         |  ✅ |
| service availability             |  ✅ |
| QoS                              |  ✅ |
| WaitSet                          |  ✅ |
| GuardCondition                   |  ✅ |
| Event                            |  ✅ |
| ROS 2 Topic wire compatibility   |  ✅ |
| ROS 2 Service wire compatibility |  ✅ |
| Graph introspection              |  ❌ |
| SerializedMessage                |  ❌ |
| loaned message                   |  ❌ |
| zero-copy                        |  ❌ |
| actual QoS                       |  ❌ |
| Action                           |  ❌ |

<a id="dmw-architecture"></a>

## 3. 总体架构

本章先定义 DMW 在系统中的位置和 type-erased runtime boundary，再通过[公共 API 与对象模型](#dmw-public-object-model)冻结 public entity、value type、Factory 与 RAII 关系。

### 3.1 系统分层

```text
┌───────────────────────────────────────────────────────┐
│                       dclcpp                          │
│                                                       │
│ Publisher<T> / Subscriber<T>                          │
│ Client<S> / Server<S>                                 │
│ Executor / Callback / Future / Action                 │
└───────────────────────────┬───────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────┐
│                         dmw                           │
│                                                       │
│ Context / Node                                        │
│ MessageType / ServiceType                             │
│ Publisher / Subscriber                                │
│ Client / Server                                       │
│ Qos / Gid / MessageInfo / RequestId                   │
│ WaitSet / GuardCondition / Event                      │
│ TypeRegistry / TopicRegistry                          │
│ Discovery / Matched State                             │
│ ROS 2 Fast DDS Compatibility                          │
└───────────────────────────┬───────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────┐
│                      Fast DDS                         │
│                                                       │
│ DomainParticipant                                     │
│ Topic                                                 │
│ DataWriter / DataReader                               │
│ TypeSupport / TopicDataType                           │
│ WaitSet / Condition                                   │
│ SampleIdentity                                        │
│ Discovery                                             │
└───────────────────────────┬───────────────────────────┘
                            ▼
                        DDSI-RTPS
```

### 3.2 Fast DDS 映射

```text
Context
   ↓
DomainParticipant

Publisher
   ↓
DataWriter

Subscriber
   ↓
DataReader

Client
   ├── request DataWriter
   └── response DataReader

Server
   ├── request DataReader
   └── response DataWriter
```

### 3.3 Non-template Runtime API

DMW runtime API 不使用消息模板类型：

```cpp
class Publisher;
class Subscriber;

class Client;
class Server;

class MessageType;
class ServiceType;
```

禁止：

```cpp
template<class T>
class Publisher;
```

typed API 属于：

`dclcpp`

而不是 DMW。

### 3.4 Type Erasure

Topic：

```cpp
Result<void> Publisher::write(
    const void* message);

Result<bool> Subscriber::read(
    void* message,
    MessageInfo& info);
```

Service：

```cpp
Result<RequestId> Client::write_request(
    const void* request);

Result<bool> Client::read_response(
    void* response,
    RequestId& request_id);

Result<bool> Server::read_request(
    void* request,
    RequestId& request_id);

Result<void> Server::write_response(
    const RequestId& request_id,
    const void* response);
```

### 3.5 Type-erased Pointer 公共契约

#### 3.5.1 非空要求

以下参数不得为 `nullptr`：

```text
Publisher::write(message)

Subscriber::read(message, ...)

Client::write_request(request)

Client::read_response(response, ...)

Server::read_request(request, ...)

Server::write_response(..., response)
```

如果指针为 `nullptr`：

`ErrorCode::InvalidArgument`

DMW 不访问该指针。

#### 3.5.2 Concrete Type 前置条件

传入的对象必须：

1. 是完整构造且仍存活的 C++ object；
2. 满足正常 C++ alignment；
3. 对象生命周期覆盖整个 DMW 调用；
4. concrete type 必须与 endpoint 创建时绑定的 `MessageType` 完全对应。

例如：

```cpp
Foo wrong_message;

bar_publisher->write(&wrong_message);
```

如果 Publisher 实际绑定的是：

`Bar`

则属于调用者违反前置条件。

由于 `void*` 本身不包含运行时 C++ type information，DMW V1：

> **不保证能够检测错误 concrete message type。**

这种错误不映射到：

`TypeMismatch`

而属于 programming contract violation。

`dclcpp` typed API 必须通过模板类型消除正常用户路径上的这一风险。

#### 3.5.3 `TypeMismatch` 的用途

`ErrorCode::TypeMismatch` 只用于 DMW 实际可验证的情况，例如：

- 同一 DDS Topic name 注册了不同 wire type；
- TypeRegistry 中同一 wire type name 对应不同 binding identity；
- Service request / response descriptor 与已有 registry entry 冲突。

它不用于尝试识别任意 `void*` 的真实 C++ 动态类型。

#### 3.5.4 接收输出状态

如果：

```cpp
subscriber.read(message, info)
```

返回：

`success + false`

则：

```text
message
MessageInfo
```

必须保持调用前内容不变。

如果在真正读取 DDS sample 之前发生：

```text
InvalidArgument
ContextShutdown
ParentDestroyed
InvalidState
```

等错误，输出也必须保持不变。

如果：

1. DDS sample 已经被取出；
2. 随后 deserialize 失败；

则：

- C++ output object 必须保持可析构的合法对象状态；
- 具体字段内容允许 unspecified；
- `MessageInfo` 允许 unspecified；
- sample 允许已经从 DataReader history 中移除。

该错误返回：

`DDSError`

或与实际错误更匹配的 ErrorCode。

#### 3.5.5 Event 输出状态

`Event::take(EventInfo&)` 使用相同规则：

```text
NoData
    -> EventInfo unchanged

error before consuming status
    -> EventInfo unchanged

error after Fast DDS status consumption
    -> EventInfo valid but content unspecified
```

#### 3.5.6 Service take 输出与有限扫描

`Client::read_response()` 与 `Server::read_request()` 对 payload、`RequestId` 和其它 public
metadata 使用同一事务边界：`false` 或 middleware sample consumption 前的任何失败保持
全部 caller output 不变；sample 已消费后的 metadata conversion、correlation、payload
commit、Error 或允许传播的 C++ exception，只保证全部 output 仍 valid/destructible，字段
内容可以 unspecified，sample 可以已经从 middleware history 移除。

需要过滤 invalid、foreign、duplicate 或不可关联 sample 的任意 public `take()`，单次调用
必须使用有限的 call-start candidate budget：filtered sample 消耗 budget，并发新 arrival 不得
延长本次调用；budget 耗尽且没有 returnable sample 时返回 `false`。因此 `false`
是“本次有限候选扫描未取得 public sample”，不保证返回瞬间 middleware history 绝对为空。
后续调用可以继续处理剩余或新到达 sample。

<a id="dmw-public-object-model"></a>

### 3.6 公共 API 与对象模型

#### 3.6.1 公共类型分类

##### 3.6.1.1 Resource/Entity

```text
Context
Node

Publisher
Subscriber

Client
Server

WaitSet
GuardCondition
Event
```

##### 3.6.1.2 Value/Descriptor/Result

```text
ContextOptions
NodeOptions

PublisherOptions
SubscriberOptions
ClientOptions
ServerOptions

WaitSetOptions
GuardConditionOptions

MessageType
ServiceType

Gid
RequestId
MessageInfo

Qos
QosDuration

EventType
EventInfo

WaitTimeout
WaitableRegistration
WaitResult

Error
Result<T>
```

#### 3.6.2 Resource/Entity 语义

Resource/Entity 统一：

```text
Factory
+
Result<std::unique_ptr<T>>
+
non-copyable
+
non-movable
+
stable identity
+
stable address
+
RAII
```

示例：

```cpp
class Publisher
{
public:
    ~Publisher() noexcept;

    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    Publisher(Publisher&&) = delete;
    Publisher& operator=(Publisher&&) = delete;

private:
    class Impl;

    explicit Publisher(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
```

ownership transfer：

```cpp
std::unique_ptr<Publisher> first = ...;
auto second = std::move(first);
```

移动的是：

`unique_ptr`

而不是 Publisher 对象本身。

#### 3.6.3 公共 Factory

唯一 Factory 结构：

```text
                     Context::create()
                            │
                            ▼
                         Context
             ┌──────────────┼──────────────┐
             ▼              ▼              ▼
       create_node    create_wait_set  create_guard_condition
             │
             ▼
            Node
     ┌───────┼────────┬────────┐
     ▼       ▼        ▼        ▼
Publisher Subscriber Client  Server
     │       │
     └ create_event ┘
```

#### 3.6.4 Factory 命名

冻结为：

```text
Context
    Context::create()

Node
    Context::create_node()

Publisher
    Node::create_publisher()

Subscriber
    Node::create_subscriber()

Client
    Node::create_client()

Server
    Node::create_server()

WaitSet
    Context::create_wait_set()

GuardCondition
    Context::create_guard_condition()

Event
    Publisher::create_event()
    Subscriber::create_event()
```

不得提供 `create_subscription()` 或 `create_service()` 这类与当前实体命名和归属层级不一致的 Factory。

#### 3.6.5 Factory 返回类型

统一：

```cpp
Result<std::unique_ptr<T>>
```

例如：

```cpp
Result<std::unique_ptr<Publisher>>
Node::create_publisher(...);
```

#### 3.6.6 事务式创建

流程：

```text
public Factory
      │
      ▼
Impl::create()
      │
      ▼
validate
      │
      ▼
transactional resource build
      │
 ┌────┴─────┐
 ▼          ▼
success    failure
 │           │
 ▼           ▼
complete    local RAII rollback
Impl
 │
 ▼
public entity
```

任何步骤失败不得返回：

`half-valid public object`

#### 3.6.7 构造函数

Resource/Entity constructor：

- private；
- `noexcept`；
- 只包装完整有效的 `Impl`。

```cpp
Publisher::Publisher(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}
```

禁止：

```cpp
Publisher publisher;
publisher.initialize(...);
```

#### 3.6.8 `Impl::create()`

内部中间资源必须通过 local RAII handle 管理，例如：

```text
TypeRegistrationHandle
TopicHandle
DataWriterHandle
DataReaderHandle
ConditionHandle
```

任一步失败：

```text
local object destruction
    ↓
reverse rollback
```

不得依赖易遗漏的大段手写 rollback。

#### 3.6.9 `Impl::destroy()`

内部：

```cpp
class Publisher::Impl
{
public:
    ~Impl() noexcept
    {
        destroy();
    }

private:
    void destroy() noexcept;
};
```

要求：

- private；
- `noexcept`；
- idempotent；
- best-effort；
- 尽可能逆创建顺序销毁；
- 某一步失败后继续其余 cleanup；
- 不从析构抛异常。

#### 3.6.10 共享内部状态

public ownership：

`unique_ptr`

与内部 dependency lifetime：

`shared_ptr / weak_ptr`

必须区分。

例如：

```text
Context facade
    ↓
Context::Impl
    ↓
shared_ptr<Context>
    ▲      ▲       ▲
    │      │       │
  Node   WaitSet  Endpoint
```

内部共享状态只用于：

- teardown safety；
- dependency lifetime；
- concurrency coordination。

它不改变 public ownership。

<a id="dmw-error-result"></a>

## 4. 错误、Result 与日志

本章集中定义所有公共错误表示、`Result<T>` alternative 访问规则、exception boundary、timeout/NoData 区分和 logging 边界。其它章节不得重新定义这些通用规则。

### 4.1 ErrorCode

V1 冻结：

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

不包含：

`Ok`

。

### 4.2 Error

```cpp
class Error
{
public:
    Error(
        ErrorCode code,
        std::string message);

    ErrorCode
    code() const noexcept;

    std::string_view
    message() const noexcept;

private:
    ErrorCode code_;
    std::string message_;
};
```

每个合法 Error 都表示失败。

不存在：

`Error{Ok}`

。

### 4.3 Result<T> 基础契约

```cpp
template<class T>
class Result
{
public:
    static Result success(T value);

    static Result failure(Error error);

    bool has_value() const noexcept;

    explicit operator bool() const noexcept;

    T& value() & noexcept;

    const T& value() const & noexcept;

    T&& value() && noexcept;

    Error& error() & noexcept;

    const Error& error() const & noexcept;

    Error&& error() && noexcept;

private:
    // expected-like storage
};
```

### 4.4 Result<T> Default Construction

`Result<T>`：

`not default constructible`

。

创建时必须明确是：

```text
success
or
failure
```

。

### 4.5 Move-only Value

`Result<T>` 必须支持 move-only T。

例如：

```cpp
Result<std::unique_ptr<Node>>
result = ...;

if (!result) {
    // result.error()
}

std::unique_ptr<Node> node =
    std::move(result).value();
```

因此：

```cpp
T&& value() &&
```

是 V1 必需 public contract。

### 4.6 Copy / Move Semantics

`Result<T>`：

- 如果 T copyable，则 Result copyable；
- 如果 T 不可 copy，则 Result 不可 copy；
- 如果 T movable，则 Result movable。

`Error` 本身：

- copyable；
- movable。

### 4.7 Wrong-alternative Access

调用：

```cpp
result.value()
```

要求：

`has_value() == true`

。

调用：

```cpp
result.error()
```

要求：

`has_value() == false`

。

违反该前置条件属于 programming contract violation。

DMW V1 定义：

> 错误 alternative access 必须调用 `std::terminate()`；debug build 可以在 terminate 前触发 assertion。

它：

- 不抛 exception；
- 不是 UB；
- 不返回 fake value。

### 4.8 Result<void>

```cpp
template<>
class Result<void>
{
public:
    static Result success();

    static Result failure(Error error);

    bool has_value() const noexcept;

    explicit operator bool() const noexcept;

    void value() const noexcept;

    Error& error() & noexcept;

    const Error& error() const & noexcept;

    Error&& error() && noexcept;

private:
    // ...
};
```

`value()` 只有在 success 状态合法。

失败状态调用 `value()`：

`std::terminate()`

。

### 4.9 Result 与异常

DMW runtime expected failure：

`不使用 C++ exception 传播`

。

例如：

```text
invalid configuration
DDS create failure
shutdown
timeout
```

通过：

`Result<T>`

表达。

内部无法恢复的 programming bug 可使用 assertion / terminate。

#### 4.9.1 Public exception boundary

只有 destructor、trivial observer 和实现能够保证不失败且不分配的操作使用
`noexcept`。返回 `Result<T>` 的普通 public operation 默认不标 `noexcept`。

边界规则固定为：

```text
expected middleware/configuration/lifecycle failure
    -> Result<T>

Fast DDS 可转换错误
    -> Result<T>

invalid argument
    -> Result<T>

std::bad_alloc
    -> 允许作为 C++ exception 向上传播

其他未预期 C++ exception
    -> 允许向上传播；不可恢复 programming bug 可 terminate
```

未预期 exception 不得伪装成普通 DMW Error。V1 不提供 allocation-free Error，
因此不定义 `ErrorCode::BadAllocation`。`ResourceExhausted` 只表达逻辑/中间件资源
上限，例如 pending request 上限、token ID 耗尽或 DDS resource limit，不表示
进程 heap OOM。

### 4.10 Timeout 与 NoData

```text
Subscriber no sample
    -> success + false

WaitSet timeout
    -> WaitStatus::Timeout
```

均不是 Error。

但 Service response discovery workaround 的：

`response writer wait timeout`

是一次 operation failure：

`ErrorCode::Timeout`

。

### 4.11 Logging

日志用于：

- create/destroy；
- rollback；
- DDS error；
- matched state；
- service identity；
- registry conflict；
- WaitSet；
- shutdown；
- ROS compatibility。

日志不能代替 Result。

<a id="dmw-context-runtime"></a>

## 5. Context 与 Node Runtime

`Context` 是一个 Domain ID 和一个 DomainParticipant 的 runtime root；`Node` 是 logical entity。Context shutdown 改变运行状态，内部 shared state 只保证 teardown 安全，不延长 Active 状态。

### 5.1 ContextOptions

```cpp
struct ContextOptions
{
    std::uint32_t domain_id{0};
    std::string participant_name;
    RuntimeMode runtime_mode{
        RuntimeMode::DDS};
};
```

`RuntimeMode` 是 Context-scoped immutable property。它影响 Participant/Fast DDS
container QoS、`SystemDefault` 解析、DDS naming 与 interoperability scope；同一 Context
内的 endpoint 不得分别覆盖 RuntimeMode。

C++17 示例：

```cpp
ContextOptions options;
options.domain_id = 0;
options.participant_name = "dcl";

auto context_result =
    Context::create(options);
```

本文档不使用 C++20 designated initializer。

### 5.2 Context API

```cpp
class Context
{
public:
    static Result<std::unique_ptr<Context>>
    create(const ContextOptions& options);

    ~Context() noexcept;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    std::uint32_t domain_id() const noexcept;

    bool is_shutdown() const noexcept;

    Result<void> shutdown();

    Result<std::unique_ptr<Node>>
    create_node(
        const NodeOptions& options);

    Result<std::unique_ptr<WaitSet>>
    create_wait_set(
        const WaitSetOptions& options = {});

    Result<std::unique_ptr<GuardCondition>>
    create_guard_condition(
        const GuardConditionOptions& options = {});

private:
    class Impl;

    explicit Context(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
```

### 5.3 Context 与 DomainParticipant

冻结：

```text
1 Context
    =
1 DDS Domain ID
    =
1 DomainParticipant
```

不提供 public `Participant`。

### 5.4 Multi-domain

多个 Domain：

```cpp
ContextOptions domain0_options;
domain0_options.domain_id = 0;

ContextOptions domain10_options;
domain10_options.domain_id = 10;

auto context0 =
    Context::create(domain0_options);

auto context10 =
    Context::create(domain10_options);
```

形成：

```text
Process
├── Context(domain=0)
│   └── Participant(domain=0)
│
└── Context(domain=10)
    └── Participant(domain=10)
```

### 5.5 Context 内部状态机

内部冻结：

```text
Active
   │
   │ shutdown linearization
   ▼
ShuttingDown
   │
   │ wake / stop / drain
   ▼
Shutdown
   │
   │ last internal reference released
   ▼
Destroyed
```

其中：

```text
Active
ShuttingDown
Shutdown
```

是 runtime state。

`Destroyed` 是内部 resource lifetime 终点，不是 public runtime state。

<a id="dmw-shutdown"></a>

### 5.6 Shutdown 线性化点

`shutdown()` 的线性化点是：

`Active -> ShuttingDown`

的原子状态转换。

一旦转换发生：

> Context 永远不能重新进入 Active。

从此以后所有需要 Active runtime 的新操作，在完成参数合法性检查后返回：

`ContextShutdown`

。

### 5.7 `is_shutdown()`

定义：

```text
Active
    -> false

ShuttingDown
    -> true

Shutdown
    -> true
```

`is_shutdown()`：

- thread-safe；
- 可与 `shutdown()` 并发；
- 不抛异常。

### 5.8 重复 shutdown

`shutdown()` 是幂等 runtime transition。

第一个线程：

`Active -> ShuttingDown`

并执行 shutdown propagation。

其他线程如果观察到：

`ShuttingDown`

则等待该 shutdown attempt 完成。

所有参与同一次 shutdown 的线程得到相同 terminal `Result<void>`。

如果已经是：

`Shutdown`

再次调用 `shutdown()` 返回此前记录的 terminal shutdown result。

因此：

- 成功 shutdown 后重复调用继续 success；
- 如果第一次 shutdown 完成但报告内部 cleanup error，后续调用返回相同 Error；
- Context 状态不会因为 Error 回到 Active。

这是公共契约允许的通用实现能力，不要求 Fast DDS 实现刻意产生 cleanup Error。该实现可以在其
已声明的 liveness/teardown assumptions 下实现更窄的“正常 runtime shutdown 恒为 success”
子集。runtime 已提交 terminal result 后才发生的 Fast DDS final cleanup/delete failure，只进入
diagnostic、conservative retention 或 quarantine，不得 retroactively 改变此前返回的 result。

### 5.9 Shutdown propagation

进入 ShuttingDown 后至少执行：

1. 阻止新 Node；
2. 阻止新 endpoint；
3. 阻止新 WaitSet；
4. 阻止新 GuardCondition；
5. 通知所有 WaitSet；
6. 打断 Fast DDS WaitSet wait；
7. 等待 WaitSet 离开需要 Context Active 的 Fast DDS WaitSet wait；
8. 标记 runtime Shutdown。

如果某个 Fast DDS wake 操作失败：

- Context 仍保持 ShuttingDown；
- 必须继续其他 WaitSet；
- 必须执行安全 fallback，例如 detach / condition invalidation；
- 不允许恢复 Active。

只有所有 registered WaitSet 已经：

```text
woken
or
safely detached from active Fast DDS WaitSet wait
```

以后，状态才转换为：

`Shutdown`

。

如果期间发生错误：

`shutdown() -> Error`

但：

`is_shutdown() == true`

。

### 5.10 Context Destructor

如果用户没有显式 shutdown：

```text
~Context()
    ↓
best-effort shutdown_noexcept()
```

错误：

- 写日志；
- 不抛异常。

Context facade 析构不等于立刻删除 DomainParticipant。

### 5.11 Context 最终销毁

如果还有 child：

```text
Publisher
Subscriber
Client
Server
Node
WaitSet
GuardCondition
Event
```

依赖 `Context`：

```text
Context facade gone
        │
        ▼
Context remains
        │
        ▼
children safely destruct
        │
        ▼
last reference released
        │
        ▼
DomainParticipant destroyed
```

### 5.12 Node

```cpp
struct NodeOptions
{
    std::string node_name;
    std::string node_namespace{"/"};
};
```

Node 是 logical entity，不等于 Participant。

### 5.13 Node API

```cpp
class Node
{
public:
    ~Node() noexcept;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    Node(Node&&) = delete;
    Node& operator=(Node&&) = delete;

    std::string_view name() const noexcept;
    std::string_view node_namespace() const noexcept;

    Result<std::unique_ptr<Publisher>>
    create_publisher(
        const MessageType& type,
        std::string_view topic_name,
        const Qos& qos,
        const PublisherOptions& options = {});

    Result<std::unique_ptr<Subscriber>>
    create_subscriber(
        const MessageType& type,
        std::string_view topic_name,
        const Qos& qos,
        const SubscriberOptions& options = {});

    Result<std::unique_ptr<Client>>
    create_client(
        const ServiceType& type,
        std::string_view service_name,
        const Qos& qos,
        const ClientOptions& options = {});

    Result<std::unique_ptr<Server>>
    create_server(
        const ServiceType& type,
        std::string_view service_name,
        const Qos& qos,
        const ServerOptions& options = {});

private:
    friend class Context;

    class Impl;

    explicit Node(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
```

### 5.14 Node 生命周期

内部：

```text
Node facade
    │
    ▼
NodeState
 ▲  ▲  ▲  ▲
 │  │  │  │
Pub Sub Client Server
```

Node facade 可以先析构。

如果 Context 仍 Active：

```cpp
publisher->publish(...);
subscriber->take(...);
```

已有 endpoint 继续正常工作。

Node facade 析构后：

- 不再创建 endpoint；
- NodeState 保留 logical identity；
- 最后一个 endpoint 释放后 NodeState 才最终释放。

### 5.15 全局错误优先级

所有 public runtime API 使用统一错误优先级：

#### 5.15.1 第一级：参数错误

首先检查可以安全检测的 public 参数错误：

```text
nullptr
invalid token
invalid name
invalid QoS input
```

返回对应：

```text
InvalidArgument
InvalidName
...
```

#### 5.15.2 第二级：Context 状态

参数有效后，如果 Context 已处于：

```text
ShuttingDown
or
Shutdown
```

返回：

`ContextShutdown`

#### 5.15.3 第三级：Parent 状态

Context Active，但 parent endpoint 已被销毁：

`ParentDestroyed`

#### 5.15.4 第四级：Object-local 状态

例如：

```text
AlreadyRegistered
NotRegistered
Busy
NotFound
```

#### 5.15.5 第五级：Middleware runtime error

最后才是：

```text
DDSError
Timeout
```

等底层错误。

<a id="dmw-type-system"></a>

## 6. 类型系统

### 6.1 Gid

Fast DDS GUID 不进入普通 public API。

```cpp
struct Gid
{
    static constexpr std::size_t Size = 16;

    std::array<std::uint8_t, Size> data{};
};

bool operator==(
    const Gid& lhs,
    const Gid& rhs) noexcept;

bool operator!=(
    const Gid& lhs,
    const Gid& rhs) noexcept;
```

提供：

```cpp
struct GidHash
{
    std::size_t operator()(
        const Gid& gid) const noexcept;
};
```

内部：

```text
Fast DDS GUID_t
      ↕
dmw::Gid
```

### 6.2 MessageType

`MessageType` 是一个 cheap-copy、可重新绑定的 runtime descriptor handle。

handle 指向的 binding descriptor 在构造后不可变；copy/move assignment 只会让
handle 改为引用另一个不可变 descriptor，并不修改已有 descriptor 内容。

```cpp
namespace fastdds
{
class MessageTypeAdapter;
}

class MessageType
{
public:
    MessageType(const MessageType&) = default;
    MessageType& operator=(
        const MessageType&) = default;

    MessageType(MessageType&&) noexcept = default;
    MessageType& operator=(
        MessageType&&) noexcept = default;

    std::string_view
    type_name() const noexcept;

private:
    friend class fastdds::MessageTypeAdapter;

    class Impl;

    explicit MessageType(
        std::shared_ptr<const Impl> impl) noexcept;

    std::shared_ptr<const Impl> impl_;
};
```

不提供：

```cpp
MessageType();
bool valid();
```

存在的 MessageType 必须有效。

### 6.3 `type_name()`

`MessageType::type_name()` 表示：

> **DDS wire type name**

而不是普通 C++ type name。

例如 ROS-compatible String：

`std_msgs::msg::dds_::String_`

### 6.4 Fast DDS Binding

专用 header：

`dmw/fastdds/message_type.hpp`

提供：

```cpp
namespace dmw::fastdds
{

class MessageTypeAdapter
{
public:
    static Result<MessageType> create(
        eprosima::fastdds::dds::TypeSupport
            type_support,
        std::type_index pubsub_type);

    static const eprosima::fastdds::dds::TypeSupport& type_support(
        const MessageType& message_type) noexcept;

    static std::type_index pubsub_type(const MessageType& message_type) noexcept;
};

template<class PubSubTypeT>
Result<MessageType>
create_message_type();

}
```

构造路径固定为：

```text
create_message_type<PubSubTypeT>()
        ↓
construct PubSubTypeT
        ↓
fastdds::MessageTypeAdapter::create(
    type_support,
    typeid(PubSubTypeT))
        ↓
MessageType::Impl::create(...)
        ↓
完整有效的 MessageType handle
```

`fastdds::MessageTypeAdapter` 只服务于 binding integration，不属于普通 runtime
consumer API。它是唯一可以构造 `MessageType::Impl` 并调用 private
`MessageType` 构造函数的 integration access point；`create_message_type()` 不绕过
`Impl::create()` 的验证和错误返回。

`MessageTypeAdapter` 的完整声明只出现在 Fast DDS binding header；普通
`dmw/message_type.hpp` 仅 forward declare 它，因此核心 public header 不出现任何
Fast DDS/Fast CDR 类型。

普通 runtime consumer 不需要包含该 header。

### 6.5 BindingIdentity

V1 TypeRegistry 不使用：

```text
Impl pointer address
TypeSupport object pointer address
```

作为 type identity。

`MessageType::Impl` 内部必须保存：

```text
DDS wire type name
+
BindingIdentity
```

对于 Fast DDS static binding：

```text
BindingIdentity
    =
binding implementation id
+
C++ PubSubType binding type identity
```

V1 `dmw::fastdds_binding` 可以通过：

`std::type_index(typeid(PubSubTypeT))`

建立进程内 binding identity。

因此 V1 Fast DDS binding 要求 RTTI 可用。

### 6.6 MessageType 等价规则

两个独立构造的 MessageType：

```cpp
auto a = create_message_type<FooPubSubType>();
auto b = create_message_type<FooPubSubType>();
```

如果：

```text
wire type name identical
+
BindingIdentity identical
```

则认为是同一 registered type，可复用 TypeRegistry entry。

如果：

```text
wire type name identical
+
BindingIdentity different
```

V1 不进行结构化 wire-layout 比较。

这种情况返回：

`TypeMismatch`

。

即：

> V1 不尝试证明两个不同 PubSubType 实现是否结构上 wire-equivalent。

### 6.7 ServiceType

```cpp
class ServiceType
{
public:
    ServiceType(
        MessageType request_type,
        MessageType response_type) noexcept;

    const MessageType&
    request_type() const noexcept;

    const MessageType&
    response_type() const noexcept;

private:
    MessageType request_type_;
    MessageType response_type_;
};
```

ServiceType：

- 是可复制、可移动、可重新赋值的 value handle；
- 其 request/response descriptor 指向的 binding 内容不可变；
- copyable；
- movable；
- value descriptor；
- 不拥有 DDS resource。

<a id="dmw-profile-qos"></a>

## 7. RuntimeMode、Naming 与 QoS

本章的概念顺序为：Context 选择 immutable `RuntimeMode`，RuntimeMode 决定 logical name 到 Fast DDS name 的解析规则，并与 entity kind 共同解析 `SystemDefault` QoS。`Qos` value 本身不捕获 Context 或 RuntimeMode。

```text
ContextOptions.runtime_mode
        -> resolved DDS naming
        -> SystemDefault resolution
        -> explicit DDS entity QoS
```

### 7.1 QoS 策略

```cpp
enum class HistoryPolicy
{
    SystemDefault,
    KeepLast,
    KeepAll
};

enum class ReliabilityPolicy
{
    SystemDefault,
    Reliable,
    BestEffort
};

enum class DurabilityPolicy
{
    SystemDefault,
    Volatile,
    TransientLocal
};

enum class LivelinessPolicy
{
    SystemDefault,
    Automatic,
    ManualByTopic
};
```

### 7.2 QosDuration

```cpp
class QosDuration
{
public:
    enum class Kind
    {
        SystemDefault,
        Infinite,
        Finite
    };

    static QosDuration
    system_default() noexcept;

    static QosDuration
    infinite() noexcept;

    static Result<QosDuration>
    finite(
        std::chrono::nanoseconds value);

    Kind kind() const noexcept;

    std::chrono::nanoseconds
    value() const noexcept;

private:
    // ...
};
```

`value()` 的前置条件是：

`kind() == Kind::Finite`

对 `SystemDefault` 或 `Infinite` 调用 `value()` 属于 programming contract
violation，必须调用 `std::terminate()`；debug build 可以在 terminate 前触发
assertion。它不返回 sentinel，也不抛 exception。

### 7.3 QosDuration 输入规则

```cpp
QosDuration::finite(duration)
```

要求：

`duration >= 0`

负值返回：

`InvalidArgument`

零是合法 finite duration。

### 7.4 Duration → Fast DDS

转换使用 checked conversion：

```text
nanoseconds
    ↓
seconds
+
nanosecond remainder
```

要求：

`0 <= nanosecond remainder < 1e9`

如果 seconds 超出目标 Fast DDS duration representation：

`InvalidArgument`

不得发生整数 wraparound。

### 7.5 Qos API

```cpp
class Qos
{
public:
    Qos();

    static Qos system_default();

    static Qos ros2_default();

    static Qos ros2_services_default();

    Result<void>
    keep_last(
        std::size_t depth);

    Qos& keep_all() noexcept;

    Qos& history_system_default() noexcept;

    Qos& reliable() noexcept;
    Qos& best_effort() noexcept;

    Qos& transient_local() noexcept;
    Qos& volatile_() noexcept;

    Qos& reliability_system_default() noexcept;
    Qos& durability_system_default() noexcept;

    Qos& deadline(
        QosDuration value) noexcept;

    Qos& lifespan(
        QosDuration value) noexcept;

    Qos& liveliness(
        LivelinessPolicy value) noexcept;

    Qos& liveliness_lease_duration(
        QosDuration value) noexcept;

    HistoryPolicy history() const noexcept;
    std::size_t depth() const noexcept;
    ReliabilityPolicy reliability() const noexcept;
    DurabilityPolicy durability() const noexcept;
    QosDuration deadline() const noexcept;
    QosDuration lifespan() const noexcept;
    LivelinessPolicy liveliness() const noexcept;
    QosDuration
    liveliness_lease_duration() const noexcept;
};
```

`Qos()` 与 `Qos::system_default()` 完全等价，所有 policy 初始值均编码为
`SystemDefault`；Qos value 本身不捕获 Context 或 RuntimeMode。该 value 在 entity Factory 中
按 parent Context 的 immutable `RuntimeMode` 与 entity kind 解析。

所有可能失败的 Qos setter 都提供 strong guarantee：返回 Error 时，整个 Qos
保持调用前状态。无失败返回值的 setter 必须以一次原子状态提交或等价方式避免
向调用者暴露部分更新。

### 7.6 `keep_last()` 不变量

```cpp
qos.keep_last(depth);
```

要求：

`depth > 0`

否则：

`InvalidArgument`

且 Qos 保持调用前状态。

### 7.7 KeepAll 与 Depth

调用：

```cpp
qos.keep_all();
```

后：

```text
history = KeepAll
depth = 0
```

`depth` 不参与 DDS mapping。

若以后切换回 KeepLast，必须重新：

```cpp
keep_last(valid_depth);
```

不隐式恢复旧 depth。

### 7.8 SystemDefault

`SystemDefault` 的最终解释由：

```text
RuntimeMode
+
EntityKind
```

共同决定，而不是 Qos 自身决定。

### 7.9 DDS SystemDefault

在：

`RuntimeMode::DDS`

下：

`SystemDefault`

映射到 DMW 为当前 RuntimeMode、entity kind 和 Fast DDS version 冻结的 baseline。不得读取进程 XML default、
环境变量、factory/participant mutable default profile 或其它 middleware runtime default。

### 7.10 ROS2 SystemDefault

在：

`ROS2`

下：

- DMW V1 不读取 `RMW_FASTRTPS_USE_QOS_FROM_XML`；
- 不加载 ROS 2 Fast DDS XML override；
- `SystemDefault` 按 [ROS 2 interoperability contract](#dmw-ros2-interop) 固定版本 baseline 的 Fast DDS defaults 解析。

如果需要普通 ROS 2 默认 Topic QoS，应显式使用：

```cpp
Qos::ros2_default();
```

如果需要 ROS 2 Service 默认 QoS，应显式使用：

```cpp
Qos::ros2_services_default();
```

### 7.11 ROS 2 Default QoS

`Qos::ros2_default()`：

```text
History      = KeepLast
Depth        = 10
Reliability  = Reliable
Durability   = Volatile
Deadline     = SystemDefault
Lifespan     = SystemDefault
Liveliness   = SystemDefault
Lease        = SystemDefault
```

### 7.12 ROS 2 Service Default QoS

`Qos::ros2_services_default()`：

```text
History      = KeepLast
Depth        = 10
Reliability  = Reliable
Durability   = Volatile
Deadline     = SystemDefault
Lifespan     = SystemDefault
Liveliness   = SystemDefault
Lease        = SystemDefault
```

这与 Humble RMW service default profile 的基础策略一致。

### 7.13 Unsupported Policy

如果 DMW public QoS 支持某 policy，但当前 Fast DDS baseline 无法表达：

`Unsupported`

如果 policy 值本身非法：

`InvalidArgument`

。

远程 endpoint QoS 与本地不兼容：

> 不属于 endpoint create failure。

endpoint 正常创建，但：

`matched count = 0`

并可产生：

`IncompatibleQos Event`

。

### 7.14 RuntimeMode

```cpp
enum class RuntimeMode
{
    DDS,

    ROS2
};
```

`RuntimeMode` 的 public ownership 固定在 `ContextOptions::runtime_mode`，Context 创建
成功后不可修改。所有 public `topic_name()`、service name 或等价 observer 始终返回
normalized logical DMW name，不返回 runtime-mode-specific resolved DDS name。

设 logical FQN `/a/b` 对应 `path=a/b`，V1 resolved DDS naming 为：

| RuntimeMode | Topic | Service request | Service response |
| --- | --- | --- | --- |
| `DDS` | `<path>` | `<path>_Request` | `<path>_Reply` |
| `ROS2` | `rt/<path>` | `rq/<path>Request` | `rr/<path>Reply` |

`RuntimeMode` 还决定可观察的 endpoint Fast DDS QoS behavior、service identity、matching/discovery
behavior 与 wire-interoperability scope。完整 wire interoperability 要求 `MessageType` /
`ServiceType` 同时提供兼容的 wire type name 与 CDR serializer；选择 RuntimeMode 不表示 runtime
能够自动认证任意 custom `TopicDataType`。

<a id="dmw-topic"></a>

## 8. Publisher 与 Subscriber

### 8.1 PublisherOptions / SubscriberOptions

```cpp
struct PublisherOptions
{
};

struct SubscriberOptions
{
};
```

两者继承 parent Context 的 immutable `RuntimeMode`，V1 不提供 endpoint-scoped
RuntimeMode override。

### 8.2 Publisher API

```cpp
class Publisher
{
public:
    ~Publisher() noexcept;

    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    Publisher(Publisher&&) = delete;
    Publisher& operator=(Publisher&&) = delete;

    Result<void>
    publish(
        const void* message);

    std::string_view
    topic_name() const noexcept;

    const MessageType&
    message_type() const noexcept;

    Result<std::size_t>
    matched_subscriber_count() const;

    Result<std::unique_ptr<Event>>
    create_event(
        EventType type);

private:
    friend class Node;

    class Impl;

    explicit Publisher(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
```

### 8.3 Subscriber API

```cpp
class Subscriber
{
public:
    ~Subscriber() noexcept;

    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;

    Subscriber(Subscriber&&) = delete;
    Subscriber& operator=(Subscriber&&) = delete;

    Result<bool>
    read(
        void* message,
        MessageInfo& info);

    std::string_view
    topic_name() const noexcept;

    const MessageType&
    message_type() const noexcept;

    Result<std::size_t>
    matched_publisher_count() const;

    Result<std::unique_ptr<Event>>
    create_event(
        EventType type);

private:
    friend class Node;

    class Impl;

    explicit Subscriber(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
```

### 8.4 非阻塞接收结果

```text
Result error    -> operation failed
success + true  -> data returned
success + false -> no sample currently available
```

`false` 不是 Error。

### 8.5 MessageInfo

```cpp
struct MessageInfo
{
    Gid writer_gid{};

    std::int64_t writer_timestamp{0};

    std::uint64_t to_writer_sequence{0}; // 0 = unknown/unavailable

    std::int64_t reader_timestamp{0};
};
```

字段语义固定为：

- `writer_timestamp`：middleware-provided source timestamp；不可获得或不可安全表示时为 `0`；
- `reader_timestamp`：middleware-provided receive/reception timestamp；不可获得或不可安全表示时为 `0`，不得用本地 `steady_clock` 伪造；
- `writer_gid`：middleware publication identity；不可可靠获得时为 `Gid{}`；
- `to_writer_sequence`：middleware publication/sample sequence；unknown/unavailable/不可安全表示时为 `0`。

具体 Fast DDS 字段来源与转换只属于 Fast DDS 实现规格。

### 8.6 matched Subscriber Count

```cpp
Result<std::size_t>
Publisher::matched_subscriber_count() const;
```

返回：

> 当前 DDS 层与该 DataWriter 已建立兼容 match 的 DataReader 数量。

它不是：

- ROS Graph count；
- 同名 Topic count；
- discovery 历史累计数。

### 8.7 matched Publisher Count

```cpp
Result<std::size_t>
Subscriber::matched_publisher_count() const;
```

返回当前 compatible matched DataWriter 数量。

### 8.8 Match 条件

至少要求：

```text
same DDS domain
same resolved DDS topic name
same wire type
compatible QoS
discovery completed
```

才计入 matched count。

### 8.9 Matched Count 时间语义

matched count 是：

`query-time snapshot`

DDS discovery 异步，因此：

```text
create endpoint
    ↓
immediate matched_count()
```

允许暂时观察旧值。

V1 不承诺等待 discovery convergence 的同步 API。

### 8.10 Matched Count 并发

允许与：

```text
publish
take
DDS discovery callbacks
```

并发。

内部必须：

- 使用 Fast DDS thread-safe query；
- 或 listener 更新 atomic / synchronized state。

DMW listener 不执行 user callback。

<a id="dmw-service"></a>

## 9. Client 与 Server

### 9.1 Service DDS 结构

```text
Client
├── request DataWriter
└── response DataReader

Server
├── request DataReader
└── response DataWriter
```

Client / Server 是一个整体 public primitive。

不得把四个 endpoint 分散给上层自行组合。

### 9.2 ClientOptions / ServerOptions

```cpp
struct ClientOptions
{
};

struct ServerOptions
{
    std::size_t max_pending_requests{1024};
};
```

Client/Server 继承 parent Context 的 immutable `RuntimeMode`，V1 不提供
service-endpoint-scoped RuntimeMode override。

`max_pending_requests` 必须大于 0，否则 `create_server()` 返回
`InvalidArgument`。

### 9.3 RequestId

V1 改为 runtime-mode-neutral：

```cpp
struct RequestId
{
    Gid client_gid{};

    std::int64_t sequence_number{0};
};
```

提供：

```cpp
bool operator==(
    const RequestId&,
    const RequestId&) noexcept;

struct RequestIdHash
{
    std::size_t operator()(
        const RequestId&) const noexcept;
};
```

不再把字段命名为：

`writer_gid`

因为 `ROS2` 的 service workaround 中相关 GID 可能表示 Client response reader，而非 request writer。

### 9.4 Client API

```cpp
class Client
{
public:
    ~Client() noexcept;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    Result<RequestId>
    write_request(
        const void* request);

    Result<bool>
    read_response(
        void* response,
        RequestId& request_id);

    Result<bool>
    service_is_available() const;

    std::string_view
    service_name() const noexcept;

private:
    friend class Node;

    class Impl;

    explicit Client(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
```

### 9.5 Server API

```cpp
class Server
{
public:
    ~Server() noexcept;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    Result<bool>
    read_request(
        void* request,
        RequestId& request_id);

    Result<void>
    write_response(
        const RequestId& request_id,
        const void* response);

    std::string_view
    service_name() const noexcept;

private:
    friend class Node;

    class Impl;

    explicit Server(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
```

### 9.6 Server Pending Request Contract

当：

```cpp
read_request(...)
```

成功返回：

`Taken`

后，Server 内部注册：

`RequestId -> Pending`

。

因此：

```cpp
server.write_response(id, response);
```

只允许对：

`当前该 Server Pending Request`

调用。

### 9.7 Unknown / Duplicate RequestId

如果：

`RequestId 不属于当前 pending set`

包括：

- 从未由该 Server take；
- 已经成功 response；
- 属于另一个 Server；

则：

`ErrorCode::NotFound`

。

DMW V1 不额外保留无限期 responded tombstone。

因此：

> 已成功响应过的 RequestId 再次使用，与任意 unknown RequestId 一样返回 `NotFound`。

这里的“再次使用”指应用再次调用 `write_response()`；它不表示 DMW 永久保存该
RequestId 的 transport-level 去重记录。

### 9.8 Concurrent write_response

Server 内部 request state：

```text
Pending
    ↓
Responding
    ↓
response write success
    ↓
removed
```

第二个线程对同一个：

`Responding RequestId`

调用 `write_response()`：

`Busy`

。

如果 response write：

```text
DDSError
or
Timeout
```

则状态恢复：

`Responding -> Pending`

允许应用重试。

### 9.9 Duplicate DDS Request Sample

如果底层重复收到相同 RequestId，而该 ID 已存在 Pending：

- 不再次向 public API 返回同一个 request；
- 该 sample 被视为重复 transport sample；
- DMW 继续寻找下一个可用 request；
- 没有其他 request 时返回 `NoData`。

V1 只保证 RequestId 处于 Pending/Responding 生命周期期间的 duplicate
suppression。成功响应并移除 pending entry 后，V1 不保证对未来再次出现的同一
DDS request sample 提供永久 at-most-once delivery；DDS/RTPS 正常可靠传输负责
transport-level duplicate handling，DMW 不维护无界 tombstone 数据库。

### 9.10 Pending Request 资源上限

在调用 Fast DDS `DataReader::take()` 之前，Server 必须在与 pending state 相同的
同步域内原子预留一个 capacity slot。如果 pending/Responding entry 与已预留 slot
之和已经达到：

`ServerOptions::max_pending_requests`

则 `read_request()` 直接返回：

`ErrorCode::ResourceExhausted`

且不得从 DDS history 取出新的 request，从而形成有限 backpressure。处于
`Responding` 的 entry 也计入该上限。

Fast DDS take 返回 `NoData` 或 Error 时必须释放预留 slot；取得 duplicate sample
并抑制后也释放该 slot再继续查找；取得新 request 时才把 reservation 转换为
Pending entry。由此多个并发 `read_request()` 也不得使容量超过配置上限。

Server 析构时清空全部 pending state。Context 进入 `ShuttingDown` 后，pending
request 不再可响应，`write_response()` 按全局错误优先级返回
`ContextShutdown`；最终资源销毁时清空这些 entry。

### 9.11 Client Response Filtering

DMW Client 必须过滤：

`not addressed to this Client`

的 response。

但 DMW V1：

> **不维护 dclcpp 层的 future/outstanding-request table。**

因此：

`read_response()`

只保证：

- response 属于当前 Client identity；
- RequestId 正确解析。

它不保证：

`RequestId 当前仍存在于某个 dclcpp pending future table`

。

Future / timeout / pending request 生命周期属于：

`dclcpp::Client`

。

### 9.12 Service Availability

```cpp
Result<bool>
Client::service_is_available() const;
```

结果：

```text
success + true
    -> compatible Server candidate available

success + false
    -> no compatible Server candidate

error
    -> discovery query failed
```

### 9.13 Service Availability Pairing

禁止只判断：

```text
request matched count > 0
AND
response matched count > 0
```

因为两个 endpoint 可能来自不同 remote participant。

DMW 必须维护：

`ServiceDiscoveryRegistry`

按照 remote participant identity 配对。

Candidate key：

```text
service request DDS topic
service response DDS topic

request wire type
response wire type

remote participant GUID prefix
```

仅当同一个 remote participant：

```text
has compatible request DataReader
AND
has compatible response DataWriter
```

时，该 participant 才计为一个 available Server candidate。

### 9.14 Same-participant 限制

ROS 2 Humble Fast DDS wire discovery 并没有为 request DataReader / response DataWriter 暴露一个 DMW 可以直接使用的标准 service-instance pair ID。

因此 V1 对同一 remote Participant 内多个同名 Server 的定义是：

> 同一 Participant 中只要同时存在至少一个 compatible request reader 和一个 compatible response writer，就认为该 Participant 对该 Service 可用。

DMW 不声称能够在没有完整 ROS Graph /额外 discovery metadata 的情况下证明两个 endpoint 属于同一个具体 Server object。

这属于 V1 明确且可测试的 availability contract，而不是未定义行为。

### 9.15 Service Availability Discovery Race

endpoint discovery 是异步的。

因此 Server 创建或销毁期间：

`service_is_available()`

允许短暂返回旧 snapshot。

但不得发生：

```text
request side from Participant A
+
response side from Participant B
=
true
```

这种跨 Participant 假阳性。

<a id="dmw-waitset"></a>

## 10. WaitSet、GuardCondition 与 Event

本章统一定义同步模型：WaitSet 观察 waitable，Registration 协调并发 attach/detach，GuardCondition 使用 coalesced trigger，Event 使用独立 cursor 的 level-triggered readiness。WaitSet 不拥有注册实体。

### 10.1 Waitable Entity

V1：

```text
Subscriber
Client
Server
Event
GuardCondition
```

可以加入 WaitSet。

Publisher 不直接 waitable。

### 10.2 WaitTimeout

V1 不使用：

- magic `-1`；
- `nanoseconds::max()` sentinel；
- `optional<duration>`；

表达 infinite wait。

唯一 representation：

```cpp
class WaitTimeout
{
public:
    enum class Kind
    {
        Poll,
        Finite,
        Infinite
    };

    static WaitTimeout
    poll() noexcept;

    static Result<WaitTimeout>
    finite(
        std::chrono::nanoseconds timeout);

    static WaitTimeout
    infinite() noexcept;

    Kind kind() const noexcept;

    std::chrono::nanoseconds
    duration() const noexcept;

private:
    // ...
};
```

`duration()` 的前置条件是 `kind() == Kind::Finite`。对 Poll 或 Infinite 调用属于
programming contract violation，必须调用 `std::terminate()`。

### 10.3 WaitTimeout 输入规则

```cpp
WaitTimeout::finite(timeout)
```

要求：

`timeout > 0`

。

`timeout == 0`

不表示 finite timeout，应显式：

```cpp
WaitTimeout::poll()
```

。

负值：

`InvalidArgument`

。

### 10.4 WaitSet API

```cpp
struct WaitSetOptions
{
};

class WaitSet
{
public:
    ~WaitSet() noexcept;

    WaitSet(const WaitSet&) = delete;
    WaitSet& operator=(const WaitSet&) = delete;

    WaitSet(WaitSet&&) = delete;
    WaitSet& operator=(WaitSet&&) = delete;

    Result<WaitableRegistration>
    add(Subscriber&);

    Result<WaitableRegistration>
    add(Client&);

    Result<WaitableRegistration>
    add(Server&);

    Result<WaitableRegistration>
    add(Event&);

    Result<WaitableRegistration>
    add(GuardCondition&);

    Result<void>
    remove(WaitableRegistration registration);

    Result<WaitResult>
    wait(WaitTimeout timeout);

private:
    friend class Context;

    class Impl;

    explicit WaitSet(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
```

WaitSet 只能注册与自身属于同一 Context 的 waitable。跨 Context `add()` 返回
`InvalidArgument`；该 argument error 优先于 Context state。失败不得创建 Registration、
消耗 registration ID、修改 WaitSet topology 或修改 waitable registration state。

### 10.5 Finite Wait Deadline

finite wait 在进入 `wait()` 时，只计算一次 deadline：

```text
deadline =
steady_clock::now()
+
timeout
```

所有内部 topology wakeup：

```text
add
remove
auto-detach
control GuardCondition
```

都不得重新开始完整 timeout。

内部重新进入 Fast DDS WaitSet wait 前：

```text
remaining =
deadline - steady_clock::now()
```

。

如果：

`remaining <= 0`

返回：

`WaitStatus::Timeout`

。

### 10.6 Clock

finite timeout 必须使用：

```cpp
std::chrono::steady_clock
```

或等价 monotonic clock。

不得使用 system wall clock。

Humble RMW 也明确建议 wait timeout 基于 monotonic clock；这里将该建议提升为 DMW V1 的强制规范。

### 10.7 Deadline Overflow

计算：

`now + timeout`

如果超出 `steady_clock::time_point::max()`：

`deadline = time_point::max()`

采用 saturating arithmetic，不允许整数 overflow。

### 10.8 WaitableRegistration

```cpp
enum class WaitableKind
{
    Subscriber,
    Client,
    Server,
    Event,
    GuardCondition
};

class WaitableRegistration
{
public:
    WaitableRegistration() noexcept = default;

    bool valid() const noexcept;

    WaitableKind kind() const noexcept;

    friend bool operator==(
        const WaitableRegistration&,
        const WaitableRegistration&) noexcept;

private:
    friend class WaitSet;

    std::uint64_t wait_set_id_{0};
    std::uint64_t registration_id_{0};

    WaitableKind kind_{
        WaitableKind::Subscriber
    };
};
```

### 10.9 Invalid Token

```text
wait_set_id == 0
or
registration_id == 0
```

表示 invalid token。

`WaitSet::add()` 永不返回 invalid token。

### 10.10 WaitableRegistration Scope

WaitableRegistration 只对创建它的 WaitSet 有效。

例如：

```cpp
auto token =
    wait_set_a->add(*subscriber).value();

wait_set_b->remove(token);
```

返回：

`InvalidArgument`

。

### 10.11 Token Generation

每个 WaitSet：

`registration_id`

单调递增且不复用。

每个 WaitSet 自身：

`wait_set_id`

来自 process-wide 单调唯一 ID allocator。

在当前进程生命周期：

> wait_set_id 和 registration_id 均不得复用。

如果 64 位 ID 空间耗尽：

`ResourceExhausted`

。

### 10.12 Stale Token

如果 token 曾合法，但 registration 已：

```text
remove
or
auto-detach
```

则：

```cpp
wait_set->remove(token);
```

返回：

`NotRegistered`

。

### 10.13 WaitResult Snapshot

```cpp
enum class WaitStatus
{
    Ready,
    Timeout
};

class WaitResult
{
public:
    WaitStatus status() const noexcept;

    const std::vector<WaitableRegistration>&
    ready() const noexcept;

private:
    friend class WaitSet;

    static WaitResult timeout();

    static WaitResult ready(
        std::vector<WaitableRegistration> registrations);

    WaitStatus status_{WaitStatus::Timeout};
    std::vector<WaitableRegistration> ready_;
};
```

`WaitResult` 不可由普通调用者构造。它始终满足：

```text
status() == Ready
    -> ready().size() >= 1

status() == Timeout
    -> ready().empty()
```

内部调用 `WaitResult::ready(empty_tokens)` 属于 programming contract violation，
必须 terminate/assert，而不是生成非法 snapshot。

WaitResult 表示：

> wait readiness snapshot 形成时的状态。

返回后另一个线程可能立即 detach 对应 registration。

因此：

`WaitResult contains token`

不保证：

`registration 在调用者读取它时仍然存在`

。

DMW 不允许通过 WaitableRegistration 解引用 public entity。

允许 `wait()` 已形成 Ready snapshot 后，并发 `remove(registration)` 成功返回，再由 wait 把旧 registration
交给 caller。`remove()` 返回后必须保证实现层不会因该旧 snapshot 访问已释放的
Registration/Waitable/DDS entity Info；它不撤回已经形成的 value snapshot。

dclcpp Executor 必须维护自己的：

`WaitableRegistration -> weak/high-level registration`

映射，并处理 stale registration。

### 10.14 一个 Waitable 一个 WaitSet

V1：

> 一个 waitable 同一时刻最多属于一个 WaitSet。

第二次 add：

`AlreadyRegistered`

。

### 10.15 单 Active Wait

同一 WaitSet 同时最多一个：

`active wait()`

。

第二个并发 wait：

`Busy`

。

### 10.16 add/remove 与 wait

允许：

```text
add/remove
     ↕
wait
```

并发。

WaitSet 具有 private control GuardCondition。

Topology mutation：

```text
add/remove
   ↓
trigger control condition
   ↓
wake Fast DDS WaitSet wait
   ↓
rebuild snapshot
   ↓
continue remaining original timeout
```

control wakeup 不直接返回给 user。

### 10.17 Non-owning Registration

WaitSet 不拥有 waitable。

公开：

```cpp
wait_set->add(*subscriber);
```

内部不能长期保存：

`Subscriber*`

作为唯一 registration identity。

内部采用：

```text
WaitableState
RegistrationState
```

。

### 10.18 RegistrationState

```text
Attached
   ↓
Detaching
   ↓
Detached
```

以下三条路径共用同一 detach mechanism：

```text
explicit remove
waitable destructor
WaitSet destructor
```

只有一个线程执行真实 detach。

### 10.19 Waitable 先析构

```text
waitable Closing
      ↓
invalidate registration
      ↓
trigger WaitSet control
      ↓
drain active wait reference
      ↓
detach Fast DDS condition
      ↓
destroy DDS endpoint
```

waitable 析构必要时可以短暂阻塞。

析构返回后不得存在悬空：

```text
public facade pointer
WaitableState
Fast DDS Condition
```

引用。

### 10.20 WaitSet 先析构

WaitSet 析构的前置条件是：

`该 WaitSet 没有 active wait()`

V1 不支持同一个 WaitSet facade 的析构与其自身 `wait()` 并发。Executor 关闭顺序
必须是：

```text
trigger control GuardCondition / Context shutdown
        ↓
wait() exits
        ↓
join or otherwise synchronize wait thread
        ↓
destroy WaitSet
```

满足前置条件后，析构流程为：

```text
WaitSet destructor
      ↓
invalidate registration
      ↓
detach Fast DDS conditions
      ↓
destroy Fast DDS WaitSet
```

waitable 继续有效。

### 10.21 GuardCondition Public API

```cpp
struct GuardConditionOptions
{
};

class GuardCondition
{
public:
    ~GuardCondition() noexcept;

    GuardCondition(
        const GuardCondition&) = delete;

    GuardCondition&
    operator=(
        const GuardCondition&) = delete;

    GuardCondition(
        GuardCondition&&) = delete;

    GuardCondition&
    operator=(
        GuardCondition&&) = delete;

    Result<void>
    trigger();

private:
    friend class Context;

    class Impl;

    explicit GuardCondition(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
```

V1 不提供：

```cpp
reset();
```

。

### 10.22 GuardCondition 触发模型

Fast DDS GuardCondition 的 trigger value 由应用设置，并持续保持直到再次修改；因此 DMW 必须在其上定义自己的消费语义。

DMW V1 定义：

> **pending-trigger + automatic consume semantics**

内部至少维护：

```text
trigger_generation
consumed_generation
```

。

### 10.23 Trigger 合并

每次：

```cpp
guard.trigger();
```

递增：

`trigger_generation`

。

如果多次 trigger 发生在一次消费之前：

```text
trigger
trigger
trigger
```

V1 合并为：

`一次 WaitResult ready`

而不是三个 ready event。

GuardCondition 不是计数 semaphore。

### 10.24 Trigger Before Registration

```cpp
guard->trigger();

wait_set->add(*guard);

wait_set->wait(...);
```

下一次 wait 必须立即观察到 GuardCondition ready。

即：

> pending trigger 独立于 WaitSet registration 生命周期。

### 10.25 自动消费

当 WaitSet 确定 GuardCondition ready 时：

1. snapshot 当前 `trigger_generation`；
2. 将 token 放入 WaitResult；
3. 在返回 WaitResult 前，把 `consumed_generation` 推进到 snapshot generation；
4. 如果没有新 trigger，Fast DDS GuardCondition reset 为 false；
5. 如果期间出现新 trigger，则保持/重新设置 Fast DDS GuardCondition trigger，使下一次 wait 仍能观察到 readiness。

因此：

> WaitResult 返回后，该次已报告 readiness 已经消费。

### 10.26 Trigger / Consume Race

禁止：

```text
Wait thread decides ready
Thread B trigger()
Wait thread reset false
Thread B trigger lost
```

。

实现必须使用：

- mutex；
- generation counter；
- atomic state machine；

之一保证：

> trigger 与 consume 并发时新 trigger 永不丢失。

### 10.27 Context Shutdown 与 GuardCondition

Context 已：

```text
ShuttingDown
or
Shutdown
```

以后：

```cpp
guard.trigger()
```

返回：

`ContextShutdown`

且不产生新的 pending trigger。

### 10.28 GuardCondition logical commit

`GuardCondition::trigger()` 的 success linearization point 是 coalescing logical pending trigger
的 commit。argument/Context/object validation 在此之前失败时返回对应 Error 且不产生
trigger；一旦 logical commit 成功，public result 必须是 success。Fast DDS wake 只是通知机制，
其 ReturnCode failure 或 exception 不得回滚 logical pending，也不得把已提交的 trigger 改报为
operation failure。实现可以进入 degraded notification mode，但必须依靠 bounded
wake/recheck 使 readiness 最终可观察。

<a id="dmw-event"></a>

### 10.29 Event 模型

#### 10.29.1 EventType

```cpp
enum class EventType
{
    // Subscriber
    LivelinessChanged,
    RequestedDeadlineMissed,
    RequestedIncompatibleQos,
    MessageLost,

    // Publisher
    LivelinessLost,
    OfferedDeadlineMissed,
    OfferedIncompatibleQos
};
```

matched state 不通过 Event 表达。

#### 10.29.2 EventInfo

```cpp
struct DeadlineMissedInfo
{
    std::int32_t total_count{0};
    std::int32_t total_count_change{0};
};

struct LivelinessLostInfo
{
    std::int32_t total_count{0};
    std::int32_t total_count_change{0};
};

struct LivelinessChangedInfo
{
    std::int32_t alive_count{0};
    std::int32_t not_alive_count{0};

    std::int32_t alive_count_change{0};
    std::int32_t not_alive_count_change{0};
};

enum class QosPolicyKind
{
    Unknown,
    History,
    Reliability,
    Durability,
    Deadline,
    Lifespan,
    Liveliness
};

struct IncompatibleQosInfo
{
    std::int32_t total_count{0};
    std::int32_t total_count_change{0};

    QosPolicyKind last_policy{
        QosPolicyKind::Unknown
    };
};

struct MessageLostInfo
{
    std::size_t total_count{0};
    std::size_t total_count_change{0};
};

using EventInfo =
    std::variant<
        DeadlineMissedInfo,
        LivelinessLostInfo,
        LivelinessChangedInfo,
        IncompatibleQosInfo,
        MessageLostInfo>;
```

#### 10.29.3 Event API

```cpp
class Event
{
public:
    ~Event() noexcept;

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    EventType type() const noexcept;

    Result<bool>
    take(EventInfo& info);

private:
    friend class Publisher;
    friend class Subscriber;

    class Impl;

    explicit Event(
        std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
```

Event 与其他 Resource/Entity 使用相同内部生命周期：

```text
Publisher/Subscriber::create_event()
        ↓
Event::Impl::create()
        ↓
complete Event

Event::~Event()
        ↓
Event::Impl::~Impl()
        ↓
private noexcept idempotent Event::Impl::destroy()
```

#### 10.29.4 EventType 合法组合

合法组合固定为：

| Parent | EventType |
| --- | --- |
| Publisher | `LivelinessLost` |
| Publisher | `OfferedDeadlineMissed` |
| Publisher | `OfferedIncompatibleQos` |
| Subscriber | `LivelinessChanged` |
| Subscriber | `RequestedDeadlineMissed` |
| Subscriber | `RequestedIncompatibleQos` |
| Subscriber | `MessageLost` |

EventType 本身受支持但与 parent kind 不匹配时，`create_event()` 返回
`InvalidArgument`。`Unsupported` 只用于当前 Fast DDS version/RuntimeMode 根本不支持该能力的
情况。Factory 失败时不创建半有效 Event。

#### 10.29.5 Event Parent

Event 永久绑定：

```text
one Publisher/Subscriber
+
one EventType
```

。

如果 parent endpoint 被销毁：

```text
invalidate EventState
    ↓
auto-detach WaitSet
    ↓
release Fast DDS condition
```

之后：

```cpp
event->take(...)
```

在 Context Active 时返回：

`ParentDestroyed`

。

Context shutdown 优先于 ParentDestroyed。

#### 10.29.6 Event history 与消费边界

Event 只观察自身 Factory commit 后发生的对应 event changes。Factory 在 parent EventSource
的同一同步域中把 cursor 初始化为 current cumulative state 并注册 EventState；commit 前的
history 不 replay，commit 后的 update 必须可观察。同一 parent/type 的多个 Event 各有独立
cursor，一个 Event 的 `take()` 不消费其它 Event。

Event readiness 是 level-triggered：`WaitSet::wait()` 只返回 ready token，不推进 cursor、
不构造或消费 `EventInfo`；只有 successful `Event::take()` 推进该 Event cursor。因此在 take
前重复 wait 仍可报告同一 Event ready。GuardCondition 保持独立的 coalesced
consume-on-wait 语义。

若实现层无法继续安全表示某个 parent/type 的 cumulative state，该 source/type 永久
exhausted：所有对应 Event 保持 logically ready，`Event::take()` 返回
`ResourceExhausted` 且 cursor 不变。全局错误优先级仍适用，Context/parent state error 高于
Event-local exhaustion。

<a id="dmw-registry-discovery"></a>

## 11. Registry 与 Discovery

### 11.1 TypeRegistry 是 V1 Mandatory

V1 不再使用：

`optional but recommended`

措辞。

每个 Context 必须具有 TypeRegistry。

### 11.2 TypeRegistry Key

主 key：

`DDS wire type name`

entry 保存：

```text
BindingIdentity
Fast DDS TypeSupport
reference count
```

。

### 11.3 TypeRegistry 冲突

已有：

```text
wire_type_name = X
binding_identity = A
```

再次注册：

```text
wire_type_name = X
binding_identity = A
```

：

`reuse`

。

再次注册：

```text
wire_type_name = X
binding_identity = B
```

：

`TypeMismatch`

。

### 11.4 TopicRegistry 是 V1 Mandatory

每个 Context 必须具有 TopicRegistry。

Key：

```text
resolved DDS topic name
+
DDS wire type name
```

Value：

```text
Topic handle
reference count
```

。

### 11.5 TopicRegistry Conflict

如果同一：

`DDS topic name`

已绑定：

`wire type A`

再次尝试：

`wire type B`

返回：

`TypeMismatch`

。

### 11.6 Discovery

V1 discovery 支持：

```text
Participant discovery
Publisher/Subscriber matching
matched count
Service endpoint matching
Service availability
```

。

不暴露完整 public ROS Graph。

### 11.7 ServiceDiscoveryRegistry

内部维护：

```text
ServiceKey
+
remote Participant identity
```

。

ServiceKey 至少包含：

```text
request DDS topic
response DDS topic
request wire type
response wire type
RuntimeMode
```

。

用于消除跨 Participant request/response 拼接导致的 availability 假阳性。

<a id="dmw-concurrency"></a>

## 12. 并发与对象销毁

### 12.1 Fast DDS Threads

Fast DDS 负责内部：

- discovery；
- transport；
- receive；
- listener。

DMW listener 不执行：

```text
user callback
Python callback
dclcpp callback
```

。

### 12.2 Listener 允许行为

只允许：

```text
update atomic state
update discovery registry
update matched count
mark readiness
trigger internal condition
record diagnostic
```

。

### 12.3 Public Operation 并发

V1 要求以下 API 可以被不同应用线程并发调用：

```text
Publisher::write()

Subscriber::read()

Publisher::matched_subscriber_count()

Subscriber::matched_publisher_count()

Client::write_request()

Client::read_response()

Client::service_is_available()

Server::read_request()

Server::write_response()
    — 不同 RequestId

GuardCondition::trigger()

Event::take()
```

。

同一 `RequestId` 的两个 `write_response()` 按 [Client/Server contract](#dmw-service) 返回 `Busy`。

### 12.4 Object Destruction

同一 public facade：

```text
ordinary API call
↕
destructor
```

不允许调用者无同步并发。

例如：

```text
publish()
↕
publisher.reset()
```

调用者必须同步。

### 12.5 WaitSet 特殊并发

以下场景由 DMW 自己保证：

```text
WaitSet::wait()
↕
registered waitable destruction
```

。

调用者不需要先 remove。

该特例不包括：

```text
WaitSet::wait()
↕
同一 WaitSet::~WaitSet()
```

后者仍遵守 15.4；调用者必须先唤醒并同步 wait thread，再析构 WaitSet。

### 12.6 Shutdown 并发

允许：

```text
shutdown
↕

publish
take
matched count
service operations
wait
```

。

每个操作：

- 要么在线性化点之前完成正常操作；
- 要么观察 Context 已关闭并返回 ContextShutdown。

不得：

```text
UAF
deadlock
half-destroyed DDS resource access
```

。

### 12.7 Factory 并发

允许：

```text
Context::create_node
Context::create_wait_set
Context::create_guard_condition

Node::create_publisher
Node::create_subscriber
Node::create_client
Node::create_server
```

来自不同线程并发调用。

Registry 必须同步。

<a id="dmw-ros2-interop"></a>

## 13. ROS 2 Fast DDS Wire Contract

### 13.1 Compatibility Baseline

`ROS2` 不是模糊的“兼容 Humble”。

V1 compatibility test baseline 固定为：

| 组件 | Release | Upstream Git ref |
| --- | --- | --- |
| OS | Ubuntu 22.04 | Jammy image digest 由 CI baseline manifest 冻结 |
| ROS 2 | Humble | ROS apt snapshot 由 CI baseline manifest 冻结 |
| `rmw` | 6.1.3 | `6.1.3` |
| `rmw_fastrtps_cpp` | 6.2.10 | `6.2.10` |
| `rosidl_typesupport_fastrtps_cpp` | 2.2.4 | `2.2.4` |
| Fast DDS | 2.6.12 | `v2.6.12` |
| Fast CDR | 1.0.29 | `v1.0.29` (`959ff6c`) |

截至 2026 年 8 月，ROS Index 中 Humble 的 `rmw` 为 6.1.3、
`rmw_fastrtps_cpp` 为 6.2.10、Fast DDS Humble 线为 2.6.x 且当前发布版本为
2.6.12，Humble 的 `rosidl_typesupport_fastrtps_cpp` 为 2.2.4；Fast CDR 使用
官方 `v1.0.29` release。

每次 interoperability CI 必须生成并归档 baseline manifest，至少记录：

```text
upstream Git ref
resolved full Git commit
apt repository snapshot/date
dpkg-query 的完整 Debian package revision
OS image digest
CPU architecture
compiler version
```

表中的 release/ref 与归档 manifest 共同构成可复现 baseline；只有 semver 而没有
对应 manifest 的测试结果不得用于扩大 compatibility guarantee。

更新任何 baseline dependency 时：

> 必须重新运行[测试与验收](#dmw-verification)中的全部 ROS interoperability tests。

未经重新验证，不自动扩展 `ROS2` compatibility guarantee。

### 13.2 Source of Truth

RuntimeMode 是以下三部分共同定义的可验证契约：

1. 本文档明确冻结的规范；
2. baseline manifest 固定的 reference implementation，包括
   `rmw_fastrtps_cpp`、`rosidl_typesupport_fastrtps_cpp`、Fast DDS 和 Fast CDR；
3. [测试与验收](#dmw-verification)中 interoperability tests 的可复现结果。

三者不存在“文档无条件覆盖实现或测试”的简单优先级。任意两者冲突都属于：

`RuntimeModeDefect`

必须先修复规范、RuntimeMode implementation 或 test，使三者重新一致；冲突状态下
不得把新的 baseline 标记为 Frozen，也不得声称已验证 wire compatibility。

ROS 2 官方命名规范定义了 `rt`、`rq`、`rr` 等 DDS namespace prefix；`rmw_fastrtps` 官方文档也展示了诸如 `rq/add_two_intsRequest` 和 `rr/add_two_intsReply` 的实际 service endpoint 名称。

### 13.3 ROS Name Validation

`ROS2` 模式下 DMW V1 接受：

- absolute name；
- relative name。

V1 不支持由 DMW 自己展开：

```text
~
{}
substitution
```

。

这些更高层 name expansion 属于 dclcpp。

### 13.4 Node Namespace Normalization

`NodeOptions::node_namespace`：

```text
empty
    -> "/"

"robot"
    -> "/robot"

"/robot"
    -> "/robot"
```

除 root `/` 外，最终 namespace 不带 trailing slash。

以下属于 InvalidName：

```text
//
/foo/
// repeated slash
empty path token
unsupported "~"
unsupported "{...}"
```

。

### 13.5 Logical Name → FQN

用户名字：

#### 13.5.1 Absolute name

`/foo`

直接：

`/foo`

。

#### 13.5.2 Relative name

Node namespace：

`/robot`

topic：

`camera`

解析：

`/robot/camera`

。

### 13.6 Topic DDS Name

ROS Topic：

`/foo`

映射：

`rt/foo`

。

ROS Topic：

`/robot/camera`

映射：

`rt/robot/camera`

。

### 13.7 Service Request DDS Topic

Service FQN：

`/add_two_ints`

request DDS topic：

`rq/add_two_intsRequest`

。

一般形式：

`"rq" + service_fqn + "Request"`

其中 `service_fqn` 以 `/` 开头。

### 13.8 Service Response DDS Topic

同一 Service：

`rr/add_two_intsReply`

一般形式：

`"rr" + service_fqn + "Reply"`

。

### 13.9 DDS Type Name

DMW V1 不在 runtime 根据：

`package/msg/Type`

字符串自己发明 DDS type name。

MessageType binding 必须已经包含 exact DDS wire type。

例如：

`std_msgs::msg::dds_::String_`

。

ROS 2 Fast DDS 使用这种 mangled DDS type naming；eProsima 的 ROS 2/DDS tooling 也以 `std_msgs::msg::dds_::String_` 作为典型 ROS 2 DDS type。

### 13.10 ROS Service Request/Response Type

例如：

`example_interfaces/srv/AddTwoInts`

request MessageType 应对应：

`example_interfaces::srv::dds_::AddTwoInts_Request_`

response MessageType 应对应：

`example_interfaces::srv::dds_::AddTwoInts_Response_`

具体 wire type name 由 ROS-compatible type binding 提供。

### 13.11 CDR Representation

ROS2 Service payload：

> request / response message 本身直接按照相应 ROS Fast DDS message type CDR 序列化。

V1 不在 user payload 中额外插入：

```text
RequestId wrapper
client GUID field
sequence field
```

。

Service correlation metadata 使用：

```text
DDS SampleIdentity
related_sample_identity
```

。

Humble `rmw_fastrtps` request/response 路径本身使用 Fast CDR `DDS_CDR` representation，并通过 sample identity metadata 完成 request/response correlation。

### 13.12 Client Request Identity

在 `ROS2` 模式：

Client 拥有：

```text
request DataWriter GUID = request_writer_gid
response DataReader GUID = response_reader_gid
```

发送 request 前：

```text
WriteParams.related_sample_identity.writer_guid
    =
response_reader_gid
```

。

Fast DDS 成功 write 后生成：

```text
sample_identity.writer_guid
    =
request_writer_gid

sample_identity.sequence_number
    =
request sequence
```

Humble `rmw_fastrtps` 正是把 response reader GUID 放进 request 的 `related_sample_identity.writer_guid`，并从 write 后的 sample identity 得到 sequence number。

### 13.13 DMW `write_request()` 返回值

DMW 对外标准化：

```cpp
RequestId {
    client_gid = response_reader_gid,
    sequence_number = request_sequence
}
```

这样：

```text
Client send
Server take
Server response
Client take
```

四个阶段使用同一 normalized RequestId。

### 13.14 Server Take Request

Server 收到 request 后读取：

```text
SampleInfo.sample_identity
SampleInfo.related_sample_identity
```

。

如果：

`related_sample_identity.writer_guid`

不是 unknown：

```text
client_gid =
related_sample_identity.writer_guid
```

。

否则 fallback：

```text
client_gid =
sample_identity.writer_guid
```

。

sequence：

`sample_identity.sequence_number`

。

Humble `rmw_fastrtps` 也采用这一兼容处理：当 related identity 中存在 response reader GUID 时，用它替换 request identity 中的 writer GUID，以便后续正确将 response 定向回 Client。

### 13.15 Server Send Response

Server response：

```text
WriteParams.related_sample_identity.writer_guid
    =
request_id.client_gid

WriteParams.related_sample_identity.sequence_number
    =
request_id.sequence_number
```

。

随后发送 response payload。

Humble `rmw_fastrtps` 的 `rmw_send_response` 也是把 request header GUID 和 sequence number写入 response `related_sample_identity`。

### 13.16 Response-reader Discovery Workaround

如果：

`request_id.client_gid`

表示 response DataReader GUID，则 Server response DataWriter 在写 response 前必须确保该 reader 已匹配。

`ROS2` V1 固定：

`service response discovery timeout = 100 ms`

该值来自 Fast DDS 2.6.12 `ReliabilityQosPolicy::max_blocking_time` baseline
default。V1 Qos 尚未公开 reliability max blocking time，因此这里不从用户 Qos
读取；未来公开该 policy 时再以 resolved response-writer QoS 为准。

等待语义固定为：

`deadline = steady_clock::now() + 100 ms`

- 使用 monotonic `steady_clock`；
- discovery/control wake 不重置原始 deadline；
- Context 进入 `ShuttingDown` 必须立即中断并返回 `ContextShutdown`；
- 同一 Server facade 的析构与 `write_response()` 不得并发；
- 如果等待期间 discovery registry 已确认目标 Client response reader 消失，则按
  Humble workaround 语义视为 response 已不再需要发送并返回 success；
- 否则 deadline 到期且目标仍未匹配时返回 `Timeout`。

超时：

`ErrorCode::Timeout`

。

这是 Humble `rmw_fastrtps` 为 service request/reply discovery race 使用的兼容行为。

### 13.17 Client Take Response

Client 获取 response：

`response.related_sample_identity`

。

合法 response 至少要求相关 GUID 对应当前 Client：

```text
response_reader_gid
or
request_writer_gid
```

。

不属于当前 Client 的 response：

- 不返回给上层；
- 丢弃/忽略；
- 继续读取后续 sample。

Humble `rmw_fastrtps` 的 response take 同样会根据 Client 自己的 reader/writer GUID 过滤 response。

返回：

```cpp
RequestId {
    client_gid = response_reader_gid,
    sequence_number =
        related_sample_identity.sequence_number
}
```

。

### 13.18 Service QoS Contract

标准 ROS Service compatibility test 使用：

```text
History      KeepLast
Depth        10
Reliability  Reliable
Durability   Volatile

Deadline     default
Lifespan     default

Liveliness   SystemDefault
Lease        default
```

。

对应 DMW：

```cpp
Qos::ros2_services_default();
```

。

### 13.19 Fast DDS Implementation Defaults

为了与固定 `rmw_fastrtps_cpp` baseline 行为尽量一致，ROS2 integration 实现应以固定版本的 rmw_fastrtps 行为为参考，包括：

- Fast DDS entity QoS mapping；
- publication behavior；
- history memory policy；
- data sharing baseline。

当前官方 `rmw_fastrtps` 文档说明默认使用 synchronous publication mode，并设置 Fast DDS-specific history/data-sharing 行为；这些属于实现 baseline，而不是独立 public DMW QoS policy。

### 13.20 Wire Compatibility 与 Graph Compatibility

```text
ROS wire compatibility
        ≠
ROS Graph compatibility
```

V1 保证：

```text
Topic data exchange
Service request/reply
matched DDS endpoint
```

。

不保证：

```text
ros2 node list
ros2 topic list
ros2 service list
```

完整呈现 DMW logical Node。

<a id="dmw-verification"></a>

## 14. 实现边界、测试与验收

本章只规定代码组织边界和公共行为验收，不新增运行时语义。测试必须引用前述正文契约；若测试描述与正文冲突，以正文为准并修复测试。

### 14.1 目录结构

```text
dmw/
├── CMakeLists.txt
│
├── include/
│   └── dmw/
│       ├── context.hpp
│       ├── node.hpp
│       │
│       ├── message_type.hpp
│       ├── service_type.hpp
│       │
│       ├── publisher.hpp
│       ├── subscriber.hpp
│       ├── client.hpp
│       ├── server.hpp
│       │
│       ├── qos.hpp
│       ├── runtime_mode.hpp
│       │
│       ├── gid.hpp
│       ├── message_info.hpp
│       ├── request_id.hpp
│       ├── wait_set.hpp
│       ├── wait_timeout.hpp
│       ├── waitable_registration.hpp
│       ├── wait_result.hpp
│       ├── guard_condition.hpp
│       │
│       ├── event.hpp
│       ├── event_info.hpp
│       │
│       ├── error.hpp
│       ├── result.hpp
│       ├── visibility_control.hpp
│       │
│       └── fastdds/
│           └── message_type.hpp
│
└── src/
    ├── context.cpp
    ├── node.cpp
    ├── publisher.cpp
    ├── subscriber.cpp
    ├── client.cpp
    ├── server.cpp
    ├── event.cpp
    ├── guard_condition.cpp
    ├── wait_set.cpp
    ├── fastdds/
    │   └── message_type.cpp
    └── impl/
        ├── context_impl.hpp
        ├── context_impl.cpp
        ├── node_impl.hpp
        ├── node_impl.cpp
        ├── publisher_impl.hpp
        ├── publisher_impl.cpp
        ├── subscriber_impl.hpp
        ├── subscriber_impl.cpp
        ├── client_impl.hpp
        ├── client_impl.cpp
        ├── server_impl.hpp
        ├── server_impl.cpp
        ├── event_impl.hpp
        ├── event_impl.cpp
        ├── guard_condition_impl.hpp
        ├── guard_condition_impl.cpp
        ├── wait_set_impl.hpp
        ├── wait_set_impl.cpp
        ├── message_type_impl.hpp
        ├── event_parent_state.hpp
        ├── event_parent_state.cpp
        ├── reader_wait_state.hpp
        ├── request.hpp
        ├── response.hpp
        ├── discovery_graph.hpp
        ├── temporary_sample.hpp
        ├── lock_rank.hpp
        ├── name.hpp
        └── fastdds/
            ├── context.hpp
            ├── identity.hpp
            ├── message_type.cpp
            ├── process_runtime.hpp
            ├── qos.hpp
            └── return_code.hpp
```

根 `src/*.cpp` 与公开头文件一一对应，只保留 public-object 生命周期包装、只读 getter 和向对应 `Impl` 的转发；它们不得直接包含 Fast DDS 头。`src/impl/*.cpp` 同时承担对象业务逻辑和 Fast DDS runtime/helper 逻辑。DMW 当前是 Fast DDS 专用实现，不引入多 DDS runtime dispatch。`WaitSetState`、`ReaderWaitState`、`EventParentState` 与 service state 等 runtime/concurrency authority 均位于 `dmw::impl`。

### 14.2 Fast DDS Boundary

普通 public headers 不出现：

```text
eprosima::fastdds::*
eprosima::fastcdr::*
```

。

唯一 integration boundary：

`dmw/fastdds/message_type.hpp`

。

### 14.3 PImpl Naming

采用：

```cpp
Context::Impl
Publisher::Impl
Subscriber::Impl
```

不使用：

```text
ContextImpl
PublisherImpl
```

。

共享 state 使用：

```text
Context
NodeState
WaitableState
RegistrationState
```

。

### 14.4 Targets

Runtime：

`dmw::dmw`

。

Type binding：

`dmw::fastdds_binding`

。

关系：

```text
dmw::fastdds_binding
    ├── dmw::dmw
    └── Fast DDS compile interface
```

普通 consumer：

```cmake
target_link_libraries(app
    PRIVATE
        dmw::dmw
)
```

dclcpp：

```cmake
target_link_libraries(dclcpp
    PRIVATE
        dmw::dmw
        dmw::fastdds_binding
)
```

。

### 14.5 测试与验收

#### 14.5.1 Result Tests

必须覆盖：

```text
Result<int> success

Result<int> error

operator bool

has_value

value() &

value() const &

value() &&

error() &

error() const &

error() &&

Result<unique_ptr<T>>

Result<void>

wrong value access -> death

wrong error access -> death

representative std::bad_alloc -> propagates as C++ exception

no ErrorCode::BadAllocation translation
```

。

#### 14.5.2 Context Tests

```text
create success

create rollback

shutdown success

shutdown twice

concurrent shutdown

is_shutdown during shutdown

Context destruction without explicit shutdown

Context destroyed before children

children operations -> ContextShutdown

children destruction safe

ContextOptions RuntimeMode is immutable after create

all child endpoint options inherit Context RuntimeMode
```

。

模拟 shutdown wake failure 时：

```text
runtime remains non-Active
terminal Result follows the declared Fast DDS shutdown subset
no blocked infinite WaitSet remains
```

。

#### 14.5.3 Type-erased API Tests

覆盖：

```text
publish(nullptr)
take(nullptr)
write_request(nullptr)
write_response(nullptr)
```

：

`InvalidArgument`

。

覆盖：

`NoData`

时 output unchanged。

#### 14.5.4 QoS Tests

覆盖：

```text
finite(-1ns) -> InvalidArgument

finite(0ns) -> valid QosDuration

finite value() -> stored duration

SystemDefault/Infinite value() -> death

Qos() == Qos::system_default()

keep_last(0) -> InvalidArgument

keep_last(1) -> success

keep_all -> depth canonical 0

duration overflow

DDS SystemDefault

ROS2 SystemDefault

ros2_default

ros2_services_default

failing setter -> entire Qos unchanged
```

。

#### 14.5.5 Topic Tests

```text
basic pub/sub

multiple publishers

multiple subscribers

reliable

best effort

volatile

transient local
```

。

#### 14.5.6 Matched Count Tests

Publisher：

```text
0
-> 1
-> 2
-> 1
-> 0
```

。

Subscriber 对称。

QoS incompatible endpoint：

`must not count as matched`

。

#### 14.5.7 Service Tests

覆盖：

```text
Client/Server create

request

response

RequestId

multiple clients

response routing

service availability
```

。

#### 14.5.8 Server Request Tracking

覆盖：

```text
take request
    -> pending

response
    -> success
    -> remove pending

same id again
    -> NotFound

foreign id
    -> NotFound

two concurrent responses same id
    -> one proceeds
    -> one Busy

DDS write failure
    -> request remains Pending

max_pending_requests == 0
    -> create_server InvalidArgument

pending count reaches limit
    -> read_request ResourceExhausted
    -> DDS sample remains in DDS history

duplicate DDS sample while Pending
    -> suppressed

duplicate after successful response
    -> no permanent at-most-once guarantee

Server destruction / Context shutdown
    -> pending state safely cleared or made unusable

NoData / pre-consumption error
    -> payload + RequestId unchanged

post-consumption Error / exception
    -> payload + RequestId valid/destructible

continuous filtered arrivals
    -> one take terminates at its call-start finite budget
```

。

Response-reader discovery workaround 必须覆盖：

```text
reader already matched -> immediate write
reader matches before 100 ms -> write
repeated discovery wake -> original deadline unchanged
deadline expires -> Timeout
Context shutdown -> immediate ContextShutdown
target reader confirmed gone -> success without write
```

#### 14.5.9 Service Availability Pairing

构造：

```text
Participant A:
    request Reader only

Participant B:
    response Writer only
```

必须：

`service_is_available() == false`

。

构造同一 Participant：

```text
request Reader
+
response Writer
```

：

`eventually true`

。

#### 14.5.10 Wait Timeout Tests

覆盖：

```text
poll

finite

infinite

negative finite

deadline

steady clock
```

。

关键测试：

```text
wait 100ms
topology wake at 20ms
topology wake at 40ms
topology wake at 60ms
```

总等待时间仍以原始：

`100ms deadline`

为基准，不得变成：

`160ms / 200ms / ...`

。

#### 14.5.11 WaitableRegistration Tests

覆盖：

```text
default token invalid

add -> valid

wrong WaitSet remove -> InvalidArgument

cross-Context add -> InvalidArgument with no token/topology mutation

removed token -> NotRegistered

auto-detached token -> NotRegistered

registration id never reused

WaitSet id never reused
```

。

#### 14.5.12 GuardCondition / Event Tests

覆盖：

```text
trigger -> next wait ready

trigger before registration
    -> next wait ready

trigger x3 before wait
    -> one readiness

readiness consumed
    -> next wait not ready

trigger concurrent with consume
    -> trigger not lost

remove then re-add with pending trigger
    -> ready

Context shutdown
    -> trigger returns ContextShutdown

Fast DDS wake failure after logical trigger commit
    -> trigger success and readiness remains logically pending

Publisher + each publisher EventType -> success

Subscriber + each subscriber EventType -> success

Publisher + subscriber-only EventType -> InvalidArgument

Subscriber + publisher-only EventType -> InvalidArgument

Event create rollback

Event parent destroyed -> ParentDestroyed

event history before Factory commit -> not replayed

wait reports Event repeatedly until successful Event::take

two Events of same parent/type -> independent cursors

Event source exhaustion -> remains ready; take ResourceExhausted; cursor unchanged
```

。

#### 14.5.13 WaitSet Teardown Tests

```text
wait + Subscriber destruction

wait + Client destruction

wait + Server destruction

wait + Event destruction

wait + GuardCondition destruction

WaitSet destruction after active wait exits

control GuardCondition -> wait exits -> synchronize -> WaitSet destruction

waitable first

add/remove during wait

second wait -> Busy
```

。

全部要求：

```text
no UAF
no deadlock
no stale Fast DDS condition access
```

。

不测试也不支持同一 WaitSet facade 的 `wait()` 与析构无同步并发；该场景违反
[WaitSet 与对象销毁契约](#dmw-waitset)定义的调用者前置条件。

#### 14.5.14 Registry Tests

TypeRegistry：

```text
same wire name
same binding type
    -> reuse

same wire name
different binding
    -> TypeMismatch
```

。

TopicRegistry：

```text
same topic
same type
    -> reuse

same topic
different type
    -> TypeMismatch
```

。

#### 14.5.15 ROS Topic Interoperability

使用 [ROS 2 interoperability contract](#dmw-ros2-interop) 固定的 baseline。

验证：

```text
DMW Publisher
    ->
ROS Subscriber

ROS Publisher
    ->
DMW Subscriber
```

。

至少检查：

```text
FQN normalization

rt/ naming

DDS wire type

CDR

QoS

matched count
```

。

#### 14.5.16 ROS Service Interoperability

验证：

```text
DMW Client
    ->
ROS Server

ROS Client
    ->
DMW Server
```

。

必须验证：

```text
rq/<service>Request

rr/<service>Reply

request DDS type

response DDS type

request related reader GUID

request sample sequence

Server RequestId

response related_sample_identity

Client filtering

multi-client

service availability
```

。

#### 14.5.17 Version Regression Test

升级：

```text
rmw
rmw_fastrtps
rosidl_typesupport_fastrtps
Fast DDS
Fast CDR
```

任一基线版本后：

必须重新执行：

```text
17.15
17.16
```

。

未完成 interoperability regression，不得修改：

`ROS2 validated baseline`

。

每次运行必须归档完整 baseline manifest，并验证 release、Git ref、resolved
commit、Debian package revision、OS image digest、architecture 和 compiler version
均与测试报告一致。

<a id="dmw-frozen-invariants"></a>

## 15. Frozen Invariant 索引与附录

本章是正文契约的审查索引，不是第二份 normative specification。每一项都必须能够回溯到前述正文；若简述与正文冲突，以正文为准，并必须修复本索引。编号用于评审和测试追踪，不得被实现当作独立语义来源。

### 15.1 架构

1. C++17。
2. Fast DDS-only。
3. runtime API non-template。
4. 普通 public API 隐藏 Fast DDS 类型。
5. 不提供 C API。
6. 不提供 middleware plugin abstraction。
7. 不提供 Action primitive。
8. Context 是 runtime root。
9. 一个 Context 固定一个 Domain。
10. 一个 Context 创建一个 DomainParticipant。
11. 多 Domain 通过多个 Context。
12. Node 是 logical entity。

### 15.2 Factory 与 Ownership

13. Resource/Entity Factory 返回 `Result<std::unique_ptr<T>>`。
14. Resource/Entity non-copyable、non-movable。
15. public entity stable identity。
16. public entity stable address。
17. ownership transfer 移动 `unique_ptr`。
18. 只有 `Context::create()` 是 static root Factory。
19. 其他 entity 由 parent Factory 创建。
20. Factory 名与返回实体完全一致；WaitSetOptions/GuardConditionOptions 是空的 V1 扩展点。
21. 使用 `Subscriber`，不使用 public `Subscription`。
22. 使用 `Server`，不使用 public `Service` entity。
23. 不使用两阶段初始化。
24. 不提供 public destroy。

### 15.3 Resource 生命周期

25. `Impl::create()` 事务式创建。
26. local RAII rollback。
27. 不返回 half-valid object。
28. `Impl::destroy()` private。
29. `Impl::destroy()` noexcept。
30. `Impl::destroy()` idempotent。
31. cleanup failure 不阻断其余 cleanup。
32. Context 可晚于 facade 销毁。
33. Node facade 可以早于 endpoint。
34. Context facade 可以早于 children。
35. Context shutdown 后 child 仍可安全析构。

### 15.4 Result 与 Error

36. `Result<T>` public contract 已冻结。
37. 支持 move-only T。
38. 提供 `value() & / const& / &&`。
39. 提供 `error() & / const& / &&`。
40. 提供 `operator bool()` 和 `has_value()`。
41. 提供 `Result<void>`。
42. ErrorCode 不包含 Ok。
43. wrong alternative access 调用 terminate。
44. runtime expected error 不使用 exception；`std::bad_alloc` 允许传播，V1 不定义 `BadAllocation` ErrorCode。

### 15.5 Type Erasure

45. nullptr 返回 InvalidArgument。
46. concrete type 必须与 MessageType 匹配。
47. wrong concrete type 属于调用者 contract violation。
48. TypeMismatch 不用于运行时检查任意 `void*`。
49. 所有 take 的 NoData / pre-consumption failure 保持全部 output unchanged。
50. Fast DDS consume 后的 Error/exception 保证 output valid/destructible but unspecified；过滤使用 call-start finite budget。

### 15.6 Context Shutdown

51. 内部状态为 Active / ShuttingDown / Shutdown。
52. Active→ShuttingDown 是 shutdown linearization point。
53. shutdown 不可恢复。
54. `is_shutdown()` 对 ShuttingDown 返回 true。
55. concurrent shutdown 等待同一 terminal result。
56. repeated shutdown 返回记录的 terminal result。
57. shutdown error 不恢复 Active。
58. shutdown 必须唤醒或安全 detach 所有 active WaitSet。
59. ContextShutdown 是统一 lifecycle error。

### 15.7 Type / Registry

60. MessageType 是可重新绑定的 cheap-copy handle，其指向的 binding descriptor 不可变。
61. MessageType 不存在 invalid default state。
62. `type_name()` 是 DDS wire type name。
63. TypeRegistry mandatory。
64. TopicRegistry mandatory。
65. Type identity 不使用 object pointer address。
66. 相同 wire name + same binding identity 可复用。
67. 相同 wire name + different binding identity 返回 TypeMismatch。
68. V1 不做结构化 wire-layout equivalence。

### 15.8 QoS

69. negative QosDuration 非法；只有 Finite duration 可调用 `value()`。
70. duration conversion 必须 checked；`Qos()` 等价于 `system_default()`，失败 setter 保持整个 Qos 不变，并提供全部基础 policy getter。
71. keep_last depth 必须 > 0。
72. KeepAll canonical depth 为 0。
73. SystemDefault 由 Context 的 immutable RuntimeMode + entity kind + frozen Fast DDS baseline 解析，不读取 runtime/XML mutable default。
74. ROS2 不读取 ROS XML overrides。
75. 提供 `ros2_default()`。
76. 提供 `ros2_services_default()`。
77. incompatible remote QoS 不导致本地 create failure。

### 15.9 Topic

78. Publisher 支持 matched Subscriber count。
79. Subscriber 支持 matched Publisher count。
80. matched count 是 DDS current snapshot。
81. matched count 不依赖 ROS Graph。
82. incompatible endpoint 不计入 matched count。

### 15.10 Service

83. Client = request Writer + response Reader。
84. Server = request Reader + response Writer。
85. Client/Server 整体创建。
86. Client/Server 整体 rollback。
87. Client/Server 整体销毁。
88. RequestId = client_gid + sequence。
89. Server 只允许响应自己 pending 的 RequestId；pending 有正数上限，满时在 Fast DDS take 前返回 ResourceExhausted。
90. unknown / already responded ID 返回 NotFound。
91. concurrent response 同 ID 返回 Busy。
92. response write failure后 request 恢复 Pending；V1 只在 Pending/Responding 生命周期内去重。
93. Client 过滤其他 Client 的 response。
94. DMW 不维护 dclcpp Future outstanding table。
95. service availability 返回 Result<bool>。
96. availability 必须按 remote Participant 配对 request/response endpoint。
97. 不允许跨 Participant 拼接产生 availability true。

### 15.11 WaitSet

98. WaitTimeout 使用唯一 value type。
99. Poll / Finite / Infinite 明确区分；只有 Finite 可调用 `duration()`。
100. negative finite timeout 非法。
101. finite wait 使用 steady_clock。
102. topology wake 不重置原始 timeout。
103. WaitableRegistration 绑定创建它的 WaitSet。
104. registration 0 值 invalid。
105. registration 在进程生命周期不复用。
106. stale registration 返回 NotRegistered。
107. wrong WaitSet registration 返回 InvalidArgument。
108. WaitResult 是不可非法构造的 readiness snapshot：Ready 非空、Timeout 为空；remove 不撤回已形成的 stale value snapshot。
109. 一个 waitable 同时只能加入一个 WaitSet。
110. 同一 WaitSet 同时一个 active wait。
111. add/remove 可以与 wait 并发。
112. WaitSet 非 owning。
113. waitable 析构自动 detach。
114. WaitSet 析构自动 detach，但前置条件是自身没有 active wait；不支持自身 `wait()` 与析构并发；跨 Context add 返回 InvalidArgument且无状态变化。

### 15.12 GuardCondition / Event

115. GuardCondition 只有 trigger，无 public reset。
116. GuardCondition 使用 pending-trigger semantics。
117. 多 trigger 在消费前合并。
118. trigger before registration 必须可观察。
119. WaitResult 返回前自动消费本次 trigger；logical trigger commit 后 Fast DDS wake failure不改变 public success。
120. concurrent trigger/consume 不得丢 trigger。
121. Event 是 persistent endpoint-bound entity。
122. EventInfo 使用 variant。
123. Event 具有完整 PImpl 生命周期和固定 parent/EventType 合法组合；Parent endpoint 销毁后返回 ParentDestroyed；Event 不 replay pre-creation history、保持 level-triggered，只有 successful take 消费独立 cursor，source exhaustion保持 ready并返回 ResourceExhausted。

### 15.13 ROS 2 Compatibility

124. RuntimeMode 名称为 `ROS2`；RuntimeMode 由 ContextOptions 唯一持有且创建后 immutable，endpoint options 不提供 override。
125. compatibility 由规范、manifest 固定的 reference implementation 和 interoperability tests 共同定义。
126. ROS relative name 先解析成 FQN。
127. Topic DDS name 使用 `rt`。
128. request DDS topic 使用 `rq...Request`。
129. response DDS topic 使用 `rr...Reply`。
130. DDS wire type name 由 MessageType binding 明确提供。
131. Service payload 不增加自定义 request wrapper。
132. correlation 使用 SampleIdentity / related_sample_identity。
133. Client request related GUID 使用 response reader GUID。
134. Server 规范化 RequestId。
135. response related identity 使用 RequestId。
136. Client 根据自己的 endpoint GUID 过滤 response。
137. ROS Service default QoS 固定；response-reader discovery timeout 使用 steady-clock 100 ms 原始 deadline。
138. V1 保证 wire compatibility，不保证完整 Graph compatibility。
139. dependency baseline 更新必须重新做 interoperability regression 并归档完整可复现 manifest。

### 15.14 C++17

140. 全部 public 示例必须符合标准 C++17。
141. 文档禁止使用 C++20 designated initializer。

### 15.15 最终架构

```text
                     Context
                        │
          ┌─────────────┼──────────────┐
          │             │              │
          ▼             ▼              ▼
        Node         WaitSet      GuardCondition
          │
 ┌────────┼──────────┬─────────┐
 ▼        ▼          ▼         ▼
Publisher Subscriber Client   Server
    │       │
    └── Event
```

底层：

```text
Context
   ↓
DomainParticipant

Publisher
   ↓
DataWriter

Subscriber
   ↓
DataReader

Client
   ├── Request DataWriter
   └── Response DataReader

Server
   ├── Request DataReader
   └── Response DataWriter
```

DMW V1 最终能力边界：

```text
Context / Node
      +
Topic Pub/Sub
      +
matched count
      +
Service Client/Server
      +
WaitSet
      +
GuardCondition
      +
Event
      +
Basic QoS
      +
ROS 2 Humble Fast DDS wire compatibility
```

而：

```text
Graph
Serialized Message
Loaned Message
Zero-copy
Advanced Introspection
Action
```

明确留给后续版本。

## 结论

DMW V1 的核心目标不是复制完整 ROS 2 RMW API，而是：

> **提供构建 DCL Client Library 所需的 RMW 基础 middleware primitive，同时把生命周期、并发、错误模型和 ROS 2 Fast DDS wire behavior 定义到足以直接实现的程度。**

Factory 保证：

> 创建成功即完整有效。

RAII 保证：

> public ownership 结束时自动、安全释放。

Shared internal state 保证：

> Node / Context / WaitSet 非正常析构顺序仍然 memory-safe。

WaitSet contract 保证：

> endpoint 与 WaitSet 任一侧先销毁都不会产生悬空 Fast DDS condition。

`Result<T>` contract 保证：

> Factory 和 runtime error 具有统一、明确且支持 move-only object 的 C++17 使用模型。

ROS compatibility contract 保证：

> `ROS2` 不再只是“兼容目标”，而是具有固定版本、确定 naming、type、QoS 和 request/reply identity mapping 的可测试 wire contract。
