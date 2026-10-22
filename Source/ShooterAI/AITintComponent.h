// AITintComponent.h
// 敌人染色：让观察者一眼分清"敌人 / 玩家 / 队友"。
//
// ============================================================
// 为什么"敌我区分"比"场景好看"更重要
// ============================================================
//   这是 AI 行为作品集。观察者需要同时跟踪 15 个敌人的行为，
//   如果它们全都长一样（模板白色 Mannequin），
//   注意力会耗在"哪个是哪个"上，而不是"AI 在做什么"。
//
//   ⇒ 高饱和纯色染色不是"美术偷懒"，是**降低认知负担**。
//     技术上：一组区分度极高的颜色，比写实材质更有效地传达信息。//
// ============================================================
// 为什么不改材质资产
// ============================================================
//   直接改 M_ 材质会影响所有使用者（包括玩家），不可逆且难回滚。
//   ⇒ 运行时创建**动态材质实例**，只改这个角色自己的一份。
//      取消勾选就还原，不污染资产。
//
// ============================================================
// 兼容性
// ============================================================
//   不同角色用不同母材质。这里不假设参数名，
//   而是**遍历材质的所有标量/向量参数**，凡是名字里带
//   "color / tint / base" 的都设一遍 —— 兼容性优先于精确性。
//   精确指定参数名的做法在换模型时必然失效。

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AITintComponent.generated.h"

class UMeshComponent;
class UMaterialInstanceDynamic;

/**
 * 敌人染色组件。
 *
 * 挂到 BP_ShooterNPC 上，勾 bEnabled，选颜色即可。
 */
UCLASS(ClassGroup = (GameAI), meta = (BlueprintSpawnableComponent, DisplayName = "AI Tint"))
class SHOOTERAI_API UAITintComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAITintComponent();

	virtual void BeginPlay() override;
	virtual void OnRegister() override;

	/**
	 * 是否染色。
	 * 取消勾选会还原原始材质（不会留下染色残留）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Tint")
	bool bEnabled = true;

	/** 染色颜色。默认高饱和红：在灰盒场景里最醒目，且与"敌人=危险"直觉一致。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Tint")
	FLinearColor TintColor = FLinearColor(1.0f, 0.05f, 0.05f, 1.0f);

	/**
	 * 是否同时给自发光（Emissive）上色。
	 *
	 * 勾上后角色会"发亮"，在暗环境或远景里更容易辨认。
	 * 太亮会丢失立体感，所以强度单独控制。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Tint")
	bool bApplyEmissive = true;

	/** 自发光强度。0 = 无自发光，1 = 与颜色等亮，>1 会过曝。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI Tint", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float EmissiveStrength = 0.35f;

	/** 手动重新应用染色（改了颜色后调用）。 */
	UFUNCTION(BlueprintCallable, Category = "AI Tint")
	void ApplyTint();

	/** 还原原始材质。 */
	UFUNCTION(BlueprintCallable, Category = "AI Tint")
	void ClearTint();

private:
	/** 被处理过的网格体组件（用于还原）。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMeshComponent>> TintedMeshes;

	/** 每个网格体原始的材质列表（ClearTint 时还原）。 */
	TArray<TArray<TObjectPtr<UMaterialInterface>>> OriginalMaterials;

	/** 把染色应用到所有网格体。 */
	void ApplyTintInternal();
};
