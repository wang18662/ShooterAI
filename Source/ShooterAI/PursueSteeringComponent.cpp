// ============================================================
// PursueSteeringComponent.cpp
// ============================================================
//
// 【这个文件做什么】
//   敌人 AI 的"移动与感知"组件。本文件只放**核心**部分：
//     · 组件生命周期    构造函数（TickGroup 设置）、BeginPlay（事件订阅、道具初始化）
//     · 追击算法        GetPursueTargetLocation / UpdatePursue —— 追"你将要在的位置"
//     · 群体分离        ComputeSeparationOffset —— 多个敌人不要叠在一起
//     · 导航投影        ProjectToNavMesh —— 目标点必须落在可行走面上
//     · 引擎桥接        ResolveAIController / CanSeeTarget / MoveTo / FaceLocation
//     · TickComponent   每帧入口（受伤计时 + 旧旁路的开关判断）
//     · 跨 Task 信号    bSuppressFire —— 行为层告诉射击层"本帧别开火"
//
// 【这个组件被拆成了 4 个 .cpp，但**类定义不变**】
//   这是刻意选择：C++ 允许同一个类的成员函数分散在多个 .cpp 里。
//   所以拆分"减小了单文件体积、分离了职责"，但：
//     · 蓝图不用改（还是同一个组件）
//     · 52 处外部调用点不用改（接口一个没动）
//     · 行为完全不变
//   如果把职责拆成 4 个独立组件，就必须在蓝图里手工加组件，
//   漏掉的后果是静默失效（不报错、只是某功能不工作）—— 风险不成比例。
//
// 【文件导航】
//   本文件          核心：追击算法 / 分离 / 投影 / 引擎桥接 / Tick
//   _Health.cpp     血量读取 / 档位 / 回血道具 / 受伤谨慎 / 逃跑提速
//   _Cover.cpp      掩体射线探测（安全 = 看不到，不是 = 距离远）
//   _Legacy.cpp     旧 AutoPilot 旁路 + 3 个未接线算法（当前不参与执行）
//
// 【本文件最该记住的一条设计】
//   ★ 预判点算出来可能落在墙里。算完必须过 ProjectToNavMesh，
//     不可达就退回目标当前位置 —— 否则 MoveTo 会**静默失败**（不报错，只是不动）。
//
// ============================================================

#include "PursueSteeringComponent.h"
#include "ShooterAIDebug.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "AIController.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"
#include "NavigationSystem.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "CollisionQueryParams.h"

