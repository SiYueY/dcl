# DCL 系统架构设计

> 文档状态：Draft V0.1  
> 项目名称：DCL — DDS Client Libraries  
> 适用范围：`dmw`、`dclcpp`、`dclpy` 统一架构  
> 主要目标平台：Linux / Ubuntu 22.04、Ubuntu 24.04  
> C++ 标准：C++17  
> DDS 实现：Fast DDS  
> ROS 2 互操作基线：ROS 2 Humble / Jazzy + `rmw_fastrtps_cpp`

---

## 1. 文档目的

本文档定义 DCL 项目的整体系统架构、模块边界、依赖关系、运行时模型、类型系统、通信原语、ROS 2 互操作策略、构建安装方式以及后续演进原则。

DCL 的目标不是重新实现 ROS 2，也不是构建一个新的 DDS 实现，而是在 Fast DDS 之上提供更轻量、直接、现代的 Client Library：

- `dmw`：面向上层 Client Library 的非模板 C++ middleware/runtime 核心；
- `dclcpp`：面向 C++ 用户的强类型 Client Library；
- `dclpy`：面向 Python 用户的 Client Library；
- Fast DDS：唯一底层 DDS 实现；
- ROS 2 Compatibility：在不依赖 ROS 2 Runtime 的情况下实现 DDS/wire-level 互操作。

本文档负责定义“整个系统如何分层和协作”。`dmw`、`dclcpp`、`dclpy` 的具体设计分别由独立设计文档描述。

---

## 2. 项目定位

### 2.1 DCL 是什么

DCL 是一组基于 DDS 的 Client Libraries：

```text
DCL
├── dmw       DDS Middleware / Common Runtime Layer
├── dclcpp    DDS Client Library for C++
└── dclpy     DDS Client Library for Python
```

其总体调用关系为：

```text
                       Applications
                   /                  \
                  ▼                    ▼
           C++ Application       Python Application
                  │                    │
                  ▼                    ▼
               dclcpp                dclpy
                  │                    │
                  │              _dclpy native binding
                  │                    │
                  └──────────┬─────────┘
                             ▼
                            dmw
                             │
                             ▼
                         Fast DDS
                             │
                             ▼
                         DDSI-RTPS
```

### 2.2 DCL 不是什么

DCL V1 明确不承担以下职责：

1. 不实现 DDS/RTPS 协议栈；
2. 不替代 Fast DDS；
3. 不复制 ROS 2 的 `rcl` 包层级或新增独立的 `rcl` 等价层，但可以把适合跨语言共享的 `rcl` / `rcl_action` 类 runtime 语义直接收敛到 `dmw`；
4. 不提供类似 ROS 2 RMW 的多 middleware plugin 机制；
5. 不以 C ABI 作为核心接口；
6. 不重新实现完整 IDL 编译器和 CDR 代码生成器；
7. 不要求系统安装 ROS 2 才能运行；
8. 不以 ROS Node/Graph 为 DCL Core 的基础语义；
9. 不把用户 callback、Future、Executor 调度策略、Python asyncio/GIL 等语言运行时职责下沉到 `dmw`；
10. V1 不承诺完整 ROS Graph 工具兼容。

---

## 3. 核心架构原则

### 3.1 只保留两层 Client/Middleware 结构

DCL 借鉴 ROS 2 `rmw_fastrtps → rcl/rcl_action → rclcpp/rclpy` 中经过实践验证的职责划分，但不复制 `rclcpp → rcl → rmw` 三层包结构。

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

在该两层结构中，`dmw` 同时承担 Fast DDS binding 与语言无关 common runtime 的职责；这不是增加新的中间层，而是避免 `dclcpp` 和 `dclpy` 分别重复实现同一协议、状态机和等待语义。

### 3.2 `dclcpp` 与 `dclpy` 平级

禁止形成：

```text
dclpy → dclcpp → dmw
```

正确关系：

```text
             dmw
           ▲     ▲
          /       \
     dclcpp       _dclpy
                    ▲
                    │
                  dclpy
```

