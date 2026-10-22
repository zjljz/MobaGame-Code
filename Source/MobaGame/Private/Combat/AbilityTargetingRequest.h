#pragma once

#include "Combat/AbilityTargeting.h"
#include "Types/TargetingSystemDataStores.h"

// 按原生 Handle 持有值和观察引用，不让共享 Preset/Task 保存本次施法状态。
struct FAbilityTargetingRequestData
{
	// 本次请求观察的目标 Actor；不持有其生命周期。
	TWeakObjectPtr<AActor> Unit;
	FVector Point = FVector::ZeroVector;
	FVector Direction = FVector::ZeroVector;
	FVector Origin = FVector::ZeroVector;
	// 请求发起者的战斗实体 ID。
	FGuid Entity;
	// 请求所属比赛 ID。
	FGuid Match;
	// 请求发起者的队伍 ID。
	int32 Team = INDEX_NONE;
	FAbilityTargetGeometry Geometry;
	EAbilityDelivery Delivery = EAbilityDelivery::Instant;
	FString FailureReason;
};

DECLARE_TARGETING_DATA_STORE(FAbilityTargetingRequestData)

