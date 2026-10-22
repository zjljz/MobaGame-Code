#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "HeroAttributeComponent.generated.h"

class AHeroCharacter;
class UHeroDefinition;
class UMatchRulesDefinition;
struct FGameplayEffectSpec;

// 不保存等级副本；结果只描述本次受控经验操作。
struct MOBAGAME_API FHeroExperienceResult
{
	bool bAccepted = false;
	int64 AddedExperiencePoints = 0;
	int32 PreviousLevel = 0;
	int32 NewLevel = 0;
};

/** Hero 唯一的成长、资源比例和恢复时间协调者；ASC/Set 仍由 Hero 持有。 */
UCLASS()
class MOBAGAME_API UHeroAttributeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UHeroAttributeComponent();

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;


public:
	/** 仅服务器首次初始化使用；Begin 后由 Hero 授予来源，Finish 才填满最终资源并 Ready。 */
	bool BeginInitialization(const UHeroDefinition* HeroDef, const UMatchRulesDefinition* MatchRules, FString& Reason);

	bool FinishInitialization();

	void AbortInitialization();

	/** 只接受可信服务器奖励；不产生技能等级或奖励分配副作用。 */
	FHeroExperienceResult AddExperience(int64 Reward);

	void FlushRegeneration();

	// 死亡先切断时钟，复活在同一实体上填满当前最终上限。
	void StopForDeath();

	void RestoreForRespawn();

	void BeforeAttributeChange(const FGameplayAttribute& Attribute);

	void AfterAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue);

	void BeginSourceBatch();

	void EndSourceBatch();

	bool ValidateModifiers(const FGameplayEffectSpec& Spec) const;


private:
	friend class UHeroAbilitySystemComponent;

	void RefreshSourceInterval();

	void ApplyGrowth(int32 Level);

	void RestoreResources();

	void NotifyProgression();

	void AdjustResource(const FGameplayAttribute& Resource, float OldMaximum, float NewMaximum, float OldResource);

	void CaptureRegenerationInterval();

	float EvaluateRegenerationAttribute(int32 Index, double Time) const;


public:
	bool IsReady() const { return bReady; }
	bool IsInitializing() const { return bInitializing; }
	bool HasNoResource() const;
	int32 GetMaximumLevel() const;
	float GetMaximumCooldownReduction() const;
	int64 GetTotalExperiencePoints() const { return TotalExperiencePoints; }
	const UMatchRulesDefinition* GetRules() const { return Rules; }
	float GetResourceMaximumDuringIntegration(bool bMana, float EffectiveMaximum) const;


private:
	UFUNCTION()
	void OnRep_TotalExperiencePoints();


private:
	UPROPERTY(ReplicatedUsing=OnRep_TotalExperiencePoints)
	int64 TotalExperiencePoints = 0;

	UPROPERTY(Transient)
	TObjectPtr<const UHeroDefinition> Definition;

	UPROPERTY(Transient)
	TObjectPtr<const UMatchRulesDefinition> Rules;

	bool bReady = false;
	bool bInitializing = false;
	bool bCoordinating = false;
	bool bDead = false;
	bool bIntegrating = false;
	int32 BatchDepth = 0;
	double LastRegenerationTime = 0;
	float BatchHealth = 0;
	float BatchMana = 0;
	float BatchMaxHealth = 0;
	float BatchMaxMana = 0;
	FTimerHandle RegenerationTimer;

	// 只供尚未结算时间区间使用的原生 Snapshot；不向玩法公开，不写回成长配置。
	TArray<FGameplayEffectAttributeCaptureSpec> RegenerationCaptures;
	TMap<FActiveGameplayEffectHandle, double> RegenerationExpirations;
	FGameplayTagContainer RegenerationTags;
	float IntegrationMaxHealth = 0;
	float IntegrationMaxMana = 0;
};

// 原生 GE 句柄变更仍由原调用方持有；作用域只协调最终上限和经过时间。
class MOBAGAME_API FHeroAttributeBatchScope
{
public:
	explicit FHeroAttributeBatchScope(AHeroCharacter* Hero);
	~FHeroAttributeBatchScope();

	FHeroAttributeBatchScope(const FHeroAttributeBatchScope&) = delete;
	FHeroAttributeBatchScope& operator=(const FHeroAttributeBatchScope&) = delete;


private:
	UHeroAttributeComponent* Component = nullptr;
};
