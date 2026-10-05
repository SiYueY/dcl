# DCLPY V1 设计与实现规格

| 属性 | 定义 |
| --- | --- |
| 文档状态 | V1 Implementation Specification |
| 模块 | DCLPY — DDS Client Library for Python |
| Python package | `dclpy` |
| Python C++ extension | `_dclpy` |
| Binding implementation | `src/_dclpy/` |
| 下层 runtime | `dmw` |
| DDS backend | Fast DDS |
| C++ 标准 | C++17 |
| Python binding | pybind11 |
| Python build backend | scikit-build-core |
| 当前首要环境 | Ubuntu 22.04 / ROS 2 Humble / Fast DDS 2.6.x |
| 当前首要 Python | CPython 3.12 |
| V1 Python 支持 | CPython 3.10 / 3.11 / 3.12 / 3.13 |
| ROS interface 基础 | ROSIDL C++ + `rosidl_typesupport_fastrtps_cpp` |
| 当前集成 interface package | `mfr3duo_msgs` |
| V1 必须验证 Action | `mfr3duo_msgs/action/Move`、`mfr3duo_msgs/action/Grasp` |

本文档定义 DCLPY V1 的架构、API、并发、生命周期、类型绑定、Executor、asyncio、Service、Action、构建、部署和测试契约。

Phase 0中的新增 API和修复项是交付前置条件；本文中的目标契约不表示当前源码已实现这些能力。

除明确标记为后续能力的内容之外，开发实现不应再次自行引入另一套：

```text
生命周期模型
Future 状态模型
Executor 调度模型
Action 状态模型
Message ownership 模型
Python/C++ binding ABI
```

当前核心目标：

```text
rodesk
   │
   ▼
roserver / roboagent
       Python 3.12
          │
          ▼
        dclpy
          │
          ▼
        _dclpy
          │
          ▼
          dmw
          │
          ▼
       Fast DDS
          │
       DDS / RTPS
          │
          ▼
   rmw_fastrtps_cpp
          │
          ▼
    mfr3duo_ros2
     ROS 2 Humble
```

最终必须实现：

```text
Python ABI compatibility
        ↓
DCLPY binding / CPython-specific wheel
```

与：

```text
ROS 2 wire compatibility
        ↓
ROSIDL C++ / DMW / Fast DDS
```

彻底解耦。

---

# 1. 架构边界、API 参考与 DMW 前置条件

## 1.1 总体分层

固定：

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
```

禁止：

```text
dclpy -> dclcpp -> dmw
```

禁止：

```text
dclpy -> rclpy -> rcl/rmw
```

DCLPY 与 DCLCPP 是 DMW 之上的平级 Client Library。

## 1.2 三类设计权威

DCLPY 不试图重新发明 ROS Python Client Library 的用户模型，但也不承诺 `rclpy` 二进制或 API 完全兼容。

冻结以下原则：

### Python public API

**优先参考 rclpy。**

对于已有成熟概念，尽量沿用名称和使用习惯：

```text
Node
Publisher
Subscription
Client
Service
Timer

Future
SingleThreadedExecutor

ActionClient
ActionServer
ClientGoalHandle
ServerGoalHandle

create_publisher()
create_subscription()
create_client()
create_service()
create_timer()

call_async()
wait_for_service()

send_goal_async()
get_result_async()
cancel_goal_async()

succeed()
abort()
canceled()
```

没有明确理由时不人为创造不同的名称。

### Runtime semantics

**以 DMW public/runtime contract 为直接权威。**

例如：

```text
WaitSet
Timer scheduling
Graph
RequestId
Service correlation
Goal FSM
Action expiry
QoS
```

DCLPY不复制实现。

### ROS wire/protocol semantics

**参考 ROS 2 specification、rcl 和 rcl_action。**

尤其：

```text
Action CancelGoal
GetResult
GoalStatus
Goal acceptance
result retention
ROS naming
QoS
```

必须保持 wire-compatible 行为。

因此总体原则是：

> API 形态 rclpy-first，runtime DMW-first，ROS protocol rcl/rcl_action-first。

## 1.3 DCLPY 与 DMW职责

DMW继续负责：

```text
Context / Node

Topic primitive

Service correlation
Service availability

WaitSet
GuardCondition
Event

Timer scheduling

Graph

Parameter common state

Action endpoint composition
Goal FSM
Cancel selection
Result retention
Result expiry

QoS
```

DCLPY负责：

```text
Python object model
Python/C++ binding

Python Future
callback

SingleThreadedExecutor
AsyncIOExecutor

TaskRegistry
NativeIoDispatcher

Python exception
GIL

generated ROS interface binding

Action typed result payload
Python GoalHandle
```

## 1.4 V1不实现

V1明确不实现：

```text
rclpy compatibility shim
完整 rclpy API clone

MultiThreadedExecutor
callback group

Python CDR serializer

通用 runtime ROS introspection serializer

Fast DDS backend abstraction

CPython stable ABI / abi3

一个 extension 跨多个 Python minor version

自动 /clock subscription

完整 ROS logical Node graph
```

这些能力不得阻塞 V1。

---

## 1.5 Phase 0：修复 DMW Action 过期通知

当前任何可以 prune expired Goal 的路径都必须保留过期通知。

`ActionGoalRegistry`增加：

```cpp
std::vector<GoalId> expired_notifications_;
```

所有删除过期 Goal 的路径统一：

```text
detect expiration
        ↓
append GoalId to expired_notifications_
        ↓
erase GoalRecord
```

包括：

```text
status_snapshot()
register_result_request()
take_expired_goals()
未来其他 prune path
```

不能：

```text
只有 take_expired_goals() 自己删除时才返回 GoalId
```

### `take_expired_goals()`

固定语义：

```text
prune newly expired goals
        ↓
drain expired_notifications_
        ↓
return every unconsumed expired GoalId
```

同一个 Goal：

```text
删除一次
通知一次
```

### WaitSet readiness

只保存通知还不够。

如果 Goal已经被：

```text
status_snapshot()
```

提前 prune，则 `earliest_expiry()`已经看不到它。

因此 ActionServer logical readiness定义为：

```text
expired_notifications not empty
        OR
now >= earliest_expiry
```

有未消费 notification：

```text
kActionGoalExpiredBit
```

必须保持 ready。

---

## 1.6 Phase 0：Action deadline变化必须主动唤醒 WaitSet

当前 WaitSet进入 native wait前才计算：

```text
earliest_expiry()
```

如果另一个线程之后把 Goal切换到 terminal：

```text
update_goal_state()
        ↓
new earlier expiry deadline
```

正在 sleep的 WaitSet必须重新计算 deadline。

禁止依赖：

```text
下一条 DDS message
周期 timeout
polling
```

触发重新计算。

DMW内部增加 Action logical wait notification。

概念结构：

```text
ActionGoalRegistry
        │
        ├── goal state revision
        ├── expiry revision
        └── wait notification
```

以下事件必须 notify：

```text
terminal transition creates expiry deadline

expired notification queue becomes non-empty

earliest expiry changes earlier

ActionServer destruction/closing
```

ActionServer WaitSet registration订阅该 notification。

notification发生时：

```text
wake native WaitSet
        ↓
recompute logical readiness
        ↓
recompute runtime deadline
```

不得引入固定轮询周期。

### Action sub-channel背压

DMW WaitSet增加：

```cpp
Result<void> set_interest(
    WaitableRegistration registration, std::uint32_t detail_mask);
```

V1用于 ActionServer aggregate registration；允许的位为 Goal / Cancel / Result request
及 kActionGoalExpiredBit。未知 bit返回 InvalidArgument，失效 token返回 NotRegistered，
其他 WaitableKind返回 Unsupported。默认 interest为该 aggregate的全部有效位。

set_interest()在 WaitSet topology协调下同步更新 logical readiness和 native reader conditions：
suppressed reader从 native WaitSet detach，恢复时重新 attach；
不能仅过滤返回 mask而让 suppressed DDS condition持续触发 native wake。
不感兴趣的 logical expiry不参与本轮 readiness/deadline计算。
更新递增 topology/wake generation，进行中的 wait重新取一致 snapshot。

DCLPY request capacity背压按 sub-channel设置 interest：
GetResult容量不足只屏蔽 Result request，保留仍有 ticket的 Goal/Cancel和 expiry。
不能整体 unregister ActionServer而使 control reserve无法服务 Cancel。
ProtocolLane BUSY仍按§6.6整体暂停 request事务；
capacity恢复后 owner重新计算 interest，dispatch还须检查最新 admission/lane状态，
不能因为旧 ready batch含有某位就继续 take已暂停的 request。

---

## 1.7 Phase 0：Service request显式放弃

DMW `Server`增加：

```cpp
Result<void> discard_request(const RequestId& request_id);
```

语义：

```text
Pending
    -> remove
    -> restore request capacity
    -> success

Responding
    -> Busy

unknown
    -> NotFound
```

它：

```text
不发送 response
不构造错误 response
只释放本地 pending request state
```

这是 language-neutral primitive。

用于：

```text
Python callback exception
Task cancellation
invalid response
entity closing
protocol delivery permanently failed
```

---

## 1.8 Phase 0：Action constituent request放弃

DMW ActionServer增加：

```cpp
Result<void> discard_goal_request(const RequestId& request_id);
Result<void> discard_cancel_request(const RequestId& request_id);
Result<void> discard_result_request(const RequestId& request_id);
```

Goal / Cancel分别委托内部 goal_server / cancel_server 的 discard_request()，
使用§1.7的 Pending / Responding / unknown返回语义。

Result request存在两份关联：

```text
result_server pending state
ActionGoalRegistry pending_result_requests
```

因此 discard_result_request()必须作为一个完整的 request事务：

1. 在 Action result-request协调锁下检查 constituent Server状态；
2. Pending：删除 Server pending state，同时删除 registry中的 RequestId关联，恢复容量；
3. Responding：返回 Busy，不改变任何关联；
4. unknown：清理可能残留的 registry关联，再返回 NotFound。

register_result_request()、take_pending_result_requests()、write_result_response()的
request claim，以及 discard_result_request()必须使用相同的协调顺序。
禁止出现“Server记录已删除，另一线程又把同一 RequestId挂回 registry”的窗口。

锁顺序固定为：

```text
Action result-request coordination
    -> constituent Server pending lock
    -> GoalRegistry lock
```

Phase 0必须把 constituent Server内部 response claim与 write阶段拆开；
Action wrapper只在 claim阶段持 coordination lock，不增加 public raw-write bypass API。
不得持有 coordination / pending / registry lock执行 DDS write或等待 reader discovery。
write_result_response()在协调区内把 request claim为 Responding并从 registry解除关联，
然后释放协调锁执行 native write；失败后恢复 Server Pending，由同一个 delivery owner重试，
不重新挂回 Goal pending列表。

take_pending_result_requests()只转移仍待 Goal terminal处理的关联。
已经转移给 ResultDelivery的请求由 delivery owner负责结束。
一次 discard成功后，该 ID不得再由 registry返回；重复调用返回 NotFound，
不会重复释放 capacity。DCLPY cleanup遇到 NotFound表示 request已结束，
可幂等释放对应 WorkLease；Busy必须等待正在执行的 write completion，不能提前释放。

Phase 0同时修复 accept_goal()的异常安全：

```text
reserve GoalId
    -> arm noexcept reservation rollback guard
    -> write accepted response
    -> commit GoalRecord
    -> disarm guard
```

错误返回或 commit前的 C++ exception均必须回滚 reservation。
不得只在 Result失败分支回滚。successful response write之后的 commit必须使用
预分配记录完成，不再分配。commit invariant损坏返回新增的 ErrorCode::ProtocolFault，
属于不可回滚的 fatal protocol fault，不能作为普通未接受 Goal重试。
Python映射 ProtocolFaultError并进入§6.11的 FAULTED处理。
native worker必须捕获所有 job exception并生成 completion，
具体规则见§4.17。

---

## 1.9 Phase 0：availability wait可中断

DMW增加：

```cpp
Result<void> Client::interrupt_waits();
Result<void> ActionClient::interrupt_waits();

Result<AvailabilityWaitToken> Client::prepare_availability_wait() const;
Result<AvailabilityWaitToken> ActionClient::prepare_availability_wait() const;

Result<bool> Client::wait_for_service(
    WaitTimeout timeout, const AvailabilityWaitToken& token) const;
Result<bool> ActionClient::wait_for_server(
    WaitTimeout timeout, const AvailabilityWaitToken& token) const;
```

并新增 ErrorCode::Interrupted。

AvailabilityWaitToken是 language-neutral opaque value，包含所属 wait state identity
和捕获时的 interruption generation。prepare_availability_wait()不阻塞、不发起 wait；
token必须在 native resource存活期间使用。其他 entity的 token返回 InvalidArgument。

interrupt_waits()在 wait-state mutex下递增 generation并 notify_all。
带 token的 wait必须在同一 mutex下检查 generation，随后才能检查 availability或进入
condition-variable wait；每次醒来重新检查。generation不匹配立即返回 Interrupted，
包括 token捕获之后、native wait进入之前已经发生 interruption的情况。

原有不带 token的 wait API保留，以 native入口捕获 generation；
它只能中断该入口之后的 wait。DCLPY availability binding必须使用带 token的 overload。

DCLPY admission固定为：

```text
lock Context admission, then EntityBacking
    -> verify Context accepts work and entity == OPEN
    -> create OperationLease
    -> prepare_availability_wait()
    -> unlock
    -> release GIL
    -> native wait(timeout, token)
