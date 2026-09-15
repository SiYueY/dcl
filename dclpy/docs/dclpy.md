# DCLPY 整体架构设计

> 文档状态：Architecture Draft V0.2  
> 模块名称：DCLPY — DDS Client Library for Python  
> 当前阶段：定义整体架构与 DMW common-runtime 映射，不进入完整实现  
> 下层依赖：`dmw`

---

## 1. 文档目的

本文档定义 `dclpy` 的总体架构、模块边界和与 `dmw`/`dclcpp` 的关系，并明确 Python Client Library 如何复用 DMW 已下沉的 Timer、Graph、Service availability 和 Action common runtime。

当前阶段不冻结：

- 具体 Python 消息生成方式；
- pybind11 或 nanobind 最终选择；
- asyncio 调度细节；
- Python packaging 细节；
- 用户 API 的最终字面命名。

但以下职责边界已经冻结：DMW 是唯一 language-neutral runtime authority；Python Future/callback/Executor/GIL/exception presentation 只属于 DCLPY。

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

`dmw` 负责两种 Client Library 必须共享的 language-neutral communication/runtime semantics，包括 Topic、Service、Service availability wait、Timer、Graph snapshot/change、WaitSet、common QoS profiles，以及 Action 的公共协议和状态机；`dclpy` 只在其上增加 Python 类型、callback、Future、Executor、asyncio、exception 和 GIL integration。不得在 Python 层重新实现一套与 `dclcpp` 平行的 middleware protocol state machine。

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
│ QoS / Graph value wrappers                    │
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
│ Timer/Graph/Action native wrapper             │
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
GraphSnapshot / GraphEvent
Executor
MessageType
ServiceType
ActionType
```

其中 Topic、Service、Timer、Graph、Action protocol/runtime state、WaitSet readiness 等 language-neutral 语义必须来自同一个 `dmw` runtime；两种 Client Library 不分别维护第二套 transport identity、Graph cache、Goal FSM、Action endpoint composition 或 Timer readiness 语义。

语言特性差异：

| 能力 | dclcpp | dclpy |
|---|---|---|
| 类型系统 | C++ templates | Python runtime types |
| Future | `std::future` / promise | Python Future / asyncio Future |
| Pending Future registry | C++ container | Python/native mapping |
| Callback | `std::function` | Python callable |
| Executor | C++ executor | Python executor / asyncio |
| Message | native C++ type | Python wrapper/native-bound type |
| Errors | C++ exception/status | Python exceptions |
| GIL | 不适用 | DCLPY authority |

这些差异是合理的语言层重复，不应为了“统一实现”而下沉到 DMW。

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

`_dclpy` 只做 binding、ownership bridge、GIL boundary、type conversion 和 exception translation；不得成为第二个 middleware/runtime implementation。

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

Subscription callback 必须经过 Python Executor，而不是 DDS/DMW internal thread。

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

DMW 负责 request identity / DDS correlation / service availability 和 blocking wait；DCLPY 负责 Python Future、pending Future completion 和 callback。Future registry 不下沉到 DMW，因为其 cancellation、exception 和 asyncio integration 属于 Python runtime semantics。

### 8.1 `wait_for_service`

Python 层不实现 `time.sleep()` 轮询。

```text
dclpy.Client.wait_for_service()
        ↓
_dclpy releases GIL
        ↓
dmw::Client::wait_for_service(WaitTimeout)
```

等待结束后重新获取 GIL 并返回 Python `bool` 或抛出对应异常。

---

## 9. QoS

Python QoS 对象最终映射到 `dmw::Qos`。

```text
dclpy.QoS
    ↓
_dclpy
    ↓
