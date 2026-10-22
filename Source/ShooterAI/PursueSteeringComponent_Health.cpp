// ============================================================
// PursueSteeringComponent_Health.cpp
// ============================================================
//
// 【这个文件做什么】
//   敌人"受伤之后的所有状态"都在这：
//     · 血量读取      反射读蓝图里的 Current HP（属性名不写死，靠遍历找）
//     · 血量档位      低血量 / 危险血量的判定
//     · 回血道具      剩余个数、消耗
//     · 回血执行      反射写回血量
//     · 受伤谨慎      挨打后进入 5 秒"谨慎期"
//     · 逃跑提速      逃跑时改 MaxWalkSpeed
//
// 【为什么单独一个文件】
//   这里全是"有状态 + 反射访问"的东西。反射读写的失败是**静默**的
//   （属性名不对就读回 0，不报错），所以它的调试方式和纯算法完全不同：
//   纯算法可以喂假数据单独验，这里必须靠探针（GetHealthProbeLog）。
//   放在一起改起来安全 —— 改血量相关的东西时不用翻 1300 行。
//
// 【不在这里的东西】
//   · 掩体探测           → PursueSteeringComponent_Cover.cpp
//   · 追击/分离等纯算法   → PursueSteeringComponent.cpp
//   · 旧旁路 + 未接线算法 → PursueSteeringComponent_Legacy.cpp
//
// 【血量的两个"坑"（都是实测踩出来的）】
//   ① 属性名不能猜。曾连试 Current HP / CurrentHP / Health / CurrentHealth
//      三轮全失败，改成"遍历全部属性、挑名字含 hp/health 的"一次解决。
//   ② 反射写不会触发蓝图的事件驱动 UI 刷新。敌人血量玩家看不到，
//      游戏逻辑读的是实际值，所以不影响 —— 但如果以后要给敌人加血条，得注意这点。
//
// ============================================================

#include "PursueSteeringComponent.h"
#include "ShooterAIDebug.h"

#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

void UPursueSteeringComponent::HandleAnyDamage(AActor* DamagedActor, float Damage,
	const UDamageType* DamageType, AController* InstigatedBy, AActor* DamageCauser)
{
	// 只有真的掉血才算受伤（Damage <= 0 是治疗/无效命中）
	if (Damage <= 0.0f)
	{
		return;
	}

	TimeSinceDamaged = 0.0f;

	UE_LOG(LogTemp, Warning, TEXT("[Pursue] %s 受伤 %.0f（来自 %s）→ 进入谨慎期 %.1fs"),
		*GetNameSafe(DamagedActor), Damage,
		*GetNameSafe(DamageCauser), WoundedCautionDuration);

	// 屏幕即时反馈：让"受伤是否被接收到"一眼可验，不用去 Output Log 里翻。
	// 这是低频事件（挨一枪才触发一次，2 秒自动消失），
	// 所以放在「聚合/关键」级别（>=1）就显示，用固定小 Key 排在上方。
	if (GEngine && DamagedActor && ShooterAIDebug::ShouldPrintAggregate())
	{
		GEngine->AddOnScreenDebugMessage(ShooterAIDebug::Key::Wounded, 2.0f, FColor::Red,
			FString::Printf(TEXT("[Wounded] %s 受伤 %.0f → 谨慎 %.1fs"),
				*GetNameSafe(DamagedActor), Damage, WoundedCautionDuration));
	}
}

bool UPursueSteeringComponent::IsWounded() const
{
	if (!bUseWoundedCaution || WoundedCautionDuration <= 0.0f)
	{
		return false;
	}
	return TimeSinceDamaged < WoundedCautionDuration;
}

float UPursueSteeringComponent::GetTimeSinceDamaged() const
{
	return TimeSinceDamaged;
}

// ============================================================
// ⑥ 受伤谨慎 + ⑤·a 血量读取与档位判定
//    （受伤谨慎排前面，因为下面读血量时会用到 TimeSinceDamaged 的语义）
// ============================================================

