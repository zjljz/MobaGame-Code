#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "CombatEffectContext.generated.h"

class UAbilitySystemComponent;

// 仅服务器的本次提交结果；不作为复制状态或累计战斗记录。
USTRUCT()
struct MOBAGAME_API FCombatSettlementResult
{
	GENERATED_BODY()

public:
	UPROPERTY()
	bool bApplied = false;

	UPROPERTY()
	bool bCritical = false;

	UPROPERTY()
	bool bDied = false;

	UPROPERTY()
	float Amount = 0;

	UPROPERTY()
	float ShieldAbsorbed = 0;

	UPROPERTY()
	float HealthDelta = 0;

	UPROPERTY()
	float Overflow = 0;

	UPROPERTY()
	FName Rejection;
};

/** 稳定归因按值存活；数值仍由原生 CaptureSpec 持有。每目标独立复制。 */
USTRUCT()
struct MOBAGAME_API FCombatEffectContext : public FGameplayEffectContext
{
	GENERATED_BODY()

public:
	virtual UScriptStruct* GetScriptStruct() const override;

	virtual FGameplayEffectContext* Duplicate() const override;

	virtual bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess) override;


public:
	FGuid Match;
	FGuid Participant;
	FGuid Entity;
	FGuid Delivery;
	int32 Team = INDEX_NONE;
	mutable FCombatSettlementResult Result;
};

template<>
struct TStructOpsTypeTraits<FCombatEffectContext> : TStructOpsTypeTraitsBase2<FCombatEffectContext>
{
	enum { WithNetSerializer = true, WithCopy = true };
};

namespace MobaCombat
{
	MOBAGAME_API FCombatEffectContext* GetContext(const FGameplayEffectContextHandle& Context);

	/** 在目标预检后、原生 Commit 前准备；失败则连同 Spec 丢弃，不重抓已提交来源。 */
	MOBAGAME_API FGameplayEffectSpecHandle PrepareSpec(UAbilitySystemComponent* Source, const UGameplayEffect* Effect, float Level, FGuid Delivery = FGuid());

	MOBAGAME_API FGameplayEffectSpecHandle PrepareSpec(UAbilitySystemComponent* Source, TSubclassOf<UGameplayEffect> Effect, float Level, FGuid Delivery = FGuid());

	MOBAGAME_API FGameplayEffectSpec CopyForTarget(const FGameplayEffectSpec& Spec);

	MOBAGAME_API FGameplayEffectAttributeCaptureDefinition SourceAttackDamageCapture();
	MOBAGAME_API FGameplayEffectAttributeCaptureDefinition SourceAbilityPowerCapture();

	/** 使用原生来源捕获求金额；仅未使用的零系数允许没有对应属性。 */
	MOBAGAME_API bool ResolveRawAmount(const FGameplayEffectSpec& Spec, const FGameplayEffectModifierMagnitude& Base,
		const FScalableFloat& AttackCoefficient, const FScalableFloat& AbilityCoefficient, float& Amount);

	MOBAGAME_API bool EvaluateCapture(const FGameplayEffectSpec& Spec, const FGameplayEffectAttributeCaptureDefinition& Definition, float& Value);
}