```

entity close在同一个 EntityBacking mutex下标记 CLOSING。
Context shutdown先在 Context admission mutex下停止 admission，再遍历 existing entities
执行 interruption。interrupt调用在稳定 native access保护下执行，不依赖新 user lease。

因此 close只能发生在 token捕获之前或之后：

```text
close first -> admission rejected

token first -> close increments generation
           -> native wait sees mismatch, even if it has not started
```

仅在 release GIL之后检查 CLOSING不能替代上述原子 admission。
DMW wait path不得反向获取 DCLPY admission/backing mutex。

interruption是 generation变化，不是永久取消：
未来正常新调用取得新的 token，不受过去 interrupt影响。
DCLPY在 CLOSING之后拒绝新调用，所以不会重新取得可无限等待的 token。

entity close映射 EntityClosedError；Context shutdown触发的 interruption映射
ContextShutdownError。V1不暴露 Publisher.wait_for_all_acked()；
任何新增无限阻塞 API必须定义同等的入场与 interruption契约。

---

# 2. Binding ABI、Message 数据模型与构建部署

## 2.1 工程目录

```text
dclpy/
├── CMakeLists.txt
├── pyproject.toml
├── cmake/
│
├── dclpy/
│   ├── __init__.py
│   ├── context.py
│   ├── node.py
│   ├── publisher.py
│   ├── subscription.py
│   ├── client.py
│   ├── service.py
│   ├── timer.py
│   ├── action.py
│   ├── future.py
│   ├── executor.py
│   ├── asyncio_executor.py
│   ├── graph.py
│   ├── parameter.py
│   ├── qos.py
│   ├── exceptions.py
│   ├── _io.py
│   └── _typing.py
│
├── include/
│   └── dclpy/
│       └── interface_binding.h
│
├── src/_dclpy/
│   ├── module.cpp
│   ├── context.cpp
│   ├── node.cpp
│   ├── entity.cpp
│   ├── publisher.cpp
│   ├── subscription.cpp
│   ├── client.cpp
│   ├── service.cpp
│   ├── timer.cpp
│   ├── action.cpp
│   ├── wait_set.cpp
│   ├── graph.cpp
│   ├── parameter.cpp
│   ├── qos.cpp
│   ├── type_support.cpp
│   ├── error.cpp
│   └── build_info.cpp
│
├── tools/
│   └── interface_generator/
│
└── tests/
    ├── unit/
    ├── integration/
    └── ros2/
```

`dclpy/`是可直接导入的 Python package；`src/_dclpy/`只保存 `_dclpy`
扩展的 C++ 实现。`include/dclpy/`保存供生成接口扩展使用的稳定 ABI 头，三者
均不再使用 `binding/` 作为目录层级。

对应的 C++ namespace 固定为：`src/_dclpy/`中的实现使用
`dclpy::detail`；`include/dclpy/`根层的 ABI 支持使用 `dclpy`；生成接口扩展
使用 `dclpy::provider`。不保留 `dclpy::binding` 兼容别名。

---

## 2.2 DMW shared runtime

Core extension和所有 generated interface extension必须链接同一个：

```text
libdmw.so
```

固定：

```text
                  libdmw.so
                  ▲       ▲
                 /         \
             _dclpy    interface extension
```

禁止：

```text
_dclpy
    -> static DMW copy A

interface extension
    -> static DMW copy B
```

Phase 0必须完成：

```text
shared build
install
dmwConfig.cmake
SOVERSION
consumer test
```

---

## 2.3 Python版本

同一源码分别构建：

```text
cp310
cp311
cp312
cp313
```

不使用：

```text
abi3
```

当前第一 release gate：

```text
Ubuntu 22.04
ROS 2 Humble
Python 3.12
```

---

## 2.4 ROS interface package

例如：

```text
mfr3duo_msgs
```

生成：

```text
mfr3duo_msgs_dclpy
```

导入：

```python
from mfr3duo_msgs_dclpy.action import Move
from mfr3duo_msgs_dclpy.action import Grasp
```

标准 package同样：

```text
geometry_msgs
    ->
geometry_msgs_dclpy
```

对 mfr3duo 集成，顶层 CMake 提供可重复的 provider 构建入口：

```bash
cmake -S dclpy -B build-dclpy-mfr3duo \
  -DDCLPY_BUILD_MFR3DUO_INTERFACES=ON \
  -DDCLPY_MFR3DUO_INTERFACE_PREFIX=<mfr3duo_msgs-install-prefix> \
  -DDCLPY_ROS_INTERFACE_PREFIX=<ros-humble-prefix> \
  -DDCLPY_INCLUDE_DIR=<dclpy-source>/include
cmake --build build-dclpy-mfr3duo
cmake --install build-dclpy-mfr3duo
```

该目标从已安装的 ROSIDL 生成并安装 `mfr3duo_msgs_dclpy` 与其 Action
依赖，以及 `nav_msgs_dclpy` 与其消息依赖；因此冻结的 Odometry state、Move
和 Grasp E2E 不依赖临时生成目录。

---

## 2.5 Python message object

Python message直接包装 generated C++ ROS message。

例如：

```text
Python JointState
       │
       ▼
sensor_msgs::msg::JointState
```

不是：

```text
pure Python structure
    ↓
每次 publish 全量转换
```

---

## 2.6 MessageBindingV1

Core和所有 interface extension共享 `include/dclpy/interface_binding.h`。
该头定义完整 V1 ABI，不允许各 extension自行复制或扩展结构。

### ABI header与错误返回

```cpp
struct DclpyBindingHeaderV1 {
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    const char* compatibility_id;
};

enum class DclpyBindingErrorCodeV1 : std::uint32_t {
    None = 0,
    InvalidArgument = 1,
    TypeMismatch = 2,
    ResourceExhausted = 3,
    Internal = 4
};

struct DclpyBindingErrorV1 {
    DclpyBindingErrorCodeV1 code;
    char message[256];
};
```

DCLPY_INTERFACE_BINDING_ABI = 1。compatibility_id冻结构建兼容信息：
CPython minor / ABI、pybind11 internals ABI、C++标准库 ABI、
ROS interface profile以及 DMW ABI/SOVERSION。
不要求 compiler patch字符串完全相同，但不得把不兼容的标准库或 pybind11 ABI混用。

error由 caller分配并初始化为 None；adapter写入固定容量、NUL终止的 UTF-8 diagnostic，
允许截断，不转移内存，不包含 PyObject，不调用 Python C API。
所有非 Python返回值的 fallible adapter通过该 error报告错误；异常不得跨 ABI。

### Message binding

每个 message class携带版本化 PyCapsule：

```text
attribute: __dclpy_message_binding__
capsule name: dclpy.MessageBindingV1
```

```cpp
struct DclpyMessageBindingV1 {
    DclpyBindingHeaderV1 header;
    const char* python_qualified_name;
    const dmw::MessageType* (*message_type)() noexcept;
    bool (*is_instance)(PyObject*) noexcept;
    void* (*sample_ptr)(PyObject*) noexcept;
    PyObject* (*create_instance)() noexcept;
    void* (*clone_sample)(
        const void* source, DclpyBindingErrorV1* error) noexcept;
    void (*destroy_sample)(void* sample) noexcept;
};
```

message_type()返回 import阶段已经成功初始化的 immutable descriptor。
is_instance() / sample_ptr()必须持 GIL调用；type不匹配时分别返回 false / nullptr。
create_instance()必须持 GIL调用，成功返回 new reference，失败返回 nullptr并设置 Python error。

clone_sample()执行 new SampleT(*source)或等价 copy：
成功返回独立 sample并保持 error == None；失败返回 nullptr并设置 error。
它和 destroy_sample()不访问 Python状态，允许无 GIL调用；
调用者必须保证 source不会同时被修改。destroy_sample(nullptr)无操作，且必须 noexcept。

adapter捕获 bad_alloc并映射 ResourceExhausted，其余异常映射相应错误。
create_instance()内部捕获异常并设置 Python exception；不得让异常穿过 noexcept入口。

### Service binding

每个 service class公开 Request / Response，并携带：

```text
attribute: __dclpy_service_binding__
capsule name: dclpy.ServiceBindingV1
```

```cpp
struct DclpyServiceBindingV1 {
    DclpyBindingHeaderV1 header;
    const char* python_qualified_name;
    const dmw::ServiceType* (*service_type)() noexcept;
    const DclpyMessageBindingV1* request;
    const DclpyMessageBindingV1* response;
};
```

descriptor的 request/response类型必须与 Request / Response class的 message capsule一致。
Core通过这些元数据构造 Service，不根据 Python class名字猜测 C++类型。

### Action binding与 envelope元数据

每个 action class公开 Goal / Result / Feedback，并携带：

```text
attribute: __dclpy_action_binding__
capsule name: dclpy.ActionBindingV1
```

```cpp
struct DclpyActionBindingV1 {
    DclpyBindingHeaderV1 header;
    const char* python_qualified_name;
    const dmw::ActionType* (*action_type)() noexcept;
    const DclpyMessageBindingV1* goal;
    const DclpyMessageBindingV1* result;
    const DclpyMessageBindingV1* feedback;
    const DclpyServiceBindingV1* send_goal;
    const DclpyServiceBindingV1* cancel_goal;
    const DclpyServiceBindingV1* get_result;
    const DclpyMessageBindingV1* feedback_message;
    const DclpyMessageBindingV1* status_message;
};
```

generated Action.Impl同时提供与这些 descriptor逐一一致的 class：

```text
SendGoalService
CancelGoalService
GetResultService
FeedbackMessage
GoalStatusMessage
```

V1 envelope字段固定为 ROS Action结构：

| Type | 字段 |
| --- | --- |
| SendGoal.Request | goal_id、goal |
| SendGoal.Response | accepted、stamp |
| CancelGoal.Request | goal_info.goal_id、goal_info.stamp |
| CancelGoal.Response | return_code、goals_canceling |
| GetResult.Request | goal_id |
| GetResult.Response | status、result |
| FeedbackMessage | goal_id、feedback |
| GoalStatusMessage | status_list，其中 item为 goal_info、status |

Python action层在持 GIL时通过 generated constructor/getter/setter构造和读取这些 envelope，
再按§2.8冻结为 OwnedSample。Core不得根据 layout进行 reinterpret_cast或猜测字段偏移；
worker只使用 OwnedSample与 DMW descriptor，不访问 Python envelope。

CancelGoalService、GoalStatusMessage及 UUID/Time/GoalInfo等标准 nested type引用所属
interface package的唯一 binding；不得在每个 action extension重复注册它们。

### Capsule validation与存活期

Core在接受 type之前验证 capsule name、header版本、最小 struct_size、
compatibility_id、必要函数指针，以及 Service/Action constituent descriptor与 class一致性。
不兼容时在 import/type注册阶段报 ImportError，不能留到 native write后才发现。

binding table、descriptor及 qualified name必须在 provider extension存活期间地址稳定。
Context的 binding registry在持 GIL时保留 provider和依赖 extension的 strong reference；
OwnedSample只持 C++ binding handle，不持 PyObject。
registry必须在所有 OwnedSample、jobs和 completion清理完成后，才在持 GIL时释放 provider。

---

## 2.7 OwnedSample

DCLPY core定义：

```text
OwnedSample
```

内部：

```text
MessageBindingV1*
void* sample
```

析构：

```text
binding->destroy_sample(sample)
```

它：

```text
不包含 PyObject
worker线程可以持有
销毁不要求 GIL
```

---

## 2.8 所有 outbound message采用调用时快照

这是 V1 的统一发送规则。

任何可能：

```text
释放 GIL
跨线程排队
晚于调用时实际 serialization
```

的 outbound operation，都不得直接持有用户 Python message backing。

固定：

```text
Python call
    ↓
GIL held
    ↓
validate message
    ↓
clone_sample()
    ↓
OwnedSample
    ↓
release GIL / enqueue
```

因此：

```python
future = publisher.publish_async(msg)
msg.position = [...]
```

worker发送的是：

> `publish_async()` 被调用时的 message 值。

而不是 worker真正执行时的新值。

同步：

```python
publisher.publish(msg)
```

同样：

```text
snapshot while GIL held
        ↓
release GIL
        ↓
DMW write(snapshot)
```

因此另一个 Python线程随后修改 `msg`不会与 Fast DDS serialization产生 C++ data race。

---

## 2.9 必须 snapshot 的对象

统一包括：

```text
Topic message

Service request
Service response

Action goal request
Action cancel request
Action result request

Goal response
Cancel response
GetResult response

Feedback message
Status message
```

用户提供或 Python层构造的任何发送对象：

```text
进入 native blocking/write path前必须拥有独立 C++ snapshot。
```

---

## 2.10 Receive object

接收：

```text
create_instance()
        ↓
obtain sample_ptr()
        ↓
DMW read()
        ↓
