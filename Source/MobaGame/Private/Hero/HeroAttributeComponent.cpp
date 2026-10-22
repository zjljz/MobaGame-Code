#include "Hero/HeroAttributeComponent.h"

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceSubsystem.h"
#include "Hero/HeroCharacter.h"
#include "Hero/HeroDefinition.h"
#include "GameplayEffectAggregator.h"
#include "MobaMessageTags.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

UHeroAttributeComponent::UHeroAttributeComponent()
{
	SetIsReplicatedByDefault(true);
	PrimaryComponentTick.bCanEverTick = false;
}

void UHeroAttributeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorld()->GetTimerManager().ClearTimer(RegenerationTimer);
	bReady = false;
	Super::EndPlay(EndPlayReason);
}

void UHeroAttributeComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(UHeroAttributeComponent, TotalExperiencePoints, COND_OwnerOnly);
}

bool UHeroAttributeComponent::BeginInitialization(const UHeroDefinition* HeroDef, const UMatchRulesDefinition* MatchRules, FString& Reason)
{
	auto* Hero = Cast<AHeroCharacter>(GetOwner());
	if (!IsInGameThread() || !Hero || !Hero->HasAuthority() || bReady || bInitializing || !HeroDef || !MatchRules
		|| !HeroDef->ValidateInitialization(Reason) || !HeroDef->ValidateAttributes(MatchRules->AttributeRules, Reason)
		|| Hero->GetHeroAbilitySystem()->GetAvatarActor() != Hero || Hero->GetHeroAbilitySystem()->GetOwnerActor() != Hero)
	{
		return false;
	}

	Definition = HeroDef;
	Rules = MatchRules;
	bInitializing = true;
	bDead = false;
	TotalExperiencePoints = 0;
	ApplyGrowth(1);
	Hero->GetHeroAbilitySystem()->SetNumericAttributeBase(UHeroAttributeSet::GetDamageTakenMultiplierAttribute(), 1);
	return true;
}

bool UHeroAttributeComponent::FinishInitialization()
{
	if (!bInitializing || bReady || !GetOwner()->HasAuthority())
	{
		return false;
	}

	RestoreResources();
	bInitializing = false;
	bReady = true;
	LastRegenerationTime = GetWorld()->GetTimeSeconds();
	CaptureRegenerationInterval();
	GetWorld()->GetTimerManager().SetTimer(RegenerationTimer, this, &ThisClass::FlushRegeneration, 0.25f, true);
	NotifyProgression();
	return true;
}

void UHeroAttributeComponent::AbortInitialization()
{
	// 失败的新生实体由生成方销毁；不把半成品发布为 Ready。
	bInitializing = false;
	bReady = false;
}

FHeroExperienceResult UHeroAttributeComponent::AddExperience(int64 Reward)
{
	FHeroExperienceResult Result;
	auto* Hero = Cast<AHeroCharacter>(GetOwner());
	if (!IsInGameThread() || !Hero || !Hero->HasAuthority() || !bReady || bCoordinating || BatchDepth != 0 || Reward < 0)
	{
		return Result;
	}

	Result.bAccepted = true;
	Result.PreviousLevel = int32(Hero->GetAttributes()->GetHeroLevel());
	Result.NewLevel = Result.PreviousLevel;
	const TArray<int64>& Thresholds = Rules->AttributeRules.ExperienceThresholds;
	const int64 Remaining = Thresholds.Last() - TotalExperiencePoints;
	Result.AddedExperiencePoints = FMath::Min(Reward, Remaining);
	if (Result.AddedExperiencePoints == 0)
	{
		return Result;
	}

	BeginSourceBatch();
	TotalExperiencePoints += Result.AddedExperiencePoints;
	int32 Low = Result.PreviousLevel - 1;
	int32 High = Thresholds.Num();
	while (Low + 1 < High)
	{
		const int32 Middle = Low + (High - Low) / 2;
		if (Thresholds[Middle] <= TotalExperiencePoints)
		{
			Low = Middle;
		}
		else
		{
			High = Middle;
		}
	}

	Result.NewLevel = Low + 1;
	if (Result.NewLevel != Result.PreviousLevel)
	{
		ApplyGrowth(Result.NewLevel);
	}

	EndSourceBatch();
	NotifyProgression();
	return Result;
}

