#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "CombatantInterface.generated.h"

class UAbilitySystemComponent;
struct FGameplayEffectContextHandle;

UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UCombatantInterface : public UInterface
{
	GENERATED_BODY()
};

// 战斗身份不要求 PlayerState、Controller 或英雄技能槽。
class MOBAGAME_API ICombatantInterface
{
	GENERATED_BODY()

public:
	virtual UAbilitySystemComponent* GetCombatAbilitySystem() const = 0;

	virtual FGuid GetCombatEntity() const = 0;

	virtual FGuid GetCombatParticipant() const = 0;

	virtual FGuid GetCombatMatch() const = 0;

	virtual int32 GetCombatTeam() const = 0;

	virtual bool IsCombatAlive() const = 0;

	virtual void OnCombatHealthDepleted(const FGameplayEffectContextHandle& Context) = 0;
};
