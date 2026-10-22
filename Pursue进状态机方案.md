# 把 Pursue 放进状态机：为什么可以、怎么做

> 直接回答：**可以，而且架构上更正确。**
> 我之前没这么做，是选了「省事」的路，不是「不可能」。

---

## 一、先纠正一个前提

你问「为什么不能」，但这个前提本身不成立。技术上完全可行，而且这才是标准做法。

UE 的 StateTree 提供了 C++ 扩展点：

```cpp
struct FStateTreeTaskBase
{
    virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext&, const FStateTreeTransitionResult&) const;
    virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext&, const float DeltaTime) const;
    virtual void ExitState(FStateTreeExecutionContext&, const FStateTreeTransitionResult&) const;
};
```

继承 `FStateTreeTaskCommonBase`，实现这三个函数，就能做出一个能在 StateTree 编辑器里拖进状态的自定义节点。

---

## 二、为什么我之前选了「旁路」

诚实说明当时的判断：

| 考虑 | 当时的权衡 |
|---|---|
| **改 StateTree 资产** | 要在编辑器里手动加状态、连转换、绑参数。你当时还没摸清架构，操作风险高 |
| **验证速度** | 组件旁路只需要「挂组件 + 勾一个开关」，1 分钟见效 |
| **跑通优先** | 你的目标是尽快看到斜插效果，架构洁癖可以先放 |

**这是工程实用主义，不是技术限制。**

**代价也确实存在**（当时没跟你说清）：

```
StateTree 认为敌人在 Roam（漫游）
       ↓
实际敌人在追击（被组件抢了方向盘）
       ↓
状态不一致 → 调试困难、行为不可预测、面试讲不清
```

---

## 三、两种架构的对比

| 维度 | A · 旁路（现状） | B · 融入状态机（新增） |
|---|---|---|
| 决策 | StateTree 只管漫游/射击 | StateTree 管漫游/追击/搜索/射击 |
| 移动指令 | 组件在 PostUpdateWork 抢 | Task 在状态内正常下达 |
| 状态一致性 | ❌ 状态说漫游，实际在追 | ✅ 完全一致 |
| 可调试 | 只能看屏幕文字 | StateTree 调试器可视化 |
| 能否加断点 | 难 | 可以在 Task 里断点 |
| 面试可讲性 | 会被追问「为什么不用状态机管」 | 标准答案 |
| 改动成本 | 已完成 | 需要在编辑器加 2 个状态 |

---

## 四、已经写好的代码

| 文件 | 作用 |
|---|---|
| `Source/ShooterAI/StateTreePursueTask.h` | Task 声明 + 实例数据（Target / StopDistance / AcceptanceRadius / bUseSeparation） |
| `Source/ShooterAI/StateTreePursueTask.cpp` | 实现：进入时关掉组件 AutoPilot → 算预判点 → 下 MoveTo |
| `ShooterAI.Build.cs` | 加了 `StateTreeModule` 依赖 |

**编译已通过**（DLL 151.5KB → 166KB）。

### 职责怎么分

```
StateTree      →  决定「现在该干什么」（Roam / Chase / Search / Shoot）
Pursue Task    →  决定「往哪追」（调组件算预判点，然后下 MoveTo）
PursueSteering →  只算「落点在哪」（纯算法，不再驱动移动）
CharacterMovement → 执行位移
```

**关键改动**：Task 的 `EnterState` 里会执行 `Comp->bAutoPilot = false;`
——把组件降级为纯算法，不再抢移动指令。所以两者**不会打架**。

---

## 五、怎么在编辑器里接上（约 15 分钟）

### 步骤 1 · 打开 ST_Shooter

内容浏览器 → `Content/Variant_Shooter/Blueprints/AI/ST_Shooter` → 双击打开 StateTree 编辑器。

### 步骤 2 · 新建 Chase 状态