两种语言 Client Library 共用相同 `dmw` runtime 和 middleware semantics，但可以拥有不同的 callback、future、executor 和语言友好 API。

### 3.3 `dmw` 使用非模板 C++17 API

`dmw` 是 C++ 库，但其核心公共接口保持：

- non-template；
- type-erased；
- RAII；
- 不暴露 Fast DDS public types；
- 不依赖 `dclcpp`；
- 不依赖 `dclpy`；
- 可以提供 Topic、Service、Timer 和 Action common runtime primitive；
- 不提供用户 callback、Future 或高层 Executor。

### 3.4 Fast DDS 是唯一 backend

V1 不设计：

```text
dmw_fastrtps
dmw_cyclonedds
dmw_connext
```

也不设计 backend factory、plugin ABI 或虚拟 middleware interface。

这意味着 `dmw` 内部实现可以直接使用 Fast DDS，只要不把 Fast DDS 类型泄漏到 `dmw` 的稳定公共接口。

### 3.5 类型安全只存在于 Client Library 层

```text
dclcpp::Publisher<Msg>
        │
        ▼
dmw::Publisher
        │
        ▼
Fast DDS DataWriter
```

`dclcpp` 负责 `Msg`、`ServiceT`、`ActionT` 强类型；`dmw` 负责 type-erased runtime primitive。

Action 也遵循相同原则：`dclcpp::ActionType<ActionT>` / Python runtime type 保留语言类型信息，而 DMW 使用 type-erased `ActionType` 和 runtime state。

### 3.6 用户 callback 不运行在 Fast DDS 内部线程

DCL 默认采用：

```text
Fast DDS
   │
   ▼
WaitSet / Conditions
   │
   ▼
dclcpp Executor / dclpy Executor
   │
   ▼
User callback
```

Fast DDS Listener 可用于内部 wakeup/discovery 等轻量通知，但不得把任意用户业务 callback 直接放入 DDS 内部线程执行。

### 3.7 Language-neutral Runtime 下沉原则

跨 `dclcpp` / `dclpy` 必须保持一致，并且不依赖具体语言运行时的功能，应优先由 `dmw` 实现一次：

```text
protocol mapping
runtime state machine
identity / correlation
readiness / wait semantics
Timer scheduling state
Action Goal state / protocol state
ROS 2 wire compatibility behavior
```

与语言本身直接耦合的功能保留在 Client Library：

```text
C++ templates / Python runtime type
std::function / Python callable
std::future / Python Future / asyncio Future
Executor callback scheduling policy
C++ exception presentation
Python exception / GIL / asyncio integration
```

该原则的目标不是消除所有重复代码，而是避免同一个通信协议或运行时状态机在 C++ 与 Python 中出现两个 authority。

---

## 4. 模块职责

## 4.1 `dmw`

负责：

- Context / Node runtime；
- Participant 生命周期；
- Publisher / Subscription；
- Client / Service；
- Timer runtime；
- Action common runtime；
- MessageType / ServiceType / ActionType；
- Goal identity / GoalState / Goal FSM；
- Action endpoint composition；
- goal/result/cancel correlation；
- Action availability、status bookkeeping 与 result cache protocol state；
- QoS；
- WaitSet / GuardCondition / Event；
- MessageInfo / RequestId；
- Topic/Service/Action DDS naming；
- request/response identity correlation；
- discovery；
- Fast DDS 资源管理；
- ROS 2 DDS wire compatibility；
- type registration 与 endpoint 创建。

不负责：

- C++ 模板 API；
- 用户 callback；
- C++ future；
- Python Future / asyncio；
- Python GIL；
- 高层 Executor 策略。

## 4.2 `dclcpp`

负责：

