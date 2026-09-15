# DCLPY 整体架构设计

> 文档状态：Architecture Draft V0.1  
> 模块名称：DCLPY — DDS Client Library for Python  
> 当前阶段：仅定义整体架构，不进入实现  
> 下层依赖：`dmw`

---

## 1. 文档目的

本文档仅定义 `dclpy` 的长期总体架构、模块边界和与 `dmw`/`dclcpp` 的关系。

当前阶段不冻结：

- 具体 Python 消息生成方式；
- pybind11 或 nanobind 最终选择；
- asyncio 细节；
- Python packaging 细节；
- API 的最终命名。

这些在 `dmw` 和 `dclcpp` 基础稳定后再进入详细设计。

---

## 2. 核心定位

`dclpy` 是与 `dclcpp` 平级的 Python Client Library。

禁止：

```text
dclpy → dclcpp → dmw
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

`_dclpy` 是 native extension，直接绑定 `dmw` C++ API。

`dmw` 负责两种 Client Library 必须共享的 language-neutral communication/runtime semantics，包括 Topic、Service、Timer、WaitSet 以及 Action 的公共协议和状态机；`dclpy` 只在其上增加 Python 类型、callback、Future、Executor、asyncio 和 GIL integration。不得在 Python 层重新实现一套与 `dclcpp` 平行的 middleware protocol state machine。

---

## 3. 总体架构

```text
                 Python Application
                         │
                         ▼
┌───────────────────────────────────────────────┐
│                    dclpy                      │
│                                               │
│ Context / Node                                │
│ Publisher / Subscription                      │
│ Client / Service                              │
│ Timer                                         │
│ ActionClient / ActionServer                   │
│ QoS                                           │
│ Executor / asyncio                            │
│ Python message wrappers                       │
└──────────────────────┬────────────────────────┘
                       ▼
┌───────────────────────────────────────────────┐
│                  _dclpy.so                    │
│                                               │
│ Python ↔ C++ object binding                  │
│ Python exception mapping                      │
│ GIL boundary                                  │
│ Native wait/publish/take                      │
└──────────────────────┬────────────────────────┘
                       ▼
                      dmw
                       │
                       ▼
                   Fast DDS
```

---

## 4. dclpy 与 dclcpp 的一致性原则

两个 Client Library 应保持“概念一致”，但不要求 API 字面一致。

共同概念：

```text
Context
Node
Publisher
Subscription
Client
Service
Timer
ActionClient
ActionServer
QoS
Executor
MessageType
ServiceType
ActionType
```

其中 Topic、Service、Timer、Action protocol/runtime state、WaitSet readiness 等 language-neutral 语义必须来自同一个 `dmw` runtime；两种 Client Library 不分别维护第二套 transport identity、Goal FSM、Action endpoint composition 或 Timer readiness 语义。

语言特性差异：

| 能力 | dclcpp | dclpy |
|---|---|---|
| 类型系统 | C++ templates | Python runtime types |
| Future | `std::future` 或自定义 | Python Future / asyncio Future |
| Callback | `std::function` | Python callable |
| Executor | C++ executor | Python executor / asyncio |
| Message | native C++ type | Python wrapper/native-bound type |
| Errors | exception/status | Python exceptions |

---

## 5. Native Binding

`dclpy` 不要求 `dmw` 提供 C API。

推荐架构：

```text
Python
 ↓
dclpy
 ↓
_dclpy native extension
 ↓
dmw C++
```

Native binding 技术可选择：

- pybind11；
- nanobind。

初始实现可优先选择团队熟悉且生态成熟的方案。

---

## 6. Message 架构

Python message 不应要求用户接触：

```text
MsgPubSubType
Fast DDS TypeSupport
```

理想 API：

```python
from my_interfaces import JointState

msg = JointState()
msg.position = [1.0, 2.0]

pub = node.create_publisher(
    JointState,
    "/joint_states",
    qos)

pub.publish(msg)
```

内部目标：

```text
Python JointState type
        │
        ├── native message binding
        └── dmw::MessageType
```

### 第一推荐方向

优先考虑：Fast DDS-Gen 生成 C++ Msg，然后对 C++ Msg 提供 Python binding。

```text
IDL
 ↓
fastddsgen
 ↓
