#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Combat/CombatantInterface.h"
#include "TargetCombatant.generated.h"

class UAbilitySystemComponent;
class UHeroAttributeSet;
class USphereComponent;

// 仅供目标/投送测试的普通战斗实体，不持有 PlayerState、Controller 或英雄槽。
UCLASS(Transient, NotBlueprintable)
class ATargetCombatant : public AActor, public ICombatantInterface
{
	GENERATED_BODY()

public:
	ATargetCombatant();

	virtual void BeginPlay() override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Props) const override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual UAbilitySystemComponent* GetCombatAbilitySystem() const override;

	virtual void OnCombatHealthDepleted(const FGameplayEffectContextHandle& Context) override;


public:
	float GetHealth() const;

	void SetHealth(float Health);

	void UseBlockingWorldDynamicCollision();


public:
	virtual FGuid GetCombatEntity() const override { return Entity; }
	virtual FGuid GetCombatParticipant() const override { return Participant; }
	virtual FGuid GetCombatMatch() const override { return Match; }
	virtual int32 GetCombatTeam() const override { return Team; }
	virtual bool IsCombatAlive() const override { return bAlive; }


public:
	UPROPERTY(Replicated)
	FGuid Entity;

	UPROPERTY(Replicated)
	FGuid Participant;

	UPROPERTY(Replicated)
	FGuid Match;

	UPROPERTY(Replicated)
	int32 Team = 21;

	UPROPERTY(Replicated)
	bool bAlive = true;

	bool bEnded = false;

	UPROPERTY(Replicated)
	int32 DeathCount = 0;

	FGuid KillerEntity;

	FGuid KillerParticipant;


private:
	UPROPERTY()
	TObjectPtr<UAbilitySystemComponent> Abilities;

	UPROPERTY()
	TObjectPtr<UHeroAttributeSet> Attributes;

	UPROPERTY()
	TObjectPtr<USphereComponent> Collision;

	// 两个真实碰撞组件重叠，用于证明同一Actor重复碰撞不会重复提交。
	UPROPERTY()
	TObjectPtr<USphereComponent> SecondCollision;
};
