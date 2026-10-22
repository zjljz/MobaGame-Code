#include "Combat/TargetDamageAbility.h"

#include "Combat/CombatDamageExecution.h"
#include "Combat/CombatEffectContext.h"
#include "Combat/AbilityTargetingPreset.h"
#include "Combat/AbilityTargetingRequest.h"
#include "TargetingSystem/TargetingSubsystem.h"
#include "Player/HeroPlayerController.h"
#include "Combat/CombatEffectParameters.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Hero/HeroCharacter.h"
#include "Combat/CombatantInterface.h"
#include "Combat/AbilityProjectile.h"
#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceSubsystem.h"
#include "Engine/World.h"
#include "MobaGameplayTags.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

namespace
{
	/** 限定单次投送使用的伤害 GE；不接受持续/周期效果或额外属性修改路径。 */
	bool ValidateDamageGE(const UGameplayEffect* DamageGE)
	{
		return DamageGE && DamageGE->DurationPolicy == EGameplayEffectDurationType::Instant
			&& DamageGE->Period.IsStatic() && DamageGE->Period.GetValueChecked() == UGameplayEffect::NO_PERIOD
			&& DamageGE->FindComponent<UCombatDamageParametersComponent>() && DamageGE->Executions.Num() == 1
			&& DamageGE->Executions[0].CalculationClass == UCombatDamageExecution::StaticClass() && DamageGE->Modifiers.IsEmpty()
			&& DamageGE->Executions[0].CalculationModifiers.IsEmpty() && DamageGE->Executions[0].ConditionalGameplayEffects.IsEmpty()
			&& !DamageGE->FindComponent<UCombatHealingParametersComponent>() && !DamageGE->FindComponent<UCombatShieldEffectComponent>();
	}
}

UTargetDamageAbility::UTargetDamageAbility()
{
	FGameplayTagContainer AbilityClassificationTags(MobaGameplayTags::Ability_CancelOnDeath);
	AbilityClassificationTags.AddTag(MobaGameplayTags::Ability_Active);
	SetAssetTags(AbilityClassificationTags);
}

void UTargetDamageAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	bDeliveryCommitted = false;
	FailureReason.Reset();
	const UAbilityTargetEvent* TargetEvent = Cast<UAbilityTargetEvent>(CurrentEventData.OptionalObject.Get());
	AActor* SourceActor = ActorInfo ? ActorInfo->AvatarActor.Get() : nullptr;
	const ICombatantInterface* SourceCombatant = Cast<ICombatantInterface>(SourceActor);
	const UMatchRulesDefinition* MatchRules = SourceActor ? SourceActor->GetWorld()->GetSubsystem<UExperienceSubsystem>()->GetRules() : nullptr;
	if (!SourceActor || !SourceActor->HasAuthority() || !SourceCombatant || !SourceCombatant->IsCombatAlive() || !SourceCombatant->GetCombatAbilitySystem()
		|| !MatchRules || !TargetingPreset || !TargetEvent || !ValidateDamageGE(DamageEffect.GetDefaultObject()) || GetAbilityLevel() <= 0)
	{
		FinishActivation(TEXT("DeliveryConfig"));
		return;
	}

	const FAbilityTargetIntent TargetIntent = TargetEvent->Intent;
	const FAbilityTargetGeometry TargetGeometry = GetGeometry(&MatchRules->CombatRules);
	FString Reason;
	if (!MobaTargeting::ValidateIntent(SourceActor->GetActorLocation(), TargetGeometry, TargetIntent, Reason))
	{
		FinishActivation(Reason);
		return;
	}

	if (Delivery > EAbilityDelivery::Projectile || !FMath::IsFinite(ProjectileSpeed) || ProjectileSpeed <= 0 || !FMath::IsFinite(ProjectileLifetime) || ProjectileLifetime <= 0
		|| (Delivery == EAbilityDelivery::Projectile && TargetIntent.Policy != EAbilityTargetPolicy::Direction && TargetIntent.Policy != EAbilityTargetPolicy::Unit
			&& TargetIntent.Policy != EAbilityTargetPolicy::Point))
	{
		FinishActivation(TEXT("ProjectilePolicy"));
		return;
	}

	if (Delivery == EAbilityDelivery::Projectile && TargetIntent.Policy == EAbilityTargetPolicy::Point
		&& FVector::DistSquaredXY(SourceActor->GetActorLocation(), TargetIntent.Point) < UE_SMALL_NUMBER)
	{
		FinishActivation(TEXT("Direction"));
		return;
	}

	PendingDeliverySnapshot = NewObject<UAbilityDeliverySnapshot>();
	UAbilityDeliverySnapshot* DeliverySnapshot = PendingDeliverySnapshot;
	DeliverySnapshot->Entity = SourceCombatant->GetCombatEntity();
	DeliverySnapshot->Participant = SourceCombatant->GetCombatParticipant();
	DeliverySnapshot->Match = SourceCombatant->GetCombatMatch();
	DeliverySnapshot->Team = SourceCombatant->GetCombatTeam();
	DeliverySnapshot->Origin = SourceActor->GetActorLocation();
	DeliverySnapshot->Geometry = TargetGeometry;

	UTargetingSubsystem* TargetingSystem = UTargetingSubsystem::Get(SourceActor->GetWorld());
	FTargetingSourceContext SourceContext;
	SourceContext.SourceActor = SourceActor;
	TargetingRequest = UTargetingSubsystem::MakeTargetRequestHandle(TargetingPreset, SourceContext);
	FAbilityTargetingRequestData& Request = UE::TargetingSystem::TTargetingDataStore<FAbilityTargetingRequestData>::FindOrAdd(TargetingRequest);
	Request.Unit = TargetIntent.Unit.Get();
	Request.Point = TargetIntent.Point;
	Request.Direction = TargetIntent.Direction;
	Request.Origin = DeliverySnapshot->Origin;
	Request.Entity = DeliverySnapshot->Entity;
	Request.Match = DeliverySnapshot->Match;
	Request.Team = DeliverySnapshot->Team;
	Request.Geometry = TargetGeometry;
	Request.Delivery = Delivery;
	FTargetingImmediateTaskData::FindOrAdd(TargetingRequest).bReleaseOnCompletion = true;
	TargetingSystem->ExecuteTargetingRequestWithHandle(TargetingRequest, FTargetingRequestDelegate::CreateUObject(this, &ThisClass::OnTargetingCompleted));
}

