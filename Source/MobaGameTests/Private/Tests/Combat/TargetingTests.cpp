#include "Combat/AbilityTargeting.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Combat/CombatGameplayEffects.h"
#include "AbilitySystemComponent.h"
#include "Combat/AbilityProjectile.h"
#include "Combat/TargetDamageAbility.h"
#include "Combat/CombatRules.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "MobaGameplayTags.h"
#include "Match/MatchGameState.h"
#include "Tests/Combat/TargetCombatant.h"
#include <limits>

namespace
{
	class FTargetingWorld
	{

	public:
		FTargetingWorld()
		{
			const auto Values =
				UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(
					false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
			Match = World->SpawnActor<AMatchGameState>();
			World->SetGameState(Match);
			Match->MatchId = FGuid::NewGuid();
			Match->ExperienceInstance = FGuid::NewGuid();
			Match->Teams.Teams = {7, 21, 35};
			FTeamRelationEntry Relation;
			Relation.From = 7;
			Relation.To = 21;
			Relation.Relation = ETeamRelation::Enemy;
			Match->Teams.Relations.Add(Relation);
			Swap(Relation.From, Relation.To);
			Match->Teams.Relations.Add(Relation);
		}

		~FTargetingWorld() { Destroy(); }


	public:
		void EndPlay()
		{
			if (!World || bEnded)
			{
				return;
			}

			World->EndPlay(EEndPlayReason::Quit);
			bEnded = true;
		}

		void Destroy()
		{
			if (!World)
			{
				return;
			}

			EndPlay();
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
			World = nullptr;
		}

		ATargetCombatant* Spawn(const FVector& Location, int32 Team = 21, uint32 Identity = 0)
		{
			FActorSpawnParameters Parameters;
			Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			auto* Actor = World->SpawnActor<ATargetCombatant>(ATargetCombatant::StaticClass(), Location, FRotator::ZeroRotator, Parameters);
			Actor->Entity = Identity ? FGuid(0, 0, 0, Identity) : FGuid::NewGuid();
			Actor->Participant = FGuid::NewGuid();
			Actor->Match = Match->MatchId;
			Actor->Team = Team;
			return Actor;
		}

		AActor* Wall(const FVector& Location)
		{
			auto* Actor = World->SpawnActor<AActor>();
			auto* Box = NewObject<UBoxComponent>(Actor);
			Actor->SetRootComponent(Box);
			Actor->AddInstanceComponent(Box);
			Box->SetBoxExtent(FVector(12, 150, 150));
			Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Box->SetCollisionObjectType(ECC_WorldStatic);
			Box->SetCollisionResponseToAllChannels(ECR_Block);
			Box->RegisterComponent();
			Actor->SetActorLocation(Location);
			return Actor;
		}

		AAbilityProjectile* Fire(ATargetCombatant* Source, float Damage = 25, float Lifetime = 2, FVector Direction = FVector::ForwardVector)
		{
			auto* Snapshot = NewObject<UAbilityDeliverySnapshot>(World);
			Snapshot->Entity = Source->Entity;
			Snapshot->Participant = Source->Participant;
			Snapshot->Match = Source->Match;
			Snapshot->Team = Source->Team;
			Snapshot->Damage = Damage;
			Snapshot->Origin = Source->GetActorLocation();
			Snapshot->Geometry.Policy = EAbilityTargetPolicy::Direction;
			auto* Abilities = Source->GetCombatAbilitySystem();
			auto Context = Abilities->MakeEffectContext();
			Context.AddSourceObject(Snapshot);
			const auto Spec = Abilities->MakeOutgoingSpec(UCombatDamageEffect::StaticClass(), 1, Context);
			const FTransform Transform(Direction.Rotation(), Source->GetActorLocation() + Direction * 50);
			auto* Projectile = World->SpawnActorDeferred<AAbilityProjectile>(AAbilityProjectile::StaticClass(), Transform, Source, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Projectile->Initialize(Snapshot, Spec, Direction, 1200, Lifetime, Source);
			Projectile->FinishSpawning(Transform);
			return Projectile;
		}

		UWorld* World = nullptr;


	public:
		AMatchGameState* Match = nullptr;


	private:
		bool bEnded = false;
	};

	FAbilityTargetIntent MakeIntent(EAbilityTargetPolicy Policy)
	{
		FAbilityTargetIntent Intent;
		Intent.Policy = Policy;
		Intent.Point = FVector(300, 0, 100);
		Intent.Direction = FVector(2, 0, 0);
		return Intent;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetingGeometryTest, "Moba.Targeting.Geometry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTargetingGeometryTest::RunTest(const FString& Parameters)
{
	auto* Ability = NewObject<UTargetDamageAbility>();
	FCombatRules Rules;
	Rules.bAllowNeutralTargets = true;
	TestTrue(TEXT("旧Unit仍继承Experience中立开关"), Ability->GetGeometry(&Rules).AllowedRelations.Contains(ETeamRelation::Neutral));
	Ability->bLegacyUnitRange = false;
	TestFalse(TEXT("新Unit显式Enemy不被旧开关扩大"), Ability->GetGeometry(&Rules).AllowedRelations.Contains(ETeamRelation::Neutral));
	Ability->Targeting.AllowedRelations.Add(ETeamRelation::Neutral);
	TestTrue(TEXT("新Unit显式允许Neutral生效"), Ability->GetGeometry(&Rules).AllowedRelations.Contains(ETeamRelation::Neutral));
	const FVector Origin(0, 0, 100);
	FString Reason;
	for (EAbilityTargetPolicy Policy : {EAbilityTargetPolicy::None, EAbilityTargetPolicy::Self, EAbilityTargetPolicy::Unit, EAbilityTargetPolicy::Point, EAbilityTargetPolicy::Direction,
			 EAbilityTargetPolicy::Area, EAbilityTargetPolicy::Line, EAbilityTargetPolicy::Cone})
	{
		FAbilityTargetGeometry Geometry;
		Geometry.Policy = Policy;
		auto Intent = MakeIntent(Policy);
		const FString Label = FString::Printf(TEXT("Policy=%d "), int32(Policy));
		TestTrue(Label + TEXT("合法载荷"), MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason));
		Intent.Point.X = std::numeric_limits<double>::quiet_NaN();
		TestFalse(Label + TEXT("NaN坐标拒绝"), MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason));
		Intent = MakeIntent(Policy);
		Intent.Direction.Y = std::numeric_limits<double>::infinity();
		TestFalse(Label + TEXT("Inf方向拒绝"), MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason));
		Intent = MakeIntent(Policy);
		TestFalse(Label + TEXT("非有限来源拒绝"), MobaTargeting::ValidateIntent(FVector(0, 0, std::numeric_limits<double>::infinity()), Geometry, Intent, Reason));
		TestFalse(Label + TEXT("非有限目标不包含"), MobaTargeting::Contains(Origin, FVector(std::numeric_limits<double>::quiet_NaN(), 0, 100), Geometry, Intent));
		Geometry.Range = std::numeric_limits<float>::infinity();
		TestFalse(Label + TEXT("非有限配置拒绝"), Geometry.IsValid());
	}

