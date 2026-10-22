#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Session/ResumeCredential.h"
#include "MatchGameMode.generated.h"

class AParticipantState;
class AHeroPlayerController;

UCLASS()
class MOBAGAME_API AMatchGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMatchGameMode();

	virtual void StartPlay() override;

	virtual void PostLogin(APlayerController* NewPlayerController) override;

	virtual void Tick(float DeltaSeconds) override;

	virtual void Logout(AController* ExitingController) override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayerController) override;


public:
	// 参赛者与控制关联
	bool OwnsParticipant(const AParticipantState* ParticipantState) const;

	void DetachConnection(AHeroPlayerController* HeroController);

	void TryPrepareHero(AHeroPlayerController* HeroController);


	// 席位准入
	/**
	 * 服务器拥有连接的握手入口；只预留席位，内容提交前没有控制权限。
	 */
	void BeginAdmission(AHeroPlayerController* HeroController, bool bResume, const FResumeCredential& Credential);

	/**
	 * 在当前World重新检查预留代次后提交；恢复只换控制关联，不重建或重授。
	 */
	void CompleteAdmission(AHeroPlayerController* HeroController, FGuid AdmissionRequestId, FGuid ExperienceInstanceId, bool bContentLoaded);

	void ReleaseAdmission(AHeroPlayerController* HeroController, FName AdmissionResultReason);


	// 比赛结束
	/**
	 * 服务器比赛结束入口；撤销当前Match资格并清理保留对象，重复调用幂等。
	 */
	void FinishMatch();


public:
	bool IsMatchAdmissionOpen() const { return bMatchAdmissionOpen; }


private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FAdmissionStartupTest;
#endif

	// Match 实际拥有原 PlayerState；不使用默认 inactive duplicate/name/IP 找回。
	// 当前比赛保留的 ParticipantState Actor 列表。
	UPROPERTY()
	TArray<TObjectPtr<AParticipantState>> Participants;

	struct FSeat
	{
		// 恢复凭证密钥的摘要字节。
		TArray<uint8> Digest;

		// 此席位对应的 ParticipantState 弱引用。
		TWeakObjectPtr<AParticipantState> Participant;

		// 当前预留此席位的 HeroPlayerController 弱引用。
		TWeakObjectPtr<AHeroPlayerController> ReservedBy;

		// 当前准入请求身份 ID，提交或释放预留后失效。
		FGuid Request;

		// 准入预留截止时刻，使用 FPlatformTime::Seconds 时钟域，不是剩余时长。
		double Deadline = 0;
		bool bCommitted = false;
	};

	TArray<FSeat> Seats;

	// 等待准入的 HeroPlayerController 到截止时刻的映射；成功绑定、断开、超时或比赛结束时移除。
	TMap<TWeakObjectPtr<AHeroPlayerController>, double> PendingConnections;

	// 仅保存首次准入的连接，不缓存凭证；截止时间仍由 PendingConnections 唯一持有。
	// 首次准入等待 Experience 加载/激活结束的控制器队列；Tick 移交后重试准入，失败释放时移除。
	TArray<TWeakObjectPtr<AHeroPlayerController>> ContentPendingConnections;

	// 比赛准入及其维护流程的开关，不表示内容 Ready；结束比赛时关闭。
	bool bMatchAdmissionOpen = true;
};

