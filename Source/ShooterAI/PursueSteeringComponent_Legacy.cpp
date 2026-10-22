// ============================================================
// PursueSteeringComponent_Legacy.cpp
// ============================================================
//
// ⚠️ 【重要】本文件里的代码**当前都不参与执行**。保留是为了"可查 + 可回退"，
//    不是遗留垃圾。删之前请先读下面的说明。
//
// 【这个文件做什么】
//   两类东西：
//   ── A. AutoPilot 旧旁路（本项目第一阶段的架构）──────────────
//      在 StateTree Task 出现之前，由组件自己接管移动（自己找玩家、
//      自己算落点、自己 MoveTo），靠 TickGroup 抢在状态机之后下指令。
//      · AutoPilotTick  整套旁路逻辑
//      **什么时候会跑**：只有 bAutoPilot = true 时。
//      而 StateTreePursueTask::EnterState 里有一句强制 `Comp->bAutoPilot = false;`
//      —— 所以 Task 模式下它永远不跑。
//      **为什么保留**：它是"算法能不能单独跑通"的隔离环境。当怀疑 Task
//      接线有问题时，把 bAutoPilot 打开就能判断"是算法错了还是接线错了"。
//      重构完成后，旧路径变成了调试工具。
//
//   ── B. 三个未接线的 steering 算法 ────────────────────────────
//      ComputeArrive / ComputeCollisionAvoidance / ComputeWander
//      三个都完整实现、都编译通过，但**全项目零调用**。
//      **为什么主动放弃接线**（这是工程判断，不是遗漏）：
//        · Arrive：UE 的寻路组件已内置接近减速，接了是重复，视觉上看不出差别
//        · CollisionAvoidance：敌人不互相穿过是物理碰撞 + 导航半径在起作用，
//          观众看不出区别
//        · Wander：漫游轨迹平滑与否在 demo 里几乎不可辨，且要新写一个 Task，
//          成本最高收益最低
//      判断依据：demo 只有 1 分钟，观众能记住的是"热力图""CPU 数字"这类
//      强视觉信号。面试口径说"5 个 steering 算法移植、2 个接入执行路径"
//      是诚实的；说"5 个都生效"会被追问穿。
//
// 【不在这里的东西】
//   · 血量 / 回血 / 受伤   → PursueSteeringComponent_Health.cpp
//   · 掩体探测             → PursueSteeringComponent_Cover.cpp
//   · 追击、分离、投影     → PursueSteeringComponent.cpp
//
// 【如果要重新启用某个算法】
//   1. 确认它的输入（当前位置、目标位置）能从 Task 里拿到
//   2. 在 StateTreePursueTask.cpp 对应分支里调用，把返回值叠加到 MoveTo 的目标点
//   3. 记得它输出的是"目标点偏移"不是"速度" —— 这是为适配 NavMesh 刻意改的语义
//
// ============================================================

#include "PursueSteeringComponent.h"

#include "AIController.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationSystem.h"
#include "DrawDebugHelpers.h"

// ============================================================
// ⑧ 旧旁路：AutoPilotTick
// ============================================================
// Task 模式下 bAutoPilot = false，本函数不会被调用。
// 保留用途见文件头 A 段：它是"算法能否单独跑通"的隔离调试环境。