void UHeroAttributeComponent::FlushRegeneration()
{
	if (!GetOwner()->HasAuthority() || !bReady || bCoordinating || BatchDepth != 0)
	{
		return;
	}

	TGuardValue<bool> CoordinationGuard(bCoordinating, true);
	const double Now = GetWorld()->GetTimeSeconds();
	const double Elapsed = FMath::Max(0.0, Now - LastRegenerationTime);
	LastRegenerationTime = Now;
	auto* Hero = CastChecked<AHeroCharacter>(GetOwner());
	if (bDead || !Hero->IsAlive() || Elapsed == 0)
	{
		return;
	}

	const auto* Attributes = Hero->GetAttributes();
	double Health = Attributes->GetHealth();
	double Mana = Attributes->GetMana();
	double SegmentStart = Now - Elapsed;
	float MaxHealth = EvaluateRegenerationAttribute(0, SegmentStart);
	float MaxMana = EvaluateRegenerationAttribute(1, SegmentStart);
	TArray<double> Boundaries;
	for (const auto& Entry : RegenerationExpirations)
	{
		if (Entry.Value > SegmentStart && Entry.Value <= Now)
		{
			Boundaries.AddUnique(Entry.Value);
		}
	}

	Boundaries.AddUnique(Now);
	Boundaries.Sort();
	for (double Boundary : Boundaries)
	{
		Health = FMath::Min(double(MaxHealth), Health + double(EvaluateRegenerationAttribute(2, SegmentStart)) * (Boundary - SegmentStart));
		Mana = FMath::Min(double(MaxMana), Mana + double(EvaluateRegenerationAttribute(3, SegmentStart)) * (Boundary - SegmentStart));
		const float NextMaxHealth = EvaluateRegenerationAttribute(0, Boundary);
		const float NextMaxMana = EvaluateRegenerationAttribute(1, Boundary);
		Health = MaxHealth > 0 ? Health / MaxHealth * NextMaxHealth : 0;
		Mana = MaxMana > 0 ? Mana / MaxMana * NextMaxMana : 0;
		MaxHealth = NextMaxHealth;
		MaxMana = NextMaxMana;
		SegmentStart = Boundary;
	}

	IntegrationMaxHealth = MaxHealth;
	IntegrationMaxMana = MaxMana;
	TGuardValue<bool> IntegrationGuard(bIntegrating, true);
	auto* AbilitySystem = Hero->GetHeroAbilitySystem();
	AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), float(Health));
	AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetManaAttribute(), HasNoResource() ? 0 : float(Mana));
}

void UHeroAttributeComponent::StopForDeath()
{
	if (!GetOwner()->HasAuthority())
	{
		return;
	}

	bDead = true;
	LastRegenerationTime = GetWorld()->GetTimeSeconds();
	CastChecked<AHeroCharacter>(GetOwner())->GetHeroAbilitySystem()->SetNumericAttributeBase(UHeroAttributeSet::GetShieldAttribute(), 0);
}

void UHeroAttributeComponent::RestoreForRespawn()
{
	if (!GetOwner()->HasAuthority() || !bReady)
	{
		return;
	}

	TGuardValue<bool> CoordinationGuard(bCoordinating, true);
	RestoreResources();
	bDead = false;
	LastRegenerationTime = GetWorld()->GetTimeSeconds();
	CaptureRegenerationInterval();
}

void UHeroAttributeComponent::BeforeAttributeChange(const FGameplayAttribute& Attribute)
{
	if (Attribute == UHeroAttributeSet::GetMaxHealthAttribute() || Attribute == UHeroAttributeSet::GetMaxManaAttribute()
		|| Attribute == UHeroAttributeSet::GetHealthRegenerationAttribute() || Attribute == UHeroAttributeSet::GetManaRegenerationAttribute())
	{
		FlushRegeneration();
	}
}

