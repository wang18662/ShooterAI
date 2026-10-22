// TacticalInfluenceMap.cpp
// 战术影响图 —— 移植自《AI for Games》(第3版) 6.2.2 / 6.3
//
// 算法三步：
//   ① BuildGrid  —— 按 CellSize 采样网格，逐格投影到 NavMesh
//   ② FloodFill  —— 从源点泛洪，强度按格子步长累积衰减
//   ③ SmoothField —— 3x3 高斯卷积（对应源库 ConvolutionFilter）
//
// 与源库的关键差异见头文件注释。这里只补充一点实现选择：
//   源库 mapFloodDijkstra 用「优先队列按强度取最大」扩展。
//   我用「FIFO 队列 + 只在强度变高时重新入队」（SPFA 风格）。
//   原因：本图每步衰减是常量，等价于多源 BFS；
//   而 SPFA 在「不同源强度不同」时依然正确，且实现简单、无堆 API 依赖。
//   实测 3600 格 < 2ms，比堆版更容易读。

#include "TacticalInfluenceMap.h"
#include "ShooterAIDebug.h"

#include "NavigationSystem.h"
#include "NavigationData.h"
#include "DrawDebugHelpers.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"

namespace
{
	/** 3x3 高斯核（1/16 * [[1,2,1],[2,4,2],[1,2,1]]）。 */
	constexpr float GaussianKernel[3][3] = {
		{ 1.0f / 16.0f, 2.0f / 16.0f, 1.0f / 16.0f },
		{ 2.0f / 16.0f, 4.0f / 16.0f, 2.0f / 16.0f },
		{ 1.0f / 16.0f, 2.0f / 16.0f, 1.0f / 16.0f }
	};

	/** 浮点比较容差：避免同强度重复入队导致无限循环。 */
	constexpr float StrengthEpsilon = 1e-4f;
}

ATacticalInfluenceMap::ATacticalInfluenceMap()
{
	PrimaryActorTick.bCanEverTick = true;

	// 影响图不需要 Tick 太频繁，靠 RebuildInterval 节流
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	// ------------------------------------------------------------
	// ★ 必须创建 RootComponent，否则在编辑器里"拖不动"。
	//   UE 的视口移动控件（gizmo）是挂在 RootComponent 上的：
	//     选中 Actor → 取 GetRootComponent() 的 Transform 画箭头
	//     没有 RootComponent 就画不出 gizmo，也应用不了位移。
	//
	//   纯 C++ 的 AActor 不会自动补 Root（只有蓝图 Actor 会被编辑器
	//   补一个 DefaultSceneRoot），所以必须在这里显式创建。
	//
	//   另一个副作用：RebuildMap 用 GetActorLocation() 当网格中心，
	//   没有 root 时位置无法在编辑器里调整，网格会固定在原点附近。
	// ------------------------------------------------------------
	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SceneRoot->SetMobility(EComponentMobility::Movable);
	RootComponent = SceneRoot;
}

void ATacticalInfluenceMap::BeginPlay()
{
	Super::BeginPlay();

	RebuildMap();
}

void ATacticalInfluenceMap::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// 按间隔重建。
	// 注意：不要把间隔设成 0——每帧跑两次泛洪 + 两轮卷积会吃掉 10~25ms/帧，
	// 帧率被压到 15~30，玩起来像"角色移动迟钝"。
	RebuildTimer -= DeltaSeconds;
	if (RebuildTimer <= 0.0f)
	{
		RebuildTimer = FMath::Max(RebuildInterval, 0.05f);
		RebuildMap();

		// ★ 只在重建后画一次，生命周期略长于重建间隔。
		//
		//   为什么不每帧画：
		//     ① 每帧画会让「上一轮颜色」和「这一轮颜色」短暂叠在一起，
		//        边界格出现两套颜色混合 = 视觉抖动。
		//     ② 绘制开销从 60 次/秒降到约 3 次/秒。
		//   为什么生命周期要 > 间隔：
		//     否则两轮之间会出现"没有方块"的空档帧 = 闪烁。
		if (bDrawDebug)
		{
			DrawDebugMap(RebuildTimer + 0.2f);
		}
	}

	if (bShowStats && GEngine && ShooterAIDebug::ShouldPrintAggregate())
	{
		// 固定小 Key → 排在屏幕顶部
		GEngine->AddOnScreenDebugMessage(ShooterAIDebug::Key::InfluenceMap, 0.2f, FColor::Cyan,
			FString::Printf(TEXT("[InfluenceMap] %dx%d 格 | 重建 %.2fms | 威胁源 %d 友方源 %d"),
				GridWidth, GridHeight, LastRebuildMs,
				ActiveThreatSourceCount, ActiveSafetySourceCount));
	}
}