UPursueSteeringComponent::UPursueSteeringComponent()
{
	// 手动模式不 Tick；只有 bAutoPilot 打开时才需要每帧跑（见 TickComponent 里的提前返回）
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// 关键：放到 tick 的最后一组（TG_PostUpdateWork）。
	// UE 的移动指令是「后下达者胜」——StateTree 每帧在 PrePhysics 下发自己的 MoveTo/StopMovement，
	// 如果我们也排在 PrePhysics，谁先谁后不确定，指令常常被覆盖，
	// 表现就是「敌人按状态树逻辑走，预判完全没用」。
	// 排到最后一个 tick 组，保证我们的 MoveTo 是本帧最后一条指令。
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UPursueSteeringComponent::BeginPlay()
{
	Super::BeginPlay();

	// 保险：即使蓝图里 tick 开关被关掉，也强制开启（TickComponent 内部会按 bAutoPilot 提前返回）
	SetComponentTickEnabled(true);

	// 道具在角色出生时补满。
	// 注意：这里用 "=" 而不是 "+="，因为 BeginPlay 可能因为重生成而多次进入，
	// 而道具应该是每局独立计算，不是累加。
	RemainingHealItems = HealItemCount;

	// 订阅受伤事件。
	// 用 AActor 自带的 OnTakeAnyDamage：不需要 Health 变量、不需要改蓝图，
	// 只要这个 Actor 被 ApplyDamage 命中就会触发。
	if (AActor* OwnerActor = GetOwner())
	{
		OwnerActor->OnTakeAnyDamage.AddDynamic(this, &UPursueSteeringComponent::HandleAnyDamage);
	}

	UE_LOG(LogTemp, Warning, TEXT("[Pursue] %s BeginPlay: AutoPilot=%s  Memory=%s  WoundedCaution=%s"),
		*GetNameSafe(GetOwner()),
		bAutoPilot ? TEXT("ON") : TEXT("OFF"),
		bUseMemory ? TEXT("ON") : TEXT("OFF"),
		bUseWoundedCaution ? TEXT("ON") : TEXT("OFF"));
}

// ============================================================
// 算法核心：与源库 Pursue::getSteering 逐行对应
// ============================================================

FVector UPursueSteeringComponent::GetPursueTargetLocation(
	const FVector& TargetPosition, const FVector& TargetVelocity,
	const FVector& CharacterPosition, const FVector& CharacterVelocity) const
{
	// Vector3 direction = target.position - character.position;
	const FVector Direction = TargetPosition - CharacterPosition;
	const float Distance = Direction.Size();
	const float Speed = CharacterVelocity.Size();

	// float prediction = (speed <= distance / maxPrediction) ? maxPrediction : distance / speed;
	const float Prediction = (Speed <= Distance / MaxPrediction) ? MaxPrediction : Distance / Speed;

	// Seek::target.position += target.velocity * prediction;  →  直接返回预测点
	FVector Predicted = TargetPosition + TargetVelocity * Prediction;

	// ------------------------------------------------------------
	// ★ 第一道约束：偏移幅度不超过「当前距离 × PredictedOffsetRatio」
	//
	//   纯源库公式的偏移是绝对值，不随距离缩放。
	//   玩家横移 600cm/s、MaxPrediction=1s 时偏移可达 600cm：
	//     距离 1500cm → 偏角 22°
	//     距离  600cm → 偏角 45°   ← 看起来像往侧面跑，不像在追人
	//
	//   按比例限制后偏角上限 = atan(Ratio)，与距离无关：
	//     Ratio = 0.2 → 偏角恒定不超过 ≈11°
	//   近身自然更精准，远处仍保留足够预判。
	// ------------------------------------------------------------
	{
		const FVector Offset = Predicted - TargetPosition;
		const float OffsetLen = Offset.Size();

		if (OffsetLen > KINDA_SMALL_NUMBER && PredictedOffsetRatio > 0.0f)
		{
			const float MaxOffset = Distance * PredictedOffsetRatio;
			if (OffsetLen > MaxOffset)
			{
				Predicted = TargetPosition + Offset / OffsetLen * MaxOffset;
			}
		}
	}

	// ------------------------------------------------------------
	// ★ 第二道约束：预判点不许越过 AI 自身（防「倒退追人」）
	//
	//   纯源库公式在「玩家正朝 AI 冲」时会算出 AI 身后的点：
	//     AI(0,0)、玩家(300,0)、玩家速度(-600,0)、AI 速度 400
	//     Prediction = 300/400 = 0.75s
	//     Predicted = (300,0) + (-600,0)*0.75 = (-150,0)   ← AI 背后 150cm
	//   AI 会往后退着够这个点，表现为「面朝玩家、身体倒着走」。
	//
	//   修法：把预判点投影到「玩家→AI」这条射线上，
	//   最多推进到「距 AI 还剩 SafetyMargin」的位置。
	//   横向跑（玩家速度垂直于连线）时 AlongToSelf ≈ 0，不会被截断。
	// ------------------------------------------------------------
	if (Distance > KINDA_SMALL_NUMBER && PredictedSafetyMargin > 0.0f)
	{
		// 从玩家指向 AI 的单位方向
		const FVector ToSelf = (CharacterPosition - TargetPosition) / Distance;

		// 预判点在这条射线上推进了多远（负数 = 朝远离 AI 的方向，无需截断）
		const float AlongToSelf = FVector::DotProduct(Predicted - TargetPosition, ToSelf);

		// 允许的最大推进距离：到 AI 跟前还剩 SafetyMargin 就停
		const float MaxAdvance = FMath::Max(0.0f, Distance - PredictedSafetyMargin);

		if (AlongToSelf > MaxAdvance)
		{
			Predicted = TargetPosition + ToSelf * MaxAdvance;
		}
	}

	return Predicted;
}

float UPursueSteeringComponent::GetPredictionTime(
	const FVector& TargetPosition, const FVector& TargetVelocity,
	const FVector& CharacterPosition, const FVector& CharacterVelocity) const
{
	const float Distance = (TargetPosition - CharacterPosition).Size();
	const float Speed = CharacterVelocity.Size();
	return (Speed <= Distance / MaxPrediction) ? MaxPrediction : Distance / Speed;
}

bool UPursueSteeringComponent::ProjectToNavMesh(const FVector& Location, FVector& OutProjectedLocation) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return false;
	}

	const FVector QueryExtent(MaxNavProjectionDistance, MaxNavProjectionDistance, MaxNavProjectionDistance * 2.0f);

	FNavLocation NavLocation;
	if (!NavSys->ProjectPointToNavigation(Location, NavLocation, QueryExtent))
	{
		return false;
	}

	if (FVector::Dist2D(NavLocation.Location, Location) > MaxNavProjectionDistance)
	{
		return false;
	}

	OutProjectedLocation = NavLocation.Location;
	return true;
}