bool UPursueSteeringComponent::AutoPilotTick(AActor* Target)
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn || !Target)
	{
		AutoPilotState = TEXT("no pawn/target");
		CurrentMode = EPursueMode::Idle;
		return false;
	}

	AAIController* AIC = ResolveAIController();
	if (!AIC)
	{
		AutoPilotState = FString::Printf(TEXT("no AIController (ctrl=%s)"),
			OwnerPawn->GetController() ? *OwnerPawn->GetController()->GetClass()->GetName() : TEXT("None"));
		CurrentMode = EPursueMode::Idle;
		return false;
	}

	// 出生点纠偏：Spawner 生成的敌人可能站在 NavMesh 空洞里，
	// 此时 MoveTo 从第一步就失败，表现是「站着不动、只会射击」。
	if (!bSnappedToNavMesh && bSnapToNavMeshOnStart)
	{
		const FVector Here = OwnerPawn->GetActorLocation();
		const FVector SearchExtent(300.0f, 300.0f, 400.0f);

		if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
		{
			FNavLocation NavLocation;
			if (NavSys->ProjectPointToNavigation(Here, NavLocation, SearchExtent))
			{
				bSnappedToNavMesh = true;

				if (FVector::Dist2D(NavLocation.Location, Here) > 5.0f)
				{
					FVector NewLoc = NavLocation.Location;
					NewLoc.Z = Here.Z + 5.0f;
					OwnerPawn->SetActorLocation(NewLoc, false, nullptr, ETeleportType::TeleportPhysics);
				}
			}
		}
	}

	const bool bCanSee = bCheckLineOfSight ? CanSeeTarget(Target) : true;
	const float Distance = FVector::Dist(OwnerPawn->GetActorLocation(), Target->GetActorLocation());

	// ---------- 档位 ①：看得见 → 追预判点 ----------
	if (bCanSee)
	{
		// 看见就刷新记忆
		LastKnownLocation = Target->GetActorLocation();
		bHasMemory = true;
		TimeSinceLastSeen = 0.0f;
		ScanElapsed = 0.0f;
		CurrentMode = EPursueMode::Pursuing;

		const FVector Pursued = UpdatePursue(Target);

		if (StopDistance > 0.0f && Distance <= StopDistance)
		{
			AIC->StopMovement();
			FaceLocation(Target->GetActorLocation());
			AutoPilotState = FString::Printf(TEXT("PURSUE-STOP d=%.0f 停下开火"), Distance);
		}
		else
		{
			// 目标点 = 预判点 + 分离偏移（多敌不挤成一坨）
			FVector Destination = Pursued + ComputeSeparationOffset();

			// 偏移后仍要过一遍可达性：墙面/地图外则退回原始预判点
			FVector Projected;
			if (ProjectToNavMesh(Destination, Projected))
			{
				Destination = Projected;
			}
			else
			{
				Destination = Pursued;
			}

			MoveTo(Destination, AutoPilotAcceptanceRadius);

			AutoPilotState = bUseSeparation
				? FString::Printf(TEXT("PURSUE d=%.0f 分离推力(最近%.0f)"), Distance, NearestAllyDistance)
				: FString::Printf(TEXT("PURSUE d=%.0f -> 预判点"), Distance);
		}

		if (bDrawDebug && GetWorld())
		{
			DrawDebugLine(GetWorld(), LastKnownLocation, LastKnownLocation + FVector(0, 0, 200), FColor::Green, false, 0.15f, 0, 2.0f);
		}
		return true;
	}

	// ---------- 档位 ②：看不见但有记忆 → 去最后已知位置搜 ----------
	TimeSinceLastSeen += AutoPilotRetryInterval;

	const bool bMemoryAlive = bUseMemory && bHasMemory && TimeSinceLastSeen <= MemoryDuration;

	if (bMemoryAlive)
	{
		// 先算分离偏移：它会把目标点推远，所以「到没到」的判定要把偏移量算进去，
		// 否则敌人被同伴推着，永远进不了 SearchAcceptanceRadius，就永远不会进入搜索阶段。
		const FVector SepOffset = ComputeSeparationOffset();
		const float DistToMemory = FVector::Dist2D(OwnerPawn->GetActorLocation(), LastKnownLocation);

		if (DistToMemory > SearchAcceptanceRadius + SepOffset.Size())
		{
			// 还在赶过去的路上
			CurrentMode = EPursueMode::Chasing;
			ScanElapsed = 0.0f;

			// 记忆点可能不可走（玩家贴在墙后/窗外），投影一下再走
			FVector Target2D = LastKnownLocation;
			FVector Projected;
			if (ProjectToNavMesh(LastKnownLocation, Projected))
			{
				Target2D = Projected;
			}

			// 多敌搜同一点也要分散，否则会叠在记忆点上
			FVector SearchDest = Target2D + SepOffset;
			FVector ProjectedSearch;
			SearchDest = ProjectToNavMesh(SearchDest, ProjectedSearch) ? ProjectedSearch : Target2D;

			MoveTo(SearchDest, SearchAcceptanceRadius);
			AutoPilotState = FString::Printf(TEXT("CHASE-MEMORY d=%.0f 去最后已知位置"), DistToMemory);
			if (bDrawDebug && GetWorld())
			{
				DrawDebugSphere(GetWorld(), LastKnownLocation, 40.0f, 12, FColor::Green, false, 0.15f);
			}
		}
		else
		{
			// 到了 → 原地环视
			CurrentMode = EPursueMode::Scanning;
			ScanElapsed += AutoPilotRetryInterval;

			AIC->StopMovement();

			// 环视：绕 Y 轴匀速转，制造「在看四周」的感觉
			const float ScanYaw = OwnerPawn->GetActorRotation().Yaw + ScanElapsed * 120.0f;
			const FVector LookAt = OwnerPawn->GetActorLocation()
				+ FRotator(0.0f, ScanYaw, 0.0f).Vector() * 500.0f;
			FaceLocation(LookAt);

			AutoPilotState = FString::Printf(TEXT("SCAN %.1f/%.1fs 原地搜寻"), ScanElapsed, ScanDuration);

			// 搜够了还没找到 → 清记忆，放手
			if (ScanElapsed >= ScanDuration)
			{
				ClearMemory();
				AutoPilotState = TEXT("GIVE-UP 搜不到 -> 交还状态树");
				return false;
			}
		}

		return true;
	}

	// ---------- 档位 ③：没记忆 / 记忆过期 → 放手 ----------
	CurrentMode = EPursueMode::Idle;

	if (bUseMemory && bHasMemory)
	{
		// 记忆过期了，顺手清掉，避免下一帧又走进来
		ClearMemory();
	}

	AutoPilotState = TEXT("IDLE 交还状态树");
	return false;
}

