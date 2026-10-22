// AILodSubsystem.cpp
// LOD 全局统计实现

#include "AILodSubsystem.h"
#include "AILodComponent.h"
#include "ShooterAIDebug.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

void UAILodSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	CachedStats.LevelCounts.SetNumZeroed(static_cast<int32>(EAILodLevel::MAX));
}

void UAILodSubsystem::Deinitialize()
{
	CachedStats.LevelCounts.Reset();
	Super::Deinitialize();
}

UAILodSubsystem* UAILodSubsystem::Get(const UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}

	UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;

	return World ? World->GetSubsystem<UAILodSubsystem>() : nullptr;
}

void UAILodSubsystem::Tick(float DeltaTime)
{
	// 不调 Super::Tick —— UTickableWorldSubsystem 的 Tick 是纯接口，
	// 父类没有需要执行的逻辑（调了也只是空转）。

	RefreshTimer += DeltaTime;
	if (RefreshTimer < RefreshInterval)
	{
		return;
	}
	RefreshTimer = 0.0f;

	RefreshStats();

	if (bShowOnScreen && GEngine && ShooterAIDebug::ShouldPrintAggregate())
	{
		// 固定小 Key → 永远排在屏幕顶部，不会被每个单位的行挤走
		GEngine->AddOnScreenDebugMessage(
			ShooterAIDebug::Key::AILod, 0.6f, FColor::Cyan, GetStatsString());
	}
}

void UAILodSubsystem::RefreshStats()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	CachedStats.LevelCounts.Reset();
	CachedStats.LevelCounts.SetNumZeroed(static_cast<int32>(EAILodLevel::MAX));
	CachedStats.TotalCount = 0;
	CachedStats.EffectiveThinkPerFrame = 0.0f;
	CachedStats.FullRateCount = 0;

	// 遍历所有 Actor，找它们身上的 UAILodComponent。
	// 15 个敌人 × 每 0.5 秒一次遍历 = 开销可忽略。
	// 不用注册机制是刻意的：注册要处理 BeginPlay/EndPlay 顺序、
	// 动态生成的 Pawn、以及关卡切换时的清理，代码量翻倍但收益为零。
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		UAILodComponent* LodComp = Actor->FindComponentByClass<UAILodComponent>();
		if (!LodComp)
		{
			continue;
		}

		const int32 LevelIndex = static_cast<int32>(LodComp->GetCurrentLevel());
		if (CachedStats.LevelCounts.IsValidIndex(LevelIndex))
		{
			++CachedStats.LevelCounts[LevelIndex];
		}

		++CachedStats.TotalCount;

		if (LevelIndex == static_cast<int32>(EAILodLevel::Level0_Full))
		{
			++CachedStats.FullRateCount;
		}

		// 等效「每帧思考次数」：
		//   interval = 0     → 每帧 1 次        → 权重 1.0
		//   interval = 0.05  → 20Hz，60fps 下每 3 帧 1 次 → 权重 1/3
		//   interval = 0.4   → 2.5Hz，每 24 帧 1 次       → 权重 1/24
		// 公式：1 / (1 + interval * 60)
		const float Interval = LodComp->GetAppliedTickInterval();
		CachedStats.EffectiveThinkPerFrame += 1.0f / (1.0f + Interval * 60.0f);
	}
}

FString UAILodSubsystem::GetStatsString() const
{
	if (CachedStats.TotalCount == 0)
	{
		return TEXT("[AILod] 场上没有 AI LOD 组件");
	}

	const int32 L0 = CachedStats.LevelCounts.IsValidIndex(0) ? CachedStats.LevelCounts[0] : 0;
	const int32 L1 = CachedStats.LevelCounts.IsValidIndex(1) ? CachedStats.LevelCounts[1] : 0;
	const int32 L2 = CachedStats.LevelCounts.IsValidIndex(2) ? CachedStats.LevelCounts[2] : 0;
	const int32 L3 = CachedStats.LevelCounts.IsValidIndex(3) ? CachedStats.LevelCounts[3] : 0;

	// 省 CPU 比例：满频时总思考次数 = TotalCount
	const float SavedPercent = CachedStats.TotalCount > 0
		? (1.0f - CachedStats.EffectiveThinkPerFrame / CachedStats.TotalCount) * 100.0f
		: 0.0f;

	return FString::Printf(
		TEXT("[AILod] 总 %d | L0=%d L1=%d L2=%d L3=%d | 等效 %.1f 次/帧（省 %.0f%% CPU）"),
		CachedStats.TotalCount, L0, L1, L2, L3,
		CachedStats.EffectiveThinkPerFrame, SavedPercent);
}
