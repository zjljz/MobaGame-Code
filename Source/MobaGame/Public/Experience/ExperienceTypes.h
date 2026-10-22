#pragma once

#include "CoreMinimal.h"
#include "UObject/PrimaryAssetId.h"
#include "ExperienceTypes.generated.h"

UENUM(BlueprintType)
enum class EExperienceState : uint8
{
	Unloaded,
	Loading,
	Activating,
	ExperienceReady,
	Stopping,
	Failed
};

UENUM(BlueprintType)
enum class EExperienceFailure : uint8
{
	None,
	Busy,
	ModeNotAllowed,
	MissingMode,
	InvalidMode,
	MissingFeature,
	FeatureLoadFailed,
	MissingDefinition,
	UnsupportedSelectionPolicy,
	InvalidMapAssociation,
	AssetLoadFailed,
	MissingWorldContext,
	WorldUnavailable,
	MapLoadFailed,
	ActivationFailed,
	Timeout
};

USTRUCT(BlueprintType)
struct MOBAGAME_API FExperienceSnapshot
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadOnly)
	EExperienceState State = EExperienceState::Unloaded;

	UPROPERTY(BlueprintReadOnly)
	EExperienceFailure Failure = EExperienceFailure::None;

	UPROPERTY(BlueprintReadOnly)
	FPrimaryAssetId ModeId;

	UPROPERTY(BlueprintReadOnly)
	int64 Generation = 0;

	UPROPERTY(BlueprintReadOnly)
	FName ModeConfigName;
};
