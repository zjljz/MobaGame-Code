#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "GameplayAbilitySpecHandle.h"

class AHeroPlayerController;
class AHeroCharacter;
class AParticipantState;
class UHeroAbilitySystemComponent;

/** 只观察当前拥有端；身份校验失败时不保留半套对象引用。 */
struct FClientTargetContext
{
	bool HasSameBinding(const FClientTargetContext& Other) const;

	bool HasSamePresentation(const FClientTargetContext& Other) const;




	TWeakObjectPtr<AHeroPlayerController> Controller;
	TWeakObjectPtr<AHeroCharacter> Hero;

	// 当前拥有端的参赛者状态 Actor；与 ParticipantId 的身份值分开保存。
	TWeakObjectPtr<AParticipantState> Participant;

	// 观察当前英雄的单个 ASC，不持有能力对象集合。
	TWeakObjectPtr<UHeroAbilitySystemComponent> Abilities;

	TWeakObjectPtr<UWorld> World;
	FGuid MatchId;
	FGuid EntityId;
	FGuid ParticipantId;

	// 当前控制绑定的 ConnectionEpoch；变化时旧绑定和旧反馈均失效。
	uint32 Epoch = 0;

	uint32 LifeGeneration = 0;
	uint32 DeathCount = 0;
	uint32 LoadoutRevision = 0;

	// ASC 能力状态版本；与槽映射的 LoadoutRevision 分开比较。
	uint32 StateRevision = 0;

	// 当前 Controller 的输入 Profile 值；切换时使旧预览选择失效。
	uint8 Profile = 0;

	uint8 LifeState = 0;
	bool bValid = false;
	bool bAlive = false;
	bool bReady = false;
};

struct FTargetPreviewSlot
{
	bool operator==(const FTargetPreviewSlot& Other) const;




	FGameplayTag Slot;
	FGameplayAbilitySpecHandle Handle;
	bool bEnabled = false;
};

namespace MobaClientTarget
{
	FClientTargetContext ReadContext(AHeroPlayerController* Controller);

	TArray<FTargetPreviewSlot> ReadSlots(const FClientTargetContext& Context);
}