float UPursueSteeringComponent::GetHealthPercent() const
{
	if (!bUseHealthBehavior)
	{
		return 1.0f;
	}

	const AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return 1.0f;
	}

	// 第一次读时按名字找属性并缓存。
	// 蓝图里的变量名可能带空格（"Current HP"）也可能驼峰（"CurrentHP"），两种都试；
	// 值可能是 float 也可能是 int，都要支持。
	if (!bHealthPropertySearched)
	{
		bHealthPropertySearched = true;

		// ------------------------------------------------------------
		// ★ 诊断优先：先遍历一遍所有属性，把名字里含 HP / Health 的都记下来。
		//
		//   为什么不能只靠"猜几个名字"：
		//     蓝图变量的内部 FName 和编辑器显示名可能不一致
		//     （显示 "Current HP"，内部可能是 CurrentHP / Current_HP / HP…），
		//     猜名字猜不中就永远读不到，而且没有任何提示。
		//   遍历一次就知道真实名字和类型，一次排查到底。
		// ------------------------------------------------------------
		for (TFieldIterator<FProperty> It(OwnerActor->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop)
			{
				continue;
			}

			const FString PropName = Prop->GetName();
			const FString Lower = PropName.ToLower();

			// 只关心"像血量"的属性，排除无关的（如 HealthUI、MaxHealth 之类会在下面判断）
			if (!Lower.Contains(TEXT("hp")) && !Lower.Contains(TEXT("health")))
			{
				continue;
			}

			// 记录诊断串，供屏幕显示
			FString ValueStr = TEXT("?");
			if (const FFloatProperty* AsFloat = CastField<FFloatProperty>(Prop))
			{
				ValueStr = FString::Printf(TEXT("%.1f"), AsFloat->GetPropertyValue_InContainer(OwnerActor));
			}
			else if (const FIntProperty* AsInt = CastField<FIntProperty>(Prop))
			{
				ValueStr = FString::Printf(TEXT("%d"), AsInt->GetPropertyValue_InContainer(OwnerActor));
			}
			else if (const FDoubleProperty* AsDouble = CastField<FDoubleProperty>(Prop))
			{
				ValueStr = FString::Printf(TEXT("%.1f"), AsDouble->GetPropertyValue_InContainer(OwnerActor));
			}

			HealthProbeLog += FString::Printf(TEXT("%s(%s)=%s "),
				*PropName, *Prop->GetCPPType(), *ValueStr);

			// 挑一个作为"当前血量"：
			//   优先名字含 Current 的（Current HP / CurrentHP），
			//   其次任意含 HP/Health 且不是 Max/UI 的。
			const bool bIsMax = Lower.Contains(TEXT("max"));
			const bool bIsUI = Lower.Contains(TEXT("ui")) || Lower.Contains(TEXT("hud"));
			if (bIsMax || bIsUI)
			{
				continue;
			}

			if (FFloatProperty* AsFloat = CastField<FFloatProperty>(Prop))
			{
				CachedHealthProperty = AsFloat;
			}
			else if (FIntProperty* AsInt = CastField<FIntProperty>(Prop))
			{
				CachedHealthIntProperty = AsInt;
			}
			else if (FDoubleProperty* AsDouble = CastField<FDoubleProperty>(Prop))
			{
				CachedHealthDoubleProperty = AsDouble;
			}

			// 名字里带 current 的最可信，直接定下来
			if (Lower.Contains(TEXT("current")))
			{
				break;
			}
		}

		if (HealthProbeLog.IsEmpty())
		{
			HealthProbeLog = TEXT("(该类上没有名字含 HP/Health 的属性)");
		}
	}

	// 读值（float / int / double 各一条路）
	float CurrentHealth = -1.0f;
	if (CachedHealthProperty)
	{
		CurrentHealth = CachedHealthProperty->GetPropertyValue_InContainer(OwnerActor);
	}
	else if (CachedHealthIntProperty)
	{
		CurrentHealth = static_cast<float>(CachedHealthIntProperty->GetPropertyValue_InContainer(OwnerActor));
	}
	else if (CachedHealthDoubleProperty)
	{
		CurrentHealth = static_cast<float>(CachedHealthDoubleProperty->GetPropertyValue_InContainer(OwnerActor));
	}
	else
	{
		// 读不到血量 → 当作满血，不影响正常行为
		return 1.0f;
	}

	return (MaxHealth > 0.0f) ? FMath::Clamp(CurrentHealth / MaxHealth, 0.0f, 1.0f) : 1.0f;
}