- C++17 typed API；
- `Context` / `Node`；
- `MsgType<MsgT>`；
- `Publisher<MsgT>` / `Subscription<MsgT>`；
- `Client<ServiceT>` / `Service<ServiceT>`；
- `Timer` 的 C++ callback wrapper；
- `ActionType<ActionT>` / `ActionClient<ActionT>` / `ActionServer<ActionT>`；
- typed GoalHandle；
- typed QoS；
- WaitSet wrapper；
- Executor；
- callback / future；
- Fast DDS-Gen `Msg + MsgPubSubType` 的一次性类型绑定。

不在 DCLCPP 重复实现 DMW 已经维护的 Goal FSM、Action endpoint composition、result cache protocol state 或 transport correlation。

## 4.3 `dclpy`

V1 仅做整体架构设计，后续实现。

负责：

- Pythonic Node/Publisher/Subscription/Client/Service/Timer/Action API；
- Python typed/runtime wrappers；
- Python GoalHandle wrapper；
- Python callback/future；
- Python Executor 与 asyncio 适配；
- 通过 `_dclpy` native extension 直接调用 `dmw`；
- Python message wrapper 与 native C++ message 的绑定；
- GIL boundary 与 Python exception mapping。

不在 DCLPY 重复实现 DMW 已经提供的 Timer 状态、Goal FSM、Action endpoint composition 和 transport correlation。

---

## 5. 运行时对象模型

推荐对象关系：

```text
DCL Runtime
│
├── dmw::Context
│    └── Fast DDS DomainParticipant
│
├── dmw::Node A
│    ├── Publisher
│    ├── Subscription
│    ├── Client
│    ├── Service
│    ├── ActionClient / ActionServer
│    └── ...
│
├── dmw::Timer
│
└── dmw::Node B
     └── ...
```

### 5.1 Context

`Context` 是 process 级或 domain 级 runtime owner，负责：

- Domain ID；
- Fast DDS `DomainParticipant`；
- shutdown；
- type registry；
- topic registry；
- shared discovery resources；
- wait/wakeup infrastructure；
- language-neutral Timer/Action runtime 所需的 shared state。

V1 默认推荐：同一 `Context` 共享一个 `DomainParticipant`。

### 5.2 Node

DCL 的 `Node` 是高层逻辑通信实体，不要求与 `DomainParticipant` 一一对应。

```text
Context / DomainParticipant
        │
        ├── Node A
        ├── Node B
        └── Node C
```

这有利于：

- 降低 Participant 数量；
- 统一发现资源；
- 后续实现 ROS 2 Graph logical node metadata；
- 允许多个 Node 共享底层 participant。

---

## 6. 类型系统设计

## 6.1 不实现完整 `dclcpp_codegen`

DCL V1 使用 Fast DDS-Gen 负责：

```text
IDL
 ↓
fastddsgen
 ↓
Msg
MsgPubSubType
CDR Aux
TypeObject Support
```

DCL 不重复实现 IDL parser、CDR serializer generator、TypeObject generator。

## 6.2 C++ 类型绑定

C++ 侧采用“一次绑定、后续复用”：

```cpp
auto msg_type = dclcpp::create_msg_type<Msg, MsgPubSubType>();
```

返回：

```cpp
MsgType<Msg>
```

后续：

```cpp
auto pub = node->create_publisher(msg_type, "/topic", qos);
auto sub = node->create_subscription(msg_type, "/topic", qos, callback);
```

`MsgPubSubType` 不再出现在后续业务 API 中。

## 6.3 Runtime 类型描述

核心 runtime descriptor：

```text
dclcpp::MsgType<MsgT>
        │
        ▼
dmw::MessageType
        │
        ▼
Fast DDS TypeSupport
        │
        ▼
MsgPubSubType
```

其中：

- `MsgT` 在 `dclcpp` 中保持强类型；
- `MsgPubSubType` 在绑定阶段被 type erase；
- `dmw` 只认识 `MessageType`；
- Python 后续也复用同一个 `dmw::MessageType` 概念。

## 6.4 Service 类型

```text
ServiceType
├── Request MessageType
└── Response MessageType
```

C++ 高层：

```text
dclcpp::ServiceType<ServiceT>
        │
        ▼
dmw::ServiceType
```

