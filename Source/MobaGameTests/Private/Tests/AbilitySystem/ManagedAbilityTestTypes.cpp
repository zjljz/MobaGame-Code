#include "Tests/AbilitySystem/ManagedAbilityTestTypes.h"

#include "Combat/HeroAttributeSet.h"
#include "GameplayEffectComponents/AbilitiesGameplayEffectComponent.h"
#include "GameplayEffectComponents/AdditionalEffectsGameplayEffectComponent.h"
#include "GameplayEffectComponents/CancelAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/CustomCanApplyGameplayEffectComponent.h"
#include "GameplayEffectComponents/RemoveOtherGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "MobaGameplayTags.h"

UManagedAbilityTestAbility::UManagedAbilityTestAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	NetSecurityPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnly;
	CooldownGameplayEffectClass = UManagedAbilityCooldownEffect::StaticClass();
}

void UManagedAbilityTestAbility::ActivateAbility(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	EndAbility(Handle, ActorInfo, ActivationInfo, false, false);
}

void UManagedAbilityHoldingAbility::ActivateAbility(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	// 由测试显式 Finish 或原生取消路径结束，用于观察真实活动 Spec。
}

void UManagedAbilityHoldingAbility::Finish()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, false, false);
}

void UManagedAbilityUncancelableAbility::ActivateAbility(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	SetCanBeCanceled(false);
}

UManagedAbilityPassiveEffect::UManagedAbilityPassiveEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	StackingType = EGameplayEffectStackingType::None;
}

UManagedAbilityCooldownEffect::UManagedAbilityCooldownEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FScalableFloat(30);
	FInheritedTagContainer Tags;
	Tags.AddTag(MobaGameplayTags::Cooldown_Self);
	auto* Component = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("CooldownTags"));
	GEComponents.Add(Component);
	Component->SetAndApplyTargetTagChanges(Tags);
}

int32 UManagedAbilityRejectRequirement::ApplicationAttempts = 0;

bool UManagedAbilityRejectRequirement::CanApplyGameplayEffect_Implementation(const UGameplayEffect* GameplayEffect, const FGameplayEffectSpec& Spec, UAbilitySystemComponent* ASC) const
{
	++ApplicationAttempts;
	return false;
}

UManagedAbilityRejectedEffect::UManagedAbilityRejectedEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	auto* Component = CreateDefaultSubobject<UTargetTagRequirementsGameplayEffectComponent>(TEXT("RequireMissingTag"));
	GEComponents.Add(Component);
	Component->ApplicationTagRequirements.RequireTags.AddTag(MobaGameplayTags::State_Invulnerable);
}

UManagedAbilityEventHoldingAbility::UManagedAbilityEventHoldingAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerExecution;
	auto& Trigger = AbilityTriggers.AddDefaulted_GetRef();
	Trigger.TriggerTag = MobaGameplayTags::Event_UnitAttack;
	Trigger.TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;
}

UManagedAbilityPrimaryCostEffect::UManagedAbilityPrimaryCostEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	auto& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UHeroAttributeSet::GetManaAttribute();
	Modifier.ModifierOp = EGameplayModOp::Additive;
	Modifier.ModifierMagnitude = FScalableFloat(-20);
}

UManagedAbilityAdditionalEffect::UManagedAbilityAdditionalEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	auto* Component = CreateDefaultSubobject<UAdditionalEffectsGameplayEffectComponent>(TEXT("InstantChild"));
	GEComponents.Add(Component);
	Component->OnApplicationGameplayEffects.AddDefaulted_GetRef().EffectClass = UManagedAbilityPrimaryCostEffect::StaticClass();
}

UManagedAbilityRemoveOtherEffect::UManagedAbilityRemoveOtherEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	auto* Component = CreateDefaultSubobject<URemoveOtherGameplayEffectComponent>(TEXT("RemoveCooldown"));
	GEComponents.Add(Component);
	auto& Query = Component->RemoveGameplayEffectQueries.AddDefaulted_GetRef();
	Query.EffectDefinition = UManagedAbilityCooldownEffect::StaticClass();
}

UManagedAbilityCancelEffect::UManagedAbilityCancelEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	auto* Component = CreateDefaultSubobject<UCancelAbilityTagsGameplayEffectComponent>(TEXT("CancelExistingAbility"));
	GEComponents.Add(Component);
	FInheritedTagContainer WithTags, WithoutTags;
	WithTags.AddTag(MobaGameplayTags::Ability_CancelOnDeath);
	Component->SetAndApplyCanceledAbilityTagChanges(WithTags, WithoutTags);
}

UManagedAbilityGrantAbilityEffect::UManagedAbilityGrantAbilityEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	auto* Component = CreateDefaultSubobject<UAbilitiesGameplayEffectComponent>(TEXT("UntrackedGrant"));
	GEComponents.Add(Component);
	FGameplayAbilitySpecConfig Config;
	Config.Ability = UManagedAbilityTestAbility::StaticClass();
	Component->AddGrantedAbilityConfig(Config);
}

int32 UManagedAbilityCustomTagsComponent::ApplicationCount = 0;

void UManagedAbilityCustomTagsComponent::OnGameplayEffectApplied(FActiveGameplayEffectsContainer& Container, FGameplayEffectSpec& Spec, FPredictionKey& PredictionKey) const
{
	++ApplicationCount;
}

UManagedAbilityCustomComponentEffect::UManagedAbilityCustomComponentEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	GEComponents.Add(CreateDefaultSubobject<UManagedAbilityCustomTagsComponent>(TEXT("CustomSubclass")));
}

UManagedAbilityCustomRequirementEffect::UManagedAbilityCustomRequirementEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	auto* Component = CreateDefaultSubobject<UCustomCanApplyGameplayEffectComponent>(TEXT("RejectApplication"));
	GEComponents.Add(Component);
	Component->ApplicationRequirements.Add(UManagedAbilityRejectRequirement::StaticClass());
}

UManagedAbilityRemovalRequirementEffect::UManagedAbilityRemovalRequirementEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	auto* Component = CreateDefaultSubobject<UTargetTagRequirementsGameplayEffectComponent>(TEXT("RemoveOnTag"));
	GEComponents.Add(Component);
	Component->RemovalTagRequirements.RequireTags.AddTag(MobaGameplayTags::State_Untargetable);
}

UManagedAbilityOngoingRequirementEffect::UManagedAbilityOngoingRequirementEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	auto* Component = CreateDefaultSubobject<UTargetTagRequirementsGameplayEffectComponent>(TEXT("OngoingTag"));
	GEComponents.Add(Component);
	Component->OngoingTagRequirements.RequireTags.AddTag(MobaGameplayTags::State_Untargetable);
}

UManagedAbilityGrantedTagEffect::UManagedAbilityGrantedTagEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	auto* Component = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("GrantedTag"));
	GEComponents.Add(Component);
	FInheritedTagContainer Tags;
	Tags.AddTag(MobaGameplayTags::State_Untargetable);
	Component->SetAndApplyTargetTagChanges(Tags);
}

int32 UManagedAbilityOwnedTagTestAbility::ActivationCount = 0;

UManagedAbilityOwnedTagTestAbility::UManagedAbilityOwnedTagTestAbility()
{
	auto& Trigger = AbilityTriggers.AddDefaulted_GetRef();
	Trigger.TriggerTag = MobaGameplayTags::State_Untargetable;
	Trigger.TriggerSource = EGameplayAbilityTriggerSource::OwnedTagAdded;
}

void UManagedAbilityOwnedTagTestAbility::ActivateAbility(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	++ActivationCount;
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
}
