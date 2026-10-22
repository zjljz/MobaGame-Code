#include "Session/MobaSessionSubsystem.h"

#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "MobaLogChannels.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#if !UE_BUILD_SHIPPING
namespace
{
	TAutoConsoleVariable<int32> CVarDevModeIndex(TEXT("moba.DevModeIndex"), -1,
		TEXT("Initial non-Shipping startup: -1 = Frontend; >=0 = EnabledModes index, using that Mode's first ModeConfig."));
}
#endif

EInitialStartupDecision UMobaSessionSubsystem::TryResolveInitialStartup(UWorld* World, const UGameConfiguration* GameConfig, FMatchLaunchSelection& OutLaunchSelection)
{
	if (bInitialStartupHandled || !World || World->GetNetMode() == NM_Client)
	{
		return EInitialStartupDecision::Skip;
	}

	bInitialStartupHandled = true;
	OutLaunchSelection = FMatchLaunchSelection();
#if !UE_BUILD_SHIPPING
	// 显式恢复仍接管启动意图；不让本机开发索引覆盖已有恢复入口。
	if (!FParse::Param(FCommandLine::Get(), TEXT("MobaResume")))
	{
		const int32 ModeIndex = CVarDevModeIndex.GetValueOnGameThread();
		if (ModeIndex == -1)
		{
			return EInitialStartupDecision::Frontend;
		}

		if (!GameConfig || !GameConfig->EnabledModes.IsValidIndex(ModeIndex))
		{
			UE_LOG(LogMobaControl, Error, TEXT("InitialStartupFailed DevModeIndex=%d EnabledModeCount=%d Reason=InvalidDevelopmentIndex"),
				ModeIndex, GameConfig ? GameConfig->EnabledModes.Num() : 0);
			return EInitialStartupDecision::Failure;
		}

		OutLaunchSelection.ModeId = GameConfig->EnabledModes[ModeIndex].ModeId;
		return EInitialStartupDecision::Match;
	}
#endif
	return EInitialStartupDecision::Frontend;
}

bool UMobaSessionSubsystem::LoadCredential(FResumeCredential& OutCredential) const
{
	OutCredential.Clear();
	const int64 CredentialFileSizeBytes = IFileManager::Get().FileSize(*GetCredentialPath());
	if (CredentialFileSizeBytes <= 0 || CredentialFileSizeBytes > 4096)
	{
		return false;
	}

	TArray<uint8> ProtectedCredentialBytes;
	return FFileHelper::LoadFileToArray(ProtectedCredentialBytes, *GetCredentialPath()) && ResumeCredential::Unprotect(ProtectedCredentialBytes, OutCredential);
}

bool UMobaSessionSubsystem::SaveCredential(const FResumeCredential& Credential)
{
	TArray<uint8> ProtectedCredentialBytes;
	if (!ResumeCredential::Protect(Credential, ProtectedCredentialBytes))
	{
		return false;
	}

	const FString CredentialPath = GetCredentialPath(), TemporaryCredentialPath = CredentialPath + TEXT(".tmp");
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(CredentialPath), true) || !FFileHelper::SaveArrayToFile(ProtectedCredentialBytes, *TemporaryCredentialPath))
	{
		return false;
	}

	// 先写保护后的完整临时文件；失败不以明文或仅进程内票据代替持久化。
#if PLATFORM_WINDOWS
	const bool bSaved = MoveFileExW(*FPaths::ConvertRelativePathToFull(TemporaryCredentialPath), *FPaths::ConvertRelativePathToFull(CredentialPath), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
	if (bSaved)
	{
		bFreshJoin = false;
		bResumeRequired = true;
	}

	return bSaved;
#else
	return false;
#endif
}

FString UMobaSessionSubsystem::GetCredentialPath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LocalCredentials/resume.dpapi"));
}

bool UMobaSessionSubsystem::RequiresResume() const
{
	if (bFreshJoin)
	{
		return false;
	}

	if (bResumeRequired || FParse::Param(FCommandLine::Get(), TEXT("MobaResume")))
	{
		return true;
	}

	// 新 PIE GameInstance 默认新准入，不由上一轮磁盘票据推断恢复意图。
	// 显式恢复及本轮成功保存票据后的恢复要求已在上方处理。
	if (const UWorld* World = GetWorld(); World && World->WorldType == EWorldType::PIE)
	{
		return false;
	}

	return IFileManager::Get().FileExists(*GetCredentialPath());
}