## 6.5 Action 类型

Action 的语言强类型和运行时描述分离：

```text
ActionT
├── SendGoal ServiceType
├── CancelGoal ServiceType
├── GetResult ServiceType
├── Feedback MsgType
└── Status MsgType
```

C++ 高层：

```text
dclcpp::ActionType<ActionT>
        │
        ▼
dmw::ActionType
        │
        ├── SendGoal ServiceType
        ├── CancelGoal ServiceType
        ├── GetResult ServiceType
        ├── Feedback MessageType
        └── Status MessageType
```

`dmw::ActionType` 是 type-erased runtime descriptor；`dclcpp` / `dclpy` 各自保留语言层的强类型或 runtime type 表达。

---

## 7. Topic 架构

```text
dclcpp::Publisher<Msg>
        │
        ▼
dmw::Publisher
        │
        ▼
Fast DDS DataWriter
```

接收：

```text
Fast DDS DataReader
        │
        ▼
dmw::Subscriber
        │
        ▼
dclcpp Executor
        │
        ▼
Subscription<Msg> callback
```

Topic primitive 在 `dmw` 中为一级能力。

---

## 8. Service 架构

Service 是 `dmw` 一级 primitive。

```text
Client
├── Request DataWriter
└── Response DataReader

Service
├── Request DataReader
└── Response DataWriter
```

`dmw` 负责：

- request identity；
- sequence number；
- writer GUID；
- SampleIdentity；
- related_sample_identity；
- multiple-client response correlation；
- pending request metadata；
- service availability；
- blocking service availability wait；
- request/reply naming；
- service QoS。

`dclcpp` / `dclpy` 负责：

- typed request/response 或 Python request/response wrapper；
- language-specific Future / callback；
- 对 DMW wait API 的语言友好 wrapper。

---

## 9. Action 架构

Action 的 wire protocol 仍然建立在：

```text
Action
├── SendGoal Service
├── CancelGoal Service
├── GetResult Service
├── Feedback Topic
└── Status Topic
```

之上，但 3 Service + 2 Topic 的组合和公共协议状态不再由 `dclcpp` / `dclpy` 分别实现。

```text
             dclcpp Action                 dclpy Action
                    │                          │
                    └────────────┬─────────────┘
                                 ▼
                         dmw Action runtime
                                 │
                 ┌───────────────┼───────────────┐
                 ▼               ▼               ▼
             Goal FSM      3 Service + 2 Topic  correlation
```

`dmw` 负责：

- `ActionType` runtime descriptor；
- ActionClient / ActionServer common runtime；
- Goal identity；
- Goal state machine；
- goal/result/cancel correlation；
- Action availability；
- status bookkeeping；
- result cache protocol state；
- WaitSet readiness；
- ROS 2 Action endpoint naming 和 wire-level compatibility semantics。

Client Library 负责：

- typed/Python Action API；
- GoalHandle 的语言层 wrapper；
- Future；
- callback；
- Executor dispatch；
- Python asyncio/GIL integration。

因此 Action protocol bug、Goal FSM bug 或 cancel/result correlation bug 只需要在 DMW 修复一次，而不是在 C++ 与 Python 各维护一套 authority。

---

## 10. Executor / WaitSet 架构

`dmw` 只提供 language-neutral waiting/runtime primitives，不提供用户 callback Executor：

```text
dmw::WaitSet
├── Subscription readiness
├── Client response readiness
├── Service request readiness
├── Timer readiness
├── Action runtime readiness
├── Events
└── GuardCondition
```

`dclcpp` 决定 C++ 执行策略：

```text
dclcpp::SingleThreadedExecutor
future: MultiThreadedExecutor
```

`dclpy` 后续可独立实现：

```text
dclpy.Executor
asyncio integration
```

二者共享 `dmw::WaitSet` 和底层 readiness/protocol state，不共享 executor implementation。

### 10.1 Timer

