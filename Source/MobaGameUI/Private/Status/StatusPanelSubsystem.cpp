#include "Status/StatusPanelSubsystem.h"

#include "ClientPresentationSubsystem.h"
#include "ClientUIConfiguration.h"
#include "Engine/LocalPlayer.h"
#include "Status/StatusPanelWidget.h"

bool UStatusPanelSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer) && !IsRunningDedicatedServer() && !IsRunningCommandlet();
}

void UStatusPanelSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UClientPresentationSubsystem>();
	if (auto* PresentationSubsystem = GetLocalPlayer()->GetSubsystem<UClientPresentationSubsystem>())
	{
		if (const UClientUIConfiguration* Configuration = PresentationSubsystem->GetConfiguration())
		{
			bRegistered = PresentationSubsystem->RegisterLayer(TEXT("Status"), Configuration->StatusClass, 10);
		}
	}
}

void UStatusPanelSubsystem::Deinitialize()
{
	if (bRegistered)
	{
		if (auto* PresentationSubsystem = GetLocalPlayer()->GetSubsystem<UClientPresentationSubsystem>())
		{
			PresentationSubsystem->UnregisterLayer(TEXT("Status"));
		}
	}

	bRegistered = false;
	Super::Deinitialize();
}

