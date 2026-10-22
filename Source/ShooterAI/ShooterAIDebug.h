// ShooterAIDebug.h
// 屏幕调试输出的集中开关。
//
// ============================================================
// 为什么需要这个文件
// ============================================================
//   到后期项目里同时有 5 个调试源在往屏幕上打字：
//     · TacticalInfluenceMap   —— 1 行（聚合）
//     · AILodSubsystem         —— 1 行（聚合）
//     · AILodComponent         —— **每个敌人 1 行**
//     · PursueSteeringComponent—— **每个敌人 1~2 行**
//     · StateTreePursueTask    —— 每个敌人 1 行
//     · StateTreeShootControl  —— 1 行
//   15 个敌人时轻松超过 30 行，把关键信息挤出屏幕。
//
// ⇒ 集中成一个 CVar，控制台一条命令切换：
//     ShooterAI.ScreenDebug 0   // 全关
//     ShooterAI.ScreenDebug 1   // 只留聚合/关键（默认）
//     ShooterAI.ScreenDebug 2   // 每个单位一行（排查个体行为时）
//
// ============================================================
// 另一个正交的改动：Key 决定显示顺序
// ============================================================
//   `GEngine->AddOnScreenDebugMessage(Key, ...)` 是**按 Key 升序**排列的。
//   所以：
//     · 聚合/关键行用**固定小 Key**（3、4、5）→ 永远排在屏幕最上方
//     · 每个单位的行用**指针派生的大 Key** → 自动沉到下面
//   这样即使全开，重要信息也不会被挤走。
//
//   小 Key 约定（千万别和别人的撞）：
//     3 = InfluenceMap
//     4 = AILodSubsystem
//     5 = ShootControl
//     6 = Wounded 受伤提示（事件型，2 秒后自动消失）
//     100+ / 指针派生 = 每个单位的行为行

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

namespace ShooterAIDebug
{
	/**
	 * 屏幕调试级别。
	 *   0 = 全关
	 *   1 = 只留聚合/关键行
	 *   2 = 每个单位一行（默认）
	 *
	 * ★ 为什么默认是 2 而不是 1：
	 *   1 级只显示聚合统计（"有 15 个敌人、省 83% CPU"），
	 *   但**看不到任何一个敌人在做什么**——行为分档（PURSUING / RETREAT / FLEE）
	 *   全在 2 级。做行为验证时没有这些信息等于瞎猜。
	 *
	 *   代价是 15 个敌人 15 行。缓解办法：
	 *     · 聚合信息用固定小 Key（3/4/5）排在屏幕顶部，不会被挤走
	 *     · 真要干净画面时，控制台一条命令：ShooterAI.ScreenDebug 1
	 */
	inline TAutoConsoleVariable<int32> CVarScreenDebug(
		TEXT("ShooterAI.ScreenDebug"),
		2,
		TEXT("屏幕调试输出级别：0=全关 1=只留聚合/关键 2=每单位一行（默认）"),
		ECVF_Cheat);

	/** 当前级别。 */
	inline int32 GetLevel()
	{
		return CVarScreenDebug.GetValueOnGameThread();
	}

	/** 是否打印「聚合/关键」类信息（级别 >= 1）。 */
	inline bool ShouldPrintAggregate()
	{
		return GetLevel() >= 1;
	}

	/** 是否打印「每个单位一行」的详细诊断（级别 >= 2）。 */
	inline bool ShouldPrintPerActor()
	{
		return GetLevel() >= 2;
	}

	// ---------- 固定 Key，保证聚合信息永远在屏幕顶部 ----------
	namespace Key
	{
		constexpr uint64 InfluenceMap = 3;
		constexpr uint64 AILod        = 4;
		constexpr uint64 ShootControl = 5;
		constexpr uint64 Wounded      = 6;
	}
}