dmw::Qos
```

### 9.1 Common profile authority

以下 profile 的实际数值不得在 Python 再硬编码一套：

```text
system_default
ros2_default
ros2_sensor_data
ros2_services_default
ros2_parameters
ros2_parameter_events
ros2_action_status_default
```

Python 可暴露熟悉的模块级常量或 factory：

```python
qos_profile_sensor_data
qos_profile_services_default
qos_profile_action_status_default
```

但 `_dclpy` 必须从对应 `dmw::Qos` profile 构造，确保 DCLCPP/DCLPY 只有一个 common preset authority。

---

## 10. Action API 目标

Action 的 language-neutral protocol/runtime semantics 位于 DMW，DCLPY 不使用 3 Service + 2 Topic 自行实现一套 Python-side Action protocol。

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
- Goal registry；
- goal/result/cancel transport correlation；
- cancel criteria matching；
- Action availability / blocking wait；
- status snapshot；
- result lifecycle common state；
- aggregate WaitSet readiness；
- ROS 2 Action wire-level naming 和 compatibility semantics。

DCLPY 负责：

- Python Action type wrapper；
- Python generated/action message 字段访问；
- Python ClientGoalHandle / ServerGoalHandle wrapper；
- Python Future 与 Pending Future registry；
- feedback/result/cancel callback；
- asyncio/await integration；
- Python exception 和 GIL boundary。

### 10.1 Typed message 与 DMW protocol value

DMW 不理解任意 Python/generated Action message layout。`_dclpy`/Python binding 负责把 typed/native message 中的公共协议字段转换为：

```text
dmw::GoalId
dmw::GoalInfo
dmw::GoalState
dmw::CancelGoalCriteria
dmw::GoalStatusInfo
```

例如 SendGoal request 中的 UUID、CancelGoal request 中的 GoalInfo/timestamp、Status message 的 GoalStatus 列表。

状态转换和匹配发生在 DMW；Python 只负责 wire message/value 的映射和用户 policy callback。

### 10.2 ActionClient Future registry

DCLPY 维护语言层映射：

```text
Goal request RequestId   -> Python Future
Cancel request RequestId -> Python Future
Result request RequestId -> Python Future
GoalId                   -> feedback callback / weak ClientGoalHandle
```

这与 ROS 2 `rclpy` 的职责类似，但 transport request/response identity 与五个 endpoint readiness 不在 Python 重复实现。

### 10.3 Aggregate readiness

`dmw::ActionClient` 在 DMW WaitSet 中作为一个 logical waitable。ready 后 `_dclpy` 读取 `ActionClientReady`：

```text
goal_response
cancel_response
result_response
feedback
status
```

`dmw::ActionServer` 对应：

```text
goal_request
cancel_request
result_request
```

Python Executor 根据子通道 readiness 创建 Python task/callback，不分别注册五个/三个底层 transport endpoint。

### 10.4 Goal/cancel policy

用户 goal/cancel/execute callback 仍属于 Python policy：

```text
DMW takes protocol request / computes common state
        ↓
Python Executor callback decides accept/reject
        ↓
_dclpy commits DMW Goal state transition
        ↓
write typed response
```

DMW 是 Goal FSM authority，Python callback 只是 policy decision authority。

---

## 11. Timer / Graph / Executor / asyncio

### 11.1 Timer

Timer 的 period、deadline/readiness、cancel/reset 和 WaitSet integration 属于 language-neutral runtime semantics，由 `dmw::Timer` 提供。Python Timer 只包装 native Timer 并保存 Python callback：

```text
dclpy.Timer
    ├── dmw::Timer
    └── Python callable
```

推荐 Python API：

```python
timer = node.create_timer(period_sec, callback, autostart=True)
timer.cancel()
timer.reset()
ready = timer.is_ready()
remaining_ns = timer.time_until_next_call()
```

Timer callback 必须由 Python Executor 调度，不得从 DMW/Fast DDS internal thread 直接进入 Python。

Executor 收到 Timer ready 后由 native binding 调用 `dmw::Timer::take(TimerInfo&)` 消费本次触发，再把 TimerInfo 转换为 Python value 并执行 callback。DMW 负责 missed-cycle skip/re-align semantics，Python 不重新计算 deadline。

### 11.2 Graph

DCLPY 不维护 Python discovery cache。

```text
Fast DDS discovery
        ↓
DMW GraphSnapshot / revision / GraphEvent
        ↓
_dclpy value conversion
        ↓
Python list/dict/value objects
```

Python 可以暴露：

```python
context.get_graph_snapshot()
node.get_topic_names_and_types()
node.get_service_names_and_types()
node.count_publishers(name)
node.count_subscribers(name)
```

这些接口每次基于 DMW snapshot/query。GraphEvent 可加入 Executor 的 native WaitSet 以响应 topology 变化；Python 层不建立长期 authority cache。

### 11.3 Executor / asyncio

Python 侧不能简单复用 C++ Executor。

推荐分两阶段：

#### Stage A

实现普通：

```text
dclpy.Executor
```

使用 native thread 调 `dmw::WaitSet::wait()`，ready 后回到 Python 调 callback。Subscription、Client、Service、Timer、Action common runtime 和 GraphEvent 均通过 DMW readiness 接入，不在 Python Executor 内复制底层等待状态机。

#### Stage B

增加：

```text
asyncio integration
```

目标：

- service futures 可 await；
- Action goal/result/cancel Future 可 await；
- 不长期持有 GIL 阻塞 wait；
- wait 时释放 GIL；
- callback/task 进入 Python 前重新获取 GIL。

---

## 12. GIL 原则

正式实现必须遵守：

1. 阻塞 `dmw::WaitSet::wait()`、`wait_for_service()`、`wait_for_server()` 时释放 GIL；
2. publish/take 若执行时间可能较长，可评估释放 GIL；
3. 访问 Python object 必须持有 GIL；
4. Fast DDS/DMW native thread 不直接进入 Python；
5. shutdown 时避免 native callback 与 Python interpreter teardown 竞态；
6. Graph/Timer/Action native state transition 不依赖持有 GIL。

---

## 13. 错误映射

```text
dmw::ErrorCode
      ↓
