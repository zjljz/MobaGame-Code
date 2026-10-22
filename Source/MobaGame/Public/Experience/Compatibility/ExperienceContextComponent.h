#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ExperienceContextComponent.generated.h"

// 仅保留旧反射名；World 运行上下文统一由 ExperienceSubsystem 持有。
UCLASS()
class MOBAGAME_API UExperienceContextComponent : public UActorComponent
{
	GENERATED_BODY()
};
