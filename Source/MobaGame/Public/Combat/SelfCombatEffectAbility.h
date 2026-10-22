#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/SelfCommitAbility.h"
#include "SelfCombatEffectAbility.generated.h"

UCLASS()
class MOBAGAME_API USelfCombatEffectAbility : public USelfCommitAbility
{
	GENERATED_BODY()

public:
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr, FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

	virtual void ActivateAbility(
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* Event) override;


public:
	bool WasEffectCommitted() const { return bEffectCommitted; }


public:
	// 自身效果的类引用；应用时读取 CDO，不是活动效果实例。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TSubclassOf<UGameplayEffect> Effect;


private:
	bool bEffectCommitted = false;
};