void UHeroAttributeComponent::AfterAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
	if (!bReady || bCoordinating || BatchDepth != 0)
	{
		return;
	}

	TGuardValue<bool> CoordinationGuard(bCoordinating, true);
	const auto* Attributes = CastChecked<AHeroCharacter>(GetOwner())->GetAttributes();
	if (Attribute == UHeroAttributeSet::GetMaxHealthAttribute())
	{
		AdjustResource(UHeroAttributeSet::GetHealthAttribute(), EvaluateRegenerationAttribute(0, LastRegenerationTime), NewValue, Attributes->GetHealth());
	}
	else if (Attribute == UHeroAttributeSet::GetMaxManaAttribute())
	{
		AdjustResource(UHeroAttributeSet::GetManaAttribute(), EvaluateRegenerationAttribute(1, LastRegenerationTime), NewValue, Attributes->GetMana());
	}

	if (Attribute == UHeroAttributeSet::GetMaxHealthAttribute() || Attribute == UHeroAttributeSet::GetMaxManaAttribute()
		|| Attribute == UHeroAttributeSet::GetHealthRegenerationAttribute() || Attribute == UHeroAttributeSet::GetManaRegenerationAttribute())
	{
		CaptureRegenerationInterval();
	}
}

void UHeroAttributeComponent::BeginSourceBatch()
{
	if (!GetOwner()->HasAuthority())
	{
		return;
	}

	if (BatchDepth == 0)
	{
		FlushRegeneration();
		const auto* Attributes = CastChecked<AHeroCharacter>(GetOwner())->GetAttributes();
		BatchHealth = Attributes->GetHealth();
		BatchMana = Attributes->GetMana();
		BatchMaxHealth = Attributes->GetMaxHealth();
		BatchMaxMana = Attributes->GetMaxMana();
	}

	++BatchDepth;
}

void UHeroAttributeComponent::EndSourceBatch()
{
	if (!GetOwner()->HasAuthority() || BatchDepth <= 0 || --BatchDepth != 0 || !bReady)
	{
		return;
	}

	TGuardValue<bool> CoordinationGuard(bCoordinating, true);
	const auto* Attributes = CastChecked<AHeroCharacter>(GetOwner())->GetAttributes();
	AdjustResource(UHeroAttributeSet::GetHealthAttribute(), BatchMaxHealth, Attributes->GetMaxHealth(), BatchHealth);
	AdjustResource(UHeroAttributeSet::GetManaAttribute(), BatchMaxMana, Attributes->GetMaxMana(), BatchMana);
	CaptureRegenerationInterval();
}

bool UHeroAttributeComponent::ValidateModifiers(const FGameplayEffectSpec& Spec) const
{
	for (const auto& Modifier : Spec.Def->Modifiers)
	{
		float Magnitude = 0;
		if (!Modifier.ModifierMagnitude.AttemptCalculateMagnitude(Spec, Magnitude) || !FMath::IsFinite(Magnitude))
		{
			return false;
		}

		const auto Attribute = Modifier.Attribute;
		const auto Operation = Modifier.ModifierOp;
		if (Attribute == UHeroAttributeSet::GetManaAttribute())
		{
			if (Spec.Def->DurationPolicy != EGameplayEffectDurationType::Instant || Operation != EGameplayModOp::AddBase || Magnitude > 0 || (HasNoResource() && Magnitude < 0))
			{
				return false;
			}

			continue;
		}

		if (Spec.Def->DurationPolicy == EGameplayEffectDurationType::Instant || Attribute == UHeroAttributeSet::GetHealthAttribute()
			|| Attribute == UHeroAttributeSet::GetHeroLevelAttribute() || Attribute == UHeroAttributeSet::GetShieldAttribute())
		{
			return false;
		}

		if (Attribute == UHeroAttributeSet::GetMoveSpeedAttribute())
		{
			const float MinimumSlow = Rules ? Rules->AttributeRules.MinimumSlowMultiplier : 1;
			const bool bValidSpeed = ((Operation == EGameplayModOp::AddBase && Magnitude >= 0)
				|| (Operation == EGameplayModOp::MultiplyAdditive && Magnitude >= 1)
				|| (Operation == EGameplayModOp::MultiplyCompound && Magnitude >= MinimumSlow && Magnitude <= 1 && Spec.Def->GetStackingType() == EGameplayEffectStackingType::None));
			if (!bValidSpeed)
			{
				return false;
			}
		}
		else if (Attribute == UHeroAttributeSet::GetArmorAttribute() || Attribute == UHeroAttributeSet::GetMagicResistanceAttribute()
			|| Attribute == UHeroAttributeSet::GetCriticalChanceAttribute() || Attribute == UHeroAttributeSet::GetCriticalDamageMultiplierAttribute()
			|| Attribute == UHeroAttributeSet::GetCooldownReductionAttribute())
		{
			if (Operation != EGameplayModOp::AddBase)
			{
				return false;
			}
		}
		else if (Attribute == UHeroAttributeSet::GetDamageTakenMultiplierAttribute())
		{
			if (Operation != EGameplayModOp::MultiplyCompound || Magnitude < 0)
			{
				return false;
			}
		}
		else if (Attribute == UHeroAttributeSet::GetMaxHealthAttribute() || Attribute == UHeroAttributeSet::GetMaxManaAttribute()
			|| Attribute == UHeroAttributeSet::GetAttackDamageAttribute() || Attribute == UHeroAttributeSet::GetAbilityPowerAttribute()
			|| Attribute == UHeroAttributeSet::GetHealthRegenerationAttribute() || Attribute == UHeroAttributeSet::GetManaRegenerationAttribute())
		{
			if ((Operation != EGameplayModOp::AddBase && Operation != EGameplayModOp::MultiplyAdditive && Operation != EGameplayModOp::MultiplyCompound)
				|| (Operation == EGameplayModOp::MultiplyAdditive && Magnitude < 1) || Magnitude < 0)
			{
				return false;
			}
		}
		else
		{
			return false;
		}
	}

	return true;
}