FVector UPursueSteeringComponent::UpdatePursue(AActor* Target)
{
	const AActor* Owner = GetOwner();
	const FVector CharacterPosition = Owner ? Owner->GetActorLocation() : FVector::ZeroVector;

	if (!Target)
	{
		CurrentPredictedLocation = CharacterPosition;
		bLastPredictionValid = false;
		return CurrentPredictedLocation;
	}

	const FVector TargetPosition = Target->GetActorLocation();
	const FVector TargetVelocity = Target->GetVelocity();
	const FVector CharacterVelocity = Owner ? Owner->GetVelocity() : FVector::ZeroVector;

	const FVector Predicted = GetPursueTargetLocation(
		TargetPosition, TargetVelocity, CharacterPosition, CharacterVelocity);

	// 唯一的一条判断：预测点能不能走到？走不到就直接锁人物当前位置。
	FVector Projected;
	if (ProjectToNavMesh(Predicted, Projected))
	{
		CurrentPredictedLocation = Projected;
		bLastPredictionValid = true;
	}
	else
	{
		CurrentPredictedLocation = TargetPosition;
		bLastPredictionValid = false;
	}

	if (bDrawDebug && GetWorld())
	{
		const FColor DebugColor = bLastPredictionValid ? FColor::Red : FColor::Yellow;
		DrawDebugSphere(GetWorld(), CurrentPredictedLocation, 25.0f, 12, DebugColor, false, 0.1f);
	}

	return CurrentPredictedLocation;
}

FVector UPursueSteeringComponent::GetPursueSteering(
	const FVector& TargetPosition, const FVector& TargetVelocity,
	const FVector& CharacterPosition, const FVector& CharacterVelocity,
	float MaxAcceleration) const
{
	const FVector Predicted = GetPursueTargetLocation(TargetPosition, TargetVelocity, CharacterPosition, CharacterVelocity);
	// 对应源库 Seek: result.linear.normalize(); result.linear *= maxAcceleration;
	return (Predicted - CharacterPosition).GetSafeNormal() * MaxAcceleration;
}

// ============================================================
// AutoPilot：三档行为
// ============================================================

AAIController* UPursueSteeringComponent::ResolveAIController()
{
	if (!CachedAIController)
	{
		if (APawn* OwnerPawn = Cast<APawn>(GetOwner()))
		{
			CachedAIController = Cast<AAIController>(OwnerPawn->GetController());
		}
	}
	return CachedAIController;
}

