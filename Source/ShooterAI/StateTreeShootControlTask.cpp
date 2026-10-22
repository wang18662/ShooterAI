// StateTreeShootControlTask.cpp
//
// ============================================================
// 架构：只走反射调蓝图函数，不用接口
// ============================================================
//   曾经做过「原生接口」方案（IShooterControlInterface），
//   现在**已彻底移除**，原因是一次代价很大的教训：
//
//     接口函数从 StartShooting 改名成 AIStartShooting 之后，
//     蓝图里原来连好的 Event 变成孤儿节点，新的 Event 没人实现。
//     但 `ImplementsInterface()` 仍然返回 true（接口还挂在类上），
//     于是 Execute_AIStartShooting 落到 **UHT 生成的空实现**，
//     静默无事发生 —— 而代码判定"成功"，永远不会回退到反射路径。
//
//     ⇒ 之后所有修复（重试、探针、延迟停火）都在一条死通道上做优化。
//
//   结论：**「接口存在」不等于「接口被实现」**。
//   而 `Start Shooting` 本来就是蓝图里已存在的 UFunction，
//   `FindFunction` 必然能找到 —— 反射路径更直接、零配置、
//   且只有一种失败模式（找不到函数），不会静默。
//
// ============================================================
// 架构避坑：不和 BP_ShooterNPC 自己的 Tick 抢驱动
// ============================================================
//   BP_ShooterNPC 内部有 Tick 事件（K2Node_Event_DeltaSeconds）＋
//   `AI Shooting` / `Is Shooting` / `On Semi Weapon Refire` 等痕迹，
//   说明它有自己的一套 Tick 驱动射击。
//   本 Task 的定位因此收紧为：
//     · EnterState 追一次「开火」命令（把标志置位）
//     · ExitState 延迟停火（debounce，防状态抖动打断）
//     · 不逐帧强行扣扳机，避免和它自己的 Tick 打架
//
// ============================================================
// 关于「刚进游戏不攻击，玩家开枪后才恢复」
// ============================================================
//   根因是 EnterState 只跑一次，而那一刻 NPC 的感知还没锁定目标 /
//   武器还没 Spawn 完，蓝图会静默 return。
//   所以保留「周期性重申开火」（bReissueFire），等条件就绪自动补上。

#include "StateTreeShootControlTask.h"

#include "StateTreeExecutionContext.h"
#include "ShooterAIDebug.h"
#include "PursueSteeringComponent.h"

#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UnrealType.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "CollisionQueryParams.h"

// ============================================================
// 蓝图函数名候选
// ============================================================
//
// 蓝图里函数名带空格（"Start Shooting"）还是驼峰（"StartShooting"），
// 取决于当初怎么命名，二进制里两种痕迹都存在，所以两种都试，不赌。
static const TCHAR* const GStartFunctionNames[] = {
	TEXT("Start Shooting"),
	TEXT("StartShooting")
};

static const TCHAR* const GStopFunctionNames[] = {
	TEXT("Stop Shooting"),
	TEXT("StopShooting")
};

/**
 * 从 Shooter 眼睛到 Target 眼睛打一条射线，判断有没有遮挡。
 *
 * ★ 这个函数治的是「隔着墙开火」
 *   ShootControl 只负责调蓝图的开火/停火，本身不看视线。
 *   于是敌人躲到掩体后面、或者玩家绕到墙另一侧之后，
 *   它依然在开火 —— 表现为「穿墙打我」，把回血系统的意义全部抵消。
 *   真机上是否掉血取决于子弹有没有物理碰撞，但**开火动作本身**就已经错了：
 *   玩家看到的是"明明隔着一堵墙，它还在突突我"。
 *
 * 通道用 ECC_Visibility（与 PursueTask 的 CanSeeTarget 保持一致），
 * 这样"能看见"和"能开火"是同一个判据，不会出现
 * "它说看不见我，却还在打我"这种自相矛盾。
 */
static bool HasLineOfSight(const AActor* Shooter, const AActor* Target)
{
	if (!Shooter || !Target)
	{
		return false;
	}

	const UWorld* World = Shooter->GetWorld();
	if (!World)
	{
		return false;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ShootControlLOS), false, Shooter);
	Params.AddIgnoredActor(Target);

	const FVector Eye = Shooter->GetActorLocation() + FVector(0.0f, 0.0f, 60.0f);
	const FVector TargetEye = Target->GetActorLocation() + FVector(0.0f, 0.0f, 50.0f);

	FHitResult Hit;
	// 打不到东西 = 视线通畅
	return !World->LineTraceSingleByChannel(Hit, Eye, TargetEye, ECC_Visibility, Params);
}