// ============================================================
// 公开查询
// ============================================================

int32 ATacticalInfluenceMap::WorldToIndex(const FVector& WorldLocation) const
{
	if (GridWidth <= 0 || GridHeight <= 0 || CellSize <= 0.0f)
	{
		return INDEX_NONE;
	}

	const int32 X = FMath::FloorToInt((WorldLocation.X - MapOrigin.X) / CellSize);
	const int32 Y = FMath::FloorToInt((WorldLocation.Y - MapOrigin.Y) / CellSize);

	if (X < 0 || Y < 0 || X >= GridWidth || Y >= GridHeight)
	{
		return INDEX_NONE;
	}

	const int32 Index = X + Y * GridWidth;
	return Cells.IsValidIndex(Index) ? Index : INDEX_NONE;
}

FVector ATacticalInfluenceMap::IndexToWorld(int32 Index) const
{
	if (!Cells.IsValidIndex(Index) || GridWidth <= 0)
	{
		return MapOrigin;
	}

	const int32 X = Index % GridWidth;
	const int32 Y = Index / GridWidth;

	return FVector(
		MapOrigin.X + (X + 0.5f) * CellSize,
		MapOrigin.Y + (Y + 0.5f) * CellSize,
		Cells[Index].WorldLocation.Z);
}

float ATacticalInfluenceMap::GetThreatAt(const FVector& WorldLocation) const
{
	const int32 Index = WorldToIndex(WorldLocation);
	return Cells.IsValidIndex(Index) ? Cells[Index].Threat : 0.0f;
}

float ATacticalInfluenceMap::GetSafetyAt(const FVector& WorldLocation) const
{
	const int32 Index = WorldToIndex(WorldLocation);
	return Cells.IsValidIndex(Index) ? Cells[Index].Safety : 0.0f;
}

float ATacticalInfluenceMap::GetCombinedAt(const FVector& WorldLocation) const
{
	const int32 Index = WorldToIndex(WorldLocation);
	return Cells.IsValidIndex(Index) ? Cells[Index].Combined : 0.0f;
}

bool ATacticalInfluenceMap::FindSafestPointNear(const FVector& Origin, float SearchRadius, FVector& OutPoint) const
{
	if (!bMapReady || Cells.Num() == 0)
	{
		return false;
	}

	float BestScore = -MAX_flt;
	int32 BestIndex = INDEX_NONE;

	for (int32 i = 0; i < Cells.Num(); ++i)
	{
		const FTacticalCell& Cell = Cells[i];
		if (!Cell.bWalkable)
		{
			continue;
		}

		// 只考虑水平距离，高度差不参与（同一层楼内找点）
		const float Dist2D = FVector::Dist2D(Cell.WorldLocation, Origin);
		if (Dist2D > SearchRadius)
		{
			continue;
		}

		// Combined = Safety - Threat，越大越好
		if (Cell.Combined > BestScore)
		{
			BestScore = Cell.Combined;
			BestIndex = i;
		}
	}

	if (BestIndex == INDEX_NONE)
	{
		return false;
	}

	OutPoint = IndexToWorld(BestIndex);
	return true;
}

