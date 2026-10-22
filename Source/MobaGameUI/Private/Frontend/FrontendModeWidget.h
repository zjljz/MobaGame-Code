#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UObject/PrimaryAssetId.h"
#include "FrontendModeWidget.generated.h"

class UButton;
class UTextBlock;
class UFrontendWidget;

UCLASS(Abstract, Blueprintable)
class UFrontendModeWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;


public:
	void PresentMode(UFrontendWidget* MenuWidget, FPrimaryAssetId InModeId, const FText& Label, bool bSelected);


private:
	UFUNCTION()
	void ChooseMode();


private:
	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UButton> SelectButton;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> LabelText;

	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UWidget> SelectionMarker;

	TWeakObjectPtr<UFrontendWidget> OwnerMenu;
	FPrimaryAssetId ModeId;
};

