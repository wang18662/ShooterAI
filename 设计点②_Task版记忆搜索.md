# 设计点② 记忆与搜索：StateTree Task 版

> 把三档状态机从组件（`AutoPilotTick`）移植成 StateTree Task 的内部阶段机。
> 代码：`Source/ShooterAI/StateTreePursueTask.h/.cpp`

---

## 一、为什么移植

架构切到 StateTree 之后，`AutoPilotTick` 被关闭（Task 的 `EnterState` 会设 `bAutoPilot = false`），
于是设计点② 的三档逻辑**不再执行**。

| 能力 | 组件模式 | Task 模式 |
|---|---|---|
| ① 预判追击 | ✅ `AutoPilotTick` 调 | ✅ Task 直接调 `UpdatePursue` |
| ② 记忆与搜索 | ✅ `AutoPilotTick` 内 | ❌ **不执行** → 本次移植解决 |
| ③ 群体分离 | ✅ `AutoPilotTick` 内 | ✅ Task 直接调 `ComputeSeparationOffset` |

---

## 二、阶段机设计

```
看得见 ──→ PURSUING（绿）  追预判点，同时刷新 LastKnownLocation
   │
   ↓ 看不见
有记忆 ──→ CHASING（青）   赶去最后已知位置
   │
   ↓ 到达 SearchAcceptanceRadius 内
           SCANNING（品红） 原地转圈，120°/秒
   │
   ↓ 扫满 ScanDuration 或 失联超 MemoryDuration
           return Failed ──→ 状态机接管（回漫游 / 去 Investigate Location）
```

### 参数（StateTree 编辑器里可调）

| 参数 | 默认 | 作用 |
|---|---|---|
| `Use Memory` | ✅ | 关掉 = 模板原始行为（一丢就走） |
| `Memory Duration` | 10s | 记忆保留时长，超过彻底放弃 |
| `Chase Timeout` | 4s | **追赶超时**：追不到就就地搜索（见下方「坑」） |
| `Scan Duration` | 4s | 原地环视多久 |
| `Search Acceptance Radius` | 150cm | 多近算「到了」 |
| `Scan Yaw Speed` | 120°/s | 环视转速 |

### 踩过的坑：记忆点不可达 → 卡在 CHASING 到超时

**症状**：敌人走到某个位置后停住不动，**没有紫色 SCANNING**，10 秒后直接 GIVE-UP 退出。

**根因**：

```
你贴着柱子消失 → 记忆点落在柱子另一侧
        ↓
敌人绕到柱子这一侧，最近只能到 2D 距离 200cm
        ↓
MoveToLocation 每帧失败（不可达），敌人停住 ——— 且不报错
        ↓
200 > SearchAcceptanceRadius(150) → 永远停在 CHASING 分支
        ↓
只能等 MemoryDuration 到期被 GIVE-UP
```

`MoveToLocation` 失败不报错是 UE 的既有行为（排查 spawner 时也遇到过）。

**修法**：加 `ChaseTimeout`（默认 4 秒）。追不到就在**当前位置**开始扫描，而不是卡死。

时序设计：`ChaseTimeout 4s + ScanDuration 4s = 8s < MemoryDuration 10s`，
保证能在记忆到期前走完「追 → 搜 → 放弃」完整流程。

**行为变化**：从「追不到就卡住」变成「追不到就就地搜一轮再放弃」——
后者才是真实游戏里 AI 的做法。

---

## 三、四个设计判断（面试可讲）

### ① 记忆数据放 `InstanceData`，不放组件

组件是所有敌人**共享**的算法库。记忆如果存在组件成员里，15 个敌人会互相覆盖。

`InstanceData` 由 StateTree 按「每个树实例 + 每个 Task」分配，天然是**一敌一份**。

> 同理，`TickCount` 也不能用 `static` —— static 全局共享，
> 第一个敌人打过日志后其他敌人就不打了，排查时会误以为只有一个敌人在跑。

### ② 放弃时返回 `Failed`，不返回 `Succeeded`

`Failed` 才会触发状态机的 **`On State Failed`** 转换。
这样「放弃后去哪」（回漫游 / 去 `Investigate Location`）由你在 StateTree 里配，**代码不写死决策**。

这是「Task 只管移动，状态机管决策」这条分层的具体体现。

### ③ 环视用转向，不用随机移动

