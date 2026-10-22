// AILodSubsystem.h
// LOD 全局统计（对应源库 FrequencyScheduler 的「集中调度」视角）
//
// 为什么需要一个 Subsystem：
//   BehavioralLOD 是「单个 agent 自己选档」，但性能叙事需要的是
//   「全场有几个 AI 在满频思考」这个全局数字。
//   15 个敌人各自知道自己的等级，但没人知道总数——
//   这就是 Subsystem 的职责。
//
// 与源库 FrequencyScheduler 的关系：
//   源库用 phase 把 100 个 agent 错开到不同帧执行，避免同一帧全跑。
//   本 Subsystem 承担同样的「聚合观察」职责，但不做 phase 错峰——
//   因为 UE 的 tick interval 天然就在不同偏移上（组件创建时刻不同），
//   额外加 phase 收益很小、复杂度不低。

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AILodSubsystem.generated.h"

class UAILodComponent;

/** 某一时刻的全场 LOD 分布快照。 */
USTRUCT(BlueprintType)
struct FAILodStats
{
	GENERATED_BODY()

	/** 各等级的数量，索引 = EAILodLevel 的整数值（0~3）。 */
	UPROPERTY(BlueprintReadOnly, Category = "LOD")
	TArray<int32> LevelCounts;

	/** 总 AI 数量。 */
	UPROPERTY(BlueprintReadOnly, Category = "LOD")
	int32 TotalCount = 0;

	/** 等效的「满频思考次数/帧」。等于 Σ(1 / (1 + interval*60))。 */
	UPROPERTY(BlueprintReadOnly, Category = "LOD")
	float EffectiveThinkPerFrame = 0.0f;

	/** 满频（Level0）的数量。 */
	UPROPERTY(BlueprintReadOnly, Category = "LOD")
	int32 FullRateCount = 0;
};

/**
 * LOD 全局统计。
 *
 * 用法：不需要手动创建，UE 会自动为每个 World 生成一个。
 * 直接调 UAILodSubsystem::Get(World) 拿实例。
 *
 * 继承 UTickableWorldSubsystem 而不是 UWorldSubsystem：
 * 后者没有 Tick，要么自己开 Timer，要么实现 FTickableGameObject。
 * UTickableWorldSubsystem 把这两件事都做好了，只需要实现 Tick 和 GetStatId。
 */
UCLASS()
class SHOOTERAI_API UAILodSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---------- UTickableWorldSubsystem ----------
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UAILodSubsystem, STATGROUP_Tickables);
	}
	// 不覆盖 IsTickable：父类已实现「World 有效且已初始化」的判断，
	// 覆盖成恒 true 反而会在 World 销毁阶段被 tick 到而崩溃。

	/** 拿当前世界的实例（找不到返回 nullptr）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "LOD", meta = (WorldContext = "WorldContextObject"))
	static UAILodSubsystem* Get(const UObject* WorldContextObject);

	/** 重新统计（一般不用手动调，内部按间隔自动跑）。 */
	UFUNCTION(BlueprintCallable, Category = "LOD")
	void RefreshStats();

	/** 上一次统计的结果。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "LOD")
	const FAILodStats& GetStats() const { return CachedStats; }

	/** 返回一行可读的汇总字符串，例如 "LOD 满频 4 / 总 15，等效 6.2 次/帧"。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "LOD")
	FString GetStatsString() const;

	/** 是否在屏幕左上角显示汇总。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Debug")
	bool bShowOnScreen = true;

	/** 统计间隔（秒）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Debug", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float RefreshInterval = 0.5f;

private:
	/** 缓存结果。 */
	FAILodStats CachedStats;

	/** 计时器。 */
	float RefreshTimer = 0.0f;
};
