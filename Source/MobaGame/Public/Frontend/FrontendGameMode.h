#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "FrontendGameMode.generated.h"

UCLASS()
class MOBAGAME_API AFrontendPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AFrontendPlayerController();

	virtual void BeginPlay() override;
};

/** 只装配主 Game 的前台 Experience，不开放比赛准入或生成战斗 Pawn。 */
UCLASS()
class MOBAGAME_API AFrontendGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AFrontendGameMode();

	virtual void StartPlay() override;
};
