#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Teams/TeamConfiguration.h"
#include "Player/InputProfileDefinition.h"
#include "Combat/CombatRules.h"
#include "Hero/HeroAttributeConfiguration.h"
#include "ExperienceDefinitions.generated.h"

class ULevel;
class UHeroDefinition;
class UGameplayEffect;
class UItemCatalogDefinition;
class UExperienceDefinition;
class UMapDefinition;
class UMatchRulesDefinition;

// 比赛人数规则独立于地图提供的出生位置；开局阶段由后续比赛流程消费最低人数。
USTRUCT(BlueprintType)
struct MOBAGAME_API FMatchPlayerConfiguration
{
	GENERATED_BODY()

public:
	// 人数配置校验
	bool IsValid(int32 TeamCount, FString& Reason) const;


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "1"))
	int32 MaximumParticipants = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "1"))
	int32 MinimumPlayersToStart = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta = (ClampMin = "1"))
	int32 MaximumPlayersPerTeam = 2;
};

UENUM(BlueprintType)
enum class EHeroSelectionPolicy : uint8
{
	Fixed,
	SystemAssigned,
	PlayerChoice
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FHeroSelectionRules
{
	GENERATED_BODY()

public:
	bool IsValid(FString& Reason) const;

	bool Allows(const FSoftObjectPath& HeroDefPath) const;


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	EHeroSelectionPolicy Policy = EHeroSelectionPolicy::Fixed;

	// 允许选择的 HeroDefinition 资产软引用，不包含已生成的角色实例。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TArray<TSoftObjectPtr<UHeroDefinition>> AllowedHeroes;

	// 固定选择策略使用的 HeroDefinition 资产软引用。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TSoftObjectPtr<UHeroDefinition> FixedHero;
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FItemEconomyRules
{
	GENERATED_BODY()

public:
	bool IsValid(FString& Reason) const;


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	bool bItemsEnabled = false;

	// 物品目录定义资产软引用，不是运行时背包。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TSoftObjectPtr<UItemCatalogDefinition> ItemCatalog;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	int32 InitialGold = 1000;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FVector ShopCenter = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	float ShopRadius = 1000;
};

UCLASS(BlueprintType)
class MOBAGAME_API UMatchRulesDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	bool IsValid(FString& Reason) const;


public:
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FTeamConfiguration TeamRules;

	// 比赛人数与每队容量配置，不是玩家列表。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FMatchPlayerConfiguration Players;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FHeroSelectionRules HeroSelectionRules;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FCombatRules CombatRules;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FHeroAttributeRules AttributeRules;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FItemEconomyRules ItemEconomyRules;
};

UCLASS(BlueprintType)
class MOBAGAME_API UExperienceDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;


public:
	// 所需 GameFeature 的插件名称；加载时解析为 URL，并非 URL 或插件实例数组。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TArray<FString> FeatureDependencies;

	// 默认 HeroDefinition 资产软引用；不是已经生成的默认角色。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TSoftObjectPtr<UHeroDefinition> DefaultHero;

	// 输入配置定义资产软引用，与下方允许的输入模式枚举集合区分。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration", meta = (AssetBundles = "Client"))
	TArray<TSoftObjectPtr<UInputProfileDefinition>> InputProfiles;

	// 允许的输入模式枚举值，不是输入配置资产。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TArray<EHeroInputProfile> AllowedInputProfiles;

	// 需要预载的 GameplayEffect 类软引用，不是已应用效果实例。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration", meta = (AssetBundles = "Server,Client"))
	TArray<TSoftClassPtr<UGameplayEffect>> CombatEffects;
};

// 模式允许的完整组合；服务器只接受组合身份，不接受客户端任意拼接软引用。
USTRUCT(BlueprintType)
struct MOBAGAME_API FModeConfiguration
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FName ModeConfigName;

	// 此模式组合选择的 ExperienceDefinition 资产软引用。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TSoftObjectPtr<UExperienceDefinition> ExperienceDef;

	// 地图定义资产软引用；实际 World 地图由该定义配置。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TSoftObjectPtr<UMapDefinition> MapDef;

	// 此模式组合选择的 MatchRulesDefinition 资产软引用。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TSoftObjectPtr<UMatchRulesDefinition> RulesDef;
};

UCLASS(BlueprintType)
class MOBAGAME_API UModeDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	const FModeConfiguration* FindModeConfig(FName ModeConfigName) const;


public:
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Configuration")
	TArray<FModeConfiguration> ModeConfigs;
};

UCLASS(BlueprintType)
class MOBAGAME_API UMapDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif


public:
	// 出生点统计
	static int32 CountPlayerStarts(const ULevel* Level);


public:
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;


public:
	// World 由本次动态 LevelStreaming 显式加载，避免仅加载包却宣称地图已进入 World。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Map")
	TSoftObjectPtr<UWorld> World;

	// 地图自检所需的出生点数，不是玩家准入容量。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Map")
	int32 MinimumPlayerStarts = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Map")
	bool bRequireNavigation = false;
};