1. 在左侧状态树里，右键 `Root` → **Add State**
2. 命名 `Chase`
3. 选中 Chase，在右侧细节面板的 **Tasks** 里点 `+` → 搜 **`Pursue Steering`** → 选中

### 步骤 3 · 配参数

选中刚加的 `Pursue Steering` Task，细节面板会出现：

| 参数 | 建议值 | 说明 |
|---|---|---|
| `Target` | 留空 | 留空则自动找 0 号玩家；留空最简单 |
| `Stop Distance` | `250` | 进入 2.5 米停下开火 |
| `Acceptance Radius` | `100` | |
| `Use Separation` | ✅ | 多敌不挤 |
| `Fail When No Target` | ❌ | 找不到玩家时保持 Running，由转换条件决定离开 |

### 步骤 4 · 加转换条件（关键）

Chase 状态需要两条出口：

| 转换 | 条件 | 目标状态 |
|---|---|---|
| **看见玩家** | 进入 Chase 的条件 | 从 Roam/Shooting → Chase |
| **看不见玩家** | `Sense` 任务的 `Target == None` | Chase → Roam（或新的 Search） |

> **具体怎么连**：StateTree 的转换在状态右侧的 **Transitions** 里配。
> 条件可以绑定 `StateTreeTask_SenseEnemies` 的输出。
> 这一步没法用脚本代劳——必须你在编辑器里点。

### 步骤 5 · 关掉组件的 AutoPilot（重要）

虽然 Task 的 `EnterState` 会自动关，但为了保险：

打开 `BP_ShooterNPC` → `Pursue Steering` 组件 → **取消勾选 `Auto Pilot`**。

**否则**：在你还没配好转换前，组件会继续旁路接管，看不出 Task 是否生效。

---

## 六、验证是否真的走的是 Task

Play 后看屏幕，**现在应该同时出现两行**（Key 已错开，不再互相覆盖）：

| 文字 | 来源 | 含义 |
|---|---|---|
| `[PursueComp] AutoPilot=OFF（已交给状态机，正常）` | 组件 | ✅ 组件已降级为纯算法 |
| `[PursueTask] CHASE d=1500 pred=(...)` | **Task** | ✅ 状态机在驱动 |
| `[PursueTask] STOP d=200` | **Task** | ✅ 已到射程 |
| 只有 `[PursueComp]`、没有 `[PursueTask]` | — | ❌ Chase 状态没进入，查转换条件 |

### 更可靠的证据：Output Log

Task 的 `EnterState` / `ExitState` 会往 Output Log 打 Warning 级日志：

```
LogTemp: Warning: [PursueTask] EnterState  BP_ShooterAIController_C_0  → AutoPilot 已关闭，改由状态机驱动
LogTemp: Warning: [PursueTask] ExitState  BP_ShooterAIController_C_0  → 已停止移动
```

**看到 EnterState 就说明 Chase 状态真的进来了**，比屏幕文字可靠。

### 不用看任何输出也能判断

手动取消 `BP_ShooterNPC` 的 `Auto Pilot` 勾选后 Play：

| 现象 | 含义 |
|---|---|
| 敌人还在追 | ✅ Task 在驱动（AutoPilot 已关还能追，只能是 Task） |
| 敌人完全不动 | ❌ Task 没跑 |

---

## 七、排查过的一个坑：调试消息 Key 冲突

早期版本组件和 Task 用**同一个 Key**（都是 Pawn 指针）显示屏幕文字。
`AddOnScreenDebugMessage` 的 Key 是**全局显示槽位**，同 Key 会互相覆盖。

组件在 `TG_PostUpdateWork` 执行、比 StateTree 晚，所以 **Task 的文字每帧都被组件覆盖掉**，
屏幕上永远只看到 `[Pursue]`——看起来像"Task 没生效"，实际可能一直在跑。

**修法**：Task 的 Key 改成 `Pawn 指针 + 1`；组件前缀改为 `[PursueComp]` 以区分。

