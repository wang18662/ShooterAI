// BallisticsComponent.h
// 弹道解算（Firing Solution）—— 移植自《AI for Games》(第3版) 3.5.3
// 源库: D:\c++\game AI\game AI\game AI\core\ch03_movement\FiringSolution.h
//
// ============================================================
// 要解决的问题
// ============================================================
//   模板的瞄准逻辑（ShooterWeapon.cpp:213）是：
//       FindLookAtRotation(枪口, 目标位置 + 随机偏移)
//   也就是「直接指向目标」。
//
//   但子弹是【真实抛射物】，带重力会下坠。所以：
//     远距离目标 → 子弹落点在地面 → 打不中
//     移动目标   → 子弹飞到时人已经走了 → 打不中
//
//   这不是"AI 笨"，是"没算弹道"。
//
// ============================================================
// 算法（不是"简单抬高枪口"，是逆向求解）
// ============================================================
//   抛射物位置：  p(t) = start + u·sm·t + 0.5·g·t²
//   令 p(t) = end，且约束 |u| = 1（u 是单位方向，sm 是初速标量）
//
//   消去 u 后得到关于 t² 的二次方程：
//       |g|²t⁴ - 4(g·Δ + sm²)t² + 4|Δ|² = 0      (Δ = end - start)
//
//   解出飞行时间 t 后，反解方向：
//       u = (2Δ - g·t²) / (2·sm·t)
//
//   复杂度 O(1)。两个解对应「高抛」和「平射」，取较小的 t（平射，更快命中）。
//
// ============================================================
// 源库的两处坑（都踩过，已在实现中修正）
// ============================================================
//   ① 书中伪代码把 delta 写成 start - end（"从目标返回起点"），
//      但式(3.5) 的 Δ = end - start，符号相反会导致方向完全反向。
//
//   ② 判别式 < 0 时无解 —— 表示"这个初速打不到那么远"，
//      必须能优雅降级（回退到直瞄），不能崩溃或乱打。
//
// ============================================================
// 与源库的差异（胶水层）
// ============================================================
//   1. Vector3 → FVector
//   2. std::optional<Vector3> → bool + 出参（UE 反射更友好，蓝图也能调）
//   3. 增加「移动目标提前量」：源库只解算固定点，
//      实际射击要预测目标在 t 秒后的位置，用迭代逼近（2~3 次足够收敛）
//   4. 增加「平射/高抛」选择开关：默认取较小 t（平射）

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BallisticsComponent.generated.h"

class AActor;

/** 弹道解算结果。 */
USTRUCT(BlueprintType)
struct FBallisticSolution
{
	GENERATED_BODY()

	/** 是否解算成功。false 表示这个初速打不到目标（需要更快的枪或更近的距离）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Ballistics")
	bool bValid = false;

	/** 解算出的发射方向（单位向量，世界空间）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Ballistics")
	FVector LaunchDirection = FVector::ZeroVector;

	/** 预测的飞行时间（秒）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Ballistics")
	float FlightTime = 0.0f;

	/** 解算时瞄准的目标点（移动目标经过提前量修正后的位置）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Ballistics")
	FVector AimPoint = FVector::ZeroVector;

	/** 需要的仰角（度）。0 = 平射，正 = 抬高。用于调试显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "Ballistics")
	float ElevationDegrees = 0.0f;
};

/**
 * 弹道解算组件。
 *
 * 用法：
 *   挂到开火者（玩家角色 / NPC）上，然后在蓝图里调用：
 *     · GetBallisticAimPoint(枪口位置, 目标点)  ← 最常用，直接给"该瞄哪"
 *     · SolveFixedTarget / SolveMovingTarget    ← 需要完整解时用
 *
 * ★ MuzzleVelocity 必须和武器实际子弹速度一致
 *   这个值不是"参考值"，它直接参与解算。
 *   填错了，解出来的仰角就是错的，而且错得很隐蔽（近距离看不出来）。
 *   去武器蓝图里看 Projectile 的 Initial Speed，填同一个数。
 *
 * 不需要 Tick：纯计算，调用时才算。
 */
UCLASS(ClassGroup = (GameAI), meta = (BlueprintSpawnableComponent, DisplayName = "Ballistics"))
class SHOOTERAI_API UBallisticsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBallisticsComponent();

	// ============================================================
	// 参数
	// ============================================================

