#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "AbilitySetDefinition.generated.h"

class UGameplayAbility;
class UGameplayEffect;

USTRUCT(BlueprintType)
struct MOBAGAME_API FAbilityGrantDefinition
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FGameplayTag Slot;

	// 技能类软引用；不是已授予的技能实例。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Server,Client"))
	TSoftClassPtr<UGameplayAbility> Ability;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	int32 Level = 1;

	// 0 沿用旧配置的初始等级为上限；正数显式启用升级。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin="0"))
	int32 MaxLevel = 0;
};

UCLASS(BlueprintType)
class MOBAGAME_API UAbilitySetDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	// 来源配置校验
	/**
	 * 只接受已加载且可按来源撤销的配置；不在权威事务中同步加载资源。
	 */
	bool ValidateGrant(FString& Reason) const;

	/**
	 * 仅允许已审核、不会响应后续标签变化的原生被动组件。
	 */
	static bool HasNonReactiveComponents(const UGameplayEffect* Effect);


public:
	virtual FPrimaryAssetId GetPrimaryAssetId() const override { return {TEXT("AbilitySet"), GetFName()}; }


public:
	// 技能授予配置条目；运行时 Spec/句柄由 ASC 单独持有。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Server,Client"))
	TArray<FAbilityGrantDefinition> Abilities;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	bool bAllowCopy = false;

	// 被动效果类软引用；不是运行时 ActiveGE 集合。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (AssetBundles = "Server,Client"))
	TArray<TSoftClassPtr<UGameplayEffect>> Effects;
};

