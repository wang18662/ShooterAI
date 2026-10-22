// StateTreeShootControlTask.h
// 让 StateTree 里的 AI「边追边打」的 Task。
//
// ============================================================
// 为什么需要这个 Task
// ============================================================
//   项目的 `ST_Shooter` 里：
//     Attack Enemy 状态 → 有 Run Parallel ST_Shooter_ShootAtTarget（但绑不上，死代码）
//     Chase 状态       → 只有 Pursue Steering，没有任何射击
//   ⇒ AI 一进入 Chase 就只追不打。
//   "开局打一下" 是它最初经过 Attack Enemy 时打的那一发。
//
//   本 Task 用「接口调用」绕过模板那个绑不上的 C++ Task：
//     EnterState → 调 IShooterControlInterface::StartShooting(Target)
//     ExitState  → 调 IShooterControlInterface::StopShooting()
//
// ============================================================
// 为什么 EnterState 调一次就够，不需要 Tick
// ============================================================
//   蓝图里的 `Start Shooting` 会把「正在射击」标志置位，
//   武器自己的半自动/全自动循环会持续开火，直到 `Stop Shooting`。
//   所以这里是「开关」而非「每帧扣扳机」—— 设 bShouldCallTick = false。
//
// ============================================================
// 为什么 bShouldStateChangeOnReselect = false
// ============================================================
//   和模板的 FStateTreeShootAtTargetTask 保持一致：
//   Task 是「持续型」的，状态被反复选中的时候不需要重新触发
//   Enter/Exit（否则会 停火→开火 抖动）。
// ============================================================

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "Engine/TimerHandle.h"
#include "StateTreeShootControlTask.generated.h"

class AActor;

/**
 * Task 实例数据（StateTree 编辑器里能看到的参数）。
 */
USTRUCT()
struct FStateTreeShootControlTaskInstanceData
{
	GENERATED_BODY()

	/**
	 * 谁开枪。
	 *
	 * 留空（默认）时会自动从 StateTree 上下文取 Pawn
	 * （Owner 是 AIController 就取它的 Pawn，Owner 是 Pawn 就直接用）。
	 * 一般不需要手动绑。
	 */
	UPROPERTY(EditAnywhere, Category = "Context")
	TObjectPtr<AActor> Shooter = nullptr;

	/**
	 * 打谁。
	 * 在 StateTree 里绑定到感知到的玩家；留空则自动找 0 号玩家。
	 */
	UPROPERTY(EditAnywhere, Category = "Context")
	TObjectPtr<AActor> Target = nullptr;

	/** 是否在屏幕打印状态（调试用）。 */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowDebug = true;

	// ============================================================
	// 自动重试（自愈）
	// ============================================================

	/**
	 * 是否周期性重新下达「开火」指令。
	 *
	 * ★ 为什么必须有这个（实测踩到的坑）
	 *   最初设计成「EnterState 调一次开火，Tick 完全关闭」，
	 *   结果出现「刚进游戏 AI 不攻击，玩家开一枪之后才恢复正常」。
	 *
	 *   原因：EnterState 只跑一次，而那一刻 NPC 可能还没准备好
	 *   （武器还没 Spawn 完、Weapon 引用为空、Target 还没锁定…）。
	 *   蓝图里的 `Start Shooting` 在这些情况下会**静默 return**，
	 *   而我又不会重试 ⇒ 永久哑火，直到状态被外部原因重新进入。
	 *   玩家开枪产生噪音 → AI 重新感知 → 状态重新进入 → 才恢复。
	 *
	 *   ⇒ 所以改成「周期性重申开火指令」：只要还在这个状态里，
	 *     每隔 ReissueInterval 再喊一次。命令是幂等的
	 *     （蓝图侧只设标志位 + 尝试开火），重复喊不会打出额外子弹。
	 */
	UPROPERTY(EditAnywhere, Category = "Behavior")
	bool bReissueFire = true;

	/** 重申开火指令的间隔（秒）。太频繁会干扰武器自身射速，0.5 左右合适。 */
	UPROPERTY(EditAnywhere, Category = "Behavior", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float ReissueInterval = 0.5f;

	/**
	 * 离开状态后延迟多久才真正停火（秒）。
	 *
	 * ★ 为什么必须延迟（实测：进出 60+ 次）
	 *   StateTree 的状态可能在极短时间内反复进出（Chase ↔ Attack Enemy 来回跳）。
	 *   如果 ExitState 立刻 StopShooting：
	 *     Enter 开火 → Exit 停火 → Enter 开火 → Exit 停火 …
	 *   每帧都被打断，子弹根本出不来，表现为「偶尔才打出一发」。
	 *
	 *   改成延迟停火（debounce）：离开后先挂一个定时器，
	 *   1.5 秒内若重新进入就取消定时器（射击不中断）；
	 *   只有真的离开超过 1.5 秒才停火。
	 *
	 *   这是 UI/事件处理里常用的 debounce 手法，
	 *   用在「状态可能抖动」的行为控制上同样成立。
	 */
	UPROPERTY(EditAnywhere, Category = "Behavior", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float StopFireDelay = 1.5f;

	// ---------- 运行时状态（不要在编辑器里改） ----------

	/** Tick 计数（探针）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	int32 TickCount = 0;

	/** 重申计时器。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	float ReissueTimer = 0.0f;

	/** 重申次数（调试判读用）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	int32 ReissueCount = 0;

	/** 最近一次实际调用的蓝图函数名（调试判读用，能看到走的是哪种命名）。 */
	UPROPERTY(VisibleAnywhere, Category = "Runtime")
	FString LastUsedFunctionName;

	/**
	 * 待定的「延迟停火」定时器。
	 *
	 * 每敌一份（InstanceData 是 per-instance 的），
	 * 重新进入状态时用它取消上一次的停火安排 —— 这是 debounce 的关键：
	 * 没有句柄就取消不掉，1.5 秒后照样会停火，抖动依然会打断射击。
	 */
	FTimerHandle PendingStopTimer;
};

/**
 * 通过接口控制 AI 射击的 StateTree Task。
 *
 * 用法：
 *   在 ST_Shooter 的 Chase 状态里，和 Pursue Steering 并列添加本 Task，
 *   Target 绑定到玩家即可。AI 就会一边追一边打。
 */
USTRUCT(meta = (DisplayName = "Shoot Control (Interface)", Category = "ShooterAI"))
struct SHOOTERAI_API FStateTreeShootControlTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FStateTreeShootControlTaskInstanceData;

	FStateTreeShootControlTask();

	virtual const UStruct* GetInstanceDataType() const override
	{
		return FInstanceDataType::StaticStruct();
	}

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;

	/** 周期性重申开火指令（见 bReissueFire 的注释）。 */
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context,
		const float DeltaTime) const override;

	virtual void ExitState(FStateTreeExecutionContext& Context,
		const FStateTreeTransitionResult& Transition) const override;

#if WITH_EDITOR
	virtual FText GetDescription(const FGuid& ID, FStateTreeDataView InstanceDataView,
		const IStateTreeBindingLookup& BindingLookup,
		EStateTreeNodeFormatting Formatting = EStateTreeNodeFormatting::Text) const override;
#endif
};