Timer 的 period、deadline/readiness、cancel/reset 和 WaitSet integration 归 `dmw::Timer`：

```text
dmw::Timer
    │
    ├── dclcpp::Timer + std::function
    └── dclpy.Timer + Python callable
```

DMW Timer 不执行 user callback。用户 callback 仍由相应 Client Library Executor 调度。

---

## 11. QoS 架构

分为两层：

```text
dclcpp::QoS / dclpy.QoS
          │
          ▼
       dmw::Qos
          │
          ▼
Fast DDS QoS
```

`dmw::Qos` 提供 DCL 需要的公共 DDS QoS 表达，不直接暴露 Fast DDS `DataWriterQos` / `DataReaderQos`。

对于 Fast DDS 特定高级能力，可后续提供隔离的 extension options，但不得污染标准 API。

---

## 12. ROS 2 互操作架构

### 12.1 基线

DCL 当前保持同一份 DMW 源码支持两个主要验证环境：

```text
ROS 2 Humble
+
Fast DDS 2.6.x
+
rmw_fastrtps_cpp Humble
```

以及：

```text
ROS 2 Jazzy
+
Fast DDS 2.14.x
+
rmw_fastrtps_cpp Jazzy
```

当前日常开发环境可以继续以 Humble 为主，但后续 DMW 设计与实现以 Fast DDS 2.14.x 的现代能力为主要实现基础，并以 `rmw_fastrtps` Jazzy 作为 ROS 2 Fast DDS 工程行为参考，同时持续验证 Humble/Fast DDS 2.6.x 兼容性。

对于 language-neutral runtime 的职责划分，可以参考 ROS 2 Jazzy 的 `rcl` / `rcl_action`；对于 C++ 与 Python 客户端职责边界，可以参考 `rclcpp` / `rclpy`。这些参考不意味着 DCL 复制对应包层级。

### 12.2 Wire compatibility 与 Graph compatibility 分离

第一阶段：

```text
Topic wire interoperability
Service wire interoperability
Action interoperability
```

后续：

```text
ros2 node list
ros2 node info
rqt_graph
full graph metadata
```

### 12.3 `dmw` 负责的 ROS 2 compatibility

- DDS topic naming；
- DDS service request/reply naming；
- DDS type naming；
- QoS mapping；
- request/response correlation；
- Action endpoint naming；
- Action Goal/result/cancel wire semantics；
- Action status/feedback wire semantics；
- discovery details；
- Graph metadata（后续）。

### 12.4 `dclcpp` / `dclpy` 负责的 ROS 2-facing Action API

- typed/Python ActionType；
- typed/Python GoalHandle；
- Future / callback；
- Executor dispatch；
- Python asyncio/GIL integration；
- 将 DMW Action runtime 的结果映射为对应语言 API。

Action endpoint composition、Goal FSM、cancel/result correlation 和协议级状态由 DMW 统一实现。

---

## 13. 错误模型

### 13.1 `dmw`

推荐采用明确的 C++ result/error 模型，不让异常穿越 runtime boundary 作为常规控制流。

例如：

```cpp
enum class ErrorCode;
class Error;
template<typename T> class Result;
```

或使用简单 `Status + output` 组合。

设计目标：

- Fast DDS ReturnCode 转换为 DCL error；
- `dmw` 错误不携带 Fast DDS public type；
- `dclcpp` 可选择抛异常或返回状态；
- `dclpy` 可映射为 Python exception。

具体形态由 `dmw` 设计文档冻结。

---

## 14. 生命周期与资源所有权

基本规则：

1. 所有资源 RAII；
2. `Context` 生命周期长于 Node；
3. Node 生命周期长于 endpoint；
4. MessageType/ServiceType/ActionType 可共享；
5. Fast DDS type registration 由 `Context` 管理；
6. Endpoint/Timer/Action runtime 析构必须先停止 wait/callback 可见性，再释放底层资源；
7. shutdown 必须可重复调用且有明确定义；
8. 禁止 dangling raw DDS pointer 逃逸到 public API。

