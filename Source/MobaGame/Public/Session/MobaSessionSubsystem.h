#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Session/ResumeCredential.h"
#include "Experience/GameConfiguration.h"
#include "MobaSessionSubsystem.generated.h"

enum class EInitialStartupDecision : uint8
{
	Skip,
	Frontend,
	Match,
	Failure
};

// 持有本机票据和 GameInstance 内的一次性启动标记；服务器参赛记录由 MatchGameMode 拥有。
UCLASS()
class MOBAGAME_API UMobaSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * 每个 GameInstance 只解析一次初始入口；已处理、无 World 或客户端返回 Skip，不改 Selection。
	 * 非 Shipping 用 moba.DevModeIndex 选择启用模式；Match 的实际配置名由加载器读取 ModeConfigs[0] 后解析。
	 * 无效开发设置返回 Failure；不降级为 Frontend，也不重新尝试本次初始启动。
	 */
	EInitialStartupDecision TryResolveInitialStartup(UWorld* World, const UGameConfiguration* GameConfig, FMatchLaunchSelection& OutLaunchSelection);


	// 本机票据读写
	bool LoadCredential(FResumeCredential& OutCredential) const;

	/**
	 * 用户级保护后原子替换；失败保留旧文件，不允许明文或仅内存后备。
	 */
	bool SaveCredential(const FResumeCredential& Credential);

	FString GetCredentialPath() const;

	bool RequiresResume() const;


	// 连接方式选择
	void RequireResume()
	{
		// 显式连接接管启动意图，后续 World 不再解析本地默认入口。
		bInitialStartupHandled = true;
		bResumeRequired = true;
		bFreshJoin = false;
	}

	void RequestFreshJoin()
	{
		// 显式连接接管启动意图，后续 World 不再解析本地默认入口。
		bInitialStartupHandled = true;
		bFreshJoin = true;
		bResumeRequired = false;
	}


private:
	bool bInitialStartupHandled = false;

	bool bResumeRequired = false;

	bool bFreshJoin = false;
};

