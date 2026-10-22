#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Experience/GameConfiguration.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "GameFlowSubsystem.generated.h"

class UExperienceSubsystem;
struct FExperienceSnapshot;

UENUM(BlueprintType)
enum class EGameFlowState : uint8
{
	Frontend,
	StartingMatch,
	InMatch,
	ReturningFrontend,
	Failed
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FGameFlowSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	EGameFlowState State = EGameFlowState::Frontend;

	UPROPERTY(BlueprintReadOnly)
	FText Failure;

	UPROPERTY(BlueprintReadOnly)
	int64 Generation = 0;
};

/** 本地导航与跨 World 启动意图；不拥有服务器参赛者、内容加载状态或战斗规则。 */
UCLASS()
class MOBAGAME_API UGameFlowSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	virtual void Deinitialize() override;


public:
	// World 交接
	void EnterFrontend(UWorld* World);

	/**
	 * 仅配置的本地比赛宿主可认领一次；不将客户端意图当成远程服务器许可。
	 */
	bool ClaimLocalMatch(UWorld* World, FMatchLaunchSelection& OutLaunchSelection);

	void ObserveMatch(UWorld* World);

	void NotifyMatchReady(UWorld* World);

	void NotifyMatchEnded(UWorld* World);

	void NotifyStartupFailure();

	void NotifyAdmissionFailure(UWorld* World, FName Reason);

	void BeginRemoteConnection();


	// 本地意图
	/**
	 * 非 Shipping 本地开发入口；组合仍由加载器从所选 Mode 的首项解析。
	 */
	bool RequestLocalMatch(FPrimaryAssetId ModeId);

	/**
	 * 本地比赛结束与远程客户端离开分别处理；不替远程服务器结束比赛。
	 */
	bool RequestFrontend();


private:
	// 内容停止与 travel
	void ObserveContent(UWorld* World);

	void QueueAdvance();

	/**
	 * 根据当前导航状态和所观察的 Experience 状态推进流程。
	 * 返回前台时先清理本地输入、结束本地比赛并等待内容停止，再进入前台或切换地图；
	 * 启动本地比赛时等待前台内容停止，内容加载失败时转入失败或返回流程。
	 * 由 QueueAdvance 延后调用；条件尚未满足时不推进，后续内容状态通知可再次排队。
	 */
	void Advance();

	void TravelTo(const TSoftObjectPtr<UWorld>& Destination);

	void SetState(EGameFlowState State);

	void Fail(const FText& Reason);

	void HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& Error);

	void HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& Error);


public:
	const FGameFlowSnapshot& GetSnapshot() const { return GameFlowSnapshot; }

	bool IsFrontendWorld(const UWorld* World) const;

	bool CanStartLocalMatch() const;


private:
	FGameFlowSnapshot GameFlowSnapshot;

	// 等待目标本地比赛 World 认领的启动选择；认领、返回前台或失败时清空，不表示正在加载。
	FMatchLaunchSelection PendingSelection;

	// 上述本地比赛请求的一次性身份 ID；有效才允许目标 World 认领，认领后立即失效。
	FGuid PendingRequest;
	TWeakObjectPtr<UWorld> FlowWorld;

	// 当前观察的 World Experience 子系统弱引用；用于状态订阅，不持有内容资产。
	TWeakObjectPtr<UExperienceSubsystem> ObservedContent;
	FTimerHandle AdvanceTimer;
};

