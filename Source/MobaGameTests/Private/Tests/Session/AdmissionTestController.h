#pragma once

#include "Player/HeroPlayerController.h"
#include "Engine/LevelStreamingDynamic.h"
#include "AdmissionTestController.generated.h"

class AMatchGameMode;

/**
 * 准入 World fixture 的拥有端边界；只截获凭证落盘；准入结果沿用真实 Controller 回调。
 */
UCLASS()
class AAdmissionTestController : public AHeroPlayerController
{
	GENERATED_BODY()

public:
	virtual void ClientStoreCredential_Implementation(const FResumeCredential& Credential) override;

	virtual void ClientAdmissionResult_Implementation(FGuid Request, FName Result) override;


public:
	int32 CredentialCount = 0;

	// 可选故障注入始终先运行基类真实回调；模拟同步 Ready 失败释放席位。
	TWeakObjectPtr<AMatchGameMode> ReleaseOnReservation;

	int32 ReentrantReleaseCount = 0;
};

/**
 * 仅为准入状态检查借用瞬态 Level；不代表真实 streaming 行为验证。
 */
UCLASS()
class UAdmissionTestStreaming : public ULevelStreamingDynamic
{
	GENERATED_BODY()

public:
	// 只设置准入谓词所需的借用引用，不把 PersistentLevel 交给原生 streaming/卸载流程。
	void BindFixtureLevel(ULevel* Level) { LoadedLevel = Level; }
};
