// AILodComponent.cpp
// 行为 LOD —— 移植自《AI for Games》(第3版) 10.3.4 / 10.1.1
//
// 核心思路（一句话）：
//   把「BehaviorRecord 选档位」映射成「StateTree 选 Tick 间隔」。
//   教材里重要的是哪个 behavior 在跑，UE 里重要的是 StateTree 多久跑一次。
//   语义完全对应：重要的每帧跑，不重要的降频跑。

#include "AILodComponent.h"
#include "ShooterAIDebug.h"

#include "PursueSteeringComponent.h"

#include "Components/StateTreeComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

UAILodComponent::UAILodComponent()
{
	// 本组件自己要 Tick，但不需要每帧——EvaluationInterval 会节流
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// 放到移动之后评估：这样拿到的是这一帧最新的位置
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	// 在构造函数里填默认档位，而不是 BeginPlay：
	// 这样 CDO 上就有默认值，编辑器 Details 面板能直接看到并修改，
	// 而不是打开一看是个空数组。
	InitializeDefaultTiers();
}

void UAILodComponent::BeginPlay()
{
	Super::BeginPlay();

	// 构造函数已填默认档位；这里只在「被清空」时兜底
	if (Tiers.Num() == 0)
	{
		bTiersInitialized = false;
		InitializeDefaultTiers();
	}

	ResolveCachedComponents();

	// 首帧就按当前情况定档，避免开局一瞬间全部满频
	ForceEvaluate();
}

void UAILodComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 离开游戏时把 tick 间隔还原，避免影响同一进程里后续运行的关卡
	if (bApplyToStateTree && CachedStateTree)
	{
		CachedStateTree->SetComponentTickInterval(0.0f);
	}

	Super::EndPlay(EndPlayReason);
}

void UAILodComponent::InitializeDefaultTiers()
{
	if (bTiersInitialized && Tiers.Num() > 0)
	{
		return;
	}

	// 四个档位，从高到低。
	// 注意 UpperBound / LowerBound 的语义：importance 落在这个闭区间内就选中该档。
	// 顺序很重要：FindTierIndex 从头扫描，取第一个命中，
	// 所以区间可以重叠（高优先档放前面 = 更倾向于保持高频率）。
	Tiers.Reset();

	{
		FAILodTier Tier;
		Tier.ImportanceUpperBound = 1.0f;
		Tier.ImportanceLowerBound = 0.75f;
		Tier.TickInterval = 0.0f;      // 每帧
		Tier.Description = TEXT("满频：玩家眼前 / 正在交战");
		Tiers.Add(Tier);
	}
	{
		FAILodTier Tier;
		Tier.ImportanceUpperBound = 0.749f;
		Tier.ImportanceLowerBound = 0.5f;
		Tier.TickInterval = 0.05f;     // 20Hz
		Tier.Description = TEXT("高频：附近但玩家看不到");
		Tiers.Add(Tier);
	}
	{
		FAILodTier Tier;
		Tier.ImportanceUpperBound = 0.499f;
		Tier.ImportanceLowerBound = 0.25f;
		Tier.TickInterval = 0.15f;     // ~7Hz
		Tier.Description = TEXT("中频：中距离");
		Tiers.Add(Tier);
	}
	{
		FAILodTier Tier;
		Tier.ImportanceUpperBound = 0.249f;
		Tier.ImportanceLowerBound = 0.0f;   // 兜底：覆盖到 0
		Tier.TickInterval = 0.4f;      // 2.5Hz
		Tier.Description = TEXT("低频：很远，只维持基本行为");
		Tiers.Add(Tier);
	}

	bTiersInitialized = true;
}

void UAILodComponent::ResolveCachedComponents()
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	if (!CachedStateTree)
	{
		CachedStateTree = OwnerActor->FindComponentByClass<UStateTreeComponent>();
	}

	if (!CachedPursue)
	{
		CachedPursue = OwnerActor->FindComponentByClass<UPursueSteeringComponent>();
	}

	if (!CachedPlayerPawn)
	{
		CachedPlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);
	}
}

void UAILodComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// 节流：不必每帧算 importance
	EvaluationTimer += DeltaTime;
	if (EvaluationTimer < EvaluationInterval)
	{
		return;
	}
	EvaluationTimer = 0.0f;

	ForceEvaluate();

	if (bShowDebug)
	{
		DrawDebugInfo();
	}
}

void UAILodComponent::ForceEvaluate()
{
	// 依赖组件可能晚于 BeginPlay 才创建（比如运行时生成的 Pawn），每次评估补一次
	if (!CachedStateTree || !CachedPlayerPawn)
	{
		ResolveCachedComponents();
	}

	float DistScore = 0.0f;
	bool bInView = false;
	bool bInCombat = false;

	CurrentImportance = ComputeImportance(DistScore, bInView, bInCombat);

	const int32 TargetIndex = FindTierIndex(CurrentImportance);
	if (TargetIndex == INDEX_NONE)
	{
		// 教材假定存在覆盖全部取值的后备档位；这里配置出错才会走到，
		// 保守处理：保持现状，不崩。
		return;
	}

	const int32 CurrentIndex = static_cast<int32>(CurrentLevel);

	// ---------- 滞回：升档立即，降档要攒够次数 ----------
	if (TargetIndex == CurrentIndex)
	{
		DowngradeCounter = 0;
		return;
	}

	if (TargetIndex < CurrentIndex)
	{
		// 升档（数字更小 = 更频繁）：立即生效。
		// 为什么升档不等：玩家突然看到这个敌人，卡顿会立刻被察觉，
		// 宁可多花几毫秒 CPU 也不能让他看到抽帧。
		DowngradeCounter = 0;
		CurrentLevel = static_cast<EAILodLevel>(TargetIndex);
		ApplyTier(TargetIndex);
	}
	else
	{
		// 降档：需要连续 DowngradeConfirmCount 次都判定要降
		++DowngradeCounter;
		if (DowngradeCounter >= FMath::Max(1, DowngradeConfirmCount))
		{
			DowngradeCounter = 0;
			CurrentLevel = static_cast<EAILodLevel>(TargetIndex);
			ApplyTier(TargetIndex);
		}
	}
}

float UAILodComponent::ComputeImportance(float& OutDistanceScore, bool& OutInView,
	bool& OutInCombat) const
{
	OutDistanceScore = 0.0f;
	OutInView = false;
	OutInCombat = false;

	const AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return 0.0f;
	}

	// ---------- ① 距离项 ----------
	const FVector MyLoc = OwnerActor->GetActorLocation();
	if (CachedPlayerPawn)
	{
		const float Dist = FVector::Dist(MyLoc, CachedPlayerPawn->GetActorLocation());
		OutDistanceScore = 1.0f - FMath::Clamp(Dist / FMath::Max(MaxImportanceDistance, 1.0f), 0.0f, 1.0f);
	}

	// ---------- ② 玩家视野项 ----------
	const float DistToPlayer = CachedPlayerPawn
		? FVector::Dist(MyLoc, CachedPlayerPawn->GetActorLocation())
		: TNumericLimits<float>::Max();

	OutInView = IsInPlayerView() && DistToPlayer <= InViewBonusMaxDistance;

	// ---------- ③ 战斗项 ----------
	if (CachedPursue)
	{
		// 组件处在 Pursuing/Chasing/Scanning 任一档，都说明它在主动做事
		OutInCombat = (CachedPursue->CurrentMode != EPursueMode::Idle);
	}

	// ---------- 组合 ----------
	// 用「距离为主 + 两个加成」，而不是 max：
	// 如果只用 max，一个远处但在视锥里的敌人会直接跃到满频，
	// 但玩家其实看不清它，那是浪费。
	float Importance =
		OutDistanceScore * DistanceWeight
		+ (OutInView ? InViewBonus : 0.0f)
		+ (OutInCombat ? CombatBonus : 0.0f);

	return FMath::Clamp(Importance, 0.0f, 1.0f);
}

