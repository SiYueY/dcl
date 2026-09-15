# Docker 开发与兼容性验证环境

本文档说明 DCL 仓库中的 Docker 开发环境，包括 ROS 2 Humble 与 Jazzy 的构建、测试及 DDS 互操作配置。

Docker 环境用于提供稳定、可重复的 ROS 2 / Fast DDS 验证基线，不替代宿主机日常开发环境，也不将 DCL 源码打包进镜像。

## 1. 设计目标

DCL 当前需要同时验证两套 ROS 2 / Fast DDS 环境：

| 环境 | Ubuntu | ROS 2 | Fast DDS 基线 | 入口 |
| --- | --- | --- | --- | --- |
| Humble | 22.04 Jammy | Humble | 2.6.x | `./docker/humble.sh` |
| Jazzy | 24.04 Noble | Jazzy | 2.14.x | `./docker/jazzy.sh` |

Docker 环境承担以下职责：

- 为同一份 DCL/DMW 源码提供独立的 Humble 与 Jazzy 构建环境；
- 验证 DMW 在两套 Fast DDS 2.x 基线上的源码兼容性；
- 运行 DMW 单元测试、构建测试及 ROS 2 互操作测试；
- 为 Humble 与 Jazzy 之间的 DDS 通信测试提供统一网络条件；
- 为后续 CI 提供与本地开发一致的验证入口。

当前目标是**源码级兼容**。Humble 与 Jazzy 分别使用各自环境中的 Fast DDS，不要求同一个预编译 DMW 二进制同时兼容两个 Fast DDS ABI。

## 2. 目录结构

```text
DCL/
├── docker/
│   ├── Dockerfile
│   ├── docker.sh
│   ├── entrypoint.sh
│   ├── cross-integration-test.sh
│   ├── humble.sh
│   └── jazzy.sh
└── docs/
    └── docker.md
```

各文件职责如下：

| 文件 | 职责 |
| --- | --- |
| `docker/Dockerfile` | 定义 Humble/Jazzy 共用的开发镜像内容 |
| `docker/docker.sh` | Docker 公共实现，负责镜像、容器、构建和测试 |
| `docker/entrypoint.sh` | 在容器内映射宿主 UID/GID，随后降权执行请求命令 |
| `docker/cross-integration-test.sh` | 构建两个发行版的 peer，并验证 Humble/Jazzy 的 DDS wire 互操作 |
| `docker/humble.sh` | Humble / Ubuntu 22.04 公开入口 |
| `docker/jazzy.sh` | Jazzy / Ubuntu 24.04 公开入口 |
| `docs/docker.md` | Docker 环境设计与使用说明 |

`docker.sh` 属于内部公共实现。正常使用时应调用 `humble.sh` 或 `jazzy.sh`，不需要直接向 `docker.sh` 传递 `--ros-distro`、`--base-image` 等内部参数。

## 3. 基本使用

首次使用前确保脚本具有执行权限：

```bash
chmod +x docker/docker.sh docker/humble.sh docker/jazzy.sh
```

### 进入开发环境

```bash
./docker/humble.sh
./docker/jazzy.sh
```

无参数时默认执行 `shell`，进入对应 ROS 2 环境的交互式 Bash。

### 构建 DMW

```bash
./docker/humble.sh build
./docker/jazzy.sh build
```

`build` 会依次执行 CMake configure 和 compile。

### 运行测试

```bash
./docker/humble.sh test
./docker/jazzy.sh test
```

`test` 是自包含操作，会依次执行：

```text
configure → build → ctest
```

不要求先单独执行 `build`。

### 重建镜像

当 `Dockerfile`、基础镜像或镜像依赖发生变化时执行：

```bash
./docker/humble.sh rebuild
./docker/jazzy.sh rebuild
```

普通 `shell`、`build`、`test` 只在对应镜像不存在时自动构建镜像，不会每次强制执行 `docker build`。

### 查看帮助

```bash
./docker/humble.sh --help
./docker/jazzy.sh --help
```

## 4. 工作区与构建目录

