#include "Experience/MobaAssetManager.h"

#include "Experience/GameConfiguration.h"
#include "MobaLogChannels.h"

void UMobaAssetManager::StartInitialLoading()
{
	Super::StartInitialLoading();
	
	GameConfiguration = GameConfigurationAsset.LoadSynchronous();
	FString Reason = TEXT("RootObjectUnavailable");
	// 根对象不可用时短路，不访问配置字段；非空才校验，并由校验填写具体原因。
	if (!GameConfiguration || !GameConfiguration->ValidateConfiguration(Reason))
	{
		// 非 Fatal；已加载但配置无效的对象仍保留，允许停 PIE 后修正保存。
		UE_LOG(LogMobaExperience, Error, TEXT("GameConfigurationLoadFailed Asset=%s Reason=%s"), *GameConfigurationAsset.ToString(), *Reason);
		return;
	}

	UE_LOG(LogMobaExperience, Display, TEXT("GameConfigurationLoaded Asset=%s Frontend=%s EnabledModeCount=%d"),
		*GameConfiguration->GetPathName(), *GameConfiguration->FrontendExperience.ToString(), GameConfiguration->EnabledModes.Num());
}
