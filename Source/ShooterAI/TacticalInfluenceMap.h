// TacticalInfluenceMap.h
// 战术影响图（Tactical Influence Map）
// 移植自《AI for Games》(第3版) 6.2.2 简单影响地图 + 6.3 战术位置
// 源库:
//   D:\c++\game AI\game AI\game AI\core\ch06_tactical\FloodDijkstra.h
//   D:\c++\game AI\game AI\game AI\core\ch06_tactical\ConvolutionFilter.h
//
// 为什么需要它（UE 没有的东西）：
//   UE 有 EQS（环境查询），但 EQS 是「对一批候选点逐个打分」——
//   每次查询都要重新跑一遍。影响图是「先算出整张地图的场」，
//   算一次，任意位置随时查。3A 射击游戏让 AI「知道哪里危险」的标配做法。
//
//   更关键的是：NavMesh 给的是「最短路径」，它不知道「火力覆盖区」在哪。
//   影响图补的正是这一层——把威胁信息变成 AI 可查询的空间数据。
//
// 算法三步（与源库对应）：
//   ① 建格   ← 对应源库 TacticalMap（格子地图）
//   ② 泛洪   ← 对应 mapFloodDijkstra（优先队列按强度取最大）
//   ③ 平滑   ← 对应 convolve（3x3 高斯，双缓冲）
//
// 与源库的差异（胶水层，面试可讲）：
//   1. source 库 strengthFunction(city, location) 要求调用方知道「到源点的距离」，
//      但泛洪过程中拿不到这个距离。改成「累积衰减」：
//      neighborStrength = currentStrength - DecayPerCell
//      等价于线性衰减，且天然包含「绕路成本」——绕障走远了强度自然更低，
//      这正是 flood 的意义。
//   2. Node = int（一维格子索引）→ 一维 TArray + X + Y*Width 换算，同构但更省内存。
//   3. 源库输出 LocationRecord 列表；UE 按世界坐标查询，所以额外提供
//      GetThreatAt(Location) / FindSafestPoint()。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TacticalInfluenceMap.generated.h"

class UNavigationSystemV1;

/** 一格的数据。 */
USTRUCT()
struct FTacticalCell
{
	GENERATED_BODY()

	/** 格子中心的世界坐标（已投影到 NavMesh 上）。 */
	UPROPERTY()
	FVector WorldLocation = FVector::ZeroVector;

	/** 是否可走（NavMesh 投影成功）。不可走的格子不参与泛洪。 */
	UPROPERTY()
	bool bWalkable = false;

	/** 威胁强度（0~1）。越大越危险。 */
	UPROPERTY()
	float Threat = 0.0f;

	/** 友方/安全强度（0~1）。越大越安全。 */
	UPROPERTY()
	float Safety = 0.0f;

	/** 综合值 = Safety - Threat。正数偏安全，负数偏危险。 */
	UPROPERTY()
	float Combined = 0.0f;
};

/**
 * 战术影响图。
 *
 * 用法：
 *   1. 关卡里放一个 ATacticalInfluenceMap
 *   2. Details 里设 MapExtent（覆盖范围）、CellSize（精度）
 *   3. 勾 bDrawDebug 看热力图（红=危险 绿=安全）
 *   4. AI 用 GetThreatAt() / FindSafestPoint() 查询
 */
UCLASS(ClassGroup = (GameAI), meta = (DisplayName = "Tactical Influence Map"))
class SHOOTERAI_API ATacticalInfluenceMap : public AActor
{
	GENERATED_BODY()

public:
	ATacticalInfluenceMap();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// ============================================================
	// 地图参数
	// ============================================================

