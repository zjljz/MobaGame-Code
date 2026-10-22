#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ItemDefinition.generated.h"

class UAbilitySetDefinition;

// PrimaryAsset负责扫描身份，ItemId是目录内短键；依赖仍由Experience显式展开。
UCLASS(BlueprintType)
class MOBAGAME_API UItemDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	/**
	 * 仅检查已加载闭包，不在服务器交易中发起加载。失败返回稳定原因码。
	 */
	bool ValidateDefinition(FString& Reason) const;


public:
	virtual FPrimaryAssetId GetPrimaryAssetId() const override { return {TEXT("Item"), GetFName()}; }


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FName ItemId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	int32 Price = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	int32 MaxStack = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	bool bConsumable = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TSoftObjectPtr<UAbilitySetDefinition> AbilitySet;

	// 重复引用表示所需数量；不自动购买缺少的组件。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TArray<TSoftObjectPtr<UItemDefinition>> Recipe;
};

UCLASS(BlueprintType)
class MOBAGAME_API UItemCatalogDefinition : public UDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	/**
	 * 只查询已加载条目；权威使用前必须完成ValidateCatalog。
	 */
	const UItemDefinition* FindItem(FName ItemId) const;

	bool ValidateCatalog(FString& Reason) const;


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TArray<TSoftObjectPtr<UItemDefinition>> Items;
};
