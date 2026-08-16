# DMW Fast DDS V1 实现基线最终审计

> **性质：非规范性审计记录。** 本文描述指定源版本和当前工作树的实现状态，
> 不修改或放宽 `dmw.md`、`dmw_fastdds.md` 的任何 requirement。后者仍为
> `Frozen Candidate`；本文不得被用作“全量规格已冻结”的依据。

| 属性 | 值 |
| --- | --- |
| 审计基线 Git revision | `fc3492fe301d90fa971df916d2743245349f0350`（含未提交工作树） |
| `dmw.md` SHA-256 | `b9a453416ad0a97c3dc084dbb5e221f98ec8bef5d2edc9dc7201f2656c0ddb9a` |
| `dmw_fastdds.md` SHA-256 | `401e3ac19583155307df5ac58cad5116f959ceaf0897e3cf80c63c4c7eeb82aa` |
| 审计标准 | 实现优先：实现存在才可闭合；测试缺口单列，不将测试成功外推为规格等价 |
| 总结 | **未闭合。** 公共契约及一组关键数据路径已实现；完整内部状态机、失败证据和验证矩阵尚未完成。 |

此前版本的本报告所登记的两个 source digest 与当前规范正文不一致，故其全部
`Pass` 结论均已失效。本记录只对上表 digest 完全匹配的源有效。

## 1. 判定规则

| 状态 | 含义 |
| --- | --- |
| 已实现 | 可在当前源中定位实现，且未发现与该条款可观察语义相冲突的路径。 |
| 保守等价 | 内部机制不同，但安全性或外部可观察语义不弱于条款；表中必须列出边界。 |
| 部分实现 | 主路径已存在，但缺少规定的状态、失败/并发路径、资源边界或证据。 |
| 未实现 | 所需机制不存在，或现有行为与条款冲突。 |
| 不适用 | 仅限规范明确允许 V1 排除的项目，且引用该排除。 |

“闭合”只包括“已实现”和“保守等价”。“有代码但没有定向测试”可标为
`已实现（测试缺口）`，但不能成为验证矩阵通过的证据。

## 2. 全条款矩阵

本表以规范的全部编号段为审计单元；每个编号范围中的子条款均已按其正文要求
检查。`实现证据`为当前工作树定位；`验证`只列已存在的直接证据，`—` 为测试缺口。