namespace
{
	/** 累积进入/退出次数：判断状态是否在反复进出。 */
	int32 GEnterCount = 0;
	int32 GExitCount = 0;

	/** 把 StateTree 的 Owner 展开成真正的 Actor（Owner 可能是组件）。 */
	UObject* UnwrapOwner(const FStateTreeExecutionContext& Context)
	{
		UObject* OwnerObject = Context.GetOwner();

		if (UActorComponent* AsComponent = Cast<UActorComponent>(OwnerObject))
		{
			OwnerObject = AsComponent->GetOwner();
		}

		return OwnerObject;
	}

	/**
	 * 屏幕调试文字。
	 *
	 * Key 用固定小值（ShooterAIDebug::Key::ShootControl）：
	 * AddOnScreenDebugMessage 按 Key 升序显示，小 Key 排最上方，
	 * 不会被每个单位那些"指针派生的大 Key"挤到看不见。
	 */
	void ShowDebug(bool bEnabled, const FColor& Color, const FString& Message)
	{
		if (!bEnabled || !GEngine || !ShooterAIDebug::ShouldPrintAggregate())
		{
			return;
		}
		GEngine->AddOnScreenDebugMessage(ShooterAIDebug::Key::ShootControl, 0.5f, Color, Message);
	}

	/** 按候选名列表找一个存在的蓝图函数；找不到返回 nullptr。 */
	UFunction* FindFirstExistingFunction(const AActor* Candidate, const TCHAR* const* Names, int32 NumNames)
	{
		if (!IsValid(Candidate))
		{
			return nullptr;
		}
		for (int32 i = 0; i < NumNames; ++i)
		{
			if (UFunction* Found = Candidate->FindFunction(FName(Names[i])))
			{
				return Found;
			}
		}
		return nullptr;
	}

	/** 把一个 Actor 展开成「它自己 + 它的 Pawn（如果是 Controller）」。 */
	void AppendCandidates(AActor* InActor, TArray<AActor*>& OutCandidates)
	{
		if (!IsValid(InActor))
		{
			return;
		}

		OutCandidates.AddUnique(InActor);

		// Controller → 它控制的 Pawn
		if (const AController* AsController = Cast<AController>(InActor))
		{
			if (APawn* ControlledPawn = AsController->GetPawn())
			{
				OutCandidates.AddUnique(ControlledPawn);
			}
		}

		// Pawn → 它的 Controller（反向兜底）
		if (const APawn* AsPawn = Cast<APawn>(InActor))
		{
			if (AController* OwningController = AsPawn->GetController())
			{
				OutCandidates.AddUnique(OwningController);
			}
		}
	}

	/**
	 * 解析出真正能开火的 Actor。
	 *
	 * ★ 踩过的坑
	 *   StateTree 里新加 Task 时，Context 类参数会**自动绑定到 Context Object**。
	 *   本项目 StateTree 挂在 AIController 上，所以 `Shooter` 参数被自动绑成
	 *   `BP_ShooterAIController_C_0` —— 一个控制器，不是 NPC。
	 *   所以不能盲信绑定值，要逐候选验证「能不能开火」。
	 */
	AActor* ResolveShooter(const FStateTreeExecutionContext& Context, AActor* ExplicitShooter)
	{
		TArray<AActor*> Candidates;
		AppendCandidates(ExplicitShooter, Candidates);

		if (AActor* OwnerActor = Cast<AActor>(UnwrapOwner(Context)))
		{
			AppendCandidates(OwnerActor, Candidates);
		}

		for (AActor* Candidate : Candidates)
		{
			if (FindFirstExistingFunction(Candidate, GStartFunctionNames,
				UE_ARRAY_COUNT(GStartFunctionNames)) != nullptr)
			{
				return Candidate;
			}
		}

		return nullptr;
	}

