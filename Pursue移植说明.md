# Pursue 预判追击 · 移植说明

> 把 C++ 库 `D:\c++\game AI\game AI\game AI\core\ch03_movement\Pursue.h` 的算法移植进 Unreal，作为敌人 AI 的第一处差异化设计点。

## 一、本次做了什么

| 文件 | 作用 |
|---|---|
| `Source/ShooterAI/PursueSteeringComponent.h/.cpp` | 移植后的组件（核心交付） |
| `Source/ShooterAI/ShooterAI.h/.cpp` | 游戏模块入口 |
| `Source/ShooterAI/ShooterAI.Build.cs` | 模块依赖声明 |
| `Source/ShooterAI.Target.cs` + `ShooterAIEditor.Target.cs` | 编译目标 |
| `ShooterAI.uproject` | 从「纯蓝图」改成「C++ 模块」 |

## 二、算法核心 vs 胶水层（讲清设计的关键）

Pursue 的核心算法只有 4 行，**引擎无关**，直接照搬：

```cpp
direction  = target.position - character.position
distance   = direction.length()
speed      = character.velocity.length()
prediction = (speed <= distance / maxPrediction) ? maxPrediction : distance / speed
predictedPos = target.position + target.velocity * prediction   // 预测目标未来位置
```

这三处是「胶水」，必须重写：

| 源库（C++ 库） | Unreal 移植 | 为什么改 |
|---|---|---|
| `Vector3`（自研结构体） | `FVector` | UE 内置，等价 |
| `Kinematic character/target`（值成员） | 每帧读 `GetActorLocation()` / `GetVelocity()` | UE 角色数据在 Actor 上，实时读 |
| 输出「加速度 SteeringOutput」 | 输出「预测拦截点 FVector」 | UE 移动走 NavMesh；加速度直接积分会穿墙 |

**核心分工**：steering 算「该往哪追」，NavMesh MoveTo 算「怎么走过去」——两者正交。

## 三、挂载步骤

### 第 1 步：转 C++ 项目并编译

1. 关闭 UE 编辑器（如果 ShooterAI 开着）。
2. 双击 `D:\UnrealProjects\ShooterAI\ShooterAI.uproject`。
3. 弹「rebuild modules now?」→ 选 **Yes**（自动用 VS 编译）。
   - 没弹窗的话：右键 `.uproject` → **Generate Visual Studio project files**，再双击打开。
4. 首次编译较慢（几分钟），等右下角编译进度走完，编辑器打开。

> 环境已确认：VS 2022 + MSVC 14.44 已装（`D:\vs`），C++ 工作负载完整，可直接编译。

### 第 2 步：验证算法（最快看到效果，不碰模板 AI）

1. 打开竞技场关卡 `Content/Variant_Shooter/Lvl_ArenaShooter`。
2. 场景里选一个敌人（BP_ShooterNPC），打开它的蓝图。
3. 左上角 **Add Component** → 搜 **Pursue Steering** → 加上。
4. 在细节面板勾上 **Draw Debug**。
5. 打开事件图表（EventGraph）：
   - **Event Tick** → 拖出组件 → **UpdatePursue**，`Target` 接「玩家引用」。
6. 编译保存，Play。
7. **验证**：操控玩家**横向跑**，看敌人身上的红球是否「预判」到玩家前方（而不是停在玩家当前位置）。

> 预期：玩家横移时，红球落在玩家移动方向前方；玩家静止时，红球 ≈ 玩家位置。

### 第 2 步：最省事的接法（AutoPilot，不用连蓝图）

1. 打开 `Content/Variant_Shooter/Blueprints/AI/BP_ShooterNPC`
2. 左上角 **Add** → 搜 **Pursue Steering** → 加上
3. 选中组件，确认细节面板：
   - **Auto Pilot** ✅（默认已开，**务必确认显示为 true**）
   - **Show Diagnostics** ✅（默认开，屏幕左上会打印状态）
   - **Draw Debug** 可选（画球）
4. 编译 + 保存

**就这两步。** 不需要 Event Tick、不需要 Get Player Pawn、不需要连任何线。

> 如果屏幕左上显示 `AutoPilot=OFF`，说明蓝图实例里保存了旧值（C++ 改默认值不会更新已存实例）——手动勾上即可。

### 排查：看屏幕左上那行字

| 显示 | 含义 | 该怎么办 |
|---|---|---|
| `PURSUE d=1500 -> 预判点` | 接管成功 | 看敌人是否斜插 |
| `STOP+shoot d=200` | 进射程（<2.5m）停下开火 | 正常行为 |
| `no LOS -> 交还状态树` | 视线判定失败（只有勾了 CheckLineOfSight 才会出现） | 取消勾选 |
| `no AIController` | 敌人没被 AIController 接管 | 查 NPC 的 AI Controller Class |
| `no player pawn` | 找不到玩家 | 确认已 Play 且是玩家角色 |
| `AutoPilot=OFF` | 组件开关没开 | 细节面板勾上 |
| **完全没字** | 组件没 Tick | 检查组件是否真的加到了 NPC 上 |