	/**
	 * 子弹初速（cm/s）。
	 *
	 * ★ 必须与武器实际使用的子弹速度一致，否则解算白算。
	 *   本项目 BP_ShooterProjectile_Bullet 的 Initial Speed = 8000，与此默认值一致。
	 *
	 * ★ 注意 Max Speed：如果 Max Speed > Initial Speed，子弹会在飞行中加速，
	 *   实际飞行时间比解算的短，表现为"补偿过头"。
	 *   解算假设匀速运动，所以两者最好相等。
	 *
	 * 留 0 表示「不启用弹道解算」。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ballistics", meta = (ClampMin = "0.0", ClampMax = "50000.0"))
	float MuzzleVelocity = 8000.0f;

	/**
	 * 重力大小（cm/s²）。
	 *
	 * ★★ 这里填的是【实际生效的重力】，不是 UE 的标准重力 ★★
	 *
	 *   很多人以为填 980（UE 默认重力）就对了，其实不然：
	 *     ProjectileMovement 组件有 `Projectile Gravity Scale`，
	 *     子弹真正受到的重力 = 980 × GravityScale。
	 *
	 *   本项目的 BP_ShooterProjectile_Bullet：
	 *     Gravity Scale = 0.5  ⇒  实际重力 = 490
	 *
	 *   如果这里填 980（多算一倍），解算会把仰角抬高一倍：
	 *     近距离（下坠仅几厘米）看不出来
	 *     中距离（几十厘米）容易被当成散布
	 *     远距离（1 米以上）子弹明显从头顶飞过 ← 只有这里才能暴露
	 *
	 *   换算公式：GravityZ = 980 × ProjectileGravityScale
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ballistics", meta = (ClampMin = "0.0", ClampMax = "5000.0"))
	float GravityZ = 490.0f;

	/**
	 * 移动目标提前量的迭代次数。
	 *
	 * 为什么要迭代：解算飞行时间 t 需要知道目标位置，
	 * 而目标位置又取决于 t（它会动）。互相依赖，只能迭代逼近。
	 * 2~3 次就足够收敛（误差 < 几 cm）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ballistics", meta = (ClampMin = "0", ClampMax = "6"))
	int32 InterceptIterations = 3;

	/**
	 * 是否优先选择「平射」解（较小飞行时间）。
	 *
	 * 抛射方程有两个解：平射（快、弹道低）和高抛（慢、弹道高）。
	 * 射击游戏几乎总是要平射 —— 高抛的子弹飞行时间长，目标早跑了。
	 * 只有榴弹这类曲射武器才可能要高抛。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ballistics")
	bool bPreferFlatTrajectory = true;

	/** 解算失败时是否画调试信息（打不到时能看到）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ballistics|Debug")
	bool bDrawDebug = false;

	/**
	 * 是否在屏幕打印提前量探针。
	 *
	 * 打开后会显示两行：
	 *   [Ballistics] 输入距离=2000 目标=BP_FirstPersonCharacter_C_1 速度=600
	 *   [Ballistics] 飞行=0.25s 提前量=150cm | 到目标=2000cm | 瞄点偏移=150cm
	 *
	 * 用来诊断"子弹打在身后"这类问题：
	 *   · 速度=0        → TargetActor 没接或它确实不动
	 *   · 输入距离=10000 → 上游传进来的是射线终点（没打到人），不是玩家位置
	 *   · 提前量=0       → 速度太小被跳过
	 *   · 提前量正常但还打空 → 问题在下游（枪口位置/散布）
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ballistics|Debug")
	bool bShowLeadDebug = false;

	// ============================================================
	// 核心 API
	// ============================================================

	/**
	 * 解算「打固定点」的发射方向。
	 *
	 * @param StartLocation   枪口位置
	 * @param TargetLocation  目标点
	 * @param OutSolution     输出结果
	 * @return 是否解算成功
	 */
	UFUNCTION(BlueprintCallable, Category = "Ballistics")
	bool SolveFixedTarget(const FVector& StartLocation, const FVector& TargetLocation,
		FBallisticSolution& OutSolution) const;

	/**
	 * 解算「打移动目标」的发射方向（含提前量）。
	 *
	 * @param StartLocation  枪口位置
	 * @param TargetActor    目标 Actor（会读它的位置和速度）
	 * @param OutSolution    输出结果
	 * @return 是否解算成功
	 */
	UFUNCTION(BlueprintCallable, Category = "Ballistics")
	bool SolveMovingTarget(const FVector& StartLocation, AActor* TargetActor,
		FBallisticSolution& OutSolution) const;

