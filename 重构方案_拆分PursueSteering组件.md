# 重构方案：拆分 PursueSteeringComponent

> ## ✅ 已执行（2026-10-22 03:35）
>
> **方案 A 第 1–4 步全部完成，一次编译通过。**
>
> | 文件 | 拆分前 | 拆分后 |
> |---|---|---|
> | `PursueSteeringComponent.cpp` | 1,336 行 | **505 行** |
> | `PursueSteeringComponent_Health.cpp` | —— | 329 行 |
> | `PursueSteeringComponent_Cover.cpp` | —— | 197 行 |
> | `PursueSteeringComponent_Legacy.cpp` | —— | 475 行 |
> | `PursueSteeringComponent.h` | 726 行 | 800 行（新增 74 行导航图与说明） |
>
> **验证结果**：
> - 函数定义 **29 个 → 29 个**，无重复、无丢失
> - 花括号 **154 / 154** 平衡
> - `Result: Succeeded`，DLL **517.5 KB → 517.5 KB**（无增删功能，仅重组）
> - 每个 `.cpp` 开头都有 30–45 行说明：**我是谁 / 为什么单独存在 / 不在这里的去哪了**
> - `.h` 头部 74 行章节地图，并按 9 节重新编号（修掉三处重复的 ④⑤⑥）
>
> **原始文件备份**：`D:\UnrealProjects\ShooterAI\_backup_before_split\`
>
> 下面保留的是决策时的完整分析，作为"为什么这样做"的记录。

---

> **目标文件**：`PursueSteeringComponent.cpp`（**1,336 行**）+ `.h`（**726 行**）
> 全项目最大的单文件，也是职责最混杂的一个。
>
> 本文档给出三种方案、逐行归属、执行步骤、验证方法，以及**我建议做到哪一步就停**。

---

## 一、为什么要动它

### 1.1 客观证据（不是"感觉乱"）

**证据一：章节编号重复**

文件里有 **两个 ⑤、两个 ⑥、两个 ④**：

| 编号 | 第一处 | 第二处 |
|---|---|---|
| ④ | 第 131 行 血量档位 | 第 415 行 群体分离 |
| ⑤ | 第 203 行 回血道具 | 第 451 行 到达减速 |
| ⑥ | 第 292 行 掩体探测 | 第 484 行 碰撞规避 |

这不是笔误，是**分批添加留下的化石** —— 每次加功能时复制了上一段的编号模板。它本身就证明：**这个文件没有统一的结构规划。**

**证据二：31% 的代码不在执行路径上**

| 区块 | 行数 | 状态 |
|---|---|---|
| AutoPilot 旧旁路（852–1024） | 173 | 默认关闭（Task 里强制 `bAutoPilot = false`） |
| Arrive / CollisionAvoidance / Wander（1092–1336） | 245 | **零调用**（全项目 grep 只有声明和定义） |
| **合计** | **418 行** | **占 31%** |

**证据三：一个文件承担 6 种不同性质的职责**

| 性质 | 举例 | 变了会影响 |
|---|---|---|
| 数学计算 | `GetPursueTargetLocation` | 什么都不影响（纯函数） |
| 几何查询 | `IsInCover`、`FindCoverPointEx` | 只有掩体逻辑 |
| 反射读蓝图数据 | `GetHealthPercent` | 所有依赖血量的判断 |
| 状态存储 | `RemainingHealItems` | 跨整局的游戏状态 |
| 跨 Task 信号 | `bSuppressFire` | 开火行为 |
| 引擎桥接 | `MoveTo`、`FaceLocation` | 旧旁路通道 |

**这六种东西的"修改风险"完全不同**（纯函数改了能立刻验；反射改错了是静默失败）。混在一起意味着**改一个小地方也要重新理解整个文件**。

---

## 二、三种方案对比

| | **A. 拆成多个 .cpp** | **B. 拆成多个组件** | **C. 只加导航注释** |
|---|---|---|---|
| 做法 | 同一个类，成员函数分散在 4 个 `.cpp` | 4 个独立 `UActorComponent` | 文件不动，顶部加行号地图 |
| 蓝图要改 | ❌ 不用 | ✅ **必须手动加组件** | ❌ 不用 |
| 调用点要改 | ❌ 不用（接口不变） | ✅ **52 处**（4 个消费文件） | ❌ 不用 |
| 行为会变 | ❌ 不会 | ⚠️ 可能（静默失效风险高） | ❌ 不会 |
| 最大文件 | **1,336 → 469 行** | ~300 行 | 1,336 行（不变） |
| 真正解决 SRP | ❌ 只解决"文件太大" | ✅ 是 | ❌ 否 |
| 工作量 | 40 分钟 | 2–3 小时 + 反复编译 | 10 分钟 |
| 风险 | **极低** | **高** | **零** |

### 为什么 B 的风险远高于表面

**C++ 允许同一个类的成员函数写在不同的 .cpp 里。** 所以 A 能"减小文件"而**不碰任何接口**。

B 做不到这一点，它必须改接口，而这里有个**无法用代码绕过**的环节：

> 新增的组件必须**在蓝图 `BP_ShooterNPC` 里手动添加**。
> 这是编辑器操作，不是代码能完成的。
> 而且漏掉的后果是**静默失效** —— 敌人不报错、不崩溃，只是某些功能突然不工作。

这个项目在蓝图脚本化上已经踩过坑：`FixPickups.py` / `FixPickups2.py` / `FixPickups3.py` 三次尝试全失败，最终结论是「**蓝图类默认值用编辑器 UI 改，别用脚本**」。

**所以 B 不是"风险高"，而是"有一道必须靠人手跨过去的坎"。**

---

## 三、方案 A 的逐行归属（可直接执行）

### 3.1 拆分结果

```
PursueSteeringComponent.cpp          469 行   核心：生命周期 + 算法 + 桥接
PursueSteeringComponent_Health.cpp   285 行   血量 + 回血 + 受伤 + 提速
PursueSteeringComponent_Cover.cpp    155 行   掩体探测
PursueSteeringComponent_Legacy.cpp   418 行   旧旁路 + 3 个未接线算法
─────────────────────────────────────────────
合计                                1,327 行（原 1,336，差异是分隔线与注释）
```

**`.h` 保持不变**（726 行）—— 类定义只能在头文件里，不能拆。但要在头部加「导航地图」并修掉重复编号。

### 3.2 具体归属表

| 目标文件 | 迁入内容 | 原行范围 | 行数 |
|---|---|---|---|
| **`_Health.cpp`** | `HandleAnyDamage`、`IsWounded`、`GetTimeSinceDamaged` | 58–96 | 39 |
| | `GetHealthPercent`（含反射属性查找）、`IsLowHealth`、`IsCriticalHealth`、`ApplyFleeSpeed` | 98–273 | 176 |
| | `ConsumeHealItem`、`ApplyHeal`（含反射写入） | 275–344 | 70 |
| **`_Cover.cpp`** | `IsInCover`、`FindCoverPoint`、`FindCoverPointEx` | 346–500 | 155 |
| **`_Legacy.cpp`** | `AutoPilotTick`（+ `TickComponent` 里的旁路调用说明） | 852–1024 | 173 |
| | `ComputeArrive`、`ComputeCollisionAvoidance`、`ComputeWander` | 1092–1336 | 245 |
| **主 `.cpp` 保留** | 构造函数 + `BeginPlay` + 停火开关内联实现 | 1–57 | 57 |
| | `GetPursueTargetLocation`、`GetPredictionTime`、`ProjectToNavMesh`、`UpdatePursue`、`GetPursueSteering` | 503–670 | 168 |
| | `ResolveAIController`、`CanSeeTarget`、`FaceLocation`、`MoveTo` | 672–742 | 71 |
| | `ComputeSeparationOffset`、`ClearMemory` | 744–850 | 107 |
| | `TickComponent` | 1025–1090 | 66 |
| **`.h` 不动** | `GetOriginalWalkSpeed`、`GetHealthProbeLog` 等内联函数 | `.h` 178 / 187 | —— |

**注意 `TickComponent`（1025–1090）留在主文件**：它只有一句 `if (bAutoPilot) AutoPilotTick(Target);`，
调用被迁走的函数是允许的（同类的成员函数，链接期解析），**不需要跟着搬**。

### 3.3 执行步骤（4 步，每步可单独编译验证）

| 步 | 动作 | 编译验证点 |
|---|---|---|
| **1** | 新建 `PursueSteeringComponent_Cover.cpp`，把 346–500 剪切过去，加 `#include "PursueSteeringComponent.h"` | 编译应通过；掩体行为不变 |
| **2** | 新建 `_Health.cpp`，剪切 58–96 / 98–273 / 275–344 | 编译应通过；血量/回血不变 |
| **3** | 新建 `_Legacy.cpp`，剪切 852–1024 / 1092–1336 | 编译应通过；行为完全不变（本来就没跑） |
| **4** | 主 `.cpp` 加文件头导航注释；`.h` 修重复编号 + 加章节地图 | 只改注释，无编译影响 |

