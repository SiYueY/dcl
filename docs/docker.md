# Docker 开发与兼容性验证环境

本文档说明 DCL 仓库当前 Docker 开发/测试环境，以及它在新的 Fast DDS 2.14.x / `rmw_fastrtps` Jazzy 参考策略中的角色。

Docker 用于提供可重复的构建和 interoperability 环境，不替代宿主机日常开发，也不把 DCL 源码复制进镜像。

## 1. 环境定位

DCL 当前维护两套 ROS 2 验证环境：

| 环境 | Ubuntu | ROS 2 | Fast DDS line | 角色 | 入口 |
| --- | --- | --- | --- | --- | --- |
| Humble | 22.04 Jammy | Humble | 2.6.x | compatibility validation | `./docker/humble.sh` |
| Jazzy | 24.04 Noble | Jazzy | 2.14.x | primary modern validation | `./docker/jazzy.sh` |

这张表描述**验证角色**，不是 DMW reference hierarchy。

DMW 设计本身同时以：

```text
Fast DDS 2.14.x
+
rmw_fastrtps Jazzy
```

作为平等主要参考基线；Docker Jazzy 环境用于落实这套现代实现的主要 build/test/interoperability 验证。Humble 环境验证同一源码对 Fast DDS 2.6.x 的兼容性。

## 2. 镜像基线

wrapper 使用 ROS Desktop Full：

```text
Humble -> osrf/ros:humble-desktop-full
Jazzy  -> osrf/ros:jazzy-desktop-full
```

Desktop Full 提供完整 ROS 2 desktop/visualization/CLI 环境；DCL Dockerfile 只额外安装：

- build/debug tooling；
- workspace tooling；
- Fast DDS；
- `rmw_fastrtps_cpp`；
- ROSIDL Fast RTPS type support；
- integration-test 需要的 ROS messages；
- Xvfb 等开发辅助工具。

不在 Dockerfile 中长期 pin 具体 Debian revision；实际验证版本由 CI/test manifest 记录。

## 3. 目录结构

```text
docker/
├── Dockerfile
├── docker.sh
├── entrypoint.sh
├── humble.sh
├── jazzy.sh
└── cross-integration-test.sh
```

职责：

| 文件 | 职责 |
| --- | --- |
| `Dockerfile` | Humble/Jazzy 共用镜像定义 |
| `docker.sh` | 公共 launcher 实现 |
| `entrypoint.sh` | runtime UID/GID 与降权执行 |
| `humble.sh` | Humble 公开入口 |
| `jazzy.sh` | Jazzy 公开入口 |
| `cross-integration-test.sh` | Humble/Jazzy cross-version Topic/Service wire probe |

正常用户调用 wrapper，不直接传 `docker.sh --ros-distro ...` 内部参数。

## 4. 支持命令

两个发行版 wrapper 均支持：

```text
shell
build
test
integration-test
benchmark
rebuild
```

### 4.1 shell

```bash
./docker/humble.sh
./docker/jazzy.sh
```

无 command 时等价于 `shell`。

### 4.2 build

```bash
./docker/humble.sh build
./docker/jazzy.sh build
```

标准 build tree：

```text
build/docker/<distro>/dmw
```

配置：

```text
BUILD_TESTING=ON
DMW_ENABLE_DDS_INTEGRATION_TESTS=OFF
```

因此普通开发 build 不引入 ROS 2 DDS integration-test 环境差异。

### 4.3 test

```bash
./docker/humble.sh test
./docker/jazzy.sh test
```

`test` 是 self-contained：

```text
configure -> build -> ctest
```

使用标准 build tree，不开启 DDS integration tests。

### 4.4 integration-test

```bash
./docker/humble.sh integration-test
./docker/jazzy.sh integration-test
```

独立 build tree：

```text
build/docker/<distro>/dmw-integration
```

配置：

```text
BUILD_TESTING=ON
DMW_ENABLE_DDS_INTEGRATION_TESTS=ON
```

CTest 只运行：

```text
--label-regex integration
```

### 4.5 benchmark

```bash
./docker/humble.sh benchmark
./docker/jazzy.sh benchmark
```

benchmark 使用 integration build tree 构建当前 DMW foundation benchmark，然后直接运行 benchmark executable。

### 4.6 rebuild

```bash
./docker/humble.sh rebuild
./docker/jazzy.sh rebuild
```

显式重建镜像。普通 `shell/build/test/integration-test/benchmark` 只有在本地镜像不存在时自动 build image。

## 5. 工作区与构建隔离

仓库 bind mount 为：

```text
/workspace/dcl
```

宿主机实际仓库路径不做假设。

构建目录必须隔离：

```text
build/docker/humble/dmw
build/docker/humble/dmw-integration
build/docker/jazzy/dmw
build/docker/jazzy/dmw-integration
```

原因：CMake Cache 含编译器、依赖版本和绝对路径，不能在 Humble/Jazzy/宿主机构建间共享。

## 6. 容器 runtime

标准 container 使用：

```text
--rm
--init
--network host
```

并设置：

```text
HOME=/tmp/dcl-home
ROS_DOMAIN_ID=<validated value, default 23>
```

源码目录和临时 HOME 都通过 bind mount 提供。

### 6.1 UID/GID

launcher 把宿主 UID/GID 传给 `entrypoint.sh`：

```text
DCL_HOST_UID
DCL_HOST_GID
```

entrypoint 在容器中建立 disposable runtime identity 后降权执行，避免宿主构建目录生成 root-owned artifact。

## 7. DDS transport policy

