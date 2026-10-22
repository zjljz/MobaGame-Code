#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "GameplayTagContainer.h"
#include "Player/InputProfileDefinition.h"
#include "Session/ResumeCredential.h"
#include "Combat/AbilityTargeting.h"
#include "Items/ItemInventoryComponent.h"
#include "HeroPlayerController.generated.h"

class AHeroCharacter;
class AParticipantState;
class UEnhancedInputComponent;
struct FInputActionValue;

UCLASS()
class MOBAGAME_API AHeroPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AHeroPlayerController();

	virtual void BeginPlay() override;

	virtual void ReceivedPlayer() override;

	virtual void Tick(float DeltaSeconds) override;

	virtual void SetupInputComponent() override;

	virtual void PawnLeavingGame() override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual bool CallRemoteFunction(UFunction* Function, void* Parameters, FOutParmRec* OutParms, FFrame* Stack) override;


public:
	// Experience 加载
	/**
	 * 身份复制、准入条件或 Experience 状态变化后重新检查；允许事件以任意顺序到达。
	 * 只推进本地内容加载及其就绪报告，Pawn、ASC 与输入仍沿用各自的就绪检查。
	 */
	void RefreshExperienceLoading();

	/**
	 * 离场前移除本地战斗映射与输入引用；不修改服务器参赛资格。
	 */
	void PrepareForFrontend();


	// 准入握手与重连
	void AcceptContentReady() { bContentReady = true; }

	UFUNCTION(Server, Reliable)
	void ServerBeginAdmission(bool bResume, const FResumeCredential& Credential);

	UFUNCTION(Server, Reliable)
	void ServerAdmissionReady(FGuid AdmissionRequestId, FGuid ExperienceInstanceId, bool bLoaded);

	UFUNCTION(Client, Reliable)
	void ClientStoreCredential(const FResumeCredential& Credential);

	UFUNCTION(Client, Reliable)
	void ClientAdmissionResult(FGuid AdmissionRequestId, FName Result);

	UFUNCTION(Client, Reliable)
	void ClientBindingState(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LifeGeneration, uint32 DeathCount);

	UFUNCTION(Exec)
	void MobaReconnect();


	// 输入就绪
	// 物品交易意图与结果
	UFUNCTION(Server, Reliable)
	void ServerItemRequest(const FItemRequest& Request);

	UFUNCTION(Client, Reliable)
	void ClientItemResult(const FItemResult& Result);

	UFUNCTION(Exec)
	void MobaBuy(const FString& ItemId, int32 Quantity = 1);

	UFUNCTION(Exec)
	void MobaSell(const FString& InstanceId);

	UFUNCTION(Exec)
	void MobaCombine(const FString& ItemId);

	UFUNCTION(Exec)
	void MobaUseItem(const FString& InstanceId);

	UFUNCTION(Exec)
	void MobaItems();


	bool ValidateIntent(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision, FString& Reason) const;

	bool IsLocallyReady() const;

	UFUNCTION(Server, Reliable)
	void ServerContentReady(FGuid ExperienceInstanceId);

	UFUNCTION(Server, Reliable)
	void ServerPlayerReady(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision);


private:
	/**
	 * 核对当前 possession、参赛者与绑定代次；不要求服务器已开放输入，可用于 PlayerReady 前置检查。
	 */
	bool IsCurrentHeroBinding(const AHeroCharacter* Hero) const;

	bool IsHeroLoadoutReady(const AHeroCharacter* Hero) const;

	/**
	 * 输入组件建立或内容就绪时尝试安装；条件未齐时等待后续通知，同一组件不重复绑定。
	 */
	void InstallInputBindings();


public:
	void ClearLocalInput();


	// 输入配置与操作
	UFUNCTION(Server, Reliable)
	void ServerSelectProfile(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision, EHeroInputProfile Profile);

	UFUNCTION(Server, Reliable)
	void ServerClickMove(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision, FVector_NetQuantize Destination);

	UFUNCTION(Server, Reliable)
	void ServerActivateSlot(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision, FGameplayTag Slot);

	UFUNCTION(Server, Reliable)
	void ServerActivateTargetSlot(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision, FGameplayTag Slot, const FAbilityTargetIntent& Intent);

	/** 服务器能力完成目标查询并提交后回报；绑定已改变时不向新绑定发送旧结果。 */
	void CompleteTargetAbility(const AHeroCharacter* SourceHero, uint32 ConnectionEpoch, bool bAccepted, const FString& Reason);

	bool BuildCursorIntent(const FAbilityTargetGeometry& Geometry, FAbilityTargetIntent& Intent) const;

	UFUNCTION(Client, Reliable)
	void ClientClickPath(uint32 ConnectionEpoch, uint32 InProfileRevision, const TArray<FVector>& Points);

	UFUNCTION(Client, Reliable)
	void ClientIntentResult(uint32 Sequence, FName Operation, bool bAccepted, FName Reason);


