# Fast DDS 版本演进与兼容性说明

## 1. 文档目的

本文用于梳理 eProsima Fast DDS 从早期 Fast RTPS 1.x 到 Fast DDS 2.x 的主要版本变化，并重点分析 ROS 2 Humble 与 Jazzy 所对应的 Fast DDS 版本线之间可能影响 DCL 的兼容性差异。

本文不试图复制完整 Release Notes，而是关注以下问题：

- Fast RTPS / Fast DDS 的 API、ABI 和行为如何演进；
- DDS-PIM、RTPS、传输、发现、QoS、类型系统和序列化相关的重要变化；
- Fast CDR 与 Fast DDS-Gen 的版本迁移影响；
- ROS 2 Humble / Jazzy 所使用 Fast DDS 基线之间的关键差异；
- 哪些差异可能要求 DCL 修改源码，哪些只需要通过构建和互操作测试验证。

## 2. 版本号说明

需要先区分 Fast DDS upstream 版本与 ROS / Debian 软件包版本号。

根据 eProsima 官方 `versions.md`：

- 1.x 主线中记录了 `1.6.0`，随后进入 `1.7.x`；
- 未在官方版本记录中找到 `1.6.9`；
- Fast DDS 2.6 官方维护文档当前记录到 `2.6.12`；
- 未在官方 upstream 版本资料中找到 `2.6.14`。

因此，如果系统软件包中出现类似 `1.6.9` 或 `2.6.14` 的字符串，需要进一步确认它代表的是：

- upstream Fast DDS 版本；
- ROS Debian package revision；
- Ubuntu package revision；
- 其他组件（例如 Fast CDR、Fast DDS-Gen）的版本。

本文以下使用 eProsima upstream 官方版本号。

对于 DCL 当前 ROS 2 兼容目标，更重要的官方基线是：

| ROS 2 | Ubuntu Tier 1 | Fast DDS 基线 |
| --- | --- | --- |
| Humble Hawksbill | Ubuntu 22.04 Jammy | 2.6.x |
| Jazzy Jalisco | Ubuntu 24.04 Noble | 2.14.0 |

REP-2000 中的版本是 ROS 2 发行时的低水位基线，发行周期内补丁版本可能继续更新。

## 3. 1.6 到 2.6 的主要演进

### 3.1 Fast RTPS 1.6

Fast RTPS 1.6.0 的核心新增能力包括：

- Persistence；
- Security Access Control Plugin API；
- 内置 Access Permissions Plugin。

这一阶段仍然以 Fast RTPS 自身的 Publisher / Subscriber API 和 RTPS 能力为主。

### 3.2 Fast RTPS 1.7

1.7.x 是一次明显的能力扩展：

- TCP transport；
- Dynamic Types；
- DDS Security 1.1；
- `FASTRTPS_DEFAULT_PROFILES_FILE`；
- 更严格的 XML profile 解析；
- discovery、TCP、key-only data 等多项修复。

从 1.7 开始，类型生成和运行时行为已经与 1.6 存在明显差异。

eProsima 在后续 2.6 Release Notes 中明确指出：

> 从早于 1.7.0 的版本升级时，必须使用新的代码生成工具重新生成 IDL 生成代码。

因此，1.6 与现代 Fast DDS 之间不能假定生成类型源码可以直接复用。

### 3.3 Fast RTPS 1.8

1.8.x 继续补齐 DDS 能力：

- IDL 4.2；
- Deadline QoS；
- Lifespan QoS；
- Disable Positive ACKs；
- Liveliness QoS；
- TCP TLS；
- Best-Effort writer 非阻塞写；
- 更严格的资源限制和实时相关数据结构；
- Domain ID 限制；
- UDP non-blocking send 可配置。

这一阶段开始形成后来 Fast DDS DDS-PIM QoS 能力的基础。

### 3.4 Fast RTPS 1.9

1.9.x 的主要变化：

- Discovery Server；
- allocation QoS 的初步实现；
- non-blocking calls；
- Fast-RTPS-Gen 从主仓库拆分为独立项目；
- Intra-process delivery；
- participant discovery filtering；
- `STRICT_REALTIME`；
- 多项 discovery、reliability、liveliness 和并发修复。

从 1.9.4 开始，Intra-process delivery 默认开启。

### 3.5 Fast RTPS 1.10