void UTargetDamageAbility::EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	const UAbilityTargetEvent* TargetEvent = Cast<UAbilityTargetEvent>(CurrentEventData.OptionalObject.Get());
	const TWeakObjectPtr<AHeroPlayerController> RequestController = TargetEvent ? TargetEvent->RequestController : nullptr;
	const uint32 RequestEpoch = TargetEvent ? TargetEvent->ConnectionEpoch : 0;
	const AHeroCharacter* SourceHero = ActorInfo ? Cast<AHeroCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	const FString ResultReason = FailureReason.IsEmpty() && !bDeliveryCommitted ? TEXT("Canceled") : FailureReason;
	PendingDeliverySnapshot = nullptr;
	PendingDamageSpec = FGameplayEffectSpecHandle();

	// 原生系统可能仍持有排队请求，完成时自动释放；旧回调不再属于本次激活。
	TargetingRequest.Reset();
	if (RequestController.IsValid())
	{
		RequestController->CompleteTargetAbility(SourceHero, RequestEpoch, bDeliveryCommitted, ResultReason);
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

#if WITH_EDITOR
EDataValidationResult UTargetDamageAbility::IsDataValid(FDataValidationContext& ValidationContext) const
{
	Super::IsDataValid(ValidationContext);
	if (!TargetingPreset || !GetGeometry().IsValid() || !ValidateDamageGE(DamageEffect.GetDefaultObject()) || !DamageType.IsValid() || Delivery > EAbilityDelivery::Projectile || !
		FMath::IsFinite(ProjectileSpeed) || ProjectileSpeed <= 0
		|| !FMath::IsFinite(ProjectileLifetime) || ProjectileLifetime <= 0
		|| (Delivery == EAbilityDelivery::Projectile && TargetingPreset->Geometry.Policy != EAbilityTargetPolicy::Direction && TargetingPreset->Geometry.Policy != EAbilityTargetPolicy::Unit
			&& TargetingPreset->Geometry.Policy != EAbilityTargetPolicy::Point))
	{
		ValidationContext.AddError(NSLOCTEXT("MobaValidation", "TargetDelivery", "目标几何与投送参数必须有效；伤害 GE 必须为固定 Period=0 的 Instant，并符合约定的伤害执行结构。"));
	}

	return ValidationContext.GetNumErrors() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

void UTargetDamageAbility::OnTargetingCompleted(FTargetingRequestHandle RequestHandle)
{
	if (TargetingRequest != RequestHandle || !IsActive())
	{
		return;
	}

	// 复制完成结果；应用 GE 可触发其他能力，不能跨这些调用持有全局 DataStore 的引用。
	const FAbilityTargetingRequestData Request = *UE::TargetingSystem::TTargetingDataStore<FAbilityTargetingRequestData>::Find(RequestHandle);
	const TArray<FTargetingDefaultResultData> TargetResults = FTargetingDefaultResultsSet::Find(RequestHandle)->TargetResults;
	if (!Request.FailureReason.IsEmpty())
	{
		FinishActivation(Request.FailureReason);
		return;
	}

	AActor* SourceActor = FTargetingSourceContext::Find(RequestHandle)->SourceActor.Get();
	UAbilityDeliverySnapshot* DeliverySnapshot = PendingDeliverySnapshot;
	AAbilityProjectile* AbilityProjectile = nullptr;
	FVector ProjectileDirection = Request.Direction.GetSafeNormal2D();
	if (Delivery == EAbilityDelivery::Projectile)
	{
		if (Request.Geometry.Policy == EAbilityTargetPolicy::Unit)
		{
			ProjectileDirection = (Request.Unit->GetActorLocation() - Request.Origin).GetSafeNormal2D();
		}

		if (Request.Geometry.Policy == EAbilityTargetPolicy::Point)
		{
			ProjectileDirection = (Request.Point - Request.Origin).GetSafeNormal2D();
		}

		if (ProjectileDirection.IsNearlyZero())
		{
			FinishActivation(TEXT("Direction"));
			return;
		}

		AbilityProjectile = SourceActor->GetWorld()->SpawnActorDeferred<AAbilityProjectile>(
			AAbilityProjectile::StaticClass(), FTransform(ProjectileDirection.Rotation(), Request.Origin), SourceActor, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!AbilityProjectile)
		{
			FinishActivation(TEXT("ProjectileSpawn"));
			return;
		}
	}

	// 目标和投射物生成预检完成后才冻结来源；Commit 失败丢弃本次 Spec。
	PendingDamageSpec = MobaCombat::PrepareSpec(CurrentActorInfo->AbilitySystemComponent.Get(), DamageEffect, GetAbilityLevel());
	if (!PendingDamageSpec.IsValid())
	{
		if (AbilityProjectile)
		{
			AbilityProjectile->Destroy();
		}

		FinishActivation(TEXT("DamageSpec"));
		return;
	}

	PendingDamageSpec.Data->AddDynamicAssetTag(DamageType);
	FCombatDamageParameters DamageValues;
	const auto* DamageComp = DamageEffect.GetDefaultObject()->FindComponent<UCombatDamageParametersComponent>();
	const auto* MatchRules = SourceActor->GetWorld()->GetSubsystem<UExperienceSubsystem>()->GetRules();
	FGameplayTagContainer DamageTags;
	PendingDamageSpec.Data->GetAllAssetTags(DamageTags);
	ECombatDamageType ResolvedType;
	if (!MatchRules || !MatchRules->CombatRules.ResolveDamageType(DamageTags, ResolvedType) || !DamageComp->Resolve(*PendingDamageSpec.Data, DamageValues))
	{
		if (AbilityProjectile)
		{
			AbilityProjectile->Destroy();
		}

		FinishActivation(TEXT("DamageParameters"));
		return;
	}

	const FGameplayEffectSpecHandle DamageSpec = PendingDamageSpec;
	if (!CommitAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo))
	{
		if (AbilityProjectile)
		{
			AbilityProjectile->Destroy();
		}

		FinishActivation(TEXT("Commit"));
		return;
	}

	bDeliveryCommitted = true;
	if (AbilityProjectile)
	{
		AbilityProjectile->Initialize(DeliverySnapshot, DamageSpec, ProjectileDirection, ProjectileSpeed, ProjectileLifetime, SourceActor);
		AbilityProjectile->FinishSpawning(FTransform(ProjectileDirection.Rotation(), Request.Origin));
	}
	else
	{
		for (const FTargetingDefaultResultData& TargetData : TargetResults)
		{
			AActor* TargetActor = TargetData.HitResult.GetActor();
			FString Reason;
			if (MobaTargeting::ValidateCandidate(SourceActor, TargetActor, Request.Entity, Request.Match, Request.Team, Request.Geometry.AllowedRelations, Request.Origin, Reason))
			{
				FGameplayEffectSpec TargetSpec = MobaCombat::CopyForTarget(*DamageSpec.Data);
				CastChecked<ICombatantInterface>(TargetActor)->GetCombatAbilitySystem()->ApplyGameplayEffectSpecToSelf(TargetSpec);
			}
		}
	}

	FinishActivation(FString());
}

void UTargetDamageAbility::FinishActivation(const FString& Reason)
{
	FailureReason = Reason;
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, !Reason.IsEmpty());
}

FAbilityTargetGeometry UTargetDamageAbility::GetGeometry(const FCombatRules* CombatRules) const
{
	return TargetingPreset->GetGeometry(CombatRules);
}
