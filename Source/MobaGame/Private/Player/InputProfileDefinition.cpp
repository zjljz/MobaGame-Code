#include "Player/InputProfileDefinition.h"

#include "InputAction.h"
#include "InputMappingContext.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#if WITH_EDITOR
EDataValidationResult UInputProfileDefinition::IsDataValid(FDataValidationContext& Context) const
{
	Super::IsDataValid(Context);
	Mapping.LoadSynchronous();
	SwitchProfile.LoadSynchronous();
	PrimaryAbility.LoadSynchronous();
	UnitAttack.LoadSynchronous();
	TargetMapping.LoadSynchronous();
	TargetAbility.LoadSynchronous();
	if (Profile == EHeroInputProfile::WASD)
	{
		Move.LoadSynchronous();
	}
	else
	{
		Click.LoadSynchronous();
	}

	if (!IsLoaded())
	{
		Context.AddError(NSLOCTEXT("MobaValidation", "Input", "InputProfile 的映射及本模式必需动作必须可加载。"));
	}

	return Context.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

bool UInputProfileDefinition::IsLoaded() const
{
	return (TargetMapping.IsNull() && TargetAbility.IsNull() || TargetMapping.IsValid() && TargetAbility.IsValid()) && Mapping.IsValid() && SwitchProfile.IsValid()
		&& PrimaryAbility.IsValid() && UnitAttack.IsValid() && (Profile == EHeroInputProfile::WASD ? Move.IsValid() : Click.IsValid());
}