| 条款 | 状态 | 实现证据 | 验证/说明 |
| --- | --- | --- | --- |
| §1.1–1.4 文档、基线、命名、总体原则 | 已实现 | `src/impl/*` 命名和 Fast DDS 边界 | 文档/构建检查；不构成 vendor 基线证明 |
| §1.5–1.6 vendor liveness、GuidPrefix 约束 | 部分实现 | `participant_observation.hpp` tombstone/reuse degradation | `service_match_state_test` 覆盖 reuse；无跨进程/不可靠 discovery 证明 |
| §2.1 Context entity mapping | 已实现 | `context.cpp`, `context_state.hpp` | `context_state_test` |
| §2.2–2.3 process runtime/ID allocator | 部分实现 | `process_runtime.hpp` process-lifetime retention | 无完整 process root、binding quarantine、不可 wrap allocator 测试 |
| §2.4–2.17 error priority、operation gate、Factory、shutdown linearization | 部分实现 | `context.cpp`, endpoint factories | 普通路径测试存在；creation indeterminate 和全部 priority matrix 未覆盖 |
| §2.18–2.24 ChildRegistry、ack-all、executor failure | 部分实现 | `shutdown_children_`, `ShutdownExecutionState` | 并发 shutdown 测试；不是预分配 intrusive ChildRegistry/ack-all 协议 |
| §2.25 facade destruction | 保守等价 | `ContextState::shutdown`, shared state retention | 隐式 shutdown 有覆盖；无 executor-loss/exception injection |
| §3 DDS ownership/operation status | 部分实现 | endpoint impl、context teardown | 无完整 entity status 分离及 contained graph 证据 |
| §4.1–4.12 binding、TemporarySample、Type/Topic registry | 部分实现 | `fastdds/message_type.cpp`, `context.cpp`, `node.cpp` | 现有 type/message tests；registry creating/retiring/orphan FSM 不完整 |
| §4.13–4.18 QoS authority/canonical fingerprint | 部分实现 | `fastdds/qos.hpp`, `context.cpp` | `qos_test` 覆盖 RuntimeMode overrides；无 13-policy canonical `TopicQosFingerprint` |
| §4.19–4.26 QoS policy mapping/golden tests | 部分实现 | `fastdds/qos.hpp` | KeepLast/Reliability等映射已测；完整 golden/XML isolation 缺失 |
| §5.1–5.7 listener state、drain、degraded teardown | 部分实现 | `impl/event_parent_state.cpp`, `service_match_state.hpp` | callback gate/retention 已有；完整 listener install/uninstall second-drain protocol 缺失 |
| §5.8–5.13 discovery listener/status masks | 部分实现 | `participant_observation.hpp`, endpoint listeners | 单进程状态测试；discovery final teardown/status mask matrix 缺失 |
| §5.14–5.20 participant/remote/service/target registries | 部分实现 | `participant_observation.hpp`, `service_match_state.hpp` | tombstone/exact removed/reuse 测试；精确图 rebuild、乱序 callback 与生命周期证据不完整 |
| §5.21–5.22 Event source/fan-out | 部分实现 | `impl/event_parent_state.cpp` | `event_parent_state_test`；degraded callback fan-out failure injection 缺失 |
| §6.1–6.7 wait holds、reader/writer/guard info | 部分实现 | `reader_wait_state.hpp`, `wait_set.cpp` | WaitSet 基础测试；historical hold ownership 未完整实现 |
| §6.8–6.9 reader destruction/deferred retry | 部分实现 | `reader_wait_state.hpp` quarantine retention | close/claim 覆盖；无 `DeleteDeferredByWaitSet` registry/retry FSM |
| §6.10 endpoint creation transaction | 部分实现 | `node.cpp`, endpoint sources | rollback 顺序已改善；partial DDS creation evidence/hidden entity rules 缺失 |
| §6.11–6.13 publish/take/MessageInfo | 已实现（测试缺口） | `publisher.cpp`, `subscriber.cpp`, `fastdds/message_type.cpp` | 数据路径和互操作已有；bounded filter/message-info failure matrix 不全 |
| §7.1–7.11 service composition/correlation/target mode | 部分实现 | `client.cpp`, `server.cpp`, `client_impl.hpp`, `server_impl.hpp` | service unit/integration 覆盖基本匹配；fallback/history 精确语义未全证 |
| §7.12 target wait | 已实现 | `service_match_state.hpp`, `server.cpp` | exact reader/participant Removed -> no-write 与 100ms deadline 测试 |
| §7.13–7.18 pending/capacity/take | 已实现（测试缺口） | `server_impl.hpp`, `server.cpp` | capacity-before-take 已实现；ABA、allocation 与 bounded scan 故障注入缺失 |
| §7.19–7.20 send response transaction/pending shutdown | 部分实现 | `server.cpp`, response match state | NotFound/Busy/removed 支持；ephemeral child registration、完整 rollback/shutdown race 未实现 |
| §8.1–8.18 waitable/registration/topology | 部分实现 | `wait_set.cpp`, reader/guard states | bad_alloc rollback/topology generation 已有；预分配 WaitSetInfo、active-wait drain FSM 缺失 |
| §8.19–8.26 poisoned/retired/control guard | 部分实现 | `wait_set.cpp` repair/poison path | 基本 poisoned/repair；RetiredWaitSetRegistry、全部 attach/detach indeterminate matrix 缺失 |
| §8.27–8.31 wait loop/capacity topology | 已实现 | `wait_set.cpp`, `server.cpp`, `reader_wait_state.hpp` | 100ms slice、full remove/available reattach；idle scale performance test 缺失 |
| §8.32–8.36 Guard generations/exhaustion | 部分实现 | `guard_condition_impl.hpp`, `guard_condition.cpp` | generation merge 已实现；logical-only degradation/ID and generation exhaustion 未完整证明 |
| §8.37–8.44 Event cursor/exhaustion/destruction | 部分实现 | `impl/event_parent_state.cpp`, `event.cpp` | independent cursors/registration overflow；generation exhaustion/degraded lifecycle 与 destruction matrix 不全 |
| §9.1–9.16 retirement/final teardown | 部分实现 | endpoint destructors, `context.cpp`, `process_runtime.hpp` | 安全 retention 倾向存在；retirement registry、container evidence barriers、exact teardown order 缺失 |
| §9.17 process terminal quarantine | 部分实现 | `process_runtime.hpp` | participant/listener retention；无 intrusive no-allocation adoption、ProcessBindingQuarantine、full entity graph ownership |
| §10.1–10.4 lock ranks/API call rules | 部分实现 | `lock_rank.hpp`, ranked mutex usages | 部分运行时断言；17 级在所有路径的应用、same-rank/API instrumentation 缺失 |
| §10.5–10.17 ReturnCode mapping/baseline tests | 部分实现 | `fastdds/return_code.hpp` | 部分集中映射；operation-specific matrix 与 vendor failure tests 缺失 |
| §11.1–11.25 verification matrix/frozen invariants | 未实现 | 现有 11 unit + integration tests | 规范列出的故障注入、sanitizer、poison/control/discovery/teardown/benchmark 集合尚未全建 |
| §12.1–12.2 public preconditions/final architecture | 部分实现 | public headers + current implementation | 公共前置条件大体闭合；“最终内部架构”依赖上述未闭合机制 |

## 3. 已可证明的闭合子集

