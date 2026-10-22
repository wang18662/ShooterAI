// AITintComponent.cpp

#include "AITintComponent.h"

#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

UAITintComponent::UAITintComponent()
{
	// 纯初始化组件，不需要 Tick
	PrimaryComponentTick.bCanEverTick = false;
}

void UAITintComponent::OnRegister()
{
	Super::OnRegister();
}

void UAITintComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bEnabled)
	{
		ApplyTintInternal();
	}
}

void UAITintComponent::ApplyTint()
{
	ApplyTintInternal();
}

void UAITintComponent::ClearTint()
{
	// 还原：把原始材质列表逐个塞回去。
	// 用 SetMaterial 而不是重新创建 MID —— 原材质指针还在，
	// 直接还原最干净，也不会在内存里留一堆废弃的 MID。
	for (int32 MeshIndex = 0; MeshIndex < TintedMeshes.Num(); ++MeshIndex)
	{
		UMeshComponent* Mesh = TintedMeshes[MeshIndex];
		if (!IsValid(Mesh) || !OriginalMaterials.IsValidIndex(MeshIndex))
		{
			continue;
		}

		const TArray<TObjectPtr<UMaterialInterface>>& Originals = OriginalMaterials[MeshIndex];
		for (int32 MatIndex = 0; MatIndex < Originals.Num(); ++MatIndex)
		{
			if (IsValid(Originals[MatIndex]))
			{
				Mesh->SetMaterial(MatIndex, Originals[MatIndex]);
			}
		}
	}

	TintedMeshes.Reset();
	OriginalMaterials.Reset();
}

void UAITintComponent::ApplyTintInternal()
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	// 已经染过就先还原，避免"染染色色"叠出奇怪的结果
	if (TintedMeshes.Num() > 0)
	{
		ClearTint();
	}

	// 收集所有网格体组件。
	// 用遍历而不是硬编码名字：模板角色有第一人称/第三人称两套网格，
	// 名字可能变（Mesh / FirstPersonMesh / CharacterMesh0…），
	// 按类型收集才不会漏。
	TArray<UMeshComponent*> Meshes;
	OwnerActor->GetComponents<UMeshComponent>(Meshes);

	for (UMeshComponent* Mesh : Meshes)
	{
		if (!IsValid(Mesh))
		{
			continue;
		}

		const int32 NumMaterials = Mesh->GetNumMaterials();
		if (NumMaterials <= 0)
		{
			continue;
		}

		// 记录原始材质（用于还原）
		TArray<TObjectPtr<UMaterialInterface>> Originals;
		Originals.Reserve(NumMaterials);

		for (int32 MatIndex = 0; MatIndex < NumMaterials; ++MatIndex)
		{
			UMaterialInterface* OriginalMat = Mesh->GetMaterial(MatIndex);
			Originals.Add(OriginalMat);

			if (!IsValid(OriginalMat))
			{
				continue;
			}

			// 创建动态实例：只影响这一个网格，不污染资产
			UMaterialInstanceDynamic* MID = Mesh->CreateDynamicMaterialInstance(MatIndex, OriginalMat);
			if (!MID)
			{
				continue;
			}

			// ------------------------------------------------------------
			// ★ 不假设参数名，而是遍历所有参数并"凡名字像就设"
			//
			//   不同母材质的参数名差异很大：
			//     BaseColor / Base Color / Tint / Color / Diffuse…
			//   在无法直接查看材质资产的情况下，
			//   硬编码一个参数名必然在换模型时失效。
			//   遍历 + 模糊匹配的组合兼容性最好，代价只是多几次 Set 调用。
			// ------------------------------------------------------------
			TArray<FMaterialParameterInfo> ScalarInfos;
			TArray<FGuid> ScalarIds;
			MID->GetAllScalarParameterInfo(ScalarInfos, ScalarIds);

			for (const FMaterialParameterInfo& Info : ScalarInfos)
			{
				const FString LowerName = Info.Name.ToString().ToLower();

				// 自发光强度：名字含 emissive / glow 且不是纯颜色参数
				if (bApplyEmissive
					&& (LowerName.Contains(TEXT("emissive")) || LowerName.Contains(TEXT("glow")))
					&& !LowerName.Contains(TEXT("color")))
				{
					MID->SetScalarParameterValue(Info.Name, EmissiveStrength);
				}
			}

			TArray<FMaterialParameterInfo> VectorInfos;
			TArray<FGuid> VectorIds;
			MID->GetAllVectorParameterInfo(VectorInfos, VectorIds);

			for (const FMaterialParameterInfo& Info : VectorInfos)
			{
				const FString LowerName = Info.Name.ToString().ToLower();

				const bool bLooksLikeColor =
					LowerName.Contains(TEXT("color")) ||
					LowerName.Contains(TEXT("tint")) ||
					LowerName.Contains(TEXT("diffuse")) ||
					LowerName.Contains(TEXT("base"));

				if (!bLooksLikeColor)
				{
					continue;
				}

				// 自发光颜色单独处理（要乘强度，否则只是换个色调）
				if (LowerName.Contains(TEXT("emissive")) || LowerName.Contains(TEXT("glow")))
				{
					if (bApplyEmissive)
					{
						MID->SetVectorParameterValue(Info.Name,
							TintColor * EmissiveStrength);
					}
				}
				else
				{
					MID->SetVectorParameterValue(Info.Name, TintColor);
				}
			}

			// 如果母材质一个像颜色的参数都没有（比如纯贴图材质），
			// 上面的循环什么也不会做 —— 这是可接受的：
			// 用 SkeletalMesh 的覆盖材质做染色是另一套方案，
			// 但会丢失原材质的贴图细节，不如"参数不匹配就不染"稳妥。
		}

		TintedMeshes.Add(Mesh);
		OriginalMaterials.Add(Originals);
	}
}