1.10.0 是传输层的重要节点：

- 引入 Shared Memory Transport；
- Transport API 重构；
- 支持完全避免动态分配的配置参数；
- built-in endpoint history 可配置；
- Fast CDR 升级至 1.0.13；
- Fast-RTPS-Gen 升级至 1.0.4。

这为 Fast DDS 2.x 中 SHM 默认启用以及后续 Data Sharing 奠定了基础。

### 3.6 Fast DDS 2.0

2.0 是最重要的架构分界点之一。

主要变化：

- 引入符合 DDS 1.4 的 DDS API；
- Fast RTPS 旧高层 API 开始进入兼容 / 弃用阶段；
- RTPS participant 创建 API 调整；
- domain ID 从 RTPS participant attributes 调整到更高层 participant attributes；
- Fast CDR 升级；
- Shared Memory transport 在 2.0.2 默认启用；
- Discovery Server 大规模降低 discovery traffic；
- Fast DDS CLI 出现。

对于现代代码，新 DDS-PIM API 应视为主要公共接口，旧 Fast RTPS Publisher / Subscriber API 不应作为新的兼容层基础。

### 3.7 Fast DDS 2.1

2.1 对 DDS-PIM 和 RTPS API 继续扩展：

- DDS-PIM 和 RTPS 层均出现 ABI break；
- 增加 incompatible QoS 相关回调；
- persistence 能力增强；
- XML QoS 查询 API 增强。

值得注意的是：

- API 可以保持 source-compatible；
- 但动态库 ABI 并不保证跨 minor 保持不变。

因此 DCL 应采用“同一份源码分别针对目标 Fast DDS 编译”的策略，而不是尝试提供一个同时兼容多个 Fast DDS minor 的二进制。

### 3.8 Fast DDS 2.2

2.2 是 DDS 高层 API 的另一个重要节点：

- `TopicDataType` interface 扩展，产生 ABI break；
- DataWriter loan sample API；
- loanable sequence；
- DataReader `read` / `take` API；
- 完整 DDS traditional C++ API；
- Data Sharing delivery。

这意味着依赖 `TopicDataType` 的用户类型适配层是跨版本兼容时需要重点验证的位置。

### 3.9 Fast DDS 2.3

主要变化：

- Fast DDS Statistics；
- Discovery Super Client；
- unique network flows；
- 获取 writer / reader locator 的 API；
- `SampleInfo` 增加 reception timestamp；
- `DataReader::get_unread_count()`；
- ReturnCode 相关 ABI 调整。

### 3.10 Fast DDS 2.4

主要变化：

- WaitSet；
- GuardCondition；
- StatusCondition；
- Flow Controllers；
- runtime 增加 Discovery Server；
- Data Sharing 文件目录可配置。

对 DCL 很重要的一点是：现代 Fast DDS 的标准 WaitSet / Condition 能力从该阶段已经形成。

因此 DCL 可以基于：

```text
Fast DDS callback / status
        ↓
Condition / WaitSet
        ↓
DCL Executor
        ↓
user callback
```

避免直接在 Fast DDS listener 线程执行用户回调。

### 3.11 Fast DDS 2.5

主要变化：

- zero-valued `InstanceHandle_t`；
- transport concatenation；
- 从字符串加载 XML profiles；
- 针对单个 instance 等待 ACK；
- entity 创建阶段生成 GUID；
- DataReader history 对 `instance_state` / `view_state` 的实现调整。

### 3.12 Fast DDS 2.6

2.6.0 官方说明：

- 与前一 minor 保持 API 兼容；
- DDS-PIM 和 RTPS 层都存在 ABI break；
- 部分 API 被弃用。

主要变化包括：

- TransportInterface / NetworkFactory 支持运行时更新网络接口；
- endpoint discovery API；
- content filter discovery API；
- `fastdds::Time_t` API 扩展并逐步替代 RTPS 层 `Time_t`；
- DataWriter / DataReader API 持续扩展。

2.6.1 进一步增加：

- writer-side content filtering；
- `DataWriter::get_key_value()`；
- `DataReader::lookup_instance()`；
- `SampleLostStatus`。

2.6 后续 patch 主要以稳定性、安全性和 bug fix 为主。当前官方 2.6 文档的维护版本为 2.6.12，并明确说明 2.6 minor 仅继续接收 critical issue 和 security fix。

