# 根因分析：Pursue Task 只进了 EnterState，Tick 从未执行

> 本文只分析 + 给方案，未改任何代码。

---

## 一、证据

你给的日志：

```
[PursueTask] EnterState  Owner=BP_ShooterAIController_C_1  Pawn=BP_ShooterNPC_C_1  → AutoPilot 已关闭
[PursueTask] ExitState  BP_ShooterNPC_C_1
```

**关键特征：EnterState 和 ExitState 之间，零次 Tick。**

这不是"Task 没生效"，而是**状态只活了一瞬间**。

---

## 二、根因：StateTree 的 `Any` 完成策略

### 引擎源码（`StateTreeTypes.h:921`）

```cpp
/** How the tasks control the completion of the state. */
UPROPERTY()
EStateTreeTaskCompletionType CompletionTasksControl = EStateTreeTaskCompletionType::Any;
```

### 枚举定义（`StateTreeTasksStatus.h:16`）

```cpp
UENUM()
enum class EStateTreeTaskCompletionType : uint8
{
    /** All tasks need to complete for the group to completes. */
    All,
    /** Any task completes the group. */
    Any,
};
```

### 这两行代码合起来的含义

**一个状态里如果挂了多个 Task，默认是「任何一个完成，整个状态就算完成」。**

`Any` 是**默认值**，你新建状态时不会注意到它。

### 所以发生了什么

```
进入 Attack 状态
    ↓
Attack 里原有的 Task（ShootAtTarget 之类）先返回 Succeeded / Failed
    ↓
因为 CompletionControl = Any → 状态被判定"完成"
    ↓
立即 ExitState，其他 Task 不再 Tick
    ↓
我的 Pursue Task：EnterState 跑了，Tick 一次都没轮到
```

**这完全解释了「EnterState → ExitState，中间没有 Tick」的现象。**

---

## 三、还有一个我自己埋的坑（需要一并修）

`StateTreePursueTask.cpp` 的 `Tick()` 里有 5 个出口：

```cpp
if (!Comp)                        { return Failed; }   // ← 这里没有任何日志！
if (!AIC || !Pawn)                { ShowDebug + return Failed; }
if (!Target)                      { ShowDebug + return ...; }
if (Distance <= StopDistance)     { ShowDebug + return Running; }
// 正常路径
ShowDebug("[PursueTask] CHASE ...");
```

第一个分支 `!Comp` **静默返回 Failed，不打印任何东西**。

如果 Tick 真跑到了这一行，行为上也会是「EnterState → 立即 ExitState」，和现在的现象**一模一样**，但完全看不到痕迹。

**所以现在有两种可能，日志无法区分：**

| 可能 | 机制 | 概率 |
|---|---|---|
| **A** | `Any` 策略下别的 Task 先完成，我的 Tick 没轮到 | **高** |
| **B** | 我的 Tick 跑了，但 `!Comp` 静默 Failed | 低（EnterState 用同一函数且成功了） |

---

## 四、解决方案

### 方案 1（推荐先试）：取消 Pursue Task 的「参与完成判定」

**改哪里**：StateTree 编辑器 → 选中 `Pursue Steering` Task → Details 面板

**改什么**：找到 **`Consider for Completion`**（可能在 Advanced 分类下），**取消勾选**

**为什么**：引擎源码 `StateTreeTaskBase.h:148-152` 明确写了这个字段的用途：

```cpp
/**
 * True if the task is considered for completion.
 * False if the task runs in the background without affecting the state completion.
 */
uint8 bConsideredForCompletion : 1;
```

**取消勾选后，Pursue Task 变成"后台运行"**——它返回什么都不影响状态完成判定，可以持续 Tick。

**优点**：不改代码，1 分钟。
**局限**：Pursue 的寿命仍受 Attack 状态限制（Attack 状态被别的 Task 结束，Pursue 还是会退出）。

---

### 方案 2（架构正确）：恢复独立的 Chase 状态

**改哪里**：StateTree 编辑器

**改什么**：
1. 新建 `Chase` 状态，**只放 Pursue Steering 一个 Task**
2. 给 Roam/Idle 加转换：`On Tick` → `Goto State Chase`，条件 `Target Is Valid`
3. 给 Chase 加转换：`On Tick` → `Goto State Roam`，条件 `Target Is Not Valid`

**为什么**：状态里只有它一个 Task，就没有「别的 Task 先完成」的问题；
而且 Chase 的进出条件完全由"看得见/看不见"决定，语义清晰、可调试。

**这是最初方案里就该做的事**，上次漏了"新建状态不会自动激活"这一步。

---

### 方案 3（加固，建议无论选哪个都做）：补掉静默失败点

**改哪里**：`StateTreePursueTask.cpp` 的 `Tick()`

**改什么**：把第一个分支

```cpp
if (!Comp) { return EStateTreeRunStatus::Failed; }
```

改成

```cpp
if (!Comp)
{
    ShowDebug(Pawn, FColor::Red, TEXT("[PursueTask] 失败：找不到 PursueSteering 组件"));
    UE_LOG(LogTemp, Warning, TEXT("[PursueTask] Tick 失败：找不到组件 Owner=%s"),
        *GetNameSafe(UnwrapOwner(Context)));
    return EStateTreeRunStatus::Failed;
}
```

**为什么**：这次的排查之所以绕这么久，就是因为有几个出口"失败但不留痕"。
**任何 return Failed 都必须留下痕迹**，否则下次还会重复这次的困境。

---

## 五、我建议的执行顺序

| 步骤 | 做什么 | 耗时 | 目的 |
|---|---|---|---|
| 1 | 方案 3：补掉静默失败点（改代码，需要编译） | 5 分钟 | 以后失败必留痕 |
| 2 | 方案 1：取消 `Consider for Completion`（不改代码） | 1 分钟 | 立刻验证 Pursue 能否持续 Tick |
| 3 | 若 2 成功但行为不理想 → 方案 2：恢复独立 Chase 状态 | 15 分钟 | 架构最终态 |

**如果想快速见到效果**：先只做 **方案 1**，不改代码、不用编译、不用关编辑器。

---

## 六、待你确认

请告诉我：

1. **先做方案 1 还是先做方案 3？**（方案 1 零成本，方案 3 要编译）
2. **`Consider for Completion` 这个勾选项，你在 Task 的 Details 面板里找得到吗？**
   如果找不到，可能是被折叠在 `Advanced` 里，或者需要先展开。
3. Attack 状态里现在**一共挂了几个 Task**？（这决定 `Any` 策略的影响有多大）

确认后我再动手。