expose Python object
```

read期间该对象尚未暴露给用户，因此不需要 message-level lock。

V1 read path保持 GIL。

---

## 2.11 字段语义

### Scalar

直接映射 backing field。

### Nested message

getter返回：

```text
live child view
```

例如：

```python
msg.header.stamp.sec = 10
```

直接修改 parent C++ object。

child wrapper必须持有：

```text
parent strong reference
```

### Array / sequence

getter返回 copy：

```python
x = msg.position
x.append(1.0)
```

不改变 backing message。

修改：

```python
msg.position = [1.0, 2.0]
```

### Sequence of nested message

getter同样返回 detached Python list。

修改通过整体重新赋值。

---

## 2.12 字段赋值失败原子性

所有复杂 setter，包括 string、array、sequence和 nested message，固定执行：

```text
validate
    -> construct complete staged C++ value
    -> commit with noexcept swap or proven noexcept move assignment
```

最后的 commit必须具有编译期 noexcept证明，例如 is_nothrow_swappable或
is_nothrow_move_assignable；只验证 staged value成功构造不够。

禁止以可能分配或部分修改目标的 copy assignment作为 commit。
无法提供 noexcept commit的 generated field必须在生成/构建阶段失败，
不得降级为可能破坏原 field的 setter。

type mismatch、integer overflow、allocation failure、nested conversion failure和
bound violation均发生在 commit之前，原 field保持不变。
commit必须修改原 backing field对象，保留§2.11 live child view对 inline nested object的有效性；
不得销毁 parent并以另一块 backing替换它。

---

## 2.13 整数范围

generated binding必须验证：

```text
int8
uint8
int16
uint16
int32
uint32
int64
uint64
```

对应 C++范围。

禁止让 Python arbitrary precision integer静默截断。

---

## 2.14 Cross-package nested types

例如：

```text
mfr3duo_msgs
    -> geometry_msgs/Pose
```

则：

```text
mfr3duo_msgs_dclpy
    -> dependency geometry_msgs_dclpy
```

同一个 generated C++ type只能由其所属 package绑定一次。

禁止：

```text
两个 extension分别注册 geometry_msgs::msg::Pose
```

---

## 2.15 BuildInfo

公开：

```python
dclpy.build_info()
```

至少返回：

```python
{
    "dclpy_version": "...",
    "dclpy_interface_abi": 1,

    "dmw_version": "...",
    "dmw_soversion": "...",

    "python_version": "...",
    "python_abi": "cp312",

    "compiler": "...",
    "cxx_standard": "17",

    "fastdds_version": "...",
    "fastcdr_version": "...",

    "ros_profile": "humble",
    "build_type": "Release",
}
```

仅用于 diagnostics。

---

# 3. Entity 生命周期、Work 保留与 Future 状态机

## 3.1 三类资源保护

统一使用：

```text
OperationLease
DispatchLease
WorkLease
```

三者解决不同问题。

### OperationLease

保护：

```text
正在执行的用户/native API operation
```

例如：

```text
wait_for_service()
publish()
service_is_ready()
```

### DispatchLease

保护：

```text
WaitResult已经返回
但对应 dispatch尚未结束
```

### WorkLease

保护：

```text
asyncio Task
NativeIoDispatcher job
Service request task
Action protocol transaction
Action result delivery
queued send operation
```

即：

> entity已经离开当前 Python call stack，但仍有已接受的异步工作依赖它。

---

## 3.2 EntityBacking

所有 DMW entity wrapper统一使用：

```text
EntityBacking<T>
```

核心状态：

```cpp
enum class State {
    Open,
    Closing,
    Closed
};
```

内部至少包含：

```text
mutex
condition_variable

state

unique_ptr<T> resource

active_operations
dispatch_refs
work_refs

executor_registered

destroy_started
```

---

## 3.3 OperationLease

仅：

```text
OPEN
```

允许创建新的 user OperationLease，且所属 Context仍接受 user work。

锁顺序为 Context admission mutex -> EntityBacking mutex；
availability operation在同一 admission内捕获§1.9 token。

流程：

```text
lock
    ↓
Context admission open and state == OPEN ?
    ↓
active_operations++
    ↓
obtain stable T*
```

结束：

```text
active_operations--
        ↓
try_finalize()
```

CLOSING之后：

```text
拒绝新的用户 operation
```

---

## 3.4 WorkLease

WorkLease在**工作被接受时**创建。

例如：

```text
Service request被take
        ↓
create WorkLease
        ↓
start coroutine
        ↓
response/discard complete
        ↓
release WorkLease
```

或：

```text
publish_async()
        ↓
snapshot
        ↓
create WorkLease
        ↓
queue job
        ↓
job completion
        ↓
release
```

### CLOSING后的规则

CLOSING后：

```text
禁止创建新的用户 WorkLease
```

但是：

> CLOSING前已经创建的 WorkLease可以继续使用 resource 完成自身的 protocol cleanup。

因此内部 cleanup**不重新调用 OperationLease acquire**。

WorkLease自己持有合法 backing access。cleanup可以转移或派生已有 WorkLease，
不得以 cleanup名义接受新的用户工作。待入队、重试、completion待消费均属于未结束的 work。

---

## 3.5 WorkLease transfer

以下流程不能在中间出现：

```text
work_refs == 0
```

例如 Service：

```text
callback task
    ↓
response I/O job
```

必须：

```text
transfer WorkLease
```

而不是：

```text
release task lease
    ↓
重新 acquire response lease
```

否则 close可能在中间销毁 Server。

---

## 3.6 最终销毁条件

只有同时满足：

```text
state == CLOSING

active_operations == 0

dispatch_refs == 0

work_refs == 0

executor_registered == false

destroy_started == false
```

才开始真正析构。

流程：

```text
lock
    ↓
destroy_started = true
move resource -> local
unlock
    ↓
~T()
    ↓
lock
state = CLOSED
notify_all()
unlock
```

注意：

> `CLOSED`只能在 DMW destructor 已实际完成后发布。

不能：

```text
state = CLOSED
notify
    ↓
之后才析构 native object
```

因此：

```python
wait_closed()
```

返回 true确实表示物理 teardown已经完成。

---

## 3.7 close()

```python
entity.close()
```

固定：

```text
OPEN -> CLOSING
```

然后：

1. 拒绝新 OperationLease；
2. 拒绝新 user WorkLease；
3. interrupt native blocking waits；
4. 通知 owner Executor停止该 entity admission并移除 DDS registration；
5. cancel未开始 user jobs，settle未完成 Future；
6. owner control path取消该 entity的 managed Tasks并执行已接受 protocol cleanup；
7. 保留 EntityRecord、mailbox路由和 leases，直到 cleanup完成；
8. `try_finalize()`。

`close()`：

```text
non-blocking with respect to existing work
```

它保证：

```text
返回后用户不能启动新操作
```

不保证资源已经物理销毁。

需要等待：

```python
entity.wait_closed(timeout=None)
```

---

## 3.8 Future状态

```text
PENDING
   ├── result/exception -> FINISHED
   └── cancel           -> CANCELED
```

所有状态转换在同一个 lock下完成。

只有：

```text
PENDING -> terminal
```

允许一次。

cancel与completion：

```text
谁先 commit terminal state
谁胜出
```

---

## 3.9 Future callback

完成：

```text
lock
change state
move callbacks
notify condition
unlock
    ↓
invoke/schedule callbacks
```

callback永远不在 Future lock下执行。

---

## 3.10 Future执行线程

如果 Future关联 Executor：

```text
done callback
```

由该 Executor调度。

SingleThreadedExecutor：

```text
spin thread
```

AsyncIOExecutor：

```text
event-loop thread
```

如果 Future没有 Executor关联，并由用户线程直接：

```python
future.cancel()
```

则 cancellation callback允许在调用线程执行。

---

## 3.11 await

每个 awaiter拥有独立 bridge。

```text
DCLPY Future
   │
   ├── asyncio waiter A
   ├── asyncio waiter B
   └── callback
```

取消 waiter A：

```text
只取消 A
```

不会自动：

```text
future.cancel()
```

---

## 3.12 loop关闭

`loop.call_soon_threadsafe()`抛出：

```text
RuntimeError
```

时：

```text
detach that bridge
do not mutate underlying Future
do not leak exception
```

---

## 3.13 OutboundOperation

任何存在：

```text
queue
worker write
RequestId后来才产生
```

的异步请求都建立：

```text
OutboundOperation
```

状态：

```text
QUEUED
   ↓
SENDING
   ↓
SEND_COMMITTED
   ↓
REGISTERED
   ↓
COMPLETED
```

terminal：

```text
CANCELED
FAILED
CLOSED
SHUTDOWN
EXECUTOR_STOPPED
```

它在 RequestId出现前就存在。

---

## 3.14 Future关闭保证

硬性规则：

> 所有已经返回给用户的 Future最终必须进入 terminal state。

不能因为：

```text
Client close
Executor shutdown
Context shutdown
queued job cancellation
write正在进行
RequestId尚未产生
```

永久停留 PENDING。

### User cancel

```text
Future -> CANCELED
```

### Client close

未完成 Future：

```text
EntityClosedError
```

### Executor stop

依赖该 Executor完成的 Future：

```text
ExecutorStoppedError
```

### Context shutdown

```text
ContextShutdownError
```

Context在请求各 Executor停止之前，先原子记录 ContextShutdown cause并传给各 owner。
各 owner control path按该 cause对 pending Future提交 ContextShutdownError；
调用 request_shutdown的外部线程不遍历/修改 owner Python registry。
Executor停止不能把本次 Context shutdown替换成 ExecutorStoppedError。独立 Executor stop使用 ExecutorStoppedError；独立 entity close使用 EntityClosedError。
并发独立关闭与用户取消仍遵守§3.8：首次 terminal commit胜出。

---

## 3.15 cancel发生在各发送阶段

### QUEUED

尚未 worker执行：

```text
remove/cancel queue job
Future -> CANCELED
release WorkLease
```

### SENDING

native write无法强制中断：

```text
mark cancel_requested
Future -> CANCELED
```

worker完成后：

如果 write失败：

```text
discard completion
release WorkLease
```

如果产生 RequestId：

```text
直接注册 tombstone
不注册 active Future
```

### REGISTERED

```text
remove pending Future
insert RequestId tombstone
Future -> CANCELED
```

---

## 3.16 Request tombstone

默认：

```text
max entries = 4096
TTL = 60 seconds
```

late response：

```text
matching tombstone
    -> drop
```

不是 unknown protocol error。

---

# 4. Executor、worker交接与 asyncio

## 4.1 SingleThreadedExecutor

API：

```python
executor = SingleThreadedExecutor(context)

executor.add_node(node)
executor.remove_node(node)

executor.spin()
executor.spin_once(timeout_sec=None)

executor.wake()

executor.request_shutdown()
executor.shutdown(timeout=None)
```

状态：

```text
CREATED
RUNNING
STOPPING
STOPPED
```

STOPPED不可重新启动。

---

## 4.2 Node一次只能属于一个 Executor

重复 attach返回 AlreadyRegisteredError。

Node及其 accepted work归属固定的 (ExecutorId, attachment_generation)。
异步 I/O admission必须有 owner Executor，且该 Executor未 STOPPING；
CREATED状态可接受 admission，但 completion只能在 start/spin或其 shutdown control path处理。

remove_node()必须在停止该 Node admission后检查 quiescence：

```text
no outstanding ready batch / DispatchLease
no accepted Tasks / WorkLeases / native jobs
no unconsumed completion or Future notifications
```

未满足时返回 BusyError并保留原 attachment；不得让新 Executor接管旧 work。
成功 remove，或原 Executor完成 STOPPED且 Node work已全部结束之后，才能重新 attach。
重新 attach递增 generation；旧 generation的 completion不能应用于新 registry。
standalone Executor stop完成其 protocol cleanup，不要求销毁仍存活的 Node。

---

## 4.3 Registration map

WaitSet registration显式映射到 EntityRecord：

```text
WaitableRegistration -> EntityRecord
```

EntityRecord至少保存 entity strong reference、EntityBacking、WaitableKind、
registration以及 owner ExecutorId / attachment_generation。

DDS registration与 accepted work记录分离。
close、backpressure或 ProtocolLane暂停可以移除 DDS registration，
但 EntityRecord必须保留到 outstanding ready batch、Tasks、jobs、
completion和已登记的 Future通知全部结束。

ready-batch归属检查和 DispatchLease获取必须与 unregister/finalize原子协调：
已经 CLOSED或 destroy_started的 backing不得再取得 DispatchLease。
关闭后只允许利用既有 accepted work lease执行 cleanup。

---

## 4.4 DispatchLease

处理一个 ReadyWaitable之前：

```text
dispatch_refs++
```

dispatch结束：

```text
dispatch_refs--
```

如果 entity已经 CLOSING：

```text
不再调用 user callback
```

但可以执行必要 native cleanup。

---

## 4.5 bounded dispatch

一轮最多：

```text
Subscription -> 1 sample
Client       -> 1 response
Service      -> 1 request
Timer        -> 1 consume
GraphEvent   -> 1 take
```

Action：

```text
每个 ready sub-channel最多1 logical item
```

不无限 drain。

---

## 4.6 AsyncIOExecutor API

```python
executor = AsyncIOExecutor(
    context,
    loop=None,
    max_callback_tasks=64,
    max_service_tasks_per_service=8,
    max_execute_tasks_per_server=8,
)