**关键操作细节**：

> 每次剪切后，**主 `.cpp` 里不能再留同名定义**，否则 linker 报 `already defined`。
> 反过来如果**漏剪**，也是同样的报错 —— 所以编译报 `LNK2005` 时，去 grep 同名函数即可定位。

### 3.4 `.h` 的头部导航地图（阶段 4 产出，示例）

```cpp
// ============================================================
// 本文件章节地图（按职责，不按添加顺序）
// ============================================================
//  ① 算法核心        GetPursueTargetLocation / UpdatePursue       纯计算，可单独验
//  ② 群体分离        ComputeSeparationOffset                      纯计算，可单独验
//  ③ 掩体探测        IsInCover / FindCoverPointEx  → _Cover.cpp    几何查询
//  ④ 血量与回血      GetHealthPercent / ApplyHeal  → _Health.cpp   反射读写，静默失败
//  ⑤ 受伤谨慎        IsWounded / WoundedCautionDuration → _Health.cpp
//  ⑥ 跨 Task 信号    bSuppressFire / SetFireSuppressed             给 ShootControl 读
//  ⑦ 引擎桥接        MoveTo / FaceLocation / ProjectToNavMesh     与引擎握手
//  ⑧ 旧旁路（停用）  AutoPilotTick                 → _Legacy.cpp    Task 模式下强制关闭
//  ⑨ 算法储备（未接线）Arrive / CollisionAvoid / Wander → _Legacy.cpp  零调用，见注释说明
// ============================================================
```

