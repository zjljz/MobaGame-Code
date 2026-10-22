#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetManager.h"
#include "MobaAssetManager.generated.h"

class UGameConfiguration;

UCLASS(Config = Game)
class MOBAGAME_API UMobaAssetManager : public UAssetManager
{
	GENERATED_BODY()

public:
	/**
	 * 进程级 AssetManager 初始化后同步加载纯数据根；不限定 PIE，也不在每个 World 开始时重载。
	 * 缺失或字段校验失败只记录诊断，允许 Editor 继续启动以修复配置。
	 */
	virtual void StartInitialLoading() override;


public:
	const UGameConfiguration* GetGameConfiguration() const { return GameConfiguration; }


private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FScopedGameConfigurationOverride;
#endif

	// 由 Game 配置层级读取根资产软路径，项目默认值在 DefaultGame.ini；不是 C++ 写死路径。
	// INI 只定位根资产，开局选择和允许集合在资产中维护。
	UPROPERTY(Config)
	TSoftObjectPtr<UGameConfiguration> GameConfigurationAsset;

	// 同步加载后强持有的根对象；对象存在与配置字段是否合法是两项不同检查。
	UPROPERTY(Transient)
	TObjectPtr<UGameConfiguration> GameConfiguration;
};
