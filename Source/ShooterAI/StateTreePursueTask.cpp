// StateTreePursueTask.cpp
//
// 三档阶段机：Pursuing（追预判点）→ Chasing（去最后已知位置）→ Scanning（原地环视）
//
// 调试约定：
//   任何 return EStateTreeRunStatus::Failed 都必须同时做两件事：
//     ① UE_LOG(Warning) —— 持久、可过滤
//     ② ShowDebug(红色) —— Play 时肉眼可见
//   原因：之前 Tick 里有一个 if(!Comp) return Failed 是静默的，
//   现象表现为「EnterState 后立刻 ExitState」，但完全查不到痕迹。

#include "StateTreePursueTask.h"

#include "StateTreeExecutionContext.h"
#include "PursueSteeringComponent.h"
#include "ShooterAIDebug.h"
#include "TacticalInfluenceMap.h"
#include "AIPathVizComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "AIController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "CollisionQueryParams.h"

namespace
{
	/**
	 * StateTree 的 Owner 在不同配置下可能是：
	 *   - AAIController（StateTreeAIComponent 的标准情况）
	 *   - UStateTreeAIComponent（组件本身）
	 *   - APawn（少数情况）
	 * 统一在这里解析，避免 EnterState 能找到、Tick 找不到的不一致。
	 */
	UObject* UnwrapOwner(const FStateTreeExecutionContext& Context)
	{
		// GetOwner() 返回 TNotNull<UObject*>，它有 operator UObject*() 隐式转换，
		// 直接赋给裸指针即可（没有 .Get() 方法）。
		UObject* OwnerObject = Context.GetOwner();
		if (UActorComponent* AsComponent = Cast<UActorComponent>(OwnerObject))
		{
			OwnerObject = AsComponent->GetOwner();
		}
		return OwnerObject;
	}

	AAIController* ResolveAIController(const FStateTreeExecutionContext& Context)
	{
		UObject* OwnerObject = UnwrapOwner(Context);
		if (!OwnerObject)
		{
			return nullptr;
		}

		if (AAIController* AIC = Cast<AAIController>(OwnerObject))
		{
			return AIC;
		}

		// Owner 是 Pawn：从控制器反查
		if (APawn* AsPawn = Cast<APawn>(OwnerObject))
		{
			return Cast<AAIController>(AsPawn->GetController());
		}

		return nullptr;
	}

	APawn* ResolvePawn(const FStateTreeExecutionContext& Context)
	{
		if (AAIController* AIC = ResolveAIController(Context))
		{
			return AIC->GetPawn();
		}
		return Cast<APawn>(UnwrapOwner(Context));
	}

	/**
	 * 屏幕调试文字：Task 用 Pawn+1 作 Key，与组件（Pawn）错开，避免互相覆盖。
	 *
	 * ★ bEnabled = false 时完全不打印。
	 *   屏幕空间有限，Pursue 的调试行会和 ShootControl / LOD 抢位置，
	 *   所以默认关闭，排查追击逻辑时在 Task 面板勾 bShowDebug。
	 */
	void ShowDebug(bool bEnabled, const APawn* Pawn, const FColor& Color, const FString& Message)
	{
		// 每个单位一行：只在详细级别（ShooterAI.ScreenDebug 2）且本 Task 开关打开时打印
		if (!bEnabled || !GEngine || !Pawn || !ShooterAIDebug::ShouldPrintPerActor())
		{
			return;
		}
		const uint64 Key = static_cast<uint64>(reinterpret_cast<UPTRINT>(Pawn)) + 1;
		GEngine->AddOnScreenDebugMessage(Key, 0.2f, Color, Message);
	}

	/** 失败统一出口：日志 + 屏幕字，禁止静默失败。 */
	EStateTreeRunStatus FailWithReason(const FStateTreeExecutionContext& Context,
		const APawn* Pawn, const FString& Reason, bool bShowDebugOnScreen)
	{
		UE_LOG(LogTemp, Warning, TEXT("[PursueTask] Tick 失败：%s  (Pawn=%s  Owner=%s)"),
			*Reason,
			*GetNameSafe(Pawn),
			*GetNameSafe(UnwrapOwner(Context)));

		ShowDebug(bShowDebugOnScreen, Pawn, FColor::Red,
			FString::Printf(TEXT("[PursueTask] 失败：%s"), *Reason));

		return EStateTreeRunStatus::Failed;
	}

	/** 把 RunStatus 转成可读文字，用于 ExitState 日志。 */
	FString RunStatusToString(EStateTreeRunStatus Status)
	{
		switch (Status)
		{
		case EStateTreeRunStatus::Running:   return TEXT("Running");
		case EStateTreeRunStatus::Succeeded: return TEXT("Succeeded");
		case EStateTreeRunStatus::Failed:    return TEXT("Failed");
		case EStateTreeRunStatus::Stopped:   return TEXT("Stopped");
		case EStateTreeRunStatus::Unset:     return TEXT("Unset");
		default:                             return TEXT("Unknown");
		}
	}

	/** 阶段名，用于日志。 */
	const TCHAR* PhaseToString(EPursuePhase Phase)
	{
		switch (Phase)
		{
		case EPursuePhase::Pursuing: return TEXT("PURSUING");
		case EPursuePhase::Chasing:  return TEXT("CHASING");
		case EPursuePhase::Scanning: return TEXT("SCANNING");
		case EPursuePhase::Retreating: return TEXT("RETREAT");
		case EPursuePhase::Fleeing:    return TEXT("FLEE");
		case EPursuePhase::Healing:    return TEXT("HEALING");
		case EPursuePhase::Desperate:  return TEXT("DESPERATE");
		default:                     return TEXT("?");
		}
	}

	/**
	 * 判断能否看见目标。
	 *
	 * 为什么 Task 里要自己判一次，而不是依赖 StateTree 的 Target 绑定：
	 *   绑定只告诉我们"感知系统当前有没有目标"，但感知有延迟/去抖。
	 *   这里用「视线射线」做快速判断，能让"看得见/看不见"的切换更及时，
	 *   而且和组件里的判定逻辑保持一致。
	 */
	bool CanSeeTarget(const APawn* Pawn, const AActor* Target)
	{
		if (!Pawn || !Target)
		{
			return false;
		}

		const UWorld* World = Pawn->GetWorld();
		if (!World)
		{
			return false;
		}

		FCollisionQueryParams Params(SCENE_QUERY_STAT(PursueTaskLOS), false, Pawn);
		Params.AddIgnoredActor(Target);
		FHitResult Hit;
		const FVector Eye = Pawn->GetPawnViewLocation();
		const FVector TargetEye = Target->GetActorLocation() + FVector(0.0f, 0.0f, 50.0f);

		// 打不到东西 = 视线通畅
		return !World->LineTraceSingleByChannel(Hit, Eye, TargetEye, ECC_Visibility, Params);
	}
}

// ============================================================
// 组件解析
// ============================================================

UPursueSteeringComponent* FStateTreePursueTask::ResolveComponent(const FStateTreeExecutionContext& Context)
{
	APawn* Pawn = ResolvePawn(Context);
	if (!Pawn)
	{
		return nullptr;
	}
	return Pawn->FindComponentByClass<UPursueSteeringComponent>();
}

// ============================================================
// Task 生命周期
// ============================================================

