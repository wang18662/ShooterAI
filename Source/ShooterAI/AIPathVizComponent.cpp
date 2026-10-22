// AIPathVizComponent.cpp

#include "AIPathVizComponent.h"

#include "PursueSteeringComponent.h"
#include "ShooterAIDebug.h"

#include "GameFramework/Pawn.h"
#include "AIController.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

UAIPathVizComponent::UAIPathVizComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// 放在 PostPhysics：这样拿到的是这一帧移动完成后的最终位置，
	// 轨迹点不会因为"先采样后移动"而整体偏移一帧。
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UAIPathVizComponent::BeginPlay()
{
	Super::BeginPlay();

	ResolveCachedReferences();

	// 第一个点就用出生位置，避免轨迹从"第一次移动后"才开始
	if (const AActor* OwnerActor = GetOwner())
	{
		LastSampleLocation = OwnerActor->GetActorLocation();
		TrailPoints.Add(LastSampleLocation + FVector(0, 0, TrailZOffset));
	}
}

void UAIPathVizComponent::ResolveCachedReferences()
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	if (!CachedPawn)
	{
		CachedPawn = Cast<APawn>(OwnerActor);
	}

	if (!CachedAIController)
	{
		CachedAIController = Cast<AAIController>(OwnerActor->GetInstigatorController());
		if (!CachedAIController && CachedPawn)
		{
			CachedAIController = Cast<AAIController>(CachedPawn->GetController());
		}
	}

	if (!CachedPursue)
	{
		CachedPursue = OwnerActor->FindComponentByClass<UPursueSteeringComponent>();
	}
}

void UAIPathVizComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bEnabled)
	{
		return;
	}

	// 可视化不算核心逻辑，受统一调试级别控制：
	// 级别 0（全关）时连采样都停，避免无意义开销。
	if (!ShooterAIDebug::ShouldPrintAggregate())
	{
		return;
	}

	ResolveCachedReferences();

	if (bDrawTrail)
	{
		SampleTrail(DeltaTime);
		DrawTrail();
	}

	if (bDrawStateLabel)
	{
		DrawStateLabel();
	}

	if (bDrawTargetPoint)
	{
		DrawTargetMarker();
	}
}

void UAIPathVizComponent::ClearTrail()
{
	TrailPoints.Reset();

	if (const AActor* OwnerActor = GetOwner())
	{
		LastSampleLocation = OwnerActor->GetActorLocation();
		TrailPoints.Add(LastSampleLocation + FVector(0, 0, TrailZOffset));
	}
}

void UAIPathVizComponent::SetDisplayPhase(const FString& PhaseText)
{
	OverridePhaseText = PhaseText;
}

// ============================================================
// ① 轨迹采样
// ============================================================

void UAIPathVizComponent::SampleTrail(float DeltaTime)
{
	const AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	SampleTimer += DeltaTime;
	if (SampleTimer < SampleInterval)
	{
		return;
	}
	SampleTimer = 0.0f;

	const FVector Current = OwnerActor->GetActorLocation();

	// 位移太小就不采样：静止的敌人不该在同一个位置堆几百个点。
	// 这个判据比"按时间采样"更合理 —— 站着不动时轨迹自然停止延伸。
	if (FVector::DistSquared(Current, LastSampleLocation)
		< MinSampleDistance * MinSampleDistance)
	{
		return;
	}

	LastSampleLocation = Current;
	TrailPoints.Add(Current + FVector(0, 0, TrailZOffset));

	// 环形丢弃最旧的点：保持线长有限，且不用 Shrink 反复分配内存
	if (TrailPoints.Num() > MaxTrailPoints)
	{
		TrailPoints.RemoveAt(0, TrailPoints.Num() - MaxTrailPoints, EAllowShrinking::No);
	}
}

void UAIPathVizComponent::DrawTrail() const
{
	const UWorld* World = GetWorld();
	if (!World || TrailPoints.Num() < 2)
	{
		return;
	}

	// 逐段画，而不是 DrawDebugLine 一次画折线：
	// 分段可以按新旧做颜色渐变（越旧越暗），一眼看出方向。
	const int32 Total = TrailPoints.Num();

	for (int32 i = 1; i < Total; ++i)
	{
		// 0 = 最旧，1 = 最新
		const float Alpha = static_cast<float>(i) / static_cast<float>(Total - 1);

		// 越新越亮：旧段压暗到 25%，避免整条线抢走角色的视觉焦点
		FColor SegmentColor = TrailColor;
		SegmentColor.R = static_cast<uint8>(TrailColor.R * FMath::Lerp(0.25f, 1.0f, Alpha));
		SegmentColor.G = static_cast<uint8>(TrailColor.G * FMath::Lerp(0.25f, 1.0f, Alpha));
		SegmentColor.B = static_cast<uint8>(TrailColor.B * FMath::Lerp(0.25f, 1.0f, Alpha));

		DrawDebugLine(World, TrailPoints[i - 1], TrailPoints[i], SegmentColor,
			false, 0.25f, 0, TrailThickness);
	}
}

