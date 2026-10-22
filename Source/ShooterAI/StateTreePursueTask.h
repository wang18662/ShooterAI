// StateTreePursueTask.h
// 把 PursueSteeringComponent 包成 StateTree Task。
//
// 为什么需要它：
//   之前组件是「旁路」接管移动——在 StateTree 外面抢 AIController 的 MoveTo 指令。
//   能跑，但状态机认为敌人在 Roam，实际却在追击，两者不一致，也不好调试。
//
// 现在改成「融入」：
//   StateTree 里加一个 Chase 状态，挂这个 Task。
//   组件只负责算预判点（算法），Task 负责下移动指令（驱动）。
//   决策、算法、执行三层各归其位。

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "StateTreePursueTask.generated.h"

class AActor;
class APawn;
class AAIController;
class UPursueSteeringComponent;

/**
 * Task 内部的三档阶段。
 *
 * 为什么放在 Task 里而不是状态机里：
 *   这三档的切换全部由「看得见 / 看不见 / 到没到 / 搜够没」这些连续量决定，
 *   如果每档都拆成一个 StateTree 状态，需要写 4 条转换 + 3 个状态，
 *   而且记忆数据要在状态间传递。放进一个 Task 里用枚举推进更紧凑。
 *
 *   真正需要状态机介入的只有「放弃」——那时返回 Failed，
 *   由 StateTree 的 On State Failed 转换决定去哪（回漫游 / 去调查）。
 */
UENUM()
enum class EPursuePhase : uint8
{
	/** 看得见，追预判点 */
	Pursuing,
	/** 看不见但有记忆，赶去最后已知位置 */
	Chasing,
	/** 到了记忆点，原地环视 */
	Scanning,
	/** 血量偏低：边后撤边开火（保持压力，但不硬顶） */
	Retreating,
	/** 血量危险：转身逃跑并提速 */
	Fleeing,
	/** 已到掩体，静止回血（消耗道具） */
	Healing,
	/** 血量危险但没道具了：拼死冲锋，不再后退 */
	Desperate,
};

/**
 * Task 的实例数据（在 StateTree 编辑器的 Task 面板里能看到的参数）。
 *
 * 注意：这里同时承担「参数」和「运行时状态」两个角色。
 *   - Category="Parameter" / "Context" 的字段给设计师调
 *   - Category="Runtime" 的字段是每个敌人各自的进度，不要在编辑器里改
 *
 * 为什么运行时状态放 InstanceData 而不是放组件：
 *   组件是共享的算法库，不该为每个敌人存一份进度；
 *   InstanceData 由 StateTree 按 (树实例, Task) 分配，天然是每敌一份。
 */
USTRUCT()
struct FStateTreePursueTaskInstanceData
{
	GENERATED_BODY()

	// ============================================================
	// 参数（编辑器可调）
	// ============================================================

	/** 要追的目标。在 StateTree 里绑定到感知到的玩家。不绑则自动找 0 号玩家。 */
	UPROPERTY(EditAnywhere, Category = "Context")
	TObjectPtr<AActor> Target = nullptr;

	/** 进入这个距离就停下（cm）。0 = 一直追到贴脸。 */
	UPROPERTY(EditAnywhere, Category = "Parameter", meta = (ClampMin = "0.0", ClampMax = "3000.0"))
	float StopDistance = 250.0f;

	/** 到达半径（cm）。 */
	UPROPERTY(EditAnywhere, Category = "Parameter", meta = (ClampMin = "10.0", ClampMax = "500.0"))
	float AcceptanceRadius = 100.0f;

	/** 是否叠加群体分离（多敌不挤成一坨）。 */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bUseSeparation = true;

	/** 找不到目标时是否立刻结束这个状态（让状态机转到搜索/漫游）。 */
	UPROPERTY(EditAnywhere, Category = "Behavior")
	bool bFailWhenNoTarget = false;

	/**
	 * 是否在屏幕打印本 Task 的调试文字。
	 *
	 * ★ 默认**开启**：行为分档（PURSUING / RETREAT / FLEE）是这块的核心逻辑，
	 *   看不到档位就等于没法验证。屏幕行数受 ShooterAI.ScreenDebug 总开关控制，
	 *   所以不会失控（见下）。
	 *
	 * ★ 双重门控说明：
	 *   本开关为真 **且** `ShooterAI.ScreenDebug >= 2` 才真正打印。
	 *   默认级别是 1（只留聚合信息），所以想看到这行需要：
	 *     控制台输入  ShooterAI.ScreenDebug 2
	 *   这是刻意的：15 个敌人各一行会把屏幕挤满，
	 *   排查单个敌人行为时再临时提高级别。
	 */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowDebug = true;

