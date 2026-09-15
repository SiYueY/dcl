# Fast DDS 版本演进与 DCL 兼容性说明

## 1. 文档目的

本文梳理 Fast RTPS / Fast DDS 1.x 到 2.14.x 的主要演进，并说明这些变化如何影响 DCL/DMW 的源码兼容、wire interoperability、类型绑定、QoS、WaitSet、Service 和 Action 设计。

本文重点回答：

- Fast DDS 2.6.x 与 2.14.x 的差异对 DCL 有什么影响；
- Fast CDR / generated type 的版本边界在哪里；
- 为什么 DCL 追求 source compatibility 而不是跨 minor ABI；
- 为什么 Fast DDS 2.14.x 与 `rmw_fastrtps` Jazzy 是**平等**主要参考基线；
- `rcl`、`rcl_action`、`rclcpp`、`rclpy` 分别用于参考什么职责边界；
- Humble / Fast DDS 2.6.x 在新设计中的定位。

## 2. 版本号与验证基线

需要区分：

```text
Fast DDS upstream version
ROS Debian package version/revision
Ubuntu package revision
Fast CDR version
Fast DDS-Gen version
```

DCL 当前重点验证两条环境线：

| ROS 2 | Tier-1 OS | Fast DDS line | DCL 角色 |
| --- | --- | --- | --- |
| Humble | Ubuntu 22.04 Jammy | 2.6.x | compatibility validation |
| Jazzy | Ubuntu 24.04 Noble | 2.14.x | primary modern validation |

实际 CI/test baseline 必须记录解析后的完整 package revision、Fast DDS/Fast CDR version、image digest 和 compiler，而不能只记录 `Humble` / `Jazzy` 名称。

## 3. Fast RTPS / Fast DDS 历史演进

本章只记录对 DCL 架构判断有影响的变化，不替代 eProsima release notes。

### 3.1 Fast RTPS 1.6

主要能力：

- Persistence；
- Security Access Control Plugin API；
- built-in access permission support。

这一时期仍以早期 Fast RTPS API 为主。

### 3.2 Fast RTPS 1.7

明显扩展：

- TCP transport；
- Dynamic Types；
- DDS Security 1.1；
- XML profile 环境配置；
- discovery/TCP/key-only 等修复。

从早于 1.7 的 generated code 升级到现代 Fast DDS 时不能假定源码兼容，通常需要重新生成类型代码。

### 3.3 Fast RTPS 1.8

主要增加：

- IDL 4.2；
- Deadline/Lifespan/Liveliness QoS；
- TLS/TCP；
- 更严格的资源限制与实时相关配置。

这些能力构成后续 DDS-PIM QoS 的基础。

### 3.4 Fast RTPS 1.9

主要变化：

- Discovery Server；
- allocation QoS；
- non-blocking calls；
- generator 从主仓库拆分；
- intra-process delivery；
- discovery/reliability/liveliness 并发修复。

### 3.5 Fast RTPS 1.10

重要节点：

- Shared Memory Transport；
- Transport API 重构；
- allocation/real-time 配置加强；
- built-in endpoint history 可配置。

### 3.6 Fast DDS 2.0

2.0 是架构分界：

- 引入符合 DDS 1.4 的 DDS-PIM API；
- 旧 Fast RTPS 高层 API进入兼容/弃用阶段；
- Shared Memory 后续成为默认 transport 之一；
- Discovery Server 显著演进；
- Fast DDS CLI 出现。

DCL 新代码应以现代 DDS-PIM public API 为核心，不以旧 Fast RTPS publisher/subscriber API 作为长期抽象基础。

### 3.7 Fast DDS 2.1

DDS-PIM 和 RTPS 层均经历 ABI break，同时继续补充 incompatible QoS、persistence 与 XML 能力。

关键结论：

> source compatibility 与 binary ABI compatibility 是两个问题。

### 3.8 Fast DDS 2.2

重要变化：

- `TopicDataType` interface 扩展；
- DataWriter loan sample；
- DataReader read/take；
- Data Sharing；
- 更完整的 DDS C++ API。

`TopicDataType` / generated code 因此是 DCL 跨版本 source compatibility 的重点。

### 3.9 Fast DDS 2.3

主要变化包括：

- Statistics；
- Discovery Super Client；
- network-flow APIs；
- reception timestamp；
- `DataReader::get_unread_count()`；
- ReturnCode/ABI 调整。

### 3.10 Fast DDS 2.4

与 DCL 当前 runtime 设计关系很大：

