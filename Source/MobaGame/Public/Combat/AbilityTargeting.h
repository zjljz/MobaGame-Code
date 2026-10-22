#pragma once

#include "CoreMinimal.h"
#include "Teams/TeamConfiguration.h"
#include "Combat/CombatRules.h"
#include "AbilityTargeting.generated.h"

UENUM(BlueprintType)
enum class EAbilityTargetPolicy : uint8
{
	None,
	Self,
	Unit,
	Point,
	Direction,
	Area,
	Line,
	Cone
};

UENUM(BlueprintType)
enum class EAbilityTargetRange : uint8
{
	Planar,
	Spatial
};

UENUM(BlueprintType)
enum class EAbilityDelivery : uint8
{
	Instant,
	Projectile
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FAbilityTargetIntent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EAbilityTargetPolicy Policy = EAbilityTargetPolicy::Unit;

	// 选中的目标 Actor；不是单位 ID。
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<AActor> Unit = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector Point = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector Direction = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FAbilityTargetGeometry
{
	GENERATED_BODY()

public:
	bool IsValid() const;


public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	EAbilityTargetPolicy Policy = EAbilityTargetPolicy::Unit;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float Range = 900;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float HeightTolerance = 200;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float Radius = 150;

	// 线段半宽与锥体半角。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float HalfWidth = 75;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float HalfAngleDegrees = 30;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	int32 MaxTargets = 16;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TArray<ETeamRelation> AllowedRelations = {ETeamRelation::Enemy};

	// Unit 可显式选择三维球形射程；Planar 使用 XY 距离与高度差。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	EAbilityTargetRange RangeMetric = EAbilityTargetRange::Planar;
};

// 仅服务器的投送几何与关系身份；不保存伤害或暴击数值。
UCLASS()
class MOBAGAME_API UAbilityDeliverySnapshot : public UObject
{
	GENERATED_BODY()

public:
	// 释放时的来源战斗实体 ID。
	FGuid Entity;

	// 释放时的来源参赛者 ID。
	FGuid Participant;

	// 释放所属比赛 ID。
	FGuid Match;

	// 释放时的来源队伍 ID。
	int32 Team = INDEX_NONE;

	FVector Origin = FVector::ZeroVector;

	FAbilityTargetGeometry Geometry;
};

// 同步 GameplayEvent 的只读意图，不存储客户端候选集合。
UCLASS()
class MOBAGAME_API UAbilityTargetEvent : public UObject
{
	GENERATED_BODY()

public:
	FAbilityTargetIntent Intent;

	TWeakObjectPtr<class AHeroPlayerController> RequestController;

	uint32 ConnectionEpoch = 0;
};

namespace MobaTargeting
{
	MOBAGAME_API bool ValidateIntent(const FVector& Origin, const FAbilityTargetGeometry& Geometry, const FAbilityTargetIntent& Intent, FString& Reason);

	MOBAGAME_API bool Contains(const FVector& Origin, const FVector& TargetPosition, const FAbilityTargetGeometry& Geometry, const FAbilityTargetIntent& Intent);

	MOBAGAME_API bool ValidateCandidate(const AActor* SourceActor, const AActor* TargetActor, FGuid SourceEntityId, FGuid MatchId, int32 SourceTeamId, const TArray<ETeamRelation>& AllowedRelations, const FVector& SightOrigin, FString& Reason);

	MOBAGAME_API void DrawIndicator(UWorld* World, const FVector& Origin, const FAbilityTargetGeometry& Geometry, const FAbilityTargetIntent& Intent);
}

