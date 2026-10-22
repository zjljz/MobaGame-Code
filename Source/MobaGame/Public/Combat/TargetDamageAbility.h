#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/SelfCommitAbility.h"
#include "Combat/AbilityTargeting.h"
#include "Types/TargetingSystemTypes.h"
#include "TargetDamageAbility.generated.h"

class UGameplayEffect;
class UAbilityTargetingPreset;
struct FCombatRules;

// 目标伤害与投送 archetype；目标规则引用 Preset，GA 配置 GE、投送、成本与冷却。
UCLASS()
class MOBAGAME_API UTargetDamageAbility : public USelfCommitAbility
{
	GENERATED_BODY()

public:
	UTargetDamageAbility();

	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	                             FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& ValidationContext) const override;
#endif

private:
	// 单次目标请求与投送
	void OnTargetingCompleted(FTargetingRequestHandle RequestHandle);

	void FinishActivation(const FString& Reason);


public:
	FAbilityTargetGeometry GetGeometry(const FCombatRules* CombatRules = nullptr) const;


public:
	bool WasDeliveryCommitted() const { return bDeliveryCommitted; }


public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TObjectPtr<UAbilityTargetingPreset> TargetingPreset;

	// 伤害效果类引用；不是活动效果实例。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TSubclassOf<UGameplayEffect> DamageEffect;

	// 伤害分类标签；由战斗规则解析为 ECombatDamageType。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FGameplayTag DamageType;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	EAbilityDelivery Delivery = EAbilityDelivery::Instant;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	float ProjectileSpeed = 1200;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	float ProjectileLifetime = 2;

private:
	// Immediate 请求也可能由原生系统排队；用 Handle 关联完成回调，取消后由原生完成释放。
	FTargetingRequestHandle TargetingRequest;

	// 等待本次 TargetingRequest 完成；EndAbility 清空；若生成投射物，由投射物接管快照保活。
	UPROPERTY()
	TObjectPtr<UAbilityDeliverySnapshot> PendingDeliverySnapshot;

	// 等待本次目标查询完成的伤害 Spec；EndAbility 无论成功或取消都会清空。
	UPROPERTY()
	FGameplayEffectSpecHandle PendingDamageSpec;

	FString FailureReason;

	// 最近一次激活已通过成本/冷却提交并进入投送阶段；不代表命中或伤害生效，每次激活开始时清零。
	bool bDeliveryCommitted = false;
};