C++ Msg + MsgPubSubType
 ↓
Python binding
 ↓
Python Msg object
```

优点：

- 不重新实现 Python CDR serializer；
- 不维护 Python ↔ C++ 字段转换副本；
- publish/take 可直接使用 native message memory；
- 与 `dmw::MessageType` 自然集成。

该方向需在正式实现阶段验证复杂字段、sequence、string、nested type 的 Python ergonomics。

---

## 7. Topic API 目标

```python
node = dclpy.Node("controller")

pub = node.create_publisher(
    JointState,
    "/joint_states",
    qos)

sub = node.create_subscription(
    JointState,
    "/joint_states",
    callback,
    qos)
```

Native path：

```text
Python Publisher
    ↓
_dclpy Publisher
    ↓
dmw::Publisher
```

Subscription callback 必须经过 Python Executor，而不是 DDS internal thread。

---

## 8. Service API 目标

```python
client = node.create_client(GetState, "/get_state")
future = client.call_async(request)
```

Server：

```python
service = node.create_service(
    GetState,
    "/get_state",
    callback)
```

DMW 仍负责 request identity / DDS correlation / service availability 和 blocking wait；DCLPY 负责 Python Future、pending Future completion 和 callback。Future registry 不下沉到 DMW，因为其 cancellation、exception 和 asyncio integration 属于 Python runtime semantics。

---

## 9. Action API 目标

Action 的 language-neutral protocol/runtime semantics 下沉到 DMW，DCLPY 不再使用 3 Service + 2 Topic 自行实现一套 Python-side Action protocol。

```text
dclpy.ActionClient / ActionServer
            ↓
          _dclpy
            ↓
dmw::ActionClient / ActionServer
            ↓
3 Service + 2 Topic
```

DMW 负责：

- Action endpoint composition；
- Goal identity；
- Goal state machine；
- goal/result/cancel correlation；
- Action availability；
- status bookkeeping；
- result/cancel 等公共 protocol state；
- WaitSet readiness；
- ROS 2 Action wire-level naming 和 compatibility semantics。

DCLPY 负责：

- Python Action type wrapper；
- Python ClientGoalHandle / ServerGoalHandle wrapper；
- Python Future；
- feedback/result/cancel callback；
- asyncio/await integration；
- Python exception 和 GIL boundary。

Action 详细 API 设计后置，但不得重新在 Python 层实现 DMW 已经拥有的 Goal FSM、endpoint composition 或 transport correlation。

---

## 10. Executor / asyncio

### 10.1 Timer

Timer 的 period、deadline/readiness、cancel/reset 和 WaitSet integration 属于 language-neutral runtime semantics，由 `dmw::Timer` 提供。Python Timer 只包装 native Timer 并保存 Python callback：

```text
dclpy.Timer
    ├── dmw::Timer
    └── Python callable
```

Timer callback 必须由 Python Executor 调度，不得从 DMW/Fast DDS internal thread 直接进入 Python。

### 10.2 Executor / asyncio

Python 侧不能简单复用 C++ Executor。

推荐分两阶段：

### Stage A

实现普通：

```text
dclpy.Executor
```

使用 native thread 调 `dmw::WaitSet::wait()`，ready 后回到 Python 调 callback。Subscription、Client、Service、Timer 和 Action common runtime 均通过 DMW readiness 接入，不在 Python Executor 内复制底层等待状态机。

### Stage B

增加：

```text
asyncio integration
```

目标：

- service futures 可 await；
- Action result 可 await；
- 不长期持有 GIL 阻塞 wait；
- wait 时释放 GIL；
- callback 进入 Python 前重新获取 GIL。

---

## 11. GIL 原则

正式实现必须遵守：

1. 阻塞 `dmw::WaitSet::wait()` 时释放 GIL；
2. publish/take 若执行时间可能较长，可评估释放 GIL；
3. 访问 Python object 必须持有 GIL；
4. native DDS thread 不直接进入 Python；
5. shutdown 时避免 native callback 与 Python interpreter teardown 竞态。

---

## 12. 错误映射

```text
dmw::ErrorCode
      ↓
_dclpy mapping
      ↓