推荐：

```text
Context
  owns Participant
  owns registries
  owns common runtime coordination

Node
  references Context

Publisher/Subscription/Client/Service/Timer/Action runtime
  reference Node/Context runtime state
```

---

## 15. 线程安全原则

V1 要求明确区分：

- construction/destruction thread safety；
- publish/take thread safety；
- wait set mutation thread safety；
- timer/action runtime state thread safety；
- executor callback serialization；
- shutdown 与 active wait 的并发行为。

默认原则：

- `Publisher::publish()` 可并发调用，前提为 Fast DDS/内部实现允许；
- endpoint create/destroy 与 executor mutation 需要受控；
- `WaitSet` 不允许无限制并发修改；
- Timer/Action 的公共状态转换必须由 DMW 定义清晰的并发语义；
- shutdown 必须唤醒所有等待线程；
- callback 生命周期由 Client Library 层控制。

---

## 16. 单仓库目录结构

```text
dcl/
├── CMakeLists.txt
├── cmake/
│
├── dmw/
│   ├── CMakeLists.txt
│   ├── include/dmw/
│   └── src/
│
├── dclcpp/
│   ├── CMakeLists.txt
│   ├── include/dclcpp/
│   └── src/
│
├── dclpy/
│   ├── CMakeLists.txt
│   ├── pyproject.toml
│   ├── src/dclpy/
│   └── native/
│
├── test_interfaces/
├── examples/
│   ├── cpp/
│   └── python/
│
├── tests/
│   ├── dmw/
│   ├── dclcpp/
│   ├── dclpy/
│   └── interoperability/
│
├── tools/
└── docs/
```

---

## 17. 构建依赖关系

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
dmw     !→ dclcpp
dmw     !→ dclpy
dclcpp  → dmw
_dclpy  → dmw
dclpy   → _dclpy
dclcpp  !↔ dclpy
```

---

## 18. CMake target 规划

核心 target：

```text
dmw
dclcpp
_dclpy
```

辅助 target：

```text
dcl_test_interfaces
examples...
tests...
```

根目录：

```cmake
add_subdirectory(dmw)
add_subdirectory(dclcpp)
add_subdirectory(dclpy)
```

其中 `dclpy` build 可通过 option 关闭。

---

## 19. 安装布局

C++：

```text
<prefix>/include/dmw/
<prefix>/include/dclcpp/
<prefix>/lib/libdmw.so
<prefix>/lib/libdclcpp.so
<prefix>/lib/cmake/dmw/
<prefix>/lib/cmake/dclcpp/
```

Python：

```text
site-packages/dclpy/
├── __init__.py
├── ...
└── _dclpy.so
```

---

## 20. 测试架构

```text
tests/
├── dmw/
│   ├── unit/
│   └── integration/
├── dclcpp/
│   ├── unit/
│   └── integration/
├── dclpy/
│   ├── unit/
│   └── integration/
└── interoperability/
    ├── cpp_python/
    └── ros2/
        ├── topic/
        ├── service/
        └── action/
