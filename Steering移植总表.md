# Steering 算法移植总表：9 选 5 的工程判断

> 源库：`game-ai-toolkit/src/gameai/core/steering.py`（Python，引擎无关）
> 目标：Unreal Engine 5.8 `PursueSteeringComponent`（C++，走 NavMesh）

---

## 一、总表

| # | 算法 | 代码状态 | 接线状态 | 用途 |
|---|---|---|---|---|
| 1 | `seek` | — | — | Pursue 内部就是 seek，不单独暴露 |
| 2 | `arrive` | ✅ 已实现 | ❌ **未接线** | 接近固定点减速（CHASING 分支可用） |
| 3 | `flee` | — | — | 敌人不逃跑，无场景 |
| 4 | `obstacle_avoidance` | — | — | **UE NavMesh 已内置**，手写会打架 |
| 5 | `pursue` | ✅ 已实现 | ✅ **已接线** | 预判追击（设计点①） |
| 6 | `evade` | — | — | 同 flee，无逃跑场景 |
| 7 | `separation` | ✅ 已实现 | ✅ **已接线** | 群体分离（设计点③） |
| 8 | `collision_avoidance` | ✅ 已实现 | ❌ **未接线** | 预测式避撞（比 separation 聪明） |
| 9 | `wander` | ✅ 已实现 | ❌ **未接线** | 平滑游荡（替代随机点直线走） |

### 状态要诚实区分

| 说法 | 含义 | 能不能说"生效" |
|---|---|---|
| **已实现** | 有完整算法代码 + 蓝图可调用的 `UFUNCTION` | ❌ 只能说"实现了" |
| **已接线** | 有 C++ 调用点，进入实际执行路径，能改变行为 | ✅ 可以说"生效" |

**当前事实**：
- `pursue` / `separation` —— **已实现 + 已接线**，demo 里能看到
- `arrive` / `collision_avoidance` / `wander` —— **已实现、未接线**，代码在但无人调用

> **为什么不再推进这三个**（2026-10-21 决定）：
>
> | 算法 | 不推进的原因 |
> |---|---|
> | `arrive` | UE 的 `PathFollowingComponent` 已内置接近减速，接了也是重复；视觉上**看不出来** |
> | `collision_avoidance` | 敌人不互相穿过是**物理碰撞 + NavMesh agent 半径**在起作用，观众看不出差别 |
> | `wander` | 漫游轨迹平滑与否在 demo 里**几乎不可辨**；且要新写 StateTree Task + 手动改资产，成本最高、收益最低 |
>
> **判断依据**：demo 时长只有 1 分钟，观众能记住的是「热力图」「帧率数字」这类强视觉信号。
> 花 40 分钟接一个看不出来的功能，不如把时间放在能演的画面上。
>
> **面试时怎么讲**：说"5 个算法移植，2 个接入执行路径"是诚实的；说"5 个都生效"会被追问穿。

---

## 二、为什么跳过这 4 个（这段比代码值钱）

### `obstacle_avoidance` —— 重复造轮子

源库的做法：沿期望方向采样 `is_walkable(x,y)`，被挡就产生侧向绕行力。

**UE 里不需要**，因为：

| UE 已有的 | 做了什么 |
|---|---|
| NavMesh + `MoveToLocation` | 已经算出一条绕开障碍的路径 |
| `FNavLocation` 投影 | 保证目标点可达 |
| 角色胶囊体 + 扫掠 | 物理层不穿墙 |

如果手写避障再叠加一个侧向力，会和 NavMesh 生成的路径**互相拉扯**——这正是 `separation` 移植时踩过的坑（改速度 → 穿墙 / 和寻路打架）。**同样的错误不该犯第二次。**

### `flee` / `evade` —— 没有使用场景

| 算法 | 需要什么场景 | 本项目有吗 |
|---|---|---|
| `flee` | 敌人受伤后逃跑 | ❌ 敌人是主动进攻型 |
| `evade` | 预判式逃离 | ❌ 同上 |

移植了也**演不出来**，demo 里拍不到，面试时讲不清"用在哪"。

如果以后做"血量低于 30% 撤退"，这两个可以直接拿来用——**代码在源库里是现成的，不移植不等于没有。**