## 4. Humble 2.6.x 到 Jazzy 2.14.x 的关键变化

DCL 当前真正需要重点评估的是 2.6.x → 2.14.x，而不是每一个历史版本的全部新增功能。

### 4.1 2.7

主要变化：

- DDS `ReadCondition`；
- `find_topic()`；
- timestamp writer APIs；
- `SampleRejectedStatus`；
- transport、RTPS history 等多处 ABI break。

对 DCL：

- 如果只使用 DDS-PIM 中稳定的 Participant / Publisher / Subscriber / DataWriter / DataReader / WaitSet API，影响有限；
- 不应依赖内部 RTPS history 或 transport implementation class 的 ABI。

### 4.2 2.8

主要变化：

- transport WAN address API；
- Property QoS `propagate`；
- Ownership / Ownership Strength XML 配置；
- TLS-TCP SNI；
- external locator 配置发生 ABI change。

对 DCL 当前默认 UDPv4 ROS 2 互操作路径影响较小。

### 4.3 2.9

最重要的行为变化：

> 默认 history memory management policy 改为 `PREALLOCATED_WITH_REALLOC_MEMORY_MODE`。

这是行为变化而不是单纯新增 API。

DCL 在 ROS 2 compatibility mode 中如果显式设置 history memory policy，则不应依赖不同 Fast DDS minor 的默认值。

这也是 DCL 应坚持“关键 QoS 显式配置”的原因。

### 4.4 2.10

2.10 包含较多 ABI change：

- secure Discovery Server；
- RTPSWriter virtual API；
- network headers private；
- RTPS discovery callback；
- DDS discovery callback；
- incompatible type callback。

对 DCL：

- 应避免使用 Fast DDS internal / RTPS implementation API；
- listener callback 类型和行为需要通过实际编译验证；
- `DomainParticipantListener` 之类 API 不应成为 DCL 公共 API 的一部分。

### 4.5 2.11

主要变化：

- ContentFilteredTopic exported symbols 修复导致 ABI break；
- participant ignore-local-endpoints；
- TypeLookup Service XML 配置；
- discovery listener 行为修正。

这再次说明 Fast DDS 2.x minor 之间不能假设二进制 ABI 稳定。

### 4.6 2.12：Fast CDR 2 的分界点

2.12 是对 DCL 最值得关注的版本之一。

官方说明：

- Fast DDS 升级支持 Fast CDR 2.0.0；
- 默认 encoding 仍保持 XCDRv1，以维持与旧 Fast DDS 的互操作；
- 使用 Fast DDS-Gen v2 生成的源码建议使用 Fast DDS-Gen v3 重新生成；
- 存在一个 `MEMBER_INVALID` 从宏变为 namespaced `constexpr` 的 API break。

关键结论：

```text
Fast CDR 2.x
    ≠
默认 wire encoding 改为 XCDRv2
```

Fast DDS 2.12 仍默认 XCDRv1，因此 Fast DDS 2.6 与 2.12+ 在默认数据表示下仍以保持 wire interoperability 为目标。

但是，**生成类型源码的 C++ API** 已经发生变化。

因此应把兼容性拆成两个问题：

```text
Wire compatibility
    XCDRv1 / DDSI-RTPS

Source compatibility
    Fast CDR API
    Fast DDS-Gen generated code
```

DCL 的 `dmw` core 不应直接依赖 Fast CDR API。

Fast CDR 差异应尽可能限制在：

- Fast DDS-Gen 生成类型；
- ROSIDL Fast RTPS typesupport；
- 测试 adapter。

### 4.7 2.13

主要变化：

- Monitor Service；
- 所有线程的 thread setting 配置；
- builtin transport 配置扩展；
- interface whitelist 支持接口名称；
- TCP transport 配置增强；
- `DataRepresentationQos` 支持选择 CDR encoding。

`DataRepresentationQos` 对跨版本测试尤其重要，因为 2.13 开始用户可以更直接地控制 representation。

DCL 当前如果目标是兼容 Humble 2.6.x，应避免默认启用只有新版本才支持的 XCDRv2-only 行为。

### 4.8 2.14

2.14 的主要新增集中在 transport / security：