	// 注：曾经有 bUseThreatAwareness / ThreatAvoidRadius / ThreatAvoidThreshold
	// 三个参数用于「用影响图绕开威胁区」，已于 2026-10-21 全部删除。
	// 删除原因见 StateTreePursueTask.cpp 里对应位置的注释（简言之：
	// 威胁源是玩家单点，"绕开威胁"等于"远离玩家"，与追击目标直接矛盾）。

	// ---------- 受伤谨慎 ----------

	/**
	 * 受伤时使用的接触距离（cm）。比正常 StopDistance 大，
	 * 表现为「被打中就往后退一点，不贴脸了」。
	 */
	UPROPERTY(EditAnywhere, Category = "Wounded", meta = (ClampMin = "0.0", ClampMax = "3000.0"))
	float WoundedStopDistance = 800.0f;

	/** 是否启用受伤谨慎。 */
	UPROPERTY(EditAnywhere, Category = "Wounded")
	bool bUseWoundedStopDistance = true;

	// ---------- 血-量-档-位 行-为 ----------

	/**
	 * 边退边打时想保持的距离（cm）。
	 * 血量低于 RetreatHealthRatio 但还没到 FleeHealthRatio 时使用。
	 */
	UPROPERTY(EditAnywhere, Category = "Health", meta = (ClampMin = "200.0", ClampMax = "5000.0"))
	float RetreatDistance = 1200.0f;

	/** 逃跑时想拉开的距离（cm）。血量低于 FleeHealthRatio 时使用。 */
	UPROPERTY(EditAnywhere, Category = "Health", meta = (ClampMin = "500.0", ClampMax = "8000.0"))
	float FleeDistance = 3000.0f;

	/**
	 * 后撤/逃跑的最远距离（cm）。
	 * 超过这个距离就停下不再退，避免跑出地图或跑出战斗。
	 *
	 * ★ FLEE 时会再乘一个系数（FleeMaxRangeMultiplier），
	 *   因为真逃跑应该比"边退边打"跑得更远。
	 */
	UPROPERTY(EditAnywhere, Category = "Health", meta = (ClampMin = "1000.0", ClampMax = "20000.0"))
	float RetreatMaxRange = 4000.0f;