这是当前 Docker 文档中最重要的区分：

> **普通 shell/build/test 不全局设置 `FASTDDS_BUILTIN_TRANSPORTS=UDPv4`。**

标准 container 只设置：

```text
ROS_DOMAIN_ID
```

Fast DDS transport 使用镜像/中间件正常配置。

只有：

```text
integration-test
benchmark
cross-integration-test
```

这类明确验证 DDS wire path 的流程通过 integration container/process 设置：

```text
FASTDDS_BUILTIN_TRANSPORTS=UDPv4
```

目的：

- 排除同机 SHM/Data Sharing 路径对 wire test 的干扰；
- 明确验证 UDPv4 / DDSI-RTPS interoperability；
- 不改变普通开发环境的 Fast DDS 行为。

## 8. ROS_DOMAIN_ID

默认：

```text
ROS_DOMAIN_ID=23
```

可覆盖：

```bash
ROS_DOMAIN_ID=42 ./docker/jazzy.sh integration-test
```

launcher 验证十进制 `0..232`。

Domain 23 是测试默认值，不属于 DCL protocol contract。

## 9. GUI

GUI 是 opt-in：

```bash
./docker/jazzy.sh --gui
```

启用后 launcher：

- 传递 `DISPLAY`；
- mount `/tmp/.X11-unix`；
- `/dev/dri` 存在时透传；
- 不自动执行 `xhost +`；
- 不默认引入 Xauthority 复制逻辑。

如果宿主 X server 本身拒绝授权，再针对实际授权失败单独处理，不预先扩大容器权限。

## 10. RMW

Dockerfile 固定：

```text
RMW_IMPLEMENTATION=rmw_fastrtps_cpp
```

这是因为 DCL 的 ROS 2 interoperability target就是 Fast DDS path；避免 base image默认 RMW 选择影响测试。

这不表示 DMW链接或依赖 ROS 2 RMW runtime。DMW仍直接使用 Fast DDS。

## 11. Humble/Jazzy cross-version test

运行：

```bash
./docker/cross-integration-test.sh
```

当前 script：

1. 分别准备 Humble/Jazzy integration build；
2. 强制 peer process使用 UDPv4；
3. 验证：

```text
Humble Publisher -> Jazzy Subscriber
Jazzy Publisher  -> Humble Subscriber
Humble Client    -> Jazzy Server
Jazzy Client     -> Humble Server
```

当前 cross script 只覆盖 Topic/Service。

Action common runtime实现后，应在同一机制增加 ActionClient/ActionServer cross-version probe；在实现落地前文档不声称 cross Action 已验证。

## 12. Primary / compatibility validation

### 12.1 Jazzy

Jazzy环境是 DMW 新实现的主要验证环境，特别用于：

- Fast DDS 2.14.x API/build；
- `rmw_fastrtps` Jazzy interoperability；
- WaitSet/GuardCondition race regression；
- Service response reader behavior；
- 新 Timer/Graph/Action integration tests。

这不改变 Fast DDS 2.14.x 与 `rmw_fastrtps` Jazzy 在设计参考上的平等地位。

### 12.2 Humble

Humble环境验证：

- 2.6.x source compatibility；
- Topic/Service wire compatibility；
- private compatibility shim 是否足够；
- 新功能是否意外依赖 2.14-only API。

不因为 Humble 缺少某个现代 convenience API 就修改 DMW public contract；优先在 private implementation中兼容。

## 13. Baseline manifest

正式 CI/interoperability report 建议至少记录：

```text
ROS distro
Ubuntu image/tag/digest
Fast DDS version
Fast CDR version
rmw_fastrtps package revision
rosidl_typesupport_fastrtps package revision
compiler version
architecture
Git commit
ROS_DOMAIN_ID
transport override used by integration test
```

这样“Jazzy”或“2.14.x”只是支持线标签，具体一次验证仍可复现。

## 14. 与 DMW 设计的关系

Docker 是验证工具，不是 public runtime contract。

例如：

```text
integration test sets UDPv4
```

不意味着：

```text
DMW production runtime only supports UDPv4
```

同理：

```text
Docker uses rmw_fastrtps_cpp for ROS peer
```

不意味着：

```text
DMW depends on rmw_fastrtps
```

DMW public/implementation contract分别以 `dmw/docs/dmw.md` 和 `dmw/docs/dmw_fastdds.md` 为准。

## 15. 推荐日常流程

新 DMW 修改：

```bash
./docker/jazzy.sh build
./docker/jazzy.sh test
./docker/jazzy.sh integration-test
```

兼容性回归：

```bash
./docker/humble.sh build
./docker/humble.sh test
./docker/humble.sh integration-test
```

跨版本 wire probe：

```bash
./docker/cross-integration-test.sh
```

涉及性能时：

```bash
./docker/jazzy.sh benchmark
```

## 16. 非目标

当前 Docker 环境不引入：

- Docker Compose；
- devcontainer；
- privileged mode；
- NVIDIA runtime；
- Wayland forwarding；
- 自动 `xhost +`；
- 多套 profile framework；
- 单独 headless mode flag。

有实际需求再扩展。

## 17. 总结

Docker 环境当前承担三个职责：

```text
reproducible build
+
Jazzy/2.14 modern validation
+
Humble/2.6 compatibility/wire regression
```

最重要的环境边界是：普通开发不强制 UDPv4；只有 integration/benchmark/cross-wire 测试显式固定 UDPv4。这样既保持日常环境接近真实 Fast DDS default，又让 interoperability test 有清晰、稳定的数据路径。