- authentication handshake properties；
- transport output channel API；
- netmask filter；
- allowlist / blocklist；
- builtin transport 更多配置项；
- TCP server listening port 行为调整。

这些变化对 DCL 当前核心 DDS-PIM API 的影响相对有限。

Fast DDS 2.14 是 Fast DDS 2.x 的最后一个 minor 版本线。

## 5. 2.6 与 2.14 的差异总结

| 维度 | Fast DDS 2.6.x | Fast DDS 2.14.x | DCL 影响 |
| --- | --- | --- | --- |
| DDS-PIM 基础 API | 已较完整 | 继续扩展 | 应优先使用稳定公共 DDS API |
| ABI | minor 间已有 break | 2.7～2.14 多次继续 break | 不提供跨 minor 单一二进制 |
| RTPS internal API | 可用但变化较大 | 多次重构 / private 化 | DCL core 应避免依赖 |
| WaitSet / Condition | 已支持 | 持续支持 | 可作为 Executor 基础 |
| SHM | 默认 transport 之一 | 持续增强 | 跨版本测试建议显式 UDPv4 |
| Data Sharing | 已存在 | 持续演进 | ROS compatibility mode 可显式关闭 |
| History memory 默认值 | 2.6 时代默认值与后续不同 | 2.9 后默认 PREALLOCATED_WITH_REALLOC | 关键 QoS 应显式设置 |
| Fast CDR | Fast CDR 1.x 体系 | 2.12+ 支持 Fast CDR 2.x | 类型 adapter / generated code 是高风险点 |
| 默认 encoding | XCDRv1 | 2.12+ 仍默认 XCDRv1 | 有利于 wire compatibility |
| DataRepresentationQos | 较早 API | 2.13 完善选择 encoding | 不应默认依赖新 representation |
| Fast DDS-Gen | 旧生成代码体系 | 推荐 Gen v3 | generated type 必须双环境验证 |
| Type system | XTypes 能力已存在 | TypeLookup / TypeObject 持续增强 | ROSIDL / TypeObject interop 需重点测试 |
| Transport config | 基础 UDP/TCP/SHM | interface / allowlist / builtin transport 配置显著增强 | DCL 默认 UDPv4 可减少差异 |
| Discovery | Discovery Server 已成熟 | secure / monitor /配置继续增强 | Simple discovery 基本兼容 |
| Security | 已支持 | 持续增强 | 当前非 DCL V1 核心 |

## 6. 对 DCL 的直接结论

### 6.1 不应以 Fast DDS minor 版本作为公共运行时模式

DCL 公共 API 应继续保持：

```cpp
enum class RuntimeMode {
    DDS,
    ROS2
};
```

不应出现：

```cpp
HUMBLE
JAZZY
FAST_DDS_2_6
FAST_DDS_2_14
```

ROS distro 和 Fast DDS minor 是构建 / 测试环境，不是 DCL 用户语义。

### 6.2 目标应是源码兼容，而不是 ABI 兼容

推荐模型：

```text
same DCL source
    ├── build against Humble / Fast DDS 2.6.x
    └── build against Jazzy  / Fast DDS 2.14.x
```

而不是：

```text
one DCL binary
    ├── Fast DDS 2.6
    └── Fast DDS 2.14
```

Fast DDS 官方版本记录已经明确指出多个 2.x minor 存在 ABI break。

### 6.3 DMW core 应尽量保持零版本条件编译

优先目标：

```text
dmw production source
    Fast DDS 2.6.x  ✅
    Fast DDS 2.14.x ✅
    version #ifdef  0
```

不要预先加入：

```cpp
#ifdef ROS_HUMBLE
#ifdef ROS_JAZZY
#if FASTDDS_VERSION_MINOR ...
```

只有实际双环境编译证明某个 API 无法通过公共写法兼容时，才考虑非常局部的 compatibility shim。

### 6.4 优先使用 DDS-PIM 稳定公共 API

DCL 应重点依赖：

- `DomainParticipantFactory`;
- `DomainParticipant`;
- `Publisher`;
- `Subscriber`;
- `DataWriter`;
- `DataReader`;
- `Topic`;
- `TopicDataType`;
- `TypeSupport`;
- `WaitSet`;
- `Condition`;
- DDS QoS 类型。

尽量避免：

- internal discovery implementation；
- private network headers；
- RTPS writer / reader implementation；
- history implementation；
- transport implementation internals。

