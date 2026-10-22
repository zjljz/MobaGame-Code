#include "AbilitySystem/HeroAbilitySystemComponent.h"

#include "AbilitySystem/AbilitySetDefinition.h"
#include "MobaMessageTags.h"
#include "Abilities/GameplayAbility.h"
#include "Combat/SelfCombatEffectAbility.h"
#include "Combat/CombatStateEffectComponent.h"
#include "Combat/CombatEffectParameters.h"
#include "Combat/CombatEffectContext.h"
#include "Combat/CombatCooldown.h"
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "UObject/UObjectHash.h"
#include "Hero/HeroCharacter.h"
#include "Hero/HeroAttributeComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "MobaGameplayTags.h"

UHeroAbilitySystemComponent::UHeroAbilitySystemComponent()
{
	SetIsReplicatedByDefault(true);
	SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
}

FActiveGameplayEffectHandle UHeroAbilitySystemComponent::ApplyGameplayEffectSpecToSelf(const FGameplayEffectSpec& Spec, FPredictionKey PredictionKey)
{
	if (const auto* Hero = Cast<AHeroCharacter>(GetOwner()))
	{
		auto* Component = Hero->GetAttributeComponent();
		if (!Spec.Def || Hero->GetAttributes()->IsSettlementBusy() || !Component->ValidateModifiers(Spec))
		{
			if (auto* Context = MobaCombat::GetContext(Spec.GetContext()))
			{
				Context->Result = {};
				Context->Result.Rejection = Hero->GetAttributes()->IsSettlementBusy() ? TEXT("Busy") : TEXT("Modifiers");
			}

			return FActiveGameplayEffectHandle();
		}

		Component->FlushRegeneration();
	}

	const auto AppliedHandle = Super::ApplyGameplayEffectSpecToSelf(Spec, PredictionKey);
	// 同层数刷新也必须使用活动 Spec 最新强度重算聚合器；不伪造层数/期限变化。
	if (Spec.Def && Spec.Def->GetStackingType() == EGameplayEffectStackingType::AggregateBySource)
	{
		if (const auto* Active = GetActiveGameplayEffect(AppliedHandle))
		{
			const TMap<FGameplayTag, float> Magnitudes = Active->Spec.SetByCallerTagMagnitudes;
			UpdateActiveGameplayEffectSetByCallerMagnitudes(AppliedHandle, Magnitudes);
		}
	}

	if (const auto* Hero = Cast<AHeroCharacter>(GetOwner()))
	{
		Hero->GetAttributeComponent()->RefreshSourceInterval();
	}

	return AppliedHandle;
}

void UHeroAbilitySystemComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(UHeroAbilitySystemComponent, Loadout, COND_OwnerOnly);
}

void UHeroAbilitySystemComponent::NotifyAbilityEnded(FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, bool bWasCancelled)
{
	Super::NotifyAbilityEnded(Handle, Ability, bWasCancelled);
	if (!bEndingPlay && IsOwnerActorAuthoritative() && GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(CleanupTimer);
		CleanupTimer = GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ThisClass::CleanupPending);
	}
}

void UHeroAbilitySystemComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearAllTimersForObject(this);
	}

	Super::EndPlay(EndPlayReason);
	Sources.Reset();
}

