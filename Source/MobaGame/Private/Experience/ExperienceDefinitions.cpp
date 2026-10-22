#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "Items/ItemDefinition.h"

#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/PlayerStart.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

bool FMatchPlayerConfiguration::IsValid(int32 TeamCount, FString& Reason) const
{
	if (MaximumParticipants < 1 || MinimumPlayersToStart < 1 || MinimumPlayersToStart > MaximumParticipants || MaximumPlayersPerTeam < 1 || MaximumPlayersPerTeam > MaximumParticipants
		|| TeamCount < 1 || int64(TeamCount) * MaximumPlayersPerTeam < MaximumParticipants)
	{
		Reason = TEXT("比赛人数需为正，最低开局人数不能超过容量，队伍总容量必须容纳参赛者。");
		return false;
	}

	return true;
}

bool FHeroSelectionRules::IsValid(FString& Reason) const
{
	TSet<FSoftObjectPath> UniqueHeroDefPaths;
	for (const auto& HeroDef : AllowedHeroes)
	{
		if (HeroDef.IsNull() || UniqueHeroDefPaths.Contains(HeroDef.ToSoftObjectPath()))
		{
			Reason = TEXT("选人范围包含空或重复英雄。");
			return false;
		}

		UniqueHeroDefPaths.Add(HeroDef.ToSoftObjectPath());
	}

	if (AllowedHeroes.IsEmpty() || Policy > EHeroSelectionPolicy::PlayerChoice || (Policy == EHeroSelectionPolicy::Fixed && !Allows(FixedHero.ToSoftObjectPath())))
	{
		Reason = TEXT("选人策略或固定英雄不合法。");
		return false;
	}

	return true;
}

bool FHeroSelectionRules::Allows(const FSoftObjectPath& HeroDefPath) const
{
	return HeroDefPath.IsValid() && AllowedHeroes.ContainsByPredicate([&HeroDefPath](const auto& AllowedHeroDef) { return AllowedHeroDef.ToSoftObjectPath() == HeroDefPath; });
}

bool FItemEconomyRules::IsValid(FString& Reason) const
{
	if (InitialGold < 0 || (!bItemsEnabled && !ItemCatalog.IsNull())
		|| (bItemsEnabled && (ItemCatalog.IsNull() || ShopCenter.ContainsNaN() || !FMath::IsFinite(ShopRadius) || ShopRadius <= 0)))
	{
		Reason = TEXT("物品规则需要有效金币、目录与商店范围；禁用物品不得引用目录。");
		return false;
	}

	return true;
}

