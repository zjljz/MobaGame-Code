#include "Match/ParticipantState.h"
#include "MobaMessageTags.h"
#include "Match/MatchGameMode.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

void AParticipantState::OnDeactivated()
{
	AMatchGameMode* MatchGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AMatchGameMode>() : nullptr;
	if (MatchGameMode && !GetWorld()->bIsTearingDown && MatchGameMode->OwnsParticipant(this))
	{
		if (!bConnected)
		{
			SetOwner(nullptr);
		}

		SetLifeSpan(0);
		return;
	}

	Super::OnDeactivated();
}

void AParticipantState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AParticipantState, MatchId);
	DOREPLIFETIME(AParticipantState, ParticipantId);
	DOREPLIFETIME(AParticipantState, TeamId);
	DOREPLIFETIME(AParticipantState, ConnectionEpoch);
	DOREPLIFETIME(AParticipantState, bConnected);
	DOREPLIFETIME(AParticipantState, PrimaryHero);
	DOREPLIFETIME_CONDITION(AParticipantState, Economy, COND_OwnerOnly);
}

bool AParticipantState::InitializeEconomy(int32 InitialGold)
{
	if (!HasAuthority() || bEconomyInitialized || InitialGold < 0)
	{
		return false;
	}

	bEconomyInitialized = true;
	Economy.Gold = InitialGold;
	ForceNetUpdate();
	OnRep_Economy();
	return true;
}

void AParticipantState::OnRep_Economy()
{
	GMP::FMessageUtils::SendObjectMessage(this, MOBA_MESSAGE_PARTICIPANT_ECONOMY_CHANGED, Economy.Revision);
}