---

## 四、方案 B 如果真要做（仅供将来参考）

### 4.1 目标组件划分

| 组件 | 职责 | 依赖 |
|---|---|---|
| `UPursueSteeringComponent` | **保留**：Pursue 算法 + Separation + NavMesh 投影 | 无 |
| `UAIHealthStateComponent` | 血量读取、档位判定、道具、回血、受伤谨慎 | 反射读蓝图 |
| `UAICoverSensorComponent` | 掩体射线探测 | 只需 World |
| `UAIFireGateComponent` | `bSuppressFire` 跨 Task 信号 | 无 |

### 4.2 必须手工完成的蓝图操作

| 蓝图 | 要做什么 |
|---|---|
| `BP_ShooterNPC` | 添加 `AIHealthState`、`AICoverSensor`、`AI Fire Gate` 三个组件 |
| 同上 | **删掉**旧 `Pursue Steering` 上已被移走的参数（否则面板上出现两套同名参数，极易混淆） |

**易错点**：如果忘了把 `HealItemCount` 从旧组件复制到新组件的默认值，敌人开局道具会变成 **0**（而不是 2）→ 表现为"低血量直接拼死，永远不回血"。**这类错误不报错，只看行为异常。**

### 4.3 调用点清单（**52 处**，改漏即静默失效）

| 消费文件 | 调用行数 | 用到的接口 |
|---|---|---|
| `StateTreePursueTask.cpp` | **43 处** | 血量判定 4 / 回血 7 / 掩体 3 / 算法 2 / 投影 6 / 提速 6 / 停火信号 3 / 其他 12 |
| `AIPathVizComponent.cpp` | **6 处** | `IsCriticalHealth`、`IsLowHealth`、`HasHealItem`、`CurrentMode`×2、`CurrentPredictedLocation` |
| `StateTreeShootControlTask.cpp` | **2 处** | `bSuppressFire`、`FireSuppressedReason` |
| `AILodComponent.cpp` | **1 处** | `CurrentMode` |
| **合计** | **52 处** | —— |

> 统计口径：grep `Comp->` / `CachedPursue->` / `PursueComp->`。
> **注意排除**同名的 `LodComp->`（`AILodSubsystem` 里 2 处是 LOD 组件，不是它）。
>
> **另有两个接口是内联在 `.h` 里的**，不会随 `.cpp` 拆分而迁移：
> `GetOriginalWalkSpeed()`（`.h` 第 178 行）、`GetHealthProbeLog()`（第 187 行）。
> 这也解释了为什么在 `.cpp` 里 grep 不到它们的定义 —— **别误以为漏了函数体。**