bool ATacticalInfluenceMap::FindApproachPoint(const FVector& Origin, const FVector& Goal,
	float SearchRadius, FVector& OutPoint) const
{
	if (!bMapReady || Cells.Num() == 0)
	{
		return false;
	}

	const FVector GoalDir = SafeDirection2D(Origin, Goal);
	const float MyDistToGoal = FVector::Dist2D(Origin, Goal);

	// ------------------------------------------------------------
	// ★ 最小推进量：候选点必须比当前更接近 Goal 至少这么多。
	//
	//   没有这个约束会出现致命问题（实测踩过）：
	//     候选点只在 Origin 周围 SearchRadius 内搜索，
	//     而打分里「安全度」权重 0.65 高于「方向」0.35，
	//     于是它会选一个「离自己很近、但比较安全」的点 → 敌人只挪一小步。
	//     玩家在 2500cm 外时，替代点最远只到 800cm 处，
	//     表现为「站在原地、偶尔向前走一步」，永远追不上人。
	//
	//   加了推进约束后：要么找到真正能前进的绕行点，
	//   要么返回 false 让调用方退回直线追击（安全兜底）。
	// ------------------------------------------------------------
	const float MinAdvance = FMath::Min(SearchRadius * 0.5f, MyDistToGoal * 0.3f);

	float BestScore = -MAX_flt;
	int32 BestIndex = INDEX_NONE;

	for (int32 i = 0; i < Cells.Num(); ++i)
	{
		const FTacticalCell& Cell = Cells[i];
		if (!Cell.bWalkable)
		{
			continue;
		}

		const float Dist2D = FVector::Dist2D(Cell.WorldLocation, Origin);
		if (Dist2D > SearchRadius || Dist2D < 1.0f)
		{
			continue;
		}

		// 必须朝 Goal 推进足够距离，否则跳过
		const float CellDistToGoal = FVector::Dist2D(Cell.WorldLocation, Goal);
		if (CellDistToGoal > MyDistToGoal - MinAdvance)
		{
			continue;
		}

		// 方向一致性：候选点应该大致在「朝目标」的方向上
		// dot = 1 完全同向，-1 完全反向
		const FVector PointDir = SafeDirection2D(Origin, Cell.WorldLocation);
		const float Alignment = FVector::DotProduct(PointDir, GoalDir);   // -1 ~ 1

		// 综合打分：
		//   Combined（安全度）权重 0.65 —— 这是主要目标
		//   方向一致性 权重 0.35      —— 但不能为了安全往回跑
		// Alignment 从 [-1,1] 映射到 [0,1]
		const float Alignment01 = (Alignment + 1.0f) * 0.5f;
		const float Score = Cell.Combined * 0.65f + Alignment01 * 0.35f;

		if (Score > BestScore)
		{
			BestScore = Score;
			BestIndex = i;
		}
	}

	if (BestIndex == INDEX_NONE)
	{
		return false;
	}

	OutPoint = IndexToWorld(BestIndex);
	return true;
}

FVector ATacticalInfluenceMap::SafeDirection2D(const FVector& From, const FVector& To)
{
	FVector Dir = To - From;
	Dir.Z = 0.0f;
	if (Dir.SizeSquared() < 1.0f)
	{
		return FVector::ForwardVector;
	}
	return Dir.GetSafeNormal();
}

