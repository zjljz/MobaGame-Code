#include "Hero/HeroMovementComponent.h"
#include "Hero/HeroCharacter.h"
#include "Combat/HeroAttributeSet.h"
#include "AbilitySystemComponent.h"
#include "MobaGameplayTags.h"

float UHeroMovementComponent::GetMaxSpeed() const
{
	const AHeroCharacter* Hero = Cast<AHeroCharacter>(CharacterOwner);
	if (!Hero)
	{
		return Super::GetMaxSpeed();
	}

	const auto* ASC = Hero->GetAbilitySystemComponent();
	if (ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Stun) || ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Root))
	{
		return 0;
	}

	return MovementMode == MOVE_None ? 0 : Hero->GetAttributes()->GetMoveSpeed();
}

void UHeroMovementComponent::PerformMovement(float DeltaSeconds)
{
	const AHeroCharacter* Hero = Cast<AHeroCharacter>(CharacterOwner);
	if (Hero
		&& (Hero->GetAbilitySystemComponent()->HasMatchingGameplayTag(MobaGameplayTags::State_Stun)
			|| Hero->GetAbilitySystemComponent()->HasMatchingGameplayTag(MobaGameplayTags::State_Root)))
	{
		StopMovementImmediately();
		Acceleration = FVector::ZeroVector;
	}

	Super::PerformMovement(DeltaSeconds);
}

void UHeroMovementComponent::MoveAutonomous(float ClientTimeStamp, float DeltaTime, uint8 CompressedFlags, const FVector& NewAcceleration)
{
	const AHeroCharacter* Hero = Cast<AHeroCharacter>(CharacterOwner);
	if (Hero && Hero->HasAuthority() && !Hero->CanAcceptPlayerInput())
	{
		StopMovementImmediately();
		return;
	}

	Super::MoveAutonomous(ClientTimeStamp, DeltaTime, CompressedFlags, NewAcceleration);
}