### 4.4 折中：只拆最独立的那一块

如果确实想验证"拆分可行"，**先只拆 `UAIFireGateComponent`**：

- 它的调用点只有 **2 处读 + 3 处写 = 5 个**
- 它没有参数、没有算法、只有一个 bool + 一个字符串
- 拆它能完整走通一遍"改代码 → 改蓝图 → 验证"的流程

**如果这一步都嫌麻烦，就说明整个 B 方案不该做。**

---

## 五、我的建议：做到哪一步就停

### 推荐路径

| 阶段 | 做什么 | 何时做 | 理由 |
|---|---|---|---|
| **0** | 先**录 demo + 写简历** | **现在** | 当前状态是可交付的；重构不产生 demo 价值 |
| **1** | 方案 A 的第 1–3 步（拆 .cpp） | demo 之后 | 40 分钟，风险极低，立刻降低阅读成本 |
| **2** | 方案 A 第 4 步（导航图 + 修编号） | 随时 | 10 分钟，纯注释 |
| **3** | 方案 B（拆组件） | **建议不做** | 需要手工改蓝图，收益（远期可维护性）与作品集目标不匹配 |

### 为什么建议中止在 B 之前

**理由一：这套系统今天刚跑通。**

11 项能力全部验证过，其中包括三个花了整晚才定位的逻辑 bug。拆组件会重新触碰 52 个调用点 —— 而这类改动引入的 bug **和刚修完的那三个是同一类**：不报错、只看行为异常。

**理由二：作品集的价值已经实现了。**

项目要证明的是"我能做出有行为的 AI"，不是"我的代码架构完美"。执行 B 对观众零收益。

**理由三：能讲清"为什么不做"比"做了"更能体现判断力。**

面试可以这么说：

> "这个组件有 1336 行，确实职责混杂——里面有 6 种不同性质的东西，还有 418 行不在执行路径上。
>
> 我做了两件事：把 418 行不参与执行的拆到独立文件并标注清楚，把重复的章节编号重新整理成职责地图。
>
> 但我**没有**把它拆成 4 个组件。原因很具体：拆组件需要在蓝图里手工添加新组件，而漏掉的后果是静默失效——不报错、只表现为某个功能突然不工作。这个项目在蓝图脚本化上踩过三次坑，结论是这类操作必须人工在编辑器里做。
>
> 判断依据是：这个项目要交付的是"能演示的 AI 行为"，不是长期维护的框架。**在 demo 还没录的时候动 52 个调用点，风险收益比不成立。**"

**这段话证明了四件事**：知道问题在哪、能量化、知道怎么修、**以及知道什么时候不修**。

---

## 六、如果决定执行，验证清单

每步编译通过后，按这张表逐项确认（**不要只看编译通过**）：

| 拆完哪一步 | 必须验证的行为 | 看什么 |
|---|---|---|
| 步 1（掩体） | 打到 25%，它逃向柱子并蹲下回血 | 头顶 `HEALING HP=xx% 已回N` |
| 步 2（血量） | 血量 60% 变橙、25% 变红、打光道具变紫 | 头顶标签颜色 |
| 步 2（血量） | 回血到 60% 自动出战 | `回血完成 HP=61% → 恢复追击` |
| 步 3（Legacy） | **完全无变化**（它本来就没跑） | 上面几项照旧通过 |
| 步 4（注释） | 编译通过即可 | —— |

**如果步 1 或步 2 后行为出现任何变化**，说明剪切时漏了或重了函数体 —— 立即 `git diff` 或对同名函数 grep，不要继续往下拆。

---

## 七、一句话总结

> **这个文件的问题不是"太长"，而是"六种修改风险完全不同的东西被放进了同一个文件"。**
>
> 修法不需要拆类 —— **C++ 允许把成员函数分散到多个 .cpp，而类不变**。
> 这样能减小文件、分离职责，同时**不动蓝图、不动 52 个调用点、不改任何行为**。
>
> 而"拆成 4 个组件"虽然更彻底，但它有一道必须靠人手跨过去的坎（蓝图加组件），
> **在 demo 未录、系统刚跑通的时候，不值得。**