| 方案 | 玩家观感 | 开销 |
|---|---|---|
| 转圈 | 明确的「我在找」 | 零寻路 |
| 随机走 | 会被误认成「又在巡逻」 | 每帧寻路 |

### ④ 判定「到没到」要把分离偏移算进去

```cpp
if (DistToMemory > SearchAcceptanceRadius + SepOffset.Size())
```

**不算偏移会出 bug**：被同伴推着的敌人永远进不了 150cm 范围，**永远不开始扫描**，
看起来像「它到了却站着不动」。

---

## 四、验证（四色流转）

### 准备

1. `BP_ShooterNPC` 的 `Pursue Steering` 组件 → **取消勾选 `Show Diagnostics`**（否则 15 个敌人的刷屏会淹没 Task 的文字）
2. Play，走到某个敌人面前让它看见你

### 步骤

| 步骤 | 操作 | 期望屏幕 |
|---|---|---|
| 1 | 站在敌人视野内 | 🟢 `PURSUING d=... pred=(...)` |
| 2 | **快速绕到柱子后面，停住不动** | 🔵 `CHASING d=... 去最后已知位置（失联 x.xs）` |
| 3 | 等它走到你消失的位置 | 🟣 `SCANNING 2.0/4.0s 原地搜寻` |
| 4 | 等 4 秒 | 日志：`搜索结束：原地环视 4.0s 未找到目标` |

**四种颜色按顺序出现 = 设计点② 在 Task 架构下完成。**

### 反证（证明参数真的生效）

| 做法 | 期望 |
|---|---|
| 取消勾 `Use Memory` | 银灰 `LOST（未启用记忆）` → 敌人直接放弃（模板原始行为） |
| `Scan Duration` 改 1 秒 | 品红只闪 1 秒就放弃 |
| 躲超 `Memory Duration`（10 秒） | 途中直接 `GIVE-UP`，连扫描都不进 |

---

## 五、日志对照表

| 日志 | 含义 |
|---|---|
| `EnterState ... 进入 PURSUING` | 状态进入 |
| `第 1 次 Tick 进来了` | Tick 真的在跑 |
| `Tick 失败：xxx` | 某个校验没过 |
| `放弃追踪：失联 x.xs 超过记忆上限 10.0s` | 记忆过期 |
| `搜索结束：原地环视 4.0s 未找到目标` | 扫描超时 |
| `ExitState ... 阶段=SCANNING 退出状态=Failed` | 确认是「我们主动放弃」而非被转换打断 |

`ExitState` 那行的 `退出状态` 是关键：

- `Failed` = 我们主动放弃（记忆过期 / 搜索超时）
- `Running` = 被状态机转换打断（比如转头看见了玩家，转去 Attack）

---

## 六、架构上的完整图景

```
StateTree（决策层）
  ├─ Roam / Attack Enemy / Investigate Location ... 决定"现在该干什么"
  └─ Chase 状态
        └─ Pursue Steering Task（本次实现）
              ├─ PURSUING / CHASING / SCANNING  三档内部推进
              └─ 放弃时 return Failed，把决策交还状态机

PursueSteeringComponent（算法层，纯计算）
  ├─ UpdatePursue()            → 预判点
  ├─ ComputeSeparationOffset() → 分离偏移
  └─ ProjectToNavMesh()        → 可达性校验

CharacterMovement（执行层，引擎）
  └─ 真正位移
```

**三层各归其位**：决策在状态机、算法在组件、执行在引擎。
Task 是连接决策与算法的**胶水层**，本身不含决策逻辑，也不含数学。

---

## 七、面试话术

> **关于记忆搜索为什么做在 Task 里**
>
> 我最初把它做成组件内的三档状态机，后来重构进 StateTree Task。原因是架构一致性：如果在组件里维护阶段，状态机就不知道敌人在干什么，调试时会看到"状态显示漫游、实际在搜索"。
>
> 移植时有个判断值得说：**放弃时我返回 `Failed` 而不是 `Succeeded`**。因为 `Failed` 会触发状态机的 `On State Failed` 转换，这样"放弃后去哪"就变成了状态机里的配置，而不是代码里写死的。Task 只负责移动，决策权留给状态机——这是分层的具体体现。
>
> 还有个小 bug 值得提：判定"到没到记忆点"时，必须把群体分离的偏移量算进去。否则被同伴推着的敌人永远进不了判定半径，永远不开始扫描，看起来像"它到了却站着不动"。