	FAbilityTargetGeometry Geometry;
	Geometry.Policy = EAbilityTargetPolicy::Unit;
	auto Intent = MakeIntent(Geometry.Policy);
	TestTrue(TEXT("单位XY距离含900边界"), MobaTargeting::Contains(Origin, FVector(900, 0, 100), Geometry, Intent));
	TestTrue(TEXT("高度200边界且非三维球距离"), MobaTargeting::Contains(Origin, FVector(900, 0, 300), Geometry, Intent));
	TestFalse(TEXT("单位超过XY范围"), MobaTargeting::Contains(Origin, FVector(900.01, 0, 100), Geometry, Intent));
	TestFalse(TEXT("单位超过高度容差"), MobaTargeting::Contains(Origin, FVector(300, 0, 300.01), Geometry, Intent));
	for (EAbilityTargetPolicy Policy : {EAbilityTargetPolicy::Point, EAbilityTargetPolicy::Area})
	{
		Geometry.Policy = Policy;
		Intent = MakeIntent(Policy);
		Intent.Point = FVector(900, 0, 300);
		TestTrue(TEXT("点中心距离和高度边界包含"), MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason));
		TestTrue(TEXT("圆半径边界包含"), MobaTargeting::Contains(Origin, Intent.Point + FVector(150, 0, 0), Geometry, Intent));
		TestFalse(TEXT("圆半径外拒绝"), MobaTargeting::Contains(Origin, Intent.Point + FVector(150.01, 0, 0), Geometry, Intent));
		TestFalse(TEXT("相对点中心的高度越界"), MobaTargeting::Contains(Origin, Intent.Point + FVector(0, 0, 200.01), Geometry, Intent));
		Intent.Point.X += 0.01;
		TestFalse(TEXT("远点拒绝而非钳制"), MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason));
		Intent.Point = FVector(500, 0, 300.01);
		TestFalse(TEXT("点中心高度越界拒绝"), MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason));
	}

	for (EAbilityTargetPolicy Policy : {EAbilityTargetPolicy::Direction, EAbilityTargetPolicy::Line, EAbilityTargetPolicy::Cone})
	{
		Geometry.Policy = Policy;
		Intent = MakeIntent(Policy);
		Intent.Direction = FVector::ZeroVector;
		TestFalse(TEXT("零方向拒绝"), MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason));
		Intent.Direction = FVector(1, 0, 1);
		TestFalse(TEXT("非地面方向拒绝"), MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason));
		Intent.Direction = FVector(20, 0, 0);
		TestTrue(TEXT("方向归一化后包含前方"), MobaTargeting::Contains(Origin, FVector(400, 0, 100), Geometry, Intent));
		TestFalse(TEXT("方向后方排除"), MobaTargeting::Contains(Origin, FVector(-1, 0, 100), Geometry, Intent));
		Intent.Point = FVector(-5000, -5000, 100);
		TestTrue(TEXT("客户端Point不改变方向类服务器起点"), MobaTargeting::Contains(Origin, FVector(400, 0, 100), Geometry, Intent));
	}

	Geometry.Policy = EAbilityTargetPolicy::Line;
	Intent = MakeIntent(Geometry.Policy);
	TestTrue(TEXT("线端点半宽角包含"), MobaTargeting::Contains(Origin, FVector(900, 75, 100), Geometry, Intent));
	TestTrue(TEXT("线半宽边界包含"), MobaTargeting::Contains(Origin, FVector(400, 75, 100), Geometry, Intent));
	TestFalse(TEXT("线半宽越界拒绝"), MobaTargeting::Contains(Origin, FVector(400, 75.01, 100), Geometry, Intent));
	Geometry.Policy = EAbilityTargetPolicy::Cone;
	Geometry.HalfAngleDegrees = 45;
	Intent = MakeIntent(Geometry.Policy);
	TestTrue(TEXT("锥角边界包含"), MobaTargeting::Contains(Origin, FVector(100, 100, 100), Geometry, Intent));
	TestFalse(TEXT("锥角外拒绝"), MobaTargeting::Contains(Origin, FVector(100, 101, 100), Geometry, Intent));
	Intent.Policy = EAbilityTargetPolicy::Unit;
	TestFalse(TEXT("客户端policy与配置不符拒绝"), MobaTargeting::ValidateIntent(Origin, Geometry, Intent, Reason));
	Geometry.Policy = static_cast<EAbilityTargetPolicy>(255);
	TestFalse(TEXT("未知policy拒绝"), Geometry.IsValid());
	Geometry = FAbilityTargetGeometry();
	Geometry.AllowedRelations = {ETeamRelation::Undefined};
	TestFalse(TEXT("Undefined不能配置成允许关系"), Geometry.IsValid());
	Geometry = FAbilityTargetGeometry();
	Geometry.MaxTargets = 17;
	TestFalse(TEXT("上限配置不可超过16"), Geometry.IsValid());
	Geometry.MaxTargets = 0;
	TestFalse(TEXT("零上限非法"), Geometry.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetingWorldTest, "Moba.Targeting.WorldPolicies", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTargetingWorldTest::RunTest(const FString& Parameters)
{
	FTargetingWorld Fixture;
	auto* Source = Fixture.Spawn(FVector(0, 0, 100), 7);
	auto* Target = Fixture.Spawn(FVector(300, 0, 100));
	TArray<AActor*> Targets;
	FString Reason;
	for (EAbilityTargetPolicy Policy : {EAbilityTargetPolicy::None, EAbilityTargetPolicy::Self, EAbilityTargetPolicy::Unit, EAbilityTargetPolicy::Point, EAbilityTargetPolicy::Direction,
			 EAbilityTargetPolicy::Area, EAbilityTargetPolicy::Line, EAbilityTargetPolicy::Cone})
	{
		FAbilityTargetGeometry Geometry;
		Geometry.Policy = Policy;
		if (Policy == EAbilityTargetPolicy::Self)
		{
			Geometry.AllowedRelations = {ETeamRelation::Ally};
		}

		auto Intent = MakeIntent(Policy);
		Intent.Unit = Target;
		const FString Label = FString::Printf(TEXT("非Hero实体Policy=%d "), int32(Policy));
		TestTrue(Label + TEXT("真实Resolve成功"), MobaTargeting::Resolve(Source, Geometry, Intent, Targets, Reason));
		TestEqual(Label + TEXT("实际目标数量"), Targets.Num(), Policy == EAbilityTargetPolicy::None ? 0 : 1);
		if (Targets.Num() == 1)
		{
			TestTrue(Label + TEXT("实际目标身份"), Targets[0] == (Policy == EAbilityTargetPolicy::Self ? Source : Target));
		}
	}

	FAbilityTargetGeometry Geometry;
	auto Intent = MakeIntent(EAbilityTargetPolicy::Unit);
	Intent.Unit = Target;
	const auto Rejected = [this, &Source, &Geometry, &Intent, &Targets, &Reason](const TCHAR* Label, const TCHAR* Expected)
	{
		TestFalse(Label, MobaTargeting::Resolve(Source, Geometry, Intent, Targets, Reason));
		TestEqual(FString(Label) + TEXT("原因"), Reason, FString(Expected));
		TestEqual(FString(Label) + TEXT("无残余结果"), Targets.Num(), 0);
	};
	Intent.Unit = Source;
	Rejected(TEXT("Unit不得自选"), TEXT("Target"));
	Intent.Unit = nullptr;
	Rejected(TEXT("Unit缺目标"), TEXT("Target"));
	Intent.Unit = Fixture.World->SpawnActor<AActor>();
	Rejected(TEXT("非Combatant拒绝"), TEXT("Target"));
	Intent.Unit = Target;
	Target->Entity.Invalidate();
	Rejected(TEXT("无Entity拒绝"), TEXT("Target"));
	Target->Entity = FGuid::NewGuid();
	Target->Match = FGuid::NewGuid();
	Rejected(TEXT("跨Match拒绝"), TEXT("Match"));
	Target->Match = Fixture.Match->MatchId;
	Target->bAlive = false;
	Rejected(TEXT("死亡目标拒绝"), TEXT("LifeState"));
	Target->bAlive = true;
	Source->bAlive = false;
	Rejected(TEXT("死亡来源不能新释放"), TEXT("Source"));
	Source->bAlive = true;
	Target->Team = 7;
	Rejected(TEXT("敌对策略拒绝友方"), TEXT("Team"));
	Target->Team = 999;
	Rejected(TEXT("未知队伍拒绝"), TEXT("Team"));
	Target->Team = 35;
	Rejected(TEXT("未定义跨队关系拒绝"), TEXT("Team"));
	Target->Team = INDEX_NONE;
	Rejected(TEXT("中立未允许拒绝"), TEXT("Team"));
	Geometry.AllowedRelations.Add(ETeamRelation::Neutral);
	TestTrue(TEXT("显式允许中立成功"), MobaTargeting::Resolve(Source, Geometry, Intent, Targets, Reason));
	Target->Team = 21;
	Target->GetCombatAbilitySystem()->AddLooseGameplayTag(MobaGameplayTags::State_Untargetable);
	Rejected(TEXT("不可选中拒绝"), TEXT("Untargetable"));
	Target->GetCombatAbilitySystem()->RemoveLooseGameplayTag(MobaGameplayTags::State_Untargetable);
	Target->GetCombatAbilitySystem()->AddLooseGameplayTag(MobaGameplayTags::State_Invulnerable);
	Rejected(TEXT("免伤拒绝"), TEXT("Invulnerable"));
	Target->GetCombatAbilitySystem()->RemoveLooseGameplayTag(MobaGameplayTags::State_Invulnerable);
	Target->SetActorLocation(FVector(900.1, 0, 100));
	Rejected(TEXT("超距离拒绝"), TEXT("Range"));
	Target->SetActorLocation(FVector(300, 0, 300.1));
	Rejected(TEXT("超高度拒绝"), TEXT("Range"));
	Target->SetActorLocation(FVector(300, 0, 100));
	auto* Wall = Fixture.Wall(FVector(150, 0, 100));
	TestTrue(TEXT("真实Visibility阻挡已注册"), Fixture.World->LineTraceTestByChannel(Source->GetActorLocation(), Target->GetActorLocation(), ECC_Visibility));
	Rejected(TEXT("真实墙体视线拒绝"), TEXT("LineOfSight"));
	Wall->Destroy();
	TestTrue(TEXT("移除墙恢复目标合法"), MobaTargeting::Resolve(Source, Geometry, Intent, Targets, Reason));
	FTargetingWorld OtherWorld;
	Intent.Unit = OtherWorld.Spawn(FVector(300, 0, 100));
	Cast<ATargetCombatant>(Intent.Unit)->Match = Fixture.Match->MatchId;
	Rejected(TEXT("跨World拒绝"), TEXT("Match"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetingOrderTest, "Moba.Targeting.StableOrderAndLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTargetingOrderTest::RunTest(const FString& Parameters)
{
	FTargetingWorld Fixture;
	auto* Source = Fixture.Spawn(FVector(0, 0, 100), 7);
	TArray<ATargetCombatant*> Expected;
	// 反向生成且同距成对，避免Actor迭代顺序碰巧满足距离/身份排序。
	for (uint32 Identity = 20; Identity > 0; --Identity)
	{
		const double Distance = 100 + ((Identity - 1) / 2) * 50;
		Expected.Add(Fixture.Spawn(FVector(Distance, 0, 100), 21, Identity));
	}

	Expected.Sort([](const ATargetCombatant& A, const ATargetCombatant& B) { return A.Entity < B.Entity; });
	Fixture.Spawn(FVector(10, 0, 100), 7, 30);
	auto* Dead = Fixture.Spawn(FVector(15, 0, 100), 21, 31);
	Dead->bAlive = false;
	FAbilityTargetGeometry Geometry;
	Geometry.Policy = EAbilityTargetPolicy::Line;
	auto Intent = MakeIntent(Geometry.Policy);
	TArray<AActor*> Targets;
	FString Reason;
	TestTrue(TEXT("服务器搜集与排序成功"), MobaTargeting::Resolve(Source, Geometry, Intent, Targets, Reason));
	TestEqual(TEXT("合法候选截取16"), Targets.Num(), 16);
	for (int32 Index = 0; Index < Targets.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("第%d项为距离再EntityId排序"), Index), Targets[Index] == Expected[Index]);
	}

	Geometry.MaxTargets = 3;
	TestTrue(TEXT("显式更小上限"), MobaTargeting::Resolve(Source, Geometry, Intent, Targets, Reason));
	TestEqual(TEXT("按配置截3"), Targets.Num(), 3);
	Geometry.Policy = EAbilityTargetPolicy::Area;
	Geometry.Radius = 900;
	Geometry.MaxTargets = 16;
	Intent = MakeIntent(Geometry.Policy);
	Intent.Point = FVector(900, 0, 100);
	Intent.Unit = Dead;
	TestTrue(TEXT("Area服务器重搜不相信无关Unit字段"), MobaTargeting::Resolve(Source, Geometry, Intent, Targets, Reason));
	TestEqual(TEXT("Area合法候选仍截16"), Targets.Num(), 16);
	if (Targets.Num() > 0)
	{
		TestTrue(TEXT("Area按点中心距离不是施法者距离排序"), Targets[0] == Expected[18]);
		TestTrue(TEXT("Area等距EntityId稳定次序"), Targets.Num() > 1 && Targets[1] == Expected[19]);
	}

	return true;
}

namespace
{
	enum class EProjectileCase : uint8
	{
		FirstHit,
		BlockingCombatant,
		BlockingUntargetable,
		DeferredDirection,
		TargetDies,
		TargetUntargetable,
		Wall,
		Expiry,
		OutOfRange,
		SourceDies,
		SourceDestroyed,
		MatchExit,
		WorldExit,
		Complete
	};

	class FProjectileScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FProjectileScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds()) {}


	public:
		bool Update() override
		{
			if (FPlatformTime::Seconds() - Started > 40)
			{
				Test->AddError(FString::Printf(TEXT("Projectile场景超时 Case=%d Frames=%d"), int32(Case), Frames));
				return true;
			}

			if (Case == EProjectileCase::Complete)
			{
				return true;
			}

			if (!Fixture)
			{
				Setup();
				return Test->HasAnyErrors();
			}

			// 每个引擎帧只推进一次，真实Movement sweep、Physics和Timer共同执行。
			Fixture->World->Tick(LEVELTICK_All, 0.05f);
			++Frames;
			if (Projectile.IsValid() && Projectile->GetActorLocation().SizeSquared2D() > 2500)
			{
				bSawMovement = true;
			}

			if (Frames == 1)
			{
				if (Case == EProjectileCase::TargetDies)
				{
					Target->bAlive = false;
				}

				if (Case == EProjectileCase::TargetUntargetable)
				{
					Target->GetCombatAbilitySystem()->AddLooseGameplayTag(MobaGameplayTags::State_Untargetable);
				}

				if (Case == EProjectileCase::SourceDies)
				{
					Source->bAlive = false;
				}

				if (Case == EProjectileCase::SourceDestroyed)
				{
					Test->TestTrue(TEXT("发射后源Actor真正Destroy"), Source->Destroy());
					Source = nullptr;
				}

				if (Case == EProjectileCase::MatchExit)
				{
					Fixture->Match->bMatchEnded = true;
				}

				if (Case == EProjectileCase::WorldExit)
				{
					Test->TestTrue(TEXT("World退出前仍有在飞投射物"), Projectile.IsValid());
					Fixture->EndPlay();
					Test->TestTrue(TEXT("普通实体收到EndPlay"), Target->bEnded);
					if (Projectile.IsValid())
					{
						Test->TestFalse(TEXT("投射物不再处于BeginPlay状态"), Projectile->HasActorBegunPlay());
						Test->TestEqual(TEXT("投射物原生寿命Timer已清理"), Projectile->GetLifeSpan(), 0.0f);
					}

					Finish();
					return false;
				}
			}

			if (Projectile.IsValid())
			{
				if (Frames > 60)
				{
					Test->AddError(FString::Printf(TEXT("投射物超过生命周期未销毁 Case=%d"), int32(Case)));
					return true;
				}

				return false;
			}

			Test->TestTrue(TEXT("真实Movement至少跨帧前进"), bSawMovement);
			const FString Label = FString::Printf(TEXT("Projectile Case=%d "), int32(Case));
			if (Case == EProjectileCase::FirstHit || Case == EProjectileCase::BlockingCombatant || Case == EProjectileCase::DeferredDirection)
			{
				Test->TestEqual(Label + TEXT("重复碰撞组件仅扣一次25"), Target->GetHealth(), 75.0f);
				Test->TestEqual(Label + TEXT("首击后不穿透"), Beyond->GetHealth(), 100.0f);
			}
			else if (Case == EProjectileCase::TargetDies || Case == EProjectileCase::TargetUntargetable || Case == EProjectileCase::BlockingUntargetable)
			{
				Test->TestEqual(Label + TEXT("途中失效目标不扣血"), Target->GetHealth(), 100.0f);
				Test->TestEqual(Label + TEXT("继续飞行命中首个合法目标"), Beyond->GetHealth(), 75.0f);
			}
			else if (Case == EProjectileCase::Wall || Case == EProjectileCase::Expiry || Case == EProjectileCase::OutOfRange || Case == EProjectileCase::MatchExit)
			{
				Test->TestEqual(Label + TEXT("销毁没有伤害"), Target->GetHealth(), 100.0f);
				Test->TestEqual(Label + TEXT("远处目标不绕过900范围"), Beyond->GetHealth(), 100.0f);
				if (Case == EProjectileCase::Expiry)
				{
					Test->TestTrue(TEXT("2秒寿命确实走过而非立即销毁"), Fixture->World->GetTimeSeconds() >= 2.0f);
				}
			}
			else
			{
				Test->TestEqual(Label + TEXT("来源消失仍由快照实际GE致死"), Target->GetHealth(), 0.0f);
				Test->TestEqual(Label + TEXT("死亡只提交一次"), Target->DeathCount, 1);
				Test->TestEqual(Label + TEXT("原Participant归属"), Target->KillerParticipant, Participant);
				Test->TestEqual(Label + TEXT("原Entity归属"), Target->KillerEntity, Entity);
			}

			int32 Remaining = 0;
			for (TActorIterator<AAbilityProjectile> It(Fixture->World); It; ++It)
			{
				++Remaining;
			}

			Test->TestEqual(Label + TEXT("World无残留投射物"), Remaining, 0);
			Finish();
			return false;
		}


	private:
		void Setup()
		{
			Fixture = MakeUnique<FTargetingWorld>();
			Source = Fixture->Spawn(FVector(0, 0, 100), 7);
			Target = Fixture->Spawn(FVector(400, 0, 100));
			Beyond = Fixture->Spawn(FVector(700, 0, 100));
			if (Case == EProjectileCase::BlockingCombatant || Case == EProjectileCase::BlockingUntargetable)
			{
				Target->UseBlockingWorldDynamicCollision();
				Beyond->UseBlockingWorldDynamicCollision();
				if (Case == EProjectileCase::BlockingUntargetable)
				{
					Target->GetCombatAbilitySystem()->AddLooseGameplayTag(MobaGameplayTags::State_Untargetable);
				}
			}

			Participant = Source->Participant;
			Entity = Source->Entity;
			if (Case == EProjectileCase::Wall)
			{
				Fixture->Wall(FVector(230, 0, 100));
			}

			if (Case == EProjectileCase::Expiry || Case == EProjectileCase::WorldExit)
			{
				Target->SetActorLocation(FVector(400, 1000, 100));
				Beyond->SetActorLocation(FVector(700, 1000, 100));
			}

			if (Case == EProjectileCase::SourceDies || Case == EProjectileCase::SourceDestroyed)
			{
				Target->SetHealth(20);
			}

			if (Case == EProjectileCase::DeferredDirection)
			{
				Target->SetActorLocation(FVector(0, 400, 100));
				Beyond->SetActorLocation(FVector(0, 700, 100));
			}

			if (Case == EProjectileCase::OutOfRange)
			{
				Target->SetActorLocation(FVector(1400, 0, 100));
				Beyond->SetActorLocation(FVector(1700, 0, 100));
			}

			Projectile = Fixture->Fire(Source, 25, 2, Case == EProjectileCase::DeferredDirection ? FVector::RightVector : FVector::ForwardVector);
			Test->TestTrue(TEXT("权威生成真实投射物"), Projectile.IsValid());
			if (Projectile.IsValid())
			{
				Test->TestEqual(TEXT("投射物携带原参赛者"), Projectile->SourceParticipant, Participant);
				Test->TestEqual(TEXT("投射物携带原实体"), Projectile->SourceEntity, Entity);
			}

			Frames = 0;
			bSawMovement = false;
		}

		void Finish()
		{
			Fixture.Reset();
			Source = nullptr;
			Target = nullptr;
			Beyond = nullptr;
			Projectile.Reset();
			Case = static_cast<EProjectileCase>(int32(Case) + 1);
		}


	private:
		FAutomationTestBase* Test;

		double Started;

		EProjectileCase Case = EProjectileCase::FirstHit;

		TUniquePtr<FTargetingWorld> Fixture;

		ATargetCombatant* Source = nullptr;

		ATargetCombatant* Target = nullptr;

		ATargetCombatant* Beyond = nullptr;

		TWeakObjectPtr<AAbilityProjectile> Projectile;

		FGuid Participant;

		FGuid Entity;

		int32 Frames = 0;

		bool bSawMovement = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetingProjectileTest, "Moba.Targeting.ProjectileLifetime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTargetingProjectileTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FProjectileScenario>(this));
	return true;
}

#endif
