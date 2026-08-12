# DMW Fast DDS V1 Closure Audit

| 属性 | 值 |
| --- | --- |
| 性质 | Review evidence，非 normative specification |
| 审查日期 | 2026-08-12 |
| 结论 | Fast DDS 实现语义与 public contract closure 门禁均通过；运行时实现仍保持 Frozen Candidate |

本文只记录审查输入、结论和证据摘要，不复制或改写 `dmw.md` 与 `dmw_fastdds.md` 的规范条款。当前 `dmw_fastdds.md` 必须继续保持 Frozen Candidate。

## 1. Source Binding

| Source | SHA-256 |
| --- | --- |
| `dmw.md` | `4a9ca983ae1eeebc0622956a68d611990afd69fdb26a7483d321df5ec4bfc942` |
| `dmw_fastdds.md` | `6a9a394c99dc08df8de6095652f828a6da689d4e12a2d542d381fe4aa52d4fa9` |

任一 digest 不匹配时，本记录失效。重新审查不得只更新 digest。

## 2. Public Contract Closure

| IDs | `dmw.md` | Public headers | Result |
| --- | --- | --- | --- |
| PUB-001 | Context-scoped immutable profile已合入 | `context.hpp` 含 profile，四类 endpoint options无 override | Pass |
| PUB-002～PUB-015 | 已合入对应 normative contract | 除 PUB-001 外未发现与本轮相关的声明冲突 | Pass |

临时 `dmw_public_contract_merge_patch.md` 已合入并从 working tree 删除。public header 已同步；
header self-containment/compile tests 由 DMW CMake/CTest skeleton 执行并必须保持通过。

## 3. Lock-edge Graph