### `seek` —— 已经是内部构件

```python
def pursue(...):
    ...
    return seek(pos, predicted, max_speed)   # ← pursue 内部就是 seek
```

`ComputePursueSteering()` 里已经有 seek 的逻辑，单独暴露一个 `ComputeSeek()` 只是给同一个公式起两个名字。

---

## 三、三个新移植的算法：核心 vs 胶水

### ① `arrive` 到达减速

**源库（6 行）**
```python
speed = max_speed * (dist / slow_radius) if dist <= slow_radius else max_speed
return to.normalized() * speed
```

**UE 移植的诚实定位**

⚠️ **Unreal 的 `PathFollowingComponent` 在接近目标时已经内置减速**（`bStopOnOverlap` + `AcceptanceRadius`）。

所以 `ComputeArrive` **不驱动移动**，它的价值是：

| 用途 | 说明 |
|---|---|
| 输出 `LastArriveSpeedRatio`（0~1） | 供上层做动画混合、或者状态机条件判断 |
| 可选的目标点收缩 | 需要提前刹车时，把目标点朝自己拉回，缩短最后一段路径 |

**这个判断本身就是移植的一部分**：源库的 `arrive` 是「给速度」，UE 已经有速度控制了，硬塞会打架。**承认它降级为观测工具，比假装完整移植更专业。**

**面试话术**
> "Arrive 我最后没让它驱动移动。因为 Unreal 的 PathFollowing 在接近目标时本来就减速，我再叠一层会和寻路打架。所以它降级成了一个观测值——输出当前应该用的速度比例，给动画和状态转换用。**移植不是照搬，得先看目标平台已经有什么。**"

---

### ② `collision_avoidance` 碰撞规避

**与 Separation 的本质区别（这是能讲的核心）**

| | Separation | CollisionAvoidance |
|---|---|---|
| 判据 | 距离近就推 | **预测未来是否真会撞** |
| 方向 | 远离邻居当前位置 | 远离**预测碰撞点** |
| 缺点 | 恐慌式，会误判（同伴只是路过也推） | 需要速度和计算 |
| 效果 | 防止重叠 | 精准让开，只躲最早会撞的那个 |

**核心公式（源库式 3.1）**

```
t = -dot(relPos, relVel) / |relVel|²
predicted = relPos + relVel × t
if |predicted| < 2r → 危险
avoidDir = -predicted.normalized()
```

**★ 那个不能漏的负号**

源库注释明确记了：

> 注意负号——C++ 参考实现漏了负号，导致「正迎面撞来」被判成「正在远离」而跳过。

**为什么漏负号是致命的**：
- `relPos` 指向对方，`relVel` 是接近速度
- 迎面撞来 → 两者方向**相反** → `dot < 0` → 加负号得**正数** → 正确判定「未来会撞」
- 漏掉负号 → 得负数 → 被判成 `t <= 0`「正在远离」→ **完全不躲，直接撞上**

**UE 差异**

| 源库 | UE 移植 | 为什么 |
|---|---|---|
| 输出速度向量 | 输出**目标点偏移**（方向 × `CollisionRadius`） | 走 NavMesh，不改速度 |
| 每帧遍历所有目标 | **降频** `CollisionCheckInterval = 0.1s` | 15 敌 × 每帧遍历 = O(n²)，225 次/帧会掉帧 |
| 遍历传入的列表 | `TActorIterator<APawn>` + 只取 AI 控制的 | 玩家不该被推 |

**降频的实现细节**（踩过坑）
用**世界时间戳**而不是累加器：
```cpp
const float Now = GetWorld()->GetTimeSeconds();
if (Now - LastCollisionCheckTime < CollisionCheckInterval) return Cached;  // 复用上次
LastCollisionCheckTime = Now;
```
累加器的问题：多个调用点各自减 `DeltaTime` 会重复扣除，时间戳没有这个问题。

**面试话术**
> "Collision Avoidance 和 Separation 我做成了两个独立能力，区别在判据：Separation 只看距离，Collision Avoidance 算最接近时刻 t = -dot(relPos, relVel)/|relVel|²，只躲真正会撞的那个。这个公式里有个负号特别容易漏——漏了以后「迎面撞来」会变成「正在远离」，AI 完全不躲。我在注释里标了这个坑，因为源库的 C++ 参考实现就踩过。"

