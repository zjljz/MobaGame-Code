#pragma once

#include "CoreMinimal.h"

class UAbilitySystemComponent;

namespace MobaCombat
{
	/** 仅服务器；按正负策略移除可驱散 GE，冷却、装备和显式不可驱散效果始终保留。 */
	MOBAGAME_API int32 Dispel(UAbilitySystemComponent* TargetASC, bool bStrong, bool bBeneficial, bool bHarmful);
}

