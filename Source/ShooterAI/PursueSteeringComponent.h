// ============================================================
// PursueSteeringComponent.h
// ============================================================
//
// 【这个组件是什么】
//   敌人 AI 的"移动 + 感知 + 状态"组件。挂在敌人 Pawn 上（蓝图里叫 Pursue Steering）。
//   它既提供纯算法（算追击点、算掩体），也存运行状态（血量档位、剩余道具、
//   停火信号）。是**行为层（StateTree Task）和引擎之间的中间层**。
//
// 【它是怎么被使用的】
//   行为层 StateTreePursueTask 每帧调用它的接口，拿到"该往哪走 / 该不该打 /
//   还有没有道具"，然后自己下达 MoveTo 指令。
//   组件本身**不做决策** —— 决策全在 Task 里。这个边界很重要：
//   组件改了不会影响行为逻辑，行为逻辑改了不用动组件。
//
// 【注意：旧的 AutoPilot 模式已停用】
//   组件曾经能自己接管移动（bAutoPilot=true），那是 StateTree Task 出现前的架构。
//   现在 Task 的 EnterState 里会强制 `bAutoPilot = false`，所以走的是 Task 路径。
//   旧旁路代码保留在 _Legacy.cpp 里作为隔离调试手段，但不参与执行。
//
// ============================================================
// 【本文件章节地图】按职责分 9 节，实现在 4 个 .cpp 里
// ============================================================
//
//  节  职责            典型成员                          实现在哪
//  ────────────────────────────────────────────────────────────────────
//  ①  算法核心           GetPursueTargetLocation / UpdatePursue   主 .cpp
//      └ 纯数学，可单独验证；追"你将要在的位置"而非当前位置
//
//  ②  群体分离           ComputeSeparationOffset                   主 .cpp
//      └ 纯数学；多个敌人不要叠在一起
//
//  ③  导航与引擎桥接      ProjectToNavMesh / ResolveAIController    主 .cpp
//                        CanSeeTarget / MoveTo / FaceLocation
//      └ 与引擎握手；目标点必须先投影到可行走面，否则 MoveTo 静默失败
//
//  ④  掩体探测           IsInCover / FindCoverPoint / ...Ex        _Cover.cpp
//      └ 几何查询；"安全 = 看不到我"，不是"距离远"
//
//  ⑤  血量与回血         GetHealthPercent / IsLowHealth            _Health.cpp
//                        ApplyHeal / ConsumeHealItem
//      └ 反射读写；失败是静默的，必须靠 GetHealthProbeLog 探针
//
//  ⑥  受伤谨慎           IsWounded / WoundedCautionDuration       _Health.cpp
//      └ 挨打后 5 秒内把开火距离拉大（与血量档位是两套独立触发）
//
//  ⑦  跨 Task 信号       bSuppressFire / SetFireSuppressed         头文件内联
//      └ 行为层告诉射击层"本帧别开火"；两个 Task 唯一的沟通通道
//
//  ⑧  旧旁路（停用）      AutoPilotTick                            _Legacy.cpp
//      └ Task 模式下强制关闭；保留作为"算法能否单独跑通"的隔离环境
//
//  ⑨  算法储备（未接线）   ComputeArrive / CollisionAvoidance        _Legacy.cpp
//                        / Wander
//      └ 零调用；放弃理由见 _Legacy.cpp 文件头
//
//  另：记忆与搜索的状态字段（LastKnownLocation / bHasMemory / ScanElapsed）
//      定义在中段，实际使用者是旧旁路 AutoPilotTick。
//
// ============================================================
// 【源流】
//   Pursue 算法移植自《AI for Games》(第3版) 3.3.8
//   源库: core/ch03_movement/Pursue.h
//   记忆与搜索（第三档）为本项目原创 —— 源库只有"追/不追"两个极端，
//   中间缺的"去最后看见你的地方找一圈"是补上去的。
// ============================================================

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PursueSteeringComponent.generated.h"

/** 组件当前处于哪一档行为（调试可视化用） */
UENUM(BlueprintType)
enum class EPursueMode : uint8
{
	Idle       UMETA(DisplayName="放手给状态树"),
	Pursuing   UMETA(DisplayName="追击中"),
	Chasing    UMETA(DisplayName="前往最后已知位置"),
	Scanning   UMETA(DisplayName="原地搜索")
};

/**
 * 预判追击：敌人朝玩家「预测的未来位置」拦截，而不是朝「当前位置」追尾。
 *
 * 三档行为（这是差异化的核心）：
 *   ① 看得见     → 追预判点（Pursue 算法）
 *   ② 看不见但在记忆窗口内 → 去「最后看见你的位置」搜
 *   ③ 搜过一圈还没找到    → 放手，交给 StateTree 回去巡逻
 *
 * 模板的敌人只有「看得见就追 / 看不见就走」两个极端，中间缺的正是 ②。
 */