FAbilitySourceResult UHeroAbilitySystemComponent::GrantInitialSource(const UAbilitySetDefinition* AbilitySetDef, FGuid SourceId)
{
	const FAbilitySourceResult OperationCheckResult = CheckOperation(GetStateRevision());
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	const AHeroCharacter* Hero = Cast<AHeroCharacter>(GetOwner());
	if (!Hero || Hero->InitializationGeneration != 0 || !SourceId.IsValid() || !Sources.IsEmpty())
	{
		return {EAbilitySourceResult::Failed, TEXT("InitialSourceState")};
	}

	const FAbilitySourceResult DefinitionValidationResult = ValidateDefinition(AbilitySetDef);
	if (!DefinitionValidationResult.IsSuccess())
	{
		return DefinitionValidationResult;
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	FHeroGrantRecord GrantRecord;
	FName FailureReason;
	if (!StageGrant(AbilitySetDef, SourceId, GrantRecord, FailureReason))
	{
		return {EAbilitySourceResult::Failed, FailureReason};
	}


	AddSlots(GrantRecord);
	UpdateInputBlock(GrantRecord, false);
	Sources.Add(MoveTemp(GrantRecord));
	Publish(!Loadout.Slots.IsEmpty());
	return {EAbilitySourceResult::Applied};
}

bool UHeroAbilitySystemComponent::GrantFromSource(const UAbilitySetDefinition* AbilitySetDef, FGuid SourceId)
{
	return GrantManagedSource(AbilitySetDef, SourceId, GetStateRevision()).IsSuccess();
}

bool UHeroAbilitySystemComponent::RemoveSource(FGuid SourceId)
{
	const auto Result = RemoveManagedSource(SourceId, GetStateRevision());
	return Result.IsSuccess() || Result.Status == EAbilitySourceResult::Pending;
}

FAbilitySourceResult UHeroAbilitySystemComponent::ExchangeItemSources(
	const TArray<FGuid>& SourceIdsToRemove, const TArray<FGuid>& SourceIdsToAdd, const TArray<const UAbilitySetDefinition*>& AbilitySetDefsToAdd, uint32 ExpectedStateRevision)
{
	const auto OperationCheckResult = CanExchangeItemSources(SourceIdsToRemove, ExpectedStateRevision);
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	if (SourceIdsToAdd.Num() != AbilitySetDefsToAdd.Num() || SourceIdsToAdd.Num() > 6 || SourceIdsToRemove.Num() > 6)
	{
		return {EAbilitySourceResult::Failed, TEXT("SourceCount")};
	}

	TSet<FGuid> SeenSourceIds;
	for (int32 Index = 0; Index < SourceIdsToAdd.Num(); ++Index)
	{
		if (!SourceIdsToAdd[Index].IsValid() || FindSource(SourceIdsToAdd[Index]) || SeenSourceIds.Contains(SourceIdsToAdd[Index]))
		{
			return {EAbilitySourceResult::Failed, TEXT("SourceConflict")};
		}

		SeenSourceIds.Add(SourceIdsToAdd[Index]);
		const auto DefinitionValidationResult = ValidateDefinition(AbilitySetDefsToAdd[Index]);
		if (!DefinitionValidationResult.IsSuccess())
		{
			return DefinitionValidationResult;
		}

		if (AbilitySetDefsToAdd[Index]->Abilities.ContainsByPredicate([](const auto& AbilityGrant) { return AbilityGrant.Slot.IsValid(); }))
		{
			return {EAbilitySourceResult::Failed, TEXT("ItemSlot")};
		}
	}

	if (SourceIdsToAdd.IsEmpty() && SourceIdsToRemove.IsEmpty())
	{
		return {EAbilitySourceResult::NoChange};
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	TArray<FHeroGrantRecord> StagedGrantRecords;
	for (int32 Index = 0; Index < SourceIdsToAdd.Num(); ++Index)
	{
		FHeroGrantRecord GrantRecord;
		FName Failure;
		if (!StageGrant(AbilitySetDefsToAdd[Index], SourceIdsToAdd[Index], GrantRecord, Failure))
		{
			for (auto& PreviouslyStagedGrant : StagedGrantRecords)
			{
				RollbackGrant(PreviouslyStagedGrant);
			}

			return {EAbilitySourceResult::Failed, Failure};
		}

		StagedGrantRecords.Add(MoveTemp(GrantRecord));
	}

	for (FGuid SourceId : SourceIdsToRemove)
	{
		const int32 Index = Sources.IndexOfByPredicate([SourceId](const auto& GrantRecord) { return GrantRecord.Source == SourceId; });
		RollbackGrant(Sources[Index]);
		Sources.RemoveAt(Index);
	}

	for (auto& GrantRecord : StagedGrantRecords)
	{
		UpdateInputBlock(GrantRecord, false);
		Sources.Add(MoveTemp(GrantRecord));
	}

	Publish(false);
	return {EAbilitySourceResult::Applied};
}

FAbilitySourceResult UHeroAbilitySystemComponent::CanExchangeItemSources(const TArray<FGuid>& SourceIdsToRemove, uint32 ExpectedStateRevision) const
{
	const auto OperationCheckResult = CheckOperation(ExpectedStateRevision);
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	TSet<FGuid> SeenSourceIds;
	for (FGuid SourceId : SourceIdsToRemove)
	{
		const auto* GrantRecord = FindSource(SourceId);
		if (!GrantRecord || SeenSourceIds.Contains(SourceId))
		{
			return {EAbilitySourceResult::Failed, TEXT("SourceMissing")};
		}

		SeenSourceIds.Add(SourceId);
		if (GrantRecord->bPendingRemove || IsRecordActive(*GrantRecord))
		{
			return {EAbilitySourceResult::Busy, TEXT("ActiveAbility")};
		}

		if (GrantRecord->Definition->Abilities.ContainsByPredicate([](const auto& AbilityGrant) { return AbilityGrant.Slot.IsValid(); }))
		{
			return {EAbilitySourceResult::Failed, TEXT("ItemSlot")};
		}

		if (HasTagReactiveContext(GrantRecord->Definition))
		{
			return {EAbilitySourceResult::Failed, TEXT("TagReactiveContext")};
		}
	}

	return {EAbilitySourceResult::Applied};
}

FAbilitySourceResult UHeroAbilitySystemComponent::ActivateItemSource(FGuid SourceId, bool bConsumeSource)
{
	const auto OperationCheckResult = CanExchangeItemSources({SourceId}, GetStateRevision());
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	const auto* GrantRecord = FindSource(SourceId);
	if (!GrantRecord || GrantRecord->Abilities.Num() != 1 || !GrantRecord->Effects.IsEmpty())
	{
		return {EAbilitySourceResult::Failed, TEXT("NotUsable")};
	}

	const auto Handle = GrantRecord->Abilities[0];
	auto* Spec = FindAbilitySpecFromHandle(Handle);
	auto* Ability = Spec ? Cast<USelfCombatEffectAbility>(Spec->GetPrimaryInstance()) : nullptr;
	if (!Ability || !IsManagedAbilityEnabled(Handle))
	{
		return {EAbilitySourceResult::Failed, TEXT("NotUsable")};
	}

	// 冷却和护盾标签也可能触发现有能力/持续条件；不能在提交后尝试撤回即时治疗。
	if (!GameplayEffectApplicationQueries.IsEmpty())
	{
		return {EAbilitySourceResult::Failed, TEXT("TagReactiveContext")};
	}

	for (const auto& TriggeredAbilityEntry : OwnedTagTriggeredAbilities)
	{
		if (!TriggeredAbilityEntry.Value.IsEmpty())
		{
			return {EAbilitySourceResult::Failed, TEXT("TagReactiveContext")};
		}
	}

	TArray<UClass*> ComponentClasses;
	GetDerivedClasses(UGameplayEffectComponent::StaticClass(), ComponentClasses, true);
	for (auto EffectHandle : GetActiveEffects(FGameplayEffectQuery()))
	{
		const auto* ActiveEffect = GetActiveGameplayEffect(EffectHandle);
		if (!ActiveEffect)
		{
			continue;
		}

		const auto* Effect = ActiveEffect->Spec.Def.Get();
		const auto* TargetTagRequirementsComp = Effect->FindComponent<UTargetTagRequirementsGameplayEffectComponent>();
		if (TargetTagRequirementsComp && (!TargetTagRequirementsComp->OngoingTagRequirements.IsEmpty() || !TargetTagRequirementsComp->RemovalTagRequirements.IsEmpty()))
		{
			return {EAbilitySourceResult::Failed, TEXT("TagReactiveContext")};
		}

		if (UAbilitySetDefinition::HasNonReactiveComponents(Effect))
		{
			continue;
		}

		// 已审核CombatState仅响应效果应用/移除，不响应新增OwnedTag；008的来源名单保持原样。
		for (UClass* ComponentClass : ComponentClasses)
		{
			const auto* Component = Effect->FindComponent(ComponentClass);
			if (!Component || Component->GetClass() == UCombatStateEffectComponent::StaticClass()
				|| Component->GetClass() == UCombatDamageParametersComponent::StaticClass() || Component->GetClass() == UCombatHealingParametersComponent::StaticClass()
				|| Component->GetClass() == UCombatShieldEffectComponent::StaticClass() || Component->GetClass() == UAbilityCooldownParametersComponent::StaticClass())
			{
				continue;
			}

			// 其余组件仍需满足原纯被动判据，未知派生一律拒绝。
			if (Component->GetClass() != UTargetTagsGameplayEffectComponent::StaticClass() && Component->GetClass() != UAssetTagsGameplayEffectComponent::StaticClass()
				&& Component->GetClass() != UBlockAbilityTagsGameplayEffectComponent::StaticClass())
			{
				return {EAbilitySourceResult::Failed, TEXT("TagReactiveContext")};
			}
		}
	}

	if (!Ability->CheckCooldown(Handle, AbilityActorInfo.Get()))
	{
		return {EAbilitySourceResult::Failed, TEXT("Cooldown")};
	}

	if (!Ability->CanActivateAbility(Handle, AbilityActorInfo.Get()))
	{
		return {EAbilitySourceResult::Failed, TEXT("ActivationBlocked")};
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	const bool bActivated = TryActivateAbility(Handle, false);
	if (!bActivated || !Ability->WasEffectCommitted())
	{
		return {EAbilitySourceResult::Failed, TEXT("EffectRejected")};
	}

	if (bConsumeSource)
	{
		const int32 Index = Sources.IndexOfByPredicate([SourceId](const auto& GrantRecord) { return GrantRecord.Source == SourceId; });
		// 只支持无回调固定即时GA；项目来源操作在整个提交中被锁定。
		check(Index != INDEX_NONE && !IsRecordActive(Sources[Index]));
		RollbackGrant(Sources[Index]);
		Sources.RemoveAt(Index);
		Publish(false);
	}

	return {EAbilitySourceResult::Applied};
}

FAbilitySourceResult UHeroAbilitySystemComponent::GrantManagedSource(const UAbilitySetDefinition* AbilitySetDef, FGuid SourceId, uint32 ExpectedStateRevision, float Lifetime)
{
	const auto OperationCheckResult = CheckOperation(ExpectedStateRevision);
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	if (!SourceId.IsValid() || !FMath::IsFinite(Lifetime) || Lifetime < 0)
	{
		return {EAbilitySourceResult::Failed, TEXT("SourceOrLifetime")};
	}

	if (const auto* ExistingGrantRecord = FindSource(SourceId))
	{
		if (ExistingGrantRecord->bPendingRemove)
		{
			return {EAbilitySourceResult::Busy, TEXT("PendingRemove")};
		}

		return ExistingGrantRecord->Definition == AbilitySetDef && ExistingGrantRecord->Lifetime == Lifetime ? FAbilitySourceResult{EAbilitySourceResult::NoChange}
			: FAbilitySourceResult{EAbilitySourceResult::Failed, TEXT("SourceConflict")};
	}

	const auto DefinitionValidationResult = ValidateDefinition(AbilitySetDef);
	if (!DefinitionValidationResult.IsSuccess())
	{
		return DefinitionValidationResult;
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	FHeroGrantRecord GrantRecord;
	FName FailureReason;
	if (!StageGrant(AbilitySetDef, SourceId, GrantRecord, FailureReason))
	{
		return {EAbilitySourceResult::Failed, FailureReason};
	}

	GrantRecord.Lifetime = Lifetime;
	if (Lifetime > 0)
	{
		GetWorld()->GetTimerManager().SetTimer(GrantRecord.ExpiryTimer, FTimerDelegate::CreateUObject(this, &ThisClass::ExpireSource, SourceId), Lifetime, false);
	}

	const int32 PreviousSlots = Loadout.Slots.Num();
	AddSlots(GrantRecord);
	UpdateInputBlock(GrantRecord, false);
	Sources.Add(MoveTemp(GrantRecord));
	Publish(PreviousSlots != Loadout.Slots.Num());
	return {EAbilitySourceResult::Applied};
}

FAbilitySourceResult UHeroAbilitySystemComponent::RemoveManagedSource(FGuid SourceId, uint32 ExpectedStateRevision)
{
	const auto OperationCheckResult = CheckOperation(ExpectedStateRevision);
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	return BeginRemove(SourceId);
}

FAbilitySourceResult UHeroAbilitySystemComponent::ReplaceManagedSource(FGuid SourceId, const UAbilitySetDefinition* AbilitySetDef, uint32 ExpectedStateRevision)
{
	const auto OperationCheckResult = CheckOperation(ExpectedStateRevision);
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	auto* ExistingGrantRecord = Sources.FindByPredicate([SourceId](const auto& CandidateGrantRecord) { return CandidateGrantRecord.Source == SourceId; });
	if (!ExistingGrantRecord)
	{
		return {EAbilitySourceResult::Failed, TEXT("SourceMissing")};
	}

	if (ExistingGrantRecord->bPendingRemove || !CanCancelRecord(*ExistingGrantRecord))
	{
		return {EAbilitySourceResult::Busy, TEXT("ActiveAbility")};
	}

	if (ExistingGrantRecord->Definition == AbilitySetDef)
	{
		return {EAbilitySourceResult::NoChange};
	}

	const auto DefinitionValidationResult = ValidateDefinition(AbilitySetDef, SourceId);
	if (!DefinitionValidationResult.IsSuccess())
	{
		return DefinitionValidationResult;
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	FHeroGrantRecord ReplacementGrantRecord;
	FName FailureReason;
	if (!StageGrant(AbilitySetDef, SourceId, ReplacementGrantRecord, FailureReason))
	{
		return {EAbilitySourceResult::Failed, FailureReason};
	}

	for (auto Handle : ExistingGrantRecord->Abilities)
	{
		CancelAbilityHandle(Handle);
	}

	if (IsRecordActive(*ExistingGrantRecord))
	{
		RollbackGrant(ReplacementGrantRecord);
		return {EAbilitySourceResult::Busy, TEXT("CancellationIncomplete")};
	}

	const bool bHadSlots = Loadout.Slots.RemoveAll([SourceId](const auto& CandidateSlotEntry) { return CandidateSlotEntry.Source == SourceId; }) > 0;
	ReplacementGrantRecord.DisableReasons = ExistingGrantRecord->DisableReasons;
	ReplacementGrantRecord.Lifetime = ExistingGrantRecord->Lifetime;
	ReplacementGrantRecord.ExpiryTimer = ExistingGrantRecord->ExpiryTimer;
	// 替换不延长临时来源寿命，保留同Source的原timer。
	ExistingGrantRecord->ExpiryTimer.Invalidate();
	RollbackGrant(*ExistingGrantRecord);
	*ExistingGrantRecord = MoveTemp(ReplacementGrantRecord);
	AddSlots(*ExistingGrantRecord);
	UpdateInputBlock(*ExistingGrantRecord, !ExistingGrantRecord->DisableReasons.IsEmpty());
	Publish(bHadSlots || !ExistingGrantRecord->Definition->Abilities.IsEmpty() && Loadout.Slots.ContainsByPredicate([SourceId](const auto& CandidateSlotEntry) { return CandidateSlotEntry.Source == SourceId; }));
	return {EAbilitySourceResult::Applied};
}

FAbilitySourceResult UHeroAbilitySystemComponent::CopyManagedSource(const UHeroAbilitySystemComponent* DonorASC, FGuid SourceId, FGuid NewSourceId, uint32 ExpectedStateRevision)
{
	const auto OperationCheckResult = CheckOperation(ExpectedStateRevision);
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	if (!DonorASC || DonorASC->GetWorld() != GetWorld() || !DonorASC->IsOwnerActorAuthoritative() || DonorASC->bOperating)
	{
		return {EAbilitySourceResult::Failed, TEXT("Donor")};
	}

	const auto* DonorGrantRecord = DonorASC->FindSource(SourceId);
	if (!DonorGrantRecord || DonorGrantRecord->bPendingRemove || !DonorGrantRecord->Definition->bAllowCopy)
	{
		return {EAbilitySourceResult::Failed, TEXT("CopyPolicy")};
	}

	if (!NewSourceId.IsValid() || FindSource(NewSourceId))
	{
		return {EAbilitySourceResult::Failed, TEXT("SourceConflict")};
	}

	const auto DefinitionValidationResult = ValidateDefinition(DonorGrantRecord->Definition);
	if (!DefinitionValidationResult.IsSuccess())
	{
		return DefinitionValidationResult;
	}

	TArray<int32> AbilityLevelOverrides;
	for (auto Handle : DonorGrantRecord->Abilities)
	{
		const auto* Spec = DonorASC->FindAbilitySpecFromHandle(Handle);
		if (!Spec)
		{
			return {EAbilitySourceResult::Failed, TEXT("SpecMissing")};
		}

		AbilityLevelOverrides.Add(Spec->Level);
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	FHeroGrantRecord GrantRecord;
	FName FailureReason;
	if (!StageGrant(DonorGrantRecord->Definition, NewSourceId, GrantRecord, FailureReason, &AbilityLevelOverrides))
	{
		return {EAbilitySourceResult::Failed, FailureReason};
	}

	const int32 PreviousSlots = Loadout.Slots.Num();
	AddSlots(GrantRecord);
	UpdateInputBlock(GrantRecord, false);
	Sources.Add(MoveTemp(GrantRecord));
	Publish(PreviousSlots != Loadout.Slots.Num());
	return {EAbilitySourceResult::Applied};
}

FAbilitySourceResult UHeroAbilitySystemComponent::SetSourceDisabled(FGuid SourceId, FGuid DisableReasonOwnerId, FName Reason, bool bDisabled, uint32 ExpectedStateRevision)
{
	const auto OperationCheckResult = CheckOperation(ExpectedStateRevision);
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	auto* GrantRecord = Sources.FindByPredicate([SourceId](const auto& CandidateGrantRecord) { return CandidateGrantRecord.Source == SourceId; });
	if (!GrantRecord || !DisableReasonOwnerId.IsValid() || Reason.IsNone())
	{
		return {EAbilitySourceResult::Failed, TEXT("DisableReason")};
	}

	if (GrantRecord->bPendingRemove)
	{
		return {EAbilitySourceResult::Busy, TEXT("PendingRemove")};
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	auto* Reasons = GrantRecord->DisableReasons.Find(DisableReasonOwnerId);
	if (bDisabled && Reasons && Reasons->Contains(Reason) || !bDisabled && (!Reasons || !Reasons->Contains(Reason)))
	{
		return {EAbilitySourceResult::NoChange};
	}

	if (bDisabled)
	{
		GrantRecord->DisableReasons.FindOrAdd(DisableReasonOwnerId).Add(Reason);
	}
	else
	{
		Reasons->Remove(Reason);
		if (Reasons->IsEmpty())
		{
			GrantRecord->DisableReasons.Remove(DisableReasonOwnerId);
		}
	}

	UpdateInputBlock(*GrantRecord, !GrantRecord->DisableReasons.IsEmpty());
	Publish(false);
	return {EAbilitySourceResult::Applied};
}

FAbilitySourceResult UHeroAbilitySystemComponent::UpgradeManagedAbility(FGameplayAbilitySpecHandle Handle, int32 Level, uint32 ExpectedStateRevision)
{
	const auto OperationCheckResult = CheckOperation(ExpectedStateRevision);
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	auto* GrantRecord = Sources.FindByPredicate([Handle](const auto& CandidateGrantRecord) { return CandidateGrantRecord.Abilities.Contains(Handle); });
	auto* Spec = FindAbilitySpecFromHandle(Handle);
	if (!GrantRecord || !Spec)
	{
		return {EAbilitySourceResult::Failed, TEXT("SpecMissing")};
	}

	if (GrantRecord->bPendingRemove || Spec->IsActive())
	{
		return {EAbilitySourceResult::Busy, TEXT("ActiveAbility")};
	}

	const auto& AbilityGrant = GrantRecord->Definition->Abilities[GrantRecord->Abilities.IndexOfByKey(Handle)];
	const int32 MaximumLevel = AbilityGrant.MaxLevel > 0 ? AbilityGrant.MaxLevel : AbilityGrant.Level;
	if (Level < Spec->Level || Level > MaximumLevel)
	{
		return {EAbilitySourceResult::Failed, TEXT("Level")};
	}

	if (Level == Spec->Level)
	{
		return {EAbilitySourceResult::NoChange};
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	Spec->Level = Level;
	MarkAbilitySpecDirty(*Spec);
	Publish(false);
	return {EAbilitySourceResult::Applied};
}

FAbilitySourceResult UHeroAbilitySystemComponent::RemapSlots(FGameplayTag FromSlotTag, FGameplayTag ToSlotTag, uint32 ExpectedStateRevision)
{
	const auto OperationCheckResult = CheckOperation(ExpectedStateRevision);
	if (!OperationCheckResult.IsSuccess())
	{
		return OperationCheckResult;
	}

	const auto IsLegal = [](FGameplayTag Slot)
	{
		const FGameplayTag InputSlotRootTag = MobaGameplayTags::Input_Slot_Primary.GetTag().RequestDirectParent();
		return Slot.IsValid() && Slot != InputSlotRootTag && Slot.MatchesTag(InputSlotRootTag);
	};
	if (!IsLegal(FromSlotTag) || !IsLegal(ToSlotTag))
	{
		return {EAbilitySourceResult::Failed, TEXT("Slot")};
	}

	auto* FromSlotEntry = Loadout.Slots.FindByPredicate([FromSlotTag](const auto& CandidateSlotEntry) { return CandidateSlotEntry.Slot == FromSlotTag; });
	if (!FromSlotEntry)
	{
		return {EAbilitySourceResult::Failed, TEXT("SlotMissing")};
	}

	if (FromSlotTag == ToSlotTag)
	{
		return {EAbilitySourceResult::NoChange};
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	auto* ToSlotEntry = Loadout.Slots.FindByPredicate([ToSlotTag](const auto& CandidateSlotEntry) { return CandidateSlotEntry.Slot == ToSlotTag; });
	FromSlotEntry->Slot = ToSlotTag;
	if (ToSlotEntry)
	{
		ToSlotEntry->Slot = FromSlotTag;
	}

	Publish(true);
	return {EAbilitySourceResult::Applied};
}

bool UHeroAbilitySystemComponent::HasReplicatedLoadout(uint32 RequiredRevision) const
{
	if (!RequiredRevision || Loadout.Revision != RequiredRevision || !Loadout.StateRevision)
	{
		return false;
	}

	for (const auto& AbilityState : Loadout.States)
	{
		const auto* Spec = FindAbilitySpecFromHandle(AbilityState.Handle);
		if (!Spec || !Spec->Ability || Spec->Level != AbilityState.Level)
		{
			return false;
		}
	}

	for (const auto& Slot : Loadout.Slots)
	{
		if (!Loadout.States.ContainsByPredicate([&Slot](const auto& CandidateAbilityState) { return CandidateAbilityState.Handle == Slot.Handle; }))
		{
			return false;
		}
	}

	return true;
}

bool UHeroAbilitySystemComponent::IsManagedAbilityEnabled(FGameplayAbilitySpecHandle Handle) const
{
	const auto* AbilityState = Loadout.States.FindByPredicate([Handle](const auto& CandidateAbilityState) { return CandidateAbilityState.Handle == Handle; });
	return AbilityState && AbilityState->bEnabled;
}

FGameplayAbilitySpecHandle UHeroAbilitySystemComponent::FindSlot(FGameplayTag Slot) const
{
	const auto* SlotEntry = Loadout.Slots.FindByPredicate([Slot](const FHeroAbilitySlot& CandidateSlotEntry) { return CandidateSlotEntry.Slot == Slot; });
	return SlotEntry ? SlotEntry->Handle : FGameplayAbilitySpecHandle();
}

FAbilitySourceResult UHeroAbilitySystemComponent::CheckOperation(uint32 ExpectedStateRevision) const
{
	if (!IsInGameThread() || !IsOwnerActorAuthoritative() || bEndingPlay || !GetWorld())
	{
		return {EAbilitySourceResult::Failed, TEXT("Authority")};
	}

	if (bOperating || AbilityScopeLockCount > 0)
	{
		return {EAbilitySourceResult::Busy, TEXT("Operation")};
	}

	if (ExpectedStateRevision != Loadout.StateRevision)
	{
		return {EAbilitySourceResult::Failed, TEXT("StateRevision")};
	}

	return {EAbilitySourceResult::Applied};
}

FAbilitySourceResult UHeroAbilitySystemComponent::ValidateDefinition(const UAbilitySetDefinition* AbilitySetDef, FGuid IgnoredSourceId) const
{
	FString Reason;
	if (!AbilitySetDef || !AbilitySetDef->ValidateGrant(Reason))
	{
		return {EAbilitySourceResult::Failed, FName(Reason.IsEmpty() ? TEXT("Definition") : *Reason)};
	}

	for (const auto& AbilityGrant : AbilitySetDef->Abilities)
	{
		if (!AbilityGrant.Slot.IsValid())
		{
			continue;
		}

		for (const auto& Slot : Loadout.Slots)
		{
			if (Slot.Slot == AbilityGrant.Slot && Slot.Source != IgnoredSourceId)
			{
				return {EAbilitySourceResult::Failed, TEXT("SlotConflict")};
			}
		}
	}

	if (HasTagReactiveContext(AbilitySetDef))
	{
		return {EAbilitySourceResult::Failed, TEXT("TagReactiveContext")};
	}

	if (const auto* Hero = Cast<AHeroCharacter>(GetOwner()))
	{
		for (const auto& EffectClass : AbilitySetDef->Effects)
		{
			const FGameplayEffectSpec Spec(EffectClass.Get()->GetDefaultObject<UGameplayEffect>(), MakeEffectContext(), 1);
			if (!Hero->GetAttributeComponent()->ValidateModifiers(Spec))
			{
				return {EAbilitySourceResult::Failed, TEXT("AttributeModifierPolicy")};
			}
		}
	}

	return {EAbilitySourceResult::Applied};
}

bool UHeroAbilitySystemComponent::StageGrant(const UAbilitySetDefinition* AbilitySetDef, FGuid SourceId, FHeroGrantRecord& GrantRecord, FName& FailureReason, const TArray<int32>* AbilityLevelOverrides)
{
	FailureReason = TEXT("GrantFailed");
	GrantRecord.Source = SourceId;
	GrantRecord.Definition = AbilitySetDef;
	GrantRecord.bInputBlocked = true;
	for (int32 Index = 0; Index < AbilitySetDef->Abilities.Num(); ++Index)
	{
		const auto& AbilityGrant = AbilitySetDef->Abilities[Index];
		int32 InputId = 0;
		while (FindAbilitySpecFromInputID(InputId) || IsAbilityInputBlocked(InputId))
		{
			++InputId;
		}

		// 本项目不用GAS的enum输入绑定；原生Block API不会自行扩容。
		if (GetBlockedAbilityBindings().Num() <= InputId)
		{
			GetBlockedAbilityBindings_Mutable().SetNumZeroed(InputId + 1);
		}

		// 运行时定义由来源记录保活；只有可解析资产作为网络SourceObject。
		UObject* SourceObject = AbilitySetDef->IsAsset() ? const_cast<UAbilitySetDefinition*>(AbilitySetDef) : nullptr;
		FGameplayAbilitySpec AbilitySpec(AbilityGrant.Ability.Get(), AbilityLevelOverrides ? (*AbilityLevelOverrides)[Index] : AbilityGrant.Level, InputId, SourceObject);
		// 事务内 OnGiveAbility 重入不能激活尚未发布的 Spec。
		BlockAbilityByInputID(InputId);
		const auto GrantedAbilityHandle = GiveAbility(AbilitySpec);
		GrantRecord.Abilities.Add(GrantedAbilityHandle);
		GrantRecord.InputIds.Add(InputId);
		if (!FindAbilitySpecFromHandle(GrantedAbilityHandle))
		{
			RollbackGrant(GrantRecord);
			return false;
		}
	}

	if (HasTagReactiveContext(AbilitySetDef))
	{
		RollbackGrant(GrantRecord);
		FailureReason = TEXT("TagReactiveContext");
		return false;
	}

	for (const auto& EffectClass : AbilitySetDef->Effects)
	{
		auto Context = MakeEffectContext();
		if (AbilitySetDef->IsAsset())
		{
			Context.AddSourceObject(AbilitySetDef);
		}

		const auto EffectSpec = MakeOutgoingSpec(EffectClass.Get(), 1, Context);
		if (!EffectSpec.IsValid())
		{
			RollbackGrant(GrantRecord);
			return false;
		}

		const auto AppliedEffectHandle = ApplyGameplayEffectSpecToSelf(*EffectSpec.Data);
		if (!AppliedEffectHandle.IsValid() || !GetActiveGameplayEffect(AppliedEffectHandle))
		{
			RollbackGrant(GrantRecord);
			return false;
		}

		GrantRecord.Effects.Add(AppliedEffectHandle);
	}

	return true;
}

bool UHeroAbilitySystemComponent::HasTagReactiveContext(const UAbilitySetDefinition* AbilitySetDef) const
{
	const bool bAddsTags = AbilitySetDef->Effects.ContainsByPredicate(
		[](const auto& EffectClass)
		{
			const auto* Effect = EffectClass.Get() ? EffectClass.Get()->template GetDefaultObject<UGameplayEffect>() : nullptr;
			return Effect && !Effect->GetGrantedTags().IsEmpty();
		});
	if (!bAddsTags)
	{
		return false;
	}

	// 原生OwnedTag回调绕过项目事务；有该反应面时不尝试可回滚的标签授予。
	for (const auto& TriggeredAbilityEntry : OwnedTagTriggeredAbilities)
	{
		if (!TriggeredAbilityEntry.Value.IsEmpty())
		{
			return true;
		}
	}

	for (auto Handle : GetActiveEffects(FGameplayEffectQuery()))
	{
		const auto* ActiveEffect = GetActiveGameplayEffect(Handle);
		if (ActiveEffect && !UAbilitySetDefinition::HasNonReactiveComponents(ActiveEffect->Spec.Def))
		{
			return true;
		}
	}

	return false;
}

void UHeroAbilitySystemComponent::RollbackGrant(FHeroGrantRecord& GrantRecord)
{
	for (auto Handle : GrantRecord.Abilities)
	{
		ClearAbility(Handle);
	}

	for (auto Handle : GrantRecord.Effects)
	{
		RemoveActiveGameplayEffect(Handle);
	}

	UpdateInputBlock(GrantRecord, false);
	GetWorld()->GetTimerManager().ClearTimer(GrantRecord.ExpiryTimer);
}

void UHeroAbilitySystemComponent::AddSlots(const FHeroGrantRecord& GrantRecord)
{
	for (int32 Index = 0; Index < GrantRecord.Definition->Abilities.Num(); ++Index)
	{
		const FGameplayTag Slot = GrantRecord.Definition->Abilities[Index].Slot;
		if (!Slot.IsValid())
		{
			continue;
		}

		FHeroAbilitySlot& SlotEntry = Loadout.Slots.AddDefaulted_GetRef();
		SlotEntry.Slot = Slot;
		SlotEntry.Handle = GrantRecord.Abilities[Index];
		SlotEntry.Source = GrantRecord.Source;
	}
}

void UHeroAbilitySystemComponent::Publish(bool bMappingChanged)
{
	++Loadout.StateRevision;
	// 1 表示首次完整快照，包括合法的仅效果/空槽初始化。
	if (bMappingChanged || Loadout.Revision == 0)
	{
		++Loadout.Revision;
		if (auto* Hero = Cast<AHeroCharacter>(GetOwner()))
		{
			Hero->CommitLoadoutRevision(Loadout.Revision);
		}
	}

	Loadout.States.Reset();
	for (const auto& GrantRecord : Sources)
	{
		for (auto Handle : GrantRecord.Abilities)
		{
			if (const auto* Spec = FindAbilitySpecFromHandle(Handle))
			{
				auto& AbilityState = Loadout.States.AddDefaulted_GetRef();
				AbilityState.Handle = Handle;
				AbilityState.Level = Spec->Level;
				AbilityState.bEnabled = !GrantRecord.bPendingRemove && GrantRecord.DisableReasons.IsEmpty();
			}
		}
	}

	GetOwner()->ForceNetUpdate();
	OnRep_Loadout();
}

void UHeroAbilitySystemComponent::UpdateInputBlock(FHeroGrantRecord& GrantRecord, bool bBlock)
{
	if (GrantRecord.bInputBlocked == bBlock)
	{
		return;
	}

	for (int32 InputId : GrantRecord.InputIds)
	{
		if (bBlock)
		{
			BlockAbilityByInputID(InputId);
		}
		else
		{
			UnBlockAbilityByInputID(InputId);
		}
	}

	GrantRecord.bInputBlocked = bBlock;
}

FAbilitySourceResult UHeroAbilitySystemComponent::BeginRemove(FGuid SourceId)
{
	auto* GrantRecord = Sources.FindByPredicate([SourceId](const auto& CandidateGrantRecord) { return CandidateGrantRecord.Source == SourceId; });
	if (!GrantRecord)
	{
		return {EAbilitySourceResult::Failed, TEXT("SourceMissing")};
	}

	if (GrantRecord->bPendingRemove)
	{
		return {EAbilitySourceResult::Pending, TEXT("PendingRemove")};
	}

	GrantRecord->bPendingRemove = true;
	GetWorld()->GetTimerManager().ClearTimer(GrantRecord->ExpiryTimer);
	UpdateInputBlock(*GrantRecord, true);
	const bool bMappingChanged = Loadout.Slots.RemoveAll([SourceId](const auto& CandidateSlotEntry) { return CandidateSlotEntry.Source == SourceId; }) > 0;
	for (auto Handle : GrantRecord->Abilities)
	{
		CancelAbilityHandle(Handle);
	}

	// 结束移除标记不拦截全部GameplayEvent路径，保持原生InputID阻断直到最后实例结束。
	for (auto Handle : GrantRecord->Abilities)
	{
		SetRemoveAbilityOnEnd(Handle);
	}

	const bool bWaitingForAbilityEnd = IsRecordActive(*GrantRecord);
	if (!bWaitingForAbilityEnd)
	{
		RollbackGrant(*GrantRecord);
		Sources.RemoveAll([SourceId](const auto& CandidateGrantRecord) { return CandidateGrantRecord.Source == SourceId; });
	}

	Publish(bMappingChanged);
	return {bWaitingForAbilityEnd ? EAbilitySourceResult::Pending : EAbilitySourceResult::Applied};
}

void UHeroAbilitySystemComponent::CleanupPending()
{
	if (bEndingPlay)
	{
		return;
	}

	if (bOperating || AbilityScopeLockCount > 0)
	{
		CleanupTimer = GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ThisClass::CleanupPending);
		return;
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	bool bChanged = false;
	for (int32 Index = Sources.Num() - 1; Index >= 0; --Index)
	{
		if (!Sources[Index].bPendingRemove || IsRecordActive(Sources[Index]))
		{
			continue;
		}

		RollbackGrant(Sources[Index]);
		Sources.RemoveAt(Index);
		bChanged = true;
	}

	if (bChanged)
	{
		Publish(false);
	}
}

void UHeroAbilitySystemComponent::ExpireSource(FGuid SourceId)
{
	if (bEndingPlay)
	{
		return;
	}

	// timer只在Game Thread推进；若回调发生在嵌套帧，保持来源并下一帧重试。
	if (bOperating || AbilityScopeLockCount > 0)
	{
		GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::ExpireSource, SourceId));
		return;
	}

	TGuardValue<bool> OperationGuard(bOperating, true);
	FHeroAttributeBatchScope AttributeBatch(Cast<AHeroCharacter>(GetOwner()));
	BeginRemove(SourceId);
}

bool UHeroAbilitySystemComponent::CanCancelRecord(const FHeroGrantRecord& GrantRecord) const
{
	for (auto Handle : GrantRecord.Abilities)
	{
		const auto* Spec = FindAbilitySpecFromHandle(Handle);
		if (!Spec || !Spec->IsActive())
		{
			continue;
		}

		if (Spec->Ability->GetInstancingPolicy() == EGameplayAbilityInstancingPolicy::NonInstanced)
		{
			if (!Spec->Ability->CanBeCanceled())
			{
				return false;
			}
		}

		for (const auto* AbilityInstance : Spec->GetAbilityInstances())
		{
			if (AbilityInstance->IsActive() && !AbilityInstance->CanBeCanceled())
			{
				return false;
			}
		}
	}

	return true;
}

bool UHeroAbilitySystemComponent::IsRecordActive(const FHeroGrantRecord& GrantRecord) const
{
	for (auto Handle : GrantRecord.Abilities)
	{
		const auto* Spec = FindAbilitySpecFromHandle(Handle);
		if (Spec && Spec->IsActive())
		{
			return true;
		}
	}

	return false;
}

const FHeroGrantRecord* UHeroAbilitySystemComponent::FindSource(FGuid SourceId) const
{
	return Sources.FindByPredicate([SourceId](const FHeroGrantRecord& GrantRecord) { return GrantRecord.Source == SourceId; });
}
void UHeroAbilitySystemComponent::OnRep_Loadout()
{
	GMP::FMessageUtils::SendObjectMessage(this, MOBA_MESSAGE_ABILITY_LOADOUT_CHANGED, Loadout.StateRevision);
}