宿主机仓库位置不做任何假设。无论 DCL 位于：

```text
~/workspace/dcl
~/projects/dcl
/data/src/dcl
```

脚本都会根据自身位置解析实际仓库根目录，并挂载到容器中的固定路径：

```text
/workspace/dcl
```

因此容器内部目录始终为：

```text
/workspace/
└── dcl/
    ├── dclcpp/
    ├── dclpy/
    ├── dmw/
    ├── docker/
    ├── docs/
    └── ...
```

`/workspace` 保留为容器内的项目父目录。需要临时添加或并行开发其他源码时，可将其挂载为 `/workspace/<project>`，而不改变 DCL 的固定工作目录。

Docker 构建目录与宿主机构建目录隔离：

```text
build/
└── docker/
    ├── humble/
    │   └── dmw/
    └── jazzy/
        └── dmw/
```

对应路径为：

```text
build/docker/humble/dmw
build/docker/jazzy/dmw
```

不得让 Humble、Jazzy 或宿主机构建共享同一个 CMake build tree。CMake Cache 包含编译器、依赖和绝对路径信息，跨环境复用容易造成错误配置或不可预测的链接结果。

## 5. DDS 网络默认配置

Docker 环境默认就是 DDS 互操作环境，不额外提供 `dds` 模式。

每个容器默认使用：

```text
Docker network:             host
ROS_DOMAIN_ID:              23
Fast DDS builtin transport: UDPv4
RMW implementation:         rmw_fastrtps_cpp
```

### Host network

容器使用：

```text
--network host
```

这是有意的设计。DCL 当前主要运行在 Linux 上，DDS discovery 和 RTPS 通信应尽量避免受到 Docker bridge、NAT、multicast 转发和 locator 地址转换的额外影响。

Humble 与 Jazzy 容器因此直接使用同一宿主机网络栈，可以在相同 ROS Domain 中发现彼此。

### UDPv4

脚本默认设置：

```text
FASTDDS_BUILTIN_TRANSPORTS=UDPv4
```

跨 Fast DDS 版本测试的核心目标是验证 DDSI-RTPS 网络通信，而不是验证 Docker 容器间共享内存。因此默认不共享宿主机 IPC，也不将 SHM 作为测试数据路径。

这使 Humble/Fast DDS 2.6 与 Jazzy/Fast DDS 2.14 之间的通信路径更明确：

```text
Humble / Fast DDS 2.6
        │
        │ UDPv4 / DDSI-RTPS
        │
Jazzy / Fast DDS 2.14
```

### ROS_DOMAIN_ID

默认值为：

```text
ROS_DOMAIN_ID=23
```

`23` 是 DCL Docker 环境的默认测试 Domain ID。它同时保留一个小彩蛋：Humble 的 Ubuntu 基线是 22.04，Jazzy 的 Ubuntu 基线是 24.04，二者中间值为 23。

该值不是 DCL 协议约束，可以通过宿主机环境变量覆盖：

```bash
ROS_DOMAIN_ID=42 ./docker/humble.sh
ROS_DOMAIN_ID=42 ./docker/jazzy.sh
```

脚本会验证 Domain ID，并接受 `0..232` 范围内的十进制整数。

## 6. Humble 与 Jazzy DDS 通信测试

默认配置允许两个发行版容器同时运行并参与同一个 DDS Domain。

终端一启动 Humble：

```bash
./docker/humble.sh
```

终端二启动 Jazzy：

```bash
./docker/jazzy.sh
```

两个环境默认均使用 Domain 23 和 UDPv4，因此无需额外 Docker 网络配置。

最基础的验证可以先使用 ROS 2 Topic。例如在 Humble 中发布：

```bash
ros2 topic pub \
    /dcl_test \
    std_msgs/msg/String \
    '{data: "hello from humble"}'
```

在 Jazzy 中订阅：

```bash
ros2 topic echo /dcl_test std_msgs/msg/String
```

随后应反向执行一次 Jazzy Publisher → Humble Subscriber。

DCL 后续的跨发行版互操作验证应至少覆盖：

