#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "ParticipantState.generated.h"

class AHeroCharacter;
class UItemInventoryComponent;

USTRUCT()
struct MOBAGAME_API FParticipantEconomy
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Gold = 0;

	UPROPERTY()
	uint32 Revision = 0;
};

UCLASS()
class MOBAGAME_API AParticipantState : public APlayerState
{
	GENERATED_BODY()

public:
	virtual void OnDeactivated() override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;


public:
	// 初次参赛经济初始化
	bool InitializeEconomy(int32 InitialGold);


public:
	const FParticipantEconomy& GetEconomy() const { return Economy; }


private:
	UFUNCTION()
	void OnRep_Economy();


public:
	UPROPERTY(Replicated)
	FGuid MatchId;

	UPROPERTY(Replicated)
	FGuid ParticipantId;

	UPROPERTY(Replicated)
	int32 TeamId = INDEX_NONE;

	UPROPERTY(Replicated)
	uint32 ConnectionEpoch = 1;

	UPROPERTY(Replicated)
	bool bConnected = true;

	UPROPERTY(Replicated)
	TObjectPtr<AHeroCharacter> PrimaryHero;


private:
	friend class UItemInventoryComponent;

	UPROPERTY(ReplicatedUsing=OnRep_Economy)
	FParticipantEconomy Economy;

	bool bEconomyInitialized = false;
};