UCLASS(ClassGroup=(GameAI), meta=(BlueprintSpawnableComponent, DisplayName="Pursue Steering"))
class SHOOTERAI_API UPursueSteeringComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPursueSteeringComponent();

	// ============================================================
	// ① 算法核心（移植自源库 Pursue.h，纯数学，未改动）  → 主 .cpp
	// ============================================================

	/** 最大预测时间（秒），对应源库 Pursue::maxPrediction。给预判提前量设上限。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue", meta=(ClampMin="0.05", ClampMax="5.0"))
	float MaxPrediction = 1.0f;

	/** 最近一次算出的预测拦截点（供蓝图直接读取）。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Pursue")
	FVector CurrentPredictedLocation = FVector::ZeroVector;

	/** 预测点落在墙里/地图外时的可接受投影距离（cm），超过就直接锁人物。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue", meta=(ClampMin="10.0", ClampMax="1000.0"))
	float MaxNavProjectionDistance = 150.0f;

	/** 上一次是否采用了预判（false=预测点不可达，已锁人物当前位置）。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Pursue")
	bool bLastPredictionValid = false;

	/**
	 * 预判点与 AI 自身的最小间距（cm）—— 防「倒退追人」。
	 *
	 * ★ 为什么需要这个（两个蠢点的根因）：
	 *   玩家朝 AI 冲过来时，预判点会算到 AI 身后。
	 *   例：AI(0,0)、玩家(300,0) 速度 (-600,0)、AI 速度 400
	 *       Prediction = 300/400 = 0.75s
	 *       Predicted = (300,0) + (-600,0)*0.75 = (-150, 0)  ← AI 背后！
	 *   AI 为了走到这个点必须往后退，于是出现
	 *   「面朝玩家、身体倒着走」的荒谬表现。
	 *
	 * ★ 修法：预判点沿「玩家→AI」方向最多推进到距 AI 还剩 SafetyMargin 处，
	 *   不许越过 AI 自身。横向跑 / 玩家远离时完全不受影响，
	 *   只有「玩家正朝 AI 冲」这一种情况会被截断。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue", meta=(ClampMin="0.0", ClampMax="500.0"))
	float PredictedSafetyMargin = 100.0f;

	/**
	 * 预判偏移量相对距离的上限比例。
	 *
	 * ★ 为什么需要这个（横向移动时"不像朝玩家跑"的根因）：
	 *   MaxPrediction = 1.0 秒，玩家横移 600cm/s 时预判偏移最大可达 600cm，
	 *   而这是【绝对值】——不随距离缩放。
	 *     距离 1500cm → 偏角 ≈ 22°
	 *     距离  600cm → 偏角 ≈ 45°   ← 看起来像在往侧面跑
	 *
	 *   改为「偏移上限 = 当前距离 × 本比例」后：
	 *     0.2 → 偏角上限 = atan(0.2) ≈ 11°
	 *   近身精准、远处允许较大偏移，偏角始终可控。
	 *
	 * ★ 附带保证（这也是敢在追击时重新 SetFocalPoint 的前提）：
	 *   偏移 ≤ Ratio × 距离 ⇒ 移动方向与「指向玩家」方向的夹角 ≤ atan(Ratio)。
	 *   所以「面朝玩家 + 侧移」永远是自然的战斗侧步，
	 *   数学上不可能出现倒着走。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue", meta=(ClampMin="0.0", ClampMax="1.0"))
	float PredictedOffsetRatio = 0.2f;

	// ============================================================
	// ⑥ 受伤谨慎：被打中之后进入「谨慎期」              → _Health.cpp
	//    （注意：与"血量档位"是两套独立触发 —— 这个看"是否被打中"，
	//      血量档位看"血量比例"。两者可以同时生效。）
	//
	// 为什么需要：
	//   敌人现在是「无脑贴脸」——不管自己会不会被打死都往前冲。
	//   受伤后应该拉开距离、重新评估，而不是继续送。
	//
	// 实现方式（不需要血量系统）：
	//   订阅 AActor 自带的 OnTakeAnyDamage 委托，收到就记一个时间戳。
	//   这不需要 Health 变量、不需要改蓝图，
	//   只要 NPC 会被 Damage 命中就会触发。
	// ============================================================

	/** 受伤后的「谨慎期」时长（秒）。0 = 关闭该功能。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Wounded", meta=(ClampMin="0.0", ClampMax="30.0"))
	float WoundedCautionDuration = 5.0f;

	/** 谨慎期内是否真的生效（调试开关）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Wounded")
	bool bUseWoundedCaution = true;

	/** 当前是否处于谨慎期（只读，供 Task / 蓝图查询）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Wounded")
	bool IsWounded() const;

	/** 距离上次受伤过了多少秒（没受伤过返回一个很大的值）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Wounded")
	float GetTimeSinceDamaged() const;

	// ============================================================
	// ⑤·a 血量档位：受伤后的行为分级                       → _Health.cpp
	//    门槛：≤60% 边退边打 / ≤25% 找掩体或拼死
	// ============================================================
	//
	// 为什么要按血量分档，而不是统一「受伤就退」：
	//   挨一枪就转身跑，观感像"游戏坏了"；
	//   挨一枪完全不理，观感像"AI 不会躲"。
	//   真实的战斗反应是渐进的：
	//     掉了一点血 → 边打边拉开距离（保持压力）
	//     快死了     → 转身就跑，还要跑得更快
	//
	// 数据来源：`BP_ShooterNPC` 里已有 `Current HP` 变量（反射读取）。
	// 不需要我们新建血量系统，也不改蓝图。

	/** 血量上限，用于算百分比。必须和蓝图里 `Current HP` 的初始值一致。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Health", meta=(ClampMin="1.0", ClampMax="10000.0"))
	float MaxHealth = 100.0f;

	/** 血量百分比低于这个值 → 进入「边退边打」。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Health", meta=(ClampMin="0.0", ClampMax="1.0"))
	float RetreatHealthRatio = 0.6f;

	/** 血量百分比低于这个值 → 进入「逃跑」。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Health", meta=(ClampMin="0.0", ClampMax="1.0"))
	float FleeHealthRatio = 0.25f;

	/** 逃跑时的移速倍率（相对原始 MaxWalkSpeed）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Health", meta=(ClampMin="1.0", ClampMax="3.0"))
	float FleeSpeedMultiplier = 1.6f;

	/** 是否启用量血行为（调试开关）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Health")
	bool bUseHealthBehavior = true;

	/** 当前血量百分比（0~1）。读不到时为 1.0（当作满血，不影响正常追击）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Health")
	float GetHealthPercent() const;

	/** 是否处于「边退边打」档位。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Health")
	bool IsLowHealth() const;

	/** 是否处于「逃跑」档位。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Health")
	bool IsCriticalHealth() const;

	/** 原始移速（第一次读到 MaxWalkSpeed 时缓存）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Health")
	float GetOriginalWalkSpeed() const { return CachedOriginalWalkSpeed; }

	/**
	 * 血量探测日志（诊断用）。
	 *
	 * 返回"找到哪些像血量的属性、类型是什么、当前值多少"。
	 * 读不到血量时把它打到屏幕上，一眼就能看出是名字不对还是类型不对。
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Health")
	FString GetHealthProbeLog() const { return HealthProbeLog; }

	/**
	 * 应用/还原逃跑提速。
	 *
	 * @param bFlee true = 提速到 Original × FleeSpeedMultiplier；false = 还原原始值
	 *
	 * ★ 为什么必须能还原，而且要考虑"重复调用"：
	 *   如果每次调用都 `Current = Current × 倍率`，会一层层叠上去越跑越快。
	 *   所以第一次调用时先把原始值缓存下来，之后都以它为基准。
	 *   这是"幂等"的写法：连续调 bFlee=true 多次，速度不会叠加。
	 */
	UFUNCTION(BlueprintCallable, Category="GameAI|Health")
	void ApplyFleeSpeed(bool bFlee);

	// ============================================================
	// ⑤·b 回血道具（消耗品）                              → _Health.cpp
	// ============================================================
	//
	// ★ 为什么存在组件里而不是 StateTree 的 InstanceData
	//   InstanceData 在状态重入时会被重置（EnterState 里我们有清理代码），
	//   而道具数量必须贯穿整局 —— 不能因为进出 Chase 就补满。
	//   组件是 Actor 级的，生命周期跟角色走，才是正确的存放位置。

	/** 开局携带的回血道具个数。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Heal", meta=(ClampMin="0", ClampMax="10"))
	int32 HealItemCount = 2;

	/** 剩余道具个数（运行时）。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Heal")
	int32 RemainingHealItems = 0;

	/** 每次回血恢复的血量点数。中速：0.5s 时 +3 点 ⇒ 25%→60% 约 6 秒。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Heal", meta=(ClampMin="0.5", ClampMax="50.0"))
	float HealPerTick = 3.0f;

	/** 回血间隔（秒）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Heal", meta=(ClampMin="0.1", ClampMax="3.0"))
	float HealInterval = 0.5f;

	/** 回血目标百分比：到这个血量就停（通常等于 RetreatHealthRatio）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Heal", meta=(ClampMin="0.1", ClampMax="1.0"))
	float HealUntilPercent = 0.6f;

	/** 是否还有可用道具。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Heal")
	bool HasHealItem() const { return RemainingHealItems > 0; }

	// ============================================================
	// ⑦ 停火开关（跨 Task 通信）                          → 头文件内联
	// ============================================================
	//
	// ★ 为什么需要这个字段：两个 Task 互相看不见对方
	//
	//   StateTree 里「Pursue Steering」和「Shoot Control」是并列的两个 Task，
	//   各自持有独立的 InstanceData —— PursueTask 知道自己在逃，
	//   ShootControl 只知道"有视线就打"。于是出现「背对你逃跑却还在命中」。
	//
	//   ★ 为什么不能靠"看不见就不打"来解决
	//     逃跑的起因恰恰是【被看见了】。视线门控在这里永远是放行的，
	//     它管的是"墙挡不挡"，管不了"该不该打"。
	//
	//   ★ 为什么用组件做通道
	//     两个 Task 共享同一个 Pawn，组件是它们唯一天然共有的、生命周期正确的
	//     存储点。用全局变量会让多敌人互相干扰，用 InstanceData 又传不过去。
	//
	//   语义：true = 本帧不允许开火（由 PursueTask 每帧写入，ShootControl 读取）。
	UPROPERTY(BlueprintReadWrite, Category="GameAI|Fire")
	bool bSuppressFire = false;

	/** 停火原因（调试用，显示在屏幕上）。 */
	UPROPERTY(BlueprintReadWrite, Category="GameAI|Fire")
	FString FireSuppressedReason;

	/**
	 * 设置停火状态（PursueTask 每帧调用；Reason 为空表示解除）。
	 *
	 * 不加 UFUNCTION：只有 C++ 侧调用，避免 UHT 对默认参数的额外限制。
	 */
	void SetFireSuppressed(bool bSuppress, const FString& Reason)
	{
		bSuppressFire = bSuppress;
		FireSuppressedReason = bSuppress ? Reason : FString();
	}

	/**
	 * 消耗一个道具。
	 * @return 成功消耗返回 true；没道具返回 false
	 */
	UFUNCTION(BlueprintCallable, Category="GameAI|Heal")
	bool ConsumeHealItem();

	/**
	 * 恢复血量（反射写入 Current HP）。
	 *
	 * ★ 已知限制：反射写不会触发蓝图的 UI 刷新事件。
	 *   但敌人血量玩家看不到，游戏逻辑读的是实际值 ⇒ 不影响行为。
	 *
	 * @param Amount 恢复点数
	 * @return 实际写入成功
	 */
	UFUNCTION(BlueprintCallable, Category="GameAI|Heal")
	bool ApplyHeal(float Amount);

	// ============================================================
	// ④ 掩体探测（几何射线，不依赖关卡标记）             → _Cover.cpp
	//    ★ 核心观念："安全"不等于"离玩家远" —— 开阔地退 3000cm 照样挨打。
	//      真正的安全是"你看不到我"，所以用射线遮挡判定。
	// ============================================================
	//
	// ★ 为什么判据是「射线」而不是「距离」
	//   "离玩家远"不等于"安全"：开阔地退到 3000cm 还是会被打。
	//   真正的掩体是「玩家从这里看不到我」—— 只有射线能判断。
	//   ⇒ IsInCover 从候选点向威胁源发射线，被挡住才算掩体。
	//
	// ★ 为什么不用关卡标记（BP_Cover）
	//   要在灰盒关卡里手动标几十个点，成本高且改关卡就失效。
	//   射线方案零配置、自动适配任何几何。

	/** 掩体探测的视线通道。默认 Visibility（灰盒墙体都响应它）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Cover")
	TEnumAsByte<ECollisionChannel> CoverTraceChannel = ECC_Visibility;

	/** 判断"有掩护"时的眼睛高度（cm）。用角色眼高比脚底更准。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Cover", meta=(ClampMin="0.0", ClampMax="300.0"))
	float CoverEyeHeight = 100.0f;

	/**
	 * 某个点是否处于掩体后（威胁源看不到该点）。
	 *
	 * @param TestPoint 候选位置
	 * @param Threat    威胁源（通常是玩家）
	 */
	UFUNCTION(BlueprintCallable, Category="GameAI|Cover")
	bool IsInCover(const FVector& TestPoint, AActor* Threat) const;

	/**
	 * 在 Origin 周围找一个"能挡住 Threat 视线"的位置。
	 *
	 * 采样策略：以 Origin 为中心、半径 SearchRadius 的圆周上取若干方向，
	 * 同时向"远离 Threat"方向偏移，挑出第一个通过 IsInCover 且可达的点。
	 *
	 * @param Origin       搜索中心（通常是自己当前位置）
	 * @param Threat       威胁源
	 * @param SearchRadius 搜索半径（cm）
	 * @param OutPoint     输出掩体点
	 * @return 找到返回 true
	 */
	UFUNCTION(BlueprintCallable, Category="GameAI|Cover")
	bool FindCoverPoint(const FVector& Origin, AActor* Threat, float SearchRadius, FVector& OutPoint) const;

	/**
	 * 带黑名单的版本：跳过 AvoidPoints 附近 AvoidRadius 内的候选点。
	 *
	 * ★ 为什么需要黑名单
	 *   本函数是确定性的 —— 同样的起点、同样的玩家位置，每帧都返回同一个
	 *   "最近的可用点"。一旦那个点实际站不住（柱子太窄、投影偏移），
	 *   不清空记忆就会形成"选中 → 到达 → 暴露 → 再选中"的死循环。
	 *
	 * ★ 为什么不加 UFUNCTION
	 *   UFUNCTION 不支持裸指针参数，蓝图侧继续用上面的 4 参数版本即可。
	 */
	bool FindCoverPointEx(const FVector& Origin, AActor* Threat, float SearchRadius, FVector& OutPoint,
		const TArray<FVector>* AvoidPoints, float AvoidRadius = 250.0f) const;


	// ============================================================
	// ⑧ 旧旁路（已停用）：组件自己接管移动                → _Legacy.cpp
	//    ⚠️ Task 模式的 EnterState 里会强制 bAutoPilot = false，所以默认不跑。
	//       保留它是为了"算法能否单独跑通"的隔离调试环境，不是遗留垃圾。
	// ============================================================

	/** 勾上 = 组件自动接管移动（自己找玩家、自己算落点、自己 MoveTo）。默认开。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|AutoPilot")
	bool bAutoPilot = true;

	/** AutoPilot 检查间隔（秒），降频省开销。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|AutoPilot", meta=(ClampMin="0.0", ClampMax="1.0"))
	float AutoPilotRetryInterval = 0.1f;

	/** 到达半径（cm）：离目标点多近算「到了」。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|AutoPilot", meta=(ClampMin="10.0", ClampMax="500.0"))
	float AutoPilotAcceptanceRadius = 100.0f;

	/** 进入这个距离（cm）就停下开火，不再追击。0 = 永不停止。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|AutoPilot", meta=(ClampMin="0.0", ClampMax="3000.0"))
	float StopDistance = 250.0f;

	/** 是否要求「看得见」才追。默认开——因为看不见时有记忆机制接管，不会一丢就掉线。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|AutoPilot")
	bool bCheckLineOfSight = true;

	/** 出生点不在 NavMesh 上时，自动投影到最近的可走点（只做一次）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|AutoPilot")
	bool bSnapToNavMeshOnStart = true;

	// ============================================================
	// 记忆与搜索的状态字段（本项目原创的第三档，模板没有）
	// 实际使用者是旧旁路 AutoPilotTick（见 _Legacy.cpp）；Task 模式下由 Task 自己管记忆。
	//
	// 模板的两个极端：看得见死追 / 看不见立刻走。
	// 加这一档才有「像一个有记忆的对手」：你躲起来，它会去你消失的地方找。
	// ============================================================

	/** 丢失目标后是否去「最后已知位置」搜索。关掉 = 退回模板行为（一丢就走）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|Memory")
	bool bUseMemory = true;

	/** 记忆保留时长（秒）：超过这个时间没再看见，就彻底放弃。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|Memory", meta=(ClampMin="0.5", ClampMax="60.0"))
	float MemoryDuration = 10.0f;

	/** 到达最后已知位置后，原地环视多久（秒）再放弃。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|Memory", meta=(ClampMin="0.0", ClampMax="30.0"))
	float ScanDuration = 4.0f;

	/** 判定「已到达最后已知位置」的距离（cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|Memory", meta=(ClampMin="20.0", ClampMax="500.0"))
	float SearchAcceptanceRadius = 150.0f;

	/** 最后一次看见目标的位置（供调试/蓝图读取）。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Pursue|Memory")
	FVector LastKnownLocation = FVector::ZeroVector;

	/** 是否持有有效的记忆（false = 还没看见过，或记忆已清空）。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Pursue|Memory")
	bool bHasMemory = false;

	/** 当前处于哪一档行为。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Pursue|Memory")
	EPursueMode CurrentMode = EPursueMode::Idle;

	// ============================================================
	// ② 群体分离（Separation）—— 移植自源库 ch03_movement/Separation.h  → 主 .cpp
	//
	// 问题：多个敌人追同一个人时，会挤成一坨（同一目标点 + 同样路径）。
	// 解法：每个敌人算出「被邻居推开」的合力，叠加到移动目标点上。
	//
	// 与源库的差异（胶水层）：
	//   源库输出「加速度」，再积分成速度；
	//   UE 这边走 NavMesh，不能直接改速度，于是把合力当成「目标点偏移」。
	//   语义从「推着走」变成「绕着追」，但群体分散效果一致。
	// ============================================================

	/** 是否启用群体分离。关掉 = 所有敌人挤向同一个点。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Separation")
	bool bUseSeparation = true;

	/** 分离半径（cm）：进入这个距离的邻居才会推开我。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Separation", meta=(ClampMin="50.0", ClampMax="1000.0"))
	float SeparationRadius = 220.0f;

	/** 分离强度：越大推得越远。源库的 decayCoefficient 同义。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Separation", meta=(ClampMin="0.0", ClampMax="5.0"))
	float SeparationStrength = 1.0f;

	/** 分离偏移的最大长度（cm），防止把目标点推到墙里。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Separation", meta=(ClampMin="0.0", ClampMax="600.0"))
	float MaxSeparationOffset = 250.0f;

	/** 距离最近的同伴有多远（cm），-1 = 附近没有同伴。用于客观验证分离效果。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Separation")
	float NearestAllyDistance = -1.0f;

	/** 计算分离偏移向量（纯函数，不移动）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Separation")
	FVector ComputeSeparationOffset();

	// ============================================================
	// ⑨·a 到达减速（Arrive）—— 移植自源库 ch03_movement/Arrive.h   → _Legacy.cpp
	//    ⚠️ 未接线：UE 的寻路组件已内置接近减速，接了是重复，视觉上看不出差别。
	//
	// 问题：MoveToLocation 到点就硬停，冲刺中的敌人会「急刹」。
	// 解法：进入 slowRadius 后按距离比例降低「期望速度」，越近越慢。
	//
	// 与源库的差异（胶水层）：
	//   源库输出速度向量；UE 走 NavMesh，不能直接给速度，
	//   所以这里输出「按减速比例缩短后的目标点」——
	//   距离越近，目标点往回收得越多，MoveTo 的路径自然变短、
	//   配合 AcceptanceRadius 形成平滑收尾。
	// ============================================================

	/** 是否启用到达减速。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Arrive")
	bool bUseArrive = true;

	/** 减速起始半径（cm）：进入这个距离开始按比例降速。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Arrive", meta=(ClampMin="50.0", ClampMax="1500.0"))
	float ArriveSlowRadius = 400.0f;

	/** 最近一次算出的减速比例（0~1，1=全速）。用于验证是否真的在减速。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Arrive")
	float LastArriveSpeedRatio = 1.0f;

	/**
	 * 到达减速：返回一个「已经按减速比例处理过」的目标点。
	 * 距离 > SlowRadius 时原样返回目标点（全速）；
	 * 距离 <= SlowRadius 时把目标点朝自己拉回一部分，模拟降速。
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Arrive")
	FVector ComputeArrive(const FVector& GoalLocation);

	// ============================================================
	// ⑨·b 碰撞规避（Collision Avoidance）—— 移植自源库 CollisionAvoidance.h → _Legacy.cpp
	//    ⚠️ 未接线：敌人不互相穿过是物理碰撞 + 导航半径在起作用，观众看不出区别。
	//
	// 与 Separation 的区别（这是能讲的设计点）：
	//   Separation：只要靠得近就推，不管对方往哪走 —— 恐慌式躲避，会误判。
	//   CollisionAvoidance：预测未来是否「真的会撞」，只对「最早会撞上」的那个让开。
	//
	// 源库注释里明确记了 C++ 参考实现的一个 bug：
	//   最接近点时间公式 t = -dot(relPos, relVel) / |relVel|² 里那个负号被漏了，
	//   导致「正迎面冲来」被判成「正在远离」而跳过规避。
	//   本实现按正确版本（带负号）编写。
	// ============================================================

	/** 是否启用碰撞规避。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Collision")
	bool bUseCollisionAvoidance = true;

	/** 碰撞半径（cm）：预测最近点距离小于 2×半径 才算会撞。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Collision", meta=(ClampMin="20.0", ClampMax="300.0"))
	float CollisionRadius = 80.0f;

	/**
	 * 检查间隔（秒）。0 = 每帧检查。
	 * 为什么默认降频：15 个敌人 × 每帧遍历全场景 Pawn 是 O(n²)，
	 * 每帧 225 次遍历会明显掉帧。0.1 秒一次在行为上几乎看不出差别。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Collision", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CollisionCheckInterval = 0.1f;

	/** 上一步算出的规避方向（零向量=不需要规避）。用于验证。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Collision")
	FVector LastAvoidDirection = FVector::ZeroVector;

	/** 应规避的那个对象（空=没人会撞上我）。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Collision")
	TObjectPtr<AActor> AvoidTargetActor = nullptr;

	/** 计算碰撞规避方向（纯函数，不移动）。返回零向量表示安全。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Collision")
	FVector ComputeCollisionAvoidance();

	// ============================================================
	// ⑨·c 游荡（Wander）—— 移植自源库 ch03_movement/Wander.h      → _Legacy.cpp
	//    ⚠️ 未接线：漫游轨迹平滑与否在 demo 里几乎不可辨，且要新写一个 Task，
	//       成本最高收益最低。这是主动放弃，不是遗漏。
	//
	// 问题：模板漫游是「走到随机点 → 停下 → 再走」，全程直线，像机器。
	// 解法：在角色前方 offset 处放一个半径 radius 的圆，每帧在圆上取
	//       一个随机漂移点朝它走。因为圆永远在前进方向前方，
	//       轨迹是平滑曲线，不会大角度跳变。
	//
	// 与源库的差异（胶水层）：
	//   源库用 random.Random（可注入 seed 复现）；
	//   UE 用 FRandomStream，同样支持固定 seed，行为可复现便于测试。
	//   源库用「二项分布近似」（两个均匀随机之差）让漂移集中在 0 附近，
	//   避免大角度抖动 —— 这个技巧原样保留。
	// ============================================================

	/** 是否启用游荡。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Wander")
	bool bUseWander = false;

	/** 游荡圆离角色多远（cm）。太近会原地打转，太远看不出去哪。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Wander", meta=(ClampMin="50.0", ClampMax="2000.0"))
	float WanderOffset = 600.0f;

	/** 游荡圆半径（cm）：越大拐得越急。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Wander", meta=(ClampMin="10.0", ClampMax="1000.0"))
	float WanderRadius = 200.0f;

	/** 漂移幅度（弧度）：单帧最大偏转角。越小越直。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Wander", meta=(ClampMin="0.0", ClampMax="3.14"))
	float WanderRate = 0.6f;

	/** 游荡随机种子。固定值可复现，0 = 每次不同。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Wander")
	int32 WanderSeed = 0;

	/** 最近一次算出的游荡目标点（画出来能看到圆）。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Wander")
	FVector LastWanderTarget = FVector::ZeroVector;

	/**
	 * 游荡：返回一个随机漂移后的目标点。
	 * 调用方把这个点交给 MoveToLocation 即可。
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Wander")
	FVector ComputeWander();

	// ============================================================
	// 调试
	// ============================================================

	/**
	 * 是否在屏幕打印诊断行。
	 *
	 * ★ 默认关闭：这是「每个敌人一行」，15 个敌人就是 15 行，
	 *   会把顶部聚合信息（ShootControl / LOD 统计）挤到看不见。
	 *   排查单个敌人的行为时再打开，并且需要
	 *   `ShooterAI.ScreenDebug 2`（详细级别）才会真正输出。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|Debug")
	bool bShowDiagnostics = false;

	/** 是否画落点球（红=预判点，黄=锁人物，绿=最后已知位置）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GameAI|Pursue|Debug")
	bool bDrawDebug = false;

	/** 本帧 AutoPilot 干了什么（屏幕上会打印，排查用）。 */
	UPROPERTY(BlueprintReadOnly, Category="GameAI|Pursue|Debug")
	FString AutoPilotState = TEXT("idle");

	// ============================================================
	// ③ 对外接口（导航投影 / 旧旁路 / 记忆 / 算法）
	// ============================================================
	// 这些是给行为层（StateTree Task）和蓝图调用的入口。
	// 注意：Task 只用到其中少数几个 —— 决策在 Task 里，这里只提供能力。

	/** 把点投影到 NavMesh；成功且距离在阈值内返回 true，并写出吸附后的点。 */
	UFUNCTION(BlueprintCallable, Category="GameAI|Pursue")
	bool ProjectToNavMesh(const FVector& Location, FVector& OutProjectedLocation) const;

	/** 手动跑一次 AutoPilot（旧旁路，Task 模式下不调用）。返回是否成功发起了移动。 */
	UFUNCTION(BlueprintCallable, Category="GameAI|Pursue|AutoPilot")
	bool AutoPilotTick(AActor* Target);

	/** 清除记忆（例如目标死亡后重置）。 */
	UFUNCTION(BlueprintCallable, Category="GameAI|Pursue|Memory")
	void ClearMemory();

	/** 核心算法（纯函数）：返回预测拦截点。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Pursue")
	FVector GetPursueTargetLocation(
		const FVector& TargetPosition, const FVector& TargetVelocity,
		const FVector& CharacterPosition, const FVector& CharacterVelocity) const;

	/** 计算预测时间（秒）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Pursue")
	float GetPredictionTime(
		const FVector& TargetPosition, const FVector& TargetVelocity,
		const FVector& CharacterPosition, const FVector& CharacterVelocity) const;

	/** 便捷封装：读 Target 与本组件的实时位置/速度，算好存入 CurrentPredictedLocation。 */
	UFUNCTION(BlueprintCallable, Category="GameAI|Pursue")
	FVector UpdatePursue(AActor* Target);

	/** 输出「追击方向加速度」（对应源库 Seek：normalize * maxAcceleration），供调试可视化。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="GameAI|Pursue")
	FVector GetPursueSteering(
		const FVector& TargetPosition, const FVector& TargetVelocity,
		const FVector& CharacterPosition, const FVector& CharacterVelocity,
		float MaxAcceleration) const;

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** 缓存的 AIController（避免每帧 Cast）。 */
	UPROPERTY(Transient)
	TObjectPtr<class AAIController> CachedAIController = nullptr;

	/**
	 * 上次受伤后经过的时间（秒）。
	 *
	 * 初值给一个大于任何谨慎期的大数，表示「很久没受伤」，
	 * 这样 IsWounded() 在首次受伤前天然返回 false，不需要额外的 bool 标志。
	 */
	float TimeSinceDamaged = 1.0e6f;

	/** OnTakeAnyDamage 的回调。 */
	UFUNCTION()
	void HandleAnyDamage(AActor* DamagedActor, float Damage, const class UDamageType* DamageType,
		class AController* InstigatedBy, AActor* DamageCauser);

	/**
	 * 缓存的 `Current HP` 属性指针。
	 *
	 * 为什么缓存：读血量每帧都要调用（Task 里判定档位），
	 * 而 `FindFProperty` 是按名字查找，开销远大于直接读值。
	 * 第一次读到就存下来，之后直接用。
	 */
	mutable FFloatProperty* CachedHealthProperty = nullptr;

	/** 血量若是 int 类型，用这个。 */
	mutable FIntProperty* CachedHealthIntProperty = nullptr;

	/** 血量若是 double 类型，用这个（有些蓝图的 float 实际是 double）。 */
	mutable FDoubleProperty* CachedHealthDoubleProperty = nullptr;

	/** 是否已经尝试过查找血量属性（避免每次都查）。 */
	mutable bool bHealthPropertySearched = false;

	/**
	 * 血量属性探测日志（第一次查找时填好）。
	 *
	 * 内容形如 `CurrentHP(FloatProperty)=87.0 MaxHP(FloatProperty)=100.0`，
	 * 直接显示在屏幕上就能知道：
	 *   · 属性真实名字（不是猜的）
	 *   · 属性的 C++ 类型
	 *   · 当前值
	 * 读不到血量时，这条日志就是唯一的线索，比反复猜名字有效得多。
	 */
	mutable FString HealthProbeLog;

	/**
	 * 缓存的原始移速。
	 *
	 * 逃跑时要把 MaxWalkSpeed 乘一个倍率，脱离后必须还原，
	 * 否则倍率会一层层叠上去（跑得越来越快）。
	 */
	mutable float CachedOriginalWalkSpeed = -1.0f;

	float AutoPilotRetryTimer = 0.0f;

	/** 是否已经做过出生点纠偏（只做一次）。 */
	bool bSnappedToNavMesh = false;

	/** 距离最后一次看见目标，过了多少秒。 */
	float TimeSinceLastSeen = 0.0f;

	/** 到达最后已知位置后，已经原地环视了多少秒。 */
	float ScanElapsed = 0.0f;

	/** 游荡用的随机流。构造时按 WanderSeed 初始化，seed=0 则每次随机。 */
	FRandomStream WanderRng;

	/** 是否已初始化 WanderRng（避免构造顺序问题）。 */
	bool bWanderRngInitialized = false;

	/** 碰撞规避上次检查的世界时间（秒）。用时间戳而非累加器，避免多调用点互相干扰。 */
	float LastCollisionCheckTime = -1000.0f;

	/** 上次碰撞检查的缓存结果，降频期间复用，避免行为抖动。 */
	FVector CachedAvoidDirection = FVector::ZeroVector;

	/** 取一次 AIController（带缓存）。 */
	class AAIController* ResolveAIController();

	/** 判断能否看见目标：AIController 视线检测 + 眼睛到眼睛射线双保险。 */
	bool CanSeeTarget(AActor* Target) const;

	/** 让 AIController 朝某点看（原地搜索时用）。 */
	void FaceLocation(const FVector& Location);

	/** 让 AIController 移动到某点。 */
	void MoveTo(const FVector& Destination, float AcceptanceRadius);
};