> **通用教训**：同一 Actor 上多个系统打屏幕调试信息，必须错开 Key。
> 否则排查时会误判成"功能没生效"，浪费大量时间。

---

## 八、排查：屏幕上只有 `[PursueComp] IDLE`，没有 `[PursueTask]`

### 先读懂这行字

```
[PursueComp] IDLE 交还状态树 | 失联40.7s | 最近同伴-1
```

| 字段 | 含义 | 说明什么 |
|---|---|---|
| `IDLE`（不是 `AutoPilot=OFF`） | 组件的 AutoPilot 还在跑 | **Task 的 EnterState 从未执行** |
| `失联40.7s` | 40.7 秒没看见玩家 | 这个敌人离你很远，本来就不该追 |
| `最近同伴-1` | 附近没同伴 | 正常（敌人分散在地图各处） |

**关键推理**：Task 的 `EnterState` 第一件事就是 `Comp->bAutoPilot = false`。
如果它执行过，屏幕必然显示 `AutoPilot=OFF`。
现在显示 `IDLE` → **EnterState 没跑过 → Chase 状态从未被进入。**

### 原因 A：没有转换能进入 Chase（最可能）

在 StateTree 里新建状态后，**它不会自动激活**。必须有一条转换指向它。

### 原因 B：敌人根本没看见你

`失联40.7s` 说明这些敌人一直在远处。
即使转换配好了，**只有「感知到目标」那一刻才会进 Chase**。

测试时你必须**走到某个敌人跟前**，让它看见你。

---

### 两条修法

#### 修法 1（最快验通）：把 Task 挂到「已经会激活」的状态上

不用配新转换。找出 ST_Shooter 里**在看见玩家时会激活的那个状态**（模板里叫 `Shooting` 或 `Attack`），
把 `Pursue Steering` Task 也加到那个状态的 Tasks 里。

这样敌人一看见你，Task 就执行，`[PursueTask]` 立刻出现。

#### 修法 2（架构正确）：给 Chase 配转换

在 StateTree 编辑器里：

| 位置 | 配什么 |
|---|---|
| **当前激活状态**（Roam / Idle）的 **Transitions** | `+` → Trigger = **`On Tick`** → Type = **`Goto State`** → Target = **`Chase`** → Conditions 加 **`Target Is Valid`**（绑感知任务的 Target） |
| **Chase** 的 **Transitions** | `+` → Trigger = **`On Tick`** → Type = **`Goto State`** → Target = **`Roam`** → Conditions 加 **`Target Is Not Valid`** |

**条件怎么找**：在 Conditions 的 `+` 弹窗搜 `Is Valid`，选 `StateTree Object Is Valid`，
然后把它的输入绑定到 `StateTreeTask_SenseEnemies` 输出的 `Target`。

---

### 判定表

| 屏幕显示 | 含义 | 下一步 |
|---|---|---|
| `[PursueComp] IDLE` | Task 没跑 | 配转换 / 改挂到 Shooting |
| `[PursueComp] AutoPilot=OFF` + `[PursueTask] CHASE` | ✅ 成功 | 继续验证行为 |
| `[PursueComp] AutoPilot=OFF`，无 Task | EnterState 跑了但 Tick 没跑到打印 | 见下方「第九节」 |
| 两行都没有 | 组件没挂上 | 检查 BP_ShooterNPC |

> **别忘了**：所有这些的前提是你**站在敌人旁边**。
> 站在出生点不动，敌人只会显示 `IDLE`——它们看不见你，自然不会追。

---

## 九、进阶排查：AutoPilot=OFF 出现了，但 `[PursueTask]` 没有

### 现象

```
[PursueComp] AutoPilot=OFF（已交给状态机，正常）
[PursueComp] CHASE-MEMORY d=1205 | 失联8.5s
[PursueComp] IDLE ... （其余敌人）
```

