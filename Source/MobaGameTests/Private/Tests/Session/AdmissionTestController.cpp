#include "Tests/Session/AdmissionTestController.h"

#include "Match/MatchGameMode.h"

void AAdmissionTestController::ClientStoreCredential_Implementation(const FResumeCredential& Credential)
{
	++CredentialCount;
}

void AAdmissionTestController::ClientAdmissionResult_Implementation(FGuid Request, FName Result)
{
	Super::ClientAdmissionResult_Implementation(Request, Result);
	if (Request.IsValid() && Result.IsNone() && ReleaseOnReservation.IsValid())
	{
		AMatchGameMode* Mode = ReleaseOnReservation.Get();
		ReleaseOnReservation.Reset();
		++ReentrantReleaseCount;
		Mode->ReleaseAdmission(this, TEXT("FixtureReentrantRelease"));
	}
}