EStateTreeRunStatus FStateTreePursueTask::EnterState(FStateTreeExecutionContext& Context,
	const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	UPursueSteeringComponent* Comp = ResolveComponent(Context);

	if (!Comp)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[PursueTask] EnterState 失败：%s 上没有 Pursue Steering 组件"),
			*GetNameSafe(UnwrapOwner(Context)));
		return EStateTreeRunStatus::Failed;
	}

	// 每次进入状态重置运行时状态。
	// 注意：不清 LastKnownLocation 和 bHasMemory —— 那是"跨状态"的记忆，
	// 从 Attack 退回 Chase 时应该还记得上次在哪见过你。
	Data.TickCount = 0;
	Data.Phase = EPursuePhase::Pursuing;
	Data.TimeSinceLastSeen = 0.0f;
	Data.ChaseElapsed = 0.0f;
	Data.ScanElapsed = 0.0f;

	// 关键：Task 模式下关掉组件自己的 AutoPilot。
	// 否则组件仍会在 TG_PostUpdateWork 里抢移动指令，和 Task 打架。
	Comp->bAutoPilot = false;

	UE_LOG(LogTemp, Warning,
		TEXT("[PursueTask] EnterState  Owner=%s  Pawn=%s  → AutoPilot 已关闭，进入 PURSUING"),
		*GetNameSafe(UnwrapOwner(Context)),
		*GetNameSafe(ResolvePawn(Context)));

	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FStateTreePursueTask::Tick(FStateTreeExecutionContext& Context,
	const float DeltaTime) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	APawn* Pawn = ResolvePawn(Context);

	// ---------- 探针：确认 Tick 到底有没有被调用 ----------
	++Data.TickCount;
	if (Data.TickCount == 1)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[PursueTask] 第 1 次 Tick 进来了（Pawn=%s）→ 说明状态没有立即结束"),
			*GetNameSafe(Pawn));
	}

	// ---------- 逐项校验，每个失败点都留痕 ----------
	UPursueSteeringComponent* Comp = ResolveComponent(Context);
	if (!Comp)
	{
		return FailWithReason(Context, Pawn, TEXT("找不到 Pursue Steering 组件"), Data.bShowDebug);
	}

	// ------------------------------------------------------------
	// 停火状态：每帧先解除，再由本帧实际所处的阶段决定是否重新压制。
	// ------------------------------------------------------------
	//
	// ★ 为什么必须"每帧重置再按需压制"，不能只在进入 FLEE 时设 true
	//   状态是持续量，不是事件。一旦某条 return 路径忘了写，
	//   上一次的 true 就会永久残留 —— 敌人之后再也不开火，
	//   而且从屏幕上看不出原因（PursueTask 认为自己在追击，
	//   ShootControl 却一直收到"禁止开火"）。
	//   先无条件解除、再按阶段压制，任何分支都自然带上正确值。
	Comp->SetFireSuppressed(false, FString());

	// 把当前阶段同步给可视化组件（让头顶标签能显示 HEALING / DESPERATE 这类细阶段）
	// 组件自己只认 CurrentMode，不知道 Task 内部的五档。
	if (Pawn)
	{
		if (UAIPathVizComponent* Viz = Pawn->FindComponentByClass<UAIPathVizComponent>())
		{
			Viz->SetDisplayPhase(PhaseToString(Data.Phase));
		}
	}

	AAIController* AIC = ResolveAIController(Context);
	if (!AIC)
	{
		return FailWithReason(Context, Pawn, FString::Printf(
			TEXT("拿不到 AIController（Owner 类型=%s）"),
			*GetNameSafe(UnwrapOwner(Context) ? UnwrapOwner(Context)->GetClass() : nullptr)),
			Data.bShowDebug);
	}

	if (!Pawn)
	{
		return FailWithReason(Context, nullptr, TEXT("AIController 上没有 Pawn"), Data.bShowDebug);
	}

	// ---------- 找目标 ----------
	AActor* Target = Data.Target;
	if (!Target)
	{
		// 没绑就找 0 号玩家
		Target = UGameplayStatics::GetPlayerPawn(Comp, 0);
	}

	if (!Target)
	{
		ShowDebug(Data.bShowDebug, Pawn, FColor::Yellow, TEXT("[PursueTask] 没找到目标（保持 Running）"));
		return Data.bFailWhenNoTarget
			? FailWithReason(Context, Pawn, TEXT("没有目标且设置了 FailWhenNoTarget"), Data.bShowDebug)
			: EStateTreeRunStatus::Running;
	}

	// ============================================================
	// 档位 ①：看得见 → 追预判点
	// ============================================================

	const bool bCanSee = CanSeeTarget(Pawn, Target);
	const float Distance = FVector::Dist(Pawn->GetActorLocation(), Target->GetActorLocation());

	// 分离偏移每帧只算一次，而且【总是算】：
	//   - 算它会同时刷新 NearestAllyDistance（观测值），关掉分离时也要能读到真实距离
	//   - 是否施加由 Data.bUseSeparation 决定（不勾时把偏移归零）
	// 为什么提到 bCanSee 之前：原来它在 StopDistance 判断之后才调用，
	// 而"6 个敌人围住玩家"恰好都走 STOP 分支，于是永远看不到最近同伴数值。
	// ComputeSeparationOffset 是纯计算（不改位置），提前算没有副作用。
	FVector SepOffset = Comp->ComputeSeparationOffset();
	if (!Data.bUseSeparation)
	{
		SepOffset = FVector::ZeroVector;
	}

	if (bCanSee)
	{
		// 看见就刷新记忆——这是“最后已知位置”的唯一来源
		Data.LastKnownLocation = Target->GetActorLocation();
		Data.bHasMemory = true;
		Data.TimeSinceLastSeen = 0.0f;
		Data.ScanElapsed = 0.0f;
	}

	// ============================================================
	// 血量档位：优先于普通追击（★ 必须在 bCanSee 之外！）
	// ============================================================
	//
	// 分三档而不是“受伤就统一退”，因为统一的反应观感很假：
	//   挨一枪就转身跑 → 像游戏坏了
	//   挨一枪完全不理 → 像 AI 不会躲
	// 真实反应是渐进的：先边打边拉开，快死了才转身跑。
	//
	// 这段必须在“站桩/追击”判断**之前**，
	// 否则低血量的敌人会先走 STOP 分支站住不动，永远退不下去。
	//
	// ★★ 为什么还必须放在 if (bCanSee) 之外 —— 本轮最关键的一处修正
	//
	//   逃跑成功的必然结果就是「看不见玩家」：掩体挡住了视线。
	//   而这一整段原先写在 if (bCanSee) 里面，于是形成自我否定的死锁：
	//
	//     血量 25% → 逃向掩体 → 躲进去 → 看不见玩家
	//     → 掉出 bCanSee → 走“丢失目标”分支 → SCANNING 原地环视
	//     → 环视超时 → MemoryDuration 到期 → return Failed
	//     → StateTree 退出 Chase → 头顶变灰(IDLE)
	//
	//   实测表现就是这个链条：一直 FLEE、中间跳 SCAN、最后变灰，
	//   而且永远进不了 HEALING。
	//   逻辑上它自相矛盾：躲起来正是为了回血，却因为“躲起来了”
	//   而放弃回血。逃跑与回血必须处在同一个不依赖视线的分支里。
	const float HealthPercent = Comp->GetHealthPercent();
	const bool bCritical = Comp->IsCriticalHealth();
	const bool bRetreat = !bCritical && Comp->IsLowHealth();

	// ------------------------------------------------------------
	// ★ 危险血量分两条路（关键分支）：
	//   有回血道具 → FLEE（逃向掩体，准备回血）
	//   没道具了   → DESPERATE（拼死冲锋）
	//
	//   为什么不统一"危险就逃"：
	//     没道具还逃的话，它会在掩体后面永远蹲着（回不了血、也不打），
	//     表现为"敌人跑到角落发呆"，观感是 bug。
	//     真实的困兽逻辑：还有后手就撤，没后手就拼。
	// ------------------------------------------------------------
	const bool bHasHeal = Comp->HasHealItem();

	// 「该走逃向掩体 / 回血」这条流程。
	//
	// ★★ 这里曾经有一个「先有蛋还是先有鸡」的死锁，是"橙色、抽搐、不回血"的真凶
	//
	//   错误写法： bShouldHeal = bHealItemConsumed && IsLowHealth()
	//
	//   bHealItemConsumed 只在【蹲下回血的那一刻】才被置 true，
	//   而"要不要去蹲下"又由 bShouldHeal 决定。于是：
	//     初始 bHealItemConsumed = false
	//     → bShouldHeal = false
	//     → 所有找掩体 / 回血的 if 全部跳过
	//     → 只走 RETREAT 分支（橙色的那条）
	//     → 永远没机会扣道具 → bHealItemConsumed 永远是 false
	//   它自己把自己锁死了。表现就是：
	//     · 头顶橙色 RETREAT（不是红色的 FLEE）
	//     · 一直退却永远不回血
	//     · RETREAT 每帧重算目标点 → 原地抽搐
	//
	//   正确写法：两个【独立的入口】都要算进去
	//     ① 首次进入：濒死 + 还有道具  → 去找掩体
	//     ② 维持中：  道具已扣、血还没回到 60% → 继续蹲着回完
	//   出口由 ② 的血量条件自然完成（血量 > 60% → IsLowHealth 为假）。

	// ------------------------------------------------------------
	// ★ 回血无望判定（第二个死锁的兜底）
	//
	//   掩体是几何条件，不是必然存在的。在完全开阔的地图上，
	//   敌人可能永远找不到"玩家看不到我"的位置 —— 于是它带着道具
	//   永远逃跑、永不回血、永不转拼死，血量卡在 25% 以下不变。
	//   玩家看到的是"它一直跑圈，既不打我也不死"。
	//
	//   语义：带着急救包却找不到地方用 → 最后只能拼命。
	//   这是困兽逻辑的完整版：有后手就撤，**后手用不上就拼**。
	//
	//   只在「还没扣过道具」时统计：一旦进过回血（道具已扣），
	//   走的是"被打断换点"那条路，不该被这个超时误判。
	// ------------------------------------------------------------
	const bool bHealHopeless =
		Data.HealHopelessTimeout > 0.0f
		&& bHasHeal
		&& bCritical
		&& !Data.bHealItemConsumed
		&& Data.FleeElapsed >= Data.HealHopelessTimeout;

	const bool bShouldHeal = bHasHeal
		&& !bHealHopeless
		&& (bCritical || (Data.bHealItemConsumed && Comp->IsLowHealth()));

	// 拼死：没道具了，或者有道具但确认回血无望
	const bool bDesperate = bCritical && (!bHasHeal || bHealHopeless);

	// 累计"想回血但还没成功"的时长。
	// ★ 条件与 bHealHopeless 严格对齐：只在【还没扣过道具】时计时。
	//   一旦进过 HEALING（道具已扣），走的是"被打断换点"那条路，
	//   由 MaxCoverAttempts 与就地回血处理，不该被这个超时误判 ——
	//   否则慢速回血参数（比如每秒只回 1 点）会把正常回血判成无望。
	if (bShouldHeal && !Data.bHealItemConsumed)
	{
		Data.FleeElapsed += DeltaTime;
	}

	// 威胁位置：看得见用实时位置；看不见就用「你最后一次出现的地方」。
	// 逃跑方向要背离它 —— 而不是朝一个已经看不到的目标乱跑。
	FVector ThreatLoc = Target->GetActorLocation();
	if (!bCanSee && Data.bHasMemory)
	{
		ThreatLoc = Data.LastKnownLocation;
	}

	if (bCritical || bRetreat || bDesperate)
	{
		// ★ 阶段名只表示"当前在做什么"，不表示"血量状态"
		//   逃向掩体的路上是 FLEE，真正蹲下回血才切到 Healing。
		//   早期版本只要血量到 25% 就打 HEALING 标签，
		//   结果敌人一边全速跑一边显示"回血中"，调试时极容易误判。
		Data.Phase = bDesperate ? EPursuePhase::Desperate
			: (bShouldHeal ? EPursuePhase::Fleeing : EPursuePhase::Retreating);

		// ------------------------------------------------------------
		// FLEE 期间停火
		// ------------------------------------------------------------
		//
		// ★ 为什么逃命时不该开枪
		//   ① 观感：背对玩家全速跑却还在命中，非常假
		//   ② 逻辑：FLEE 的语义是"脱离战斗去疗伤"，不是"边退边打"
		//      （那是 RETREAT，那边刻意保留了面朝玩家 + 开火）
		//   ③ 游戏性：如果逃跑路上照样输出，玩家打断回血得不到任何收益，
		//      "压上去打断它" 这个玩法就失去了意义
		//
		//   ★ 注意这里不能靠 ShootControl 的视线门控来解决 ——
		//     逃跑的起因恰恰是【被看见了】，门控在这里永远放行。
		//     视线门控回答"墙挡不挡"，这里回答"该不该打"，两件事。
		if (bShouldHeal)
		{
			Comp->SetFireSuppressed(true, TEXT("逃跑找掩体"));
		}

		const FVector MyLoc = Pawn->GetActorLocation();

		// ============================================================
		// 分支 A：DESPERATE —— 没道具了，拼死冲锋
		// ============================================================
		//
		// ★ 为什么不再后退
		//   没有回血手段还后退，它会在掩体后永远蹲着（回不了血、也不打），
		//   表现为"敌人跑角落发呆"，观感是 bug。
		//   困兽逻辑：还有后手就撤，没后手就拼。
		//
		// ★ 为什么还要提速
		//   垂死冲锋比平时更猛，符合直觉，也让"绝望"这个状态在画面上可辨。
		if (bDesperate)
		{
			// 用逃跑的速度倍率（1.6）不如用专门的 1.3：
			// 拼死是"冲"，不是"逃"，速度提升应该温和一些。
			// 这里直接复用 ApplyFleeSpeed 的机制，但传 false 先归位，
			// 再由下面的直接写速度覆盖。
			Comp->ApplyFleeSpeed(false);

			if (APawn* OwnerPawn = Cast<APawn>(Pawn))
			{
				if (UCharacterMovementComponent* Movement =
					OwnerPawn->FindComponentByClass<UCharacterMovementComponent>())
				{
					const float BaseSpeed = FMath::Max(Comp->GetOriginalWalkSpeed(), 1.0f);
					Movement->MaxWalkSpeed = BaseSpeed * FMath::Max(Data.DesperateSpeedMultiplier, 1.0f);
				}
			}

			// ------------------------------------------------------------
			// ★ 必须先判「到射程了就停下开火」，不能一路贴到脸上
			//
			//   这一段是「拼死阶段一枪不放」的修复。
			//   原实现只有一句 MoveToLocation(玩家预判点)，没有停火点，
			//   于是它一路贴到玩家身前 —— 而蓝图侧的射击函数带距离判定，
			//   贴太近反而不开火，表现为"举着枪撞人，一枪不放"。
			//
			//   拼死一搏的正确表现不是"撞死你"，而是"不再躲、正面死磕"：
			//   冲到能开枪的位置就站定输出，你跑它再追。
			//   对比普通追击：那边也是 StopDistance 到了就 StopMovement，
			//   两处行为应当一致 —— 低血量不该改变"怎么开枪"，只该改变"敢不敢上"。
			//
			// ★ 为什么还必须 && bCanSee
			//   只看距离会让它停在柱子后面：距离够近、但视线被挡，
			//   于是既不开火也不移动 —— 从"不打"变成"彻底站住发呆"。
			//   看不见就继续冲（MoveToLocation 会绕路），冲到能看见为止。
			const float EngageDist = Data.DesperateEngageDistance > 0.0f
				? Data.DesperateEngageDistance
				: Data.StopDistance;

			if (EngageDist > 0.0f && Distance <= EngageDist && bCanSee)
			{
				// 已进入交火距离：站定、面朝玩家、把枪口对准 —— 开火交给 ShootControl
				AIC->StopMovement();
				AIC->SetFocalPoint(ThreatLoc, EAIFocusPriority::Gameplay);
				Data.bDesperateHolding = true;

				// 速度还原，避免"站着但移动速度仍是 1.3 倍"影响后续状态
				Comp->ApplyFleeSpeed(false);

				ShowDebug(Data.bShowDebug, Pawn, FColor(255, 60, 60),
					FString::Printf(
						TEXT("[PursueTask] DESPERATE-FIRE HP=%.0f%% 无道具·站定死磕 d=%.0f/%.0f 视线=%s"),
						HealthPercent * 100.0f, Distance, EngageDist,
						bCanSee ? TEXT("有") : TEXT("无")));

				return EStateTreeRunStatus::Running;
			}

			// 已经站定过，但玩家只挪出了一点点 → 继续站定，不追。
			// 这一段就是滞回：退出阈值放大到 1.3 倍，避免走一步就 停—冲—停 地抖。
			if (Data.bDesperateHolding
				&& Distance <= EngageDist * 1.3f && bCanSee)
			{
				AIC->StopMovement();
				AIC->SetFocalPoint(ThreatLoc, EAIFocusPriority::Gameplay);

				ShowDebug(Data.bShowDebug, Pawn, FColor(255, 60, 60),
					FString::Printf(
						TEXT("[PursueTask] DESPERATE-FIRE HP=%.0f%% 无道具·死磕(滞回) d=%.0f/%.0f"),
						HealthPercent * 100.0f, Distance, EngageDist * 1.3f));

				return EStateTreeRunStatus::Running;
			}

			// 出交火距离（或视线断了）→ 恢复冲锋
			Data.bDesperateHolding = false;

			// 还没进交火距离 → 继续冲锋（用预判点，比当前位置更准）
			const FVector ChargeDest = Comp->UpdatePursue(Target);

			// 拼死冲锋时面朝玩家——要一边冲一边打
			AIC->SetFocalPoint(ThreatLoc, EAIFocusPriority::Gameplay);

			AIC->MoveToLocation(
				ChargeDest, Data.AcceptanceRadius,
				true, true, true, false, nullptr, true);

			ShowDebug(Data.bShowDebug, Pawn, FColor(255, 60, 60),
				FString::Printf(
					TEXT("[PursueTask] DESPERATE HP=%.0f%% 无道具·拼死冲锋 d=%.0f/%.0f 视线=%s"),
					HealthPercent * 100.0f, Distance, EngageDist,
					bCanSee ? TEXT("有") : TEXT("无")));

			return EStateTreeRunStatus::Running;
		}

		// ============================================================
		// 分支 B：FLEE（有道具，找掩体） / RETREAT（边退边打）
		// ============================================================

		// 远离玩家的水平方向
		FVector AwayDir = MyLoc - ThreatLoc;
		AwayDir.Z = 0.0f;
		if (AwayDir.SizeSquared() < 1.0f)
		{
			// 正好重合：往自己背后退
			AwayDir = Pawn->GetActorForwardVector() * -1.0f;
			AwayDir.Z = 0.0f;
		}
		AwayDir = AwayDir.GetSafeNormal();

		// 目标距离：逃跑比边退边打退得更远
		const float DesiredDistance = bShouldHeal ? Data.FleeDistance : Data.RetreatDistance;

		// 最远距离：FLEE 额外放大。
		// "边退边打"要留在战场里保持压力，而"真逃跑"应该尽量脱离战斗，
		// 两档如果上限一样，观感上就没有区别了。
		const float MaxRange = bShouldHeal
			? Data.RetreatMaxRange * FMath::Max(Data.FleeMaxRangeMultiplier, 1.0f)
			: Data.RetreatMaxRange;

		// 后撤目的地。若已经超过 DesiredDistance，就往"保持该距离"的圈上退，
		// 避免走成一条直线一直后退。
		const float RetreatTargetDist = FMath::Min(
			FMath::Max(Distance, DesiredDistance), MaxRange);

		FVector RetreatDest = ThreatLoc + AwayDir * RetreatTargetDist;

		// ------------------------------------------------------------
		// ★ FLEE 优先找「真掩体」（射线判定），找到就用它当目标点
		//
		//   影响图选的是"威胁最低的点"，那是**距离场**意义的安全；
		//   而射线判定的是"玩家看不到我"，那是**几何**意义的安全。
		//   回血需要后者 —— 蹲在开阔地回血等于送死。
		//   所以这里先试掩体，找不到再退回影响图/直线后退。
		// ------------------------------------------------------------
		FString CoverNote;
		if (bShouldHeal)
		{
			// ① 先问一句：我现在就安全吗？——**这一步不看 bGiveUpCover**
			//
			// ★ 这是"卡死"的主要成因之一
			//     敌人本来就贴在柱子后面，旧逻辑却坚持把它送去
			//     0.55~1.0 倍半径（825~1500cm）外的"采样点"。
			//     那个点常常不可达（隔壁房间、台阶、被墙隔开），
			//     MoveToLocation 走到"最近可行点"后就再也不动了 ——
			//     距离永远大于到达半径，每帧重发请求，每帧原地不动。
			//     人已经在掩体里，就不该再跑：就地回血。
			//
			// ★★ 为什么必须放在 bGiveUpCover 之外 —— 修的是「永远 FLEE」死锁
			//
			//   旧结构是 if (bShouldHeal && !bGiveUpCover) {...} else { bHasCoverTarget=false; }
			//   一旦放弃掩体，就会走进 else 把 bHasCoverTarget 清零，于是：
			//     bGiveUpCover=true → bHasCoverTarget=false
			//     → 下面的"到达掩体→回血"分支永远进不去
			//     → 永远不调 ConsumeHealItem，道具永不减少
			//     → bHasHeal 永远为真 → bDesperate 永远为假
			//     → 【永远 FLEE，永远不出紫色拼死】
			//   而 bGiveUpCover 只在"血量回到 60%"时重置 —— 可血量回不去，
			//   因为回不了血。**完全自我锁死。**
			//
			//   语义上"放弃找新掩体" ≠ "不能在当前位置回血"。
			//   能否回血只取决于一件事：**玩家看不看得见我**。
			if (Comp->IsInCover(MyLoc, Target))
			{
				Data.CoverLocation = MyLoc;
				Data.bHasCoverTarget = true;
				RetreatDest = MyLoc;
				CoverNote = Data.bGiveUpCover ? TEXT(" 就地掩体(已放弃找点)") : TEXT(" 就地掩体");
			}
			else if (!Data.bGiveUpCover)
			{
				if (Data.bHasCoverTarget)
				{
					// 已经有掩体目标 → 继续走（不要每帧重找，否则会左右摇摆）
					CoverNote = TEXT(" 掩体(已定)");
					RetreatDest = Data.CoverLocation;
				}
				else
				{
					FVector Found;
					if (Comp->FindCoverPointEx(MyLoc, Target, Data.CoverSearchRadius, Found,
						&Data.RejectedCovers, Data.CoverArriveRadius + 100.0f))
					{
						Data.CoverLocation = Found;
						Data.bHasCoverTarget = true;
						Data.CoverElapsed = 0.0f;
						RetreatDest = Found;
						CoverNote = TEXT(" 掩体(新找)");
					}
					else
					{
						// 一个都找不到 → 本次逃跑放弃"找新掩体"，改直线远离。
						// 注意：只是不再找点，跑到安全处仍然可以就地回血（见上面 ①）。
						Data.bGiveUpCover = true;
						CoverNote = TEXT(" 无掩体→直线退");
					}
				}
			}
			else
			{
				// 已放弃找掩体，且当前位置不安全 → 继续远离，下一帧再测
				Data.bHasCoverTarget = false;
				CoverNote = TEXT(" 放弃掩体→直线退");
			}
		}
		else
		{
			// RETREAT（边退边打）：不需要掩体
			// （清掉可能的残留掩体目标，避免下次 FLEE 用旧点）
			Data.bHasCoverTarget = false;
			CoverNote = FString();
		}

		// 影响图微调：只在没有掩体目标时才用
		FString InfluenceNote;
		if (!Data.bHasCoverTarget && Data.bUseInfluenceForRetreat)
		{
			if (const ATacticalInfluenceMap* InfluenceMap =
				ATacticalInfluenceMap::FindInfluenceMap(Pawn))
			{
				if (InfluenceMap->IsMapReady())
				{
					FVector SaferDest;
					if (InfluenceMap->FindSafestPointNear(RetreatDest,
						Data.InfluenceSearchRadius, SaferDest))
					{
						// 只在"确实更安全"时才替换，避免无意义抖动
						const float OldCombined = InfluenceMap->GetCombinedAt(RetreatDest);
						const float NewCombined = InfluenceMap->GetCombinedAt(SaferDest);

						if (NewCombined > OldCombined + Data.InfluenceImproveThreshold)
						{
							InfluenceNote = FString::Printf(TEXT(" 影响图%+.2f"),
								NewCombined - OldCombined);
							RetreatDest = SaferDest;
						}
					}
				}
			}
		}

		// 分离偏移仍然叠加（多个敌人同时后撤时不要叠在一起）
		RetreatDest += SepOffset;

		FVector ProjectedRetreat;
		if (Comp->ProjectToNavMesh(RetreatDest, ProjectedRetreat))
		{
			RetreatDest = ProjectedRetreat;
		}

		// 逃跑时提速；边退边打保持原速（还要瞄准射击，跑太快反而打不准）
		if (bShouldHeal)
		{
			Comp->ApplyFleeSpeed(true);
		}
		else
		{
			Comp->ApplyFleeSpeed(false);
		}

		// ------------------------------------------------------------
		// 朝向：两种档位必须分开处理
		// ------------------------------------------------------------
		//
		// ★ RETREAT（边退边打）→ 面朝玩家
		//   要一边后退一边开火，必须看着人，否则打不着。
		//   表现为"倒着走" —— 这里倒着走是正确的，不是 bug。
		//
		// ★ FLEE（真逃跑）→ 面朝逃跑方向
		//   之前实现漏了这一点：FLEE 时仍用 SetFocalPoint 盯着玩家，
		//   于是 60% 血量以下是"面朝你倒退"，25% 以下还是"面朝你倒退"，
		//   只有速度变快 —— 观感上根本看不出"它在逃命"。
		//   真正的逃跑要背对你、全速跑，把后背留给你才对。
		//
		//   实现方式：清掉 Focus，让 CharacterMovement 按速度方向转身
		//   （前提是 AI 的 bOrientRotationToMovement 或 bUseControllerRotationYaw
		//     配置为按移动方向转；若配置不同，这里会退化成继续朝原方向，
		//     但至少不会一直死盯玩家）。
		if (bShouldHeal)
		{
			// 逃跑：不再盯玩家，交给移动方向决定朝向
			AIC->ClearFocus(EAIFocusPriority::Gameplay);
		}
		else if (Data.bFaceTargetWhileRetreating)
		{
			// 边退边打：必须看着玩家才能开火
			AIC->SetFocalPoint(ThreatLoc, EAIFocusPriority::Gameplay);
		}
		else
		{
			AIC->ClearFocus(EAIFocusPriority::Gameplay);
		}

		// ============================================================
		// 分支 C：FLEE → 掩体 → 回血
		// ============================================================
		//
		// ★ "到了"的判据从【距离】改成【射线】
		//
		//   旧写法：DistToCover <= CoverArriveRadius(150) 就算到达，
		//   停下，再用 IsInCover 检查是否暴露。
		//   矛盾就藏在这两步之间：掩体点是【在点本身】用眼高射线
		//   验过的，而敌人会提前 150cm 停下 —— 那 1.5 米恰好是
		//   "从柱子后面绕出来"的距离。于是每帧都在重复：
		//     到达(140cm) → 一测：暴露 → 清掉目标 → 重新找
		//     → 又选中同一个最近点 → 140cm < 150 → 算"到达" → 又暴露
		//   位移恒为 0、血量不涨 —— 这就是"血量一过 25% 卡住不动"。
		//
		//   新写法：只要射线判定"挡住了"就立刻蹲下回血。
		//   距离只决定"还要不要继续跑"，不再决定"能不能回血"。
		if (bShouldHeal && Data.bHasCoverTarget)
		{
			const float DistToCover = FVector::Dist2D(MyLoc, Data.CoverLocation);
			const bool bHiddenNow = Comp->IsInCover(MyLoc, Target);

			// 关掉"被发现就打断"时，只要到位就照回不误（调试用）
			const bool bSafe = bHiddenNow || !Data.bHealInterruptedBySight;

			if (!bSafe && DistToCover > Data.CoverArriveRadius)
			{
				Data.CoverElapsed += DeltaTime;

				if (Data.CoverElapsed > Data.CoverTravelTimeout)
				{
					// ★ 熔断：跑不到就放弃。
					//   宁可直线逃离，也绝不能站在原地把时间耗光。
					Data.RejectedCovers.Add(Data.CoverLocation);
					Data.bHasCoverTarget = false;
					Data.bGiveUpCover = true;
					Data.CoverElapsed = 0.0f;

					ShowDebug(Data.bShowDebug, Pawn, FColor::Red,
						FString::Printf(TEXT("[PursueTask] 掩体超时%.1fs → 放弃·直线远离"),
							Data.CoverTravelTimeout));

					// 不 return：本帧继续往下走到"直线远离"，立刻动起来
				}
				else
				{
					// 停车半径收紧到 1/3（150 → 50cm）。
					// 别在掩体外 1.5 米就停 —— 那正是会暴露的位置。
					const float TightRadius =
						FMath::Max(Data.CoverArriveRadius * 0.33f, 30.0f);

					AIC->MoveToLocation(Data.CoverLocation, TightRadius,
						true, true, true, false, nullptr, true);

					ShowDebug(Data.bShowDebug, Pawn, FColor::Red,
						FString::Printf(TEXT("[PursueTask] FLEE HP=%.0f%% 去掩体 距离%.0f 道具=%d%s"),
							HealthPercent * 100.0f, DistToCover,
							Comp->RemainingHealItems, *CoverNote));
					return EStateTreeRunStatus::Running;
				}
			}
			else if (!bSafe)
			{
				// 到了指定点却还是暴露（或回血途中被玩家绕过来看见）
				// → 这个点作废，拉黑后重新找。不拉黑就会反复选中它。
				Data.RejectedCovers.Add(Data.CoverLocation);
				Data.bHasCoverTarget = false;
				Data.CoverElapsed = 0.0f;
				Data.HealTimer = 0.0f;
				++Data.CoverFailCount;

				if (Data.CoverFailCount >= Data.MaxCoverAttempts)
				{
					Data.bGiveUpCover = true;
				}

				ShowDebug(Data.bShowDebug, Pawn, FColor::Orange,
					FString::Printf(TEXT("[PursueTask] 掩体无效 HP=%.0f%% 第%d/%d次"),
						HealthPercent * 100.0f, Data.CoverFailCount, Data.MaxCoverAttempts));
				return EStateTreeRunStatus::Running;
			}
			else
			{
				// 安全了 → 停下回血
				AIC->StopMovement();
				Comp->ApplyFleeSpeed(false);   // 回血时不需要加速

				// 静止回血，但面朝玩家方向（视觉上像"躲着警戒"，比背对好）
				AIC->SetFocalPoint(ThreatLoc, EAIFocusPriority::Gameplay);

				Data.Phase = EPursuePhase::Healing;
				Data.CoverElapsed = 0.0f;

				// 成功进入回血 → "回血无望"计时归零。
				// 否则那些逃跑途中绕的时间会一直累加，
				// 下一次掉血时开局就接近超时，一进场就误判成无望。
				Data.FleeElapsed = 0.0f;

				// 回血时停火。
				// 蹲在掩体后"疗伤"同时还精准点射，是最违和的画面；
				// 停火也给了玩家一个真实的窗口：看到它蹲着，就知道必须压上去打断。
				Comp->SetFireSuppressed(true, TEXT("回血中"));

				// 第一次进回血：消耗一个道具
				// ★ 用独立布尔量记录，不能用 HealedAmount==0 判断 ——
				//   万一 ApplyHeal 写不进去，HealedAmount 恒为 0，
				//   就会每帧扣一个道具，5 帧内耗光后直接跳去拼死。
				if (!Data.bHealItemConsumed)
				{
					if (!Comp->ConsumeHealItem())
					{
						// 道具刚用完 → 转拼死
						Data.bHasCoverTarget = false;
						ShowDebug(Data.bShowDebug, Pawn, FColor(255, 60, 60),
							TEXT("[PursueTask] 道具耗尽 → 转拼死冲锋"));
						return EStateTreeRunStatus::Running;
					}
					Data.bHealItemConsumed = true;
				}

				// 按 HealInterval 节拍回血
				Data.HealTimer += DeltaTime;
				if (Data.HealTimer >= Comp->HealInterval)
				{
					Data.HealTimer = 0.0f;

					if (Comp->ApplyHeal(Comp->HealPerTick))
					{
						Data.HealedAmount += Comp->HealPerTick;
					}
				}

				ShowDebug(Data.bShowDebug, Pawn, FColor::Green,
					FString::Printf(TEXT("[PursueTask] HEALING HP=%.0f%% 已回%.0f 道具剩%d"),
						Comp->GetHealthPercent() * 100.0f, Data.HealedAmount,
						Comp->RemainingHealItems));

				return EStateTreeRunStatus::Running;
			}
		}

		// 后撤移动：节流下发，避免每帧重置路径造成抽搐。
		//
		// RetreatDest 是按玩家实时位置算的，玩家挪一点目标就挪一点。
		// 每帧 MoveToLocation 会清掉已有路径重新规划，于是角色在
		// "起步—刹车—再起步"之间抖 —— 这就是抽搐的来源。
		//
		// 三个条件任一成立才重新下发：
		//   ① 从没下发过
		//   ② 目标点相对上次移动超过阈值（玩家真的走了）
		//   ③ 角色停住了但还没到位（被卡住 / 路径失效，必须重试）
		{
			const bool bNeverIssued = !Data.bHasLastMoveDest;
			const bool bDestMovedFar =
				FVector::Dist2D(Data.LastMoveDest, RetreatDest) > Data.RetreatRepathThreshold;

			const bool bStalledButFar =
				Pawn->GetVelocity().SizeSquared2D() < 100.0f
				&& FVector::Dist2D(MyLoc, RetreatDest) > Data.AcceptanceRadius * 2.0f;

			if (bNeverIssued || bDestMovedFar || bStalledButFar)
			{
				AIC->MoveToLocation(
					RetreatDest,
					Data.AcceptanceRadius,
					true, true, true, false, nullptr, true
				);

				Data.LastMoveDest = RetreatDest;
				Data.bHasLastMoveDest = true;
			}
		}

		ShowDebug(Data.bShowDebug, Pawn, bShouldHeal ? FColor::Red : FColor::Orange,
			FString::Printf(TEXT("[PursueTask] %s HP=%.0f%% d=%.0f 目标%.0f%s%s 道具=%d 无望%.1f/%.0fs 最近同伴%.0f"),
				bShouldHeal ? TEXT("FLEE") : TEXT("RETREAT"),
				HealthPercent * 100.0f, Distance, RetreatTargetDist,
				*CoverNote, *InfluenceNote,
				Comp->RemainingHealItems,
				Data.FleeElapsed, Data.HealHopelessTimeout,
				Comp->NearestAllyDistance));

		return EStateTreeRunStatus::Running;
	}

	// 血量正常：确保逃跑提速已还原，并清掉掩体/回血状态
	Comp->ApplyFleeSpeed(false);

	// 关键：血量回到正常区间后必须清掉这些，
	// 否则下次低血量会沿用上一轮的掩体点和回血进度。
	if (Data.bHasCoverTarget || Data.HealedAmount > 0.0f)
	{
		ShowDebug(Data.bShowDebug, Pawn, FColor::Green,
			FString::Printf(TEXT("[PursueTask] 回血完成 HP=%.0f%% 已回%.0f → 恢复追击（道具剩%d）"),
				HealthPercent * 100.0f, Data.HealedAmount, Comp->RemainingHealItems));
	}
	Data.bHasCoverTarget = false;
	Data.HealTimer = 0.0f;
	Data.HealedAmount = 0.0f;

	// 本轮治疗结束 → 允许下一轮重新扣道具
	Data.bHealItemConsumed = false;

	// 「回血无望」计时归零：下一次受伤必须重新累计，不能沿用上一轮的时长
	Data.FleeElapsed = 0.0f;

	// 拼死阶段的站定标记也要清：下次进入拼死时必须重新判定距离，
	// 否则会带着"已在交火"的记忆，一进场就 StopMovement 不冲锋。
	Data.bDesperateHolding = false;

	// 移动节流记忆也要清：下次进入撤退时第一帧必须能下发移动
	Data.bHasLastMoveDest = false;

	// 掩体相关的熔断状态也要清 —— 否则下一次掉到 25% 会直接
	// 带着"已放弃掩体 / 全是黑名单"的状态进场，永远不回血。
	Data.CoverElapsed = 0.0f;
	Data.CoverFailCount = 0;
	Data.bGiveUpCover = false;
	Data.RejectedCovers.Reset();

	Data.Phase = EPursuePhase::Pursuing;

	if (bCanSee)
	{
		// ============================================================
		// 进入射程：站桩让位（Standoff）
		// ============================================================
		//
		// 旧做法是 StopMovement() 然后 return —— 站住不动。
		// 问题：6 个敌人全都停在 250cm 的同一个圆周上，间距由几何决定，
		// 分离偏移算了却被丢弃，于是扎堆。
		//
		// 新做法：把「理想站位」算出来再走过去。
		//   理想点 = 玩家位置 + 从玩家指向自己的方向 × StopDistance + 分离偏移
		// 再保持 SetFocalPoint 朝玩家 → 观感是"侧步瞄准"，像真的在抢射击角度。
		//
		// 死区（MinRepositionDistance）：偏移太小就不动，
		// 否则每帧来回微调，敌人会原地抖动。
		//
		// ★ 受伤谨慎：被打中后把接触距离拉大（默认 250 → 800），
		//   表现为「挨了一枪就往后退一点，不再无脑贴脸」。
		//   这不需要改 StateTree 资产，纯粹是这一分支内的距离判定。
		float EffectiveStopDistance = Data.StopDistance;
		const bool bWounded = Data.bUseWoundedStopDistance && Comp->IsWounded();
		if (bWounded)
		{
			EffectiveStopDistance = FMath::Max(Data.StopDistance, Data.WoundedStopDistance);
		}

		if (EffectiveStopDistance > 0.0f && Distance <= EffectiveStopDistance)
		{
			// 始终朝玩家看，这是"停下来开火"的核心观感
			AIC->SetFocalPoint(Target->GetActorLocation(), EAIFocusPriority::Gameplay);

			const FVector TargetLoc = Target->GetActorLocation();
			const FVector MyLoc = Pawn->GetActorLocation();

			// 从玩家指向自己的水平方向（长度 1）；正好重合时随便取一个方向
			FVector Outward = MyLoc - TargetLoc;
			Outward.Z = 0.0f;
			if (Outward.SizeSquared() < 1.0f)
			{
				Outward = Pawn->GetActorForwardVector() * -1.0f;
				Outward.Z = 0.0f;
			}
			Outward = Outward.GetSafeNormal();

			// 理想站位：在 EffectiveStopDistance 这个圆上，再叠加分离偏移把同伴推开
			FVector Standoff = TargetLoc + Outward * EffectiveStopDistance + SepOffset;

			// 站位必须可走，否则 MoveTo 会静默失败
			FVector ProjectedStandoff;
			if (Comp->ProjectToNavMesh(Standoff, ProjectedStandoff))
			{
				Standoff = ProjectedStandoff;
			}

			const float DistToStandoff = FVector::Dist2D(MyLoc, Standoff);

			// 死区：偏移太小就别动，避免抖动。
			// ★ 但受伤时要允许「后退到更远的站位」——那是位移很大的情况，
			//   不能因为「分离偏移很小」就把这次后退拦掉。
			const bool bNeedReposition =
				(SepOffset.Size() > 30.0f && DistToStandoff > 40.0f)
				|| DistToStandoff > 120.0f;   // 站位差得远（比如受伤后要后撤）就直接走

			if (bNeedReposition)
			{
				AIC->MoveToLocation(
					Standoff,
					30.0f,   // 接受半径给小，站得准一点
					true, true, true, false, nullptr, true
				);
			}
			else
			{
				AIC->StopMovement();
			}

			ShowDebug(Data.bShowDebug, Pawn, bWounded ? FColor::Red : FColor::Orange,
				FString::Printf(TEXT("[PursueTask] PURSUING-STANDOFF%s d=%.0f 站位%.0f HP=%.0f%% | 血量探测: %s"),
					bWounded ? TEXT("(受伤谨慎)") : TEXT(""),
					Distance, EffectiveStopDistance,
					Comp->GetHealthPercent() * 100.0f,
					*Comp->GetHealthProbeLog()));

			return EStateTreeRunStatus::Running;
		}

		// 算预判点（算法在组件里）
		const FVector Pursued = Comp->UpdatePursue(Target);

		// ------------------------------------------------------------
		// 注：这里曾经有一段「用影响图绕开威胁区」的接入代码，已撤除。
		//
		// 撤除原因（实测结论，值得记住）：
		//   影响图的威胁源是【玩家位置】这一个点，
		//   所以"绕开威胁区"在数学上就等于"远离玩家"。
		//   而敌人同时又必须"接近玩家"才能攻击 —— 两个目标直接矛盾，
		//   表现为敌人在原地磨蹭、偶尔前进一步，永远追不上人。
		//
		//   更本质的问题是语义错位：
		//     「躲子弹」需要的是【弹道线】信息（子弹从哪飞到哪），
		//     而影响图是一个【慢速更新的静态场】，它回答的是
		//     "哪片区域整体更危险"，不适合做"这一步往哪躲"。
		//   要用影响图，正确的用法是选掩体 / 选集结位置 / 判断哪条路安全，
		//   而不是改写追击的目标点。
		//
		//   影响图本身（热力图、三个查询 API）仍然保留，
		//   它作为独立的战术信息层是有价值的，只是不该这样接进追击。
		// ------------------------------------------------------------
		const FVector Goal = Pursued;

		// 叠加群体分离（SepOffset 已在上方算好）
		FVector Destination = Goal + SepOffset;

		// 目标点必须可走，否则 MoveTo 会静默失败
		FVector Projected;
		Destination = Comp->ProjectToNavMesh(Destination, Projected) ? Projected : Goal;

		// ★ 追击时面朝玩家（自然的战斗侧步）
		//
		//   为什么现在可以放心设焦点（早期不敢设，怕倒退走）：
		//   预判偏移被限制在「距离 × 0.2」以内（见 PredictedOffsetRatio），
		//   所以移动方向与「指向玩家」方向的夹角恒 ≤ atan(0.2) ≈ 11°。
		//   数学上保证：面朝玩家 + 侧移 = 正常的战术走位，不可能倒退。
		//
		//   这也更符合射击游戏观感：真实 FPS 敌人是一边侧移一边瞄准你，
		//   而不是背对你跑。
		AIC->SetFocalPoint(Target->GetActorLocation(), EAIFocusPriority::Gameplay);

		AIC->MoveToLocation(
			Destination,
			Data.AcceptanceRadius,
			true,    // bStopOnOverlap
			true,    // bUsePathfinding
			true,    // bProjectDestinationToNavigation
			false,   // bCanStrafe
			nullptr,
			true     // bAllowPartialPath
		);

		ShowDebug(Data.bShowDebug, Pawn, FColor::Green,
			FString::Printf(TEXT("[PursueTask] PURSUING d=%.0f pred=(%.0f,%.0f) HP=%.0f%% 最近同伴%.0f | 血量探测: %s"),
				Distance, Pursued.X, Pursued.Y,
				Comp->GetHealthPercent() * 100.0f,
				Comp->NearestAllyDistance,
				*Comp->GetHealthProbeLog()));

		return EStateTreeRunStatus::Running;
	}

	// ============================================================
	// 从这里开始都是「看不见」
	// ============================================================

	Data.TimeSinceLastSeen += DeltaTime;

	// 关掉记忆 = 模板原始行为：一丢就走
	if (!Data.bUseMemory)
	{
		ShowDebug(Data.bShowDebug, Pawn, FColor::Silver,
			FString::Printf(TEXT("[PursueTask] LOST（未启用记忆）失联%.1fs"), Data.TimeSinceLastSeen));
		return EStateTreeRunStatus::Running;
	}

	// 记忆过期 → 彻底放弃。
	// 返回 Failed 让 StateTree 的 On State Failed 转换接管（回漫游 / 去调查）。
	// 用 Failed 而不是 Succeeded：语义是"这个任务没能完成"，不是"成功做完"。
	if (!Data.bHasMemory || Data.TimeSinceLastSeen > Data.MemoryDuration)
	{
		ShowDebug(Data.bShowDebug, Pawn, FColor::Silver,
			FString::Printf(TEXT("[PursueTask] GIVE-UP 失联%.1fs > %.1fs"), Data.TimeSinceLastSeen, Data.MemoryDuration));

		UE_LOG(LogTemp, Warning,
			TEXT("[PursueTask] 放弃追踪：失联 %.1fs 超过记忆上限 %.1fs（Pawn=%s）"),
			Data.TimeSinceLastSeen, Data.MemoryDuration, *GetNameSafe(Pawn));

		// 清空记忆，避免下次进入状态立刻用旧位置
		Data.bHasMemory = false;
		Data.Phase = EPursuePhase::Chasing;

		return EStateTreeRunStatus::Failed;
	}

	// ============================================================
	// 档位 ②：有记忆，赶去最后已知位置
	// ============================================================

	// SepOffset 已在上方算好（同时刷新了 NearestAllyDistance）。
	// "到没到"的判定要把偏移量算进去，
	// 否则被同伴推着永远进不了 SearchAcceptanceRadius，就永远不开始扫描。
	const float DistToMemory = FVector::Dist2D(Pawn->GetActorLocation(), Data.LastKnownLocation);

	// 两个独立的"该不该继续追"判据：
	//   bReached      —— 已经到记忆点附近（正常到达）
	//   bChaseTimedOut —— 追了很久还是到不了（记忆点不可达，比如柱子另一侧）
	//
	// 为什么需要 bChaseTimedOut：
	//   记忆点可能落在不可走的位置，MoveToLocation 每帧失败但不报错，
	//   敌人停在原地、2D 距离却一直大于 SearchAcceptanceRadius，
	//   于是永远进不了 SCANNING，只能等 MemoryDuration 到期被 GIVE-UP。
	//   加超时后变成「追不到就就地搜一轮」，这是真实游戏里的常见做法。
	const bool bReached = DistToMemory <= Data.SearchAcceptanceRadius + SepOffset.Size();
	const bool bChaseTimedOut = Data.ChaseElapsed >= Data.ChaseTimeout;

	if (!bReached && !bChaseTimedOut)
	{
		Data.Phase = EPursuePhase::Chasing;
		Data.ChaseElapsed += DeltaTime;
		Data.ScanElapsed = 0.0f;

		// 记忆点可能不可走（玩家贴墙/窗外跑时），投影一下
		FVector Target2D = Data.LastKnownLocation;
		FVector ProjectedMemory;
		if (Comp->ProjectToNavMesh(Data.LastKnownLocation, ProjectedMemory))
		{
			Target2D = ProjectedMemory;
		}

		FVector Dest = Target2D + SepOffset;
		FVector ProjectedDest;
		Dest = Comp->ProjectToNavMesh(Dest, ProjectedDest) ? ProjectedDest : Target2D;

		AIC->MoveToLocation(
			Dest,
			Data.SearchAcceptanceRadius,
			true, true, true, false, nullptr, true
		);

		ShowDebug(Data.bShowDebug, Pawn, FColor::Cyan,
			FString::Printf(TEXT("[PursueTask] CHASING d=%.0f 追%.1f/%.1fs（失联%.1fs）"),
				DistToMemory, Data.ChaseElapsed, Data.ChaseTimeout, Data.TimeSinceLastSeen));

		return EStateTreeRunStatus::Running;
	}

	// 从这里往下都是「该扫描了」，但要区分是到达还是超时（日志里说清）
	if (Data.ChaseElapsed >= Data.ChaseTimeout && !bReached)
	{
		// 只在第一次进入扫描时打一条，避免刷屏
		if (Data.Phase != EPursuePhase::Scanning)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[PursueTask] 追赶超时：追了 %.1fs 仍未到达记忆点（还差 %.0fcm）→ 就地搜索（Pawn=%s）"),
				Data.ChaseElapsed, DistToMemory, *GetNameSafe(Pawn));
		}
	}

	// ============================================================
	// 档位 ③：到了记忆点，原地环视
	// ============================================================

	Data.Phase = EPursuePhase::Scanning;
	Data.ScanElapsed += DeltaTime;

	AIC->StopMovement();

	// 环视：绕 Y 轴匀速转。
	// 为什么用转向而不是随机移动：
	//   ① 转圈是明确的"我在找"视觉信号，玩家一眼看懂
	//   ② 随机走会被误认成"又在巡逻"
	//   ③ 不触发寻路，零额外开销
	const float ScanYaw = Pawn->GetActorRotation().Yaw + Data.ScanElapsed * Data.ScanYawSpeed;
	const FVector LookAt = Pawn->GetActorLocation()
		+ FRotator(0.0f, ScanYaw, 0.0f).Vector() * 500.0f;
	AIC->SetFocalPoint(LookAt, EAIFocusPriority::Gameplay);

	ShowDebug(Data.bShowDebug, Pawn, FColor::Magenta,
		FString::Printf(TEXT("[PursueTask] SCANNING %.1f/%.1fs 原地搜寻"), Data.ScanElapsed, Data.ScanDuration));

	// 搜够了 → 放弃，交给状态机
	if (Data.ScanElapsed >= Data.ScanDuration)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[PursueTask] 搜索结束：原地环视 %.1fs 未找到目标（Pawn=%s）"),
			Data.ScanElapsed, *GetNameSafe(Pawn));

		Data.bHasMemory = false;

		return EStateTreeRunStatus::Failed;
	}

	return EStateTreeRunStatus::Running;
}

