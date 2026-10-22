#include "Tests/Combat/CombatCancellationTestAbility.h"
#include "MobaGameplayTags.h"

UCombatCancellationTestAbility::UCombatCancellationTestAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	NetSecurityPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnly;
	SetAssetTags(FGameplayTagContainer(MobaGameplayTags::Ability_CancelOnDeath));
}

void UCombatCancellationTestAbility::ActivateAbility(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	// 保持 active，供测试验证死亡取消真实实例；没有额外任务或后台工作。
}
