#include "Status/StatusPanelSnapshot.h"

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Abilities/GameplayAbility.h"
#include "Experience/ExperienceDefinitions.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Hero/HeroCharacter.h"
#include "Hero/HeroDefinition.h"
#include "Hero/HeroAttributeComponent.h"
#include "Player/HeroPlayerController.h"
#include "Items/ItemInventoryComponent.h"
#include "Match/ParticipantState.h"

FStatusPanelSnapshot MobaStatus::ReadOwnedStatus(AHeroPlayerController* Controller, const UMatchRulesDefinition* Rules)
{
	check(IsInGameThread());
	FStatusPanelSnapshot Result;
	if (!IsValid(Controller) || Controller->IsActorBeingDestroyed() || !Controller->IsLocalController() || !Controller->GetLocalPlayer())
	{
		return Result;
	}

	const UWorld* World = Controller->GetWorld();
	const auto* ParticipantState = Controller->GetPlayerState<AParticipantState>();
	const auto* Hero = Cast<AHeroCharacter>(Controller->GetPawn());
	if (!World || World->bIsTearingDown || World->GetNetMode() == NM_DedicatedServer
		|| !IsValid(ParticipantState) || ParticipantState->IsActorBeingDestroyed() || !IsValid(Hero) || Hero->IsActorBeingDestroyed()
		|| Hero->GetWorld() != World || ParticipantState->GetWorld() != World || Hero->GetController() != Controller
		|| Hero->GetOwner() != Controller || ParticipantState->GetOwner() != Controller || ParticipantState->PrimaryHero != Hero
		|| !ParticipantState->bConnected || !Hero->EntityId.IsValid() || !Hero->MatchId.IsValid() || !Hero->ParticipantId.IsValid()
		|| ParticipantState->ParticipantId != Hero->ParticipantId || ParticipantState->MatchId != Hero->MatchId
		|| Hero->ConnectionEpoch == 0 || ParticipantState->ConnectionEpoch != Hero->ConnectionEpoch)
	{
		return Result;
	}

	Result.bOwnedHero = true;
	Result.Entity = Hero->EntityId;
	Result.Epoch = Hero->ConnectionEpoch;
	const FHeroLifeSnapshot Life = Hero->GetLifeSnapshot();
	Result.LifeGeneration = Life.Generation;
	Result.LifeState = Life.State;
	if (!Rules || !Hero->GetDefinition() || Hero->InitializationGeneration == 0 || Life.Generation == 0)
	{
		return Result;
	}

	auto* HeroASC = Hero->GetHeroAbilitySystem();
	const auto* HeroAttributeSet = Hero->GetAttributes();
	if (!IsValid(HeroASC) || !HeroAttributeSet || !HeroASC->AbilityActorInfo.IsValid()
		|| HeroASC->AbilityActorInfo->OwnerActor.Get() != Hero || HeroASC->AbilityActorInfo->AvatarActor.Get() != Hero
		|| HeroASC->AbilityActorInfo->PlayerController.Get() != Controller)
	{
		return Result;
	}

	Result.bUsesMana = (Hero->GetDefinition()->ResourceType == EHeroResourceType::Mana);
	const float Health = HeroAttributeSet->GetHealth();
	const float MaxHealth = HeroAttributeSet->GetMaxHealth();
	const float Shield = HeroAttributeSet->GetShield();
	const float Mana = HeroAttributeSet->GetMana();
	const float MaxMana = HeroAttributeSet->GetMaxMana();
	// Life 与属性可能分批到达：不显示 Alive/0HP 或 Dead/旧HP 的混合状态。
	const bool bLifeMatchesHealth = (Life.State == EHeroLifeState::Alive && Health > 0)
		|| (Life.State == EHeroLifeState::Dead && Health == 0);
	if (bLifeMatchesHealth && FMath::IsFinite(Health) && FMath::IsFinite(MaxHealth) && MaxHealth > 0
		&& Health >= 0 && Health <= MaxHealth && FMath::IsFinite(Shield) && Shield >= 0
		&& FMath::IsFinite(Mana) && FMath::IsFinite(MaxMana) && MaxMana >= 0 && Mana >= 0 && Mana <= MaxMana)
	{
		Result.bCombatReady = true;
		Result.Health = Health;
		Result.MaxHealth = MaxHealth;
		Result.Shield = Shield;
		Result.Mana = Mana;
		Result.MaxMana = MaxMana;
		const AGameStateBase* GameState = World->GetGameState();
		if (Life.State == EHeroLifeState::Dead && GameState && FMath::IsFinite(Life.RespawnServerTime))
		{
			Result.RespawnRemaining = static_cast<float>(FMath::Max(0.0, static_cast<double>(Life.RespawnServerTime) - GameState->GetServerWorldTimeSeconds()));
		}
	}

	const auto* AttributeComp = Hero->GetAttributeComponent();
	const auto& Thresholds = Rules->AttributeRules.ExperienceThresholds;
	const float Level = HeroAttributeSet->GetHeroLevel();
	const int64 Experience = AttributeComp ? AttributeComp->GetTotalExperiencePoints() : -1;
	if (FMath::IsFinite(Level) && Level >= 1 && Level <= Thresholds.Num() && FMath::FloorToFloat(Level) == Level)
	{
		const int32 LevelIndex = int32(Level) - 1;
		const bool bMaximum = (LevelIndex + 1 == Thresholds.Num());
		const int64 Lower = Thresholds[LevelIndex];
		const int64 Upper = bMaximum ? Lower : Thresholds[LevelIndex + 1];
		// Level 与 OwnerOnly XP 到达并不原子；只在同一门槛区间时展示，后续事件使其收敛。
		if (Experience >= Lower && (bMaximum ? Experience == Upper : Experience < Upper) && (bMaximum || Upper > Lower))
		{
			Result.bProgressionReady = true;
			Result.bMaximumLevel = bMaximum;
			Result.HeroLevel = int32(Level);
			Result.MaximumHeroLevel = Thresholds.Num();
			Result.TotalExperiencePoints = Experience;
			Result.LevelExperiencePoints = Experience - Lower;
			Result.RequiredExperiencePoints = Upper - Lower;
			Result.ExperienceProgress = bMaximum ? 1.0f : float(double(Experience - Lower) / double(Upper - Lower));
		}
	}

	Result.LoadoutRevision = HeroASC->GetLoadoutRevision();
	Result.AbilityStateRevision = HeroASC->GetStateRevision();
	if (!HeroASC->HasReplicatedLoadout(Hero->RequiredLoadoutRevision))
	{
		return Result;
	}

	TArray<FStatusAbilityRow> Rows;
	TSet<FGameplayTag> SeenSlots;
	for (const FHeroAbilitySlot& Slot : HeroASC->GetSlots())
	{
		const FGameplayAbilitySpec* Spec = HeroASC->FindAbilitySpecFromHandle(Slot.Handle);
		const UGameplayAbility* Ability = Spec ? (Spec->GetPrimaryInstance() ? Spec->GetPrimaryInstance() : Spec->Ability.Get()) : nullptr;
		if (!Slot.Slot.IsValid() || SeenSlots.Contains(Slot.Slot) || !Ability || Spec->Level < 1 || HeroASC->FindSlot(Slot.Slot) != Slot.Handle)
		{
			return Result;
		}

		SeenSlots.Add(Slot.Slot);
		FStatusAbilityRow& Row = Rows.AddDefaulted_GetRef();
		Row.Slot = Slot.Slot;
		Row.Handle = Slot.Handle;
		FString AbilityName = Ability->GetClass()->GetName();
		AbilityName.RemoveFromEnd(TEXT("_C"));
		AbilityName.RemoveFromStart(TEXT("GA_"));
		Row.Name = FText::FromString(AbilityName);
		Row.Level = Spec->Level;
		Row.bEnabled = HeroASC->IsManagedAbilityEnabled(Slot.Handle);
		Ability->GetCooldownTimeRemainingAndDuration(Slot.Handle, HeroASC->AbilityActorInfo.Get(), Row.CooldownRemaining, Row.CooldownDuration);
		const FGameplayTagContainer* CooldownTags = Ability->GetCooldownTags();
		const bool bHasCooldownTag = CooldownTags && HeroASC->HasAnyMatchingGameplayTags(*CooldownTags);
		Row.bCooldownKnown = FMath::IsFinite(Row.CooldownRemaining) && FMath::IsFinite(Row.CooldownDuration)
			&& Row.CooldownRemaining >= 0 && Row.CooldownDuration >= 0 && (!bHasCooldownTag || Row.CooldownRemaining > 0);
	}

	Rows.Sort([](const FStatusAbilityRow& Left, const FStatusAbilityRow& Right) { return Left.Slot.ToString() < Right.Slot.ToString(); });
	Result.Abilities = MoveTemp(Rows);
	Result.bAbilitiesReady = true;
	Result.bItemsEnabled = Rules->ItemEconomyRules.bItemsEnabled;
	const UItemInventoryComponent* InventoryComp = Hero->GetInventory();
	if (!InventoryComp)
	{
		return Result;
	}

	const FItemInventorySnapshot InventoryState = InventoryComp->GetSnapshot();
	const FParticipantEconomy Economy = ParticipantState->GetEconomy();
	if (InventoryState.Revision != Economy.Revision || Economy.Gold < 0
		|| InventoryState.AbilityStateRevision > Result.AbilityStateRevision || InventoryState.Items.Num() > 6
		|| (InventoryState.Revision == 0 && (Economy.Gold != Rules->ItemEconomyRules.InitialGold || !InventoryState.Items.IsEmpty()))
		|| (InventoryState.Revision > 0 && InventoryState.AbilityStateRevision == 0)
		|| (!Rules->ItemEconomyRules.bItemsEnabled && !InventoryState.Items.IsEmpty()))
	{
		return Result;
	}

	TStaticArray<FStatusItemRow, 6> Items;
	TSet<FGuid> SeenInstances;
	for (const FItemInstance& Item : InventoryState.Items)
	{
		if (Item.Slot < 0 || Item.Slot >= 6 || !Item.Id.IsValid() || Item.ItemId.IsNone() || Item.Quantity < 1
			|| Items[Item.Slot].Instance.IsValid() || SeenInstances.Contains(Item.Id))
		{
			return Result;
		}

		if (Item.ActiveAbility.IsValid() && !HeroASC->FindAbilitySpecFromHandle(Item.ActiveAbility))
		{
			return Result;
		}

		SeenInstances.Add(Item.Id);
		Items[Item.Slot] = {Item.Id, Item.ItemId, Item.Quantity};
	}

	Result.Items = MoveTemp(Items);
	Result.Gold = Economy.Gold;
	Result.InventoryRevision = InventoryState.Revision;
	Result.bInventoryReady = true;
	return Result;
}

