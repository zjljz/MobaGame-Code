#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Teams/TeamConfiguration.h"
#include "CombatRules.generated.h"

UENUM(BlueprintType)
enum class ECombatDamageType : uint8
{
	Physical,
	Magical,
	TrueDamage
};

struct MOBAGAME_API FCombatDamageParameters
{
	float BaseDamage = 0;

	float Armor = 0;

	float MagicResistance = 0;

	float DamageTakenMultiplier = 1;

	bool bCanCritical = false;

	float CriticalChance = 0;

	float CriticalMultiplier = 1;

	// 由服务器提供的 [0, 1] 样本；纯公式不持有随机状态。
	float CriticalSample = 1;
};

// 共用伤害类型映射与目标关系配置。
USTRUCT(BlueprintType)
struct MOBAGAME_API FCombatRules
{
	GENERATED_BODY()

public:
	// 伤害解析
	bool ResolveDamageType(const FGameplayTagContainer& Tags, ECombatDamageType& OutType) const;


	// 目标关系与配置校验
	bool AllowsRelation(ETeamRelation Relation) const;

	bool IsValid() const;


public:
	// 只解释显式登记的伤害子标签，且映射必须与其分类根一致。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TMap<FGameplayTag, ECombatDamageType> DamageTypeMappings;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	bool bAllowNeutralTargets = false;
};

namespace MobaCombat
{
	/**
	 * 基础值、暴击、抗性、通用承伤倍率依次结算；护盾和生命由真实 GE 路径提交。
	 * 调用方须先成功解析伤害类型；本函数不重复检查类型。
	 * 失败时输出归零；输入包括未使用的抗性和暴击参数也必须有限且合法。
	 */
	MOBAGAME_API bool CalculateDamage(ECombatDamageType Type, const FCombatDamageParameters& Parameters,
		float& OutDamage, bool& bOutCritical);
}
