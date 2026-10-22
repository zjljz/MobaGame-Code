#pragma once

#include "CoreMinimal.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "TargetFeedbackSubsystem.generated.h"

/** 只登记 C 所有的两层；视口、Controller 与 World 清理由共享挂载器负责。 */
UCLASS()
class UTargetFeedbackSubsystem : public ULocalPlayerSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	virtual void Deinitialize() override;


private:
	bool bPreviewRegistered = false;
	bool bFeedbackRegistered = false;
};