void FStateTreePursueTask::ExitState(FStateTreeExecutionContext& Context,
	const FStateTreeTransitionResult& Transition) const
{
	const FInstanceDataType& Data = Context.GetInstanceData(*this);

	// 离开时停掉移动，避免带着惯性冲进下一个状态
	if (AAIController* AIC = ResolveAIController(Context))
	{
		AIC->StopMovement();
	}

	// ★ 必须解除停火。
	//   bSuppressFire 存在组件上（Actor 级，跨状态存活）。
	//   若在 FLEE / HEALING 期间退出本状态而不清理，下一个状态（比如 Attack）
	//   会永远收到"禁止开火" —— 表现为敌人追着你跑却一枪不放，
	//   而且没有任何报错，极难排查。
	if (UPursueSteeringComponent* Comp = ResolveComponent(Context))
	{
		Comp->SetFireSuppressed(false, FString());
	}

	// 诊断：CurrentRunStatus 会告诉我们"为什么会退出"
	//   Failed    → 记忆过期 或 搜索超时（我们的主动放弃）
	//   Succeeded → 某个 Task 完成了
	//   Running   → 被外部转换打断（转换条件触发）
	UE_LOG(LogTemp, Warning,
		TEXT("[PursueTask] ExitState  Pawn=%s  Tick=%d  阶段=%s  退出状态=%s"),
		*GetNameSafe(ResolvePawn(Context)),
		Data.TickCount,
		PhaseToString(Data.Phase),
		*RunStatusToString(Transition.CurrentRunStatus));
}

#if WITH_GAMEPLAY_DEBUGGER
FString FStateTreePursueTask::GetDebugInfo(const FStateTreeReadOnlyExecutionContext& Context) const
{
	// FStateTreeReadOnlyExecutionContext 没有 GetInstanceData，
	// 所以这里只能给静态描述；阶段信息看屏幕字或 Output Log。
	return TEXT("Pursue: 追预判点 / 记忆搜索 / 分离");
}
#endif