// ============================================================
// ⑨·a 到达减速（Arrive）—— 移植自源库 Arrive.h
// ============================================================
//
// 源库：
//   speed = maxSpeed * (dist / slowRadius) if dist <= slowRadius else maxSpeed
//   return to.normalized() * speed
//
// UE 移植的诚实说明（这一段很重要，不要当成"完整照搬"）：
//   Unreal 的 PathFollowingComponent 在接近目标时**已经内置减速**
//   （bStopOnOverlap + AcceptanceRadius 的组合），所以这里不能再改速度——
//   改了会和寻路打架（和 separation 踩过的坑同源）。
//   因此本方法的价值定位是【观测 + 可选的目标点收缩】，用于：
//     ① 输出 LastArriveSpeedRatio，供上层做动画混合 / 状态转换判断；
//     ② 需要"提前刹车"时，把目标点按比例收回，缩短最后一段路径。
//   它不驱动移动。真正驱动移动的仍然是 AIController->MoveToLocation。

FVector UPursueSteeringComponent::ComputeArrive(const FVector& GoalLocation)
{
	LastArriveSpeedRatio = 1.0f;

	const AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return GoalLocation;
	}

	// 只在水平面计算：UE 角色的上下位移由导航和重力负责，不该参与减速比例
	FVector To = GoalLocation - OwnerActor->GetActorLocation();
	To.Z = 0.0f;

	const float Dist = To.Size();

	// 已经到点：比例为 0，直接返回原地
	if (Dist < 1.0f)
	{
		LastArriveSpeedRatio = 0.0f;
		return GoalLocation;
	}

	// 还没进减速区：全速
	if (!bUseArrive || Dist >= ArriveSlowRadius)
	{
		LastArriveSpeedRatio = 1.0f;
		return GoalLocation;
	}

	// 进减速区：比例 = 剩余距离 / 减速半径（源库的同一公式）
	const float Ratio = FMath::Clamp(Dist / ArriveSlowRadius, 0.0f, 1.0f);
	LastArriveSpeedRatio = Ratio;

	// 把目标点朝自己拉回到 Ratio 处：
	// Ratio→1 时点几乎不变（全速），Ratio→0 时点贴到自己身上（停）。
	// 这样配合 AcceptanceRadius 形成平滑收尾，不会冲过头。
	return OwnerActor->GetActorLocation() + To * Ratio;
}

// ============================================================
// ⑨·b 碰撞规避（Collision Avoidance）—— 移植自源库 CollisionAvoidance.h
// ============================================================
//
// 与 Separation 的本质区别：
//   Separation：距离近就推，不看对方运动方向 —— 恐慌式，会误判。
//   CollisionAvoidance：算「最接近时刻」，只躲真正会撞上的那个目标。
//
// 核心公式（源库式 3.1）：t = -dot(relPos, relVel) / |relVel|²
//   ★ 负号不能漏。源库注释明确记了 C++ 参考实现漏负号导致
//     「正面冲来」被判成「正在远离」而跳过规避。本实现带负号。
//
// UE 差异（胶水层）：
//   源库输出速度向量；这里输出「目标点偏移」（方向 × CollisionRadius），
//   由调用方叠加到 MoveTo 的目标点上，不破坏寻路。
//   另外做了降频：15 敌 × 每帧遍历全场景 Pawn 是 O(n²)，必须限流。