	/** 解析失败时把候选明细拼成字符串，避免"看不到卡在哪"。 */
	FString DescribeCandidates(const FStateTreeExecutionContext& Context, AActor* ExplicitShooter)
	{
		TArray<AActor*> Candidates;
		AppendCandidates(ExplicitShooter, Candidates);
		if (AActor* OwnerActor = Cast<AActor>(UnwrapOwner(Context)))
		{
			AppendCandidates(OwnerActor, Candidates);
		}

		FString Result;
		for (AActor* Candidate : Candidates)
		{
			const bool bHasFunction =
				FindFirstExistingFunction(Candidate, GStartFunctionNames,
					UE_ARRAY_COUNT(GStartFunctionNames)) != nullptr;

			Result += FString::Printf(TEXT("[%s 有StartShooting=%s] "),
				*GetNameSafe(Candidate),
				bHasFunction ? TEXT("是") : TEXT("否"));
		}
		return Result.IsEmpty() ? TEXT("（候选为空）") : Result;
	}

	/**
	 * 直接从蓝图对象上读私有变量的值 —— 最有力的诊断手段。
	 *
	 * ★ 为什么需要它
	 *   `Start Shooting` 内部有一串守卫条件（有没有武器、有没有子弹、是否已在射击）。
	 *   光看"函数被调到了"无法区分这些情况。
	 *   反射可以把那些私有变量读出来，一次就能定位卡在哪一步。
	 */
	FString ProbeBlueprintState(const AActor* Candidate)
	{
		if (!IsValid(Candidate))
		{
			return TEXT("(无效对象)");
		}

		FString Out;

		// 布尔：是否正在射击（试多种可能的命名）
		for (const TCHAR* Name : { TEXT("IsShooting"), TEXT("bIsShooting"),
			TEXT("Is Shooting"), TEXT("bIs Shooting") })
		{
			if (const FBoolProperty* BoolProp =
				FindFProperty<FBoolProperty>(Candidate->GetClass(), Name))
			{
				Out += FString::Printf(TEXT("射击中=%s "),
					BoolProp->GetPropertyValue_InContainer(Candidate) ? TEXT("1") : TEXT("0"));
				break;
			}
		}

		// 整数：弹匣余弹
		for (const TCHAR* Name : { TEXT("CurrentBullets"), TEXT("Current Bullets") })
		{
			if (const FIntProperty* IntProp =
				FindFProperty<FIntProperty>(Candidate->GetClass(), Name))
			{
				Out += FString::Printf(TEXT("弹=%d "), IntProp->GetPropertyValue_InContainer(Candidate));
				break;
			}
		}

		// 对象：武器引用
		for (const TCHAR* Name : { TEXT("Weapon"), TEXT("CurrentWeapon"), TEXT("Current Weapon") })
		{
			if (const FObjectProperty* ObjProp =
				FindFProperty<FObjectProperty>(Candidate->GetClass(), Name))
			{
				const UObject* WeaponObj = ObjProp->GetObjectPropertyValue_InContainer(Candidate);
				Out += FString::Printf(TEXT("武器=%s "),
					WeaponObj ? TEXT("有") : TEXT("空"));
				break;
			}
		}

		// 对象：瞄准目标
		for (const TCHAR* Name : { TEXT("CurrentAimTarget"), TEXT("Current Aim Target") })
		{
			if (const FObjectProperty* ObjProp =
				FindFProperty<FObjectProperty>(Candidate->GetClass(), Name))
			{
				const UObject* AimObj = ObjProp->GetObjectPropertyValue_InContainer(Candidate);
				Out += FString::Printf(TEXT("瞄=%s "),
					AimObj ? TEXT("有") : TEXT("空"));
				break;
			}
		}

		return Out.IsEmpty() ? TEXT("(读不到已知变量)") : Out;
	}
}

// ============================================================
// 反射调用
// ============================================================
//
// ProcessEvent 要求参数缓冲区布局与 UFunction 的属性链完全一致，
// 自己拍一个结构体会越界写崩内存。
// 所以用 ParmsSize 分配 + TFieldIterator 按属性元数据定位偏移。
static void InvokeFunctionWithOptionalActorArg(AActor* TargetActor, UFunction* Function, AActor* Argument)
{
	if (!IsValid(TargetActor) || !Function)
	{
		return;
	}

	const int32 ParmsSize = Function->ParmsSize;
	if (ParmsSize <= 0)
	{
		TargetActor->ProcessEvent(Function, nullptr);
		return;
	}

	uint8* Params = static_cast<uint8*>(FMemory::Malloc(ParmsSize));
	FMemory::Memzero(Params, ParmsSize);

	if (Argument)
	{
		for (TFieldIterator<FObjectProperty> It(Function); It; ++It)
		{
			if ((It->PropertyFlags & CPF_Parm) == 0 || (It->PropertyFlags & CPF_ReturnParm))
			{
				continue;
			}

			UObject** Slot = It->ContainerPtrToValuePtr<UObject*>(Params);
			if (Slot)
			{
				*Slot = Argument;
			}
			break;   // 只填第一个 Object 入参
		}
	}

	TargetActor->ProcessEvent(Function, Params);
	FMemory::Free(Params);
}