```

核心互操作矩阵：

| Publisher/Client | Subscriber/Server | 必测 |
|---|---|---:|
| dclcpp | dclcpp | 是 |
| dclcpp | dclpy | 后续 |
| dclpy | dclcpp | 后续 |
| dclpy | dclpy | 后续 |
| dclcpp | ROS 2 Humble | 是 |
| ROS 2 Humble | dclcpp | 是 |
| dclcpp | ROS 2 Jazzy | 是 |
| ROS 2 Jazzy | dclcpp | 是 |
| dclpy | ROS 2 Humble/Jazzy | 后续 |
| ROS 2 Humble/Jazzy | dclpy | 后续 |

DMW 还必须独立验证 Humble/Fast DDS 2.6.x 与 Jazzy/Fast DDS 2.14.x 的跨版本 Topic/Service/Action common runtime 行为，Client Library 测试不替代 DMW protocol/runtime 验证。

---

## 21. 开发阶段

### Phase 1 — `dmw` Foundation 稳定

- Context / Node；
- MessageType / ServiceType；
- Publisher / Subscription；
- Client / Service；
- QoS；
- WaitSet / GuardCondition / Event；
- discovery；
- type registration；
- Humble/Jazzy 双环境 build/test/interoperability；
- correctness、lifecycle 与 performance closure。

### Phase 2 — `dmw` Shared Runtime 补齐

- Timer runtime；
- ActionType；
- ActionClient / ActionServer common runtime；
- Goal identity / Goal FSM；
- Action availability；
- result/cancel/status common state；
- WaitSet integration；
- ROS 2 Action wire compatibility。

### Phase 3 — `dclcpp`

- MsgType / ServiceType / ActionType typed wrapper；
- Publisher / Subscription；
- Timer wrapper；
- Client / Service；
- ActionClient / ActionServer / GoalHandle；
- Future / callback；
- SingleThreadedExecutor；
- C++ interoperability tests。

### Phase 4 — `dclpy`

- `_dclpy`；
- Python API；
- Python message binding；
- Timer/Action native wrapper；
- Python Future / callback；
- Executor / asyncio；
- C++ ↔ Python interoperability。

### Phase 5 — Graph / advanced features

- ROS Graph；
- advanced events；
- multi-thread executor；
- loaned messages/zero-copy；
- performance tuning；
- additional ROS 2 compatibility expansion。

---

## 22. 架构冻结项

V1 建议冻结以下规则：

1. `dclcpp`、`dclpy` 平级；
2. 两者都直接依赖 `dmw`；
3. `dmw` 是 non-template C++17 API；
4. `dmw` 不提供 C API；
5. Fast DDS 是唯一 backend；
6. 不设计独立 `rcl` 等价包层；适合跨语言共享的 common runtime 直接进入 `dmw`；
7. 不实现完整 DCL codegen；
8. IDL/C++/PubSubType 使用 Fast DDS-Gen；
9. C++ 使用 `create_msg_type<Msg, MsgPubSubType>()` 做一次绑定；
10. `dmw::MessageType` 是统一 runtime type descriptor；
11. Service 是 `dmw` primitive；
12. Timer 是 `dmw` language-neutral runtime primitive；
13. Action common runtime 是 `dmw` primitive，底层仍由 3 Service + 2 Topic 组成；
14. Goal FSM、Action correlation、协议级 result/status state 由 DMW 统一维护；
15. callback / Future / Executor / asyncio 不下沉到 DMW；
16. callback 不直接运行在 Fast DDS 内部线程；
17. WaitSet 在 `dmw`，Executor 在 Client Library；
18. ROS 2 wire compatibility 与 Graph compatibility 分阶段实现；
19. DMW 同一份源码持续验证 Humble/Fast DDS 2.6.x 与 Jazzy/Fast DDS 2.14.x；
20. 后续 DMW 实现以 Fast DDS 2.14.x 为主要实现基础，以 `rmw_fastrtps` Jazzy 为 ROS 2 工程参考，同时保持 Humble 兼容验证。

---

## 23. 总结

DCL V1 的核心思想不是复制 ROS 2，而是保留最有价值的职责分层，同时避免 C++ 与 Python 对同一 runtime protocol 重复实现：

```text
Typed / Python Client Library
        ↓
Common Middleware / Runtime Core
        ↓
Fast DDS
```

最终形成：

```text
              dmw
            ▲     ▲
           /       \
      dclcpp       dclpy
```

`dmw` 负责稳定、非模板、type-erased 的 middleware/runtime core，包括 Topic、Service、Timer、Action common runtime 和等待语义；`dclcpp` 提供现代 C++ 强类型 API、callback/Future/Executor；`dclpy` 提供 Python API、Future/asyncio/GIL/Executor。所有 DDS-specific 细节以及跨语言必须一致的协议状态均收敛到 `dmw`，同时通过 Humble/Jazzy 双环境 ROS 2 compatibility 验证实现直接通信。