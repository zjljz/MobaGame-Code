#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ClientUIConfiguration.generated.h"

class UFrontendWidget;
class UStatusPanelWidget;
class UTargetPreviewWidget;
class UClientFeedbackWidget;
class UClassicHUDWidget;

/** 客户端表现根；类硬引用使根页面及其默认子项随本配置可达。 */
UCLASS()
class UClientUIConfiguration : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, Category="Widgets")
	TSubclassOf<UFrontendWidget> FrontendClass;

	UPROPERTY(EditDefaultsOnly, Category="Widgets")
	TSubclassOf<UStatusPanelWidget> StatusClass;

	UPROPERTY(EditDefaultsOnly, Category="Widgets")
	TSubclassOf<UTargetPreviewWidget> TargetPreviewClass;

	UPROPERTY(EditDefaultsOnly, Category="Widgets")
	TSubclassOf<UClientFeedbackWidget> FeedbackClass;

	UPROPERTY(EditDefaultsOnly, Category="Classic HUD")
	FPrimaryAssetId ClassicModeId;

	UPROPERTY(EditDefaultsOnly, Category="Classic HUD")
	TSubclassOf<UClassicHUDWidget> ClassicHUDClass;
};
