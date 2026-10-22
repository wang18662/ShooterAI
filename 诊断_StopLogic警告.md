# 诊断：`StopLogic` 警告 + `[PursueTask]` 消失

> 本文只做分析，不改任何代码。
> 结论在最后，请先看证据。

---

## 一、这条警告到底是什么

### 引擎源码位置

`Engine/Plugins/Runtime/GameplayStateTree/.../StateTreeComponent.cpp:215`

```cpp
void UStateTreeComponent::StopLogic(const FString& Reason)
{
    ...
    if (!bIsRunning) { return; }        // 第 232 行：没在跑就直接退出
    DisableTick();                      // 第 237 行

    if (CurrentlyRunningExecContext)
    {
        StopTree(*CurrentlyRunningExecContext);
    }
    else
    {
        FStateTreeExecutionContext Context(...);
        if (SetContextRequirements(Context))
        {
            StopTree(Context);
        }
        else
        {
            // ↓ 你看到的这条
            STATETREE_LOG(Warning, TEXT("Context Requirements in %hs failed. Component tick is disabled."), __FUNCTION__);
        }
    }
}
```

**关键：`%hs` 是函数名。你日志里是 `StopLogic`，说明这条来自这里，不是来自 Tick。**

### 触发条件（三个同时成立）

| # | 条件 | 说明 |
|---|---|---|
| 1 | `bIsRunning == true` | StateTree 之前确实在跑（否则第 232 行就 return 了） |
| 2 | `CurrentlyRunningExecContext == nullptr` | 不是在 tick 过程中调用 |
| 3 | `SetContextRequirements()` 返回 false | **有外部数据（external data）无效** |

### 为什么会"外部数据无效"

`UStateTreeComponentSchema::SetContextRequirements` 里：

```cpp
bool bResult = Context.AreContextDataViewsValid();
```

对 AI Schema 来说，外部数据通常是：**AIController（Owner）、Pawn（AIController->GetPawn()）、Controller**。

**`StopLogic` 一般在什么时候被调用？**

| 时机 | 此时 Pawn 状态 |
|---|---|
| `EndPlay`（Actor 销毁） | 可能已销毁 → `GetPawn()` 返回 null |
| AI 死亡 / 控制器失活 | Pawn 可能仍在，但控制器已脱离 |
| 手动 StopLogic | 视情况 |

**→ 这条警告最典型的出现时机是「敌人死亡、Actor 被销毁、EndPlay 里调 StopLogic」。**

---

## 二、这条警告大概率不是「Task 不工作」的原因

理由：

1. **它出现在 `StopLogic`（停止）路径**，不是 tick 路径。
2. **`StopLogic` 之后本来就该停止 tick**，`DisableTick()` 是设计行为，不是故障。
3. **StateTree 停止后，Task 的 Tick 当然不会执行**——但那时敌人已经死了，本来也不需要它追人。

**所以：这条警告 ≈ 敌人死亡时的正常噪音。它不能证明"运行期 StateTree 挂了"。**

---

## 三、那 `[PursueTask]` 为什么没出现

### 最可能的原因：`AutoPilot=OFF` 可能是蓝图里存的，不是 Task 关的

`bAutoPilot` 是组件的**成员变量**。

- 如果 `BP_ShooterNPC` 的 `Pursue Steering` 组件上 **`Auto Pilot` 被取消勾选并保存了**，
- 那么每次 Play 生成的敌人，`bAutoPilot` 默认就是 `false`，
- 屏幕会显示 `AutoPilot=OFF（已交给状态机，正常）`，
- **但这一行与 Task 是否执行毫无关系。**

> 我之前建议过你「为了测试，手动取消 Auto Pilot 勾选」。如果后来没改回来，就会出现这个假象。

### 其他可能（按概率排序）

| # | 可能 | 如何区分 |
|---|---|---|
| A | `AutoPilot` 是蓝图默认 false，Task 其实没跑 | 看 Output Log 有没有 `[PursueTask] EnterState` |
| B | Task 跑了，但屏幕字 0.2 秒就被刷掉，截图时机错过 | 同上，日志是持久的 |
| C | Attack 状态的 Task 没启用（加了但没勾 Enable） | StateTree 调试器看该状态激活时 Task 是否在跑 |
| D | Attack 状态根本没进入（转换条件不满足） | StateTree 调试器看当前激活状态 |
| E | Tick 里解析失败（已修，但仍可能别的失败点） | 日志里应有 `[PursueTask] Tick 失败：AIC=... OwnerClass=...` |

---

## 四、需要你提供三条信息（不用改代码）

### ① Output Log 里搜 `PursueTask`

在 Output Log 的过滤框输入 `PursueTask`，看有没有：

```
[PursueTask] EnterState  Owner=...  Pawn=...  → AutoPilot 已关闭
[PursueTask] ExitState  ...
[PursueTask] Tick 失败：AIC=...  Pawn=...  OwnerClass=...
```

**这是最关键的判据**，因为 `UE_LOG` 不受屏幕消息 0.2 秒时效影响，而且 Warning 级不会被过滤掉。

### ② `BP_ShooterNPC` 里 `Auto Pilot` 现在是勾着还是取消的

如果取消着 → 说明 `AutoPilot=OFF` 是假象，Task 可能压根没跑。

### ③ 这条警告出现几次、什么时候

- Play 一开始就出现 → 有敌人的 StateTree 启动失败（更严重）
- 隔一阵才出现/敌人死了才出现 → 是死亡噪音，可忽略

---

## 五、如果你确认 Task 真没跑，我建议的修改方案

> 以下**暂不执行**，等你确认后再动手。

### 方案 1（诊断优先）：给 Task 的 Tick 加"无条件下探针"

**改哪里**：`StateTreePursueTask.cpp` 的 `Tick()` 开头
**改什么**：在函数最前面加一行 `ShowDebug(Pawn, FColor::White, TEXT("[PursueTask] Tick alive"))`，放在所有 return 之前
**为什么**：现在 Tick 里有多个 early return（拿不到 AIC、拿不到 Target、进射程停下），任何一个都可能让打印被跳过。加一行"进门就打"的探针，能确定 Tick 到底有没有被调用。

### 方案 2（配置排查）：StateTree 调试器

**改哪里**：不改代码
**做什么**：Play 时 `Tools → Debug → StateTree`，看敌人当前激活的是哪个状态、哪些 Task 在跑
**为什么**：这是引擎自带的可视化，能直接看到"状态有没有进 Attack""Task 有没有启用"，比看屏幕文字可靠得多。

### 方案 3（保底）：暂时切回组件 AutoPilot

**改哪里**：`BP_ShooterNPC` 的 `Pursue Steering` 组件勾回 `Auto Pilot`
**为什么**：三处设计点里还有两个没验证。Task 架构是"更正确"，但不验证能力，架构再正确也没内容可讲。可以先把能力验完录完 demo，再回头收拾架构。

---

## 六、我的判断（供参考）

1. 这条 `StopLogic` 警告**大概率是敌人死亡时的噪音**，不必恐慌。
2. `AutoPilot=OFF` 极可能是**蓝图里保存的默认值**，不能作为"Task 已生效"的证据。
3. 真正的判据只有一个：**Output Log 里有没有 `[PursueTask] EnterState`**。
4. 如果确实没有，我倾向先走**方案 3**（切回组件把能力验完），因为已经在这条链上花了过多时间，而 demo 还没开始录。

**请你先给我上面那三条信息，我再决定动哪里。**
