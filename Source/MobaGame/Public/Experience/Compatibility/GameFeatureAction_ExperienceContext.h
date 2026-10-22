#pragma once

#include "CoreMinimal.h"
#include "GameFeatureAction.h"
#include "GameFeatureAction_ExperienceContext.generated.h"

// 仅保留旧保存资产的反射加载兼容；当前 Experience 流程不使用此动作。
UCLASS()
class MOBAGAME_API UGameFeatureAction_ExperienceContext : public UGameFeatureAction
{
	GENERATED_BODY()
};
