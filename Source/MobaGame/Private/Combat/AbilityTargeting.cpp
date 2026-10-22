#include "Combat/AbilityTargeting.h"

#include "Combat/CombatantInterface.h"
#include "AbilitySystemComponent.h"
#include "Match/MatchGameState.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "MobaGameplayTags.h"

namespace
{
	bool Finite(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}
}

bool FAbilityTargetGeometry::IsValid() const
{
	if (Policy > EAbilityTargetPolicy::Cone || RangeMetric > EAbilityTargetRange::Spatial || !FMath::IsFinite(Range) || Range <= 0 || !FMath::IsFinite(HeightTolerance) || HeightTolerance < 0 || !FMath::IsFinite(Radius) || Radius <= 0
		|| !FMath::IsFinite(HalfWidth) || HalfWidth <= 0 || !FMath::IsFinite(HalfAngleDegrees) || HalfAngleDegrees <= 0 || HalfAngleDegrees > 90 || MaxTargets < 1 || MaxTargets > 16)
	{
		return false;
	}

	for (ETeamRelation Relation : AllowedRelations)
	{
		if (Relation == ETeamRelation::Undefined || Relation > ETeamRelation::Enemy)
		{
			return false;
		}
	}

	return !AllowedRelations.IsEmpty();
}

bool MobaTargeting::ValidateIntent(const FVector& Origin, const FAbilityTargetGeometry& Geometry, const FAbilityTargetIntent& Intent, FString& Reason)
{
	if (!Geometry.IsValid() || Intent.Policy != Geometry.Policy || !Finite(Origin) || !Finite(Intent.Point) || !Finite(Intent.Direction))
	{
		Reason = TEXT("TargetPayload");
		return false;
	}

	if (Intent.Policy == EAbilityTargetPolicy::Point || Intent.Policy == EAbilityTargetPolicy::Area)
	{
		if (FVector::DistSquaredXY(Origin, Intent.Point) > FMath::Square(Geometry.Range) || FMath::Abs(Origin.Z - Intent.Point.Z) > Geometry.HeightTolerance)
		{
			Reason = TEXT("PointRange");
			return false;
		}
	}

	if (Intent.Policy == EAbilityTargetPolicy::Direction || Intent.Policy == EAbilityTargetPolicy::Line || Intent.Policy == EAbilityTargetPolicy::Cone)
	{
		if (!FMath::IsFinite(Intent.Direction.SizeSquared2D()) || Intent.Direction.SizeSquared2D() < UE_SMALL_NUMBER || FMath::Abs(Intent.Direction.Z) > UE_KINDA_SMALL_NUMBER)
		{
			Reason = TEXT("Direction");
			return false;
		}
	}

	return true;
}

bool MobaTargeting::Contains(const FVector& Origin, const FVector& TargetPosition, const FAbilityTargetGeometry& Geometry, const FAbilityTargetIntent& Intent)
{
	FString Reason;
	if (!Finite(TargetPosition) || !ValidateIntent(Origin, Geometry, Intent, Reason))
	{
		return false;
	}

	if (Geometry.RangeMetric == EAbilityTargetRange::Spatial && Intent.Policy == EAbilityTargetPolicy::Unit)
	{
		return FVector::DistSquared(Origin, TargetPosition) <= FMath::Square(Geometry.Range);
	}

	const bool bUseTargetPointAsCenter = Intent.Policy == EAbilityTargetPolicy::Area || Intent.Policy == EAbilityTargetPolicy::Point;
	const FVector Center = bUseTargetPointAsCenter ? Intent.Point : Origin;
	if (FMath::Abs(Center.Z - TargetPosition.Z) > Geometry.HeightTolerance)
	{
		return false;
	}

	const FVector Delta = (TargetPosition - Center) * FVector(1, 1, 0);
	if (bUseTargetPointAsCenter)
	{
		return Delta.SizeSquared() <= FMath::Square(Geometry.Radius);
	}

	if (Intent.Policy != EAbilityTargetPolicy::Line && Intent.Policy != EAbilityTargetPolicy::Direction && Delta.SizeSquared() > FMath::Square(Geometry.Range))
	{
		return false;
	}

	const FVector Direction = Intent.Direction.GetSafeNormal2D();
	const double DistanceAlongDirection = FVector::DotProduct(Delta, Direction);
	switch (Intent.Policy)
	{
	case EAbilityTargetPolicy::None:
		return false;
	case EAbilityTargetPolicy::Self:
		return Delta.IsNearlyZero();
	case EAbilityTargetPolicy::Unit:
		return true;
	case EAbilityTargetPolicy::Direction:
	case EAbilityTargetPolicy::Line:
		return DistanceAlongDirection >= 0 && DistanceAlongDirection <= Geometry.Range && FMath::Abs(Delta.X * Direction.Y - Delta.Y * Direction.X) <= Geometry.HalfWidth;
	case EAbilityTargetPolicy::Cone:
		return Delta.IsNearlyZero() || DistanceAlongDirection + UE_KINDA_SMALL_NUMBER >= Delta.Size() * FMath::Cos(FMath::DegreesToRadians(Geometry.HalfAngleDegrees));
	default:
		return false;
	}
}