`AutoPilot=OFF` 出现了 → **EnterState 执行过**。
但没有 `[PursueTask]` → **Tick 没走到打印语句**。

### 根因：EnterState 和 Tick 的 Owner 解析不一致

```cpp
// EnterState 用 ResolveComponent()，里面有容错：
//   Owner 是 UActorComponent → 向上取 GetOwner()
// Tick 里却直接写：
AAIController* AIC = Cast<AAIController>(Context.GetOwner());   // ← 没有容错
if (!AIC || !Pawn) { return Failed; }                          // ← 静默返回
```

**StateTree 的 `Context.GetOwner()` 不一定是 `AAIController`**（也可能是 `UStateTreeAIComponent` 本身）。
一旦它返回的是组件，Tick 就在 `!AIC` 处静默 `return Failed` —— 不打印、不报错。

于是出现「EnterState 成功、Tick 从不产生输出」的诡异现象。

### 修法

把 Owner 解析统一到一个辅助函数，三处（Enter / Tick / Exit）共用：

```cpp
UObject* UnwrapOwner(Context)              // 组件 → 取 GetOwner()
AAIController* ResolveAIController(Context) // Owner 是 Pawn 时，从 Pawn->GetController() 反查
APawn* ResolvePawn(Context)
```

并且 Tick 失败时**必须打点东西**（红色屏幕字或 UE_LOG），不允许静默失败。

### 已排除的其他可能

| 怀疑 | 实测 | 结论 |
|---|---|---|
| `WITH_GAMEPLAY_DEBUGGER` 为 0，屏幕字被编译掉 | `Definitions.ShooterAI.h` 里是 `1` | ❌ 排除 |
| Task 没注册进反射 | `StateTreePursueTask.gen.cpp` 存在 | ❌ 排除 |
| Task 没绑到状态 | 资产里 `Chase` ×1、`StateTreePursueTask` ×2 | ❌ 排除 |
| 屏幕 Key 冲突 | Task 用 `Pawn+1`，组件用 `Pawn` | ❌ 已修 |

### 编码时还会踩的坑：`TNotNull` 不能当 bool 用

`Context.GetOwner()` 返回的是 `TNotNull<UObject*>`，不是裸指针。两种常见写法都会编译失败：

```cpp
// ❌ C2280：TNotNull 的 operator bool 被 delete 了
if (Context.GetOwner()) { }

// ❌ C2039：TNotNull 没有 Get() 方法
UObject* Raw = Context.GetOwner().Get();
```

**正解**：`TNotNull<T*>` 提供 `operator T*()` 隐式转换，**直接赋给裸指针**即可：

```cpp
// ✅
UObject* Raw = Context.GetOwner();
if (Raw) { ... }
```

---

## 十一、根因：StateTree 默认完成策略是 `Any`

### 引擎源码

`StateTreeTypes.h:921`：
```cpp
EStateTreeTaskCompletionType CompletionTasksControl = EStateTreeTaskCompletionType::Any;
```

`StateTreeTasksStatus.h:16`：
```cpp
UENUM()
enum class EStateTreeTaskCompletionType : uint8
{
    All,   // 所有 Task 完成，状态才完成
    Any,   // 任一 Task 完成，状态就完成   ← 默认
};
```

### 这意味着什么

**一个状态里挂多个 Task 时，任何一个返回 Succeeded/Failed，整个状态立即结束，其他 Task 再也不会被 Tick。**

### 日志证据

```
[PursueTask] EnterState  Owner=...  Pawn=...  → AutoPilot 已关闭
[PursueTask] ExitState  ...
```

**EnterState 和 ExitState 之间零次 Tick** —— 正是"状态被别的 Task 提前结束"的表现。

> 这是 StateTree 的**隐藏默认值**，新建状态时不会注意到。
> 所以并不是我们的代码写错。

---

## 十二、加固：任何失败都必须留痕

### 为什么要做

排查过程中最耗时的一环是：**有几个 `return Failed` 是静默的**。