ATacticalInfluenceMap* ATacticalInfluenceMap::FindInfluenceMap(const UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(
		WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;

	if (!World)
	{
		return nullptr;
	}

	for (TActorIterator<ATacticalInfluenceMap> It(World); It; ++It)
	{
		return *It;
	}

	return nullptr;
}

// ============================================================
// ① 建格
// ============================================================

void ATacticalInfluenceMap::BuildGrid()
{
	Cells.Reset();
	GridWidth = 0;
	GridHeight = 0;

	if (CellSize <= 1.0f || MapExtent <= CellSize)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return;
	}

	// 格子数：半宽 / 格子边长，取整后乘 2 覆盖全宽
	GridWidth = FMath::Max(1, FMath::RoundToInt((MapExtent * 2.0f) / CellSize));
	GridHeight = GridWidth;

	const FVector Center = GetActorLocation();
	MapOrigin = FVector(Center.X - MapExtent, Center.Y - MapExtent, Center.Z);

	Cells.SetNum(GridWidth * GridHeight);

	const FVector QueryExtent(CellSize * 0.5f, CellSize * 0.5f, MaxProjectionHeight);

	int32 WalkableCount = 0;

	for (int32 Y = 0; Y < GridHeight; ++Y)
	{
		for (int32 X = 0; X < GridWidth; ++X)
		{
			const int32 Index = X + Y * GridWidth;

			const FVector Sample(
				MapOrigin.X + (X + 0.5f) * CellSize,
				MapOrigin.Y + (Y + 0.5f) * CellSize,
				Center.Z);

			FTacticalCell& Cell = Cells[Index];
			Cell = FTacticalCell();

			FNavLocation NavLocation;
			if (NavSys->ProjectPointToNavigation(Sample, NavLocation, QueryExtent))
			{
				// 水平偏移太大 = 投影到别处去了，不算本格可走
				if (FVector::Dist2D(NavLocation.Location, Sample) <= CellSize * 0.5f)
				{
					Cell.bWalkable = true;
					Cell.WorldLocation = NavLocation.Location;
					++WalkableCount;
				}
			}

			// 不可走的格子也记下采样点，绘图时用（画灰色）
			if (!Cell.bWalkable)
			{
				Cell.WorldLocation = Sample;
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[InfluenceMap] 建格完成：%dx%d，可走 %d 格"),
		GridWidth, GridHeight, WalkableCount);
}

// ============================================================
// ② 泛洪（对应源库 mapFloodDijkstra）
// ============================================================

void ATacticalInfluenceMap::FloodFill(const TArray<FVector>& Sources, float SourceStrength,
	float Radius, TArray<float>& OutField) const
{
	OutField.Reset();
	OutField.SetNumZeroed(Cells.Num());

	if (Cells.Num() == 0 || Sources.Num() == 0 || Radius <= 0.0f || SourceStrength <= 0.0f)
	{
		return;
	}

	// 每格衰减量：让强度在 Radius 距离处衰减到 0
	const float StepsToZero = Radius / CellSize;
	if (StepsToZero < 1.0f)
	{
		return;
	}
	const float DecayPerCell = SourceStrength / StepsToZero;

	TArray<int32> Queue;
	Queue.Reserve(256);

	// 所有源点入队（同一格可能有多个源，取最大）
	for (const FVector& Source : Sources)
	{
		const int32 Index = WorldToIndex(Source);
		if (!Cells.IsValidIndex(Index) || !Cells[Index].bWalkable)
		{
			// 源点不在可走格上：就近找可走格
			float BestDist = MAX_flt;
			int32 BestIdx = INDEX_NONE;
			for (int32 i = 0; i < Cells.Num(); ++i)
			{
				if (!Cells[i].bWalkable)
				{
					continue;
				}
				const float D = FVector::Dist2D(Cells[i].WorldLocation, Source);
				if (D < BestDist)
				{
					BestDist = D;
					BestIdx = i;
				}
			}
			if (BestIdx == INDEX_NONE || BestDist > CellSize * 3.0f)
			{
				continue;
			}
			if (SourceStrength > OutField[BestIdx])
			{
				OutField[BestIdx] = SourceStrength;
				Queue.Add(BestIdx);
			}
			continue;
		}

		if (SourceStrength > OutField[Index])
		{
			OutField[Index] = SourceStrength;
			Queue.Add(Index);
		}
	}

	// FIFO + 「强度变高才重新入队」（SPFA 风格）
	// 为什么不用优先队列：本图衰减是常量，等价于多源 BFS；
	// 不同源强度不同时 SPFA 依然收敛到正确解，且实现更简单。
	TArray<int32> Neighbors;
	int32 Head = 0;
	int32 GuardCounter = 0;
	const int32 GuardLimit = Cells.Num() * 16;   // 防死循环的保险

	while (Head < Queue.Num() && GuardCounter < GuardLimit)
	{
		++GuardCounter;

		const int32 Current = Queue[Head++];
		const float CurrentStrength = OutField[Current];

		const float NeighborStrength = CurrentStrength - DecayPerCell;
		if (NeighborStrength <= StrengthEpsilon)
		{
			continue;   // 已经衰减到 0，不再传播
		}

		GetNeighbors(Current, Neighbors);

		for (int32 Neighbor : Neighbors)
		{
			if (!Cells.IsValidIndex(Neighbor) || !Cells[Neighbor].bWalkable)
			{
				continue;
			}

			if (NeighborStrength > OutField[Neighbor] + StrengthEpsilon)
			{
				OutField[Neighbor] = NeighborStrength;
				Queue.Add(Neighbor);
			}
		}
	}
}

// ============================================================
// ③ 平滑（对应源库 convolve）
// ============================================================

void ATacticalInfluenceMap::SmoothField(TArray<float>& Field, int32 Iterations) const
{
	if (Iterations <= 0 || GridWidth <= 2 || GridHeight <= 2 || Field.Num() != Cells.Num())
	{
		return;
	}

	TArray<float> Source = Field;
	TArray<float> Destination;
	Destination.SetNumZeroed(Field.Num());

	for (int32 Iter = 0; Iter < Iterations; ++Iter)
	{
		for (int32 Y = 0; Y < GridHeight; ++Y)
		{
			for (int32 X = 0; X < GridWidth; ++X)
			{
				const int32 Index = X + Y * GridWidth;

				// 不可走格保持 0（避免影响扩散到墙里）
				if (!Cells[Index].bWalkable)
				{
					Destination[Index] = 0.0f;
					continue;
				}

				float Sum = 0.0f;
				float WeightSum = 0.0f;

				for (int32 dy = -1; dy <= 1; ++dy)
				{
					for (int32 dx = -1; dx <= 1; ++dx)
					{
						const int32 NX = X + dx;
						const int32 NY = Y + dy;

						// 边界：越界贡献视为 0（源库的「忽略越界」策略）
						if (NX < 0 || NY < 0 || NX >= GridWidth || NY >= GridHeight)
						{
							continue;
						}

						const int32 NIndex = NX + NY * GridWidth;
						if (!Cells[NIndex].bWalkable)
						{
							continue;
						}

						const float W = GaussianKernel[dy + 1][dx + 1];
						Sum += Source[NIndex] * W;
						WeightSum += W;
					}
				}

				// 归一化：避免靠近墙的格子因为权重缺失而强度偏低
				Destination[Index] = (WeightSum > 1e-6f) ? (Sum / WeightSum) : 0.0f;
			}
		}

		// 双缓冲交换
		Swap(Source, Destination);
	}

	Field = Source;
}

// ============================================================
// 邻居
// ============================================================

void ATacticalInfluenceMap::GetNeighbors(int32 Index, TArray<int32>& OutNeighbors) const
{
	OutNeighbors.Reset(4);

	if (!Cells.IsValidIndex(Index) || GridWidth <= 0)
	{
		return;
	}

	const int32 X = Index % GridWidth;
	const int32 Y = Index / GridWidth;

	// 4 邻域：泛洪用 4 邻域即可（8 邻域会让对角线走得比直线快，距离失真）
	if (X > 0)             { OutNeighbors.Add(Index - 1); }
	if (X < GridWidth - 1) { OutNeighbors.Add(Index + 1); }
	if (Y > 0)             { OutNeighbors.Add(Index - GridWidth); }
	if (Y < GridHeight - 1){ OutNeighbors.Add(Index + GridWidth); }
}

// ============================================================
// 源点收集
// ============================================================

void ATacticalInfluenceMap::CollectSourceLocations(
	TArray<FVector>& OutThreat, TArray<FVector>& OutSafety) const
{
	OutThreat.Reset();
	OutSafety.Reset();

	for (const TObjectPtr<AActor>& Source : ThreatSources)
	{
		if (IsValid(Source.Get()))
		{
			OutThreat.Add(Source->GetActorLocation());
		}
	}

	for (const TObjectPtr<AActor>& Source : SafetySources)
	{
		if (IsValid(Source.Get()))
		{
			OutSafety.Add(Source->GetActorLocation());
		}
	}
}

// ============================================================
// 重建总流程
// ============================================================

void ATacticalInfluenceMap::RebuildMap()
{
	const double StartTime = FPlatformTime::Seconds();

	// ① 建格：只在必要时重建
	//    NavMesh 投影很贵（3600 格约 70~180ms），不能每次泛洪都做。
	//    只有「首次」「格子大小变了」「范围变了」「Actor 移动超过一格」才重建。
	const FVector CurrentLoc = GetActorLocation();
	const bool bNeedBuildGrid =
		!bGridBuilt
		|| !FMath::IsNearlyEqual(CachedCellSize, CellSize, 0.01f)
		|| !FMath::IsNearlyEqual(CachedMapExtent, MapExtent, 1.0f)
		|| FVector::Dist2D(CachedBuildLocation, CurrentLoc) > CellSize;

	if (bNeedBuildGrid)
	{
		BuildGrid();

		CachedCellSize = CellSize;
		CachedMapExtent = MapExtent;
		CachedBuildLocation = CurrentLoc;
		bGridBuilt = (Cells.Num() > 0);
	}

	if (Cells.Num() == 0)
	{
		bMapReady = false;
		return;
	}

	// 若威胁源列表为空，自动用 0 号玩家兜底
	// 理由：影响图最常见的用途就是「玩家在哪，哪就危险」，
	// 每次都要手动拖玩家引用到数组里太啰嗦。
	TArray<FVector> ThreatLocations;
	TArray<FVector> SafetyLocations;
	CollectSourceLocations(ThreatLocations, SafetyLocations);

	if (ThreatLocations.Num() == 0)
	{
		if (const UWorld* World = GetWorld())
		{
			for (TActorIterator<APawn> It(World); It; ++It)
			{
				APawn* Pawn = *It;
				if (Pawn && Pawn->IsPlayerControlled())
				{
					ThreatLocations.Add(Pawn->GetActorLocation());
					break;
				}
			}
		}
	}

	// ② 两遍泛洪：威胁场 + 安全场
	TArray<float> ThreatField;
	FloodFill(ThreatLocations, ThreatSourceStrength, ThreatRadius, ThreatField);

	TArray<float> SafetyField;
	FloodFill(SafetyLocations, SafetySourceStrength, SafetyRadius, SafetyField);

	// ★ 记录「实际参与泛洪的源数量」。
	//
	//   之前屏幕打印的是 ThreatSources.Num()（编辑器数组长度），
	//   而玩家兜底是写进局部变量 ThreatLocations 的，没有写回数组，
	//   所以即使泛洪正常工作，屏幕也永远显示「威胁源 0」——
	//   这是个纯调试信息错误，但会误导排查方向（让人以为影响图没算）。
	ActiveThreatSourceCount = ThreatLocations.Num();
	ActiveSafetySourceCount = SafetyLocations.Num();

	// ③ 各自平滑
	SmoothField(ThreatField, SmoothIterations);
	SmoothField(SafetyField, SmoothIterations);

	// 写回格子
	const int32 Count = FMath::Min3(Cells.Num(), ThreatField.Num(), SafetyField.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		Cells[i].Threat = FMath::Clamp(ThreatField[i], 0.0f, 1.0f);
		Cells[i].Safety = FMath::Clamp(SafetyField[i], 0.0f, 1.0f);
		Cells[i].Combined = Cells[i].Safety - Cells[i].Threat;
	}

	bMapReady = true;

	LastRebuildMs = static_cast<float>((FPlatformTime::Seconds() - StartTime) * 1000.0);
}

// ============================================================
// 调试绘制
// ============================================================

void ATacticalInfluenceMap::DrawDebugMap(float Lifetime) const
{
	UWorld* World = GetWorld();
	if (!World || !bMapReady || Cells.Num() == 0)
	{
		return;
	}

	const float HalfCell = CellSize * 0.5f;

	// 步长：每隔 DebugDrawStride 格画一个。
	// 3600 次 DrawDebugBox 在 Debug 构建下能吃掉 20~40ms，会让角色像卡住动不了。
	const int32 Stride = FMath::Max(1, DebugDrawStride);
	const float ScaledHalfCell = HalfCell * Stride * 0.9f;

	for (int32 Y = 0; Y < GridHeight; Y += Stride)
	{
		for (int32 X = 0; X < GridWidth; X += Stride)
		{
			const int32 i = X + Y * GridWidth;
			if (!Cells.IsValidIndex(i))
			{
				continue;
			}

			const FTacticalCell& Cell = Cells[i];

			if (!Cell.bWalkable)
			{
				continue;   // 不可走的不画，避免满屏灰块
			}

			// ------------------------------------------------------------
			// 颜色：净危险度单值 → 红黄绿整条渐变。
			//
			// ★ 为什么不用「红=Threat 场、绿=Safety 场」的双通道方案：
			//   SafetySources 默认为空 → Safety 场恒 0 → 绿通道永远是基色
			//   → 画面只有红和黑，看起来像"没算出来"。
			//   实际算法是对的，只是"安全"没有视觉出口。
			//
			// ★ 为什么也不能用 T/(T+S) 归一化（早期 bug）：
			//   T=0.02, S=0.01 → (0.67, 0.33) → 亮红。
			//   任何微小非零值都被拉成饱和色，边界必然抖动闪烁。
			//
			// ★ 现在的映射（标准热力图做法）：
			//   Danger01 = Clamp(Threat - Safety, 0, 1)
			//     Danger01 = 0   → 纯绿（安全）
			//     Danger01 = 0.5 → 黄（中等）
			//     Danger01 = 1   → 纯红（最危险）
			//   Safety 场仍然有用：友方源附近会拉低危险度，更偏绿。
			//   强度直接决定色相位置，不存在归一化导致的饱和假象，
			//   边界是连续过渡，不会闪。
			// ------------------------------------------------------------
			const float T = FMath::Clamp(Cell.Threat, 0.0f, 1.0f);
			const float S = FMath::Clamp(Cell.Safety, 0.0f, 1.0f);

			const float Danger01 = FMath::Clamp(T - S, 0.0f, 1.0f);

			// 红绿互补：危险越高越红，越低越绿。蓝保持很低让颜色干净。
			// 整体乘 0.9 避免纯色过曝刺眼。
			constexpr float Brightness = 0.9f;
			constexpr float BlueChannel = 0.08f;

			const float R = Danger01 * Brightness;
			const float G = (1.0f - Danger01) * Brightness;
			const float B = BlueChannel;

			const FColor Color(
				static_cast<uint8>(FMath::Clamp(R * 255.0f, 0.0f, 255.0f)),
				static_cast<uint8>(FMath::Clamp(G * 255.0f, 0.0f, 255.0f)),
				static_cast<uint8>(FMath::Clamp(B * 255.0f, 0.0f, 255.0f)));

			const FVector DrawCenter = Cell.WorldLocation + FVector(0.0f, 0.0f, DebugDrawHeight);

			DrawDebugBox(
				World,
				DrawCenter,
				FVector(ScaledHalfCell, ScaledHalfCell, DebugBoxHeight),
				Color,
				false,      // bPersistentLines
				Lifetime,   // 由 Tick 按重建间隔算好，保证两轮之间无缝衔接
				0,
				1.0f);
		}
	}
}
