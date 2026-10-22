#pragma once

#include "CoreMinimal.h"
#include "HeroAttributeConfiguration.generated.h"

UENUM(BlueprintType)
enum class EHeroResourceType : uint8
{
	Mana,
	None
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FHeroAttributeGrowthValue
{
	GENERATED_BODY()

public:
	// 一级基础值；成长只读取本配置，不从装备后的有效值反推。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float BaseValue = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float GrowthPerLevel = 0;
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FHeroAttributeGrowth
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue MaxHealth;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue MaxMana;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue HealthRegeneration;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue ManaRegeneration;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue AttackDamage;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue AbilityPower;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue Armor;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue MagicResistance;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue CriticalChance;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue CriticalDamageMultiplier;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue CooldownReduction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FHeroAttributeGrowthValue MoveSpeed;
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FHeroAttributeRules
{
	GENERATED_BODY()

public:
	bool IsValid(FString& Reason) const;


public:
	// 累计门槛，首项必须为 0；数量是最高等级的唯一来源。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TArray<int64> ExperienceThresholds;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float MaximumCooldownReduction = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float MinimumSlowMultiplier = 0;
};
