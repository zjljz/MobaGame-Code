#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "HeroAttributeSet.generated.h"

UCLASS()
class MOBAGAME_API UHeroAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UHeroAttributeSet();

	virtual void OnAttributeAggregatorCreated(const FGameplayAttribute& Attribute, FAggregator* Aggregator) const override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

	virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;

	virtual bool PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data) override;

	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;


public:
	/** 捕获侧和有效值侧共用约束；非有限输入不允许进入计算。 */
	float ConstrainValue(const FGameplayAttribute& Attribute, float Value) const;

	bool IsSettlementBusy() const { return bSettling; }


public:
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, HeroLevel)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(HeroLevel)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, Health)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(Health)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, MaxHealth)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(MaxHealth)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, Mana)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(Mana)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, MaxMana)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(MaxMana)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, HealthRegeneration)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(HealthRegeneration)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, ManaRegeneration)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(ManaRegeneration)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, AttackDamage)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(AttackDamage)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, AbilityPower)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(AbilityPower)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, Armor)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(Armor)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, MagicResistance)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(MagicResistance)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, CriticalChance)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(CriticalChance)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, CriticalDamageMultiplier)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(CriticalDamageMultiplier)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, CooldownReduction)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(CooldownReduction)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, MoveSpeed)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(MoveSpeed)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, DamageTakenMultiplier)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(DamageTakenMultiplier)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, Shield)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(Shield)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, IncomingDamage)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UHeroAttributeSet, IncomingHeal)


private:
	UFUNCTION()
	void OnRep_HeroLevel(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_MaxHealth(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_Mana(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_MaxMana(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_HealthRegeneration(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_ManaRegeneration(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_AttackDamage(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_AbilityPower(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_Armor(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_MagicResistance(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_CriticalChance(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_CriticalDamageMultiplier(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_CooldownReduction(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_MoveSpeed(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_DamageTakenMultiplier(const FGameplayAttributeData& Previous);

	UFUNCTION()
	void OnRep_Shield(const FGameplayAttributeData& Previous);


public:
	UPROPERTY(ReplicatedUsing=OnRep_HeroLevel, BlueprintReadOnly)
	FGameplayAttributeData HeroLevel;

	UPROPERTY(ReplicatedUsing=OnRep_Health, BlueprintReadOnly)
	FGameplayAttributeData Health;

	UPROPERTY(ReplicatedUsing=OnRep_MaxHealth, BlueprintReadOnly)
	FGameplayAttributeData MaxHealth;

	UPROPERTY(ReplicatedUsing=OnRep_Mana, BlueprintReadOnly)
	FGameplayAttributeData Mana;

	UPROPERTY(ReplicatedUsing=OnRep_MaxMana, BlueprintReadOnly)
	FGameplayAttributeData MaxMana;

	UPROPERTY(ReplicatedUsing=OnRep_HealthRegeneration, BlueprintReadOnly)
	FGameplayAttributeData HealthRegeneration;

	UPROPERTY(ReplicatedUsing=OnRep_ManaRegeneration, BlueprintReadOnly)
	FGameplayAttributeData ManaRegeneration;

	UPROPERTY(ReplicatedUsing=OnRep_AttackDamage, BlueprintReadOnly)
	FGameplayAttributeData AttackDamage;

	UPROPERTY(ReplicatedUsing=OnRep_AbilityPower, BlueprintReadOnly)
	FGameplayAttributeData AbilityPower;

	UPROPERTY(ReplicatedUsing=OnRep_Armor, BlueprintReadOnly)
	FGameplayAttributeData Armor;

	UPROPERTY(ReplicatedUsing=OnRep_MagicResistance, BlueprintReadOnly)
	FGameplayAttributeData MagicResistance;

	UPROPERTY(ReplicatedUsing=OnRep_CriticalChance, BlueprintReadOnly)
	FGameplayAttributeData CriticalChance;

	UPROPERTY(ReplicatedUsing=OnRep_CriticalDamageMultiplier, BlueprintReadOnly)
	FGameplayAttributeData CriticalDamageMultiplier;

	UPROPERTY(ReplicatedUsing=OnRep_CooldownReduction, BlueprintReadOnly)
	FGameplayAttributeData CooldownReduction;

	UPROPERTY(ReplicatedUsing=OnRep_MoveSpeed, BlueprintReadOnly)
	FGameplayAttributeData MoveSpeed;

	UPROPERTY(ReplicatedUsing=OnRep_DamageTakenMultiplier, BlueprintReadOnly)
	FGameplayAttributeData DamageTakenMultiplier;

	UPROPERTY(ReplicatedUsing=OnRep_Shield, BlueprintReadOnly)
	FGameplayAttributeData Shield;

	// 仅服务器 Execution 临时输入；每次取出后先清零。
	UPROPERTY()
	FGameplayAttributeData IncomingDamage;

	UPROPERTY()
	FGameplayAttributeData IncomingHeal;


private:
	bool bSettling = false;
};