```text
DCL Humble Publisher  → ROS 2 Jazzy Subscriber
ROS 2 Humble Publisher → DCL Jazzy Subscriber
DCL Jazzy Publisher   → ROS 2 Humble Subscriber
ROS 2 Jazzy Publisher → DCL Humble Subscriber

DCL Humble Client      → ROS 2 Jazzy Service
ROS 2 Humble Client    → DCL Jazzy Server
DCL Jazzy Client       → ROS 2 Humble Service
ROS 2 Jazzy Client     → DCL Humble Server
```

跨 Humble/Jazzy 测试用于评估 Fast DDS 2.6 与 2.14 的实际 wire interoperability。除非项目后续明确冻结并持续验证该矩阵，否则它应视为兼容性验证结果，而不是对 ROS 2 跨发行版兼容性的无条件承诺。

仓库提供可重复执行的 DMW wire-level 验证：

```bash
./docker/cross-integration-test.sh
```

脚本先分别运行 Humble 与 Jazzy 的完整 DMW 集成测试，然后在 host network、同一
`ROS_DOMAIN_ID` 和 `FASTDDS_BUILTIN_TRANSPORTS=UDPv4` 条件下运行四个独立进程对：

```text
Humble Publisher → Jazzy Subscriber
Jazzy Publisher  → Humble Subscriber
Humble Client    → Jazzy Server
Jazzy Client     → Humble Server
```

Topic peer 验证可靠写入和接收；Service peer 验证服务发现、request/reply wire mapping 与
request identity correlation。它们使用两个发行版各自编译的 DMW 二进制，因此覆盖 Fast DDS
2.6.x 与 2.14.x 之间的实际网络路径，而不是同一进程内的模拟。

## 7. 镜像内容与运行时职责

`Dockerfile` 只定义稳定的软件环境，不保存项目源码，也不编码宿主机特定状态。

当前镜像安装的主要依赖包括：

```text
build-essential
cmake
ninja-build
ros-${ROS_DISTRO}-fastrtps
ros-${ROS_DISTRO}-rclcpp
ros-${ROS_DISTRO}-rmw-fastrtps-cpp
ros-${ROS_DISTRO}-rosidl-typesupport-fastrtps-cpp
ros-${ROS_DISTRO}-std-msgs
```

ROS 侧默认使用：

```text
RMW_IMPLEMENTATION=rmw_fastrtps_cpp
```

Dockerfile 不负责以下内容：

```text
ROS_DOMAIN_ID
FASTDDS_BUILTIN_TRANSPORTS
host network
宿主机 UID/GID
源码挂载
临时 HOME
```

这些属于容器运行策略，由 `docker.sh` 统一设置。

源码通过 bind mount 进入 `/workspace/dcl`。`docker.sh` 将宿主机当前用户的 UID/GID 传给容器 entry point；entry point 仅在本次可删除容器的账号数据库中补齐缺失的 passwd/group 条目，随后以该 UID/GID 执行请求命令。这样既避免 Docker 构建产物在宿主机上变成 root 所有，也保证 `whoami`、`groups` 等名称查询可用。它不会挂载宿主机的 `/etc/passwd` 或 `/etc/group`，也不会修改宿主机账号数据。

每次运行容器时，`docker.sh` 使用 `mktemp` 创建独立的临时 HOME，并在脚本退出时通过 `trap` 清理。临时 HOME 不用于保存长期状态。

## 8. 测试范围与维护约束

当前 `test` 命令执行 DMW 配置、编译和 CTest。是否执行需要 Fast DDS transport 或 ROS 2 的集成测试，仍由 DMW 自身的 CMake 测试选项决定；Docker launcher 不应隐式改变 DMW 的测试语义。

`integration-test` 执行需要 DDS transport 的测试，当前覆盖：Topic QoS endpoint reuse、
event-driven WaitSet（Guard、动态注册、并发 Busy、shutdown）、生命周期反复创建/销毁、
以及 DMW ↔ rclcpp 的双向 Topic 和 AddTwoInts Service 互操作。应在两个发行版中执行：

