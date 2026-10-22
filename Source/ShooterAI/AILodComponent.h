// AILodComponent.h
// 行为细节层次（Behavioral LOD）
// 移植自《AI for Games》(第3版) 10.3.4 BehavioralLOD + 10.1.1 FrequencyScheduler
// 源库:
//   D:\c++\game AI\game AI\game AI\core\ch10_execution\BehavioralLOD.h
//   D:\c++\game AI\game AI\game AI\core\ch10_execution\FrequencyScheduler.h
//
// 为什么需要它（性能叙事）：
//   15 个敌人同屏，如果每个都每帧跑完整的 StateTree + 感知 + 寻路，
//   CPU 会被吃满。但玩家根本看不见远处 10 个敌人在做什么——
//   给它们满频思考是纯粹浪费。
//
//   教材的做法是按 importance（重要性）选行为档位；
//   UE 里对应的"行为"就是「思考频率」本身——
//   远处的敌人 0.4 秒想一次就够，眼前的必须每帧想。
//
// 与源库的差异（胶水层，面试可讲）：
//   1. 源库的 BehaviorRecord 绑定「不同的 Behavior 对象」；
//      UE 这边 StateTree 只有一棵，所以「档位」改成同一个 StateTree 的
//      【Tick 间隔】。语义完全对应：重要的每帧跑，不重要的降频跑。
//   2. 源库要求调用方自己算 importance；这里给出射击游戏的具体公式
//      （距离 + 玩家视野 + 战斗状态）。
//   3. 加了【滞回】：升档立即生效，降档要连续 N 次评估才生效。
//      源库没有这一层，因为它是纯逻辑选档、没有视觉延迟问题；
//      而掉频会让敌人动作变卡，边界抖动时玩家会看到"抽帧"。

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AILodComponent.generated.h"

class APawn;
class UStateTreeComponent;
class UPursueSteeringComponent;

/**
 * AI 的细节层次等级。
 */
UENUM(BlueprintType)
enum class EAILodLevel : uint8
{
	/** 满频：玩家眼前 / 正在交战。每帧思考。 */
	Level0_Full    UMETA(DisplayName = "0 - 满频"),
	/** 高频：附近但玩家看不到。约 20Hz。 */
	Level1_High    UMETA(DisplayName = "1 - 高频"),
	/** 中频：中距离。约 7Hz。 */
	Level2_Medium  UMETA(DisplayName = "2 - 中频"),
	/** 低频：很远，只维持基本行为。约 2.5Hz。 */
	Level3_Low     UMETA(DisplayName = "3 - 低频"),

	MAX            UMETA(Hidden)
};

/**
 * 一个档位的配置。
 *
 * 命名说明（忠实于源库）：
 *   源库 BehaviorRecord 里的 minImportance 实际是【区间上界】、
 *   maxImportance 是【区间下界】——原书命名反直觉。
 *   这里按语义改名为 ImportanceUpperBound / ImportanceLowerBound，
 *   并在注释里保留溯源，避免下游误用。
 */
USTRUCT(BlueprintType)
struct FAILodTier
{
	GENERATED_BODY()

	/** 本档能接受的 importance 上界（含）。importance 越大越重要。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ImportanceUpperBound = 1.0f;

	/** 本档能接受的 importance 下界（含）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ImportanceLowerBound = 0.0f;

	/**
	 * 应用到这个档位的 Tick 间隔（秒）。
	 * 0 = 每帧（满频）。
	 * 0.05 = 20Hz；0.15 ≈ 7Hz；0.4 = 2.5Hz。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float TickInterval = 0.0f;

	/** 这个档位的说明（编辑器里显示，方便调试时理解）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	FString Description;
};

/**
 * 单个 AI 的行为 LOD 组件。
 *
 * 用法：
 *   1. 挂到敌人蓝图（BP_ShooterNPC）上
 *   2. 它会自动找到同 Actor 上的 StateTreeComponent，按 importance 调它的 tick 间隔
 *   3. 勾 bShowDebug 能在敌人头顶看到当前 LOD 等级
 *
 * 注意：本组件自己不改变敌人"做什么"，只改变"想得多快"。
 * 决策仍然完全由 StateTree 做——这是 LOD 的正确边界。
 */
UCLASS(ClassGroup = (GameAI), meta = (BlueprintSpawnableComponent, DisplayName = "AI LOD"))
class SHOOTERAI_API UAILodComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAILodComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	// ============================================================
	// 档位配置（对应源库 BehaviorRecord[]）
	// ============================================================

