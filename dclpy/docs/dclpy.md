# DCLPY 整体架构设计

> 文档状态：V1 Architecture Frozen Candidate  
> 模块名称：DCLPY — DDS Client Library for Python  
> 下层依赖：`dmw`  
> Native binding：`_dclpy`

## 1. 文档目的

本文档定义 DCLPY 的系统边界、Python/native 分层、Topic/Service/Timer/Action/Graph API 方向、Future/Executor/GIL 语义，以及与 DMW 的责任分工。

以下原则冻结：

- DCLPY 与 DCLCPP 平级；
- `_dclpy` 直接绑定 DMW，不通过 DCLCPP；
- DMW 是唯一 language-neutral runtime authority；
- Python Future/callback/Executor/asyncio/GIL/exception mapping 不下沉。

具体 Python message codegen、pybind11/nanobind 选择和 packaging 细节可以后续冻结。

## 2. 核心定位

禁止：

```text
dclpy -> dclcpp -> dmw
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

DMW 提供：

```text
Topic
Service
Service availability/wait
Timer
Action common runtime
Graph snapshot/change
QoS profiles
WaitSet
```

DCLPY 提供：

```text
Python types
Python Future
Pending Future registry
Python callback
Executor / asyncio
Python typed Action result cache
Python exceptions
GIL management
```

## 3. 总体架构

```text
                 Python Application
                         │
                         ▼
┌───────────────────────────────────────────────┐
│                    dclpy                      │
│ Context / Node                                │
│ Publisher / Subscription                      │
│ Client / Service                              │
│ Timer                                         │
│ ActionClient / ActionServer / GoalHandle      │
│ QoS / Graph value wrappers                    │
│ Future / Executor / asyncio                   │
└──────────────────────┬────────────────────────┘
                       ▼
┌───────────────────────────────────────────────┐
│                  _dclpy.so                    │
│ Python <-> C++ binding                        │
│ message/value conversion                      │
│ exception mapping                             │
│ GIL boundary                                  │
│ DMW object ownership bridge                   │
└──────────────────────┬────────────────────────┘
                       ▼
                      dmw
                       │
                       ▼
                   Fast DDS
```

`_dclpy` 不成为第二个 middleware runtime。

## 4. 与 DCLCPP 的一致性

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
GoalHandle
QoS
GraphSnapshot / GraphEvent
Executor
MessageType
ServiceType
ActionType
```

概念一致不等于 API 字面一致。

| 能力 | dclcpp | dclpy |
| --- | --- | --- |
| 类型 | C++ templates | Python runtime/native-bound types |
| Future | `std::future` / promise | Python/asyncio Future |
| Pending registry | C++ container | Python/native mapping |
| Callback | `std::function` | Python callable |
| Executor | C++ | Python/asyncio |
| Result payload | C++ typed object | Python/native typed object |
| Exception | C++ | Python |
| GIL | N/A | DCLPY authority |

不应为了消除这些合理差异而把语言 runtime 塞入 DMW。

## 5. Native binding

推荐：

```text
Python
  ↓
dclpy
  ↓
_dclpy
  ↓
dmw C++
```

Native binding 可使用 pybind11 或 nanobind；最终选择以后冻结。

`_dclpy` 负责：

- object lifetime bridge；
- Python/native message conversion；
- DMW value conversion；
- GIL release/acquire；
- ErrorCode -> Python exception；
- native wait entry。

它不维护独立 discovery graph、Goal FSM 或 service correlation。

## 6. Message 架构

Python 用户不接触：

```text
MsgPubSubType
Fast DDS TypeSupport
```

理想 API：

```python
msg = JointState()
msg.position = [1.0, 2.0]

pub = node.create_publisher(
    JointState,
    "/joint_states",
    qos)

pub.publish(msg)
```

第一推荐方向：

```text
IDL
 ↓
Fast DDS-Gen
 ↓
C++ Msg + PubSubType
 ↓
native Python binding
 ↓
Python Msg
```

这样避免重新实现 Python CDR serializer。

内部关联：

```text
Python message type
    ├── native C++ message type
    └── dmw::MessageType
```

## 7. Topic

```python
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

发送：

```text
Python message
    ↓