bool UPursueSteeringComponent::CanSeeTarget(AActor* Target) const
{
	const AAIController* AIC = CachedAIController;
	if (!AIC || !Target)
	{
		return false;
	}

	// 主判断：控制器的视线检测（会考虑控制器的 FocalPoint 与朝向）
	if (AIC->LineOfSightTo(Target))
	{
		return true;
	}

	// 兜底：眼睛到眼睛的射线。控制器 FocalPoint 没设好时 LineOfSightTo 会误判为看不见。
	const UWorld* World = GetWorld();
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!World || !OwnerPawn)
	{
		return false;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(PursueAutoPilotLOS), false, GetOwner());
	Params.AddIgnoredActor(Target);

	FHitResult Hit;
	const FVector Eye = OwnerPawn->GetPawnViewLocation();
	const FVector TargetEye = Target->GetActorLocation() + FVector(0.0f, 0.0f, 50.0f);
	return !World->LineTraceSingleByChannel(Hit, Eye, TargetEye, ECC_Visibility, Params);
}

void UPursueSteeringComponent::FaceLocation(const FVector& Location)
{
	if (AAIController* AIC = ResolveAIController())
	{
		AIC->SetFocalPoint(Location, EAIFocusPriority::Gameplay);
	}
}

void UPursueSteeringComponent::MoveTo(const FVector& Destination, float AcceptanceRadius)
{
	if (AAIController* AIC = ResolveAIController())
	{
		AIC->MoveToLocation(
			Destination,
			AcceptanceRadius,
			true,   // bStopOnOverlap
			true,   // bUsePathfinding
			true,   // bProjectDestinationToNavigation
			false,  // bCanStrafe
			nullptr,
			true    // bAllowPartialPath
		);
	}
}

// ============================================================
// 群体分离：移植自源库 ch03_movement/Separation.h
//
// 源库逻辑：
//   for each neighbor:
//       direction = character.position - neighbor.position   // 远离邻居
//       distance  = direction.length()
//       if (distance < threshold):
//           strength = decayCoefficient * (threshold - distance)
//           result.linear += direction.normalize() * strength
//
// 胶水层的改动：源库输出「加速度」，UE 这边因为走 NavMesh，
// 把合力当作「移动目标点的偏移量」——语义从"推着走"变成"绕着追"，
// 群体分散的效果一致，但不破坏寻路。
// ============================================================

FVector UPursueSteeringComponent::ComputeSeparationOffset()
{
	NearestAllyDistance = -1.0f;

	// 注意：这里不再因为 bUseSeparation == false 就提前返回。
	// 原因：NearestAllyDistance 是「观测值」，必须在关闭分离时也能测到真实距离，
	// 否则 A/B 对比里"关掉"那组永远读到 -1，看起来像分离没生效，
	// 实际是压根没测。开关只影响「要不要施加推力」，不影响「要不要测量」。
	const bool bShouldApply = bUseSeparation;

	if (SeparationRadius <= 0.0f || !GetWorld())
	{
		return FVector::ZeroVector;
	}

	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn)
	{
		return FVector::ZeroVector;
	}

	const FVector MyPos = OwnerPawn->GetActorLocation();

	// 源库的 result.linear
	FVector Accumulated = FVector::ZeroVector;
	float Closest = -1.0f;

	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		const APawn* Other = *It;
		if (!Other || Other == OwnerPawn)
		{
			continue;
		}

		// 只对「AI 控制的同伴」生效：玩家不该被自己推开，也不需要推开别人
		if (!Cast<AAIController>(Other->GetController()))
		{
			continue;
		}

		// Vector3 direction = character.position - target->position;
		FVector Direction = MyPos - Other->GetActorLocation();
		Direction.Z = 0.0f;   // 只在水平面分离，避免上下坡时互相往上推

		const float Distance = Direction.Size();

		// ★ 先记录「最近同伴」，再判断是否施加推力。
		//
		//   原来的顺序是先 `if (Distance >= SeparationRadius) continue;`
		//   再记 Closest —— 导致所有同伴都在 220cm 外时 NearestAllyDistance
		//   恒为 -1。可这个字段名叫「最近同伴」，用户读它就是想看真实间距，
		//   不是"分离半径内的最近同伴"。
		//   语义修正：只要找到人，就记真实距离；是否施加推力另行判断。
		if (Distance > 1.0f && (Closest < 0.0f || Distance < Closest))
		{
			Closest = Distance;
		}

		if (Distance < 1.0f || Distance >= SeparationRadius)
		{
			continue;   //  仅在 distance < threshold 时才产生推力
		}

		// float strength = decayCoefficient * (threshold - distance);
		const float Strength = SeparationStrength * (SeparationRadius - Distance);

		// result.linear += direction.normalize() * strength;
		Accumulated += (Direction / Distance) * Strength;
	}

	NearestAllyDistance = Closest;

	// 关掉分离时：测量照做（上面的循环已经跑了），但不施加推力。
	if (!bShouldApply)
	{
		return FVector::ZeroVector;
	}

	// 钳制最大偏移：绝对不让分离把目标点推到墙外或甩太远
	return Accumulated.GetClampedToMaxSize(MaxSeparationOffset);
}

