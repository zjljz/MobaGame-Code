#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Hero/HeroAttributeConfiguration.h"
#include "HeroDefinition.generated.h"

class UAbilitySetDefinition;
class UStaticMesh;

UCLASS(BlueprintType)
class MOBAGAME_API UHeroDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	/**
	 * 仅检查已加载的初始化配置；不在生成 Hero 时同步加载资产。
	 * 属性与成长在初始化前结合当前比赛规则校验。
	 */
	bool ValidateInitialization(FString& Reason) const;

	bool ValidateAttributes(const FHeroAttributeRules& Rules, FString& Reason) const;


public:
	virtual FPrimaryAssetId GetPrimaryAssetId() const override { return {TEXT("Hero"), GetFName()}; }


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Server,Client"))
	TSoftObjectPtr<UAbilitySetDefinition> AbilitySet;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	EHeroResourceType ResourceType = EHeroResourceType::Mana;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowth Attributes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Client"))
	TSoftObjectPtr<UStaticMesh> BodyMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float MaxClickDistance = 1500;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float RespawnDelay = 4;

};

