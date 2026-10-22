#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayAbilitySpecHandle.h"
#include "ItemInventoryComponent.generated.h"

class AHeroPlayerController;
class UMatchRulesDefinition;
class UItemDefinition;

UENUM()
enum class EItemOperation : uint8
{
	Buy,
	Sell,
	Combine,
	Use
};

USTRUCT()
struct MOBAGAME_API FItemRequest
{
	GENERATED_BODY()

public:
	// 交易目标英雄的战斗实体 ID。
	UPROPERTY()
	FGuid Entity;

	UPROPERTY()
	uint32 Epoch = 0;

	UPROPERTY()
	uint32 Sequence = 0;

	UPROPERTY()
	uint32 Revision = 0;

	UPROPERTY()
	EItemOperation Operation = EItemOperation::Buy;

	UPROPERTY()
	FName ItemId;

	// 目标物品实例 ID；不是物品实例结构。
	UPROPERTY()
	FGuid Instance;

	UPROPERTY()
	int32 Quantity = 1;
};

USTRUCT()
struct MOBAGAME_API FItemInstance
{
	GENERATED_BODY()

public:
	// 实例身份同时是唯一ASC授予来源；堆叠数量变化不重新授予。
	UPROPERTY()
	FGuid Id;

	UPROPERTY()
	FName ItemId;

	UPROPERTY()
	int32 Slot = INDEX_NONE;

	UPROPERTY()
	int32 Quantity = 0;

	// 该物品主动技能的 Spec 句柄。
	UPROPERTY()
	FGameplayAbilitySpecHandle ActiveAbility;
};

USTRUCT()
struct MOBAGAME_API FItemInventorySnapshot
{
	GENERATED_BODY()

public:
	UPROPERTY()
	uint32 Revision = 0;

	UPROPERTY()
	uint32 AbilityStateRevision = 0;

	UPROPERTY()
	TArray<FItemInstance> Items;
};

USTRUCT()
struct MOBAGAME_API FItemResult
{
	GENERATED_BODY()

public:
	UPROPERTY()
	uint32 Epoch = 0;

	UPROPERTY()
	uint32 Sequence = 0;

	UPROPERTY()
	uint32 Revision = 0;

	UPROPERTY()
	bool bAccepted = false;

	UPROPERTY()
	FName Reason;
};

UCLASS()
class MOBAGAME_API UItemInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UItemInventoryComponent();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;


public:
	/**
	 * 服务器同步交易入口；调用者必须实际拥有本Hero，配置只从World Experience读取。
	 * 失败不提交经济/库存/来源；序号结果缓存属于连接代次，重入直接Busy。
	 */
	FItemResult Execute(AHeroPlayerController* Controller, const FItemRequest& Request);


private:
	FName Apply(AHeroPlayerController* Controller, const FItemRequest& Request, const UMatchRulesDefinition* Rules);


public:
	const FItemInventorySnapshot& GetSnapshot() const { return Snapshot; }
	int32 GetCachedResultCount() const { return Results.Num(); }


private:
	UFUNCTION()
	void OnRep_Snapshot();


private:
	UPROPERTY(ReplicatedUsing=OnRep_Snapshot)
	FItemInventorySnapshot Snapshot;

	TArray<FItemResult> Results;

	uint32 RequestEpoch = 0;

	// 当前连接代次已处理的最大请求序号，用于拒绝过期请求。
	uint32 HighWater = 0;

	bool bOperating = false;
};

