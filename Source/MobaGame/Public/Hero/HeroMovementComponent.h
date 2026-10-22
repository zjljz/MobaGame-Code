#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HeroMovementComponent.generated.h"

UCLASS()
class MOBAGAME_API UHeroMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	virtual float GetMaxSpeed() const override;

	virtual void PerformMovement(float DeltaSeconds) override;

	// 网络包始终走原生校验/确认；只在实际模拟边界阻止未获准移动。
	virtual void MoveAutonomous(float ClientTimeStamp, float DeltaTime, uint8 CompressedFlags, const FVector& NewAcceleration) override;
};