private:
	UFUNCTION()
	void OnRep_Profile();

	void ApplyProfile();

	void OnMove(const FInputActionValue& Value);

	void OnClick(const FInputActionValue& Value);

	void OnSwitch(const FInputActionValue& Value);

	void OnAbility(const FInputActionValue& Value);

	void OnUnitAttack(const FInputActionValue& Value);

	void OnTargetAbility(const FInputActionValue& Value);

	void RequestSlotInput(FGameplayTag Slot);


	// 物品交易请求
	void RequestItem(EItemOperation Operation, FName ItemId, FGuid ItemInstanceId, int32 Quantity);


public:
	const FItemResult& GetLastItemResult() const { return LastItemResult; }


private:

	void LogRejected(const TCHAR* Operation, const FString& Reason);


public:
	// 控制台诊断
	UFUNCTION(Exec)
	void MobaStatus();

	// 仅本机服务器开发诊断；没有客户端奖励 RPC。
	UFUNCTION(Exec)
	void MobaExperience(int64 Amount);

	UFUNCTION(Exec)
	void MobaProfile();

	UFUNCTION(Exec)
	void MobaAbility();

	UFUNCTION(Exec)
	void MobaAttack(const FString& TargetEntityIdText);

	UFUNCTION(Exec)
	void MobaClick(float X, float Y, float Z);

	UFUNCTION(Exec)
	void MobaRejectProbe();


public:
	bool HasContentReady() const { return bContentReady; }
	bool CanLoadContent() const { return AdmissionRequest.IsValid() && !bCredentialStoreFailed; }
	FName GetAdmissionResult() const { return AdmissionResult; }
	FGuid GetAdmissionRequest() const { return AdmissionRequest; }
	EHeroInputProfile GetActiveProfile() const { return ActiveProfile; }
	uint32 GetIntentResultSequence() const { return LastIntentResultSequence; }
	bool WasLastIntentAccepted() const { return bLastIntentAccepted; }
	FName GetLastIntentReason() const { return LastIntentReason; }


private:
	FItemResult LastItemResult;

	uint32 NextItemSequence = 0;

	// 当前准入请求 ID，不是请求数据结构。
	FGuid AdmissionRequest;

	FName AdmissionResult;

	bool bHandshakeSent = false;

	bool bAdmissionReadySent = false;

	bool bCredentialStoreFailed = false;

	// 当前控制绑定的英雄实体 ID。
	FGuid BoundEntity;

	uint32 BoundEpoch = 0;

	uint32 BoundLifeGeneration = 0;

	uint32 BoundDeathCount = 0;

	bool bContentReady = false;

	bool bContentReadySent = false;

	bool bPlayerReadySent = false;

	uint32 SentReadyRevision = 0;

	uint32 SentReadyEpoch = 0;

	// 仅标记已完成绑定的组件；输入组件重建后允许重新安装。
	TWeakObjectPtr<UEnhancedInputComponent> BoundInputComponent;

	bool bAnnouncedReady = false;

	// 切换请求发出后等待 OnRep_Profile 或拒绝结果清零；等待期间禁止本地玩法输入。
	bool bProfileSwitchPending = false;

	TArray<FVector> ClickPath;

	int32 PathIndex = 0;

	TMap<FString, double> LastRejectedTimes;

	uint32 SentIntentResultSequence = 0;

	uint32 LastIntentResultSequence = 0;

	bool bLastIntentAccepted = false;

	FName LastIntentReason;

	UPROPERTY(ReplicatedUsing = OnRep_Profile)
	EHeroInputProfile ActiveProfile = EHeroInputProfile::WASD;

	UPROPERTY(ReplicatedUsing = OnRep_Profile)
	uint32 ProfileRevision = 0;

	// 当前已应用的输入配置对象，与 ActiveProfile 枚举值区分。
	UPROPERTY()
	TObjectPtr<const UInputProfileDefinition> AppliedProfile;
};

