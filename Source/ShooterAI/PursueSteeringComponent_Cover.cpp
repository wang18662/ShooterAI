// ============================================================
// PursueSteeringComponent_Cover.cpp
// ============================================================
//
// 【这个文件做什么】
//   回答一个问题：**从这里，玩家看得见我吗？**
//   · IsInCover            单点判定（眼高对眼高打一条射线，被挡住 = 有掩体）
//   · FindCoverPoint       在周围采样找一个能挡住视线的点（蓝图可调）
//   · FindCoverPointEx     同上，但支持"黑名单"（C++ 内部用）
//
// 【为什么单独一个文件】
//   它是整个项目里唯一的"几何查询"代码：不属于算法（不产出运动），
//   也不属于血量（不存状态），只回答一个是/否。放进哪个文件都别扭。
//
// 【核心观念：安全 ≠ 距离远】
//   "离玩家远"不代表安全 —— 在开阔地退 3000cm 照样会挨打。
//   真正的掩体是"你看不到我"。所以这里用**射线遮挡**判定，不用距离。
//
// 【采样为什么是扇形，不是整圈】
//   背离玩家的方向才是安全的；扫整圈会包含"朝着玩家跑"的方向，等于送死。
//   所以以"背离方向"为中心开 180° 扇形，再由近到远分几圈试（优先用近的）。
//
// 【为什么必须有黑名单（FindCoverPointEx 的 AvoidPoints）】
//   本函数是**确定性**的：同样的起点 + 同样的玩家位置，每帧返回同一个"最近可用点"。
//   一旦那个点实际站不住（柱子太窄、投影偏移），不拉黑就等于
//   "选中 → 到达 → 暴露 → 清空 → 再选中"的无限循环 —— 表现是敌人站着不动。
//
// 【不在这里的东西】
//   "选哪个掩体、什么时候去、到了要不要回血" 全是决策，在
//   StateTreePursueTask.cpp 里（那是行为层，这里只是传感器）。
//
// ============================================================

#include "PursueSteeringComponent.h"

#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "NavigationSystem.h"

// ============================================================
// ④ 掩体探测（本节对应 .h 里的 ④）
// ============================================================

bool UPursueSteeringComponent::IsInCover(const FVector& TestPoint, AActor* Threat) const
{
	const UWorld* World = GetWorld();
	if (!World || !IsValid(Threat))
	{
		return false;
	}

	// 两个端点都抬高到眼高。
	// 用脚底做射线会因为地面起伏误判（贴地射一条线永远被地面自己挡住）。
	const FVector EyeFrom = TestPoint + FVector(0, 0, CoverEyeHeight);
	const FVector ThreatEye = Threat->GetActorLocation() + FVector(0, 0, CoverEyeHeight);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(CoverCheck), false);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.AddIgnoredActor(Threat);

	FHitResult Hit;
	const bool bBlocked = World->LineTraceSingleByChannel(
		Hit, ThreatEye, EyeFrom, CoverTraceChannel, QueryParams);

	// 被挡住 = 威胁源看不到这个点 = 有掩体
	return bBlocked;
}

bool UPursueSteeringComponent::FindCoverPoint(const FVector& Origin, AActor* Threat,
	float SearchRadius, FVector& OutPoint) const
{
	return FindCoverPointEx(Origin, Threat, SearchRadius, OutPoint, nullptr, 0.0f);
}

