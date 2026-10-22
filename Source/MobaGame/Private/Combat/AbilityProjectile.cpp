#include "Combat/AbilityProjectile.h"

#include "Combat/CombatantInterface.h"
#include "Combat/CombatEffectContext.h"
#include "Combat/AbilityTargeting.h"
#include "AbilitySystemComponent.h"
#include "Match/MatchGameState.h"
#include "Engine/World.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AAbilityProjectile::AAbilityProjectile()
{
	bReplicates = true;
	PrimaryActorTick.bCanEverTick = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(60);
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetRootComponent(Collision);
	Collision->InitSphereRadius(16);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	Collision->SetGenerateOverlapEvents(true);
	// UE 碰撞组件接入边界；不向项目传播第二套委托通知。
	Collision->OnComponentBeginOverlap.AddDynamic(this, &ThisClass::OnOverlap);
	Collision->OnComponentHit.AddDynamic(this, &ThisClass::OnBlockingHit);
	auto* BodyMeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	BodyMeshComp->SetupAttachment(Collision);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMeshFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	BodyMeshComp->SetStaticMesh(SphereMeshFinder.Object);
	BodyMeshComp->SetRelativeScale3D(FVector(0.32));
	BodyMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->SetUpdatedComponent(Collision);
	Movement->ProjectileGravityScale = 0;
	// Initialize在FinishSpawning前设置服务器世界速度，不能再按Actor旋转转换。
	Movement->bInitialVelocityInLocalSpace = false;
	Movement->bRotationFollowsVelocity = true;
	Movement->bShouldBounce = false;
}

void AAbilityProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority() || !Snapshot)
	{
		return;
	}

	const auto* MatchGameState = GetWorld()->GetGameState<AMatchGameState>();
	if (!MatchGameState || MatchGameState->bMatchEnded || MatchGameState->MatchId != Snapshot->Match)
	{
		Destroy();
		return;
	}

	if (bResumeAfterIgnoredHit && !bResolved)
	{
		bResumeAfterIgnoredHit = false;
		Movement->SetUpdatedComponent(Collision);
		Movement->Velocity = TravelVelocity;
	}
}

void AAbilityProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAbilityProjectile, SourceParticipant);
	DOREPLIFETIME(AAbilityProjectile, SourceEntity);
}

void AAbilityProjectile::Initialize(UAbilityDeliverySnapshot* InDeliverySnapshot, const FGameplayEffectSpecHandle& InDamageEffectSpec, const FVector& Direction, float Speed, float Lifetime, AActor* SourceActor)
{
	if (!HasAuthority() || !InDeliverySnapshot || !InDamageEffectSpec.IsValid())
	{
		Destroy();
		return;
	}

	Snapshot = InDeliverySnapshot;
	Spec = InDamageEffectSpec;
	SourceParticipant = Snapshot->Participant;
	SourceEntity = Snapshot->Entity;
	Collision->IgnoreActorWhenMoving(SourceActor, true);
	TravelVelocity = Direction.GetSafeNormal2D() * Speed;
	Movement->Velocity = TravelVelocity;
	SetLifeSpan(Lifetime);
	ForceNetUpdate();
}

bool AAbilityProjectile::TryResolveHit(AActor* OtherActor)
{
	if (!HasAuthority() || bResolved || !Snapshot || !Spec.IsValid() || OtherActor == GetOwner())
	{
		return false;
	}

	FString Reason;
	if (!MobaTargeting::ValidateCandidate(this, OtherActor, Snapshot->Entity, Snapshot->Match, Snapshot->Team, Snapshot->Geometry.AllowedRelations, GetActorLocation(), Reason))
	{
		return false;
	}

	const FVector TargetPosition = OtherActor->GetActorLocation();
	const double DistanceSquared = Snapshot->Geometry.RangeMetric == EAbilityTargetRange::Spatial ? FVector::DistSquared(Snapshot->Origin, TargetPosition) : FVector::DistSquaredXY(Snapshot->Origin, TargetPosition);
	if (DistanceSquared > FMath::Square(Snapshot->Geometry.Range)
		|| (Snapshot->Geometry.RangeMetric == EAbilityTargetRange::Planar && FMath::Abs(TargetPosition.Z - Snapshot->Origin.Z) > Snapshot->Geometry.HeightTolerance))
	{
		return false;
	}

	ICombatantInterface* TargetCombatant = Cast<ICombatantInterface>(OtherActor);
	// 先关闭命中，GE/死亡通知重入也只能结算一次。
	bResolved = true;
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FGameplayEffectSpec TargetSpec = MobaCombat::CopyForTarget(*Spec.Data);
	TargetCombatant->GetCombatAbilitySystem()->ApplyGameplayEffectSpecToSelf(TargetSpec);
	Destroy();
	return true;
}

void AAbilityProjectile::OnOverlap(UPrimitiveComponent* Component, AActor* OtherActor, UPrimitiveComponent* OtherComponent, int32 BodyIndex, bool bFromSweep, const FHitResult& Hit)
{
	TryResolveHit(OtherActor);
}

void AAbilityProjectile::OnBlockingHit(UPrimitiveComponent* Component, AActor* OtherActor, UPrimitiveComponent* OtherComponent, FVector Impulse, const FHitResult& Hit)
{
	if (!HasAuthority() || bResolved)
	{
		return;
	}

	if (Cast<ICombatantInterface>(OtherActor))
	{
		if (!TryResolveHit(OtherActor))
		{
			// 非法战斗目标不是墙；跳过本Actor的全部组件，下一帧恢复原生移动。
			// ProjectileMovement在blocking回调后会StopSimulating并清空UpdatedComponent。
			Collision->IgnoreActorWhenMoving(OtherActor, true);
			bResumeAfterIgnoredHit = true;
		}

		return;
	}

	bResolved = true;
	Destroy();
}

