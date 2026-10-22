#pragma once

#include "CoreMinimal.h"
#include "TeamConfiguration.generated.h"

UENUM(BlueprintType)
enum class ETeamRelation : uint8
{
	Undefined,
	Ally,
	Neutral,
	Enemy
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FTeamRelationEntry
{
	GENERATED_BODY()

public:
	// 关系查询发起方的队伍 ID。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	int32 From = INDEX_NONE;

	// 关系查询目标方的队伍 ID。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	int32 To = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	ETeamRelation Relation = ETeamRelation::Undefined;
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FTeamConfiguration
{
	GENERATED_BODY()

public:
	// 队伍关系
	// Neutral 不占玩家队伍席位；未知队伍或未配置的跨队关系明确拒绝。
	ETeamRelation Query(int32 FromTeamId, int32 ToTeamId) const;

	bool IsValid(FString& Reason) const;


public:
	// 可参赛队伍的 ID 集合。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TArray<int32> Teams;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TArray<FTeamRelationEntry> Relations;
};