_dclpy native message
    ↓
dmw::Publisher::write()
```

接收：

```text
DMW Subscriber ready
    ↓
dmw::Subscriber::read()
    ↓
Python/native message wrapper
    ↓
Python Executor callback
```

Fast DDS listener不得直接进入 Python。

## 8. Service

### 8.1 Client

```python
client = node.create_client(GetState, "/get_state")
future = client.call_async(request)
```

DCLPY 维护：

```text
dmw::RequestId -> Python Future
```

响应：

```text
DMW Client ready
    ↓
read_response()
    ↓
RequestId
    ↓
Pending Future registry
    ↓
future.set_result(response)
```

### 8.2 Server

```python
service = node.create_service(
    GetState,
    "/get_state",
    callback)
```

Executor读取 typed request、调用 Python callback，再使用 DMW RequestId 写 response。

### 8.3 availability

Python 不使用 `time.sleep()` polling：

```text
Client.wait_for_service()
    ↓
release GIL
    ↓
dmw::Client::wait_for_service(WaitTimeout)
    ↓
re-acquire GIL
```

Service pairing/discovery authority在 DMW。

## 9. QoS

```text
dclpy.QoS
    ↓
_dclpy
    ↓
dmw::Qos
```

Common preset 数值不在 Python 重复硬编码：

```text
system_default
ros2_default
ros2_sensor_data
ros2_services_default
ros2_parameters
ros2_parameter_events
ros2_action_status_default
```

Python 可以提供熟悉的名称：

```python
qos_profile_sensor_data
qos_profile_services_default
qos_profile_action_status_default
```

但值由 `dmw::Qos` 构造。

## 10. Action 总体边界

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

- endpoint composition；
- GoalId / GoalInfo；
- Goal FSM；
- Goal registry；
- cancel candidate selection；
- Service correlation；
- Action availability；
- terminal Goal / pending result RequestIds / expiry；
- status snapshot；
- aggregate WaitSet readiness。

DCLPY 负责：

- Python Action type/message fields；
- Python GoalHandle；
- user goal/cancel/feedback callbacks；
- Python Future；
- Pending Future registry；
- typed Python/native result payload cache；
- asyncio/GIL/exception mapping。

## 11. Action metadata adapter

DMW 不理解任意 Python/generated message layout。

`_dclpy` 把公共字段转换为：

```text
UUID -> dmw::GoalId
accepted timestamp -> dmw::GoalInfo
CancelGoal GoalInfo -> dmw::CancelGoalCriteria
status list <-> vector<dmw::GoalStatusInfo>
```

Goal FSM 和匹配不在 Python 重复实现。

## 12. ActionClient

Python language state：

```text
Goal RequestId   -> Python Future
Cancel RequestId -> Python Future
Result RequestId -> Python Future
GoalId           -> feedback callback / weak ClientGoalHandle
```

### 12.1 Aggregate readiness

一个 DMW ActionClient token 对应：

```cpp
ActionClientReadySet {
    goal_response,
    cancel_response,
    result_response,
    feedback,
    status
}
```

Python Executor 不分别注册底层五个 endpoint。

### 12.2 wait_for_server

```text
Python wait_for_server()
    ↓
release GIL
    ↓
dmw::ActionClient::wait_for_server()
    ↓
re-acquire GIL
```

不轮询 endpoint count。

## 13. ActionServer

### 13.1 Goal accept

流程：

```text
DMW ActionServer ready
    ↓
take typed SendGoal request
    ↓
extract GoalId
    ↓
Python goal callback
    ├── reject
    │    -> typed reject response -> DMW write/reject path
    │
    └── accept
         -> build typed accepted response
         -> DMW accept transaction:
              reserve GoalId
              write accepted response
              commit Accepted / optional Executing
              rollback reservation on write failure
```

Python 不直接操作 Goal registry。

### 13.2 ServerGoalHandle

Python `ServerGoalHandle` 保存 GoalId 和 native ActionServer reference。

`succeed()` / `abort()` / `canceled()` 等操作转换为 DMW GoalEvent。

### 13.3 Cancel

```text
raw/typed CancelGoal request
    ↓
CancelGoalCriteria
    ↓
DMW select_cancel_goals()
    ↓
