#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "HeroAbilitySystemComponent.generated.h"

class UAbilitySetDefinition;

enum class EAbilitySourceResult : uint8
{
	Applied, NoChange, Pending, Busy, Failed
};

struct MOBAGAME_API FAbilitySourceResult
{
public:
	bool IsSuccess() const { return Status == EAbilitySourceResult::Applied || Status == EAbilitySourceResult::NoChange; }


public:
	EAbilitySourceResult Status = EAbilitySourceResult::Failed;

	FName Reason;
};

USTRUCT()
struct MOBAGAME_API FHeroAbilitySlot
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FGameplayTag Slot;

	// 已授予技能的 Spec 句柄，不是技能实例。
	UPROPERTY()
	FGameplayAbilitySpecHandle Handle;

	// 授予来源的稳定 ID，不是来源 UObject。
	UPROPERTY()
	FGuid Source;
};

USTRUCT()
struct MOBAGAME_API FManagedAbilityState
{
	GENERATED_BODY()

public:
	// 已授予技能的 Spec 句柄，不是技能实例。
	UPROPERTY()
	FGameplayAbilitySpecHandle Handle;

	UPROPERTY()
	int32 Level = 1;

	UPROPERTY()
	bool bEnabled = true;
};

USTRUCT()
struct MOBAGAME_API FHeroLoadoutSnapshot
{
	GENERATED_BODY()

public:
	UPROPERTY()
	uint32 Revision = 0;

	UPROPERTY()
	uint32 StateRevision = 0;

	UPROPERTY()
	TArray<FHeroAbilitySlot> Slots;

	UPROPERTY()
	TArray<FManagedAbilityState> States;
};

USTRUCT()
struct FHeroGrantRecord
{
	GENERATED_BODY()

public:
	// 授予来源的稳定 ID，不是来源 UObject。
	UPROPERTY()
	FGuid Source;

	// 本来源只读技能集配置；运行时记录独立于配置。
	UPROPERTY()
	TObjectPtr<const UAbilitySetDefinition> Definition;

	// 本来源授予的技能 Spec 句柄。
	UPROPERTY()
	TArray<FGameplayAbilitySpecHandle> Abilities;

	// 本来源应用的活动效果句柄。
	UPROPERTY()
	TArray<FActiveGameplayEffectHandle> Effects;

	// 独立记录原生阻断计数的所有权；Spec已移除时仍能准确回收。
	TArray<int32> InputIds;

	// 每个禁用发起者独立拥有原因集合；解除时不影响别人。
	TMap<FGuid, TSet<FName>> DisableReasons;

	float Lifetime = 0;

	// 已撤槽并阻断输入；等待本来源最后一个活动技能结束后回收效果与记录。
	bool bPendingRemove = false;

	bool bInputBlocked = false;

	FTimerHandle ExpiryTimer;
};

UCLASS()
class MOBAGAME_API UHeroAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	UHeroAbilitySystemComponent();

	virtual FActiveGameplayEffectHandle ApplyGameplayEffectSpecToSelf(const FGameplayEffectSpec& Spec, FPredictionKey PredictionKey = FPredictionKey()) override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual void NotifyAbilityEnded(FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, bool bWasCancelled) override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;


