#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "Hero/HeroLifeState.h"
#include "GameplayAbilitySpecHandle.h"
#include "GameplayTagContainer.h"

class AHeroPlayerController;
class UMatchRulesDefinition;

struct FStatusAbilityRow
{
	FGameplayTag Slot;
	FGameplayAbilitySpecHandle Handle;
	FText Name;
	int32 Level = 0;
	bool bEnabled = false;
	bool bCooldownKnown = false;
	float CooldownRemaining = 0;
	float CooldownDuration = 0;
};

struct FStatusItemRow
{
	// 物品实例的唯一标识；ItemId 则标识物品定义。
	FGuid Instance;

	FName ItemId;
	int32 Quantity = 0;
};

/** 仅显示值；每次读取重新生成，不持有 Actor、ASC、Spec 或资产引用。 */
struct FStatusPanelSnapshot
{
	bool bOwnedHero = false;
	bool bCombatReady = false;
	bool bProgressionReady = false;
	bool bMaximumLevel = false;
	bool bUsesMana = false;
	bool bAbilitiesReady = false;
	bool bInventoryReady = false;
	bool bItemsEnabled = false;

	// 当前拥有英雄的 EntityId，不保存 Actor 引用。
	FGuid Entity;

	// 当前英雄控制绑定的 ConnectionEpoch，用于区分重新连接后的快照。
	uint32 Epoch = 0;

	uint32 LifeGeneration = 0;
	uint32 LoadoutRevision = 0;
	uint32 AbilityStateRevision = 0;
	uint32 InventoryRevision = 0;
	EHeroLifeState LifeState = EHeroLifeState::Alive;
	float Health = 0;
	float MaxHealth = 0;
	float Shield = 0;
	float Mana = 0;
	float MaxMana = 0;
	float RespawnRemaining = 0;
	int32 Gold = 0;
	int32 HeroLevel = 0;
	int32 MaximumHeroLevel = 0;
	int64 TotalExperiencePoints = 0;
	int64 LevelExperiencePoints = 0;
	int64 RequiredExperiencePoints = 0;
	float ExperienceProgress = 0;
	TArray<FStatusAbilityRow> Abilities;
	TStaticArray<FStatusItemRow, 6> Items;
};

namespace MobaStatus
{
	/**
	 * 仅 Game Thread，读取 Controller 自己的 Hero；Rules 必须来自同一 World 当前就绪上下文。
	 * 缺失/不匹配的区块保留 Ready=false 且不返回该区块旧值；无拥有关系时完全不查询私有库存。
	 */
	FStatusPanelSnapshot ReadOwnedStatus(AHeroPlayerController* Controller, const UMatchRulesDefinition* Rules);
}

