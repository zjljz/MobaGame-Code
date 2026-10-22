#pragma once

#include "CoreMinimal.h"
#include "HeroLifeState.generated.h"

UENUM(BlueprintType)
enum class EHeroLifeState : uint8
{
	Alive,
	Dying,
	Dead,
	Respawning
};

// 当前生命与最后一次权威归属快照；不依赖通知历史恢复。
USTRUCT(BlueprintType)
struct MOBAGAME_API FHeroLifeSnapshot
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadOnly)
	EHeroLifeState State = EHeroLifeState::Alive;

	UPROPERTY()
	uint32 Generation = 1;

	UPROPERTY()
	uint32 DeathCount = 0;

	// 击杀归属的参赛者 ID。
	UPROPERTY(BlueprintReadOnly)
	FGuid KillerParticipant;

	// 击杀归属的战斗实体 ID。
	UPROPERTY(BlueprintReadOnly)
	FGuid KillerEntity;

	// 伤害致因战斗实体 ID。
	UPROPERTY(BlueprintReadOnly)
	FGuid CauserEntity;

	UPROPERTY(BlueprintReadOnly)
	float RespawnServerTime = 0;
};

