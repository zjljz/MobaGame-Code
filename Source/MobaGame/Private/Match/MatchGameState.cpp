#include "Match/MatchGameState.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "Player/HeroPlayerController.h"

AMatchGameState::AMatchGameState()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AMatchGameState::BeginPlay()
{
	Super::BeginPlay();
	OnRep_ExperienceSelection();
}

void AMatchGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMatchGameState, MatchId);
	DOREPLIFETIME(AMatchGameState, bMatchEnded);
	DOREPLIFETIME(AMatchGameState, ExperienceInstanceId);
	DOREPLIFETIME(AMatchGameState, Selection);
	DOREPLIFETIME(AMatchGameState, Teams);
}

void AMatchGameState::OnRep_ExperienceSelection()
{
	if (auto* Controller = Cast<AHeroPlayerController>(GetWorld()->GetFirstPlayerController()))
	{
		Controller->RefreshExperienceLoading();
	}
}
