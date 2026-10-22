#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "Hero/HeroLifeState.h"
#include "Combat/CombatantInterface.h"
#include "HeroCharacter.generated.h"

class UHeroAbilitySystemComponent;
class UHeroAttributeSet;
class UHeroAttributeComponent;
class UHeroDefinition;
class UStaticMeshComponent;
class AParticipantState;
class UItemInventoryComponent;
struct FGameplayEffectContextHandle;

UCLASS()
class MOBAGAME_API AHeroCharacter : public ACharacter, public IAbilitySystemInterface, public ICombatantInterface
{
	GENERATED_BODY()

public:
	AHeroCharacter(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual void Tick(float DeltaSeconds) override;

	virtual bool IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const override;

	virtual void PossessedBy(AController* NewController) override;

	virtual void UnPossessed() override;

	virtual void OnRep_Controller() override;

	virtual void OnRep_Owner() override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;


public:
	// 实体初始化与控制绑定
	/**
	 * 仅服务器首次初始化：成长基础值、初始来源、最终上限满资源；失败不发布身份，由生成方销毁本次新实体。
	 * 同定义/参赛者/比赛重复调用幂等；控制变化只调用 RefreshActorInfo。
	 */
	bool InitializeHero(const UHeroDefinition* InHeroDef, const AParticipantState* InPlayerState);

	void RefreshActorInfo();

	void CommitLoadoutRevision(uint32 Revision);

	bool CanAcceptPlayerInput() const;


private:
	void ApplyDefinition();

	UFUNCTION()
	void OnRep_Definition();


public:
	// 死亡与复活
	/**
	 * 仅服务器且生命归零；先锁定生命代次与归属，再执行清理，最后通知。
	 */
	bool EnterDeath(const FGameplayEffectContextHandle& Context);

	virtual void OnCombatHealthDepleted(const FGameplayEffectContextHandle& Context) override { EnterDeath(Context); }


private:
	void Respawn(uint32 ExpectedGeneration);

	void ApplyLifeState();

	void NotifyLifeState();

	UFUNCTION()
	void OnRep_Life();


public:
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	virtual UAbilitySystemComponent* GetCombatAbilitySystem() const override { return GetAbilitySystemComponent(); }

	virtual FGuid GetCombatEntity() const override { return EntityId; }

	virtual FGuid GetCombatParticipant() const override { return ParticipantId; }

	virtual FGuid GetCombatMatch() const override { return MatchId; }

	virtual int32 GetCombatTeam() const override { return TeamId; }

	virtual bool IsCombatAlive() const override { return IsAlive(); }
	UItemInventoryComponent* GetInventory() const { return Inventory; }
	UHeroAbilitySystemComponent* GetHeroAbilitySystem() const { return AbilitySystem; }
	const UHeroAttributeSet* GetAttributes() const { return Attributes; }
	UHeroAttributeComponent* GetAttributeComponent() const { return AttributeComponent; }
	const UHeroDefinition* GetDefinition() const { return Definition; }
	bool IsAlive() const { return Life.State == EHeroLifeState::Alive; }
	const FHeroLifeSnapshot& GetLifeSnapshot() const { return Life; }


public:
	UPROPERTY(Replicated)
	FGuid EntityId;

	UPROPERTY(Replicated)
	FGuid ParticipantId;

	UPROPERTY(Replicated)
	FGuid MatchId;

	UPROPERTY(Replicated)
	int32 TeamId = INDEX_NONE;

	UPROPERTY(Replicated)
	uint32 InitializationGeneration = 0;

	UPROPERTY(Replicated)
	uint32 RequiredLoadoutRevision = 0;

	UPROPERTY(Replicated)
	uint32 ConnectionEpoch = 0;

	UPROPERTY(Replicated)
	bool bInputReady = false;


private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UHeroAbilitySystemComponent> AbilitySystem;

	// 英雄持有的物品背包组件。
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UItemInventoryComponent> Inventory;

	// 英雄 GAS 属性集对象，不是属性值数组。
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UHeroAttributeSet> Attributes;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UHeroAttributeComponent> AttributeComponent;

	// 英雄形体静态网格组件。
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> Body;

	// 只读英雄配置对象。
	UPROPERTY(ReplicatedUsing = OnRep_Definition)
	TObjectPtr<const UHeroDefinition> Definition;

	// 当前生命状态及最后一次死亡归属快照。
	UPROPERTY(ReplicatedUsing = OnRep_Life)
	FHeroLifeSnapshot Life;

	FTransform SpawnTransform;

	FTimerHandle RespawnTimer;
};