bool MobaTargeting::ValidateCandidate(const AActor* SourceActor, const AActor* TargetActor, FGuid SourceEntityId, FGuid MatchId, int32 SourceTeamId, const TArray<ETeamRelation>& AllowedRelations, const FVector& SightOrigin, FString& Reason)
{
	const ICombatantInterface* TargetCombatant = Cast<ICombatantInterface>(TargetActor);
	if (!IsValid(TargetActor) || !TargetCombatant || !TargetCombatant->GetCombatEntity().IsValid() || !SourceEntityId.IsValid() || !Finite(TargetActor->GetActorLocation()) || !Finite(SightOrigin))
	{
		Reason = TEXT("Target");
		return false;
	}

	const UWorld* World = TargetActor->GetWorld();
	const AMatchGameState* MatchGameState = World ? World->GetGameState<AMatchGameState>() : nullptr;
	if (!MatchGameState || MatchGameState->bMatchEnded || !MatchId.IsValid() || MatchId != MatchGameState->MatchId || TargetCombatant->GetCombatMatch() != MatchId
		|| (SourceActor && SourceActor->GetWorld() != World))
	{
		Reason = TEXT("Match");
		return false;
	}

	const UAbilitySystemComponent* TargetASC = TargetCombatant->GetCombatAbilitySystem();
	if (!TargetCombatant->IsCombatAlive() || !TargetASC)
	{
		Reason = TEXT("LifeState");
		return false;
	}

	if (TargetASC->HasMatchingGameplayTag(MobaGameplayTags::State_Untargetable))
	{
		Reason = TEXT("Untargetable");
		return false;
	}

	if (TargetASC->HasMatchingGameplayTag(MobaGameplayTags::State_Invulnerable))
	{
		Reason = TEXT("Invulnerable");
		return false;
	}

	const ETeamRelation Relation = MatchGameState->Teams.Query(SourceTeamId, TargetCombatant->GetCombatTeam());
	if (Relation == ETeamRelation::Undefined || !AllowedRelations.Contains(Relation))
	{
		Reason = TEXT("Team");
		return false;
	}

	FCollisionQueryParams SightQueryParams(SCENE_QUERY_STAT(AbilityTargetSight), false, SourceActor);
	SightQueryParams.AddIgnoredActor(TargetActor);
	if (World->LineTraceTestByChannel(SightOrigin, TargetActor->GetActorLocation(), ECC_Visibility, SightQueryParams))
	{
		Reason = TEXT("LineOfSight");
		return false;
	}

	return true;
}

void MobaTargeting::DrawIndicator(UWorld* World, const FVector& Origin, const FAbilityTargetGeometry& Geometry, const FAbilityTargetIntent& Intent)
{
	if (!World)
	{
		return;
	}

	FString Reason;
	const FColor Color = ValidateIntent(Origin, Geometry, Intent, Reason) ? FColor::Cyan : FColor::Red;
	const FVector Lift(0, 0, 4);
	DrawDebugCircle(World, Origin + Lift, Geometry.Range, 64, Color, false, 0, 0, 2, FVector::ForwardVector, FVector::RightVector, false);
	if (Intent.Policy == EAbilityTargetPolicy::Point || Intent.Policy == EAbilityTargetPolicy::Area)
	{
		DrawDebugCircle(World, Intent.Point + Lift, Geometry.Radius, 48, Color, false, 0, 0, 3, FVector::ForwardVector, FVector::RightVector, false);
	}
	else
	{
		DrawDebugDirectionalArrow(World, Origin + Lift, Origin + Lift + Intent.Direction.GetSafeNormal2D() * Geometry.Range, 50, Color, false, 0, 0, 4);
	}
}

