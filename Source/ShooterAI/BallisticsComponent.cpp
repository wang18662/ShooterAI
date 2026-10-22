// BallisticsComponent.cpp
// 弹道解算实现 —— 移植自《AI for Games》(第3版) 3.5.3 FiringSolution
//
// 这份代码的价值不在公式（公式是书上的），在三个工程判断：
//   ① 源库 delta 符号写反了，照抄会把方向射向反方向
//   ② 判别式 < 0 时必须优雅降级（回退直瞄），不能乱打
//   ③ 打移动目标要迭代求提前量，因为 t 和目标位置互相依赖

#include "BallisticsComponent.h"

#include "GameFramework/Actor.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"

UBallisticsComponent::UBallisticsComponent()
{
	// 纯计算组件，不需要 Tick
	PrimaryComponentTick.bCanEverTick = false;
}

// ============================================================
// 核心数学：解飞行时间
// ============================================================
//
// 推导（源库注释里也写了，这里保留完整推导便于讲）：
//   p(t) = start + u·sm·t + 0.5·g·t²          （抛射物位置）
//   令 p(t) = end，且 |u| = 1
//   ⇒ u·sm·t = Δ - 0.5·g·t²
//   两边取模平方（因为 |u| = 1）：
//      sm²t² = |Δ|² - (g·Δ)t² + 0.25|g|²t⁴
//   整理成关于 t² 的二次：
//      |g|²t⁴ - 4(g·Δ + sm²)t² + 4|Δ|² = 0
//
bool UBallisticsComponent::SolveFlightTime(const FVector& Delta, float MuzzleVel,
	float& OutFlightTime) const
{
	OutFlightTime = 0.0f;

	if (MuzzleVel <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	// 重力只作用于 Z，写成向量
	const FVector Gravity(0.0f, 0.0f, -GravityZ);
	const float GravitySq = Gravity.SizeSquared();          // |g|²

	if (GravitySq <= KINDA_SMALL_NUMBER)
	{
		// 无重力 = 直线弹道，直接 time = 距离 / 速度
		const float Dist = Delta.Size();
		if (Dist < KINDA_SMALL_NUMBER)
		{
			return false;
		}
		OutFlightTime = Dist / MuzzleVel;
		return true;
	}

	const float A = GravitySq;                                        // |g|²
	const float B = -4.0f * (FVector::DotProduct(Gravity, Delta) + MuzzleVel * MuzzleVel);
	const float C = 4.0f * Delta.SizeSquared();                       // 4|Δ|²

	const float Discriminant = B * B - 4.0f * A * C;

	// ★ 判别式 < 0 = 这个初速打不到那么远（或那么高）。
	//   必须返回 false 让上层降级，绝不能 sqrt(负数) 得到 NaN 然后乱射。
	if (Discriminant < 0.0f)
	{
		return false;
	}

	const float SqrtDisc = FMath::Sqrt(Discriminant);

	// 关于 t² 的两个根
	const float T2_0 = (-B + SqrtDisc) / (2.0f * A);
	const float T2_1 = (-B - SqrtDisc) / (2.0f * A);

	// 根必须为正才能开平方（负的 t² 无物理意义）
	//
	// ★ 必须允许"只有一个有效根"的情况。
	//   当判别式恰好为 0（或某个根算出来是负数）时，ValidTimes 里只有 1 个元素。
	//   早期版本直接用 ValidTimes[1]，一旦只有 1 个元素就数组越界崩溃：
	//     Assertion failed: (Index >= 0) && (Index < ArrayNum)
	//     Array index out of bounds: 1 into an array of size 1
	//   这是实机踩出来的一次崩溃，务必保留下面的分支判断。
	TArray<float> ValidTimes;
	ValidTimes.Reserve(2);

	if (T2_0 > KINDA_SMALL_NUMBER)
	{
		ValidTimes.Add(FMath::Sqrt(T2_0));
	}
	if (T2_1 > KINDA_SMALL_NUMBER)
	{
		ValidTimes.Add(FMath::Sqrt(T2_1));
	}

	if (ValidTimes.Num() == 0)
	{
		return false;
	}

	// 只有一个解：没得挑，直接用
	if (ValidTimes.Num() == 1)
	{
		OutFlightTime = ValidTimes[0];
		return true;
	}

	// 两个解对应「平射」和「高抛」：
	//   时间短的 = 平射（弹道低、命中快）→ 射击游戏默认要这个
	//   时间长的 = 高抛（弹道高、滞空久）→ 只有榴弹可能用
	OutFlightTime = bPreferFlatTrajectory
		? FMath::Min(ValidTimes[0], ValidTimes[1])
		: FMath::Max(ValidTimes[0], ValidTimes[1]);

	return true;
}

// ============================================================
// 打固定点
// ============================================================

bool UBallisticsComponent::SolveFixedTarget(const FVector& StartLocation,
	const FVector& TargetLocation, FBallisticSolution& OutSolution) const
{
	OutSolution = FBallisticSolution();
	OutSolution.AimPoint = TargetLocation;

	if (MuzzleVelocity <= KINDA_SMALL_NUMBER)
	{
		LastSolution = OutSolution;
		return false;
	}

	// ★ 关键：Δ = end - start（从枪口指向目标）
	//   源库伪代码写成了 start - end，符号反了会把子弹射向背后。
	const FVector Delta = TargetLocation - StartLocation;

	// ------------------------------------------------------------
	// ★ 近距保护：直接平射，不解二次方程
	//
	//   实机崩溃的根因就在这个区间：
	//     目标很近时 Δ 趋近 0，二次方程的一个根也趋近 0，
	//     被 `> KINDA_SMALL_NUMBER` 过滤掉后只剩 1 个根。
	//     早期代码无条件读 ValidTimes[1] ⇒ 数组越界崩溃。
	//
	//   但"加个 if 判断"只是止血，真正的问题是：
	//     近距离用弹道解算本身就毫无意义 ——
	//     100cm 处下坠仅 0.03cm，完全可以按直线打。
	//   所以在源头绕开退化情况，比在末端打补丁更稳。
	//
	//   副作用：近距离不再可能算出"高抛"解，行为更可预期。
	// ------------------------------------------------------------
	constexpr float DirectFireDistance = 150.0f;   // cm
	const float Distance = Delta.Size();

	if (Distance < DirectFireDistance)
	{
		if (Distance < KINDA_SMALL_NUMBER)
		{
			// 目标和枪口重合：没有方向可言，交回调用方（会退化成直瞄）
			LastSolution = OutSolution;
			return false;
		}

		OutSolution.bValid = true;
		OutSolution.LaunchDirection = Delta / Distance;
		OutSolution.FlightTime = Distance / MuzzleVelocity;
		OutSolution.ElevationDegrees = 0.0f;
		LastSolution = OutSolution;
		return true;
	}

	float FlightTime = 0.0f;
	if (!SolveFlightTime(Delta, MuzzleVelocity, FlightTime))
	{
		// 打不到：返回 false，调用方应回退到直瞄（至少还能蒙中近处）
		LastSolution = OutSolution;

		if (bDrawDebug && GetWorld())
		{
			DrawDebugLine(GetWorld(), StartLocation, TargetLocation, FColor::Red, false, 0.5f, 0, 2.0f);
		}
		return false;
	}

	// 反解方向：u = (2Δ - g·t²) / (2·sm·t)
	const FVector Gravity(0.0f, 0.0f, -GravityZ);
	FVector Direction = Delta * 2.0f - Gravity * (FlightTime * FlightTime);
	Direction /= (2.0f * MuzzleVelocity * FlightTime);

	if (Direction.IsNearlyZero())
	{
		LastSolution = OutSolution;
		return false;
	}

	OutSolution.bValid = true;
	OutSolution.LaunchDirection = Direction.GetSafeNormal();
	OutSolution.FlightTime = FlightTime;

	// 仰角：发射方向与「水平指向目标」的夹角
	const FVector FlatDir = FVector(Delta.X, Delta.Y, 0.0f).GetSafeNormal();
	if (!FlatDir.IsNearlyZero())
	{
		const float DotClamped = FMath::Clamp(FVector::DotProduct(OutSolution.LaunchDirection, FlatDir), -1.0f, 1.0f);
		OutSolution.ElevationDegrees = FMath::RadiansToDegrees(FMath::Acos(DotClamped));
		// 朝下射时 Acos 也给正值，用 Z 分量区分俯仰
		if (OutSolution.LaunchDirection.Z < 0.0f)
		{
			OutSolution.ElevationDegrees = -OutSolution.ElevationDegrees;
		}
	}

	LastSolution = OutSolution;

	if (bDrawDebug && GetWorld())
	{
		const float Dist = Delta.Size();
		const FVector End = StartLocation + OutSolution.LaunchDirection * Dist;
		DrawDebugLine(GetWorld(), StartLocation, End, FColor::Green, false, 0.5f, 0, 2.0f);
		DrawDebugSphere(GetWorld(), End, 20.0f, 8, FColor::Green, false, 0.5f);
	}

	return true;
}

// ============================================================
// 打移动目标（含提前量）
// ============================================================

bool UBallisticsComponent::SolveMovingTarget(const FVector& StartLocation,
	AActor* TargetActor, FBallisticSolution& OutSolution) const
{
	OutSolution = FBallisticSolution();

	if (!TargetActor)
	{
		LastSolution = OutSolution;
		return false;
	}

	// 迭代思路：
	//   第 0 轮：假设直接打目标当前位置，算出 t0
	//   第 1 轮：把目标位置按 t0 外推，再算 t1
	//   第 2 轮：用 t1 再外推…… 通常 2~3 轮就稳定
	//
	//   为什么需要迭代：t 取决于目标位置（距离），
	//   而目标位置又取决于 t（它会移动）。互相依赖，没有闭式解。
	FVector AimPoint = TargetActor->GetActorLocation();
	const FVector TargetVelocity = TargetActor->GetVelocity();

	float FlightTime = 0.0f;
	bool bSolved = false;

	const int32 Iterations = FMath::Clamp(InterceptIterations, 0, 6);

	for (int32 i = 0; i <= Iterations; ++i)
	{
		const FVector Delta = AimPoint - StartLocation;

		if (!SolveFlightTime(Delta, MuzzleVelocity, FlightTime))
		{
			if (i == 0)
			{
				// 连「当前位置」都打不到 → 这个距离超出射程，直接失败
				LastSolution = OutSolution;
				return false;
			}
			// 迭代中途失败：用上一轮的结果（上一轮是可行的）
			break;
		}

		bSolved = true;

		if (i < Iterations)
		{
			// 用这一轮的飞行时间外推目标位置，进入下一轮
			AimPoint = TargetActor->GetActorLocation() + TargetVelocity * FlightTime;
		}
	}

	if (!bSolved)
	{
		LastSolution = OutSolution;
		return false;
	}

	// 用最终 AimPoint 反解方向
	FBallisticSolution FixedSolution;
	if (!SolveFixedTarget(StartLocation, AimPoint, FixedSolution))
	{
		LastSolution = OutSolution;
		return false;
	}

	OutSolution = FixedSolution;
	OutSolution.AimPoint = AimPoint;
	LastSolution = OutSolution;

	return true;
}

float UBallisticsComponent::GetRequiredElevation(const FVector& StartLocation,
	const FVector& TargetLocation) const
{
	FBallisticSolution Solution;
	if (SolveFixedTarget(StartLocation, TargetLocation, Solution))
	{
		return Solution.ElevationDegrees;
	}
	return 0.0f;
}

// ============================================================
// 【蓝图接入用】返回"该瞄准的点"
// ============================================================
//
// 为什么是"点"：蓝图里武器最终做 FindLookAtRotation(枪口, 目标点)，
// 返回点可以直接替换 Return Node 的输入，不用在蓝图里再做向量运算。
//
// 数学等价性：
//   SolveFixedTarget 解出的是单位方向 LaunchDirection。
//   令 P = Muzzle + LaunchDirection × dist，
//   则 FindLookAtRotation(Muzzle, P) 得到的旋转，其朝向正是 LaunchDirection。
//   所以返回 P 与"直接用方向"完全等价。
FVector UBallisticsComponent::GetBallisticAimPoint(const FVector& MuzzleLocation,
	const FVector& TargetLocation) const
{
	FBallisticSolution Solution;

	// 解不出来 → 退化成直瞄。
	// 宁可近处还能命中，也不要返回错误点把子弹射上天。
	if (!SolveFixedTarget(MuzzleLocation, TargetLocation, Solution))
	{
		return TargetLocation;
	}

	const float Distance = FVector::Dist(MuzzleLocation, TargetLocation);
	if (Distance <= KINDA_SMALL_NUMBER)
	{
		return TargetLocation;
	}

	// 沿解算方向走同样距离，得到"该瞄的点"
	return MuzzleLocation + Solution.LaunchDirection * Distance;
}

// ============================================================
// 【蓝图接入用】返回"该瞄准的点"（含移动提前量）
// ============================================================
//
// 与 GetBallisticAimPoint 的唯一区别：
//   这里会先按飞行时间把目标位置外推，再去解重力。
//   GetBallisticAimPoint 打的是"现在的位置"，
//   玩家站着不动够用，跑起来必空。
FVector UBallisticsComponent::GetLeadAimPoint(const FVector& MuzzleLocation,
	const FVector& TargetLocation, AActor* TargetActor) const
{
	// ------------------------------------------------------------
	// 探针：一次 Play 就能看清提前量到底算了多少、有没有算。
	//
	// 为什么需要它：症状"子弹打在身后"有三种可能病因，光看现象分不清：
	//   ① TargetActor 无效 / 速度读到 0     → 提前量恒为 0
	//   ② 飞行的 TargetLocation 是个远处点   → 飞行时间被算大了
	//   ③ 提前量算对了但方向反了
	// 把四个关键数字打出来，一次就能定位，不用连猜三轮。
	//
	// Key 用 7，错开 InfluenceMap(3) / AILod(4) / ShootControl(5) / Wounded(6)
	// ------------------------------------------------------------
	if (bShowLeadDebug && GEngine)
	{
		const float InputDist = FVector::Dist(MuzzleLocation, TargetLocation);
		const float SpeedNow = IsValid(TargetActor) ? TargetActor->GetVelocity().Size() : -1.0f;

		GEngine->AddOnScreenDebugMessage(7, 0.5f, FColor::Orange,
			FString::Printf(
				TEXT("[Ballistics] 输入距离=%.0f 目标=%s 速度=%.0f | 计算结果见下一行"),
				InputDist,
				TargetActor ? *GetNameSafe(TargetActor) : TEXT("空"),
				SpeedNow));
	}

	// 没有目标 Actor → 无从知道速度，退化为打固定点。
	// 这样蓝图侧不用写"有目标/无目标"两个分支。
	if (!IsValid(TargetActor))
	{
		return GetBallisticAimPoint(MuzzleLocation, TargetLocation);
	}

	const FVector TargetVelocity = TargetActor->GetVelocity();

	// 目标基本静止（< 30cm/s）时不必迭代：
	// 提前量会小于几厘米，徒增开销。
	if (TargetVelocity.SizeSquared() < 30.0f * 30.0f)
	{
		return GetBallisticAimPoint(MuzzleLocation, TargetLocation);
	}

	// 迭代求提前量：
	//   每轮用当前估计的飞行时间外推目标位置，再重算飞行时间。
	//   由于 v 恒定、t 与距离近似线性，2~3 轮就收敛到厘米级。
	FVector AimPoint = TargetLocation;
	float FlightTime = 0.0f;
	bool bAnySolved = false;

	const int32 Iterations = FMath::Clamp(InterceptIterations, 0, 6);

	for (int32 i = 0; i <= Iterations; ++i)
	{
		FBallisticSolution Probe;
		if (!SolveFixedTarget(MuzzleLocation, AimPoint, Probe))
		{
			// 这一轮解不出来（超出射程）：
			// 若第一轮就失败，说明本来打不到，退回直瞄；
			// 若是后续轮失败，用上一轮的结果更稳。
			if (i == 0)
			{
				return TargetLocation;
			}
			break;
		}

		FlightTime = Probe.FlightTime;
		bAnySolved = true;

		// 用这一轮的飞行时间外推，作为下一轮的瞄准点
		if (i < Iterations)
		{
			AimPoint = TargetLocation + TargetVelocity * FlightTime;
		}
	}

	if (!bAnySolved)
	{
		return TargetLocation;
	}

	// 用最终 AimPoint 解出真实发射方向，再折算成"该瞄的点"
	FBallisticSolution Final;
	if (!SolveFixedTarget(MuzzleLocation, AimPoint, Final))
	{
		return AimPoint;
	}

	const float Distance = FVector::Dist(MuzzleLocation, AimPoint);
	if (Distance <= KINDA_SMALL_NUMBER)
	{
		return AimPoint;
	}

	const FVector Result = MuzzleLocation + Final.LaunchDirection * Distance;

	// 探针第二行：提前量与最终瞄准点
	if (bShowLeadDebug && GEngine)
	{
		const float LeadSize = FVector::Dist(AimPoint, TargetLocation);
		const float DistToPlayer = FVector::Dist(MuzzleLocation, TargetLocation);

		GEngine->AddOnScreenDebugMessage(7, 0.5f, FColor::Yellow,
			FString::Printf(
				TEXT("[Ballistics] 飞行=%.3fs 提前量=%.0fcm | 到目标=%.0fcm | 瞄点偏移=%.0fcm"),
				FlightTime, LeadSize, DistToPlayer,
				FVector::Dist(Result, TargetLocation)));
	}

	return Result;
}