_dclpy centralized mapping
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

Exception mapping 是 DCLPY authority；DMW 不抛 Python exception。所有 endpoint、Timer、Graph、Action 使用同一个集中 mapping，不各自定义 Python 异常规则。

---

## 14. 包结构

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
│       ├── graph.py
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
    ├── graph.cpp
    ├── qos.cpp
    ├── wait_set.cpp
    └── detail/
```

生成：

```text
dclpy/_dclpy.so
```

---

## 15. 构建关系

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

## 16. 与接口包的关系

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

## 17. 互操作测试目标

后续至少覆盖：

```text
dclcpp publisher → dclpy subscriber
dclpy publisher → dclcpp subscriber

dclcpp client → dclpy service
dclpy client → dclcpp service

dclcpp ActionClient → dclpy ActionServer
dclpy ActionClient → dclcpp ActionServer

dclpy ↔ ROS 2 Humble/Jazzy Topic
dclpy ↔ ROS 2 Humble/Jazzy Service
dclpy ↔ ROS 2 Humble/Jazzy Action
```

其中 Timer/Graph/Action 的 common runtime correctness 由 DMW 验证；跨语言测试重点验证 native binding、Python value conversion、Future、callback、GIL 与 Executor integration。

---

## 18. 实现前置条件

DCLPY 正式开发建议等以下能力稳定后再开始：

1. `dmw::Context/Node` 生命周期稳定；
2. `dmw::MessageType` 稳定；
3. Publisher/Subscriber 稳定；
4. WaitSet 稳定；
5. Service request/reply 与 `wait_for_service()` 稳定；
6. common QoS profiles 稳定；
7. `dmw::Timer` 与 WaitSet integration 稳定；
8. DMW GraphSnapshot/GraphEvent 稳定；
9. DMW Action common runtime public contract 稳定；
10. `dclcpp` 已验证 Topic/Service 基础 wrapper；
11. ROS 2 compatibility 的底层 naming/type/QoS/Action 规则已基本冻结。

Python Action wrapper 可在 Python Topic/Service/Timer binding 稳定后实现，但不需要等待 Python 重新实现 Action protocol。

---

## 19. 当前冻结项

当前冻结：

1. `dclpy` 与 `dclcpp` 平级；
2. `dclpy` 不依赖 `dclcpp`；
3. native extension 直接调用 `dmw`；
4. `dmw` 不因 Python 而改成 C API；
5. Python callback 不运行在 DDS/DMW internal thread；
6. Python Executor 独立设计；
7. Service availability/wait 直接复用 DMW，不 Python polling；
8. common QoS profile 数值来自 DMW；
9. Timer 的 scheduling/readiness 由 DMW 提供，DCLPY 只保留 Python callback；
10. Graph authority/cache/revision 由 DMW 提供，DCLPY 只转换 value；
11. Action endpoint composition、Goal FSM/registry、cancel matching、availability 和 common result/status state 由 DMW 提供；
12. Python Future、Pending Future registry、callback、asyncio、GIL 和语言级 GoalHandle 保留在 DCLPY；
13. Action typed message 字段映射由 Python/native type adapter 负责，不在 DMW 引入 Python/ROS message dependency；
14. Python Message 复用 `dmw::MessageType`；
15. 优先评估“绑定 Fast DDS-Gen C++ Msg”而不是自行实现 Python serialization；
16. Python exception mapping 保留 DCLPY；
17. 具体 binding 技术和用户 API 字面细节后置。

---

## 20. 总结

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

它与 DCLCPP 共享 middleware/runtime 核心，包括语言无关的 Topic、Service、common QoS profiles、Timer、Graph、Action protocol 和等待语义，但不共享 Client Library 实现。这样既能保证两种语言的行为一致，又允许 Python 使用适合自身的 Future、asyncio、GIL、callback、exception 和 Executor 模型，同时不会迫使 DMW 为 Python 改为 C API。