- Humble runtime mode 的 endpoint QoS 冻结：history reallocation、同步发布、关闭
  data sharing 以及 Reliable writer 100 ms blocking timeout；`DDS` 不施加这些覆盖。
- Server capacity 满时 request reader 从 blocking WaitSet topology 移除，释放 slot 后重新加入，
  消除了 unread request + full capacity 的 readiness 自旋。
- response 目标的 exact reader 或其 participant 被确认 Removed 时，`send_response()` 返回
  no-write success；未移除且未匹配时采用一次计算的 100 ms absolute deadline。
- participant tombstone/GuidPrefix reuse degradation、Guard 合并 generation、Event 独立 cursor
  和 Event registration exhaustion、WaitSet add 的 allocation rollback 均已在当前实现中可定位。

这些结论只覆盖所列行为，不推出相邻子条款（例如 discovery callback teardown、完整
waitset retirement 或 response transaction 的 ephemeral child shutdown）已经闭合。

## 4. 保守实现与隔离保留的边界

| 机制 | 已保障 | 未满足/不得宣称 |
| --- | --- | --- |
| Process runtime retention | listener/participant 无法安全删除时不释放 backing | 不是 §9.17 的 intrusive、no-allocation 全 DDS graph terminal quarantine；没有 ProcessBindingQuarantine |
| shutdown children map | 并发调用等待同一 shutdown 完成，普通 child 能被请求 | 不是预分配 ChildRegistry，也没有 request-all/ack-all generation 协议或 executor-failure terminal FSM |
| lock rank wrapper | 已标注并检查部分关键锁序 | 不是所有 17 rank 的完整运行时证明；不覆盖所有 Fast DDS API call 边界 |
| topic conflict check | 对当前 public QoS 做一致性检查并在冲突时降级 | 不是 13-policy canonical TopicQos/Fingerprint，不可声明完整 Fast DDS topic QoS semantic equality |
| callback gates + quarantine | 一些 listener late callback 不会释放其 backing | 不等价于所有 listener 的 install/uninstall double drain 和 discovery final teardown |
| WaitSet poison/reader retain | 不能证明 detach 时保留对象，避免立即 UAF | 不是 RetiredWaitSetRegistry、historical Info ownership 和完整 broken-control-guard replacement protocol |

## 5. 未闭合条款及最小闭合条件

| 缺口 | 最小闭合条件 |
| --- | --- |
| §2.2–2.3、§2.18–2.24 | 实现 process IDs、预分配 intrusive ChildRegistry、request-all/ack-all generations、executor exception terminal failure；加入并发/耗尽/exception injection tests。 |
| §4.13–4.18 | 构造 canonical Fast DDS TopicQos，比较 13 policy 的 semantic fields，并为冲突/reuse 建 golden tests。 |
| §5.1–5.20 | 统一 CallbackInFlightGuard 双 drain；实现 discovery commit order/rebuild、late callback、out-of-order 和 final teardown tests。 |
| §6.8–6.10、§9 | 引入 DeleteDeferredByWaitSet/retirement registries，记录 Fast DDS entity deletion evidence，并按成功证据驱动 retry/quarantine。 |
| §7.19–7.20 | 实现 ephemeral interruptible child registration、stable pending handle 的完整 phase/rollback/shutdown race 与 allocation failure tests。 |
| §8.1–8.44 | 完成 preallocated WaitSetInfo/retired registry、attach/detach indeterminate rules、control guard replacement、logical-only Guard degradation 和所有 exhaustion paths。 |
| §10 | 将每个 Fast DDS API operation 接入其 ReturnCode matrix，并对 rank/API forbidden edges 加 instrumentation tests。 |
| §11 | 建立规范列出的 failure injection、sanitizer、cross-process discovery/teardown、idle WaitSet scaling 和 ROS 2 stress suites。 |

## 6. 验证记录与最终结论

本轮于 **2026-08-14** 执行并通过以下验证；不得把它们外推为 §11 的全量验证，
也不得在后续审计中把旧运行结果复制为新证据：

```sh
cmake --build dmw/build -j2
ctest --test-dir dmw/build --output-on-failure
ctest --test-dir dmw/build-integration --output-on-failure
git -C . diff --check
```

| 验证 | 实际结果 |
| --- | --- |
| `cmake --build dmw/build -j2` | 通过 |
| `ctest --test-dir dmw/build --output-on-failure` | 11/11 通过（unit、header、install consumer） |
| `ctest --test-dir dmw/build-integration --output-on-failure` | 12/12 通过（含 DDS message path、ROS 2 topic interop） |
| `git -C . diff --check` | 通过 |

截至本报告所绑定的实现基线，安全对外声明仅为：**DMW public contract 与上文“已可证明
的闭合子集”已实现，并有现有单元/DDS/ROS 2 测试覆盖其部分行为。** 不得声明
`dmw_fastdds.md` 全条款等价实现、`V1 Implementation Frozen`，也不得以现有 CTest 全绿
替代 §11 的完整验证矩阵。