bool UAILodComponent::IsInPlayerView() const
{
	const AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !CachedPlayerPawn)
	{
		return false;
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return false;
	}

	// 用相机视锥判断，不用视线射线：
	//   LOD 只需要「大致在不在视野里」，不需要精确遮挡。
	//   射线要穿过整张地图，15 个敌人 × 每次评估 = 太贵。
	const FVector CameraLoc = PC->PlayerCameraManager
		? PC->PlayerCameraManager->GetCameraLocation()
		: CachedPlayerPawn->GetActorLocation();

	const FRotator CameraRot = PC->PlayerCameraManager
		? PC->PlayerCameraManager->GetCameraRotation()
		: CachedPlayerPawn->GetActorRotation();

	const FVector ToActor = (OwnerActor->GetActorLocation() - CameraLoc).GetSafeNormal();
	const FVector CamForward = CameraRot.Vector();

	const float Dot = FVector::DotProduct(ToActor, CamForward);
	return Dot >= ViewConeDotThreshold;
}

int32 UAILodComponent::FindTierIndex(float Importance) const
{
	// 教材策略：从头扫描，取「第一个可用」的记录。
	// 这里的实践含义：档位数组按「高频率优先」排序，
	// 所以 importance 在边界上时会偏向保持高频率（更安全）。
	for (int32 i = 0; i < Tiers.Num(); ++i)
	{
		const FAILodTier& Tier = Tiers[i];
		if (Importance <= Tier.ImportanceUpperBound && Importance >= Tier.ImportanceLowerBound)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

void UAILodComponent::ApplyTier(int32 TierIndex)
{
	if (!Tiers.IsValidIndex(TierIndex))
	{
		return;
	}

	const float NewInterval = FMath::Max(0.0f, Tiers[TierIndex].TickInterval);
	if (FMath::IsNearlyEqual(NewInterval, AppliedTickInterval, 0.001f))
	{
		return;
	}

	AppliedTickInterval = NewInterval;

	if (!bApplyToStateTree)
	{
		return;
	}

	if (!CachedStateTree)
	{
		ResolveCachedComponents();
	}

	if (CachedStateTree)
	{
		// 这一行就是整个 LOD 的落地方式：
		//   StateTree 的思考频率 = 组件的 tick 间隔。
		CachedStateTree->SetComponentTickInterval(NewInterval);
	}
}

void UAILodComponent::DrawDebugInfo() const
{
	if (!GEngine)
	{
		return;
	}

	// 每单位一行：只在详细级别（ShooterAI.ScreenDebug 2）打印，
	// 否则 15 个敌人就是 15 行，会把聚合信息挤出屏幕。
	if (!ShooterAIDebug::ShouldPrintPerActor())
	{
		return;
	}

	// ★ 默认跳过 L3。
	//   实测 15 个敌人里 13 个是 L3（远处、不在视野），
	//   这些行的内容几乎全是 `LOD3 imp=0.00`，却能把屏幕占满，
	//   把行为档位 / 射击状态这些真正要看的信息挤到看不见。
	//   只显示 L0~L2 = "正在被认真处理的那几个"，正好是排查对象。
	if (!bDebugShowLowLOD && CurrentLevel == EAILodLevel::Level3_Low)
	{
		return;
	}

	const AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	// 颜色编码：绿=满频，黄=高频，橙=中频，灰=低频
	FColor Color = FColor::Green;
	switch (CurrentLevel)
	{
	case EAILodLevel::Level0_Full:   Color = FColor::Green;  break;
	case EAILodLevel::Level1_High:   Color = FColor::Yellow; break;
	case EAILodLevel::Level2_Medium: Color = FColor::Orange; break;
	case EAILodLevel::Level3_Low:    Color = FColor::Silver; break;
	default: break;
	}

	const FString Msg = FString::Printf(TEXT("LOD%d imp=%.2f %.2fs"),
		static_cast<int32>(CurrentLevel), CurrentImportance, AppliedTickInterval);

	// Key 用 Owner 指针 + 2，避开 PursueComp(+0) 和 PursueTask(+1) 的 key
	const uint64 Key = static_cast<uint64>(reinterpret_cast<UPTRINT>(OwnerActor)) + 2;
	GEngine->AddOnScreenDebugMessage(Key, 0.3f, Color, Msg);
}
