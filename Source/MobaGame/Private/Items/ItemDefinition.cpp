#include "Items/ItemDefinition.h"

#include "AbilitySystem/AbilitySetDefinition.h"
#include "Combat/SelfCombatEffectAbility.h"
#include "Combat/CombatEffectParameters.h"
#include "Combat/HeroAttributeSet.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "MobaGameplayTags.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

namespace
{
	bool IsConfigurationClass(const UClass* Class, const UClass* NativeBaseClass)
	{
		// 允许纯数据Blueprint，禁止扩展原生回调或通过Blueprint事件添加任意事务副作用。
		for (const UClass* CurrentClass = Class; CurrentClass; CurrentClass = CurrentClass->GetSuperClass())
		{
			if (CurrentClass->HasAnyClassFlags(CLASS_Native))
			{
				return CurrentClass == NativeBaseClass;
			}

			for (TFieldIterator<UFunction> FunctionIt(CurrentClass, EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
			{
				if (!FunctionIt->Script.IsEmpty())
				{
					return false;
				}
			}
		}

		return false;
	}

	bool ConstantMagnitude(const FGameplayEffectModifierMagnitude& Magnitude, float& Value)
	{
		return Magnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::ScalableFloat && Magnitude.GetStaticMagnitudeIfPossible(1, Value) && FMath::IsFinite(Value)
			&& Magnitude == FGameplayEffectModifierMagnitude(FScalableFloat(Value));
	}

	bool HasOnlyComponents(const UGameplayEffect* Effect, bool bAllowCombatParameters)
	{
		TArray<UClass*> EffectComponentClasses;
		GetDerivedClasses(UGameplayEffectComponent::StaticClass(), EffectComponentClasses, true);
		for (UClass* EffectComponentClass : EffectComponentClasses)
		{
			const auto* Component = Effect->FindComponent(EffectComponentClass);
			if (Component && Component->GetClass() != UAssetTagsGameplayEffectComponent::StaticClass() && Component->GetClass() != UTargetTagsGameplayEffectComponent::StaticClass()
				&& !(bAllowCombatParameters && (Component->GetClass() == UCombatHealingParametersComponent::StaticClass() || Component->GetClass() == UCombatShieldEffectComponent::StaticClass())))
			{
				return false;
			}
		}

		return Effect->GameplayCues.IsEmpty() && Effect->GetBlockedAbilityTags().IsEmpty() && Effect->Period.IsStatic() && Effect->Period.GetValueAtLevel(1) == 0
			&& Effect->GetStackingType() == EGameplayEffectStackingType::None && Effect->MaxDurationMagnitude == FGameplayEffectModifierMagnitude(FScalableFloat(0));
	}

	bool SafeAbility(const FAbilityGrantDefinition& AbilityGrant, bool bConsumable)
	{
		const auto* AbilityClass = AbilityGrant.Ability.Get();
		const auto* Ability = AbilityClass ? Cast<USelfCombatEffectAbility>(AbilityClass->GetDefaultObject()) : nullptr;
		if (!Ability || !IsConfigurationClass(AbilityClass, USelfCombatEffectAbility::StaticClass()) || AbilityGrant.Slot.IsValid() || AbilityGrant.Level != 1 || AbilityGrant.MaxLevel > 1
			|| Ability->GetCostGameplayEffect() || Ability->GetInstancingPolicy() != EGameplayAbilityInstancingPolicy::InstancedPerActor
			|| Ability->GetNetExecutionPolicy() != EGameplayAbilityNetExecutionPolicy::ServerOnly || Ability->GetNetSecurityPolicy() != EGameplayAbilityNetSecurityPolicy::ServerOnly)
		{
			return false;
		}

		if (Ability->GetAssetTags() != GetDefault<USelfCombatEffectAbility>()->GetAssetTags())
		{
			return false;
		}

		// 这些原生配置也能在grant/activate时触发任意能力或标签回调，不能只检查GA父类。
		const TCHAR* RequiredEmptyTagPropertyNames[] = {TEXT("CancelAbilitiesWithTag"), TEXT("BlockAbilitiesWithTag"), TEXT("ActivationOwnedTags"), TEXT("ActivationRequiredTags"), TEXT("ActivationBlockedTags"),
			TEXT("SourceRequiredTags"), TEXT("SourceBlockedTags"), TEXT("TargetRequiredTags"), TEXT("TargetBlockedTags")};
		for (const TCHAR* TagPropertyName : RequiredEmptyTagPropertyNames)
		{
			const auto* TagContainerProperty = FindFProperty<FStructProperty>(UGameplayAbility::StaticClass(), TagPropertyName);
			if (!TagContainerProperty || !TagContainerProperty->ContainerPtrToValuePtr<FGameplayTagContainer>(Ability)->IsEmpty())
			{
				return false;
			}
		}

		const auto* AbilityTriggersProperty = FindFProperty<FArrayProperty>(UGameplayAbility::StaticClass(), TEXT("AbilityTriggers"));
		if (!AbilityTriggersProperty || FScriptArrayHelper(AbilityTriggersProperty, AbilityTriggersProperty->ContainerPtrToValuePtr<void>(Ability)).Num() != 0)
		{
			return false;
		}

		const auto* Effect = Ability->Effect.GetDefaultObject();
		bool bHealing = false;
		float Amount = 0;
		if (!Effect || !IsConfigurationClass(Effect->GetClass(), UGameplayEffect::StaticClass())
			|| !MobaCombat::ValidateSelfEffect(Effect, AbilityGrant.Level, bHealing, Amount) || bHealing != bConsumable)
		{
			return false;
		}

		// 物品只接收静态、有限、正数配置；具体样例数值不再是事务安全的替代条件。
		const auto& Magnitude = bHealing ? Effect->FindComponent<UCombatHealingParametersComponent>()->HealAmount
			: Effect->FindComponent<UCombatShieldEffectComponent>()->ShieldAmount;
		if (!ConstantMagnitude(Magnitude, Amount) || Amount <= 0 || !HasOnlyComponents(Effect, true))
		{
			return false;
		}

		if (!bHealing)
		{
			float Duration = 0;
			FGameplayTagContainer ShieldTags(MobaGameplayTags::Effect_Shield);
			ShieldTags.AddTag(MobaGameplayTags::Effect_Beneficial);
			ShieldTags.AddTag(MobaGameplayTags::Effect_Dispel_Basic);
			ShieldTags.AddTag(MobaGameplayTags::Effect_RemoveOnDeath);
			if (!ConstantMagnitude(Effect->DurationMagnitude, Duration) || Duration <= 0 || Effect->GetGrantedTags() != ShieldTags)
			{
				return false;
			}
		}

		const auto* CooldownEffect = Ability->GetCooldownGameplayEffect();
		float Duration = 0;
		const FGameplayTag CooldownFamilyTag = bConsumable ? MobaGameplayTags::Cooldown_ItemHeal : MobaGameplayTags::Cooldown_ItemShield;
		return CooldownEffect && IsConfigurationClass(CooldownEffect->GetClass(), UGameplayEffect::StaticClass()) && HasOnlyComponents(CooldownEffect, false)
			&& CooldownEffect->DurationPolicy == EGameplayEffectDurationType::HasDuration && ConstantMagnitude(CooldownEffect->DurationMagnitude, Duration) && Duration > 0
			&& CooldownEffect->Modifiers.IsEmpty() && CooldownEffect->Executions.IsEmpty() && CooldownEffect->GetGrantedTags().Num() == 1 && CooldownEffect->GetGrantedTags().HasTagExact(CooldownFamilyTag);
	}

	bool ValidateTree(const UItemDefinition* ItemDef, TSet<const UItemDefinition*>& VisitingItemDefs, TSet<const UItemDefinition*>& ValidatedItemDefs, FString& Reason)
	{
		if (!ItemDef)
		{
			Reason = TEXT("ItemNotLoaded");
			return false;
		}

		if (VisitingItemDefs.Contains(ItemDef))
		{
			Reason = TEXT("RecipeCycle");
			return false;
		}

		if (ValidatedItemDefs.Contains(ItemDef))
		{
			return true;
		}

		if (ItemDef->ItemId.IsNone() || ItemDef->Price <= 0 || ItemDef->Price > MAX_int32 / 6 || ItemDef->MaxStack != (ItemDef->bConsumable ? 3 : 1) || ItemDef->Recipe.Num() > 6)
		{
			Reason = TEXT("ItemRange");
			return false;
		}

		const auto* AbilitySetDef = ItemDef->AbilitySet.Get();
		if (!AbilitySetDef || !AbilitySetDef->ValidateGrant(Reason))
		{
			if (!AbilitySetDef)
			{
				Reason = TEXT("AbilitySetNotLoaded");
			}

			return false;
		}

		if (AbilitySetDef->bAllowCopy || AbilitySetDef->Abilities.Num() > 1 || (!AbilitySetDef->Abilities.IsEmpty() && !AbilitySetDef->Effects.IsEmpty()) || (ItemDef->bConsumable && AbilitySetDef->Abilities.Num() != 1))
		{
			Reason = TEXT("ItemAbilityPolicy");
			return false;
		}

		for (const auto& AbilityGrant : AbilitySetDef->Abilities)
		{
			if (!SafeAbility(AbilityGrant, ItemDef->bConsumable))
			{
				Reason = TEXT("ItemAbilityPolicy");
				return false;
			}
		}

		for (const auto& PassiveEffectClass : AbilitySetDef->Effects)
		{
			const auto* Effect = PassiveEffectClass.Get() ? PassiveEffectClass.Get()->GetDefaultObject<UGameplayEffect>() : nullptr;
			if (!Effect || !IsConfigurationClass(Effect->GetClass(), UGameplayEffect::StaticClass()) || !HasOnlyComponents(Effect, false)
				|| Effect->DurationPolicy != EGameplayEffectDurationType::Infinite || !Effect->Executions.IsEmpty() || Effect->Modifiers.IsEmpty() || Effect->GetGrantedTags().Num() != 1
				|| !Effect->GetGrantedTags().HasTagExact(MobaGameplayTags::Effect_Dispel_Undispellable))
			{
				Reason = TEXT("ItemPassivePolicy");
				return false;
			}

			for (const auto& Modifier : Effect->Modifiers)
			{
				float Value = 0;
				if (!ConstantMagnitude(Modifier.ModifierMagnitude, Value) || !Modifier.SourceTags.IsEmpty() || !Modifier.TargetTags.IsEmpty()
					|| !((Modifier.Attribute == UHeroAttributeSet::GetArmorAttribute() && Modifier.ModifierOp == EGameplayModOp::Additive && Value > 0 && Value <= 10000)
						|| (Modifier.Attribute == UHeroAttributeSet::GetDamageTakenMultiplierAttribute() && Modifier.ModifierOp == EGameplayModOp::MultiplyCompound && Value > 0
							&& Value <= 1)))
				{
					Reason = TEXT("ItemModifierPolicy");
					return false;
				}
			}
		}

		VisitingItemDefs.Add(ItemDef);
		int64 ComponentPrice = 0;
		for (const auto& RecipeIngredientDef : ItemDef->Recipe)
		{
			if (!ValidateTree(RecipeIngredientDef.Get(), VisitingItemDefs, ValidatedItemDefs, Reason))
			{
				return false;
			}

			ComponentPrice += RecipeIngredientDef.Get()->Price;
		}

		if (ComponentPrice > ItemDef->Price)
		{
			Reason = TEXT("RecipePrice");
			return false;
		}

		VisitingItemDefs.Remove(ItemDef);
		ValidatedItemDefs.Add(ItemDef);
		return true;
	}

#if WITH_EDITOR
	void LoadTree(const UItemDefinition* ItemDef, TSet<const UItemDefinition*>& LoadedItemDefs)
	{
		if (!ItemDef || LoadedItemDefs.Contains(ItemDef))
		{
			return;
		}

		LoadedItemDefs.Add(ItemDef);
		if (const auto* AbilitySetDef = ItemDef->AbilitySet.LoadSynchronous())
		{
			for (const auto& AbilityGrant : AbilitySetDef->Abilities)
			{
				AbilityGrant.Ability.LoadSynchronous();
			}

			for (const auto& EffectClass : AbilitySetDef->Effects)
			{
				EffectClass.LoadSynchronous();
			}
		}

		for (const auto& RecipeIngredientDef : ItemDef->Recipe)
		{
			LoadTree(RecipeIngredientDef.LoadSynchronous(), LoadedItemDefs);
		}
	}
#endif
}

#if WITH_EDITOR
EDataValidationResult UItemDefinition::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	TSet<const UItemDefinition*> LoadedItemDefs;
	LoadTree(this, LoadedItemDefs);
	FString Reason;
	if (!ValidateDefinition(Reason))
	{
		Context.AddError(FText::FromString(TEXT("Item: ") + Reason));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

bool UItemDefinition::ValidateDefinition(FString& Reason) const
{
	Reason.Reset();
	TSet<const UItemDefinition*> VisitingItemDefs, ValidatedItemDefs;
	return ValidateTree(this, VisitingItemDefs, ValidatedItemDefs, Reason);
}

#if WITH_EDITOR
EDataValidationResult UItemCatalogDefinition::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	TSet<const UItemDefinition*> LoadedItemDefs;
	for (const auto& ItemDef : Items)
	{
		LoadTree(ItemDef.LoadSynchronous(), LoadedItemDefs);
	}

	FString Reason;
	if (!ValidateCatalog(Reason))
	{
		Context.AddError(FText::FromString(TEXT("ItemCatalog: ") + Reason));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

const UItemDefinition* UItemCatalogDefinition::FindItem(FName ItemId) const
{
	for (const auto& ItemDef : Items)
	{
		if (ItemDef.Get() && ItemDef.Get()->ItemId == ItemId)
		{
			return ItemDef.Get();
		}
	}

	return nullptr;
}

bool UItemCatalogDefinition::ValidateCatalog(FString& Reason) const
{
	Reason.Reset();
	if (Items.IsEmpty())
	{
		Reason = TEXT("EmptyCatalog");
		return false;
	}

	TSet<FName> ItemIds;
	TSet<const UItemDefinition*> CatalogItemDefs, VisitingItemDefs, ValidatedItemDefs;
	for (const auto& ItemDef : Items)
	{
		if (!ItemDef.Get())
		{
			Reason = TEXT("ItemNotLoaded");
			return false;
		}

		if (ItemIds.Contains(ItemDef.Get()->ItemId))
		{
			Reason = TEXT("DuplicateItemId");
			return false;
		}

		ItemIds.Add(ItemDef.Get()->ItemId);
		CatalogItemDefs.Add(ItemDef.Get());
	}

	TSet<FGameplayTag> CooldownFamilies;
	for (const auto* ItemDef : CatalogItemDefs)
	{
		if (!ValidateTree(ItemDef, VisitingItemDefs, ValidatedItemDefs, Reason))
		{
			return false;
		}

		for (const auto& AbilityGrant : ItemDef->AbilitySet.Get()->Abilities)
		{
			const auto* Ability = AbilityGrant.Ability.Get()->GetDefaultObject<UGameplayAbility>();
			const FGameplayTag CooldownFamilyTag = Ability->GetCooldownGameplayEffect()->GetGrantedTags().First();
			if (CooldownFamilies.Contains(CooldownFamilyTag))
			{
				Reason = TEXT("SharedDefinitionCooldown");
				return false;
			}

			CooldownFamilies.Add(CooldownFamilyTag);
		}

		for (const auto& RecipeIngredientDef : ItemDef->Recipe)
		{
			if (!CatalogItemDefs.Contains(RecipeIngredientDef.Get()))
			{
				Reason = TEXT("RecipeOutsideCatalog");
				return false;
			}
		}
	}

	return true;
}