```cpp
if (!Comp) { return Failed; }   // ← 以前这里什么都不打
```

一旦走到这里，现象和"状态被别的 Task 结束"**完全一样**（都是 EnterState 后立刻 ExitState），
但日志里找不到任何痕迹，只能靠猜。

### 改了什么（`StateTreePursueTask.h/.cpp`）

| # | 改动 | 为什么 |
|---|---|---|
| 1 | InstanceData 增加 `TickCount` 计数器 | 确认 Tick 是否被调用过。**用实例字段而非 static**，因为 static 在所有敌人间共享，第一个敌人打过日志后其他就不打了 |
| 2 | Tick 开头先 `TickCount++`，`== 1` 时打"第 1 次 Tick 进来了" | 决定性探针：看到它 = Tick 真的跑了 = 状态没有立即结束 |
| 3 | 统一走 `FailWithReason()`：每个失败点都 `UE_LOG` + 红色屏幕字 | 禁止静默失败 |
| 4 | AIController 为空时，日志里带上 **Owner 的实际类名** | 下次解析失败时直接知道 Owner 是什么类型，不用再猜 |
| 5 | ExitState 打印 `Transition.CurrentRunStatus` + `TickCount` | **反查退出原因**：`Failed` = 我的失败；`Running` = 被转换条件打断；`Succeeded` = 有 Task 完成 |

### 怎么用这套日志

| 日志 | 含义 | 下一步 |
|---|---|---|
| `第 1 次 Tick 进来了` 后跟着 `CHASE d=...` | ✅ 一切正常 | 观察行为 |
| `第 1 次 Tick 进来了` 后没有 CHASE，但有 `Tick 失败：xxx` | 某个校验没过 | 按 `xxx` 修 |
| **完全没有 `第 1 次 Tick`**，直接 `ExitState ... Tick 次数=0 退出时状态=Running` | 状态被转换条件立即打断 | 检查 Chase 状态上的转换条件 |
| `ExitState ... Tick 次数=0 退出时状态=Failed` | 有 Task 在 Enter 阶段就失败了 | 看是哪个 Task |
| `ExitState ... Tick 次数=0 退出时状态=Succeeded` | 有 Task 立即完成了（被 `Any` 策略结束） | 取消该 Task 的完成判定 |

---

## 十三、要不要现在切换

我的建议：**分两步走，别一次改完。**

| 阶段 | 做什么 | 目的 |
|---|---|---|
| **现在** | 先把 3 处设计点在现有架构下验完（F 区分离 + A 区四色） | 确保能力本身是对的 |
| **验完之后** | 再切到 StateTree 架构 | 换成更正确的架构，能力不变 |

**为什么这个顺序**：

如果先换架构再验能力，一旦行为不对，你分不清是「算法有问题」还是「StateTree 接线有问题」——两个变量同时变，排查成本翻倍。

**能力先验证，架构后优化**，这是可控的顺序。

---

## 八、面试怎么讲这段（重要）

这段经历本身就是好素材，因为它体现的是**判断力**，不是技术炫技：

> **关于架构选择**
>
> 我一开始把追击做成独立组件，用 TickGroup 抢在状态机之后下达移动指令。这样能快速验证算法，但架构上不干净——状态机认为敌人在漫游，实际却在追击，两者不一致。
>
> 后来我把它重构成了 StateTree 的 Task：状态机里增加 Chase 和 Search 两个状态，组件降级为纯算法（只算预判落点），移动指令由 Task 在状态内正常下达。这样决策、算法、执行三层各归其位，也能用状态机调试器可视化。
>
> **先跑通再重构，是我刻意的顺序**：算法正确性必须先独立验证，否则架构和行为两个变量同时变，出了问题分不清是谁的锅。

这段话的价值在于：你既承认了第一版不完美，又说明了为什么不完美是有意的，还给出了改进路径。**比一上来就完美更真实。**