// ============================================================
// 下达一次「开火」指令（EnterState 与 Tick 共用）
// ============================================================
static bool TryStartShooting(FStateTreeExecutionContext& Context,
	FStateTreeShootControlTaskInstanceData& Data, bool bLogToOutput, bool bShowOnScreen)
{
	// ★ 屏幕输出与日志输出**分开控制**。
	//   曾经把 ShowDebug 塞进 bVerbose 里，导致 Tick 重试完全不打屏幕，
	//   表现为"那行字出现一下就不见了"，看不到实时状态。
	const bool bScreen = Data.bShowDebug && bShowOnScreen;

	AActor* Shooter = ResolveShooter(Context, Data.Shooter);
	if (!Shooter)
	{
		const FString Candidates = DescribeCandidates(Context, Data.Shooter);

		if (bLogToOutput)
		{
			UE_LOG(LogTemp, Error,
				TEXT("[ShootControl] 找不到带 Start Shooting 函数的 Actor。候选=%s"), *Candidates);
		}

		ShowDebug(bScreen, FColor::Red,
			FString::Printf(TEXT("[ShootControl] 无可用射击者 %s"), *Candidates));
		return false;
	}

	// Target 没绑就找 0 号玩家
	AActor* Target = IsValid(Data.Target)
		? Data.Target.Get()
		: UGameplayStatics::GetPlayerPawn(Shooter, 0);

	if (!Target)
	{
		ShowDebug(bScreen, FColor::Yellow, TEXT("[ShootControl] 没有目标，暂不开火"));
		return false;
	}

	// ------------------------------------------------------------
	// ★ 这里原本有一段「血量低于 25% 就不开火」的硬编码判断，已删除。
	//
	//   它是「紫色 DESPERATE 阶段一枪不放」的真凶。
	//
	//   原逻辑：
	//     if (HealthComp->IsCriticalHealth()) { StopShooting(); return false; }
	//
	//   而 DESPERATE 的触发条件恰好就是【血量低于 25% 且没有回血道具】——
	//   也就是说，玩家把道具打光、敌人进入拼死状态的同时，
	//   这个判断就把它永久锁死在"不许开火"上。拼死冲锋变成"举着枪撞人"。
	//
	//   更糟的是它和新的停火机制【重复且冲突】：
	//     新机制的判据是 bSuppressFire（由 PursueTask 按阶段精确写入），
	//     只在「逃向掩体」「蹲着回血」两个阶段为真；
	//     而这段旧代码按血量一刀切，把"拼死反击"也一起毙了。
	//   两套开关并存时，谁更严格谁生效 —— 于是新机制形同虚设。
	//
	//   现在停火只有一个判据：组件上的 bSuppressFire。
	//   想加"某阶段不开火"，请让 PursueTask 去写那个标志，
	//   不要再在这里按血量判断 —— 血量不等于意图。
	// ------------------------------------------------------------

	// 唯一路径：反射调蓝图函数
	if (UFunction* StartFunc = FindFirstExistingFunction(Shooter, GStartFunctionNames,
		UE_ARRAY_COUNT(GStartFunctionNames)))
	{
		InvokeFunctionWithOptionalActorArg(Shooter, StartFunc, Target);
		Data.LastUsedFunctionName = StartFunc->GetName();

		ShowDebug(bScreen, FColor::Cyan,
			FString::Printf(TEXT("[ShootControl] %s  进出=%d/%d 重申=%d  %s"),
				*GetNameSafe(Shooter),
				GEnterCount, GExitCount, Data.ReissueCount,
				*ProbeBlueprintState(Shooter)));
		return true;
	}

	// 理论上 ResolveShooter 已经确认过有函数，走不到这里
	ShowDebug(bScreen, FColor::Red, TEXT("[ShootControl] 找不到 Start Shooting 函数"));
	if (bLogToOutput)
	{
		UE_LOG(LogTemp, Error, TEXT("[ShootControl] %s 找不到 Start Shooting 函数"), *GetNameSafe(Shooter));
	}
	return false;
}