Python cancel callback per candidate
    ├── reject -> no transition
    └── accept -> DMW CancelGoal transition
    ↓
build typed response
    ↓
DMW write_cancel_response()
```

### 13.4 Result lifecycle

DMW 保存：

```text
terminal Goal state
pending GetResult RequestIds
result timeout / expiry
```

DCLPY 保存：

```text
GoalId -> Python/native typed result payload/response
```

terminal flow：

1. Python/native wrapper准备 typed result；
2. 保存 typed cache；
3. 提交 DMW terminal transition；
4. 取得 pending RequestIds；
5. 为每个 RequestId发送 typed response；
6. DMW result timeout到期产生 expired GoalId；
7. Python/native cache删除对应 result。

DMW 不缓存任意 Python object。

### 13.5 status

DMW返回 `GoalStatusInfo` snapshot；native binding装配实际 status message。

## 14. Timer

Python wrapper：

```text
dclpy.Timer
├── dmw::Timer
└── Python callable
```

推荐 API：

```python
timer = node.create_timer(
    period_sec,
    callback,
    autostart=True)

timer.cancel()
timer.reset()
ready = timer.is_ready()
remaining = timer.time_until_next_call()
```

若提供 `timer.period = value` 或 `set_period()` convenience，native binding必须调用 DMW `exchange_period()`，不得重新定义 deadline semantics。

Executor：

```text
Timer token ready
    ↓
release/no Python object access as appropriate
    ↓
dmw::Timer::consume(TimerInfo&)
    ├── false -> stale readiness, no Python callback
    └── true
          ↓
       acquire GIL
          ↓
       Python callback
```

Python 不计算 missed cycle 或 next deadline。

## 15. Graph

DCLPY 不维护 Python discovery authority cache。

```text
Fast DDS discovery
    ↓
DMW DiscoveryGraph
    ↓
GraphSnapshot / GraphEvent
    ↓
_dclpy value conversion
    ↓
Python list/dict/value objects
```

V1 可以暴露：

```python
context.get_graph_snapshot()
node.get_topic_names_and_types()
node.get_service_names_and_types()
node.get_action_names()
node.count_publishers(name)
node.count_subscribers(name)
```

这些每次来自 DMW snapshot/query。

**V1 不提供可靠 ROS Node graph。**

因此不应在 DCLPY V1 承诺：

```text
get_node_names()
get_node_names_and_namespaces()
```

除非后续 DMW 正式实现 ROS Graph metadata protocol。

GraphEvent 可以作为 Executor internal waitable，用于 topology/introspection refresh。

## 16. Executor

Python Executor 与 C++ Executor 独立实现，但共享 DMW WaitSet。

### 16.1 Stage A

普通 Python Executor：

```text
release GIL
    ↓
dmw::WaitSet::wait()
    ↓
ready tokens
    ↓
re-acquire GIL only when Python work is required
```

支持：

```text
Subscription
Client
Server
Timer
ActionClient
ActionServer
GraphEvent
Guard/Event as needed
```

### 16.2 Stage B — asyncio

目标：

- Service Future 可 await；
- Action Goal/Cancel/Result Future 可 await；
- native wait 不长期持有 GIL；
- readiness转换为 asyncio scheduling，而不是 DDS thread调用 coroutine。

不要求 DMW 认识 asyncio loop。

## 17. GIL 原则

必须满足：

1. 阻塞 DMW wait 时释放 GIL；
2. 访问 Python object 时持 GIL；
3. Fast DDS/DMW listener 不直接进入 Python；
4. shutdown/interpreter teardown 避免 native state回调悬挂 Python object；
5. Timer/Graph/Action common state不依赖 GIL；
6. publish/read可根据实际 blocking profile评估是否释放 GIL，但不得在无 GIL 状态访问 Python wrapper memory。

## 18. Error mapping

```text
dmw::ErrorCode
    ↓
_dclpy centralized mapping
    ↓