	/**
	 * FLEE 档位下，最远距离的放大系数。
	 * 例：RetreatMaxRange=4000、系数=1.5 ⇒ FLEE 最远可到 6000cm。
	 * 真逃跑应该跑得比边退边打更远，否则观感上两档没差别。
	 */
	UPROPERTY(EditAnywhere, Category = "Health", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float FleeMaxRangeMultiplier = 1.5f;

	/**
	 * 边退边打（RETREAT）时是否面向玩家。
	 *
	 * ★ 只影响 RETREAT，不影响 FLEE。
	 *   FLEE 一律背对玩家跑（朝向交给移动方向），
	 *   因为"面朝你倒退"在观感上根本不像逃命。
	 */
	UPROPERTY(EditAnywhere, Category = "Health")
	bool bFaceTargetWhileRetreating = true;

	// ---------- 掩体与回血 ----------

	/** 找掩体时的搜索半径（cm）。 */
	UPROPERTY(EditAnywhere, Category = "Health|Cover", meta = (ClampMin = "300.0", ClampMax = "5000.0"))
	float CoverSearchRadius = 1500.0f;

	/** 到达掩体点的判定距离（cm）。 */
	UPROPERTY(EditAnywhere, Category = "Health|Cover", meta = (ClampMin = "30.0", ClampMax = "500.0"))
	float CoverArriveRadius = 150.0f;

	/**
	 * 回血期间如果玩家能看到我，是否打断回血。
	 *
	 * ★ 默认开启。理由：玩家看着敌人在眼前回血却打不断，观感很假。
	 *   打断后它会重新找掩体 —— 这才是有来有回的战斗。
	 */
	UPROPERTY(EditAnywhere, Category = "Health|Cover")
	bool bHealInterruptedBySight = true;

	/**
	 * 奔向掩体的超时时间（秒）。超时后放弃掩体，改为直线远离。
	 *
	 * ★ 为什么必须有这个熔断
	 *   掩体点是"采样 + 射线"选出来的，几何上成立不代表**可达**：
	 *   它可能落在隔壁房间、台阶边缘、或被墙彻底隔开。
	 *   这种情况下 MoveToLocation 会走到"最接近的可行点"然后停住，
	 *   而距离永远大于 CoverArriveRadius —— 每帧重发请求、每帧原地不动。
	 *   观感就是"血量一过 25% 就卡死"。
	 *   与其追求"总能找到完美掩体"，不如保证"任何情况都不会站着不动"。
	 */
	UPROPERTY(EditAnywhere, Category = "Health|Cover", meta = (ClampMin = "0.5", ClampMax = "15.0"))
	float CoverTravelTimeout = 4.0f;

	/**
	 * 连续几次"到了指定点却仍然暴露"之后，本次逃跑彻底放弃掩体。
	 *
	 * ★ 防的是另一种死循环
	 *   掩体点走过去了，但射线判定还是被看见（柱子太窄、投影落点偏移），
	 *   于是换点 → 又失败 → 再换点。次数不设上限的话，
	 *   敌人会在几个点之间来回跑，永远进不了回血。
	 */
	UPROPERTY(EditAnywhere, Category = "Health|Cover", meta = (ClampMin = "1", ClampMax = "10"))
	int32 MaxCoverAttempts = 3;

	/**
	 * 「回血无望 → 转拼死」的超时（秒）。0 = 关闭该兜底。
	 *
	 * ★ 为什么需要这个兜底
	 *   掩体是几何条件，不是必然存在的。在完全开阔的地图上，
	 *   敌人可能永远找不到"玩家看不到我"的位置 —— 于是它带着道具
	 *   永远逃跑、永不回血、永不转拼死，血量卡在 25% 以下不变。
	 *   玩家看到的是"它一直在跑圈，既不打我也不死"。
	 *
	 * ★ 语义（符合直觉，不是补丁）
	 *   "带着急救包但找不到地方用" → 最后只能拼命。
	 *   这正是困兽逻辑的完整版：有后手就撤，后手用不上就拼。
	 *
	 * ★ 只统计"还没开始回血"的时间
	 *   一旦扣过道具（bHealItemConsumed），说明已经找到过掩体，
	 *   走的是另一条路（回血中被打断），不再套用这个超时。
	 *   否则会把"正在正常回血"误判成无望。
	 */
	UPROPERTY(EditAnywhere, Category = "Health|Cover", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float HealHopelessTimeout = 12.0f;

	/**
	 * 后撤时重新下发移动请求的阈值（cm）。
	 *
	 * ★ 为什么需要这个阈值 —— 它治的是"抽搐"
	 *   后撤目标点是每帧按【玩家实时位置】重算的：
	 *     目标 = 玩家位置 + 远离方向 × 距离
	 *   玩家一动，目标就动几厘米。若每帧都 MoveToLocation，
	 *   寻路会被反复重置（每次调用都会清掉已有路径重新规划），
	 *   角色就在"起步—刹车—再起步"之间抖动 —— 看上去像抽搐。
	 *   加阈值后：目标没显著变化就沿用旧路径；只有玩家真的移动了，
	 *   或者角色卡住了（停着却没到位），才重新下发。
	 */
	UPROPERTY(EditAnywhere, Category = "Health", meta = (ClampMin = "30.0", ClampMax = "800.0"))
	float RetreatRepathThreshold = 150.0f;

	/**
	 * 无道具时"拼死一搏"的提速倍率。
	 * 快死了又没退路，冲得比平时更猛，符合"困兽"直觉。
	 */
	UPROPERTY(EditAnywhere, Category = "Health|Cover", meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float DesperateSpeedMultiplier = 1.3f;

	/**
	 * 拼死冲锋时"冲到多近就停下开火"（cm）。
	 *
	 * ★ 这个参数治的是「拼死阶段不开火」
	 *
	 *   DESPERATE 分支原先只有一句 MoveToLocation(玩家预判点)，没有停火点，
	 *   于是它会一路贴到玩家脸上。而蓝图侧的射击函数带有距离判定，
	 *   贴得太近反而不开火 —— 表现就是"举着枪撞人，一枪不放"。
	 *
	 *   拼死一搏的正确表现不是"撞死你"，而是"不再躲、正面死磕"：
	 *   冲到能开枪的位置就站定开火，你跑它再追。
	 *
	 *   设成 0 表示沿用 StopDistance（默认 250）。
	 *   默认给 600 而不是 250，是为了留出安全余量 ——
	 *   各武器的有效射程不同，卡在 250 这种极限距离容易因
	 *   双方微小的位置变化在"能打/不能打"之间抖动。
	 */
	UPROPERTY(EditAnywhere, Category = "Health|Cover", meta = (ClampMin = "0.0", ClampMax = "3000.0"))
	float DesperateEngageDistance = 600.0f;

	// ---------- 影响图：后撤时选安全点 ----------

	/**
	 * 是否用影响图为后撤目标点做"安全化修正"。
	 *
	 * ★ 为什么这里用影响图是对的，而追击时用是错的
	 *   影响图的威胁源是玩家位置（一个点），
	 *   "绕开威胁"在数学上等价于"远离玩家"：
	 *     · 追击时用 → 与"接近玩家"目标矛盾 → 敌人卡住（上一轮踩过）
	 *     · 后撤时用 → 与"远离玩家"目标同向 → 语义一致，正解
	 *   所以开关放在撤退分支，而不是追击分支。
	 */
	UPROPERTY(EditAnywhere, Category = "Health")
	bool bUseInfluenceForRetreat = true;

	/** 在后撤目标点周围搜索安全点的半径（cm）。 */
	UPROPERTY(EditAnywhere, Category = "Health", meta = (ClampMin = "100.0", ClampMax = "3000.0"))
	float InfluenceSearchRadius = 600.0f;

	/**
	 * 只有安全度提升超过这个阈值才替换目标点。
	 * 太小会被噪声带着抖，太大则永远不生效。默认 0.05（5%）。
	 */
	UPROPERTY(EditAnywhere, Category = "Health", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float InfluenceImproveThreshold = 0.05f;

	// ---------- 记忆与搜索（本项目的第二处设计点） ----------

	/** 丢失目标后是否进入搜索阶段。关掉 = 一丢就走（模板原始行为）。 */
	UPROPERTY(EditAnywhere, Category = "Memory")
	bool bUseMemory = true;

	/** 记忆保留时长（秒）：超过这个时间没再看见，就彻底放弃。 */
	UPROPERTY(EditAnywhere, Category = "Memory", meta = (ClampMin = "0.5", ClampMax = "60.0"))
	float MemoryDuration = 10.0f;

	/** 到达最后已知位置后，原地环视多久（秒）再放弃。 */
	UPROPERTY(EditAnywhere, Category = "Memory", meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float ScanDuration = 4.0f;

	/** 判定「已到达最后已知位置」的距离（cm）。 */
	UPROPERTY(EditAnywhere, Category = "Memory", meta = (ClampMin = "20.0", ClampMax = "500.0"))
	float SearchAcceptanceRadius = 150.0f;

	/**
	 * 追赶超时（秒）：超过这个时间还没走到记忆点，就地在当前位置开始搜索。
	 *
	 * 为什么需要它：
	 *   记忆点可能不可达（玩家贴着柱子消失，记忆点落在柱子另一侧）。
	 *   此时 MoveToLocation 每帧失败、敌人原地不动，但 2D 距离仍大于
	 *   SearchAcceptanceRadius，于是永远进不了 SCANNING，一直卡到
	 *   MemoryDuration 到期被 GIVE-UP。
	 *   加了超时后行为变成「追不到就就地搜一轮再放弃」，符合真实直觉。
	 */
	UPROPERTY(EditAnywhere, Category = "Memory", meta = (ClampMin = "0.5", ClampMax = "30.0"))
	float ChaseTimeout = 4.0f;

	/** 环视时的转速（度/秒）。 */
	UPROPERTY(EditAnywhere, Category = "Memory", meta = (ClampMin = "0.0", ClampMax = "720.0"))
	float ScanYawSpeed = 120.0f;

	// ============================================================
	// 运行时状态（只读，每个敌人一份）
	// ============================================================

	/** 最后一次看见目标的位置。只在「看得见」时刷新。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	FVector LastKnownLocation = FVector::ZeroVector;

	/** 是否已经有过记忆（第一次看见目标之前为 false）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	bool bHasMemory = false;

	/** 当前阶段。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	EPursuePhase Phase = EPursuePhase::Pursuing;

	/** 距离最后一次看见目标，过了多少秒。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	float TimeSinceLastSeen = 0.0f;

	/** 已经在 CHASING 阶段花了多少秒（用于 ChaseTimeout 判定）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	float ChaseElapsed = 0.0f;

	/** 已原地环视多少秒。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	float ScanElapsed = 0.0f;

	/**
	 * 本实例被 Tick 过多少次（调试用）。
	 *
	 * 为什么放这里而不是用 static 变量：
	 *   static 在所有敌人之间共享，第一个敌人打过日志后其他敌人就不打了，
	 *   排查时会误以为"只有一个敌人在跑"。
	 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	int32 TickCount = 0;

	// ---------- 掩体与回血的运行时状态 ----------

	/** 已选定的掩体点（Healing 阶段站在这里）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	FVector CoverLocation = FVector::ZeroVector;

	/** 是否已找到可用掩体（避免每帧重复搜索）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	bool bHasCoverTarget = false;

	/** 回血计时器（每 HealInterval 触发一次）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	float HealTimer = 0.0f;

	/** 本次回血已恢复多少点（调试用）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	float HealedAmount = 0.0f;

	/** 本次奔掩体已花的时间（秒）。到 CoverTravelTimeout 就放弃掩体。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	float CoverElapsed = 0.0f;

	/** 本次逃跑中，掩体连续失败（到了却仍暴露）的次数。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	int32 CoverFailCount = 0;

	/**
	 * 本次逃跑中已判定无效的掩体点。
	 *
	 * ★ 为什么需要黑名单
	 *   FindCoverPoint 是确定性的：同样的起点 + 同样的玩家位置，
	 *   每帧都会返回同一个"最近的可用点"。
	 *   一旦这个点实际站不住（被看见），不拉黑就等于每帧都重新选中它，
	 *   形成"选中 → 到达 → 暴露 → 清空 → 再选中"的死循环。
	 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	TArray<FVector> RejectedCovers;

	/** 本次逃跑是否已彻底放弃掩体（超时 / 反复失败 / 一个都找不到）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	bool bGiveUpCover = false;

	/**
	 * 本次"想回血但还没成功"已持续多少秒。
	 *
	 * 用于 HealHopelessTimeout 兜底：超过阈值就判定回血无望、转拼死。
	 * 成功进入回血时清零。
	 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	float FleeElapsed = 0.0f;

	/**
	 * 本轮回血是否已经扣过一个道具。
	 *
	 * ★ 为什么必须用独立标记，而不是靠 "HealedAmount == 0" 判断
	 *   因为 HealedAmount 只有在 ApplyHeal 成功时才会累加。
	 *   一旦反射写入失败（属性类型不匹配、或蓝图那边把自己的血量写回去），
	 *   HealedAmount 就恒为 0，于是每帧都判定"这是第一次回血"→ 每帧扣一个道具
	 *   → 5 帧内道具清零 → 敌人从"想回血"直接跳到"拼死冲锋"。
	 *   用独立布尔量后，无论回血是否生效，一轮只扣一次。
	 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	bool bHealItemConsumed = false;

	/** 上一次真正发给 AIController 的移动目标（用于节流，防抽搐）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	FVector LastMoveDest = FVector::ZeroVector;

	/** 是否已经记过移动目标。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	bool bHasLastMoveDest = false;

	/**
	 * 拼死阶段当前是否处于「站定开火」状态。
	 *
	 * ★ 用途是滞回（hysteresis）
	 *   只用单阈值判断会在边界抖动：玩家在 EngageDistance 附近来回走，
	 *   敌人就 停—冲—停—冲 地抽搐。
	 *   有了这个标记，进入用 EngageDistance、退出用 EngageDistance×1.3，
	 *   中间留出一段缓冲带。
	 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	bool bDesperateHolding = false;
};

/**
 * Chase 状态用：追击 + 记忆 + 搜索（三档一体）。
 *
 * 用法（StateTree 编辑器）：
 *   1. ST_Shooter 里新建 State，命名 Chase
 *   2. 加 Task → 搜 "Pursue Steering"
 *   3. Target 绑定 Sense Enemies 的 Target Enemy（或留空自动找玩家）
 *   4. 入口：Attack Enemy 上加 On Tick → Goto State Chase，条件 Target Is Valid
 *   5. 出口：Chase 上加 On Tick → Goto State Root，条件 Target Is Not Valid
 *
 * 阶段推进（内部自动，不需要在状态机上接线）：
 *   看得见 ──→ Pursuing（追预判点，刷新记忆）
 *      ↓ 看不见
 *   有记忆 ──→ Chasing（去最后已知位置）
 *      ↓ 到达
 *             Scanning（原地环视 ScanDuration 秒）
 *      ↓ 超时 / 记忆过期
 *             return Failed → 由状态机决定去哪
 */
USTRUCT(meta = (DisplayName = "Pursue Steering", Category = "GameAI"))
struct SHOOTERAI_API FStateTreePursueTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FStateTreePursueTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;

	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context,
		const float DeltaTime) const override;

	virtual void ExitState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;

#if WITH_GAMEPLAY_DEBUGGER
	virtual FString GetDebugInfo(const FStateTreeReadOnlyExecutionContext& Context) const override;
#endif

private:
	/** 从 Context 找到本 Task 操作的组件（AIController → Pawn → PursueSteeringComponent）。 */
	static UPursueSteeringComponent* ResolveComponent(const FStateTreeExecutionContext& Context);
};