	/**
	 * 只算「需要抬高多少度」——给 HUD / 调试用。
	 * 打不到时返回 0。
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ballistics")
	float GetRequiredElevation(const FVector& StartLocation, const FVector& TargetLocation) const;

	/**
	 * 【蓝图接入用】算「该瞄准哪个点」，让子弹靠重力自然落到 TargetLocation。
	 *
	 * ★ 为什么返回"点"而不是"方向"
	 *   蓝图里武器最终是 `FindLookAtRotation(枪口, 目标点)` 来决定发射方向的。
	 *   如果返回方向，还得反推成一个点，多一步且容易错。
	 *   直接返回点，蓝图替换一下 Return Node 的输入就行。
	 *
	 * ★ 实现思路
	 *   `SolveFixedTarget` 已经解出精确的发射方向 LaunchDirection。
	 *   那么"沿这个方向走 dist 远"得到的点 P = Muzzle + Dir × dist，
	 *   从枪口看向 P 的旋转，正好就是 LaunchDirection。
	 *   所以把这个 P 交给 FindLookAtRotation，等价于直接用它算出的方向。
	 *
	 * ★ 解不出来时（超出射程/初速太低）
	 *   原样返回 TargetLocation —— 退化成直瞄。
	 *   宁可打近处还能中，也不要返回一个错误的点把子弹射到天上。
	 *
	 * @param MuzzleLocation 枪口位置（不是角色位置，差几十厘米也有影响）
	 * @param TargetLocation 想打中的点（通常是玩家位置 + 一点高度偏移）
	 * @return 应该瞄准的点
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ballistics")
	FVector GetBallisticAimPoint(const FVector& MuzzleLocation, const FVector& TargetLocation) const;

	/**
	 * 【蓝图接入用】算「该瞄准哪个点」，并自动补偿目标的**移动提前量**。
	 *
	 * ★ 与 GetBallisticAimPoint 的区别（这是关键）
	 *   GetBallisticAimPoint 只解重力：瞄的是目标【现在】的位置。
	 *     目标站着不动时够用；但玩家横向跑动时会打空 ——
	 *     子弹飞 0.4 秒，人已经跑出 300cm 了。
	 *
	 *   本函数先算飞行时间，把目标位置按它的速度外推，再解重力。
	 *   ⇒ 瞄的是"子弹到达时目标会在的位置"。
	 *
	 * ★ 迭代求提前量的原因
	 *   飞行时间 t 取决于目标位置（距离），而目标位置又取决于 t（它会动）。
	 *   互相依赖，没有闭式解，只能迭代逼近（2~3 轮足够收敛）。
	 *
	 * ★ TargetActor 为空时
	 *   退化为打 TargetLocation 这个固定点（等价于 GetBallisticAimPoint）。
	 *   这样蓝图里不用写两个分支。
	 *
	 * @param MuzzleLocation 枪口位置
	 * @param TargetLocation 目标当前所在点
	 * @param TargetActor    目标 Actor（读它的速度做提前量）；可为空
	 * @return 应该瞄准的点
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ballistics")
	FVector GetLeadAimPoint(const FVector& MuzzleLocation, const FVector& TargetLocation,
		AActor* TargetActor) const;

	/** 最近一次解算的结果（只读，供调试显示）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ballistics")
	const FBallisticSolution& GetLastSolution() const { return LastSolution; }

private:
	/**
	 * 缓存最近一次结果，方便屏幕上显示。
	 *
	 * 不加 UPROPERTY、加 mutable：
	 *   ① UHT 不接受 UPROPERTY 与 mutable 组合（会编译报错）
	 *   ② 它是纯运行时缓存，不需要序列化、也不需要蓝图直接读
	 *      （外部走 GetLastSolution() 这个 BlueprintPure getter）
	 *   ③ 需要在 const 函数里写，所以必须 mutable
	 */
	mutable FBallisticSolution LastSolution;

	/**
	 * 纯数学核心：解 |g|²t⁴ - 4(g·Δ + sm²)t² + 4|Δ|² = 0。
	 *
	 * @param Delta            从起点指向终点的向量
	 * @param MuzzleVel        初速标量
	 * @param OutFlightTime    输出飞行时间
	 * @return 是否有实解
	 */
	bool SolveFlightTime(const FVector& Delta, float MuzzleVel, float& OutFlightTime) const;
};
