#include "Items/ItemInventoryComponent.h"

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "MobaMessageTags.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/SelfCombatEffectAbility.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Items/ItemDefinition.h"
#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceSubsystem.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "Match/ParticipantState.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"

UItemInventoryComponent::UItemInventoryComponent()
{
	SetIsReplicatedByDefault(true);
}

void UItemInventoryComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(UItemInventoryComponent, Snapshot, COND_OwnerOnly);
}

FItemResult UItemInventoryComponent::Execute(AHeroPlayerController* Controller, const FItemRequest& Request)
{
	FItemResult Result;
	Result.Epoch = Request.Epoch;
	Result.Sequence = Request.Sequence;
	Result.Revision = Snapshot.Revision;
	auto Reject = [&Result](FName Reason)
	{
		Result.Reason = Reason;
		return Result;
	};
	const auto* Hero = Cast<AHeroCharacter>(GetOwner());
	const auto* ParticipantState = Controller ? Controller->GetPlayerState<AParticipantState>() : nullptr;
	if (!IsInGameThread() || !Hero || !Hero->HasAuthority() || !Controller || !Controller->HasAuthority() || !ParticipantState || ParticipantState->PrimaryHero != Hero
		|| ParticipantState->GetOwner() != Controller || !ParticipantState->bConnected || Controller->GetPawn() != Hero || Hero->GetController() != Controller)
	{
		return Reject(TEXT("Binding"));
	}

	if (Request.Entity != Hero->EntityId)
	{
		return Reject(TEXT("Entity"));
	}

	if (Request.Epoch != ParticipantState->ConnectionEpoch || Request.Epoch != Hero->ConnectionEpoch)
	{
		return Reject(TEXT("Epoch"));
	}

	if (bOperating)
	{
		return Reject(TEXT("Busy"));
	}

	if (RequestEpoch != Request.Epoch)
	{
		RequestEpoch = Request.Epoch;
		HighWater = 0;
		Results.Reset();
	}

	if (const auto* CachedItemResult = Results.FindByPredicate([&Request](const auto& CachedResult) { return CachedResult.Sequence == Request.Sequence; }))
	{
		return *CachedItemResult;
	}

	if (!Request.Sequence || Request.Sequence <= HighWater)
	{
		return Reject(TEXT("OldSequence"));
	}

	TGuardValue<bool> InventoryOperationGuard(bOperating, true);
	HighWater = Request.Sequence;
	const auto* Rules = GetWorld()->GetSubsystem<UExperienceSubsystem>()->GetRules();
	Result.Reason = Apply(Controller, Request, Rules);
	Result.bAccepted = Result.Reason.IsNone();
	Result.Revision = Snapshot.Revision;
	Results.Add(Result);
	if (Results.Num() > 32)
	{
		Results.RemoveAt(0);
	}

	return Result;
}

