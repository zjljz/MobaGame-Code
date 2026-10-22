#include "Session/MobaGameInstance.h"

#include "Engine/Engine.h"
#include "Session/GameFlowSubsystem.h"
#include "Session/MobaSessionSubsystem.h"

void UMobaGameInstance::MobaResume(const FString& Address)
{
	Connect(Address, true);
}

void UMobaGameInstance::MobaJoin(const FString& Address)
{
	Connect(Address, false);
}

void UMobaGameInstance::MobaFrontend()
{
	GetSubsystem<UGameFlowSubsystem>()->RequestFrontend();
}

void UMobaGameInstance::Connect(const FString& Address, bool bResume)
{
	if (Address.IsEmpty() || Address.Len() > 253)
	{
		return;
	}

	for (TCHAR Character : Address)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('.') && Character != TEXT(':') && Character != TEXT('-'))
		{
			return;
		}
	}

	GetSubsystem<UGameFlowSubsystem>()->BeginRemoteConnection();
	if (bResume)
	{
		GetSubsystem<UMobaSessionSubsystem>()->RequireResume();
	}
	else
	{
		GetSubsystem<UMobaSessionSubsystem>()->RequestFreshJoin();
	}

	GEngine->SetClientTravel(GetWorld(), *Address, TRAVEL_Absolute);
}
