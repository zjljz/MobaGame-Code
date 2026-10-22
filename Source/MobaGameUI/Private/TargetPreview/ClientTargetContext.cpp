#include "TargetPreview/ClientTargetContext.h"

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Engine/World.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "Match/ParticipantState.h"

bool FClientTargetContext::HasSameBinding(const FClientTargetContext& Other) const
{
	return bValid == Other.bValid && Controller == Other.Controller && Hero == Other.Hero && Participant == Other.Participant
		&& Abilities == Other.Abilities && World == Other.World && MatchId == Other.MatchId && EntityId == Other.EntityId
		&& ParticipantId == Other.ParticipantId && Epoch == Other.Epoch;
}

bool FClientTargetContext::HasSamePresentation(const FClientTargetContext& Other) const
{
	return HasSameBinding(Other) && LifeGeneration == Other.LifeGeneration && DeathCount == Other.DeathCount
		&& LifeState == Other.LifeState && bAlive == Other.bAlive && bReady == Other.bReady && Profile == Other.Profile
		&& LoadoutRevision == Other.LoadoutRevision && StateRevision == Other.StateRevision;
}

bool FTargetPreviewSlot::operator==(const FTargetPreviewSlot& Other) const
{
	return Slot == Other.Slot && Handle == Other.Handle && bEnabled == Other.bEnabled;
}

FClientTargetContext MobaClientTarget::ReadContext(AHeroPlayerController* Controller)
{
	FClientTargetContext Result;
	if (!IsValid(Controller) || !Controller->IsLocalController() || !Controller->GetLocalPlayer() || Controller->IsActorBeingDestroyed())
	{
		return Result;
	}

	UWorld* World = Controller->GetWorld();
	AHeroCharacter* Hero = Cast<AHeroCharacter>(Controller->GetPawn());
	AParticipantState* ParticipantState = Controller->GetPlayerState<AParticipantState>();
	if (!World || !World->IsGameWorld() || World->bIsTearingDown || World->GetNetMode() == NM_DedicatedServer
		|| !IsValid(Hero) || Hero->IsActorBeingDestroyed() || !IsValid(ParticipantState) || !ParticipantState->bConnected
		|| Hero->GetWorld() != World || ParticipantState->GetWorld() != World || Hero->GetController() != Controller
		|| ParticipantState->GetOwner() != Controller || ParticipantState->PrimaryHero != Hero
		|| !Hero->EntityId.IsValid() || !Hero->MatchId.IsValid() || !Hero->ParticipantId.IsValid()
		|| Hero->MatchId != ParticipantState->MatchId || Hero->ParticipantId != ParticipantState->ParticipantId
		|| !Hero->ConnectionEpoch || Hero->ConnectionEpoch != ParticipantState->ConnectionEpoch)
	{
		return Result;
	}

	UHeroAbilitySystemComponent* HeroASC = Hero->GetHeroAbilitySystem();
	if (!IsValid(HeroASC))
	{
		return Result;
	}

	const FHeroLifeSnapshot& Life = Hero->GetLifeSnapshot();
	Result.Controller = Controller;
	Result.Hero = Hero;
	Result.Participant = ParticipantState;
	Result.Abilities = HeroASC;
	Result.World = World;
	Result.MatchId = Hero->MatchId;
	Result.EntityId = Hero->EntityId;
	Result.ParticipantId = Hero->ParticipantId;
	Result.Epoch = Hero->ConnectionEpoch;
	Result.LifeGeneration = Life.Generation;
	Result.DeathCount = Life.DeathCount;
	Result.LifeState = uint8(Life.State);
	Result.LoadoutRevision = Hero->RequiredLoadoutRevision;
	Result.StateRevision = HeroASC->GetStateRevision();
	Result.Profile = uint8(Controller->GetActiveProfile());
	Result.bValid = true;
	Result.bAlive = Hero->IsAlive();
	Result.bReady = Controller->IsLocallyReady() && HeroASC->AbilityActorInfo.IsValid()
		&& HeroASC->AbilityActorInfo->AvatarActor.Get() == Hero;
	return Result;
}

TArray<FTargetPreviewSlot> MobaClientTarget::ReadSlots(const FClientTargetContext& Context)
{
	TArray<FTargetPreviewSlot> Result;
	const UHeroAbilitySystemComponent* HeroASC = Context.Abilities.Get();
	if (!Context.bValid || !Context.bReady || !HeroASC || !HeroASC->HasReplicatedLoadout(Context.LoadoutRevision))
	{
		return Result;
	}

	for (const FHeroAbilitySlot& Slot : HeroASC->GetSlots())
	{
		const FGameplayAbilitySpec* Spec = HeroASC->FindAbilitySpecFromHandle(Slot.Handle);
		if (Spec && Spec->Ability)
		{
			Result.Add({Slot.Slot, Slot.Handle, HeroASC->IsManagedAbilityEnabled(Slot.Handle)});
		}
	}

	Result.Sort([](const FTargetPreviewSlot& A, const FTargetPreviewSlot& B) { return A.Slot.ToString() < B.Slot.ToString(); });
	return Result;
}

