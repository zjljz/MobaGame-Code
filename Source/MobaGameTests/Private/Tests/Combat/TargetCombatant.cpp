#include "Tests/Combat/TargetCombatant.h"

#include "Combat/HeroAttributeSet.h"
#include "AbilitySystemComponent.h"
#include "Components/SphereComponent.h"
#include "Combat/AbilityTargeting.h"
#include "Net/UnrealNetwork.h"
#include "MobaGameplayTags.h"

ATargetCombatant::ATargetCombatant()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	Abilities = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("Abilities"));
	Abilities->SetIsReplicated(true);
	Abilities->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);
	Attributes = CreateDefaultSubobject<UHeroAttributeSet>(TEXT("Attributes"));
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetRootComponent(Collision);
	SecondCollision = CreateDefaultSubobject<USphereComponent>(TEXT("SecondCollision"));
	SecondCollision->SetupAttachment(Collision);
	for (USphereComponent* Shape : {Collision.Get(), SecondCollision.Get()})
	{
		Shape->InitSphereRadius(24);
		Shape->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Shape->SetCollisionObjectType(ECC_Pawn);
		Shape->SetCollisionResponseToAllChannels(ECR_Ignore);
		Shape->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Overlap);
		Shape->SetGenerateOverlapEvents(true);
	}
}

void ATargetCombatant::BeginPlay()
{
	Super::BeginPlay();
	Abilities->InitAbilityActorInfo(this, this);
	if (!HasAuthority())
	{
		return;
	}

	Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetMaxHealthAttribute(), 100);
	Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 100);
}

void ATargetCombatant::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ATargetCombatant, Entity);
	DOREPLIFETIME(ATargetCombatant, Participant);
	DOREPLIFETIME(ATargetCombatant, Match);
	DOREPLIFETIME(ATargetCombatant, Team);
	DOREPLIFETIME(ATargetCombatant, bAlive);
	DOREPLIFETIME(ATargetCombatant, DeathCount);
}

void ATargetCombatant::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEnded = true;
	Super::EndPlay(EndPlayReason);
}

UAbilitySystemComponent* ATargetCombatant::GetCombatAbilitySystem() const
{
	return Abilities;
}

float ATargetCombatant::GetHealth() const
{
	return Attributes->GetHealth();
}

void ATargetCombatant::SetHealth(float Health)
{
	Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), Health);
}

void ATargetCombatant::OnCombatHealthDepleted(const FGameplayEffectContextHandle& Context)
{
	if (!bAlive)
	{
		return;
	}

	bAlive = false;
	++DeathCount;
	Abilities->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(MobaGameplayTags::Effect_RemoveOnDeath));
	if (const auto* Snapshot = Cast<UAbilityDeliverySnapshot>(Context.GetSourceObject()))
	{
		KillerEntity = Snapshot->Entity;
		KillerParticipant = Snapshot->Participant;
	}
}

void ATargetCombatant::UseBlockingWorldDynamicCollision()
{
	for (USphereComponent* Shape : {Collision.Get(), SecondCollision.Get()})
	{
		Shape->SetCollisionObjectType(ECC_WorldDynamic);
		Shape->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	}
}