Python exception
```

建议 hierarchy：

```text
DclpyError
├── InvalidArgumentError
├── InvalidStateError
├── TimeoutError
├── MiddlewareError
└── TypeError
```

Timer/Graph/Action 不各自定义额外异常体系。

## 19. Ownership

Python wrapper 持有 native shared/unique holder，native holder再拥有/引用 DMW resource。

要求：

- Python GC 不直接理解 Fast DDS lifetime；
- native destructor遵守 DMW RAII；
- active wait退出并同步后再销毁 WaitSet wrapper；
- Context Python object销毁后，已有 child native backing仍可安全 teardown；
- interpreter finalization时禁止 native callback再创建 Python object。

## 20. 包结构

```text
dclpy/
├── CMakeLists.txt
├── pyproject.toml
├── src/dclpy/
│   ├── __init__.py
│   ├── context.py
│   ├── node.py
│   ├── publisher.py
│   ├── subscription.py
│   ├── client.py
│   ├── service.py
│   ├── timer.py
│   ├── action.py
│   ├── graph.py
│   ├── qos.py
│   └── executor.py
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
    └── wait_set.cpp
```

不建立 `dclpy -> dclcpp` 链接依赖。

## 21. 接口包方向

未来 interface package 可以同时提供：

```text
C++ Msg
PubSubType
DCLCPP binding
Python native message binding
```

但 DCLPY V1 不以前置完整 interface ecosystem 为条件。

## 22. 测试

### 22.1 Topic

- Python publish/subscription；
- dclcpp <-> dclpy；
- Python callback thread/GIL；
- ROS 2 interoperability。

### 22.2 Service

- Python Future completion；
- unavailable service；
- cancellation/exception；
- GIL release during wait；
- C++/Python cross communication。

### 22.3 Timer

- callback只在 `consume()` true时执行；
- cancel/reset/exchange wrapper；
- no Python callback from native thread；
- Executor integration。

完整 scheduling tests属于 DMW。

### 22.4 Action

- typed/native metadata adapter；
- GoalHandle；
- accept transaction wrapper；
- Future registries；
- typed Python result cache；
- callback policy；
- aggregate ReadySet dispatch；
- expiry removes Python result cache；
- dclcpp <-> dclpy Action。

Goal FSM/cancel matching/result RequestId protocol不在 Python重复完整测试矩阵。

### 22.5 Graph

- GraphSnapshot -> Python values；
- GraphEvent Executor integration；
- no duplicate Python cache；
- no V1 Node graph claim。

## 23. 实现前置条件

DCLPY 正式开发前，以下 DMW 能力应先稳定：

1. Context/Node lifecycle；
2. MessageType；
3. Topic；
4. Service；
5. WaitSet；
6. Timer；
7. GraphSnapshot/GraphEvent；
8. Action common runtime；
9. ROS 2 naming/type/QoS/correlation rules。

不需要等待 DCLCPP 完成所有高级功能；DCLPY 与 DCLCPP 在 DMW 之上平级演进。

## 24. V1 冻结项

1. DCLPY 与 DCLCPP 平级。
2. `_dclpy` 直接调用 DMW。
3. DMW 不因 Python 改成 C API。
4. Python callback不在 DDS/DMW internal thread。
5. Service Future registry留 DCLPY。
6. Timer scheduling在 DMW；Python Executor使用 `Timer::consume()`。
7. Action endpoint composition/Goal FSM/cancel/result common state在 DMW。
8. typed Python result payload cache留 DCLPY。
9. Goal accept通过 DMW transaction path。
10. Action ReadySet来自 DMW aggregate waitable。
11. Graph authority在 DMW；V1不承诺 ROS Node graph。
12. QoS common profile数值只在 DMW。
13. Python Executor独立设计，但使用 DMW WaitSet。
14. asyncio/GIL/exception mapping只在 Python layer。
15. Python message优先复用 generated/native C++ type而不是重写 CDR serializer。

## 25. 总结

DCLPY 的长期结构是：

```text
Pythonic API / Future / asyncio / GIL
                ↓
              _dclpy
                ↓
               DMW
 Topic / Service / Timer / Action / Graph / Wait
                ↓
            Fast DDS
```

它与 DCLCPP 共用协议和 runtime state，但不共用语言层实现。这样可以避免 C++/Python 两套 Goal FSM、两套 Graph cache和两套 Timer scheduler，同时保留 Python 对 Future、asyncio、exception 和 GIL 的自然控制权。
