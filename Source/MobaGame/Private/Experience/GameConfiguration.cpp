#include "Experience/GameConfiguration.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"

EDataValidationResult UGameConfiguration::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	FString Reason;
	if (!ValidateConfiguration(Reason))
	{
		Context.AddError(FText::FromString(Reason));
	}

	if (FrontendWorld.IsNull() || LocalMatchWorld.IsNull() || FrontendWorld == LocalMatchWorld)
	{
		Context.AddError(FText::FromString(TEXT("前台与本地比赛必须配置不同的宿主 World。")));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

bool UGameConfiguration::ValidateConfiguration(FString& Reason) const
{
	if (FrontendExperience.IsNull() || !FrontendExperience.ToSoftObjectPath().GetLongPackageName().StartsWith(TEXT("/Game/")))
	{
		Reason = TEXT("前台 Experience 必须位于宿主 /Game。");
		return false;
	}

	TSet<FPrimaryAssetId> SeenModeIds;
	for (const FEnabledModeEntry& EnabledModeEntry : EnabledModes)
	{
		if (!EnabledModeEntry.ModeId.IsValid() || EnabledModeEntry.ModeId.PrimaryAssetType != FPrimaryAssetType(TEXT("Mode")) || SeenModeIds.Contains(EnabledModeEntry.ModeId) || EnabledModeEntry.ModeDef.IsNull())
		{
			Reason = TEXT("启用模式必须具有唯一有效 Mode 身份和非空定义软引用。");
			return false;
		}

		SeenModeIds.Add(EnabledModeEntry.ModeId);
	}

	Reason.Reset();
	return true;
}

const FEnabledModeEntry* UGameConfiguration::FindEnabledMode(FPrimaryAssetId ModeId) const
{
	return EnabledModes.FindByPredicate([ModeId](const auto& EnabledModeEntry) { return EnabledModeEntry.ModeId == ModeId; });
}

FPrimaryAssetId UGameConfiguration::GetPrimaryAssetId() const
{
	return {TEXT("GameConfiguration"), GetFName()};
}

