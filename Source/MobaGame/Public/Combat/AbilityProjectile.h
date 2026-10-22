#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayEffectTypes.h"
#include "AbilityProjectile.generated.h"

class USphereComponent;
class UProjectileMovementComponent;
class UAbilityDeliverySnapshot;

UCLASS()
class MOBAGAME_API AAbilityProjectile : public AActor
{
	GENERATED_BODY()

public:
	AAbilityProjectile();

	virtual void Tick(float DeltaSeconds) override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;


public:
	void Initialize(UAbilityDeliverySnapshot* InDeliverySnapshot, const FGameplayEffectSpecHandle& InDamageEffectSpec, const FVector& Direction, float Speed, float Lifetime, AActor* SourceActor);


private:
	bool TryResolveHit(AActor* OtherActor);

	UFUNCTION()
	void OnOverlap(UPrimitiveComponent* Component, AActor* OtherActor, UPrimitiveComponent* OtherComponent, int32 BodyIndex, bool bFromSweep, const FHitResult& Hit);

	UFUNCTION()
	void OnBlockingHit(UPrimitiveComponent* Component, AActor* OtherActor, UPrimitiveComponent* OtherComponent, FVector Impulse, const FHitResult& Hit);


public:
	// 施法来源的参赛者 ID；不是参赛者对象。
	UPROPERTY(Replicated)
	FGuid SourceParticipant;

	// 施法来源的战斗实体 ID。
	UPROPERTY(Replicated)
	FGuid SourceEntity;


private:
	// 投射物的球形碰撞组件。
	UPROPERTY()
	TObjectPtr<USphereComponent> Collision;

	// 投射物移动组件，非速度值。
	UPROPERTY()
	TObjectPtr<UProjectileMovementComponent> Movement;

	// 投送期间保活的释放快照，独立于施法者连接和生命。
	UPROPERTY()
	TObjectPtr<UAbilityDeliverySnapshot> Snapshot;

	// 待命中应用的伤害效果 Spec 句柄。
	UPROPERTY()
	FGameplayEffectSpecHandle Spec;

	bool bResolved = false;

	bool bResumeAfterIgnoredHit = false;

	FVector TravelVelocity = FVector::ZeroVector;
};