void UHeroAttributeComponent::RefreshSourceInterval()
{
	if (GetOwner()->HasAuthority() && bReady && !bCoordinating && BatchDepth == 0)
	{
		FlushRegeneration();
		CaptureRegenerationInterval();
	}
}

void UHeroAttributeComponent::ApplyGrowth(int32 Level)
{
	auto* AbilitySystem = CastChecked<AHeroCharacter>(GetOwner())->GetHeroAbilitySystem();
	AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetHeroLevelAttribute(), float(Level));
#define APPLY_HERO_GROWTH(Name) \
	AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::Get##Name##Attribute(), float(double(Definition->Attributes.Name.BaseValue) \
		+ double(Definition->Attributes.Name.GrowthPerLevel) * (Level - 1)))
	APPLY_HERO_GROWTH(MaxHealth);
	APPLY_HERO_GROWTH(MaxMana);
	APPLY_HERO_GROWTH(HealthRegeneration);
	APPLY_HERO_GROWTH(ManaRegeneration);
	APPLY_HERO_GROWTH(AttackDamage);
	APPLY_HERO_GROWTH(AbilityPower);
	APPLY_HERO_GROWTH(Armor);
	APPLY_HERO_GROWTH(MagicResistance);
	APPLY_HERO_GROWTH(CriticalChance);
	APPLY_HERO_GROWTH(CriticalDamageMultiplier);
	APPLY_HERO_GROWTH(CooldownReduction);
	APPLY_HERO_GROWTH(MoveSpeed);
#undef APPLY_HERO_GROWTH
}

void UHeroAttributeComponent::RestoreResources()
{
	auto* Hero = CastChecked<AHeroCharacter>(GetOwner());
	auto* AbilitySystem = Hero->GetHeroAbilitySystem();
	AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), Hero->GetAttributes()->GetMaxHealth());
	AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetManaAttribute(), HasNoResource() ? 0 : Hero->GetAttributes()->GetMaxMana());
	AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetShieldAttribute(), 0);
}

void UHeroAttributeComponent::NotifyProgression()
{
	GetOwner()->ForceNetUpdate();
	OnRep_TotalExperiencePoints();
}

void UHeroAttributeComponent::AdjustResource(const FGameplayAttribute& Resource, float OldMaximum, float NewMaximum, float OldResource)
{
	auto* Hero = CastChecked<AHeroCharacter>(GetOwner());
	const bool bUnavailable = ((Resource == UHeroAttributeSet::GetHealthAttribute() && (bDead || !Hero->IsAlive()))
		|| (Resource == UHeroAttributeSet::GetManaAttribute() && HasNoResource()));
	const double Ratio = OldMaximum > 0 ? double(OldResource) / OldMaximum : 0;
	Hero->GetHeroAbilitySystem()->SetNumericAttributeBase(Resource, bUnavailable ? 0 : float(FMath::Clamp(Ratio * NewMaximum, 0.0, double(NewMaximum))));
}