	/** 地图半宽（cm）。以 Actor 位置为中心，向四周扩展这么多。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Map", meta = (ClampMin = "500.0", ClampMax = "30000.0"))
	float MapExtent = 6000.0f;

	/** 格子边长（cm）。越小越精细，但格子数按平方增长。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Map", meta = (ClampMin = "50.0", ClampMax = "1000.0"))
	float CellSize = 200.0f;

	/** NavMesh 投影的最大高度差（cm）。超过视为不可达。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Map", meta = (ClampMin = "50.0", ClampMax = "2000.0"))
	float MaxProjectionHeight = 500.0f;

	// ============================================================
	// 影响力源
	// ============================================================

	/** 威胁源（通常是玩家）。每个源向外辐射危险强度。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Sources")
	TArray<TObjectPtr<AActor>> ThreatSources;

	/** 友方源（队友 AI 位置）。队友附近算安全。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Sources")
	TArray<TObjectPtr<AActor>> SafetySources;

	/** 威胁源的有效半径（cm）。超出这个范围强度衰减到 0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Sources", meta = (ClampMin = "200.0", ClampMax = "20000.0"))
	float ThreatRadius = 3500.0f;

	/** 友方源的辐射半径（cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Sources", meta = (ClampMin = "200.0", ClampMax = "20000.0"))
	float SafetyRadius = 2500.0f;

	/** 威胁源的初始强度（源点自身的强度，通常 1.0 = 满）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Sources", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ThreatSourceStrength = 1.0f;

	/** 友方源的初始强度。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Sources", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SafetySourceStrength = 0.6f;

	// ============================================================
	// 平滑（对应源库 convolve）
	// ============================================================

	/** 高斯模糊迭代次数。0 = 不平滑（会看到格子锯齿）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Smoothing", meta = (ClampMin = "0", ClampMax = "10"))
	int32 SmoothIterations = 2;

	// ============================================================
	// 刷新节流
	// ============================================================

	/**
	 * 重建间隔（秒）。
	 *
	 * ★ 不要设成 0：
	 *   0 = 每帧重建。每帧要跑两次泛洪 + 两轮高斯卷积，
	 *   在 Debug 构建下 3600 格约 10~25ms/帧，帧率会被压到 15~30，
	 *   表现出来就是"角色移动变得很迟钝"。
	 *   0.15~0.5 之间肉眼看不出延迟（红色区域跟随玩家会有轻微滞后，但不影响判断）。
	 *   只在需要逐帧观察时才临时设 0，看完改回来。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Performance", meta = (ClampMin = "0.05", ClampMax = "10.0"))
	float RebuildInterval = 0.3f;

	// ============================================================
	// 调试
	// ============================================================

	/** 是画热力图。红=危险，绿=安全，灰=混合。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Debug")
	bool bDrawDebug = false;

	/**
	 * 绘制时的步长：每隔 N 格画一个。
	 *
	 * ★ 为什么必须有这个：
	 *   DrawDebugBox 每帧 3600 次调用，每个都要构造 FVector/FColor 并提交绘制指令，
	 *   在 Debug 构建下能吃掉 20~40ms，直接把帧率从 60 拉到个位数——
	 *   表现出来就是"角色动不了"。
	 *   设成 3 表示 60x60 只画 20x20 = 400 个方块，视觉上仍能看出热力分布，
	 *   开销降到 1/9。
	 *   1 = 全画（最清晰但最慢），2~4 = 推荐，>6 = 只看大概。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Debug", meta = (ClampMin = "1", ClampMax = "10"))
	int32 DebugDrawStride = 3;

	/** 热力图方块的 Z 偏移（cm），避免和地面 z-fighting。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Debug", meta = (ClampMin = "-50.0", ClampMax = "200.0"))
	float DebugDrawHeight = 15.0f;

	/** 热力图方块高度（cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Debug", meta = (ClampMin = "1.0", ClampMax = "200.0"))
	float DebugBoxHeight = 3.0f;

	// 注：早期版本这里暴露了 DebugDrawDuration 参数，已删除。
	// 原因是它两个极端都是坑：
	//   设 0 → 方块只活当前帧，采样错开就闪；
	//   设大 → 每帧累积，3600 格 × N 帧直接卡死。
	// 现在改为内部按重建间隔自动算（见 Tick），用户不需要也不应该管它。

	/** 是否在屏幕打印统计信息（格数、重建耗时）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Influence|Debug")
	bool bShowStats = true;

	// ============================================================
	// 对外查询 API
	// ============================================================

	/** 查询世界坐标处的威胁强度（0~1）。超出地图范围返回 0。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Influence")
	float GetThreatAt(const FVector& WorldLocation) const;

	/** 查询世界坐标处的安全强度（0~1）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Influence")
	float GetSafetyAt(const FVector& WorldLocation) const;

	/** 查询综合值（Safety - Threat）。正=安全，负=危险。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Influence")
	float GetCombinedAt(const FVector& WorldLocation) const;

	/**
	 * 在 Origin 周围 SearchRadius 内找「最安全」的可走点。
	 * 找不到时返回 false，OutPoint 保持不变。
	 */
	UFUNCTION(BlueprintCallable, Category = "Influence")
	bool FindSafestPointNear(const FVector& Origin, float SearchRadius, FVector& OutPoint) const;

	/**
	 * 在 Origin 周围 SearchRadius 内找「威胁最低且朝向目标方向」的点。
	 * 用途：追人时不要直线冲，从威胁低的一侧接近。
	 *
	 * @param Origin        自己的位置
	 * @param Goal          想去的方向（通常是目标位置）
	 * @param SearchRadius  搜索半径
	 * @param OutPoint      输出点
	 */
	UFUNCTION(BlueprintCallable, Category = "Influence")
	bool FindApproachPoint(const FVector& Origin, const FVector& Goal, float SearchRadius, FVector& OutPoint) const;

	/** 手动重建影响图（一般不用调，Tick 会按 RebuildInterval 自动重建）。 */
	UFUNCTION(BlueprintCallable, Category = "Influence")
	void RebuildMap();

