#include "Combat/AbilityTargetingTasks.h"

#include "Combat/AbilityTargetingRequest.h"
#include "Combat/CombatantInterface.h"
#include "EngineUtils.h"

DEFINE_TARGETING_DATA_STORE(FAbilityTargetingRequestData)

void UAbilityTargetSelectionTask::Execute(const FTargetingRequestHandle& TargetingHandle) const
{
	Super::Execute(TargetingHandle);
	FAbilityTargetingRequestData& Request = UE::TargetingSystem::TTargetingDataStore<FAbilityTargetingRequestData>::FindOrAdd(TargetingHandle);
	FTargetingDefaultResultsSet& Results = FTargetingDefaultResultsSet::FindOrAdd(TargetingHandle);
	Results.TargetResults.Reset();
	AActor* SourceActor = FTargetingSourceContext::Find(TargetingHandle)->SourceActor.Get();
	const ICombatantInterface* SourceCombatant = Cast<ICombatantInterface>(SourceActor);
	if (!SourceCombatant || !SourceCombatant->IsCombatAlive())
	{
		Request.FailureReason = TEXT("Source");
		return;
	}

	FAbilityTargetIntent TargetIntent;
	TargetIntent.Policy = Request.Geometry.Policy;
	TargetIntent.Unit = Request.Unit.Get();
	TargetIntent.Point = Request.Point;
	TargetIntent.Direction = Request.Direction;
	const auto AddCandidate = [&Results](AActor* TargetActor)
	{
		FTargetingDefaultResultData& TargetData = Results.TargetResults.AddDefaulted_GetRef();
		TargetData.HitResult.HitObjectHandle = FActorInstanceHandle(TargetActor);
		TargetData.HitResult.Location = TargetActor->GetActorLocation();
	};
	if (TargetIntent.Policy == EAbilityTargetPolicy::None)
	{
		return;
	}

	if (TargetIntent.Policy == EAbilityTargetPolicy::Self || TargetIntent.Policy == EAbilityTargetPolicy::Unit)
	{
		AActor* TargetActor = TargetIntent.Policy == EAbilityTargetPolicy::Self ? SourceActor : Request.Unit.Get();
		if (!IsValid(TargetActor) || (TargetIntent.Policy == EAbilityTargetPolicy::Unit && TargetActor == SourceActor))
		{
			Request.FailureReason = TEXT("Target");
			return;
		}

		if (!MobaTargeting::Contains(Request.Origin, TargetActor->GetActorLocation(), Request.Geometry, TargetIntent))
		{
			Request.FailureReason = TEXT("Range");
			return;
		}

		AddCandidate(TargetActor);
		return;
	}

	// 地点/方向投射物尚未命中，释放时只验证意图，不预查未来碰撞目标。
	if (Request.Delivery == EAbilityDelivery::Projectile)
	{
		return;
	}

	for (TActorIterator<AActor> ActorIt(SourceActor->GetWorld()); ActorIt; ++ActorIt)
	{
		if (*ActorIt != SourceActor && MobaTargeting::Contains(Request.Origin, ActorIt->GetActorLocation(), Request.Geometry, TargetIntent))
		{
			AddCandidate(*ActorIt);
		}
	}
}

bool UCombatTargetFilterTask::ShouldFilterTarget(const FTargetingRequestHandle& TargetingHandle, const FTargetingDefaultResultData& TargetData) const
{
	FAbilityTargetingRequestData& Request = UE::TargetingSystem::TTargetingDataStore<FAbilityTargetingRequestData>::FindOrAdd(TargetingHandle);
	AActor* SourceActor = FTargetingSourceContext::Find(TargetingHandle)->SourceActor.Get();
	FString Reason;
	const bool bAccepted = MobaTargeting::ValidateCandidate(SourceActor, TargetData.HitResult.GetActor(), Request.Entity, Request.Match,
		Request.Team, Request.Geometry.AllowedRelations, Request.Origin, Reason);
	if (!bAccepted && (Request.Geometry.Policy == EAbilityTargetPolicy::Unit || Request.Geometry.Policy == EAbilityTargetPolicy::Self))
	{
		Request.FailureReason = Reason;
	}

	return !bAccepted;
}

void UAbilityTargetSortTask::Execute(const FTargetingRequestHandle& TargetingHandle) const
{
	Super::Execute(TargetingHandle);
	const FAbilityTargetingRequestData& Request = UE::TargetingSystem::TTargetingDataStore<FAbilityTargetingRequestData>::FindOrAdd(TargetingHandle);
	FTargetingDefaultResultsSet& Results = FTargetingDefaultResultsSet::FindOrAdd(TargetingHandle);
	const FVector Center = (Request.Geometry.Policy == EAbilityTargetPolicy::Point || Request.Geometry.Policy == EAbilityTargetPolicy::Area) ? Request.Point : Request.Origin;
	Results.TargetResults.Sort([Center](const FTargetingDefaultResultData& Left, const FTargetingDefaultResultData& Right)
	{
		const AActor* LeftActor = Left.HitResult.GetActor();
		const AActor* RightActor = Right.HitResult.GetActor();
		const double LeftDistanceSquared = FVector::DistSquaredXY(Center, LeftActor->GetActorLocation());
		const double RightDistanceSquared = FVector::DistSquaredXY(Center, RightActor->GetActorLocation());
		return LeftDistanceSquared == RightDistanceSquared ? CastChecked<ICombatantInterface>(LeftActor)->GetCombatEntity() < CastChecked<ICombatantInterface>(RightActor)->GetCombatEntity() : LeftDistanceSquared < RightDistanceSquared;
	});
	if (Results.TargetResults.Num() > Request.Geometry.MaxTargets)
	{
		Results.TargetResults.SetNum(Request.Geometry.MaxTargets);
	}
}