| From | To | Allowed site / constraint | Result |
| --- | --- | --- | --- |
| Context runtime (1) | ChildRegistry (2) | child Factory registration；不得跨 Fast DDS API call | Pass |
| RemoteEndpointRegistry (6) | ServiceMatchRegistry (7) | [discovery commit order](dmw_fastdds.md#fastdds-discovery-commit-order) 定义的唯一 discovery cross-registry nested edge | Pass |
| WaitSet topology (12) | Waitable local (15) | add/bidirectional registration commit | Pass |
| Lower-rank registry | higher-rank registry | 仅正文明确列出的 handoff/edge；Target 前释放 5/6/7 | Pass |
| TopicRegistry (4) | TypeRegistry (3) | 禁止；Topic three-stage transaction在 TypeLease 前解锁 | Pass |
| Target (8) | Participant/Remote/Service (5/6/7) | 禁止；读取 stable participant atomics | Pass |
| Pending (16) | Target (8) | 禁止重叠；send_response two-phase handle revalidation | Pass |
| ChildRegistry/runtime/其它 DMW mutex | Fast DDS/private control wake | 禁止；shutdown B1 publish、B2 unlocked signal | Pass |
| topology (12) | reconciliation (13) | 禁止重叠；snapshot后 handoff | Pass |
| Waitable/Event local (15) | topology (12) | 禁止；auto-detach two-phase反转调用方向 | Pass |

same-rank peer默认不嵌套；唯一明确的 Fast DDS API call mutex exception 是单 WaitSet
`reconciliation_mutex`，且同时不得持有其它 DMW mutex。

## 4. Public Error-stage Table

| Operation family | Frozen stage order | Allocation/Fast DDS API boundary | Result |
| --- | --- | --- | --- |
| Context/Node Factory | argument -> Context -> parent -> local -> middleware | materialization仅在高优先级检查后 | Pass |
| Publisher/Subscriber Factory | name/QoS/type -> Context -> Node -> registry -> Fast DDS | Topic/Type transactional guards | Pass |
| Client/Server Factory | argument -> Context -> Node -> aggregate endpoint create | 两 endpoint all-or-rollback | Pass |
| publish/send_request | argument -> Context -> parent/local -> serialize -> write | Fast DDS write零 DMW mutex | Pass |
| Subscriber/Client take | argument -> Context -> parent/local -> Fast DDS take -> output commit | pre/post-consumption guarantee分离 | Pass |
| Server take_request | argument -> Context -> parent/local -> capacity -> finite scan -> Pending commit | full capacity在 Fast DDS take 前返回 | Pass |
| Server send_response | argument -> Context -> parent -> NotFound/Busy -> preallocate -> revalidate/claim -> target -> write | later OOM不抢占 NotFound/Busy | Pass |
| WaitSet add | argument/same-context -> Context -> waitable parent -> registration -> WaitSet local -> reconcile | Registration allocation在 ID commit 前 | Pass |
| WaitSet remove/wait | token/Context -> local topology -> Fast DDS reconciliation/wait | absolute deadline只建一次 | Pass |
| Guard trigger | argument/Context/local -> logical commit -> best-effort wake | commit 后 public success固定 | Pass |
| Event create/take | argument/Context/parent/type -> cursor registration / local source | ContextShutdown优先ParentDestroyed/Exhausted | Pass |
| shutdown | executor election -> request-all -> drain -> ack-all -> terminal commit | wake failure有bounded fallback | Pass |

## 5. FSM Completeness

| FSM | Terminal/failure closure | Result |
| --- | --- | --- |
| Runtime × ShutdownExecution | Running只到Completed或Failed；Failed保存exception且不重跑partial phases | Pass |
| Type/Topic Registry | Creating/Retiring waiter重查；Orphaned terminal；Topic Creating(tx)防重复 creator | Pass |
| Participant/Remote | canonical participant Removed terminal；remote absent-remove tombstone；illegal resurrection降级 | Pass |
| ServiceMatch/Target | NeedsRebuild generation防stale commit；Target dependency generation防lost wake | Pass |
| Pending Request | Pending -> Responding -> erased；failure Active时rollback，shutdown时不reinsert | Pass |
| WaitSet/Registration | Attached -> Detaching -> Detached单一 CAS winner；unresolved `WaitSetInfo` -> Poisoned | Pass |
| Control Guard | Healthy/LogicalOnlyDegraded/Broken replacement唯一协议 | Pass |
| Event | source Healthy/Exhausted/Degraded terminal；parent Alive -> Destroyed；cursor仅successful take推进 | Pass |
| Fast DDS entity status | NotStarted/NoSideEffect/HandleKnown/SideEffectIndeterminate 与 deletion status 分离 | Pass |

## 6. Authority / Ownership / Lifecycle Status

| Fact/resource | Sole authority / owner | Retention condition | Result |
| --- | --- | --- | --- |
| Context Active state | ContextState runtime state | shared state只延迟teardown，不延长Active | Pass |
| child shutdown request/ack | InternalChildState generations | ChildRegistry unlink前补齐ack | Pass |
| wire type | TypeRegistry canonical TypeEntry | TypeLease + orphan retention | Pass |
| Topic identity/QoS | TopicRegistry TopicEntry | independent Topic TypeLease | Pass |
| Participant lifecycle | ParticipantObservationRegistryState | Context-lifetime canonical tombstone | Pass |
| remote endpoint lifecycle | RemoteEndpointRegistryState | stable participant handle，不复制participant table | Pass |
| target response state | TargetReaderObservationRegistryState | TargetReaderKey strong-own participant handle | Pass |
| pending response right | PendingRequestRegistry/PendingEntryHandle | stable-handle revalidation消除ABA | Pass |
| Event cumulative state | EventSourceState | EventState独立cursor；parent source由Event保活 | Pass |
| WaitSet attachment | RegistrationState + published WaitSetInfo | active_wait_count/retired WaitSetInfo 保活 | Pass |
| DDS endpoint pointer | DataReaderInfo/DataWriterInfo | creation/entity status + container barrier | Pass |
| terminal unknown DDS entity graph | ProcessTerminalQuarantine | no-allocation intrusive adoption，process-lifetime | Pass |

## 7. CV / Wake Handshake

| Wait | Predicate authority | Publication/wake rule | Result |
| --- | --- | --- | --- |
| shutdown operation drain | runtime mutex + operations_in_flight | decrement与wait共享mutex/CV | Pass |
| child acknowledgement | child generations + child mutex | normal publish与wait共享mutex；bounded slice fallback | Pass |
| listener drain | ListenerState mutex + callbacks_in_flight | accepting/predicate/notify同同步域 | Pass |
| Registration drain | drain mutex + active_wait_count/phase | zero publication与wait共享mutex/CV | Pass |
| target discovery wait | Target mutex + dependency/entry generations | external change先取得Target mutex再notify | Pass |
| WaitSet wait | logical readiness/topology generation | best-effort control wake + <=100 ms slices + precheck | Pass |

## 8. Throw-point / Rollback

| Throw point | Prebuilt owner / rollback | Public channel | Result |
| --- | --- | --- | --- |
| participant/container create | TerminalContextNode/contained-entity status | mapped Error或原 exception | Pass |
| type/topic create | Creating tx + TypeLease/creation status | rollback或Orphaned后返回/传播 | Pass |
| endpoint create/listener install | aggregate retirement/listener Info | all-or-rollback；indeterminate retain | Pass |
| binding createData/deleteData/serialize | TemporarySample/ProcessBindingQuarantine | bad_alloc/other exception原样；noexcept delete隔离 | Pass |
| take conversion/output commit | TemporarySample + rollback guard | pre/post-consumption guarantee | Pass |
| Pending/Target key allocation | CapacityReservation + PendingEntryRollbackGuard | bookkeeping先rollback再传播 | Pass |
| send_response preallocation/write | stable Pending handle + ResponseRollbackGuard | NotFound/Busy优先；failure恢复或erase | Pass |
| WaitSet add/reconcile | preallocated registration/WaitSetInfo | ID commit前strong guarantee；indeterminatePoisoned | Pass |
| Event callback snapshot | source cumulative authority | callback内catch并把source/type Degraded | Pass |
| teardown/quarantine adoption | preallocated intrusive retirement/terminal node | noexcept；失败时intentional retain/leak | Pass |

## 9. Mechanical Checks

- code fences：paired；
- duplicate headings：none；
- trailing whitespace：none；
- `git diff --check`：pass；
- DMW CMake/CTest header self-containment（含 `dmw/fastdds/message_type.hpp`）：pass；
- old public entity Factory/move/shared ownership patterns：只在明确“禁止”示例中出现；
- public header profile ownership：Pass；Context 是唯一 CompatibilityProfile owner。