executor.add_node(node)

executor.start()

executor.request_shutdown(cancel_tasks=True)

await executor.shutdown_async(
    cancel_tasks=True,
    timeout=None,
)
```

`loop=None`：

```text
start()时绑定当前 running loop
```

---

## 4.7 wait thread

```text
                  AsyncIOExecutor
                        │
             ┌──────────┴──────────┐
             │                     │
             ▼                     ▼
         wait thread           asyncio loop
             │                     ▲
       DMW WaitSet.wait()           │
             │                     │
             └── ready batch ──────┘
```

wait thread绝不执行 user callback。

---

## 4.8 Batch ACK

ACK只表示：

```text
read/take/consume已经完成
+
必要 Task/job已经登记
```

不表示：

```text
coroutine完成
Service response完成
Action execute完成
```

因此：

```python
async def callback(...):
    result = await client.call_async(...)
```

不会阻塞下一轮 DMW wait。

---

## 4.9 一个未ACK batch

任意时刻最多：

```text
1 ready batch waiting for ACK
```

但：

```text
Task completion
```

与 ACK解耦。

---

## 4.10 Task分类

不能用一个全局 semaphore控制所有 coroutine。

分为：

### General callback tasks

用于：

```text
Subscription coroutine
Timer coroutine
```

受：

```text
max_callback_tasks
```

限制。

### Service tasks

每个 Service独立：

```text
max_service_tasks_per_service
```

默认：

```text
8
```

不占 `max_callback_tasks`。

### Action execute tasks

每个 ActionServer独立：

```text
max_execute_tasks_per_server
```

默认：

```text
8
```

---

## 4.11 Service嵌套调用

因此即使：

```text
max_callback_tasks = 1
```

也不会阻止：

```text
Service A coroutine
    ↓ await
Client call to Service B
    ↓
Service B coroutine
```

因为 Service A/B使用各自 Service task capacity。

### 不保证的情况

DCLPY不承诺自动解决：

```text
Service A callback
    ↓
递归调用 Service A
    ↓
超过 Service A max_service_tasks_per_service
```

或形成应用级循环依赖：

```text
A waits B
B waits A
```

这些属于应用层死锁。

达到某个 Service自己的 task capacity时：

```text
temporarily unregister only that Service
```

不影响 Client response、其他 Service和其他 waitable progress。

---

## 4.12 NativeIoDispatcher

每个 Context lazily拥有一个：

```text
NativeIoDispatcher
```

V1：

```text
1 native worker thread
```

用于所有可能阻塞的：

```text
Fast DDS synchronous write
```

---

## 4.13 两类 I/O queue

```text
protocol queue
user queue
```

protocol优先。

### user

```text
Publisher.publish_async
Client call_async request
ActionClient goal/cancel/result request
```

### protocol

```text
Service response

Action Goal response
Cancel response
Result response

Feedback
Status
```

---

## 4.14 Queue容量

每个 Context默认：

```text
total I/O tickets = 256
reserved Goal/Cancel control-response tickets = 32
```

ticket覆盖已保留、queued、running、delayed retry及 completion尚未消费的整个 job生命周期。
不是只计算 ready queue长度。一个 retry复用原 ticket，不能再次占用容量。

user、Feedback/Status、Service和 GetResult work均不能消费这32个 control reserve；
它们合计最多占用224个 ticket。Goal/Cancel response可以使用总容量中的可用 ticket。
因此等待 Goal完成的 PendingResultWork或长时间 Service Task不能耗尽 Cancel所需容量。
protocol队列内先调度 critical response，再调度 Feedback/Status；
已进入 native write的 job不可抢占。

Service / Goal / Cancel / GetResult request在 native take之前必须尝试预留
critical-response ticket及预分配的 request work/job/completion storage。
ticket保留到 response成功或明确 discard完成。
Pending GetResult同样保留 ticket，不能在 terminal时才尝试争抢 response容量。

没有 ticket时不 take新 request：Service暂时 unregister，ActionServer按§1.6屏蔽
对应 sub-channel interest，保留有容量的 Goal/Cancel及 expiry。
不能占住 event loop等待容量。ticket释放通过 owner control wake恢复 registration/interest。
ActionServer暂停期间，existing work的 completion、cleanup和已有 Goal的 terminal处理继续运行。

user queue满返回 ResourceExhaustedError。
Feedback入队失败返回 ResourceExhaustedError，不改变 Goal FSM。
Status没有 ticket时只保留最新 dirty revision，容量释放后由 control path重新尝试；
每个 server最多保留一个待发送最新 snapshot，不无限堆积。
等待容量的 Status持有 accepted cleanup WorkLease并保留 owner wake路由；
shutdown必须发送或明确放弃这份 Status work，再释放 lease，不能遗留 dirty work阻止 barrier。

对于已取得 critical ticket的 request，入队只链接预分配 storage，不得再次因 queue满失败。
take之后发生 validation/Task构造等失败时，用同一个 work owner执行 discard，
再释放 ticket和 WorkLease。未取得 ticket不得先进入 PREPARED或创建 gated Task。

---

## 4.15 NativeIo job必须拥有 WorkLease

enqueue前：

```text
snapshot
    ↓
WorkLease
    ↓
queue
```

job直到：

```text
success
failure
cancel
cleanup
```

后才释放 WorkLease。

因此：

```text
entity.close()
```

不会在已排队 job之前析构 DMW resource。

---

## 4.16 Worker不访问 Python registry

worker禁止持有：

```text
Python Future map mutex
Python dict
PyObject request registry
```

worker只处理：

```text
C++ OwnedSample
C++ DMW resource via WorkLease
C++ CompletionRecord
```

不需要 GIL。

---

## 4.17 CompletionPort

每个 Context拥有 CompletionPort router，按 ExecutorId分配纯 C++ mailbox。
worker只把 completion放入 operation admission时捕获的 mailbox，并唤醒该 owner；
不得让任意 Executor drain整个 Context的 completion。

CompletionRecord至少包含：

```text
ExecutorId
attachment_generation
operation id
entity id
job kind
result/error
optional RequestId
```

所有 routing字段在 enqueue前确定；worker不查询 Python registry。
mailbox及对应 owner control route必须存活到全部 accepted work结束，
不能在 request_shutdown或 DDS unregister时销毁。

job ticket预留 completion storage。worker捕获所有 native job exception，
包括 bad_alloc、std::exception和未知异常，仍必须发布且仅发布一条 completion。
失败 diagnostic使用预分配的固定容量缓冲区；异常报告路径不得依赖另一次分配。
middleware Result失败按 DMW ErrorCode映射；bad_alloc映射 ResourceExhaustedError，
其他未分类 native exception映射 MiddlewareError。
ProtocolFault不能进入普通 delivery retry。

worker不直接完成 Python Future、不修改 Python registry、不执行 Python callback。

---

## 4.18 Executor处理 completion

Executor只 drain自己的 mailbox。RUNNING时在 WaitSet dispatch前后，以及 control wake时处理。

SingleThreadedExecutor由 spin thread处理；
AsyncIOExecutor由绑定的 event-loop thread处理。
处理前验证 ExecutorId / attachment_generation，绝不能把旧 completion应用到新 attachment。
unexpected stale record通过旧 work owner的 cleanup路径结束并报告 invariant错误。

STOPPING停止新的 DDS user callback和 user I/O admission，但 control path必须继续运行：

```text
consume existing ready batch and ACK without starting new user callbacks
process worker completions / SendCommit / retry / capacity wakes
settle Future and deliver already registered callbacks / await bridges
finish or cancel existing Tasks according to shutdown policy
drain/discard accepted protocol work
release tickets, WorkLeases and retained EntityRecords
```

STOPPING期间的 control cleanup不受 general callback task capacity限制。
SingleThreaded spin不能先退出再等待这些 work；从未启动的 Executor由 shutdown调用线程
承担 control owner，不能与 spin并发处理同一个 mailbox。
AsyncIO control path必须留在原 loop，不得由其他线程代替它修改 asyncio对象。
AsyncIO wait thread及其 control GuardCondition必须保持到所有 producer退休、mailbox已空
且最后 ready batch已ACK；随后才能结束 wait thread并 join。
不能在 STOPPING入口就停止负责 worker completion wake/ACK交接的 wait thread。

进入 STOPPED之前必须完成 barrier：

```text
no outstanding batch / ACK
no managed Tasks or callback notifications
no owned queued/running/retry jobs or protocol work
mailbox empty and all admitted producers retired
no remaining work / dispatch refs
```

检查 barrier与关闭 mailbox必须在 admission/router协调下进行，
不能在 mailbox暂时为空而 worker仍可能投递时提前退出。
Context shutdown的 owner barrier额外等待 attached child close/cache cleanup及 physical teardown，
不得先退休 mailbox再请求依赖它的 entity cleanup。
standalone Executor shutdown只 drain该 Executor的 jobs，不能停止共享 Context的其他 Executor。

---

## 4.19 不允许 event loop等待 worker mutex

worker执行：

```text
DMW write
```

期间不得持有任何 event-loop会等待的 mutex。

因此取消原设计：

```text
pending_mutex
    held across write()
```

---

## 4.20 RequestId登记竞态

worker write成功后才得到 RequestId，因此 response可能先于 SendCommit被 owner消费。
Client event-loop state包含：

```text
send_operations[OperationId]
pending_requests[RequestId]
early_responses[RequestId]
tombstones[RequestId]
```

send operation在 enqueue之前登记。native send是否已经开始由纯 C++ operation state记录；
worker在 write前发布该状态，owner读取不等待 native write持有的锁。
Future是否 terminal与 native send是否已 reconcile是两个不同状态。

### SendCommit / send failure

owner消费 SendCommit时：

1. 找到原 operation，并从 staging移出该 RequestId对应的 early response；
2. 若 Future已 terminal，或 operation是 CANCELED / FAILED / CLOSED / SHUTDOWN /
   EXECUTOR_STOPPED：建立 tombstone、丢弃 early response，不登记 active Future；
3. 否则登记 pending_requests；若有 early response，立即完成并删除 pending记录，
   为已完成 ID建立 tombstone；
4. 对其他 staging entry移除本次已 reconcile的 candidate OperationId；
5. entry不再有 candidate时丢弃它；
6. 结束 send work，释放相应 ticket/lease或转移给仍待 response的 operation owner。

send failure同样 retire candidate并清理 staging，不得遗漏 terminal分支。
Future先 terminal而 native write仍在执行时，保留纯 C++ send owner直至 completion；
不得提前销毁 resource或抛弃 mailbox路由。

### response先到

read response后按顺序：

```text
matching tombstone -> drop
matching pending request -> complete once, remove pending, tombstone
otherwise -> examine unreconciled sends
```

只有存在“native send已开始、但 SendCommit尚未 reconcile”的 operation时，
unknown response才允许 staging。entry保存接收时可能匹配的 candidate OperationId集合；
以后新发送的 operation不能加入这个集合。

没有 candidate的 unknown response直接丢弃并记录 diagnostic。
因此 tombstone已过 TTL的迟到响应不会永久积累。
cancel/close/shutdown移除 active映射时，必须同时清理已知关联的 staging entry。

---

## 4.21 early response容量

max_early_responses = 4096。

entry生命周期由§4.20的 candidate retirement驱动，不因持续新增发送而续期；
相同 RequestId最多保留一份 response。
所有候选 send已 reconcile而仍无匹配的 entry必须立即删除。

容量耗尽属于 ResourceExhausted，不自动推断为 internal progress invariant损坏。
此时 fail outstanding operations with ResourceExhaustedError并关闭 Client，
清理 staging；仍在执行的 native send保留 owner直到 completion结束。
shutdown control path必须同样执行 candidate retirement。

tombstone容量/TTL只影响去重窗口，不能成为 orphan early response无限存活的原因。

---

## 4.22 Worker completion必须主动 wake

所有：

```text
success
failure
retry due
queue capacity release
```

都通过 CompletionPort / control GuardCondition唤醒 Executor。

禁止依赖：

```text
下一条 DDS消息
```

来处理 completion。

---

## 4.23 Retry不依赖 Executor流量

NativeIoDispatcher支持：

```text
not_before
```

的 delayed protocol job。

worker自己的：

```text
condition_variable + steady deadline
```

负责等待 retry时间。

因此：

```text
DDS没有任何新流量
```

也能执行：

```text
Status retry
Result response retry
Service response retry
```

不是固定 polling。

---

## 4.24 Shutdown thread边界

request_shutdown()始终 non-blocking、thread-safe、callback-safe。

同步 shutdown()是 blocking API。在 SingleThreadedExecutor spin thread，
或任意绑定该 Context的 AsyncIOExecutor event-loop thread调用时，
返回 InvalidStateError，不能因为 caller不是 managed callback就阻塞 loop。

shutdown_async()也必须在改变 shutdown状态之前检查 caller：
如果 asyncio.current_task()是本次待关闭 Executor管理的 Task，
或 Context下任一待关闭 Executor管理的 Task，返回 InvalidStateError。
它不能等待包含自身的 Task/lease barrier，cancel_tasks=True也不能规避此检查。

managed callback只能调用 request_shutdown()并结束自身工作。
独立于 managed work的 application Task可以 await shutdown_async()；
内部 shutdown coordinator不属于 user TaskRegistry。

---

## 4.25 Context async shutdown

公开：

```python
await context.shutdown_async(cancel_tasks=True, timeout=None)
```

caller必须在 running loop中，且通过§4.24的 managed Task检查。
Context保留唯一 internal shutdown coordinator；重复调用等待同一关闭过程。

流程：

```text
stop Context user admission and record shutdown cause
    -> interrupt availability waits
    -> request all Executors STOPPING with ContextShutdown cause
    -> each owner settles Future, runs control drain / Task cleanup
    -> close attached child entities while owner control routes remain alive
    -> wait every owner and entity teardown barrier
    -> drain Context-owned native dispatcher and join worker
    -> close remaining unattached/quiescent children and wait physical teardown
    -> DMW Context shutdown
    -> CLOSED