FName UItemInventoryComponent::Apply(AHeroPlayerController* Controller, const FItemRequest& Request, const UMatchRulesDefinition* Rules)
{
	auto* Hero = CastChecked<AHeroCharacter>(GetOwner());
	auto* ParticipantState = Controller->GetPlayerState<AParticipantState>();
	auto* HeroASC = Hero->GetHeroAbilitySystem();
	FString Reason;
	if (!Rules || !Rules->ItemEconomyRules.bItemsEnabled)
	{
		return TEXT("ItemsDisabled");
	}

	if (!Controller->ValidateIntent(Request.Entity, Request.Epoch, Hero->RequiredLoadoutRevision, Reason))
	{
		return FName(*Reason);
	}

	if (Request.Revision != Snapshot.Revision)
	{
		return TEXT("InventoryRevision");
	}

	if (Request.Quantity < 1 || Request.Quantity > 3 || (Request.Operation != EItemOperation::Buy && Request.Quantity != 1))
	{
		return TEXT("Quantity");
	}

	if (!ParticipantState->bEconomyInitialized)
	{
		return TEXT("EconomyNotReady");
	}

	const auto* Catalog = Rules->ItemEconomyRules.ItemCatalog.Get();
	if (!Catalog || !Catalog->ValidateCatalog(Reason))
	{
		return TEXT("CatalogUnavailable");
	}

	if (Request.Operation != EItemOperation::Use)
	{
		if (!FMath::IsFinite(Rules->ItemEconomyRules.ShopRadius) || Rules->ItemEconomyRules.ShopRadius <= 0 || Rules->ItemEconomyRules.ShopCenter.ContainsNaN() || Hero->GetActorLocation().ContainsNaN()
			|| FVector::DistSquared(Hero->GetActorLocation(), Rules->ItemEconomyRules.ShopCenter) > FMath::Square(Rules->ItemEconomyRules.ShopRadius))
		{
			return TEXT("ShopRange");
		}
	}

	TArray<FItemInstance> ProposedItems = Snapshot.Items;
	int64 Gold = ParticipantState->Economy.Gold;
	const UItemDefinition* ItemDef = nullptr;
	if (Request.Operation == EItemOperation::Buy || Request.Operation == EItemOperation::Combine)
	{
		ItemDef = Catalog->FindItem(Request.ItemId);
		if (!ItemDef)
		{
			return TEXT("ItemNotAllowed");
		}

		if (Request.Operation == EItemOperation::Buy && !ItemDef->Recipe.IsEmpty())
		{
			return TEXT("RecipeRequired");
		}
	}
	else
	{
		const auto* ExistingItemInstance = ProposedItems.FindByPredicate([&Request](const auto& ItemInstance) { return ItemInstance.Id == Request.Instance; });
		if (!ExistingItemInstance)
		{
			return TEXT("Instance");
		}

		ItemDef = Catalog->FindItem(ExistingItemInstance->ItemId);
		if (!ItemDef)
		{
			return TEXT("ItemNotAllowed");
		}
	}

	if (Request.Operation == EItemOperation::Use)
	{
		auto* ItemInstance = ProposedItems.FindByPredicate([&Request](const auto& CandidateItemInstance) { return CandidateItemInstance.Id == Request.Instance; });
		auto* Spec = HeroASC->FindAbilitySpecFromHandle(ItemInstance->ActiveAbility);
		auto* Ability = Spec ? Cast<USelfCombatEffectAbility>(Spec->GetPrimaryInstance()) : nullptr;
		if (!Ability || !HeroASC->IsManagedAbilityEnabled(ItemInstance->ActiveAbility))
		{
			return TEXT("NotUsable");
		}

		const bool bConsumesLastStackUnit = ItemDef->bConsumable && ItemInstance->Quantity == 1;
		if (bConsumesLastStackUnit)
		{
			const auto SourceExchangeCheckResult = HeroASC->CanExchangeItemSources({ItemInstance->Id}, HeroASC->GetStateRevision());
			if (!SourceExchangeCheckResult.IsSuccess())
			{
				return SourceExchangeCheckResult.Reason;
			}
		}

		if (!Ability->CheckCooldown(ItemInstance->ActiveAbility, HeroASC->AbilityActorInfo.Get()))
		{
			return TEXT("Cooldown");
		}

		if (ItemDef->bConsumable && Hero->GetAttributes()->GetHealth() >= Hero->GetAttributes()->GetMaxHealth())
		{
			return TEXT("FullHealth");
		}

		if (!Ability->CanActivateAbility(ItemInstance->ActiveAbility, HeroASC->AbilityActorInfo.Get()))
		{
			return TEXT("ActivationBlocked");
		}

		const auto ActivationResult = HeroASC->ActivateItemSource(ItemInstance->Id, bConsumesLastStackUnit);
		if (!ActivationResult.IsSuccess())
		{
			return ActivationResult.Reason;
		}

		if (ItemDef->bConsumable)
		{
			--ItemInstance->Quantity;
			if (bConsumesLastStackUnit)
			{
				ProposedItems.RemoveAll([&Request](const auto& CandidateItemInstance) { return CandidateItemInstance.Id == Request.Instance; });
			}
		}
	}
	else
	{
		if (Request.Operation == EItemOperation::Sell)
		{
			auto* ItemInstance = ProposedItems.FindByPredicate([&Request](const auto& CandidateItemInstance) { return CandidateItemInstance.Id == Request.Instance; });
			Gold += ItemDef->Price / 2;
			--ItemInstance->Quantity;
			ProposedItems.RemoveAll([](const auto& CandidateItemInstance) { return CandidateItemInstance.Quantity == 0; });
		}
		else if (Request.Operation == EItemOperation::Buy || Request.Operation == EItemOperation::Combine)
		{
			int64 Cost = static_cast<int64>(ItemDef->Price) * Request.Quantity;
			if (Request.Operation == EItemOperation::Combine)
			{
				if (ItemDef->Recipe.IsEmpty())
				{
					return TEXT("NoRecipe");
				}

				for (const auto& RecipeIngredientDef : ItemDef->Recipe)
				{
					const auto* ComponentDefinition = RecipeIngredientDef.Get();
					auto* ItemInstance = ProposedItems.FindByPredicate([ComponentDefinition](const auto& CandidateItemInstance) { return CandidateItemInstance.ItemId == ComponentDefinition->ItemId && CandidateItemInstance.Quantity > 0; });
					if (!ItemInstance)
					{
						return TEXT("MissingComponent");
					}

					--ItemInstance->Quantity;
					Cost -= ComponentDefinition->Price;
				}

				ProposedItems.RemoveAll([](const auto& CandidateItemInstance) { return CandidateItemInstance.Quantity == 0; });
			}

			Gold -= Cost;
			if (Cost < 0 || Gold < 0)
			{
				return TEXT("InsufficientGold");
			}

			for (int32 PurchasedUnitIndex = 0; PurchasedUnitIndex < Request.Quantity; ++PurchasedUnitIndex)
			{
				auto* StackableItemInstance = ProposedItems.FindByPredicate([ItemDef](const auto& ItemInstance) { return ItemInstance.ItemId == ItemDef->ItemId && ItemInstance.Quantity < ItemDef->MaxStack; });
				if (StackableItemInstance)
				{
					++StackableItemInstance->Quantity;
					continue;
				}

				if (ProposedItems.Num() >= 6)
				{
					return TEXT("InventoryFull");
				}

				int32 FreeSlotIndex = 0;
				while (ProposedItems.ContainsByPredicate([FreeSlotIndex](const auto& ItemInstance) { return ItemInstance.Slot == FreeSlotIndex; }))
				{
					++FreeSlotIndex;
				}

				auto& ItemInstance = ProposedItems.AddDefaulted_GetRef();
				ItemInstance.Id = FGuid::NewGuid();
				ItemInstance.ItemId = ItemDef->ItemId;
				ItemInstance.Slot = FreeSlotIndex;
				ItemInstance.Quantity = 1;
			}
		}
		else
		{
			return TEXT("Operation");
		}

		if (Gold < 0 || Gold > MAX_int32)
		{
			return TEXT("GoldOverflow");
		}

		TArray<FGuid> SourceIdsToRemove, SourceIdsToAdd;
		TArray<const UAbilitySetDefinition*> AbilitySetDefsToAdd;
		for (const auto& PreviousItemInstance : Snapshot.Items)
		{
			if (!ProposedItems.ContainsByPredicate([&PreviousItemInstance](const auto& ItemInstance) { return ItemInstance.Id == PreviousItemInstance.Id; }))
			{
				SourceIdsToRemove.Add(PreviousItemInstance.Id);
			}
		}

		for (const auto& ItemInstance : ProposedItems)
		{
			if (!Snapshot.Items.ContainsByPredicate([&ItemInstance](const auto& PreviousItemInstance) { return ItemInstance.Id == PreviousItemInstance.Id; }))
			{
				SourceIdsToAdd.Add(ItemInstance.Id);
				AbilitySetDefsToAdd.Add(Catalog->FindItem(ItemInstance.ItemId)->AbilitySet.Get());
			}
		}

		const auto SourceExchangeResult = HeroASC->ExchangeItemSources(SourceIdsToRemove, SourceIdsToAdd, AbilitySetDefsToAdd, HeroASC->GetStateRevision());
		if (!SourceExchangeResult.IsSuccess())
		{
			return SourceExchangeResult.Reason.IsNone() ? FName(TEXT("GrantFailed")) : SourceExchangeResult.Reason;
		}

		for (auto& ItemInstance : ProposedItems)
		{
			const auto* GrantRecord = HeroASC->FindSource(ItemInstance.Id);
			if (GrantRecord && !GrantRecord->Abilities.IsEmpty())
			{
				ItemInstance.ActiveAbility = GrantRecord->Abilities[0];
			}
		}
	}

	Snapshot.Items = MoveTemp(ProposedItems);
	++Snapshot.Revision;
	Snapshot.AbilityStateRevision = HeroASC->GetStateRevision();
	ParticipantState->Economy.Gold = static_cast<int32>(Gold);
	ParticipantState->Economy.Revision = Snapshot.Revision;
	ParticipantState->ForceNetUpdate();
	Hero->ForceNetUpdate();
	ParticipantState->OnRep_Economy();
	OnRep_Snapshot();
	return NAME_None;
}

void UItemInventoryComponent::OnRep_Snapshot()
{
	GMP::FMessageUtils::SendObjectMessage(this, MOBA_MESSAGE_ITEM_INVENTORY_CHANGED, Snapshot.Revision);
}