	/**
	 * 四个档位的区间与 tick 间隔。
	 * 默认值按「玩家体验优先级」排：
	 *   Level0 必须每帧（玩家看得见，卡一帧都能察觉）
	 *   Level3 降到 2.5Hz（远处敌人只需要"保持在世界里活着"）
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Tiers")
	TArray<FAILodTier> Tiers;

	// ============================================================
	// importance 计算参数
	// ============================================================

	/** importance 归零的距离（cm）。超过这个距离，距离项为 0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Importance", meta = (ClampMin = "500.0", ClampMax = "30000.0"))
	float MaxImportanceDistance = 8000.0f;

	/** 距离项在 importance 里的权重（0~1）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Importance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DistanceWeight = 0.65f;

	/** 玩家视野内的额外加成。玩家看得见 = 绝对不能卡。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Importance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float InViewBonus = 0.3f;

	/** 在视野内且距离小于这个值才给加成（cm）。远处在视锥里但看不清，不必满频。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Importance", meta = (ClampMin = "500.0", ClampMax = "30000.0"))
	float InViewBonusMaxDistance = 5000.0f;

	/** 正在追击/交战时的加成。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Importance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CombatBonus = 0.15f;

	/** 玩家相机视锥的夹角余弦阈值。0.5 ≈ 60° 半角，越大越窄。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Importance", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float ViewConeDotThreshold = 0.5f;

	// ============================================================
	// 滞回（防抖）
	// ============================================================

	/**
	 * 降档前需要连续多少次评估都低于当前档位。
	 * 1 = 立即降档（会抖）；3 = 观察 3 次才降（推荐）。
	 *
	 * 为什么需要：importance 在阈值附近抖动时，
	 * 如果没有滞回，tick 间隔会在 0 和 0.4 之间反复跳，
	 * 玩家看到的是敌人一顿一顿地动 —— 比一直低频更难看。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Hysteresis", meta = (ClampMin = "1", ClampMax = "20"))
	int32 DowngradeConfirmCount = 3;

	// ============================================================
	// 评估节流
	// ============================================================

	/** 多久评估一次 importance（秒）。太频繁浪费，太稀疏响应慢。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Performance", meta = (ClampMin = "0.05", ClampMax = "2.0"))
	float EvaluationInterval = 0.25f;

	// ============================================================
	// 调试
	// ============================================================

	/** 在敌人上方显示当前 LOD 等级和 importance。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Debug")
	bool bShowDebug = false;

	/**
	 * 是否连 L3（低频）也打印。
	 *
	 * ★ 为什么默认不打印 L3
	 *   实测 15 个敌人里有 13 个处于 L3（远处、不在视野）。
	 *   这 13 行内容几乎全是 `LOD3 imp=0.00`，
	 *   但会把屏幕占满，把真正要看的信息（行为档位、射击状态）挤到看不见。
	 *
	 *   关掉后只显示 L0~L2——也就是"正在被认真处理的那几个"，
	 *   这恰好是排查时关心的对象。需要全量时把这个勾上。
	 *
	 *   这比直接关掉整个 bShowDebug 更好：
	 *   既保住了"哪些敌人在满频思考"这个关键信息，又不刷屏。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Debug")
	bool bDebugShowLowLOD = false;

	/** 是否真的把 TickInterval 应用到 StateTree。关掉 = 只观不算（用于对比测试）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Debug")
	bool bApplyToStateTree = true;

	// ============================================================
	// 只读状态
	// ============================================================

	/** 当前 LOD 等级。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "LOD")
	EAILodLevel GetCurrentLevel() const { return CurrentLevel; }

	/** 当前 importance 值（0~1）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "LOD")
	float GetCurrentImportance() const { return CurrentImportance; }

	/** 当前应用到 StateTree 的 tick 间隔（秒）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "LOD")
	float GetAppliedTickInterval() const { return AppliedTickInterval; }

	/** 强制立即重新评估（不等 EvaluationInterval）。 */
	UFUNCTION(BlueprintCallable, Category = "LOD")
	void ForceEvaluate();

private:
	/** 当前等级。 */
	UPROPERTY(Transient)
	EAILodLevel CurrentLevel = EAILodLevel::Level0_Full;

	/** 当前 importance。 */
	UPROPERTY(Transient)
	float CurrentImportance = 1.0f;

	/** 已应用的 tick 间隔。 */
	UPROPERTY(Transient)
	float AppliedTickInterval = 0.0f;

	/** 评估计时。 */
	float EvaluationTimer = 0.0f;

	/** 连续多少次评估都低于当前档位（用于滞回）。 */
	int32 DowngradeCounter = 0;

	/** 缓存的 StateTree 组件。 */
	UPROPERTY(Transient)
	TObjectPtr<UStateTreeComponent> CachedStateTree = nullptr;

	/** 缓存的 Pursue 组件（用于判断是否在交战）。 */
	UPROPERTY(Transient)
	TObjectPtr<UPursueSteeringComponent> CachedPursue = nullptr;

	/** 缓存的玩家 Pawn。 */
	UPROPERTY(Transient)
	TObjectPtr<APawn> CachedPlayerPawn = nullptr;

	/** 是否已初始化档位（BeginPlay 里填默认值）。 */
	bool bTiersInitialized = false;

	// ---------- 内部 ----------

	/** 填默认档位配置。 */
	void InitializeDefaultTiers();

	/** 计算当前 importance（0~1）。 */
	float ComputeImportance(float& OutDistanceScore, bool& OutInView, bool& OutInCombat) const;

	/** 按 importance 找匹配的档位索引。找不到返回 INDEX_NONE。 */
	int32 FindTierIndex(float Importance) const;

	/** 把档位应用到 StateTree。 */
	void ApplyTier(int32 TierIndex);

	/** 玩家是否在看我（视锥 + 距离）。 */
	bool IsInPlayerView() const;

	/** 画调试文字。 */
	void DrawDebugInfo() const;

	/** 解析依赖组件。 */
	void ResolveCachedComponents();
};