void UPursueSteeringComponent::ClearMemory()
{
	bHasMemory = false;
	LastKnownLocation = FVector::ZeroVector;
	TimeSinceLastSeen = 0.0f;
	ScanElapsed = 0.0f;
	CurrentMode = EPursueMode::Idle;
}

void UPursueSteeringComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// ★ 受伤计时：必须放在所有提前返回之前。
	//   Task 模式下 bAutoPilot = false，本函数后面会 return，
	//   如果把计时放到后面，谨慎期永远不会结束——这是个很容易踩的坑。
	TimeSinceDamaged += DeltaTime;

	// 注：这里曾经有两处常驻提示，都已删除：
	//   ①「AutoPilot=OFF（已交给状态机，正常）」—— Task 模式下这是正常设计，
	//      15 个敌人各打一行只是噪音；答案已在类注释里写清。
	//   ② Key 声明 —— 随 ① 一起删掉（下面的模式行用自己的 Key）。

	if (!bAutoPilot)
	{
		return;
	}

	// 降频：0.1s 一次足够
	AutoPilotRetryTimer -= DeltaTime;
	if (AutoPilotRetryTimer > 0.0f)
	{
		return;
	}
	AutoPilotRetryTimer = AutoPilotRetryInterval;

	APawn* Target = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!Target)
	{
		AutoPilotState = TEXT("no player pawn");
		CurrentMode = EPursueMode::Idle;
	}
	else
	{
		AutoPilotTick(Target);
	}

	// 屏幕诊断（每个单位一行，只在详细级别打印）
	//
	// ★ 只在"真的在做事"时打印。
	//
	//   实测：15 个敌人里有 12 个处于 Idle（没目标、在漫游），
	//   它们每帧都打印 `IDLE 交还状态树 | 失联11.0s`，
	//   把真正要看的两三行（正在追击/交战的敌人）挤到看不见。
	//
	//   Idle 的语义就是"没事可做"——报告"我没事做"本身没有信息量。
	//   静默才是它该有的表现，需要看"谁在动"时，出现的行就是答案。
	if (bShowDiagnostics && GEngine && GetOwner() && ShooterAIDebug::ShouldPrintPerActor()
		&& CurrentMode != EPursueMode::Idle)
	{
		FColor Color = FColor::Silver;
		switch (CurrentMode)
		{
		case EPursueMode::Pursuing: Color = FColor::Green;  break;
		case EPursueMode::Chasing:  Color = FColor::Cyan;   break;
		case EPursueMode::Scanning: Color = FColor::Magenta; break;
		default:                    Color = FColor::Yellow; break;
		}

		const uint64 Key = static_cast<uint64>(reinterpret_cast<UPTRINT>(GetOwner()));
		const FString Msg = FString::Printf(TEXT("[PursueComp] %s | 失联%.1fs | 最近同伴%.0f"),
			*AutoPilotState, TimeSinceLastSeen, NearestAllyDistance);
		GEngine->AddOnScreenDebugMessage(Key, 0.2f, Color, Msg);
	}
}