FVector UPursueSteeringComponent::ComputeCollisionAvoidance()
{
	LastAvoidDirection = FVector::ZeroVector;
	AvoidTargetActor = nullptr;

	if (!bUseCollisionAvoidance || !GetWorld())
	{
		return FVector::ZeroVector;
	}

	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn)
	{
		return FVector::ZeroVector;
	}

	// ---------- 降频：用世界时间戳，避免多调用点各自维护计时器 ----------
	const float Now = GetWorld()->GetTimeSeconds();
	if (CollisionCheckInterval > 0.0f
		&& (Now - LastCollisionCheckTime) < CollisionCheckInterval)
	{
		// 复用上次结果，保证降频期间行为连续不抖动
		LastAvoidDirection = CachedAvoidDirection;
		return CachedAvoidDirection;
	}
	LastCollisionCheckTime = Now;

	const FVector MyPos = OwnerPawn->GetActorLocation();
	const FVector MyVel = OwnerPawn->GetVelocity();

	float ShortestTime = TNumericLimits<float>::Max();
	FVector BestAvoidDir = FVector::ZeroVector;
	AActor* BestActor = nullptr;

	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* Other = *It;
		if (!Other || Other == OwnerPawn)
		{
			continue;
		}

		// 只对 AI 控制的同伴做规避：玩家不该被推，也不该躲玩家躲到撞墙
		if (!Cast<AAIController>(Other->GetController()))
		{
			continue;
		}

		FVector RelPos = Other->GetActorLocation() - MyPos;
		FVector RelVel = Other->GetVelocity() - MyVel;
		RelPos.Z = 0.0f;
		RelVel.Z = 0.0f;

		const float RelSpeedSq = RelVel.SizeSquared();
		if (RelSpeedSq < 1.0f)
		{
			continue;   // 相对静止，不会相撞
		}

		// 最接近时刻（带负号，见上面的说明）
		const float TimeToCollision =
			-FVector::DotProduct(RelPos, RelVel) / RelSpeedSq;

		if (TimeToCollision <= 0.0f)
		{
			continue;   // 正在远离，或已过最近点
		}

		// 最近时刻的相对位置
		const FVector Predicted = RelPos + RelVel * TimeToCollision;

		// 最近点仍比 2r 远 → 安全，不用躲
		if (Predicted.Size() > 2.0f * CollisionRadius)
		{
			continue;
		}

		// 只处理「最早会撞」的那一个，不做无意义的叠加
		if (TimeToCollision < ShortestTime)
		{
			ShortestTime = TimeToCollision;

			FVector Avoid = -Predicted;   // 远离预测碰撞点
			Avoid.Z = 0.0f;
			if (Avoid.SizeSquared() > 1.0f)
			{
				BestAvoidDir = Avoid.GetSafeNormal();
				BestActor = Other;
			}
		}
	}

	// 方向 × 半径 = 目标点偏移量（语义：绕着让开，不改速度）
	const FVector Result = BestAvoidDir * CollisionRadius;

	LastAvoidDirection = Result;
	AvoidTargetActor = BestActor;
	CachedAvoidDirection = Result;

	return Result;
}

// ============================================================
// ⑨·c 游荡（Wander）—— 移植自源库 Wander.h
// ============================================================
//
// 源库思路：
//   在角色前方 offset 处放一个半径 radius 的圆，每帧在圆上取一点朝它走。
//   圆永远在前方 → 轨迹平滑，不会像「随机点 + 直线」那样机械。
//   漂移用「两个均匀随机之差」近似二项分布，集中在 0 附近，避免大角度抖动。
//
// UE 差异（胶水层）：
//   random.Random → FRandomStream（同样支持固定 seed，行为可复现）；
//   输出仍然是目标点（不输出速度），交给 MoveToLocation。
//   朝向取当前速度方向；静止时退化为 Actor 前向，避免零向量。
//
// 注意：调用方需要自己决定「多久重新取一次目标点」——
//   每帧都取会导致目标点持续抖动（虽然幅度小），建议 0.5~1 秒一次，
//   或者在到达 LastWanderTarget 附近时再取新的。

FVector UPursueSteeringComponent::ComputeWander()
{
	if (!bUseWander)
	{
		return FVector::ZeroVector;
	}

	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn)
	{
		return FVector::ZeroVector;
	}

	// 随机流只初始化一次。WanderSeed=0 表示每次运行都不同（便于观察）
	if (!bWanderRngInitialized)
	{
		WanderRng.Initialize(WanderSeed != 0 ? WanderSeed : FMath::Rand());
		bWanderRngInitialized = true;
	}

	const FVector MyPos = OwnerPawn->GetActorLocation();

	// 朝向优先用速度（正在走的方向），静止时用 Actor 前向
	FVector Heading = OwnerPawn->GetVelocity();
	Heading.Z = 0.0f;
	if (Heading.SizeSquared() < 1.0f)
	{
		Heading = OwnerPawn->GetActorForwardVector();
		Heading.Z = 0.0f;
	}
	Heading = Heading.GetSafeNormal();
	if (Heading.IsNearlyZero())
	{
		Heading = FVector::ForwardVector;
	}

	// 二项分布近似：两个均匀随机之差，结果集中在 0 附近
	const float Drift = (WanderRng.FRand() - WanderRng.FRand()) * WanderRate;

	// 游荡圆中心：前进方向前方
	const FVector CircleCenter = MyPos + Heading * WanderOffset;

	// 在圆上取漂移点：把前进方向绕 Z 轴转一个小角度
	const FVector OffsetDir = Heading.RotateAngleAxis(
		FMath::RadiansToDegrees(Drift), FVector::UpVector);

	const FVector WanderTarget = CircleCenter + OffsetDir * WanderRadius;

	LastWanderTarget = WanderTarget;
	return WanderTarget;
}