// ============================================================
// ② 状态标签
// ============================================================

FString UAIPathVizComponent::GetPhaseText() const
{
	// Task 写入的阶段优先（它知道 HEALING / DESPERATE 这类细阶段）
	if (!OverridePhaseText.IsEmpty())
	{
		return OverridePhaseText;
	}

	if (!CachedPursue)
	{
		return TEXT("IDLE");
	}

	// 血量档位优先显示 —— 它比普通追击更值得注意。
	// 这两个查询是纯读值，开销可忽略（属性指针已缓存）。
	if (CachedPursue->IsCriticalHealth())
	{
		// 还有道具 → 会去回血；没道具 → 拼死冲锋。两者视觉上要区分。
		return CachedPursue->HasHealItem() ? TEXT("FLEE") : TEXT("DESPERATE");
	}
	if (CachedPursue->IsLowHealth())
	{
		return TEXT("RETREAT");
	}

	switch (CachedPursue->CurrentMode)
	{
	case EPursueMode::Pursuing: return TEXT("PURSUING");
	case EPursueMode::Chasing:  return TEXT("SEARCHING");
	case EPursueMode::Scanning: return TEXT("SCANNING");
	default:                    return TEXT("IDLE");
	}
}

FColor UAIPathVizComponent::GetPhaseColor(const FString& PhaseText)
{
	if (PhaseText == TEXT("FLEE"))      return FColor(255, 40, 40);    // 红：逃
	if (PhaseText == TEXT("DESPERATE")) return FColor(180, 0, 200);    // 紫：拼死
	if (PhaseText == TEXT("HEALING"))   return FColor(60, 255, 180);   // 青绿：回血
	if (PhaseText == TEXT("RETREAT"))   return FColor(255, 150, 0);    // 橙：退
	if (PhaseText == TEXT("PURSUING"))  return FColor(80, 255, 80);    // 绿：追
	if (PhaseText == TEXT("SEARCHING")) return FColor(80, 200, 255);   // 青：搜
	if (PhaseText == TEXT("SCANNING"))  return FColor(220, 80, 255);   // 紫：环视
	return FColor(150, 150, 150);                                     // 灰：空闲
}

void UAIPathVizComponent::DrawStateLabel() const
{
	UWorld* World = GetWorld();
	const AActor* OwnerActor = GetOwner();
	if (!World || !OwnerActor || !GEngine)
	{
		return;
	}

	const FString PhaseText = GetPhaseText();
	const FColor Color = GetPhaseColor(PhaseText);

	const FVector LabelLocation =
		OwnerActor->GetActorLocation() + FVector(0, 0, LabelHeight);

	// DrawDebugString 会在世界里生成 3D 文字（跟随相机始终面向观察者）。
	// 只在"有事发生"时显示：IDLE 的敌人不标签，否则 15 个灰标签糊满画面。
	if (PhaseText == TEXT("IDLE"))
	{
		return;
	}

	DrawDebugString(World, LabelLocation, PhaseText, nullptr, Color, 0.25f, true, 1.3f);
}

// ============================================================
// ③ 目标点标记
// ============================================================

void UAIPathVizComponent::DrawTargetMarker() const
{
	UWorld* World = GetWorld();
	const AActor* OwnerActor = GetOwner();
	if (!World || !OwnerActor || !CachedPursue)
	{
		return;
	}

	const FVector PredictPoint = CachedPursue->CurrentPredictedLocation;

	// 预判点未初始化（全零）时不画，避免在世界原点出现一个幽灵球
	if (PredictPoint.IsNearlyZero())
	{
		return;
	}

	const FVector MyLocation = OwnerActor->GetActorLocation();

	// 只有"确实在追/在搜"时才画，IDLE 时那个点是上一轮残留，画出来会误导
	if (CachedPursue->CurrentMode == EPursueMode::Idle)
	{
		return;
	}

	// 预判点用黄色：和轨迹的青、状态标签的红/绿都区分开
	const FColor MarkerColor(255, 230, 0);

	DrawDebugSphere(World, PredictPoint, TargetMarkerRadius, 12, MarkerColor, false, 0.25f, 0, 1.5f);

	if (bDrawTargetLink)
	{
		// 细虚线效果：用较低的 line width，视觉上不与轨迹线抢
		DrawDebugLine(World, MyLocation, PredictPoint, MarkerColor, false, 0.25f, 0, 1.0f);
	}
}
