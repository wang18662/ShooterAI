# AI 验证测试卡（两项）

> 目标：用 15 分钟把三处设计点里剩下的两个亲手验完。
> 武器已删除，玩家空手 —— **不影响这两项测试**（都是看敌人行为，不需要你开枪）。

---

## 第 0 步 · 先让玩家不死（1 分钟）

跑 `DiagAndFix.py`（打开 `Lvl_AI_Full` → Tools → Execute Python Script）。

它会：
- 把 `BP_ShooterCharacter` 的 `MaxHP` 改成 `999999`，并**自动编译 + 保存蓝图**
- 同时打印每个敌人的 `Pursue Steering` 实际参数（下面诊断 `-1` 要用）

> `MaxHP` 默认 500，`BeginPlay` 里 `CurrentHP = MaxHP`，所以改这一个就够。
>
> **如果脚本没找到血量字段**：手动打开 `BP_ShooterCharacter` → 细节面板搜 `Health` → 把 `Max HP` 改成 `999999` → Compile + Save。
>
> **临时应急**（不改任何东西）：PIE 里按 `~` 输入 `God` 回车。但不是所有 PlayerController 都支持。

---

## 关于「最近同伴 = -1」：这是我的代码的哨兵值

`-1` 不是错误，是我在 `ComputeSeparationOffset()` 开头写的初始值：

```cpp
NearestAllyDistance = -1.0f;   // -1 = 附近没有同伴
```

它只在函数**真正跑起来并找到邻居**时才被更新。显示 `-1` 说明**函数根本没跑**。两个原因，按可能性排序：

### 原因 A：`Stop Distance` 还是 250（最可能）

代码顺序是这样的：

```cpp
if (StopDistance > 0 && Distance <= StopDistance)
{
    AIC->StopMovement();
    return true;          // ← 这里直接返回了
}
// 下面才有 ComputeSeparationOffset()
```

敌人一靠近到 2.5 米就 `StopMovement` + `return`，**分离计算完全没执行**，`NearestAllyDistance` 永远是初始的 `-1`。

**这就是为什么必须把 `Stop Distance` 改成 `0`** —— 不只是为了测准，还是为了让分离代码有机会执行。

### 原因 B：`Separation Radius` 实例值是 0

`Separation Radius` 是我新加的属性。蓝图里已存在的组件实例可能没拿到默认值 220，而是 0。
函数开头 `if (SeparationRadius <= 0) return;` 也会让 `NearestAllyDistance` 保持 `-1`。

**检查方法**：`DiagAndFix.py` 的日志会打印出来：
```
[DIAG]     SeparationRadius       = 220.0     ← 如果是 0，就是这里的问题
[DIAG]     StopDistance           = 250.0     ← 需要改成 0
```

---

## 测试 1 · 群体分离（F 区长旷野）

### 前置：把 Stop Distance 改成 0

打开 `BP_ShooterNPC` → 选中 `Pursue Steering` 组件 → **`Stop Distance`** 改成 **`0`**。

确认 `Separation Radius` 是 **`220`**（不是 0）。

改完 **Compile + Save**，然后 Play。

> **为什么必须改**：默认 250 意味着敌人进入 2.5 米就停下开火。六个敌人全停在同一个半径上，
> 彼此的间距由「圆周几何」决定，不是分离算法决定 —— 测不出分离到底有没有工作。
> 更严重的是：`StopMovement` 之后代码 `return` 了，分离计算压根没执行，所以显示 `-1`。
> 改成 0 后它们会持续追，分离就成了唯一能把它们分开的因素。

### 怎么测

1. 按 `F8` 飞到 F 区长旷野（大约 `-7000, 1000`）
2. 落地站住别动，等 6 个敌人从东西两端跑过来
3. 等它们围住你，**读屏幕上 6 行文字末尾的数字** `最近同伴xxx`

### 记录

| 条件 | 最近同伴（读 3 秒，记典型值） |
|---|---|
| `Use Separation` ✅ | ______ |
| `Use Separation` ❌ | ______ |

**通过标准**：开 ≥ 200，关 < 120。

### 反证做法

回 `BP_ShooterNPC` → 取消勾选 **`Use Separation`** → Compile + Save → Play → 同样场景再读一次。

---

## 测试 2 · 记忆与搜索（A 区开阔广场）

### 前置

把 `Stop Distance` 改回 **`250`**（不然敌人会一直贴着你，不好观察），`Use Separation` 勾回来。

### 怎么测

1. 回 A 区 `(-9000, 6500)` 附近
2. 让一个敌人看见你 —— 屏幕出现**绿色** `PURSUE d=...`
3. 立刻绕到某根柱子后面（柱子位置 `-10000/-8000, 7500/5500`），**停住不动**
4. **盯着屏幕颜色**

### 记录

| 顺序 | 期望颜色 | 期望文字 | 实测 |
|---|---|---|---|
| 1 | 🟢 绿 | `PURSUE d=xxx` | |
| 2 | 🔵 青 | `CHASE-MEMORY d=xxx 去最后已知位置` | |
| 3 | 🟣 品红 | `SCAN x.x/4.0s 原地搜寻` | |
| 4 | 🟡 黄 | `IDLE 交还状态树` | |

**通过标准**：四色按顺序出现，青色持续到敌人走到柱子后，品红持续约 4 秒。

### 反证做法

`BP_ShooterNPC` → 取消勾选 **`Use Memory`** → Compile + Save → 重复同样操作。

**预期**：绿色直接跳黄色，青色和品红完全不出现 —— 这就是模板的原始蠢样。

---

## 报结果时给我这三个信息

1. F 区分离：开 = ____，关 = ____
2. A 区颜色：实际看到了哪几种（按顺序）
3. 有没有出现卡住/抖动/穿墙

**数字比形容词有用。** "最近同伴 220" 比 "它们没挤在一起" 精确十倍，面试时也是这么讲。