```bash
./docker/humble.sh integration-test
./docker/jazzy.sh integration-test
./docker/cross-integration-test.sh
```

### Sanitizer 验证

AddressSanitizer/UndefinedBehaviorSanitizer 与 ThreadSanitizer 使用独立 build tree，不能与普通
构建目录混用。已验证的 Jazzy ASan/UBSan 配置为：

```bash
cmake -S dmw -B build/docker/jazzy/dmw-asan -G Ninja \
  -DBUILD_TESTING=ON -DDMW_ENABLE_DDS_INTEGRATION_TESTS=ON \
  -DDMW_ENABLE_SANITIZERS=ON
cmake --build build/docker/jazzy/dmw-asan
DMW_ENABLE_DDS_INTEGRATION=1 ctest --test-dir build/docker/jazzy/dmw-asan \
  --output-on-failure --label-regex integration
```

TSan 使用 `-fno-pie/-no-pie`。在 Docker 的 GCC TSan 环境中，进程启动前还需要关闭 ASLR；
标准容器的 seccomp profile 会拒绝该 personality 调用。因此 TSan 必须在专用、显式的验证容器中
以 `--security-opt seccomp=unconfined` 和 `setarch x86_64 -R` 运行。不要把这两个选项加入日常
开发容器的默认配置。专用容器内的配置与执行为：

```bash
cmake -S dmw -B build/docker/jazzy/dmw-tsan -G Ninja \
  -DBUILD_TESTING=ON -DDMW_ENABLE_DDS_INTEGRATION_TESTS=ON \
  -DDMW_ENABLE_TSAN=ON
cmake --build build/docker/jazzy/dmw-tsan
DMW_ENABLE_DDS_INTEGRATION=1 setarch x86_64 -R \
  ctest --test-dir build/docker/jazzy/dmw-tsan --output-on-failure \
  --label-regex integration
```

上述命令已在 Jazzy 中通过全部 5 个集成测试。基础性能基线应以同一硬件、同一 Domain、UDPv4
transport 下的 publish→receive 延迟、接收路径分配数、以及 WaitSet idle/ready 两种负载分别记录；
它们用于检测回归，不应跨不同机器比较绝对数值。

维护 Docker 环境时遵循以下约束：

- Humble/Jazzy 应尽量共用一个 `Dockerfile` 和一个 `docker.sh`；
- 发行版差异只放在 `humble.sh` / `jazzy.sh` 等环境入口中；
- 不在 DMW 生产代码中因为 Docker 环境引入 `ROS_HUMBLE`、`ROS_JAZZY` 等条件分支；
- 不在镜像中另外安装一套与 ROS 发行版脱离的 Fast DDS，默认验证发行版真实提供的 Fast DDS 栈；
- 不将源码 `COPY` 到开发镜像；
- 不复用 Humble/Jazzy 的 CMake build directory；
- 不默认启用 `--ipc=host` 或 SHM；
- 不将 `ROS_DOMAIN_ID=23` 解释为公共 API 或协议要求；
- 新增依赖时应确认它是 DCL 构建、测试或互操作验证的真实依赖；
- Dockerfile 依赖版本默认跟随所选 ROS 基础镜像的软件仓库 patch 更新，不固定易失效的 Debian revision。

Bash 脚本应持续通过：

```bash
bash -n \
    docker/docker.sh \
    docker/entrypoint.sh \
    docker/humble.sh \
    docker/jazzy.sh

shellcheck -x \
    docker/docker.sh \
    docker/entrypoint.sh \
    docker/humble.sh \
    docker/jazzy.sh
```

Dockerfile 建议使用 Hadolint 检查：

```bash
hadolint docker/Dockerfile
```

Docker 环境发生变化后，至少执行：

```bash
./docker/humble.sh rebuild
./docker/jazzy.sh rebuild

./docker/humble.sh test
./docker/jazzy.sh test
```

当修改涉及 DDS wire behavior、QoS、类型支持、Topic、Service 或 request/reply identity 时，还应执行 Humble ↔ Jazzy 的双向互操作验证。
