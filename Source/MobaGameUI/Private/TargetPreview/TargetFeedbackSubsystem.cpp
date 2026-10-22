#include "TargetPreview/TargetFeedbackSubsystem.h"

#include "ClientPresentationSubsystem.h"
#include "ClientUIConfiguration.h"
#include "Engine/LocalPlayer.h"
#include "Feedback/ClientFeedbackWidget.h"
#include "MobaLogChannels.h"
#include "TargetPreview/TargetPreviewWidget.h"

bool UTargetFeedbackSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer) && !IsRunningDedicatedServer() && !IsRunningCommandlet();
}

void UTargetFeedbackSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UClientPresentationSubsystem>();
	UClientPresentationSubsystem* PresentationSubsystem = GetLocalPlayer()->GetSubsystem<UClientPresentationSubsystem>();
	if (!PresentationSubsystem)
	{
		UE_LOG(LogMobaControl, Warning, TEXT("TargetFeedbackRegistrationFailed Reason=MissingPresentation"));
		return;
	}

	const UClientUIConfiguration* Configuration = PresentationSubsystem->GetConfiguration();
	if (!Configuration)
	{
		return;
	}

	bPreviewRegistered = PresentationSubsystem->RegisterLayer(TEXT("TargetPreview"), Configuration->TargetPreviewClass, 0, true);
	bFeedbackRegistered = PresentationSubsystem->RegisterLayer(TEXT("Feedback"), Configuration->FeedbackClass, 20);
	if (!bPreviewRegistered || !bFeedbackRegistered)
	{
		UE_LOG(LogMobaControl, Warning, TEXT("TargetFeedbackRegistrationFailed Preview=%d Feedback=%d"), bPreviewRegistered, bFeedbackRegistered);
	}
}

void UTargetFeedbackSubsystem::Deinitialize()
{
	if (UClientPresentationSubsystem* PresentationSubsystem = GetLocalPlayer()->GetSubsystem<UClientPresentationSubsystem>())
	{
		const bool bPreviewRemoved = !bPreviewRegistered || PresentationSubsystem->UnregisterLayer(TEXT("TargetPreview"));
		const bool bFeedbackRemoved = !bFeedbackRegistered || PresentationSubsystem->UnregisterLayer(TEXT("Feedback"));
		if (!bPreviewRemoved || !bFeedbackRemoved)
		{
			UE_LOG(LogMobaControl, Warning, TEXT("TargetFeedbackUnregisterFailed Preview=%d Feedback=%d"), bPreviewRemoved, bFeedbackRemoved);
		}
	}

	bPreviewRegistered = false;
	bFeedbackRegistered = false;
	Super::Deinitialize();
}