- WaitSet；
- GuardCondition；
- StatusCondition；
- Flow Controllers。

因此现代 Fast DDS 已具备支撑“middleware readiness -> Client Library Executor”的原生 wait primitives。

### 3.11 Fast DDS 2.5

主要变化：

- zero InstanceHandle；
- XML profile加载能力增强；
- per-instance ACK wait；
- entity GUID 创建行为演进；
- DataReader history 状态语义调整。

### 3.12 Fast DDS 2.6

Humble 主要版本线。

重点：

- network interface runtime update；
- endpoint discovery API；
- content filtering 能力继续演进；
- `fastdds::Time_t` 等现代 API继续推进；
- DataReader/DataWriter API扩展。

2.6.x 目前对 DCL 的意义主要是兼容性验证，而不是新设计基线。

## 4. 2.6.x 到 2.14.x 的重点变化

### 4.1 2.7

- ReadCondition；
- `find_topic()`；
- timestamp writer APIs；
- SampleRejectedStatus；
- RTPS/history/transport ABI changes。

DCL 应继续远离 private RTPS history/transport implementation。

### 4.2 2.8

- WAN/transport configuration；
- Property QoS propagate；
- Ownership XML；
- TLS/TCP enhancements。

对 DCL 基础 DDS-PIM path影响较小。

### 4.3 2.9

重要行为变化：默认 history memory policy 转向：

```text
PREALLOCATED_WITH_REALLOC_MEMORY_MODE
```

这说明 DCL 对关键 ROS compatibility behavior 应显式配置，不依赖跨 minor mutable defaults。

### 4.4 2.10

- secure Discovery Server；
- discovery callback增强；
- incompatible type callback；
- 多处 RTPS/network ABI变化。

进一步证明：DMW 不应依赖 internal RTPS implementation ABI。

### 4.5 2.11

- ContentFilteredTopic/ABI 修复；
- ignore-local-endpoints；
- TypeLookup Service配置；
- discovery listener行为修正。

### 4.6 2.12 — Fast CDR 2 边界

2.12 开始支持 Fast CDR 2.x，是 generated-type/source compatibility 的明显分界。

关键点：

```text
Fast CDR 2.x support
    !=
default wire encoding must become XCDRv2
```

默认 representation仍以维持互操作为重要目标。

DCL 因此要分开验证：

```text
wire compatibility
    vs
source/generated-code compatibility
```

Fast CDR 不应进入 DMW 普通 public API。

### 4.7 2.13

- Monitor Service；
- thread settings；
- built-in transport配置增强；
- interface name/allowlist能力增强；
- DataRepresentationQos增强。

DCL 在需要兼容 2.6.x 时，不应无条件启用只有新版本才支持的 representation/transport feature。

### 4.8 2.14

2.14 主要继续增强：

- security handshake properties；
- transport output channel；
- netmask filtering；
- allowlist/blocklist；
- built-in transport configuration；
- TCP listener行为。

Fast DDS 2.14.x 是 DCL 当前现代 Fast DDS 参考线，但**它不是单独凌驾于 `rmw_fastrtps` Jazzy 之上的“唯一主基线”**。DMW 的新设计同时对照 Fast DDS 2.14.x 与 `rmw_fastrtps` Jazzy。

## 5. 2.6 与 2.14 差异总结

| 维度 | 2.6.x | 2.14.x | DCL 策略 |
| --- | --- | --- | --- |
| DDS-PIM API | 已成熟 | 持续扩展 | 依赖稳定 public API |
| ABI | minor间已有变化 | 多次继续变化 | 不承诺跨 minor单 binary |
| WaitSet/Condition | 支持 | 更成熟 | DMW WaitSet基础 |
| SHM/Data Sharing | 已存在 | 持续增强 | wire测试显式 UDPv4 |
| history memory default | 与后续不同 | 2.9+ realloc default | compatibility path显式配置 |
| Fast CDR | 1.x体系 | 2.x support | binding/generated code重点验证 |
| encoding | XCDRv1兼容路径 | representation更可控 | 不默认依赖新 encoding |
| discovery | 已成熟 | 继续扩展 | 只依赖稳定 listener/public data |
| RTPS internals | 可见但变化大 | 多次重构/private化 | DMW避免依赖 |
| transport config | 基础 | 明显增强 | V1不暴露全部 vendor feature |

## 6. DCL 的正式参考模型

### 6.1 两个平等主要参考基线

正式模型：