```

每个 AsyncIOExecutor的 drain coroutine必须运行在它自己的 loop。
同 loop可直接 await internal drain；其他 running loop使用
asyncio.run_coroutine_threadsafe()并在 caller loop通过 asyncio.wrap_future()等待，
不得直接 await另一个 loop的 Task/Future，也不得同步调用 concurrent Future.result()。

SingleThreadedExecutor继续由其 spin/control owner处理，coordinator通过非阻塞 bridge等待 barrier。
若某个 owner loop已 stopped/closed，按§4.28保留资源并报告 timeout/fault，
不得把它的 Python registry转交到当前 loop强行 drain。

timeout为本次 caller等待所有 owner及 Context关闭完成的共享 steady absolute deadline，
不能为每个 Executor重新获得完整 timeout。它不限制 internal drain的存活期；
到期只结束本次等待，coordinator及各 owner drain继续运行。

---

## 4.26 `cancel_tasks=False`

cancel_tasks=False表示不对已有 user Task调用 cancel()，也不创建新的 user callback Task。
STOPPING仍继续 control completion、protocol cleanup和 Future/bridge通知。

“自然完成”不承诺已有 RPC一定成功：shutdown会按其 cause结束 pending DCLPY Future，
Task可以正常接收该错误并运行 finally。
已有 Task不得在 shutdown期间启动新的 user I/O；
它对已接受 request的 response/discard则使用已有 WorkLease和 critical ticket完成。

accept尚未 commit的 gated Task按§6.12结束，不在 STOPPING中首次执行用户代码。
Task依赖永不结束的外部 awaitable，或不配合 cancellation时，shutdown均可能 timeout；
不能通过强制释放 backing来终止 Python Task。

cancel_tasks=True升级会取消尚未完成的 managed Tasks；
一旦升级，后续 cancel_tasks=False不能撤销取消。

---

## 4.27 shutdown timeout

timeout只限制 caller等待关闭完成的时间。到期返回 DclpyTimeoutError，
Executor保持 STOPPING，resources继续受 leases保护。

internal shutdown coordinator由 Context/Executor强引用并通过 shield保护；
caller timeout或 cancellation只 detach本次 waiter，不取消 coordinator或已经接受的 cleanup。
不得直接用会取消 drain Task的 wait_for来实现 timeout。

随后可以再次 await shutdown_async(cancel_tasks=True, timeout=...)：
加入同一关闭过程并升级 cancellation policy。
不得启动第二套 drain或重复销毁 mailbox/native resource。

---

## 4.28 loop已经 stopped/closed

正常契约：

> AsyncIOExecutor必须在其 event loop关闭前完成 `shutdown_async()`。

如果 loop已经停止但未关闭：

其他线程可以：

```python
executor.request_shutdown()
executor.shutdown(timeout)
```

但若仍存在需要 event loop运行的 Task：

```text
shutdown可能 timeout
```

资源继续保留。

如果 loop已经 closed且仍有 managed Task：

```text
进入 FAULTED shutdown state
```

禁止：

```text
在无法执行 Python cleanup 时强制销毁 backing
```

记录明确 diagnostic。

GC/process teardown仅作为最终 fallback。

---

# 5. Topic、Service、Timer、Clock、Graph 与 Parameter

## 5.1 Publisher

参考 rclpy：

```python
publisher = node.create_publisher(
    RobotState,
    "/robot_state",
    qos_profile_sensor_data,
)
```

同步：

```python
publisher.publish(msg)
```

语义：

```text
snapshot under GIL
        ↓
release GIL
        ↓
DMW write(snapshot)
```

调用线程可能等待 Fast DDS synchronous writer。

asyncio环境优先：

```python
await publisher.publish_async(msg)
```

语义：

```text
snapshot at call time
        ↓
enqueue NativeIoDispatcher
        ↓
await Future
```

---

## 5.2 Subscription

```python
subscription = node.create_subscription(
    RobotState,
    "/robot_state",
    callback,
    qos_profile_sensor_data,
)
```

read：

```text
create instance
        ↓
DMW read
        ↓
false -> stale readiness
true  -> callback
```

---

## 5.3 Client

```python
client = node.create_client(
    GetState,
    "/get_state",
)
```

调用：

```python
future = client.call_async(request)
```

固定：

```text
snapshot request immediately
        ↓
create OutboundOperation
        ↓
create WorkLease
        ↓
enqueue user I/O job
        ↓
return Future immediately
```

caller thread不执行 Fast DDS write。

---

## 5.4 `wait_for_service`

client.wait_for_service(timeout_sec=None)：

```text
None -> infinite
0    -> poll
>0   -> finite
<0   -> InvalidArgumentError
```

binding必须使用§1.9的原子 admission：

```text
OperationLease + AvailabilityWaitToken captured before close can commit
    -> release GIL
    -> DMW wait_for_service(timeout, token)
```

close调用 interrupt_waits()，既能中断已经阻塞的 wait，
也能中断已经 admission但尚未进入 native wait的调用。
ActionClient.wait_for_server()遵守相同契约。

entity close返回 EntityClosedError；
Context shutdown触发的 interruption返回 ContextShutdownError；
正常 timeout返回 False。并发独立关闭按首先提交的 closing cause解释。

---

## 5.5 Service

```python
service = node.create_service(
    GetState,
    "/get_state",
    callback,
)
```

SingleThreaded：

```text
callback必须 sync
```

AsyncIO：

```text
sync or coroutine
```

---

## 5.6 Service WorkLease

request成功 take后：

```text
create ServiceRequestWork
        ↓
owns WorkLease
RequestId
Request object
```

这个 WorkLease一直持续到：

```text
response成功
或
discard_request成功
```

因此 coroutine await期间：

```text
Server不能被析构
```

---

## 5.7 Service coroutine

```text
take request
        ↓
reserve per-Service task slot
        ↓
ServiceRequestWork
        ↓
start task
        ↓
ACK batch
```

Task结束：

### 正常 Response

```text
validate
        ↓
snapshot response
        ↓
transfer WorkLease to protocol job
        ↓
response delivery
```

### exception

```text
report callback exception
        ↓
discard_request(RequestId)
        ↓
release WorkLease
```

### cancellation

同样：

```text
discard_request
```

后才能释放 WorkLease。

---

## 5.8 Service response retry

Native delivery失败：

```text
attempt 1
attempt 2
attempt 3
```

使用同一个：

```text
RequestId
OwnedSample
WorkLease
```

三次永久失败：

```text
discard_request(RequestId)
        ↓
complete protocol work
release critical ticket and WorkLease
```

如果 discard返回非 NotFound、非可等待 Busy的错误：

```text
fault + close Service
```

---

## 5.9 Timer

```python
timer = node.create_timer(
    0.01,
    callback,
)
```

ready后：

```text
consume()
    ├── false -> ignore
    └── true  -> callback
```

Python不重新计算 timer deadline。

---

## 5.10 Clock

```python
clock = context.create_clock(ClockType.SYSTEM)
clock = context.create_clock(ClockType.STEADY)
clock = context.create_clock(ClockType.ROS)

clock.now()
```

ROS override：

```python
clock.enable_ros_time_override(True)
clock.set_ros_time(...)
```

V1不自动订阅 `/clock`。

---

## 5.11 GraphEvent

```python
event = context.create_graph_event()
change = event.take()
```

或：

```python
event = node.create_graph_event(callback)
```

同一个 GraphEvent不得同时用于 manual take和Executor callback模式。

---

## 5.12 Parameter

Python映射：

```text
None       -> NotSet
bool       -> Bool
int        -> Integer
float      -> Double
str        -> String

bytes
bytearray  -> ByteArray

list[bool]
list[int]
list[float]
list[str]
```

`bool`判断必须先于 `int`。

Python int必须满足 `int64_t`范围。

混合数组拒绝。

空数组必须显式指定类型。

binding调用 DMW：

```text
as_bool()
as_integer()
...
```

之前必须先检查：

```text
ParameterType
```

绝不能因 Python类型错误触发 DMW的：

```text
std::terminate()
```

---

## 5.13 Error mapping

完整映射：

```text
DclpyError
├── InvalidArgumentError
├── InvalidStateError
├── InvalidNameError
├── TypeMismatchError
├── AlreadyExistsError
├── NotFoundError
├── AlreadyRegisteredError
├── NotRegisteredError
├── BusyError
├── DclpyTimeoutError
├── UnsupportedError
├── IncompatibleQosError
├── ParentDestroyedError
├── ResourceExhaustedError
├── MiddlewareError
├── ContextShutdownError
├── EntityClosedError
├── ExecutorStoppedError
├── InterruptedError
└── ProtocolFaultError
```

DMW `Interrupted`通常由 binding按上下文重新映射：

```text
entity close
    -> EntityClosedError

context shutdown
    -> ContextShutdownError
```

---

# 6. Action完整协议设计

## 6.1 Public API参考 rclpy

Action不使用：

```python
node.create_action_client()
```

作为首要 public API。

采用：

```python
move_client = ActionClient(
    node,
    Move,
    "/move",
)
```

以及：

```python
move_server = ActionServer(
    node,
    Move,
    "/move",
    execute_callback,
    goal_callback=...,
    cancel_callback=...,
)
```

与 rclpy使用习惯一致。

但 DCLPY不实现 callback group。

---

## 6.2 ActionClient

```python
future = client.send_goal_async(
    goal,
    feedback_callback=None,
    goal_id=None,
)
```

goal_id=None使用 UUID v4 compatible random 16-byte GoalId；
不得用 incremental integer、timestamp或 pointer。
用户提供的 GoalId必须为有效 16-byte UUID，并在仍被本 client跟踪时保持唯一。

send admission在任何 native write之前创建 ClientGoalRecord，
登记 GoalId -> feedback callback、pending goal operation和预创建的 ClientGoalHandle。
若 snapshot、record创建或 enqueue失败，原子撤销这些本地记录。
默认 max_goal_records = 4096，容量耗尽在发送前返回 ResourceExhaustedError。

### Feedback

每个 ready feedback sub-channel最多 take一个 envelope，按 GoalId查找 ClientGoalRecord。
未跟踪 UUID、已经拒绝/关闭/取消跟踪的 Goal直接 drop。
路由在发送前已安装，所以 feedback可以先于 goal-response Future完成被正常 dispatch，
不要求 ClientGoalHandle已经公开给用户。

callback参数为 generated FeedbackMessage envelope，即 goal_id和 feedback，
与 rclpy使用习惯一致。同步 callback在 owner Executor线程执行；
coroutine callback进入 general callback task pool，并持有独立 WorkLease。
general capacity满时 take并 drop本条 feedback，记录 bounded diagnostic；
不得因此暂停 Goal/Cancel/Result response或 Status dispatch。

### Status

每个 ready status sub-channel最多 take一个 GoalStatusArray。
只更新本 client已跟踪的 GoalId；未知 UUID不创建 tracking record。
验证 status属于 ROS GoalStatus枚举，然后更新 ClientGoalRecord中的 latest status。

goal response尚未完成时，latest status暂存于已经预创建的 record，
不会丢失先到的 Accepted / Executing / terminal状态。
accepted response完成后，handle采用已有 latest status；没有 status时初始 Accepted。
rejected handle保持 STATUS_UNKNOWN。

已知 terminal状态不被旧 non-terminal snapshot降级。
GetResult response返回其 wire status；若 handle已知 terminal，
expiry后的 STATUS_UNKNOWN result response不覆盖 handle已有 terminal状态。

### Tracking清理

rejected / send失败 / send Future取消时解除 feedback/status路由；
native send尚未结束时只保留§4.20要求的 send owner。
Goal terminal且 goal-response operation已 reconcile后，
把最终 status提交到 handle，再移除 feedback/status tracking。
已返回 handle可用自己的 GoalId继续请求 GetResult，不依赖 tracking record存活。

关闭时清理全部 routing、取消 feedback Tasks并 settle各类请求 Future。
Future.cancel()只取消本地等待，不自动发送 CancelGoal；
远端取消必须使用 cancel_goal_async()。

---

## 6.3 ClientGoalHandle

```python
goal_handle.accepted
goal_handle.goal_id
goal_handle.status