void UHeroAttributeComponent::CaptureRegenerationInterval()
{
	auto* AbilitySystem = CastChecked<AHeroCharacter>(GetOwner())->GetHeroAbilitySystem();
	RegenerationCaptures.Reset();
	for (const FGameplayAttribute& Attribute : {UHeroAttributeSet::GetMaxHealthAttribute(), UHeroAttributeSet::GetMaxManaAttribute(),
		UHeroAttributeSet::GetHealthRegenerationAttribute(), UHeroAttributeSet::GetManaRegenerationAttribute()})
	{
		FGameplayEffectAttributeCaptureSpec Capture(FGameplayEffectAttributeCaptureDefinition(Attribute, EGameplayEffectAttributeCaptureSource::Target, true));
		AbilitySystem->CaptureAttributeForGameplayEffect(Capture);
		RegenerationCaptures.Add(MoveTemp(Capture));
	}

	RegenerationExpirations.Reset();
	RegenerationTags.Reset();
	AbilitySystem->GetOwnedGameplayTags(RegenerationTags);
	for (const FActiveGameplayEffectHandle Handle : AbilitySystem->GetActiveEffects(FGameplayEffectQuery()))
	{
		const auto* Effect = AbilitySystem->GetActiveGameplayEffect(Handle);
		if (Effect && Effect->GetDuration() > 0 && Effect->Spec.GetPeriod() == 0)
		{
			RegenerationExpirations.Add(Handle, Effect->GetEndTime());
		}
	}
}

float UHeroAttributeComponent::EvaluateRegenerationAttribute(int32 Index, double Time) const
{
	if (!RegenerationCaptures.IsValidIndex(Index))
	{
		return 0;
	}

	FAggregatorEvaluateParameters Parameters;
	Parameters.SourceTags = &RegenerationTags;
	Parameters.TargetTags = &RegenerationTags;
	for (const auto& Entry : RegenerationExpirations)
	{
		if (Entry.Value <= Time)
		{
			Parameters.IgnoreHandles.Add(Entry.Key);
		}
	}

	float Value = 0;
	if (!RegenerationCaptures[Index].AttemptCalculateAttributeMagnitude(Parameters, Value) || !FMath::IsFinite(Value))
	{
		return 0;
	}

	if ((Index == 1 || Index == 3) && HasNoResource())
	{
		return 0;
	}

	return FMath::Max(Index < 2 ? 1.0f : 0.0f, Value);
}

bool UHeroAttributeComponent::HasNoResource() const
{
	const auto* Hero = Cast<AHeroCharacter>(GetOwner());
	const auto* HeroDef = Definition ? Definition.Get() : (Hero ? Hero->GetDefinition() : nullptr);
	return HeroDef && HeroDef->ResourceType == EHeroResourceType::None;
}

int32 UHeroAttributeComponent::GetMaximumLevel() const
{
	const auto* CurrentRules = Rules ? Rules.Get() : GetWorld()->GetSubsystem<UExperienceSubsystem>()->GetRules();
	return CurrentRules ? CurrentRules->AttributeRules.ExperienceThresholds.Num() : 0;
}

float UHeroAttributeComponent::GetMaximumCooldownReduction() const
{
	const auto* CurrentRules = Rules ? Rules.Get() : GetWorld()->GetSubsystem<UExperienceSubsystem>()->GetRules();
	return CurrentRules ? CurrentRules->AttributeRules.MaximumCooldownReduction : 0;
}

float UHeroAttributeComponent::GetResourceMaximumDuringIntegration(bool bMana, float EffectiveMaximum) const
{
	return bIntegrating ? (bMana ? IntegrationMaxMana : IntegrationMaxHealth) : EffectiveMaximum;
}

void UHeroAttributeComponent::OnRep_TotalExperiencePoints()
{
	GMP::FMessageUtils::SendObjectMessage(this, MOBA_MESSAGE_HERO_PROGRESSION_CHANGED, TotalExperiencePoints);
}

FHeroAttributeBatchScope::FHeroAttributeBatchScope(AHeroCharacter* Hero) : Component(Hero ? Hero->GetAttributeComponent() : nullptr)
{
	if (Component)
	{
		Component->BeginSourceBatch();
	}
}

FHeroAttributeBatchScope::~FHeroAttributeBatchScope()
{
	if (Component)
	{
		Component->EndSourceBatch();
	}
}
