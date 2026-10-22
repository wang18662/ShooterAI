# 诊断：完全没有 PursueTask 日志 = Chase 从未被进入

> 本文只做分析与方案，**不改任何代码**（代码这边已经没问题了）。

---

## 一、截图里的箭头方向说明了什么

从你给的 StateTree 截图，逐条读状态和转换：

| 状态 | 转换箭头 | 含义 |
|---|---|---|
| `Attack Enemy`（橙） | `→ ? → Root` | Attack Enemy 有一条**出口**到 Root，条件是 `?`（未绑定） |
| `Find Sniping Location` | `✓ → Next State`、`✗ → Wait at Sniping Location` | 成功去下一状态，失败去等待 |
| `Move to Sniping Location` | `→ Next State` | |
| `Wait at Sniping Location` | `→ Attack Enemy` | **回到 Attack Enemy**（循环） |
| `Investigate Location`（黄） | `→ ? → Attack Enemy, Move to Investigate Location, ...` | 出口到 Attack Enemy |
| **`Chase`（蓝）** | **`↗ Root`** | **Chase 有一条出口到 Root** |

### 关键观察

**所有箭头都是「从本状态指向别处」的出口。**

- `Attack Enemy` 的出口 → Root
- `Investigate Location` 的出口 → Attack Enemy
- **`Chase` 的出口 → Root**

**但没有任何状态有一条箭头指向 `Chase`。**

也就是说：

```
Roam / Attack Enemy / Investigate Location
        （没有任何一条转换指向 Chase）
                  ↓
            Chase 永远不会被选中
                  ↓
        EnterState 不执行 → 零日志
```

**这完全解释了「没有任何 PursueTask 警告或报错」。**

> 不是 Task 坏了，是它所在的状态压根没被激活过。
> 就像房间里的灯没坏，只是没人开开关。

---

## 二、第二个隐患：`Chase → Root` 这条出口

`Chase` 有一条指向 `Root` 的出口，箭头旁没有显示条件（和 `Attack Enemy` 一样标着 `?`）。

**如果这条转换是 `On Tick` 且条件为空/恒真，那么即使 Chase 被进入，也会在下一帧立刻弹回 Root。**

这正是你之前看到的另一幕：

```
[PursueTask] EnterState ...
[PursueTask] ExitState ...
```

（那时 Chase 是并入 Attack 的，但退出机制同类）

所以要做的事是**成对**的：**既要有进入条件，也要给出口加上正确的条件**。

---

## 三、解决方案

### 修法 A（必需）：给 Chase 加一条「进入」转换

**改哪里**：StateTree 编辑器，在 **`Attack Enemy` 状态**上

**改什么**：在 Attack Enemy 的 `Transitions` 里点 `+`：

| 字段 | 值 |
|---|---|
| Trigger | `On Tick` |
| Transition | `Goto State` |
| Target | **`Chase`** |
| Conditions | 加一条 `StateTree Object Is Valid`，输入绑 `Target`（感知任务的输出） |

**为什么加在 Attack Enemy 上**：Attack Enemy 是「已经发现目标」的状态。
发现目标后，用「目标有效的条件下转去 Chase」是最符合语义的接线——先追上去，再开打。

---

### 修法 B（必需）：给 `Chase → Root` 加上正确的条件

**改哪里**：`Chase` 状态自己的 `Transitions`

**改什么**：把现有那条 `→ Root` 的条件补上：

| 字段 | 值 |
|---|---|
| Trigger | `On Tick` |
| Transition | `Goto State` |
| Target | `Root`（保持） |
| Conditions | `StateTree Object Is **Not** Valid`（取反），输入绑 `Target` |

**为什么**：让 Chase 只在「看不见目标」时才退出。
否则它一进去就出来，你又会看到 `EnterState` → `ExitState` 零 Tick。

---

### 修法 C（可选，让行为完整）：Chase 到位后回到 Attack Enemy

**改哪里**：`Chase` 的 `Transitions`

**改什么**：再加一条：

| 字段 | 值 |
|---|---|
| Trigger | `On Tick` |
| Transition | `Goto State` |
| Target | **`Attack Enemy`** |
| Conditions | 距离判断，或复用 EQS 能否找到狙击位 |

**为什么**：这样形成完整闭环——远则追、近则打、丢则查。

---

## 四、最快的验证路径（如果只想先看到日志）

想立刻确认 Task 能跑通，可以先用**最小接线**：

1. 给 `Attack Enemy` 加一条 `On Tick` → `Goto State Chase`，**条件先不绑**（无条件）
2. 给 `Chase → Root` 的那条转换**加条件**：`Target Is Not Valid`
3. Play

预期日志：

```
[PursueTask] EnterState  Owner=...  Pawn=...
[PursueTask] 第 1 次 Tick 进来了（Pawn=...）
[PursueTask] 第 1 次 Tick 进来了...
```

**看到「第 1 次 Tick 进来了」就证明整条链路通了**，之后再把条件补精确。

> 无条件转换只是临时手段，验证完要补回条件，否则敌人一进 Attack Enemy 就转 Chase，永远不会站着开枪。

---

## 五、需要你确认

1. **`Chase` 现在有没有任何进入转换？**（截图里没看到，但可能被折叠了）
2. **`Chase → Root` 那条转换的 Trigger 和 Conditions 是什么？**（是 `On Tick` 吗？条件是空吗？）
3. 执行顺序：**先做修法 A + B（配对）**，还是先做最小接线（第四条）快速验通？

---

## 六、附带一个发现，值得你知道

模板里本来就有 **`Investigate Location`** 状态（黄色，指向 Attack Enemy），
它的语义是「去调查某个位置」——**和我们做的「搜索最后已知位置」高度重合**。

这意味着：

| 我们的设计点① | 模板已有的 |
|---|---|
| Chasing：去最后已知位置 | `Investigate Location` 状态（大概就是干这个） |
| Scanning：原地环视 4 秒 | ❓ 待确认有没有 |
| **会放弃**（`ScanDuration` 后放手） | ❓ 大概没有 |

**如果模板的 `Investigate Location` 已经做了「去最后位置查一下」，那我们的差异化重点应该落在「搜索的时长控制与放弃时机」上**，而不是「会不会去查」。

这一点需要你确认 `Investigate Location` 内部做了什么——它可能改变我们三处设计点的讲法。