Python exception
```

建议 Python exception hierarchy：

```text
DclpyError
├── InvalidArgumentError
├── InvalidStateError
├── TimeoutError
├── MiddlewareError
└── TypeError
```

最终名称以后冻结。

---

## 13. 包结构

```text
dclpy/
├── CMakeLists.txt
├── pyproject.toml
├── src/
│   └── dclpy/
│       ├── __init__.py
│       ├── context.py
│       ├── node.py
│       ├── publisher.py
│       ├── subscription.py
│       ├── client.py
│       ├── service.py
│       ├── timer.py
│       ├── action.py
│       ├── qos.py
│       └── executor.py
└── native/
    ├── module.cpp
    ├── context.cpp
    ├── node.cpp
    ├── publisher.cpp
    ├── subscription.cpp
    ├── client.cpp
    ├── service.cpp
    ├── timer.cpp
    ├── action.cpp
    ├── qos.cpp
    ├── wait_set.cpp
    └── detail/
```

生成：

```text
dclpy/_dclpy.so
```

---

## 14. 构建关系

```text
_dclpy
  │
  └── dmw
```

`_dclpy` 不链接 `dclcpp`。

Python package：

```text
dclpy → _dclpy
```

---

## 15. 与接口包的关系

未来接口包可能同时提供：

```text
my_interfaces
├── C++ Msg types
├── Fast DDS PubSubTypes
├── dclcpp-friendly binding
└── Python message binding
```

但 DCLPY V1 不要求 DCL 仓库先构建完整 interface ecosystem。

测试阶段可使用：

```text
test_interfaces/
```

验证 C++ ↔ Python interoperability。

---

## 16. 互操作测试目标

后续至少覆盖：

```text
dclcpp publisher → dclpy subscriber
dclpy publisher → dclcpp subscriber

dclcpp client → dclpy service
dclpy client → dclcpp service

dclcpp ActionClient → dclpy ActionServer
dclpy ActionClient → dclcpp ActionServer

dclpy ↔ ROS 2 Humble Topic
dclpy ↔ ROS 2 Humble Service
dclpy ↔ ROS 2 Humble Action
```

其中 Action 的 protocol correctness 由 DMW 公共 runtime 验证；跨语言测试重点验证 typed/Python binding、Future、callback 与 Executor integration。

---

## 17. 实现前置条件

DCLPY 正式开发建议等以下能力稳定后再开始：

1. `dmw::Context/Node` 生命周期稳定；
2. `dmw::MessageType` 稳定；
3. Publisher/Subscription 稳定；
4. WaitSet 稳定；
5. Service request/reply 稳定；
6. `dmw::Timer` 与 WaitSet integration 稳定；
7. DMW Action common runtime 的 public boundary 稳定；
8. `dclcpp` 已验证 Topic/Service；
9. ROS 2 compatibility 的底层 naming/type/QoS 规则已基本冻结。

Python Action wrapper 可在 Python Topic/Service/Timer binding 稳定后实现，但不需要等待 Python 重新实现 Action protocol。

---

## 18. 当前冻结项

当前只冻结：

1. `dclpy` 与 `dclcpp` 平级；
2. `dclpy` 不依赖 `dclcpp`；
3. native extension 直接调用 `dmw`；
4. `dmw` 不因 Python 而改成 C API；
5. Python callback 不运行在 DDS internal thread；
6. Python Executor 独立设计；
7. Timer 与 Action 的 language-neutral runtime semantics 由 DMW 提供，DCLPY 不重复实现；
8. Python Future、callback、asyncio、GIL 和语言级 GoalHandle 保留在 DCLPY；
9. Python Message 复用 `dmw::MessageType`；
10. 优先评估“绑定 Fast DDS-Gen C++ Msg”而不是自行实现 Python serialization；
11. 具体 binding 技术和 API 细节后置。

---

## 19. 总结

DCLPY 的长期定位是：

```text
Pythonic API
    ↓
Native binding
    ↓
DMW shared runtime
    ↓
Fast DDS
```

它与 DCLCPP 共享 middleware/runtime 核心，包括语言无关的 Topic、Service、Timer、Action protocol 和等待语义，但不共享 Client Library 实现。这样既能保证两种语言的行为一致，又允许 Python 使用适合自身的 Future、asyncio、GIL、callback 和 Executor 模型，同时不会迫使 DMW 为 Python 改为 C API。