`pred=(x,y,z)` 是当前预测点坐标，一直为 `(0,0,0)` = AutoPilot 没跑起来。


### 第 3 步（手动模式，如需精细控制）

关掉 AutoPilot，改在 EventGraph 里连：`Event Tick` → `Update Pursue(Target=玩家)` → 用返回的 `FVector` 自己接 `AI MoveTo`。

### 第 4 步（阶段 2 再做）：接进模板 StateTree

把 `ST_Shooter` 里「AI MoveTo 玩家」的目标点改成「预测点」。这步要动模板资产，留到阶段 2。

## 三-B、AutoPilot 的设计边界（面试会问）

**为什么只在「看得见」时才接管？**

StateTree 负责「该干什么」（漫游 / 搜敌 / 攻击），Pursue 负责「往哪追」。如果 Pursue 无条件接管，敌人就会永远直线追你，不再漫游、不再搜索——把上层决策吃掉了。

所以接管的门槛是 `LineOfSightTo(Target)`：**看得见才接管追击，看不见就还权给状态树**。这是「移动层覆盖决策层」的边界，不是简单的 if 判断。

**为什么用 `MoveToLocation` 而不是直接改速度？**

`MoveToLocation` 走 NavMesh 寻路，会绕墙；直接改速度是直线冲，必撞墙。这也印证了前面的分工：steering 算「往哪追」，NavMesh 算「怎么走过去」。


## 四、面试话术（三处设计之一）

> **预判追击（Pursue）**：官方模板敌人只会朝玩家「当前位置」直线追尾，玩家横向跑就永远追不上。我把《AI for Games》里的 Pursue 算法移植进来——每帧预测玩家「预测时间」后的位置，敌人朝这个拦截点追，实现斜插截断。核心是「算法核心 engine-agnostic 直接照搬、框架胶水（FVector/实时读位置/NavMesh 分工）重写」这层移植判断。

## 五、参数说明

| 参数 | 含义 | 默认 |
|---|---|---|
| `MaxPrediction` | 最大预测时间（秒），预判提前量上限 | 1.0 |
| `MaxNavProjectionDistance` | 预测点可接受投影距离（cm），超过即判不可达 | 150 |
| `bAutoPilot` | 组件自动接管追击（不用连蓝图） | true |
| `AutoPilotRetryInterval` | AutoPilot 检查间隔（秒），降频省开销 | 0.1 |
| `AutoPilotAcceptanceRadius` | AutoPilot 的到达半径（cm） | 100 |
| `StopDistance` | 进入该距离停下开火（0=一直追） | 250 |
| `bCheckLineOfSight` | 是否要求「看得见」才接管（关=无条件接管） | false |
| `bShowDiagnostics` | 屏幕左上打印诊断行 | true |
| `bDrawDebug` | 画球（红=预判点，黄=已锁人物） | false |
| `CurrentPredictedLocation` | 最近一次落点（只读） | - |
| `bLastPredictionValid` | 上次是否用了预判（false=锁人物） | - |

## 六、第二处设计点：预测点不可达时锁人物

**问题**：`Pursue` 本质是「把目标速度外推 t 秒」，它不知道墙在哪。玩家朝墙跑时，预测点会算到墙外甚至地图外，`MoveTo` 直接失败。

**解法（一条判断，两层语义）**：

```cpp
if (ProjectToNavMesh(Predicted, Projected))
    CurrentPredictedLocation = Projected;      // 可达 → 用预判点
else
    CurrentPredictedLocation = TargetPosition; // 不可达 → 锁人物当前位置
```

**为什么这么简单就够了**：
- 预判只在「开阔地带追人」时才需要额外聪明；贴墙跑时玩家本来也跑不直，追当前位置的损失很小。
- 校验失败的场景本身是少数（玩家贴墙/出界），此时回退到「更笨但一定对」的策略，比继续挣扎找折中点更划算。
- 采样收缩、物理视线兜底、多档重试这些是「把 1% 的场景做到 90 分」，而这条判断是「把 1% 的场景做到 60 分但不写错」——**功能不重要时，选后者**。

**调试**：红球=预判点，黄球=已锁人物。

**面试话术**：

> 移植 Pursue 时遇到书里不会提的问题：算法假设世界无障碍，玩家朝墙跑时预测点会落到墙外，`MoveTo` 直接失败。我先试过采样收缩 + NavMesh 投影 + 物理视线兜底的三层方案，后来砍成一条判断——**投影不到就锁人物当前位置**。理由是这个场景占比很低，回退到「笨但一定对」比精确处理更划算；过度设计反而是负担。这个取舍本身比代码更值得说。
