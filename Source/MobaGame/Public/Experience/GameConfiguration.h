#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Experience/ExperienceDefinitions.h"
#include "GameConfiguration.generated.h"

// 根配置中的唯一启用入口；稳定身份用于网络，软引用与来源信息用于读取插件内定义。
USTRUCT(BlueprintType)
struct MOBAGAME_API FEnabledModeEntry
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FPrimaryAssetId ModeId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FText Description;

	// 模式定义资产软引用，与 ModeId 配对使用。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TSoftObjectPtr<UModeDefinition> ModeDef;

	// 先注册这些来源插件以读取 Mode 及其轻量定义；随后与 Experience 依赖合并请求 Loaded。
	// 包含模式定义资产的 GameFeature 插件名称；先注册这些插件以读取定义。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TArray<FString> DefinitionSourceFeatures;
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FMatchLaunchSelection
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FPrimaryAssetId ModeId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FName ModeConfigName;
};

UCLASS(BlueprintType)
class MOBAGAME_API UGameConfiguration : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	/**
	 * 只检查宿主 Frontend 路径及启用入口的身份唯一性和非空软引用，不加载软引用目标。
	 * 这是配置内容校验，不是 UObject 存活检查；失败时写入原因，成功时清空原因。
	 */
	bool ValidateConfiguration(FString& Reason) const;


public:
	const FEnabledModeEntry* FindEnabledMode(FPrimaryAssetId ModeId) const;
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;


public:
	// 主 Game 中的前台 ExperienceDefinition 资产软引用。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TSoftObjectPtr<UExperienceDefinition> FrontendExperience;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TSoftObjectPtr<UWorld> FrontendWorld;

	// 本地开发比赛的宿主；玩法地图仍由所选 ModeConfig 的 MapDef 动态加载。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TSoftObjectPtr<UWorld> LocalMatchWorld;

	// 人工维护的启用列表；允许为空。数组顺序只供本地开发索引使用，不作为网络身份。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TArray<FEnabledModeEntry> EnabledModes;
};