#if WITH_EDITOR
EDataValidationResult UMatchRulesDefinition::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	FString Reason;
	if (!IsValid(Reason))
	{
		Context.AddError(FText::FromString(Reason.IsEmpty() ? TEXT("比赛规则无效。") : Reason));
	}

	for (const auto& HeroRef : HeroSelectionRules.AllowedHeroes)
	{
		const auto* HeroDef = HeroRef.LoadSynchronous();
		if (!HeroDef || !HeroDef->ValidateAttributes(AttributeRules, Reason))
		{
			Context.AddError(FText::FromString(FString::Printf(TEXT("英雄成长不符合当前比赛规则：%s %s"), *HeroRef.ToString(), *Reason)));
		}
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

bool UMatchRulesDefinition::IsValid(FString& Reason) const
{
	return TeamRules.IsValid(Reason) && Players.IsValid(TeamRules.Teams.Num(), Reason) && HeroSelectionRules.IsValid(Reason)
		&& CombatRules.IsValid() && AttributeRules.IsValid(Reason) && ItemEconomyRules.IsValid(Reason);
}

FPrimaryAssetId UMatchRulesDefinition::GetPrimaryAssetId() const
{
	return {TEXT("MatchRules"), GetFName()};
}

#if WITH_EDITOR
EDataValidationResult UExperienceDefinition::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	TSet<EHeroInputProfile> SeenInputProfileKinds;
	for (const auto& InputProfileDefRef : InputProfiles)
	{
		const UInputProfileDefinition* LoadedInputProfileDef = InputProfileDefRef.LoadSynchronous();
		if (!LoadedInputProfileDef || SeenInputProfileKinds.Contains(LoadedInputProfileDef->Profile) || !AllowedInputProfiles.Contains(LoadedInputProfileDef->Profile))
		{
			Context.AddError(NSLOCTEXT("MobaValidation", "Profiles", "输入配置必须可加载、唯一，并匹配允许集合。"));
		}

		if (LoadedInputProfileDef)
		{
			SeenInputProfileKinds.Add(LoadedInputProfileDef->Profile);
		}
	}

	if (SeenInputProfileKinds.Num() != AllowedInputProfiles.Num())
	{
		Context.AddError(NSLOCTEXT("MobaValidation", "RequiredProfiles", "每个允许输入必须有唯一映射；前台可为空。"));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

FPrimaryAssetId UExperienceDefinition::GetPrimaryAssetId() const
{
	return {TEXT("Experience"), GetFName()};
}

#if WITH_EDITOR
EDataValidationResult UModeDefinition::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	TSet<FName> SeenModeConfigNames;
	for (const auto& ModeConfig : ModeConfigs)
	{
		if (ModeConfig.ModeConfigName.IsNone() || SeenModeConfigNames.Contains(ModeConfig.ModeConfigName) || ModeConfig.ExperienceDef.IsNull() || ModeConfig.MapDef.IsNull() || ModeConfig.RulesDef.IsNull())
		{
			Context.AddError(NSLOCTEXT("MobaValidation", "ModeConfiguration", "模式组合需唯一名称及完整 Experience/Map/Rules 引用。"));
		}

		SeenModeConfigNames.Add(ModeConfig.ModeConfigName);
	}

	if (ModeConfigs.IsEmpty())
	{
		Context.AddError(NSLOCTEXT("MobaValidation", "EmptyMode", "模式需要至少一个合法组合。"));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

const FModeConfiguration* UModeDefinition::FindModeConfig(FName ModeConfigName) const
{
	return ModeConfigs.FindByPredicate([ModeConfigName](const auto& ModeConfig) { return ModeConfig.ModeConfigName == ModeConfigName; });
}

FPrimaryAssetId UModeDefinition::GetPrimaryAssetId() const
{
	return {TEXT("Mode"), GetFName()};
}

#if WITH_EDITOR
EDataValidationResult UMapDefinition::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	if (World.IsNull() || !World.LoadSynchronous())
	{
		Context.AddError(NSLOCTEXT("MobaValidation", "World", "MapDefinition 必须指定真实可加载的 World 地图。"));
	}
	else
	{
		const int32 StartCount = CountPlayerStarts(World->PersistentLevel);
		bool bHasNavigation = false;
		if (World->PersistentLevel)
		{
			for (AActor* Actor : World->PersistentLevel->Actors)
			{
				if (IsValid(Actor) && Actor->IsA<ANavMeshBoundsVolume>())
				{
					bHasNavigation = true;
				}
			}
		}

		if (MinimumPlayerStarts < 1 || StartCount < MinimumPlayerStarts || (bRequireNavigation && !bHasNavigation))
		{
			Context.AddError(NSLOCTEXT("MobaValidation", "MapSetup", "地图实际出生点不足，或缺少所要求的导航范围。"));
		}
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

int32 UMapDefinition::CountPlayerStarts(const ULevel* Level)
{
	if (!Level)
	{
		return 0;
	}

	int32 Count = 0;
	for (const AActor* Actor : Level->Actors)
	{
		if (IsValid(Actor) && Actor->IsA<APlayerStart>())
		{
			++Count;
		}
	}

	return Count;
}

FPrimaryAssetId UMapDefinition::GetPrimaryAssetId() const
{
	return FPrimaryAssetId(TEXT("MapDefinition"), GetFName());
}