public:
	// 来源授予、回收与物品事务
	/**
	 * Hero 首次初始化：暂存并发布初始来源与槽；最终资源由属性组件在授予结束后统一填满。
	 * 失败撤销本次授予；调用方销毁本次初始化失败的新实体，不支持任意 GA 回调副作用。
	 */
	FAbilitySourceResult GrantInitialSource(const UAbilitySetDefinition* AbilitySetDef, FGuid SourceId);

	/**
	 * 兼容初始化入口；有界来源操作使用带状态版本与分类结果的接口。
	 */
	bool GrantFromSource(const UAbilitySetDefinition* AbilitySetDef, FGuid SourceId);

	bool RemoveSource(FGuid SourceId);

	/**
	 * 物品事务的无槽来源交换；活动来源拒绝，新增全部成功后才撤旧并发布一次版本。
	 * 调用方须先验证固定物品内容，不支持任意授予/移除回调副作用。
	 */
	FAbilitySourceResult ExchangeItemSources(const TArray<FGuid>& SourceIdsToRemove, const TArray<FGuid>& SourceIdsToAdd,
		const TArray<const UAbilitySetDefinition*>& AbilitySetDefsToAdd, uint32 ExpectedStateRevision);

	FAbilitySourceResult CanExchangeItemSources(const TArray<FGuid>& SourceIdsToRemove, uint32 ExpectedStateRevision) const;

	/**
	 * 固定即时物品GA；锁内检查标签反应面、激活结果，并清理最后一件的已结束来源。
	 */
	FAbilitySourceResult ActivateItemSource(FGuid SourceId, bool bConsumeSource);

	/**
	 * 仅服务器/Game Thread；ExpectedStateRevision 必须等于当前状态版本，0也是有效初始版本。
	 */
	FAbilitySourceResult GrantManagedSource(const UAbilitySetDefinition* AbilitySetDef, FGuid SourceId, uint32 ExpectedStateRevision, float Lifetime = 0);

	FAbilitySourceResult RemoveManagedSource(FGuid SourceId, uint32 ExpectedStateRevision);

	FAbilitySourceResult ReplaceManagedSource(FGuid SourceId, const UAbilitySetDefinition* AbilitySetDef, uint32 ExpectedStateRevision);

	FAbilitySourceResult CopyManagedSource(const UHeroAbilitySystemComponent* DonorASC, FGuid SourceId, FGuid NewSourceId, uint32 ExpectedStateRevision);

	FAbilitySourceResult SetSourceDisabled(FGuid SourceId, FGuid DisableReasonOwnerId, FName Reason, bool bDisabled, uint32 ExpectedStateRevision);

	FAbilitySourceResult UpgradeManagedAbility(FGameplayAbilitySpecHandle Handle, int32 Level, uint32 ExpectedStateRevision);

	FAbilitySourceResult RemapSlots(FGameplayTag FromSlotTag, FGameplayTag ToSlotTag, uint32 ExpectedStateRevision);


	// 技能装载查询
	bool HasReplicatedLoadout(uint32 RequiredRevision) const;

	bool IsManagedAbilityEnabled(FGameplayAbilitySpecHandle Handle) const;

	FGameplayAbilitySpecHandle FindSlot(FGameplayTag Slot) const;


private:
	// 来源操作预检与暂存
	FAbilitySourceResult CheckOperation(uint32 ExpectedStateRevision) const;

	FAbilitySourceResult ValidateDefinition(const UAbilitySetDefinition* AbilitySetDef, FGuid IgnoredSourceId = {}) const;

	bool StageGrant(const UAbilitySetDefinition* AbilitySetDef, FGuid SourceId, FHeroGrantRecord& GrantRecord, FName& FailureReason, const TArray<int32>* AbilityLevelOverrides = nullptr);

	bool HasTagReactiveContext(const UAbilitySetDefinition* AbilitySetDef) const;

	void RollbackGrant(FHeroGrantRecord& GrantRecord);

	void AddSlots(const FHeroGrantRecord& GrantRecord);

	void Publish(bool bMappingChanged);

	void UpdateInputBlock(FHeroGrantRecord& GrantRecord, bool bBlock);

	FAbilitySourceResult BeginRemove(FGuid SourceId);

	void CleanupPending();

	void ExpireSource(FGuid SourceId);

	bool CanCancelRecord(const FHeroGrantRecord& GrantRecord) const;

	bool IsRecordActive(const FHeroGrantRecord& GrantRecord) const;


public:
	uint32 GetLoadoutRevision() const { return Loadout.Revision; }

	uint32 GetStateRevision() const { return Loadout.StateRevision; }

	int32 GetSourceCount() const { return Sources.Num(); }

	const TArray<FHeroAbilitySlot>& GetSlots() const { return Loadout.Slots; }

	const FHeroGrantRecord* FindSource(FGuid SourceId) const;


private:
	UFUNCTION()
	void OnRep_Loadout();


private:
	UPROPERTY(ReplicatedUsing=OnRep_Loadout)
	FHeroLoadoutSnapshot Loadout;

	// 按来源 ID 保存的运行时授予记录。
	UPROPERTY()
	TArray<FHeroGrantRecord> Sources;

	bool bOperating = false;

	bool bEndingPlay = false;

	FTimerHandle CleanupTimer;
};