goal_handle.get_result_async()
goal_handle.cancel_goal_async()
```

rejected Goal：

```text
返回 ClientGoalHandle
accepted == False
```

不是 exception。

---

## 6.4 Action request state

Goal / Cancel / Result request均使用：

```text
OutboundOperation
```

因此完整覆盖：

```text
RequestId产生前 cancel
write期间 cancel
RequestId产生后 cancel
Client close
Executor stop
Context shutdown
```

worker不会修改 Python registry。

---

## 6.5 ActionServer ProtocolLane

每个 ActionServer拥有：

```text
ProtocolLane
```

状态：

```text
IDLE
BUSY
```

用于关闭 DMW native commit 与 Python state commit之间的可见性窗口。

以下控制事务必须进入该 lane：

```text
Goal request decision/response
Cancel request decision/response
需要立即响应的 Result request
```

---

## 6.6 ProtocolLane busy时

暂时 unregister：

```text
ActionServer aggregate WaitSet registration
```

DDS仍可以接收并缓存：

```text
Goal request
Cancel request
Result request
```

但 Executor不 dispatch。

因此不会发生：

```text
DMW已经 accept
        ↓
Python仍 PREPARED
        ↓
新的 GetResult/Cancel已经被 Python处理
```

---

## 6.7 Goal accept PREPARED

Goal request成功 take之前预留 critical-response ticket、
ProtocolWork storage及 WorkLease；没有容量时按§4.14暂停 admission。

```text
read_goal_request
    -> record RequestId in ProtocolWork
    -> ProtocolLane BUSY
```

accepted response发送前完成：

1. 验证 GoalId；
2. 预留 Action execute capacity，无法预留则走 rejected response；
3. 调用同步 goal callback；
4. 构造并 snapshot accepted/rejected response；
5. 创建 ServerGoalState和 ServerGoalHandle；
6. 保存 Goal request；
7. 创建 native result cache placeholder；
8. coroutine execute预创建 gated Task及其 execute WorkLease；
9. 构造 fallback abort ResultPayload；
10. 状态设为 PREPARED。

ProtocolWork与 execute work的 lease在 admission期间建立，
后续转换/派生仍属于这份 accepted work。
任意 pre-commit preparation失败都通过同一 owner取消 gated Task、
释放 execute capacity、discard_goal_request并释放 ticket/leases，
在 finally恢复 ProtocolLane；不能等待一条从未入队的 completion。

收到有效 request但 goal callback拒绝，或 execute capacity不足时，
使用已保留 ticket发送 rejected response，不能泄漏 native RequestId。

---

## 6.8 goal callback exception

goal callback抛异常：

```text
report callback error
        ↓
accepted = false
```

然后走普通 reject response。

不让 callback exception泄漏 Goal request。

---

## 6.9 accepted native commit

accepted response使用已有 critical ticket和 OwnedSample进入 protocol job，
由 worker调用 dmw::ActionServer::accept_goal()。
ActionServer aggregate registration保持暂停，completion发送至 owner mailbox。

success completion必须先确认 shutdown/close状态：

```text
normal RUNNING owner
    -> PREPARED -> COMMITTED
    -> release gated execution
    -> mark status dirty
    -> ProtocolLane IDLE
    -> re-register and wake WaitSet

entity CLOSING / owner STOPPING / Context shutdown requested
    -> record COMMITTED
    -> do not start user execute
    -> abort with prepared fallback through accepted WorkLease
    -> drain/discard accepted requests
    -> release lane and work without re-registering user admission
```

独立 entity close及正常 shutdown属于第二条正常 cleanup路径，
不能作为 post-commit scheduling fault处理。

因此新 Cancel/GetResult只能在 Python COMMITTED完成后进入普通 dispatch；
shutdown中的旧 completion仍必须由§4.18 control path消费。

---

## 6.10 accepted response失败

Phase 0必须提供§1.8的 reservation rollback guard：
错误返回及 commit前 exception均撤销 reservation。
ProtocolFault表示 successful write之后的 native commit invariant损坏，
立即进入§6.11，禁止按未接受 Goal重试。

可重试的 delivery失败最多3次，始终复用相同 RequestId、GoalInfo、
response snapshot、critical ticket及 ProtocolWorkLease。

永久失败或尚未开始的 accepted job在 shutdown中被取消时：

```text
discard_goal_request
    -> remove PREPARED
    -> cancel gated Task
    -> release execute capacity
    -> release response ticket and work leases
    -> ProtocolLane IDLE
```

native write已开始则不能提前走这条流程；
等待 owner completion判断是否 COMMITTED，再选择成功 cleanup或失败 discard。
所有退出路径必须结束 lane，CLOSING/STOPPING时不得恢复 user admission。

---

## 6.11 post-commit failure边界

accepted wire response成功 + DMW commit是不可回滚 protocol commit point。

因此文档不再声称：

> commit后绝不会发生任何 allocation。

真正冻结：

> commit前预分配所有正常执行必需的 Python state；commit后的异常不能回滚 accepted Goal。

如果：

```text
release gate
executor scheduling
internal Python bookkeeping
```

在 commit后发生无法恢复的异常：

```text
ActionServer enters FAULTED
```

并：

1. 使用预构造 fallback result；
2. best-effort将 Goal transition为 Aborted；
3. best-effort保存 fallback result；
4. best-effort publish status；
5. 停止接受新 Goal；
6. 报告 fatal internal error。

不能：

```text
假装 Goal没有被接受
```

---

## 6.12 Execute callback

SingleThreadedExecutor只支持 sync；
AsyncIOExecutor支持 sync或 coroutine。
coroutine Task在 native commit之前创建，先 await commit gate，
COMMITTED且 owner仍允许 execute时才能执行用户代码。

server.close()以及 cancel_tasks=True shutdown由 owner control path取消已有 execute Task；
cancel_tasks=False让已经开始的 execute Task自然结束，但不启动尚未释放 gate的用户代码。
未启动 accepted protocol job可直接取消并 discard；
正在 native write的 accept job保留 ProtocolWork直到 completion，
不能让 Task cancellation代替 native commit结果。

无论 Task是运行中、仍在 gate前，还是 Task factory/eager execution路径，
完成 observer均负责注销 Task、释放 execute capacity并执行§6.13 cleanup。
Task在首次执行之前就被取消时，也不能只依赖 coroutine内部 finally释放 lease。

---

## 6.13 Execute callback异常

正常返回但 Goal仍 active，warning并 abort：
return value有效时冻结它，否则使用预构造 default ResultPayload。

普通异常且 Goal仍 active，报告 callback error并 abort(default ResultPayload)。
asyncio.CancelledError单独作为 cancellation处理；
它继承 BaseException，不能只用 except Exception覆盖。

执行 Task取消后，owner cleanup必须：

```text
if Goal still Accepted: apply Execute, then Abort
if Executing / Canceling: apply Abort
if already terminal: keep committed result
    -> settle PendingResultWork
    -> schedule/drain result deliveries with their existing tickets
    -> mark status dirty
    -> release execute capacity and WorkLease
```

该 Abort属于已有 accepted work的内部 cleanup，
即使 entity CLOSING也不 acquire新的 user OperationLease。
不得把取消 Python Task等同于 ROS Canceled；Canceled transition仍受 DMW FSM约束。

server.close()还必须枚举尚未结束的 PendingResultWork / ResultDelivery，
已有 terminal payload的 request继续 delivery，否则执行明确 discard。
Busy等待 running write completion；success或 NotFound表示该 request结束。
accepted Goal不能因 Task取消永久停留 Executing，也不能遗留 pending RequestId或 lease。
server最终 teardown由 owner清空 result cache、status snapshot及 ServerGoalState的 payload槽；
已有 ResultDelivery靠 shared ownership完成发送。用户保留的 GoalHandle只保留元信息，
关闭后 mutating API返回 EntityClosedError，不继续保留内部 OwnedSample阻止 provider退休。

---

## 6.14 ServerGoalHandle线程归属

以下 mutating API：

```text
succeed()
abort()
canceled()
publish_feedback()
```

V1要求运行于 ActionServer所属 Executor thread/event-loop thread。

其他线程调用：

```text
InvalidStateError
```

这样 Goal Python state、DMW FSM和result cache保持单线程串行提交。

---

## 6.15 Terminal transaction

以 goal_handle.succeed(result)为例：

```text
validate Result
    -> clone/freeze Result
    -> build immutable GetResult response payload
    -> stage ResultPayload
    -> DMW update_goal_state(Succeed)
```

transition失败则丢弃 staged payload并 raise，PendingResultWork保持原样。
成功后，在同一 Executor turn、返回 event loop之前：

```text
attach payload to pre-created native cache placeholder
    -> take_pending_result_requests()
    -> find existing PendingResultWork for each RequestId
    -> transfer each work to ResultDelivery with its reserved ticket
    -> mark status dirty
```

不能在 transition之后才分配 request work或争抢 response queue容量。
PendingResultWork registry、job node和 completion storage均在 take前由 ticket预留；
shared payload attachment和 work transfer使用 noexcept C++操作。
仍持有 RequestId的 work不能因 Python bookkeeping异常被静默遗失；
unexpected post-commit invariant错误进入 FAULTED并通过原 work owner逐项 drain/discard。

因此 native Terminal状态、cache commit和 pending delivery转换在同一 turn闭合。

---

## 6.16 ResultPayload

```text
ResultPayload
```

是 immutable C++ owned snapshot。

多个：

```text
GetResult delivery
```

通过：

```text
shared ownership
```

复用。

不会再次访问用户原始 `Result` Python object。

---

## 6.17 GetResult

read_result_request之前预留 critical ticket、WorkLease和 PendingResultWork storage。
take成功后立刻填入 RequestId并登记 work owner，再 extract GoalId及调用
register_result_request()。后续 validation/registration失败由该 owner执行 discard。

PendingResultWork包含：

```text
RequestId
GoalId
WorkLease
critical-response ticket
preallocated delivery/job/completion storage
state: WAITING_GOAL / READY / SENDING / RETRY / ENDED
```

### Pending

DMW保留 RequestId -> Goal关联；
DCLPY保留 WAITING_GOAL work及 ticket，不发送，不再创建一个等待 Goal完成的无界 Task。
Goal terminal时按§6.15转换；close时由 owner显式 drain/discard。

### Terminal

确认 cache存在，attach immutable payload，
把原 work无间隙转换为 ResultDelivery。

### UnknownGoal

构造 STATUS_UNKNOWN + default Result()的 immutable response payload，
使用同一个 work及 ticket进入 ResultDelivery。

三个分支均从 take开始持续拥有 RequestId和 WorkLease。
不能只有 Terminal / UnknownGoal才创建 request lifetime记录。

---

## 6.18 ResultDelivery

ResultDelivery是 PendingResultWork进入 READY / SENDING / RETRY后的 delivery阶段，
不新 acquire lease，不重新申请 ticket。

它持有：

```text
RequestId
shared_ptr<const ResultPayload>
attempt
WorkLease
critical-response ticket
reserved job/completion storage
```

直到 response成功，或明确永久失败并完成 discard，才进入 ENDED，
注销 request work并释放 ticket及 lease。
running write期间 close不重复 discard；其 completion负责继续 retry或结束 cleanup。

ResultDelivery属于 Request生命周期，与 Goal retention cache分离。
retention expiry只移除 cache索引，不移除已有 delivery及其 shared payload。

---

## 6.19 Result retention expiry

DMW retention到期：

```text
take_expired_goals()
```

DCLPY：

```text
erase GoalId -> result cache
erase GoalHandle tracking
mark status dirty
```

含义：

> 从此新的 GetResult被视为 UnknownGoal。

但是已经存在的：

```text
ResultDelivery
```

继续持有：

```text
shared_ptr<ResultPayload>
```

直至：

```text
成功发送
或
明确永久失败/discard
```

因此：

```text
Goal retention expiry
```

与：

```text
已经接受的 Result request delivery
```

彻底分离。

不会因为 retention timeout恰好到期就错误关闭 ActionServer。

---

## 6.20 Result response failure

最多：

```text
3 attempts
```

失败后：

```text
discard_result_request(RequestId)
        ↓
release ResultDelivery, critical ticket and WorkLease
```

记录 MiddlewareError。

单个 client delivery失败不要求关闭整个 ActionServer，除非：

```text
discard_result_request返回非 NotFound、非可等待 Busy的错误
或
DMW internal invariant破坏
```

---

## 6.21 Status发布

以下事件后：

```text
status_dirty = true
```

- accept commit；
- Canceling；
- Succeeded；
- Aborted；
- Canceled；
- expiry cleanup。

publish：

```text
status_snapshot()
        ↓
build GoalStatusArray snapshot
        ↓
OwnedSample
        ↓
protocol I/O job
```

如果 status正在发送期间又发生状态变化：

```text
status_dirty仍保持 true
```

发送成功后检查 revision：

```text
如果有新变化
    -> 再提交一次最新 snapshot