```text
Fast DDS 2.14.x ─────┐
                     ├── 对照 / 交叉验证 ──> DMW
rmw_fastrtps Jazzy ──┘
```

不是：

```text
Fast DDS -> rmw_fastrtps -> DMW
```

也不是反过来。

### 6.2 各参考的关注范围

| 上游 | 主要参考用途 |
| --- | --- |
| Fast DDS `2.14.x` | DDS API、Entity lifecycle、QoS、Condition/WaitSet、discovery、resource/error/teardown semantics |
| `rmw_fastrtps` Jazzy | Fast DDS生产使用、ROS naming/QoS、GID/MessageInfo、Service identity、availability、wait/discovery race、teardown pattern |
| `rcl` Jazzy | Context/Timer/WaitSet/GuardCondition/Graph 等 language-neutral runtime 边界 |
| `rcl_action` Jazzy | Action 3 Service + 2 Topic、Goal FSM、cancel/result/status common state、Action aggregate waitability |
| `rclcpp` Jazzy | C++ templates、Future/Promise、callback、Executor、exception presentation |
| `rclpy` Jazzy | Python Future/callback/Executor/asyncio/GIL |

关注范围不同不代表层级高低。

### 6.3 冲突处理

当 Fast DDS 与 `rmw_fastrtps` 的实现方式不同，DMW 依次评估：

1. Fast DDS public semantics/guarantees；
2. `rmw_fastrtps` observable ROS interoperability requirements；
3. DMW public contract和架构约束；

然后选择适合 DMW 的方案。

例如：

- DMW 不因为 `rmw_fastrtps` 支持 XML override就必须复制该功能；
- DMW 也不因为 Fast DDS 提供某个 vendor feature就自动进入 public API；
- Service response-reader wait则应吸收 `rmw_fastrtps` 使用 effective writer QoS `max_blocking_time` 的成熟模式。

## 7. Source compatibility 与 ABI

### 7.1 public RuntimeMode 不编码版本

DCL public API 保持：

```cpp
enum class RuntimeMode {
    DDS,
    ROS2
};
```

不出现：

```text
HUMBLE
JAZZY
FAST_DDS_2_6
FAST_DDS_2_14
```

发行版和 Fast DDS minor是 build/test environment，不是用户 runtime semantic。

### 7.2 source compatibility

目标：

```text
same DCL source
    ├── Fast DDS 2.14.x primary validation
    └── Fast DDS 2.6.x compatibility validation
```

### 7.3 不追求跨 minor binary ABI

Fast DDS 2.x minor之间存在 ABI break。

因此不要求：

```text
one libdmw.so
    -> load with Fast DDS 2.6
    -> load with Fast DDS 2.14
```

### 7.4 条件编译策略

优先让 production source使用共同 public API。

只有实际 2.6/2.14 build证明不可避免时，才在 private compatibility helper增加局部 version shim。

不要在 public header或业务代码大量散布 Fast DDS minor `#if`。

## 8. Fast CDR / generated types

### 8.1 高风险区域

重点验证：

- `TopicDataType`；
- Fast DDS-Gen generated code；
- ROSIDL Fast RTPS type support；
- string；
- sequence；
- nested type；
- bounded/unbounded sequence；
- keyed type。

### 8.2 public boundary

```text
DCL typed/Python message
        ↓
DMW MessageType
        ↓
Fast DDS TopicDataType
        ↓
Fast CDR
```

Fast CDR version-specific API不泄漏到普通 DMW public surface。

## 9. QoS 影响

跨版本默认值会变化，因此 ROS compatibility path对关键实现 policy显式设置。

当前参考 `rmw_fastrtps` Jazzy production pattern：

```text
history memory policy = PREALLOCATED_WITH_REALLOC
data sharing = off
writer publication mode default = synchronous
```

这些属于 Fast DDS implementation baseline，不等于新增 DMW public QoS policy。

DMW public common profiles单独冻结在 `dmw.md`。

## 10. WaitSet / Timer / Graph

### 10.1 WaitSet

Fast DDS 2.4+ 已有 WaitSet/GuardCondition/StatusCondition。

DCL 应直接利用 native primitives，并参考 Jazzy `rmw_wait` 的成熟模式：

```text
logical pre-check
attach conditions
native wait
wake
logical re-check
```

正常路径不再用固定 100 ms periodic slice掩盖 lost wakeup问题。

### 10.2 Timer

Timer不是 Fast DDS entity。

DMW 使用 monotonic scheduling state，并把 earliest Timer deadline合并进 WaitSet timeout。

