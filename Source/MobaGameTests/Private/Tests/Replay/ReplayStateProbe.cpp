#include "Tests/Replay/ReplayStateProbe.h"

#include "Components/SceneComponent.h"
#include "Net/UnrealNetwork.h"

AReplayStateProbe::AReplayStateProbe()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Movable);
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(30);
}

void AReplayStateProbe::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AReplayStateProbe, SampleIndex);
}