2.7～2.14 的变化记录表明，ABI break 最频繁的区域正是 RTPS 和 transport implementation 层。

### 6.5 Fast CDR / generated type 是最高优先级兼容测试点之一

2.12 引入 Fast CDR 2 是明显的版本边界。

DCL 应确保：

```text
dmw public API
    ↓
type-erased message interface
    ↓
Fast DDS TopicDataType adapter
    ↓
generated / ROS type support
```

Fast CDR 不应泄漏到 dmw 公共 API。

需要重点验证：

- Fast DDS-Gen 生成类型在 Humble / Jazzy 分别编译；
- ROSIDL `rosidl_typesupport_fastrtps_cpp`；
- string；
- sequence；
- nested type；
- bounded / unbounded sequence；
- keyed type。

### 6.6 DDS wire interoperability 必须通过实际通信测试确认

官方版本记录说明默认 XCDRv1 被保留以维持旧版本互操作，但这不能代替 DCL 的实际验证。

至少需要测试：

```text
Humble DCL Publisher
        ↓
Jazzy ROS 2 Subscriber

Jazzy DCL Publisher
        ↓
Humble ROS 2 Subscriber

Humble ROS 2 Publisher
        ↓
Jazzy DCL Subscriber

Jazzy ROS 2 Publisher
        ↓
Humble DCL Subscriber
```

基础类型之后应增加复杂类型，例如：

```text
std_msgs/String
sensor_msgs/JointState
自定义 nested + sequence message
```

跨版本测试建议固定：

```text
ROS_DOMAIN_ID=23
FASTDDS_BUILTIN_TRANSPORTS=UDPv4
```

这样测试的是 UDP / DDSI-RTPS wire path，而不是同机 SHM。

### 6.7 Service 兼容风险高于 Topic

Topic 互操作通过后，还不能直接认为 ROS 2 service 一定兼容。

Service 额外涉及：

- request/reply topic naming；
- sample identity；
- related sample identity；
- GUID；
- sequence number；
- ROS 2 `rmw_fastrtps_cpp` request/reply mapping。

因此后续需要单独验证：

```text
DCL Client → Humble/Jazzy ROS 2 Service
ROS 2 Client → Humble/Jazzy DCL Server
```

以及必要的 Humble ↔ Jazzy 交叉组合。

## 7. 当前兼容性验证重点

按照风险从高到低，建议后续重点检查：

1. Fast DDS-Gen / Fast CDR 1.x 与 2.x 的 generated type API；
2. ROSIDL Fast RTPS typesupport；
3. `TopicDataType` 在 2.6 与 2.14 的 source compatibility；
4. TypeObject / TypeInformation 注册行为；
5. explicit QoS 与两个版本默认值之间的差异；
6. ROS 2 request/reply identity；
7. WaitSet / Condition；
8. transport 和 discovery 的默认行为。

如果 DCL 只依赖稳定 DDS-PIM API，并显式设置关键 QoS，大部分 2.6 → 2.14 的 Fast DDS 内部 ABI 变化都不应该传播到 DCL 公共接口。

## 8. 官方资料

本文主要依据以下官方资料：

1. eProsima Fast DDS `versions.md`  
   https://github.com/eProsima/Fast-DDS/blob/master/versions.md

2. Fast DDS 2.14 Release Notes  
   https://fast-dds.docs.eprosima.com/en/2.14.x/notes/notes.html

3. Fast DDS 2.6 Release Notes  
   https://fast-dds.docs.eprosima.com/en/2.6.x/notes/notes.html

4. ROS REP-2000 — ROS 2 Releases and Target Platforms  
   https://www.ros.org/reps/rep-2000.html

## 9. 后续工作

本文是版本差异的资料基线，不代表 DCL 已完成兼容性验证。

下一步应基于 DCL 实际使用的 Fast DDS API 建立一份更具体的 compatibility audit：

```text
DCL Fast DDS API usage
        ↓
Fast DDS 2.6 header/API
        ↓
Fast DDS 2.14 header/API
        ↓
source compatibility
        ↓
Humble/Jazzy build
        ↓
wire interoperability tests
```

只有实际代码审查、双版本构建和双向通信测试全部通过后，才能确认 DCL 对 Humble / Jazzy 的兼容边界。