### 10.3 Graph

DMW internal DiscoveryGraph同时服务：

```text
Service availability
Action availability
GraphSnapshot
GraphEvent
```

V1不尝试仅凭 DDS participant name恢复 ROS logical Node identity。

## 11. Service 版本风险

Service 比 Topic多出：

- request/reply naming；
- SampleIdentity；
- related sample identity；
- GUID；
- sequence；
- response reader match race。

因此必须单独测试：

```text
DCL Client -> ROS Server
ROS Client -> DCL Server
```

Jazzy `rmw_fastrtps` response path使用 effective response writer reliability QoS的 `max_blocking_time` 等待目标 response reader；DCL实现也采用这一成熟模式，而不冻结一个独立 100 ms public常量。

## 12. Action 版本风险

Action在 Service基础上再增加：

```text
3 Service + 2 Topic
GoalId
Goal FSM
cancel matching
feedback/status
pending result requests
terminal result retention/expiry
aggregate readiness
```

DMW的 common Action设计参考 `rcl_action` Jazzy，但不依赖 `rcl_action` runtime。

默认 endpoint QoS：

```text
Goal/Cancel/Result service -> ROS services default
Feedback -> ROS default
Status -> KeepLast(1), Reliable, TransientLocal
```

ActionServer result timeout参考 Jazzy default为 10 s，并由 DMW option authority定义。

## 13. Wire interoperability

### 13.1 Topic

至少验证：

```text
DCL -> ROS 2
ROS 2 -> DCL
```

并覆盖简单/复杂 message。

### 13.2 Service

至少验证：

```text
DCL Client -> ROS Server
ROS Client -> DCL Server
multiple clients
availability
response-reader race
```

### 13.3 Action

实现后验证：

```text
DCL ActionClient -> ROS ActionServer
ROS ActionClient -> DCL ActionServer
```

包括：

- accept/reject；
- execute/succeed/abort/cancel；
- feedback/status；
- result pending/terminal；
- result expiry；
- availability；
- multiple clients/goals。

### 13.4 网络路径

跨版本 interoperability test推荐：

```text
ROS_DOMAIN_ID=23
FASTDDS_BUILTIN_TRANSPORTS=UDPv4
```

只在 integration-test process设置 UDPv4，以确保测试 DDSI-RTPS wire path；普通开发 shell/build/test不全局强制 transport。

## 14. 验证优先级

按照当前 DMW 风险排序：

1. Fast DDS 2.14.x + `rmw_fastrtps` Jazzy baseline cross-audit；
2. WaitSet / GuardCondition race；
3. Service identity/response writer target wait；
4. generated type / Fast CDR compatibility；
5. Participant/listener/teardown；
6. QoS effective behavior；
7. Graph discovery normalization；
8. Timer scheduling；
9. Action aggregate runtime；
10. Humble/2.6 compatibility regression。

## 15. CMake 与 package 名称

Fast DDS 2.14.x upstream CMake project/package仍使用：

```text
fastrtps
```

因此 DCL不应仅因产品名为 Fast DDS就盲目改为未经确认的：

```cmake
find_package(fastdds ...)
```

Primary CI可以验证解析的实际 Fast DDS版本为 2.14.x；如果继续支持 Humble source compatibility，CMake minimum requirement不必强制提高到 2.14。

## 16. 结论

DCL 当前版本策略不是“从 2.6 全面迁移后抛弃旧环境”，也不是“继续让 Humble限制所有新设计”，而是：

```text
Fast DDS 2.14.x ─────┐
                     ├── equal reference cross-audit
rmw_fastrtps Jazzy ──┘
          │
          ▼
     modern DMW design
          │
          ├── Jazzy/2.14 primary validation
          └── Humble/2.6 compatibility validation
```

通过这一策略，DMW可以采用现代 Fast DDS/Jazzy 已验证的实现模式，同时保持源码层面的 Humble兼容，而不把版本差异扩散到 public API、dclcpp或dclpy。

## 17. 官方/上游资料

主要参考：

1. eProsima Fast DDS `2.14.x` source/release notes；
2. eProsima Fast DDS `2.6.x` source/release notes；
3. ROS 2 REP-2000；
4. `ros2/rmw_fastrtps` Jazzy；
5. `ros2/rmw` Jazzy QoS profiles；
6. `ros2/rcl` Jazzy；
7. `ros2/rcl_action` Jazzy；
8. `ros2/rclcpp` Jazzy；
9. `ros2/rclpy` Jazzy。