```

因此不会丢最终状态。

---

## 6.22 Status retry progress

失败后不依赖：

```text
下一 Executor cycle
```

NativeIoDispatcher直接安排：

```text
delayed retry
```

并自行定时唤醒。

默认：

```text
最多3次连续尝试
```

三次失败：

```text
保留 status_dirty
报告 MiddlewareError
```

之后下一次状态变化再次触发发送。

---

## 6.23 Cancel selector

DMW仍然负责：

```text
select_cancel_goals()
```

Python不能自己重写 selector规则。

---

## 6.24 Exact ID cancel

当：

```text
GoalId != zero
stamp == 0
```

先查询目标状态。

### NotFound

```text
ERROR_UNKNOWN_GOAL_ID
```

### Accepted / Executing

candidate可进入 user cancel callback。

### Canceling

按 Humble行为：

```text
ERROR_GOAL_TERMINATED
```

### Succeeded / Aborted / Canceled

```text
ERROR_GOAL_TERMINATED
```

即：

```text
Canceling虽然不是真正 terminal
但 exact-ID ROS compatibility branch按不可取消处理为
ERROR_GOAL_TERMINATED
```

---

## 6.25 cancel callback异常

对所有 candidates：

```text
先调用 callback并收集 decision
```

在任何 DMW transition前完成。

如果任意 callback抛异常：

```text
不 transition任何 Goal
ERROR_REJECTED
goals_canceling = []
```

这样不会产生：

```text
部分 Goal已 Canceling
另一部分 callback又异常
```

---

## 6.26 cancel callback全部拒绝

candidate非空但用户全部拒绝：

```text
ERROR_REJECTED
goals_canceling = []
```

至少一个接受：

```text
apply DMW CancelGoal transitions
        ↓
ERROR_NONE
```

---

## 6.27 Cancel response delivery

DMW transitions完成后不能 rollback。

Cancel response最多重试3次。

永久失败：

```text
discard_cancel_request(RequestId)
```

Goal仍保持已经提交的 Canceling状态。

报告 protocol delivery error，但不反向恢复 FSM。

---

## 6.28 rejected Goal response

Goal reject同样：

```text
最多3次 response delivery
```

永久失败：

```text
discard_goal_request(RequestId)
```

没有 Goal state需要保留。

---

## 6.29 Action expiry readiness

DMW Phase 0修复后：

```text
terminal transition
    ↓
new expiry deadline
    ↓
wake active WaitSet
```

因此即使之后完全没有 DDS流量：

```text
Goal也会按时 expiry
        ↓
DCLPY接收 kActionGoalExpiredBit
        ↓
清理 cache
```

---

# 7. Shutdown、测试、开发阶段与 V1 冻结项

## 7.1 Shutdown总体原则

关闭顺序固定为：

```text
stop user admission and commit shutdown cause
    -> interrupt waits
    -> stop new DDS user dispatch
    -> keep owner control path running
    -> settle Futures and finish/cancel existing Tasks
    -> drain/discard accepted protocol work
    -> retire ready batch, producers, completion and notifications
    -> remove registrations and retire mailbox routes
    -> wait leases and physical entity destructors
    -> shutdown DMW Context
```

“stop user dispatch”不等于停止 completion/control处理。
Executor必须通过§4.18 barrier才能 STOPPED；
Context必须等所有 owner及 native worker结束才能 CLOSED。

---

## 7.2 Context shutdown顺序

Context request_shutdown首先在 admission lock下标记 shutdown requested，
拒绝新 user work并记录 ContextShutdown cause，再请求各 Executor STOPPING。
各 owner首先按该 cause结束 pending DCLPY Future；已 terminal Future不覆盖。
外部调用线程不越过 owner修改 Python registry。

availability waits通过§1.9 token/generation机制 interruption。
queued user send取消；正在执行的 native send保留 owner直到 completion并建立 tombstone。
尚未开始的 accepted Goal protocol job取消/discard；
已开始的 job等待 native commit结果，commit成功则走 fallback abort cleanup。

各 owner按§4.18继续 control drain；
callback Tasks按 cancel_tasks policy结束，
Service及 Action请求使用原 WorkLease和 critical ticket完成 response/discard。

Context shutdown中的 attached child close及 cache cleanup必须在 owner control route退休前发起；
owner barrier还要等待这些 child的 physical teardown。
已 STOPPED/成功 detach且完全 quiescent的 entity、以及从未 attach的 entity，
可由 coordinator在持 GIL和 backing保护下关闭，不需要复活旧 mailbox或执行用户回调。
所有 owner barrier完成后才停止/join Context native worker，
剩余 child也全部完成 physical teardown后，最后调用 DMW Context shutdown。
此时 router/mailbox、tickets、TaskRegistry和 pending Future均不得残留。
Context真正 CLOSED时不能存在属于它的 PENDING DCLPY Future。

多 loop协调、caller timeout/cancellation和重复 shutdown调用遵守§4.25–4.27。

---

## 7.3 Context不等待自己所在event loop

同步：

```python
context.shutdown()
```

如果当前线程属于：

```text
registered AsyncIOExecutor event-loop thread
```

直接：

```text
InvalidStateError
```

不区分是不是 Executor创建的 callback。

普通：

```python
async def stop_application():
    context.shutdown()
```

同样禁止。

正确：

```python
await context.shutdown_async()
```

---

## 7.4 `shutdown_async(cancel_tasks=False)`

允许已有 user Task自然完成。

如果 timeout：

```text
STOPPING
```

状态保留。

不强行析构。

---

## 7.5 Executor停止后的 Future

本节指独立 Executor stop；由 Context shutdown驱动时沿用 ContextShutdownError，
不覆盖§3.14已经提交的 terminal结果。

当 Executor停止时：

```text
所有依赖该 Executor dispatch 的 unresolved Future
```

完成：

```text
ExecutorStoppedError
```

如果 RequestId已经发送：

```text
insert tombstone
```

late response丢弃。

如果 native send仍在执行：

```text
Future先 terminal
send completion后登记 tombstone
```

---

## 7.6 Client close后的 Future

本节指独立 Client.close()；Context shutdown中的 child close沿用 ContextShutdown cause。
已 terminal结果始终保持不变。

Client全部：

```text
QUEUED
SENDING
REGISTERED
```

请求最终：

```text
EntityClosedError
```

已经 user-canceled的 Future保持：

```text
CANCELED
```

不能覆盖。

---

## 7.7 Context shutdown后的 Future

所有仍 PENDING：

```text
ContextShutdownError
```

这是一项 release invariant：

> Context真正 CLOSED 时，不能存在属于该 Context 的 PENDING DCLPY Future。

---

## 7.8 Message concurrency test

必须验证：

```python
msg.position = [1.0]

future = publisher.publish_async(msg)

msg.position = [2.0]
```

wire收到：

```text
[1.0]
```

同步：

```text
Thread A:
    publish(msg)
    // GIL released during write

Thread B:
    msg.position = ...
```

不得发生：

```text
TSan data race
corrupted serialization
```

同时覆盖 complex setter失败原子性：
对 string、sequence及包含多个动态字段的 nested message注入 staged allocation/conversion失败，
原 backing全部字段及已取得 live child view均保持有效。
生成器必须拒绝不能证明 noexcept commit的 field；不得使用 copy assignment绕过验收。

interface ABI验收覆盖：
Message / Service / Action capsule name、abi_version、struct_size、compatibility_id错误，
必要函数指针缺失、constituent descriptor/class不一致，均在 import/type注册时失败。
clone bad_alloc返回固定 error；create_instance失败正确设置 Python error；
provider在 queued/running OwnedSample结束之前不能释放。

---

## 7.9 Lease组合测试

必须覆盖：

```text
Service coroutine awaits
    +
service.close()

Action execute task awaits
    +
server.close()

publish_async queued
    +
publisher.close()

native write running
    +
entity.close()
```

要求：

```text
no UAF
no early destructor
no permanent work_refs
```

补充 Pending GetResult + server.close()及 execute Task取消：
要求旧请求逐项 response/discard，registry不再返回失效 RequestId，
tickets、execute capacity及 work_refs最终归零。

在 gated Task首次执行之前取消，以及使用 eager task factory时取消，
都必须由 Task completion observer结束 work。
native write/completion尚未结束时 backing及 owner mailbox不能提前退休。

---

## 7.10 Infinite wait close regression

固定：

```text
no service exists

Thread A:
    client.wait_for_service(None)

Thread B:
    client.close()
```

验收：

```text
wait exits with EntityClosedError
client eventually CLOSED
wait_closed succeeds
no Context shutdown required
```

对 Client和 ActionClient均注入三个确定性暂停点：

1. admission之前；
2. OperationLease和 AvailabilityWaitToken已经取得，但尚未进入 native wait；
3. 已经进入 native condition-variable wait。

分别执行 entity.close()与 Context request_shutdown，验证正确错误类型、无漏唤醒。
DMW单独测试 interrupt之后取得新 token的 wait不被旧 generation取消，
以及跨 entity token返回 InvalidArgument。

---

## 7.11 Async shutdown tests

覆盖：

```text
managed coroutine calls request_shutdown()

ordinary asyncio Task calls:
    await context.shutdown_async()

ordinary asyncio Task incorrectly calls:
    context.shutdown()
        -> InvalidStateError

wait thread waiting ACK
    +
shutdown

loop stopped before shutdown complete

loop closed incorrectly with managed Task
```

必须验证 managed Task调用 Executor/Context shutdown_async()，
在 cancel_tasks=True/False两种策略下均先返回 InvalidStateError；
普通 application Task可以正常 await。

在 STOPPING之后投递 SendCommit、accepted Goal completion、response失败和 capacity wake，
要求 control path继续 drain，协议 request/ticket/lease全部结束后才 STOPPED。
SingleThreaded spin退出、CREATED Executor关闭及未ACK batch也须覆盖此 barrier。

同一 Context创建两个运行于不同 loop/thread的 AsyncIOExecutor：
completion只在所属 loop处理，Context coordinator跨 loop关闭成功，
不会 await foreign-loop Task，也不会阻塞 caller loop。

caller timeout/cancellation不得取消 internal coordinator；
重复 shutdown加入同一流程，cancel_tasks=True升级生效，
已 stopped/closed owner loop按契约保留资源并报告明确状态。

---

## 7.12 Worker/event-loop race test

构造立即响应 Service：

```text
worker write has returned RequestId
但 SendCommit尚未被 event loop消费
```

ROS server立即回复。

验收：

```text
response enters early_responses
SendCommit reconciles
correct Future completes
```

不得：

```text
event loop blocking worker mutex
lost response
wrong Future completion
```

在 response已 staging而 SendCommit尚未消费时分别触发：
user cancel、Client close、Executor stop和 Context shutdown。
completion只能建立 tombstone/清理 staging，不得复活 active Future。

制造超过 tombstone TTL的迟到响应、重复已完成 response，以及未知 RequestId。
无 unreconciled send时直接 drop；有 candidate时暂存，但候选 retire后全部清理。
持续发送新 operation不能延长旧 entry生命周期，所有 staging容量保持有界。

同 Context两个 Executor同时发送请求并反向交错 completion：
仅 owner mailbox消费正确 operation；有 accepted work时 remove_node返回 Busy，
重新 attach后的 generation不会收到旧 completion。

---

## 7.13 Service saturation test

配置：

```text
max_callback_tasks = 1
max_service_tasks_per_service >= 1
```

Service A：

```python
async def A(req):
    return await client_b.call_async(...)
```

Service B正常回复。

必须成功。

另外测试：

```text
same Service recursive chain
exhaust max_service_tasks_per_service
```

应表现为：

```text
Service registration backpressure
```

而不是无界创建 Task。

这种应用递归不承诺一定 progress。

protocol容量测试必须分别占满 user、Feedback/Status、Service和 Pending GetResult ticket，
验证未 take的 request只受到 backpressure，event loop继续运行。
Pending Result/Service合计不得消费32个 Goal/Cancel control reserve；
Result request interest单独暂停，Cancel仍可进入 protocol lane并得到响应，
expiry仍能被 WaitSet报告；禁止 suppressed reader导致 busy-spin。

已经成功 take的 Service/Goal/Cancel/GetResult work均持有 response ticket：
发送、三次 retry、永久失败 discard及 Task取消均不再申请新 queue slot。
未取得 ticket时不得创建 PREPARED/gated Task；
释放 ticket后无需 DDS新输入即主动恢复 admission。

---

## 7.14 Action commit barrier test

强制让：

```text
DMW accept_goal success
```

之后暂停 worker completion handoff。

期间 remote client发送：

```text
Cancel
GetResult
```

验收：

```text
ActionServer aggregate registration仍 paused
Python不得处理这些 request
```

直到：

```text
PREPARED -> COMMITTED
```

完成后才恢复 dispatch。

在 reserve之后、accepted write commit之前注入 native exception：
同 GoalId reservation必须被撤销，重试可正常接受，worker仍产生唯一 failure completion。
注入 post-write commit invariant错误则返回 ProtocolFault，不得走三次未接受重试。

暂停 accept native write时执行 server.close()/shutdown：
未开始 job走 discard；running job保留 owner；
native commit成功后不释放用户 execute gate，而执行 fallback abort cleanup。
最后 lane、execute capacity、ticket、Task和 lease均结束，正常关闭不报告 scheduling fault。

ActionClient同时测试 Feedback和 Status先于 Goal response到达：
feedback按预登记 UUID路由，handle创建后采用缓存 status；
terminal、rejected、send取消和 close均清理 tracking，
未知 UUID不创建 record，feedback capacity耗尽不阻塞 RPC response。

---

## 7.15 Result retention/delivery test

场景：

```text
Goal terminal
GetResult before expiry
response delivery第一次 timeout
Goal retention expires
第二次 delivery retry成功
```

必须：

```text
新 GetResult -> UnknownGoal
旧 ResultDelivery -> 仍能成功发送原 result
```

不得关闭 ActionServer。

对 active Goal反复 register Pending GetResult再 discard：
Server capacity恢复，GoalRegistry中不积累被放弃的 ID，
take_pending_result_requests()不会返回已经 discard的 request。

并发测试 register、terminal take、write claim和 discard：
每个 RequestId只有一个 owner，Responding时 discard返回 Busy且不修改关联；
重复结束返回 NotFound且不会重复释放 ticket/lease。

PendingResultWork从 take到 terminal delivery / server close全程持有 WorkLease和 ticket，
terminal transition后没有 queue满分支，也不能因 retention expiry删除已有 delivery。

---

## 7.16 Retry no-traffic test

制造：

```text
Status publish fail once
```

之后不再产生任何 DDS输入。

仍必须：

```text
NativeIoDispatcher自己唤醒
        ↓
