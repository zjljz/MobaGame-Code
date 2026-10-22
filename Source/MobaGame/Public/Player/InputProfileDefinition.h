#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "InputProfileDefinition.generated.h"

class UInputMappingContext;
class UInputAction;

UENUM(BlueprintType)
enum class EHeroInputProfile : uint8
{
	WASD,
	ClickToMove
};

UCLASS(BlueprintType)
class MOBAGAME_API UInputProfileDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	// 输入资源检查
	bool IsLoaded() const;


public:
	virtual FPrimaryAssetId GetPrimaryAssetId() const override { return {TEXT("InputProfile"), GetFName()}; }


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	EHeroInputProfile Profile = EHeroInputProfile::WASD;

	// 基础输入映射上下文资产。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Client"))
	TSoftObjectPtr<UInputMappingContext> Mapping;

	// 移动输入动作资产，不是移动向量。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Client"))
	TSoftObjectPtr<UInputAction> Move;

	// 点击移动输入动作资产。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Client"))
	TSoftObjectPtr<UInputAction> Click;

	// 切换输入模式的动作资产。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Client"))
	TSoftObjectPtr<UInputAction> SwitchProfile;

	// 主技能输入动作资产，不是 GA。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Client"))
	TSoftObjectPtr<UInputAction> PrimaryAbility;

	// 单位攻击输入动作资产。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Client"))
	TSoftObjectPtr<UInputAction> UnitAttack;

	// 目标技能附加映射上下文资产。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(AssetBundles="Client"))
	TSoftObjectPtr<UInputMappingContext> TargetMapping;

	// 目标技能输入动作资产，不是 GA。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(AssetBundles="Client"))
	TSoftObjectPtr<UInputAction> TargetAbility;
};