---

### ③ `wander` 游荡

**源库思路**

```
在角色前方 offset 处放一个半径 radius 的圆
每帧在圆上随机取一点，朝它走
```

**为什么这样做而不是「随机选个点走过去」**

| 方案 | 轨迹 | 问题 |
|---|---|---|
| 随机点 + 直线 | 折线，突然转向 | 像机器，玩家一眼看出假 |
| **游荡圆** | 平滑曲线 | ✅ 因为圆始终在前进方向前方 |

**那个「二项分布近似」的小技巧**

```python
drift = (rng.random() - rng.random()) * wander_rate
```

两个 `[0,1)` 均匀随机**相减**，结果集中在 0 附近（三角分布），而不是均匀分布。

**为什么重要**：如果直接用 `uniform(-rate, rate)`，大角度偏转的概率和小角度一样高，轨迹会抖。相减之后，小偏转占多数，偶尔才大拐弯——**这才像真实的闲逛。**

这个技巧原样保留，是移植时唯一不需要改的东西。

**UE 差异**

| 源库 | UE | 为什么 |
|---|---|---|
| `random.Random(seed)` | `FRandomStream` | UE 内置，同样支持固定 seed → 行为可复现 |
| 输出速度 | 输出目标点 | 同上 |
| `heading` 由调用方传入 | 优先取 `GetVelocity()`，静止时退化到 `GetActorForwardVector()` | 静止 agent 没有速度方向，不能用零向量 |

**调用方要注意的**：别每帧都调 `ComputeWander()`，否则目标点持续微抖。建议 0.5~1 秒取一次，或者到达 `LastWanderTarget` 附近再取。

**面试话术**
> "Wander 我用的是「游荡圆」而不是随机点直线走。圆永远在前进方向前方，所以轨迹是平滑曲线。里面有个小技巧：漂移量用两个均匀随机数相减，结果集中在 0 附近，这样小幅度拐弯占多数、偶尔才大拐——比均匀分布自然得多。这个技巧我原样保留了。"

---

## 四、移植方法论（复用自 Pursue）

这套流程已经用了三次，是稳定的：

| 步骤 | 做什么 |
|---|---|
| **1. 分核心与胶水** | 标记哪些是引擎无关的数学（照搬），哪些是平台耦合（重写） |
| **2. 判断平台已有什么** | UE 有 NavMesh / PathFollowing / FRandomStream → 对应部分降级或替换 |
| **3. 转换输出语义** | 源库给「速度/加速度」→ UE 给「目标点/偏移」 |
| **4. 加观测字段** | 每个算法至少暴露一个可读数值，让"是否生效"可验证 |
| **5. 性能兜底** | 遍历类算法必须降频，O(n²) 在 15 个单位时会掉帧 |
| **6. 写清降级理由** | "为什么没完整照搬"比"我照搬了"更有说服力 |

---

## 五、验证方式

### `arrive`
- 观测：`LastArriveSpeedRatio`
- 预期：距离 > 400cm 时 = 1.0；距离 200cm 时 ≈ 0.5；距离 50cm 时 ≈ 0.125

### `collision_avoidance`
- 场景：两个敌人被吸引到同一个点，且运动方向相对
- 观测：`LastAvoidDirection`（非零 = 正在规避）、`AvoidTargetActor`（谁让我躲）
- 预期：正面相向时 `LastAvoidDirection` 非零；平行同向时为零（不会误判）

### `wander`
- 观测：`LastWanderTarget` + 画线
- 预期：轨迹是平滑曲线，不是折线；`WanderSeed` 固定时每次运行路径一致

---

## 六、一句话总结

> **9 个算法里我实现了 5 个，其中 2 个接入了执行路径。**
> **跳过的 4 个不是不会写，是因为 UE 已经有（obstacle_avoidance）、
> 或者这个项目用不上（flee / evade）、或者它已经是内部构件（seek）。**
> **实现的 5 个里只接了 2 个，因为剩下 3 个的视觉收益不足以抵消接线成本。**
> **判断「不该做什么」和写出代码一样是工程判断的一部分。**
