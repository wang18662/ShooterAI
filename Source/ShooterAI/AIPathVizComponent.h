// AIPathVizComponent.h
// AI 行为可视化：给观察者"看见 AI 在想什么"的能力。
//
// ============================================================
// 为什么需要它（作品集的核心问题）
// ============================================================
//   这个项目做了八项 AI 能力，但其中大部分是**纯逻辑**：
//     预判追击、记忆搜索、群体分离、影响图选点、血量分档……
//   玩家/面试官看着屏幕，只能看到"敌人冲过来了"，
//   看不到"它为什么走这条路"、"它是不是在预判我"。
//
//   ⇒ 需要把内部决策外显成可见的图形。这一层不是"美术"，
//     是**技术演示的表达手段**。行业里几乎所有 AI 演示都有类似的可视化。
//
// ============================================================
// 三个可视化层（可分别开关）
// ============================================================
//   ① 轨迹线   —— 记录最近走过的路径
//       用途：一眼看出"斜插预判" vs "直线跟随" 的区别。
//       预判追击会画出弧线，直追会画出直线，对比极强。
//
//   ② 状态标签 —— 头顶显示当前行为档位（PURSUING / RETREAT / FLEE…）
//       用途：把"内部状态"直接贴在角色身上，
//             比翻屏幕角落的调试行直观得多。
//
//   ③ 目标点标记 —— 画出它在追的那个点（预判点/记忆点/撤退点）
//       用途：这是最有说服力的一层。
//             预判点会出现在玩家前方，能直观证明"它在算你的未来位置"。
//
// ============================================================
// 性能
// ============================================================
//   轨迹线用 TArray 存采样点，数量有上限（MaxPoints），
//   绘制用 DrawDebugLine，只在 NeedDraw 时才画。
//   默认每 0.1 秒采样一次，15 个敌人 ≈ 每秒 150 次采样，可忽略。

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AIPathVizComponent.generated.h"

class APawn;
class AAIController;
class UPursueSteeringComponent;

/**
 * AI 行为可视化组件。
 *
 * 挂到敌人身上（BP_ShooterNPC），勾上想看的层即可。
 * 渲染全部用 DrawDebug*，不需要任何材质/资产。
 */
UCLASS(ClassGroup = (GameAI), meta = (BlueprintSpawnableComponent, DisplayName = "AI Path Viz"))
class SHOOTERAI_API UAIPathVizComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAIPathVizComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	// ============================================================
	// ① 轨迹线
	// ============================================================

	/** 是否画最近走过的轨迹。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Trail")
	bool bDrawTrail = true;

	/** 轨迹最多保留多少个采样点（决定线有多长）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Trail", meta = (ClampMin = "8", ClampMax = "512"))
	int32 MaxTrailPoints = 150;

	/** 采样间隔（秒）。太密费性能，太疏线会折。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Trail", meta = (ClampMin = "0.02", ClampMax = "1.0"))
	float SampleInterval = 0.1f;

	/** 相邻采样点的最小位移（cm）。静止时不采样，避免点堆积在一处。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Trail", meta = (ClampMin = "1.0", ClampMax = "200.0"))
	float MinSampleDistance = 12.0f;

	/** 轨迹线粗细。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Trail", meta = (ClampMin = "0.5", ClampMax = "10.0"))
	float TrailThickness = 2.5f;

	/** 轨迹整体抬高（cm），避免和地面 z-fighting。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Trail", meta = (ClampMin = "0.0", ClampMax = "200.0"))
	float TrailZOffset = 8.0f;

	/** 轨迹颜色（默认青色，在灰盒场景里最显眼且不与被染色的敌人混淆）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Trail")
	FColor TrailColor = FColor(0, 220, 255);

	// ============================================================
	// ② 状态标签
	// ============================================================

	/** 是否在头顶显示当前行为档位（PURSUING / RETREAT / FLEE / IDLE…）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Label")
	bool bDrawStateLabel = true;

	/** 标签在头顶的高度（cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Label", meta = (ClampMin = "50.0", ClampMax = "1000.0"))
	float LabelHeight = 140.0f;

	// ============================================================
	// ③ 目标点标记
	// ============================================================

	/** 是否画出它当前在追的那个点（预判点 / 记忆点 / 撤退点）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Target")
	bool bDrawTargetPoint = true;

	/** 标记球半径（cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Target", meta = (ClampMin = "5.0", ClampMax = "100.0"))
	float TargetMarkerRadius = 22.0f;

	/** 是否画一条从自己指向目标点的线（能直观看出追的是哪个点）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz|Target")
	bool bDrawTargetLink = true;

	// ============================================================
	// 全局
	// ============================================================

	/**
	 * 总开关。
	 * 关掉时本组件完全不做事（连轨迹采样都停），用于正式录画面时一键静音。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Viz")
	bool bEnabled = true;

	/** 清空已记录的轨迹（比如换了目标、或想重新演示时）。 */
	UFUNCTION(BlueprintCallable, Category = "AI Viz")
	void ClearTrail();

	/**
	 * 由 StateTree Task 每帧写入当前阶段的可读名（如 HEALING / DESPERATE）。
	 *
	 * ★ 为什么需要这个"反向写入"
	 *   标签原本只读 Pursue 组件的 CurrentMode，而 HEALING、DESPERATE
	 *   这类细阶段是 Task 的运行时状态，组件并不知道。
	 *   所以由知道真相的一方（Task）主动告诉可视化组件，
	 *   比让可视化组件去猜（或反过来把阶段挪进组件）更干净。
	 *
	 *   传空串表示"回到组件默认判定"。
	 */
	UFUNCTION(BlueprintCallable, Category = "AI Viz")
	void SetDisplayPhase(const FString& PhaseText);

private:
	/** 轨迹采样点（世界坐标）。 */
	TArray<FVector> TrailPoints;

	/** 采样计时。 */
	float SampleTimer = 0.0f;

	/** 上一次采样的位置（用于判断位移是否够大）。 */
	FVector LastSampleLocation = FVector::ZeroVector;

	/** 由 Task 写入的阶段名（空 = 用组件自己的判定）。 */
	FString OverridePhaseText;

	/** 解析出来的角色 Pawn（缓存，避免每帧 Cast）。 */
	UPROPERTY(Transient)
	TObjectPtr<APawn> CachedPawn = nullptr;

	/** 缓存的 AIController。 */
	UPROPERTY(Transient)
	TObjectPtr<AAIController> CachedAIController = nullptr;

	/** 缓存的 Pursue 组件（读当前阶段/预判点）。 */
	UPROPERTY(Transient)
	TObjectPtr<UPursueSteeringComponent> CachedPursue = nullptr;

	/** 解析依赖组件。 */
	void ResolveCachedReferences();

	/** 采样一次轨迹。 */
	void SampleTrail(float DeltaTime);

	/** 画轨迹线。 */
	void DrawTrail() const;

	/** 画头顶状态标签。 */
	void DrawStateLabel() const;

	/** 画目标点标记与连线。 */
	void DrawTargetMarker() const;

	/** 取当前行为档位的可读文字（读不到返回空串）。 */
	FString GetPhaseText() const;

	/** 档位对应的颜色。 */
	static FColor GetPhaseColor(const FString& PhaseText);
};