bool UPursueSteeringComponent::FindCoverPointEx(const FVector& Origin, AActor* Threat,
	float SearchRadius, FVector& OutPoint,
	const TArray<FVector>* AvoidPoints, float AvoidRadius) const
{
	const UWorld* World = GetWorld();
	if (!World || !IsValid(Threat))
	{
		return false;
	}

	const FVector ThreatLoc = Threat->GetActorLocation();

	// 远离威胁的水平方向 —— 候选点主要往这个方向找
	FVector AwayDir = Origin - ThreatLoc;
	AwayDir.Z = 0.0f;
	if (AwayDir.SizeSquared() < 1.0f)
	{
		AwayDir = FVector::ForwardVector;
	}
	AwayDir = AwayDir.GetSafeNormal();

	// 在"远离方向"两侧各 90° 的扇形内采样（即背离玩家的整个半圆）。
	// 为什么不采整圈：背离玩家跑到敌人正面等于送死，
	// 而纯直线后退找到的往往是"更远但没遮挡"的开阔地。
	// 扇形 + 射线过滤，才能找到"侧向绕到掩体后"的路径。
	constexpr int32 NumSamples = 11;         // 11 个方向（每 18°）
	constexpr float FanHalfAngleDeg = 90.0f; // 扇形半角

	// 半径从近到远逐圈试，优先找近的（少跑路、更快进掩体）。
	//
	// ★ 内圈必须够近：早期版本最小圈是 0.55 倍（1500 × 0.55 = 825cm），
	//   于是敌人哪怕就贴在柱子边上，也会被派去 8 米外的采样点。
	//   那个点经常不可达，表现为"血量一过 25% 就卡住不动"。
	//   加上 0.2 倍（300cm）这一圈，脚下有掩体就地用。
	constexpr int32 NumRings = 4;
	constexpr float RingScales[NumRings] = { 0.2f, 0.4f, 0.65f, 1.0f };

	FVector BestPoint = FVector::ZeroVector;
	float BestDistSq = FLT_MAX;
	bool bFound = false;

	for (int32 Ring = 0; Ring < NumRings; ++Ring)
	{
		// 0.55 / 0.8 / 1.0 倍半径
		const float Radius = SearchRadius * (0.55f + 0.225f * Ring);

		for (int32 i = 0; i < NumSamples; ++i)
		{
			// 把 i 映射到 [-FanHalfAngle, +FanHalfAngle]
			const float Alpha = (NumSamples == 1)
				? 0.0f
				: (static_cast<float>(i) / static_cast<float>(NumSamples - 1)) * 2.0f - 1.0f;
			const float AngleDeg = Alpha * FanHalfAngleDeg;

			const FVector CandidateDir = AwayDir.RotateAngleAxis(AngleDeg, FVector::UpVector);
			const FVector Candidate = Origin + CandidateDir * Radius;

			// 必须可走
			FVector Projected;
			if (!ProjectToNavMesh(Candidate, Projected))
			{
				continue;
			}

			// 投影不能跑太远。
			// 候选点落在墙里 / 房间外时，投影会把它吸附到最近的另一块导航面上，
			// 常常是"隔壁房间"或"台阶顶上" —— 这种点路径根本走不到，
			// 选中它就会让敌人冲着墙走然后卡住。超过 250cm 直接判为不可用。
			if (FVector::Dist(Projected, Candidate) > 250.0f)
			{
				continue;
			}

			// 跳过已经试过、站不住的点（黑名单）。
			// 不做这个过滤，每帧都会重新选中同一个无效点 → 死循环。
			if (AvoidPoints && AvoidRadius > 0.0f)
			{
				const float AvoidRadiusSq = FMath::Square(AvoidRadius);
				bool bAvoided = false;
				for (const FVector& Bad : *AvoidPoints)
				{
					if (FVector::DistSquared2D(Bad, Projected) < AvoidRadiusSq)
					{
						bAvoided = true;
						break;
					}
				}
				if (bAvoided)
				{
					continue;
				}
			}

			// 必须真的有遮挡
			if (!IsInCover(Projected, Threat))
			{
				continue;
			}

			const float DistSq = FVector::DistSquared(Origin, Projected);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				BestPoint = Projected;
				bFound = true;
			}
		}

		// 找到近的就不再往远了找
		if (bFound)
		{
			break;
		}
	}

	if (bFound)
	{
		OutPoint = BestPoint;
	}
	return bFound;
}
