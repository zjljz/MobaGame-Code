#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "Experience/GameConfiguration.h"
#include "Experience/MobaAssetManager.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

// 仅既有负向场景临时替换根；不修改保存资产，提前结束或超时也恢复原根。
class FScopedGameConfigurationOverride
{
public:
	explicit FScopedGameConfigurationOverride(UMobaAssetManager& InAssets)
		: Assets(&InAssets), Original(InAssets.GameConfiguration.Get())
	{
		if (Original.IsValid())
		{
			Configuration.Reset(DuplicateObject<UGameConfiguration>(Original.Get(), GetTransientPackage()));
			Configuration->SetFlags(RF_Transient);
			InAssets.GameConfiguration = Configuration.Get();
		}
	}

	~FScopedGameConfigurationOverride()
	{
		if (UMobaAssetManager* Manager = Assets.Get())
		{
			Manager->GameConfiguration = Original.Get();
		}
	}

	FScopedGameConfigurationOverride(const FScopedGameConfigurationOverride&) = delete;
	FScopedGameConfigurationOverride& operator=(const FScopedGameConfigurationOverride&) = delete;


public:
	UGameConfiguration* Get() const { return Configuration.Get(); }


private:
	TWeakObjectPtr<UMobaAssetManager> Assets;

	TStrongObjectPtr<UGameConfiguration> Original;

	TStrongObjectPtr<UGameConfiguration> Configuration;
};
#endif