// ============================================================
// Task 实现
// ============================================================

FStateTreeShootControlTask::FStateTreeShootControlTask()
{
	// 必须开 Tick：EnterState 只跑一次，而那一刻 NPC 可能还没准备好
	// （武器未 Spawn、感知未锁定目标），蓝图会静默 return。
	// 没有重试就会永久哑火 —— 这是"刚进游戏不攻击"的根因。
	bShouldCallTick = true;

	// 持续型：状态被重新选中时不重复触发 Enter/Exit
	bShouldStateChangeOnReselect = false;
}

EStateTreeRunStatus FStateTreeShootControlTask::EnterState(FStateTreeExecutionContext& Context,
	const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);

	++GEnterCount;

	// 重置本状态的运行时进度
	Data.TickCount = 0;
	Data.ReissueTimer = 0.0f;
	Data.ReissueCount = 0;

	// ★ 取消上一次安排的「延迟停火」（debounce 的关键）。
	//   状态抖动时每次重进都取消停火，射击就不会被打断。
	if (AActor* ShooterForTimer = ResolveShooter(Context, Data.Shooter))
	{
		if (UWorld* World = ShooterForTimer->GetWorld())
		{
			World->GetTimerManager().ClearTimer(Data.PendingStopTimer);
		}
	}

	TryStartShooting(Context, Data, /*bLogToOutput=*/true, /*bShowOnScreen=*/true);

	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FStateTreeShootControlTask::Tick(FStateTreeExecutionContext& Context,
	const float DeltaTime) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);

	++Data.TickCount;

	if (!Data.bReissueFire)
	{
		return EStateTreeRunStatus::Running;
	}

	Data.ReissueTimer += DeltaTime;
	if (Data.ReissueTimer < Data.ReissueInterval)
	{
		return EStateTreeRunStatus::Running;
	}
	Data.ReissueTimer = 0.0f;
	++Data.ReissueCount;

	// ------------------------------------------------------------
	// 视线门控：看不见目标就不开火
	// ------------------------------------------------------------
	//
	// ★ 为什么门控放在这里，而不是在每个 Strategy/State 里各写一遍
	//   开火指令只有这一处下发，管住这里就等于管住所有状态。
	//   放进 PursueTask 只能管到追击和撤退，管不到 Attack 状态。
	//
	// ★ 为什么必须停火，而不是"少打几发"
	//   玩家躲在掩体后仍然被打，等于掩体系统不存在。
	//   回血前要跑到掩体后面 —— 如果墙挡不住子弹，
	//   那"回血"这个设计从一开始就是假的。
	AActor* Shooter = ResolveShooter(Context, Data.Shooter);
	AActor* ShootTarget = Data.Target;
	if (!ShootTarget && Shooter)
	{
		ShootTarget = UGameplayStatics::GetPlayerPawn(Shooter, 0);
	}

	const bool bCanShoot = HasLineOfSight(Shooter, ShootTarget);

	// ------------------------------------------------------------
	// 第二道门控：逻辑停火（PursueTask 请求）
	// ------------------------------------------------------------
	//
	// ★ 与视线门控的区别（两道门控各管一件事，不能互相替代）
	//   视线门控：墙挡不挡？   —— 管线是几何
	//   这道门控：该不该打？   —— 按行为阶段决定
	//
	//   逃跑的起因就是"被玩家看见了"，视线门控在这里永远放行；
	//   而逃跑的语义是"脱离战斗去疗伤"，这时候背对着人跑还命中，
	//   既假，又让玩家"压上去打断回血"这个玩法失去收益。
	//
	// ★ 数据来源是 Pawn 上的 Pursue Steering 组件
	//   PursueTask 每帧写入（进入时置 true、正常阶段置 false、ExitState 清零），
	//   所以这里读到的一定是本帧的最新值，不会读到上一帧的残留。
	//
	// ★ 为什么要同时试 Shooter 和它的 Pawn
	//   ResolveShooter 返回的是"第一个有 Start Shooting 函数的 Actor"，
	//   它可能是 Pawn，也可能是 Controller（本项目的 StateTree 挂在 AIController 上）。
	//   而 Pursue Steering 组件挂在 Pawn 身上 —— 只查 Shooter 的话，
	//   一旦拿到的是 Controller 就永远读不到停火标志，静默失效。
	bool bSuppressed = false;
	FString SuppressReason;
	if (Shooter)
	{
		const UPursueSteeringComponent* PursueComp =
			Shooter->FindComponentByClass<UPursueSteeringComponent>();

		if (!PursueComp)
		{
			if (const AController* AsController = Cast<AController>(Shooter))
			{
				if (const APawn* ControlledPawn = AsController->GetPawn())
				{
					PursueComp = ControlledPawn->FindComponentByClass<UPursueSteeringComponent>();
				}
			}
		}

		if (PursueComp)
		{
			bSuppressed = PursueComp->bSuppressFire;
			SuppressReason = PursueComp->FireSuppressedReason;
		}
	}

	if (!bCanShoot || bSuppressed)
	{
		// 无视线 / 被要求停火 → 显式停火
		// （蓝图侧的开火状态是持久的，不叫停会一直打）
		if (Shooter)
		{
			if (UFunction* StopFunc = FindFirstExistingFunction(Shooter,
				GStopFunctionNames, UE_ARRAY_COUNT(GStopFunctionNames)))
			{
				InvokeFunctionWithOptionalActorArg(Shooter, StopFunc, nullptr);
			}
		}

		ShowDebug(Data.bShowDebug,
			bSuppressed ? FColor(180, 120, 255) : FColor::Silver,
			bSuppressed
				? FString::Printf(TEXT("[ShootControl] %s 停火（%s）"),
					*GetNameSafe(Shooter), *SuppressReason)
				: FString::Printf(TEXT("[ShootControl] %s 无视线→停火 目标=%s"),
					*GetNameSafe(Shooter), *GetNameSafe(ShootTarget)));

		return EStateTreeRunStatus::Running;
	}

	// 重申开火指令（幂等：蓝图侧只设标志 + 尝试开火，受武器 RefireRate 限制）。
	// 屏幕输出必须保留（同 Key 覆盖刷新，不刷屏），否则看不到实时状态。
	TryStartShooting(Context, Data, /*bLogToOutput=*/false, /*bShowOnScreen=*/true);

	return EStateTreeRunStatus::Running;
}