bool UPursueSteeringComponent::IsLowHealth() const
{
	if (!bUseHealthBehavior)
	{
		return false;
	}
	return GetHealthPercent() <= RetreatHealthRatio;
}

bool UPursueSteeringComponent::IsCriticalHealth() const
{
	if (!bUseHealthBehavior)
	{
		return false;
	}
	return GetHealthPercent() <= FleeHealthRatio;
}

void UPursueSteeringComponent::ApplyFleeSpeed(bool bFlee)
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn)
	{
		return;
	}

	UCharacterMovementComponent* Movement = OwnerPawn->FindComponentByClass<UCharacterMovementComponent>();
	if (!Movement)
	{
		return;
	}

	// 第一次调用时缓存原始移速。
	// 如果不缓存、直接 Current × 倍率，多次调用会叠加，敌人会越跑越快。
	if (CachedOriginalWalkSpeed < 0.0f)
	{
		CachedOriginalWalkSpeed = Movement->MaxWalkSpeed;
	}

	const float DesiredSpeed = bFlee
		? CachedOriginalWalkSpeed * FMath::Max(FleeSpeedMultiplier, 1.0f)
		: CachedOriginalWalkSpeed;

	// 只在确实需要变化时才写，避免每帧写一个相同的值（无害但没必要）
	if (!FMath::IsNearlyEqual(Movement->MaxWalkSpeed, DesiredSpeed, 1.0f))
	{
		Movement->MaxWalkSpeed = DesiredSpeed;
	}
}

// ============================================================
// ⑤·b 回血道具
// ============================================================

bool UPursueSteeringComponent::ConsumeHealItem()
{
	if (RemainingHealItems <= 0)
	{
		return false;
	}

	--RemainingHealItems;

	UE_LOG(LogTemp, Log, TEXT("[Heal] %s 消耗回血道具，剩余 %d"),
		*GetNameSafe(GetOwner()), RemainingHealItems);

	return true;
}

bool UPursueSteeringComponent::ApplyHeal(float Amount)
{
	if (Amount <= 0.0f)
	{
		return false;
	}

	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return false;
	}

	// 血量属性指针应该在 GetHealthPercent 里已经查好了。
	// 若还没查（比如从未调用过 GetHealthPercent），这里触发一次。
	if (!bHealthPropertySearched)
	{
		GetHealthPercent();   // 副作用：完成属性查找与缓存
	}

	const float MaxHp = FMath::Max(MaxHealth, 1.0f);

	// 按属性实际类型分别写。
	// ★ 为什么不用一个通用接口：FProperty 没有统一的 SetPropertyValue 泛型接口，
	//   必须按具体类型分派。这也是"反射写"比"反射读"更啰嗦的原因。
	if (CachedHealthProperty)
	{
		const float Current = CachedHealthProperty->GetPropertyValue_InContainer(OwnerActor);
		const float NewValue = FMath::Min(Current + Amount, MaxHp);
		CachedHealthProperty->SetPropertyValue_InContainer(OwnerActor, NewValue);
		return true;
	}

	if (CachedHealthIntProperty)
	{
		const int32 Current = CachedHealthIntProperty->GetPropertyValue_InContainer(OwnerActor);
		const int32 NewValue = FMath::Min(Current + FMath::RoundToInt(Amount), FMath::RoundToInt(MaxHp));
		CachedHealthIntProperty->SetPropertyValue_InContainer(OwnerActor, NewValue);
		return true;
	}

	if (CachedHealthDoubleProperty)
	{
		const double Current = CachedHealthDoubleProperty->GetPropertyValue_InContainer(OwnerActor);
		const double NewValue = FMath::Min(Current + static_cast<double>(Amount), static_cast<double>(MaxHp));
		CachedHealthDoubleProperty->SetPropertyValue_InContainer(OwnerActor, NewValue);
		return true;
	}

	return false;
}
