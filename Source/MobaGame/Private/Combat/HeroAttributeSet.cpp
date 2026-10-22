#include "Combat/HeroAttributeSet.h"

#include "Combat/CombatantInterface.h"
#include "Combat/CombatEffectContext.h"
#include "Hero/HeroAttributeComponent.h"
#include "Hero/HeroCharacter.h"
#include "GameplayEffectExtension.h"
#include "GameplayEffectAggregator.h"
#include "GameplayEffectAggregatorLibrary.h"
#include "MobaGameplayTags.h"
#include "MobaMessageTags.h"
#include "Net/UnrealNetwork.h"

UHeroAttributeSet::UHeroAttributeSet()
{
	DamageTakenMultiplier.SetBaseValue(1);
	DamageTakenMultiplier.SetCurrentValue(1);
	CriticalDamageMultiplier.SetBaseValue(1);
	CriticalDamageMultiplier.SetCurrentValue(1);
}

void UHeroAttributeSet::OnAttributeAggregatorCreated(const FGameplayAttribute& Attribute, FAggregator* Aggregator) const
{
	Super::OnAttributeAggregatorCreated(Attribute, Aggregator);
	if (Attribute == GetMoveSpeedAttribute() && Aggregator)
	{
		Aggregator->EvaluationMetaData = &FAggregatorEvaluateMetaDataLibrary::MostNegativeMod_AllPositiveMods;
	}
}

void UHeroAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, HeroLevel, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, Mana, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, MaxMana, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, HealthRegeneration, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, ManaRegeneration, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, AttackDamage, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, AbilityPower, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, Armor, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, MagicResistance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, CriticalChance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, CriticalDamageMultiplier, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, CooldownReduction, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, MoveSpeed, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, DamageTakenMultiplier, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroAttributeSet, Shield, COND_None, REPNOTIFY_Always);
}

void UHeroAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	if (auto* Hero = Cast<AHeroCharacter>(GetOwningActor()); Hero && Hero->HasAuthority())
	{
		Hero->GetAttributeComponent()->BeforeAttributeChange(Attribute);
	}

	NewValue = ConstrainValue(Attribute, FMath::IsFinite(NewValue) ? NewValue : GetOwningAbilitySystemComponent()->GetNumericAttribute(Attribute));
}

void UHeroAttributeSet::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
	Super::PostAttributeChange(Attribute, OldValue, NewValue);
	if (auto* Hero = Cast<AHeroCharacter>(GetOwningActor()); Hero && Hero->HasAuthority())
	{
		Hero->GetAttributeComponent()->AfterAttributeChange(Attribute, OldValue, NewValue);
	}
}

bool UHeroAttributeSet::PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data)
{
	if (bSettling || !FMath::IsFinite(Data.EvaluatedData.Magnitude))
	{
		return false;
	}

	if (auto* Hero = Cast<AHeroCharacter>(GetOwningActor()))
	{
		Hero->GetAttributeComponent()->FlushRegeneration();
	}

	return Super::PreGameplayEffectExecute(Data);
}

void UHeroAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);
	ICombatantInterface* Combatant = Cast<ICombatantInterface>(GetOwningActor());
	if (!Combatant || !GetOwningActor()->HasAuthority() || bSettling)
	{
		return;
	}

	TOptional<FCombatSettlementResult> Completed;
	{
		TGuardValue<bool> SettlementGuard(bSettling, true);
		auto* AbilitySystem = GetOwningAbilitySystemComponent();
		const FGameplayAttribute Attribute = Data.EvaluatedData.Attribute;
		if (Attribute == GetIncomingDamageAttribute() || Attribute == GetIncomingHealAttribute())
		{
			const float Amount = AbilitySystem->GetNumericAttribute(Attribute);
			auto* Context = MobaCombat::GetContext(Data.EffectSpec.GetContext());
			const float PreviousHealth = GetHealth();
			AbilitySystem->SetNumericAttributeBase(Attribute, 0);
			if (!Combatant->IsCombatAlive() || !FMath::IsFinite(Amount) || Amount <= 0)
			{
				return;
			}

			if (Attribute == GetIncomingDamageAttribute())
			{
				const float Absorbed = FMath::Min(GetShield(), Amount);
				if (Context)
				{
					Context->Result.ShieldAbsorbed = Absorbed;
					Context->Result.Overflow = FMath::Max(0.0f, Amount - Absorbed - PreviousHealth);
				}

				AbilitySystem->SetNumericAttributeBase(GetShieldAttribute(), GetShield() - Absorbed);
				if (Absorbed > 0 && GetShield() == 0)
				{
					AbilitySystem->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(MobaGameplayTags::Effect_Shield));
				}

				AbilitySystem->SetNumericAttributeBase(GetHealthAttribute(), FMath::Max(0.0f, GetHealth() - (Amount - Absorbed)));
			}
			else
			{
				const double Healed = FMath::Min(double(GetMaxHealth()), double(GetHealth()) + Amount);
				AbilitySystem->SetNumericAttributeBase(GetHealthAttribute(), float(Healed));
				if (Context)
				{
					Context->Result.Overflow = FMath::Max(0.0f, Amount - (GetHealth() - PreviousHealth));
				}
			}

			if (Context)
			{
				Context->Result.bApplied = true;
				Context->Result.HealthDelta = GetHealth() - PreviousHealth;
				Context->Result.bDied = (PreviousHealth > 0 && GetHealth() == 0);
			}
		}
		else if (Attribute == GetHealthAttribute() || Attribute == GetManaAttribute())
		{
			AbilitySystem->SetNumericAttributeBase(Attribute, ConstrainValue(Attribute, AbilitySystem->GetNumericAttribute(Attribute)));
		}

		if (GetHealth() == 0 && Combatant->IsCombatAlive())
		{
			Combatant->OnCombatHealthDepleted(Data.EffectSpec.GetEffectContext());
		}

		if (const auto* Context = MobaCombat::GetContext(Data.EffectSpec.GetContext()); Context && Context->Result.bApplied
			&& (Attribute == GetIncomingDamageAttribute() || Attribute == GetIncomingHealAttribute()))
		{
			Completed = Context->Result;
			Completed->bDied = (Completed->bDied && !Combatant->IsCombatAlive());
			Context->Result.bDied = Completed->bDied;
		}
	}

	if (Completed.IsSet())
	{
		// 不让通知回调跨越资源暂存/Busy区域；周期下一tick会重新初始化自己的结果。
		const FGameplayEffectContextHandle Context = Data.EffectSpec.GetContext();
		GMP::FMessageUtils::SendObjectMessage(GetOwningAbilitySystemComponent(), MOBA_MESSAGE_COMBAT_SETTLED, Context, Completed.GetValue());
	}
}