void FStateTreeShootControlTask::ExitState(FStateTreeExecutionContext& Context,
	const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);

	++GExitCount;

	AActor* Shooter = ResolveShooter(Context, Data.Shooter);
	if (!Shooter)
	{
		return;
	}

	// ★ 延迟停火（debounce），不立刻 StopShooting。
	//
	//   实测过 Chase 状态在极短时间内进出几十次。
	//   如果这里立刻停火，就会变成：
	//     Enter 开火 → Exit 停火 → Enter 开火 → Exit 停火 …
	//   每帧被打断，子弹根本出不来。
	//
	//   改成挂定时器：期间若重新 EnterState 会 ClearTimer 取消，
	//   只有真的离开超过 StopFireDelay 才停火。
	TWeakObjectPtr<AActor> WeakShooter(Shooter);
	const float Delay = FMath::Max(Data.StopFireDelay, 0.01f);

	if (UWorld* World = Shooter->GetWorld())
	{
		FTimerDelegate Delegate;
		Delegate.BindLambda([WeakShooter]()
		{
			AActor* StillShooter = WeakShooter.Get();
			if (!IsValid(StillShooter))
			{
				return;
			}

			if (UFunction* StopFunc = FindFirstExistingFunction(StillShooter,
				GStopFunctionNames, UE_ARRAY_COUNT(GStopFunctionNames)))
			{
				InvokeFunctionWithOptionalActorArg(StillShooter, StopFunc, nullptr);
			}
		});

		// 句柄存在 InstanceData 里，下次 EnterState 能把它取消掉
		World->GetTimerManager().SetTimer(Data.PendingStopTimer, Delegate, Delay, false);
	}

	ShowDebug(Data.bShowDebug, FColor::Silver,
		FString::Printf(TEXT("[ShootControl] Exit %s  %.1fs后停火  进出=%d/%d"),
			*GetNameSafe(Shooter), Delay, GEnterCount, GExitCount));
}

#if WITH_EDITOR
FText FStateTreeShootControlTask::GetDescription(const FGuid& ID,
	FStateTreeDataView InstanceDataView, const IStateTreeBindingLookup& BindingLookup,
	EStateTreeNodeFormatting Formatting) const
{
	return FText::FromString(TEXT("<b>Shoot Control</b>（反射调 Start Shooting，带重试与延迟停火）"));
}
#endif