retry
        ↓
success
```

同样测试：

```text
Service response retry
Result response retry
```

---

## 7.17 Action expiry no-traffic test

Goal terminal后：

```text
没有任何后续 DDS流量
```

WaitSet仍必须按 result timeout唤醒并报告：

```text
kActionGoalExpiredBit
```

---

## 7.18 Parameter测试

必须包括：

```text
bool vs int

INT64 range

mixed array

empty array

wrong accessor prevention

atomic set

change set
```

---

## 7.19 Error mapping测试

DMW每一个：

```text
ErrorCode
```

必须有明确 Python映射。

禁止 fallback成模糊：

```text
RuntimeError
```

除非真的属于未分类 internal invariant。

包含 Phase 0新增 Interrupted与 ProtocolFault。
Context shutdown先提交 ContextShutdown cause，owner停止时提交 ContextShutdownError；
独立 Executor shutdown提交 ExecutorStoppedError。
Future已 canceled/finished时不得覆盖先前 terminal状态。

---

## 7.20 Python version matrix

完整：

```text
current Humble Fast DDS
    × cp310
    × cp311
    × cp312
    × cp313
```

minimum DMW/Fast DDS：

```text
minimum supported
    × cp310
    × cp312
```

现代 compatibility：

```text
Fast DDS 2.14.x
    × selected Python
```

---

## 7.21 DCLCPP interop

属于 conditional gate。

如果：

```text
DCLCPP implementation available
```

运行：

```text
DCLPY <-> DCLCPP
```

否则不能阻塞 DCLPY V1 release。

---

## 7.22 mfr3duo E2E manifest

Phase 0从本地实际：

```text
mfr3duo_msgs
mfr3duo_ros2
controller config
launch config
```

生成：

```text
tests/ros2/mfr3duo_endpoints.yaml
```

冻结：

```text
actual state topic/type/QoS

mfr3duo_msgs/action/Move
actual Move endpoint/QoS

mfr3duo_msgs/action/Grasp
actual Grasp endpoint/QoS
```

具体 endpoint不得凭设计文档猜测。

---

# 8. 开发阶段

虽然正文按系统能力组织，开发仍按照依赖顺序执行。

## Phase 0 — DMW readiness

完成：

```text
Action expired notification queue

Action deadline/readiness wake notification

Server::discard_request()

ActionServer::
    discard_goal_request()
    discard_cancel_request()
    discard_result_request()

Client::interrupt_waits()

ActionClient::interrupt_waits()

AvailabilityWaitToken
Client/ActionClient::prepare_availability_wait()
token-aware wait overloads and admission/interrupt race regression
WaitSet::set_interest() for ActionServer sub-channel backpressure

Action result-request coordinated claim/discard/registry cleanup
accept_goal reservation rollback guard and exception regression

ErrorCode::Interrupted
ErrorCode::ProtocolFault

DMW shared build
SOVERSION
install consumer

现有 DMW regression

mfr3duo endpoint manifest
```

这是硬前置。

---

## Phase 1 — Core binding / lifecycle

实现：

```text
pybind11
scikit-build-core

Context
Node

EntityBacking

OperationLease
DispatchLease
WorkLease

close / wait_closed

Error mapping
BuildInfo

QoS
Clock
Parameter
```

优先：

```text
cp310
cp312
```

---

## Phase 2 — Interface binding / Topic

实现：

```text
MessageBindingV1
ServiceBindingV1
ActionBindingV1 and generated Action.Impl metadata
DclpyBindingErrorV1 / capsule compatibility validation
OwnedSample / provider lifetime

clone/destroy ABI

field generator
noexcept atomic setter
cross-package types

Publisher
Subscription

SingleThreadedExecutor
```

完成：

```text
Python 3.12 DCLPY
    <->
ROS 2 Humble Topic
```

---

## Phase 3 — Future / NativeIo / synchronous Service

实现：

```text
Future state machine

OutboundOperation

CompletionPort router / per-Executor mailbox / attachment generation

NativeIoDispatcher
response tickets / control reserve / non-blocking backpressure
worker exception completion / STOPPING control barrier

early response staging / candidate retirement

Client
Service sync callback

request tombstones
```

完成双向 ROS Service interop。

---

## Phase 4 — AsyncIO / Service coroutine

这一阶段必须在 Service coroutine之前完成 AsyncIO基础设施。

实现：

```text
AsyncIOExecutor

batch ACK

Task registries

Service task pool

Action execute capacity foundation

shutdown_async

Context async shutdown
managed Task self-wait guard
cross-loop coordinator / shielded timeout / cancellation upgrade

Service coroutine callback
```

因此不再出现：

```text
Phase 3要求 Service coroutine
但 Phase 4才有 AsyncIOExecutor
```

依赖倒置。

---

## Phase 5 — Timer / Graph / Parameter完整化

完成：

```text
Timer
GraphSnapshot
GraphEvent

Parameter full API

lifecycle stress
```

---

## Phase 6 — Action

实现：

```text
consume Phase 2 ActionBindingV1

ActionClient
ClientGoalHandle
Feedback/Status routing and pre-response status staging

ActionServer
ProtocolLane

PREPARED / COMMITTED barrier

ServerGoalHandle

Cancel

GetResult

ResultPayload
PendingResultWork / ResultDelivery / ticket transfer
Task cancellation and close protocol cleanup

Status publication

Expiry cleanup
```

完成 ROS Action双向 interop。

---

## Phase 7 — Packaging / mfr3duo integration

完成：

```text
cp310
cp311
cp312
cp313
```

生成：

```text
mfr3duo_msgs_dclpy
```

运行已经在 Phase 0冻结的：

```text
state interface
Move Action
Grasp Action
```

系统 E2E。

---

# 9. V1 最终冻结原则

以下内容不得在实现阶段重新设计：

1. DCLPY与DCLCPP平级。
2. `_dclpy`直接使用DMW。
3. 不依赖`rclpy`。
4. Public API优先参考rclpy。
5. runtime contract以DMW为直接权威。
6. ROS wire protocol以ROS 2/rcl/rcl_action为参考。
7. Python ABI与ROS Python ABI解耦。
8. Python 3.12为第一目标。
9. cp310～cp313分别构建。
10. V1不用abi3。
11. `src/_dclpy/`为C++ binding目录。
12. Core和interface extension共享同一`libdmw.so`。
13. Fast DDS patch不由Humble字符串锁死。
14. Action expiry通知必须先在DMW修复。
15. Action deadline变化必须主动wake WaitSet。
16. DMW增加`Server::discard_request()`。
17. DMW ActionServer增加三个 constituent discard API。
18. Client/ActionClient availability wait在原子 admission时捕获 token，关闭中断也覆盖尚未进入 native wait的调用。
19. EntityBacking统一管理native resource。
20. OperationLease保护同步用户操作。
21. DispatchLease保护WaitResult dispatch。
22. WorkLease保护Task和I/O job。
23. CLOSING拒绝新用户工作。
24. 已接受cleanup允许在CLOSING中继续。
25. native resource析构必须等待三类lease和WaitSet registration全部退出。
26. CLOSED只能在destructor实际完成后发布。
27. `close()`逻辑关闭与物理teardown分离。
28. `wait_closed()`保证物理teardown完成。
29. 所有outbound message在调用/调度时snapshot。
30. Fast DDS worker永远不序列化用户可变Python backing。
31. `publish_async()`发送调用时值。
32. complex field setter只以经过证明的 noexcept swap/move提交，保证失败原子性。
33. nested getter是live view。
34. sequence getter是copy。
35. cross-package generated type只绑定一次。
36. Future terminal transition必须原子。
37. Future callback不得在Future lock中执行。
38. awaiter取消默认不取消底层Future。
39. 所有返回给用户的Future最终必须terminal。
40. queued/sending/registered request全部有明确shutdown结果。
41. RequestId产生前也必须由OutboundOperation跟踪。
42. worker不访问Python registry。
43. event loop不等待worker持有的native-write mutex。
44. worker通过CompletionPort router投递到固定owner Executor mailbox及attachment generation。
45. response早于RequestId注册通过early-response staging解决。
46. tombstone和early response有界，孤立entry随候选send retirement清理。
47. AsyncIO batch ACK不等待coroutine完成。
48. one-batch-in-flight仍然保留。
49. general callback、Service、Action execute使用不同capacity。
50. Service task不受general task capacity阻塞。
51. DCLPY不承诺解决任意应用级Service循环依赖。
52. NativeIoDispatcher为asyncio隔离synchronous Fast DDS write。
53. NativeIo job全部持WorkLease。
54. critical response优先于Feedback/Status和user，Goal/Cancel保留32个control ticket。
55. retry由NativeIoDispatcher自身定时驱动。
56. retry不能依赖下一条DDS流量。
57. synchronous shutdown不能阻塞Executor所属event-loop thread。
58. 普通application Task可await shutdown_async；managed Task必须用request_shutdown并禁止自等待。
59. timeout shutdown保持STOPPING和资源保护。
60. loop关闭前必须完成AsyncIOExecutor shutdown。
61. Action API采用rclpy风格`ActionClient(...)`、`ActionServer(...)`。
62. GoalId默认UUID v4。
63. ActionServer具有ProtocolLane。
64. accepted native commit期间暂停该ActionServer request dispatch。
65. PREPARED→COMMITTED后才恢复Action dispatch。
66. accepted response之前预创建正常运行所需Python state。
67. commit后的异常不可回滚accepted Goal。
68. post-commit internal failure进入ActionServer fault处理。
69. ServerGoalHandle mutating API在V1中Executor-affine。
70. terminal Result在DMW transition前freeze。
71. transition失败撤销staged payload。
72. transition成功后在同一Executor turn中commit result cache。
73. GetResult完整处理Pending/Terminal/UnknownGoal。
74. ResultPayload immutable。
75. ResultDelivery生命周期与Goal retention生命周期分离。
76. expiry只禁止新的GetResult，不破坏已接受delivery。
77. DMW仍是唯一result expiry authority。
78. status由DCLPY基于DMW snapshot发布。
79. status变化通过revision/dirty机制合并。
80. status retry具有主动progress机制。
81. Cancel selector仍由DMW决定。
82. exact-ID Canceling按Humble兼容行为返回ERROR_GOAL_TERMINATED。
83. cancel callback decisions先全部收集，再提交FSM变化。
84. cancel callback异常不产生部分transition。
85. Goal/Cancel/Result request结束必须同时清理constituent pending与registry关联。
86. DCLCPP interop是conditional gate。
87. `mfr3duo_msgs`是当前真实集成interface package。
88. `Move`、`Grasp`是V1实际Action验收对象。
89. endpoint/QoS在Phase 0从源码冻结。
90. 当前最终release gate是Python 3.12 RoServer与ROS 2 Humble mfr3duo_ros2完整通信。
91. accept_goal在commit前的错误返回及exception均由noexcept guard回滚reservation。
92. post-write commit invariant错误为ProtocolFault，不能当作未接受Goal重试。
93. worker exception必须生成唯一completion，报告路径使用预分配storage。
94. STOPPING继续owner control drain，producer、mailbox、Task及protocol work退出后才能STOPPED。
95. Context在所有owner barrier和native worker结束后才shutdown DMW并CLOSED。
96. remove/reattach Node必须满足quiescence，不能迁移未结束work。
97. response ticket在take前预留并覆盖retry/completion/discard，已有request入队不再争抢容量。
98. Pending GetResult从take开始持有PendingResultWork、WorkLease和ticket。
99. shutdown timeout/caller cancellation不取消唯一internal coordinator，cancellation policy只能升级。
100. Context shutdown跨loop使用各owner loop的drain，不能await foreign-loop Task。
101. Action execute Task取消必须处理Goal FSM及pending result requests，并结束capacity/lease。
102. accept completion遇到CLOSING/STOPPING不启动用户execute，使用预构造fallback清理。
103. ActionClient在发送前安装Feedback/Status路由，并保留先于Goal response到达的status。
104. ActionClient tracking有界且有明确retirement，feedback容量不足不阻塞RPC response。
105. Message/Service/Action capsule、错误ABI、envelope元数据、兼容校验和provider lifetime统一由V1头定义。
106. Action容量背压使用DMW sub-channel interest，Result暂停不能阻断Cancel及expiry。

完成上述规格后，DCLPY V1才满足：

> 开发者能够直接按照设计完成实现，而无需重新决定 message ownership、线程安全、Entity teardown、Future lifecycle、asyncio progress、worker交接或 Action protocol transaction。

这一标准。