float UHeroAttributeSet::ConstrainValue(const FGameplayAttribute& Attribute, float Value) const
{
	const auto* Hero = Cast<AHeroCharacter>(GetOwningActor());
	const auto* Component = Hero ? Hero->GetAttributeComponent() : nullptr;
	if (Attribute == GetManaAttribute() || Attribute == GetMaxManaAttribute() || Attribute == GetManaRegenerationAttribute())
	{
		if (Component && Component->HasNoResource())
		{
			return 0;
		}
	}

	if (Attribute == GetHealthAttribute())
	{
		const float Maximum = Component ? Component->GetResourceMaximumDuringIntegration(false, GetMaxHealth()) : GetMaxHealth();
		return FMath::Clamp(Value, 0.0f, FMath::Max(0.0f, Maximum));
	}

	if (Attribute == GetManaAttribute())
	{
		const float Maximum = Component ? Component->GetResourceMaximumDuringIntegration(true, GetMaxMana()) : GetMaxMana();
		return FMath::Clamp(Value, 0.0f, FMath::Max(0.0f, Maximum));
	}

	if (Attribute == GetHeroLevelAttribute())
	{
		return FMath::Clamp(FMath::FloorToFloat(Value), 1.0f, Component ? float(FMath::Max(1, Component->GetMaximumLevel())) : 16777216.0f);
	}

	if (Attribute == GetCriticalChanceAttribute() || Attribute == GetCooldownReductionAttribute())
	{
		return FMath::Clamp(Value, 0.0f, Attribute == GetCriticalChanceAttribute() ? 1.0f : (Component ? Component->GetMaximumCooldownReduction() : 0.0f));
	}

	if (Attribute == GetMaxHealthAttribute() || Attribute == GetMaxManaAttribute() || Attribute == GetCriticalDamageMultiplierAttribute())
	{
		return FMath::Max(1.0f, Value);
	}

	if (Attribute == GetArmorAttribute() || Attribute == GetMagicResistanceAttribute())
	{
		return Value;
	}

	return FMath::Max(0.0f, Value);
}

void UHeroAttributeSet::OnRep_HeroLevel(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, HeroLevel, Previous);
}

void UHeroAttributeSet::OnRep_Health(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, Health, Previous);
}

void UHeroAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, MaxHealth, Previous);
}

void UHeroAttributeSet::OnRep_Mana(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, Mana, Previous);
}

void UHeroAttributeSet::OnRep_MaxMana(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, MaxMana, Previous);
}

void UHeroAttributeSet::OnRep_HealthRegeneration(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, HealthRegeneration, Previous);
}

void UHeroAttributeSet::OnRep_ManaRegeneration(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, ManaRegeneration, Previous);
}

void UHeroAttributeSet::OnRep_AttackDamage(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, AttackDamage, Previous);
}

void UHeroAttributeSet::OnRep_AbilityPower(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, AbilityPower, Previous);
}

void UHeroAttributeSet::OnRep_Armor(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, Armor, Previous);
}

void UHeroAttributeSet::OnRep_MagicResistance(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, MagicResistance, Previous);
}

void UHeroAttributeSet::OnRep_CriticalChance(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, CriticalChance, Previous);
}

void UHeroAttributeSet::OnRep_CriticalDamageMultiplier(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, CriticalDamageMultiplier, Previous);
}

void UHeroAttributeSet::OnRep_CooldownReduction(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, CooldownReduction, Previous);
}

void UHeroAttributeSet::OnRep_MoveSpeed(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, MoveSpeed, Previous);
}

void UHeroAttributeSet::OnRep_DamageTakenMultiplier(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, DamageTakenMultiplier, Previous);
}

void UHeroAttributeSet::OnRep_Shield(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroAttributeSet, Shield, Previous);
}