	/** 地图是否已建好（第一次 RebuildMap 之前所有查询返回 0）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Influence")
	bool IsMapReady() const { return bMapReady; }

	/** 格子数量（X 方向）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Influence")
	int32 GetGridWidth() const { return GridWidth; }

	/** 格子数量（Y 方向）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Influence")
	int32 GetGridHeight() const { return GridHeight; }

	/** 最近一次重建耗时（毫秒），用于性能说明。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Influence")
	float GetLastRebuildMs() const { return LastRebuildMs; }

	/**
	 * 最近一次重建中「实际参与泛洪」的威胁源数量。
	 *
	 * 注意与 ThreatSources.Num() 的区别：
	 *   数组为空时会自动用 0 号玩家兜底，那份坐标只進局部变量，
	 *   所以数组长度可能是 0，而实际泛洪源是 1。
	 *   调试显示应该看这个值，否则会误以为影响图没在算。
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Influence")
	int32 GetActiveThreatSourceCount() const { return ActiveThreatSourceCount; }

	/** 最近一次重建中实际参与泛洪的友方源数量。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Influence")
	int32 GetActiveSafetySourceCount() const { return ActiveSafetySourceCount; }

	// ============================================================
	// 静态查找：让 AI 不用自己写 TActorIterator
	// ============================================================

	/** 在当前世界里找第一个 ATacticalInfluenceMap。找不到返回 nullptr。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Influence", meta = (WorldContext = "WorldContextObject"))
	static ATacticalInfluenceMap* FindInfluenceMap(const UObject* WorldContextObject);

private:
	/**
	 * 一维格子数组，索引 = X + Y * GridWidth。
	 * Transient：这些是运行时数据，不需要序列化进关卡（否则关卡文件会暴增）。
	 */
	UPROPERTY(Transient)
	TArray<FTacticalCell> Cells;

	/** 网格尺寸。 */
	int32 GridWidth = 0;
	int32 GridHeight = 0;

	/** 地图左下角（最小 X / 最小 Y）的世界坐标。 */
	FVector MapOrigin = FVector::ZeroVector;

	/** 是否已完成首次构建。 */
	bool bMapReady = false;

	/**
	 * 网格是否已建好。
	 *
	 * 为什么要这个标记：BuildGrid 要对每个格子做一次 NavMesh 投影，
	 * 3600 格 × 每格约 20~50 微秒 = 70~180ms。如果每次泛洪都重建网格，
	 * 每 0.5 秒卡一次，完全没法玩。
	 * 所以网格只在「首次」或「关键参数变化」时重建，泛洪每次都跑。
	 */
	bool bGridBuilt = false;

	/** 建格时用的参数快照，用于判断是否需要重建网格。 */
	float CachedCellSize = -1.0f;
	float CachedMapExtent = -1.0f;
	FVector CachedBuildLocation = FVector(MAX_flt, MAX_flt, MAX_flt);

	/** 重建计时。 */
	float RebuildTimer = 0.0f;

	/** 最近一次重建耗时（毫秒）。 */
	float LastRebuildMs = 0.0f;

	/** 最近一次重建中实际参与泛洪的源数量（含玩家兜底）。 */
	int32 ActiveThreatSourceCount = 0;
	int32 ActiveSafetySourceCount = 0;

	// ---------- 内部实现 ----------

	/** ① 建格：按 CellSize 采样网格，逐格投影到 NavMesh。 */
	void BuildGrid();

	/**
	 * ② 泛洪：从源点按强度从高到低扩展。
	 * 对应源库 mapFloodDijkstra。
	 *
	 * @param Sources      源点世界坐标列表
	 * @param SourceStrength 源点初始强度
	 * @param Radius       有效半径（cm）
	 * @param OutField     输出的强度场（与 Cells 等长）
	 */
	void FloodFill(const TArray<FVector>& Sources, float SourceStrength,
		float Radius, TArray<float>& OutField) const;

	/**
	 * ③ 平滑：对强度场做 N 次 3x3 高斯卷积。
	 * 对应源库 convolve。
	 */
	void SmoothField(TArray<float>& Field, int32 Iterations) const;

	/** 世界坐标 → 格子索引。超出范围返回 INDEX_NONE。 */
	int32 WorldToIndex(const FVector& WorldLocation) const;

	/** 格子索引 → 世界坐标（格子中心）。 */
	FVector IndexToWorld(int32 Index) const;

	/** 取某格的可走邻居索引（4 邻域，可选 8 邻域）。 */
	void GetNeighbors(int32 Index, TArray<int32>& OutNeighbors) const;

	/**
	 * 画热力图。
	 * @param Lifetime 方块的存活时间（秒）。由 Tick 按重建间隔自动算，
	 *                 保证「上一轮消失前下一轮已经出现」，不会出现空档帧。
	 */
	void DrawDebugMap(float Lifetime) const;

	/** 收集 ThreatSources / SafetySources 的有效世界坐标。 */
	void CollectSourceLocations(TArray<FVector>& OutThreat, TArray<FVector>& OutSafety) const;

	/** 源点和目标点之间的水平方向规范化（用于 GetApproachPoint 打分）。 */
	static FVector SafeDirection2D(const FVector& From, const FVector& To);
};
