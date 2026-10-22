#include "Combat/CombatRules.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include <limits>
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Hero/HeroCharacter.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/CombatGameplayEffects.h"
#include "AbilitySystem/SelfCommitAbility.h"
#include "Hero/HeroDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Match/ParticipantState.h"
#include "Match/MatchGameState.h"
#include "Player/HeroPlayerController.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MobaGameplayTags.h"
#include "MobaMessageTags.h"
#include "Tests/Combat/CombatCancellationTestAbility.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRulesTest, "Moba.Combat.Rules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCombatRulesTest::RunTest(const FString& Parameters)
{
	FCombatRules Rules;
	float Damage = 123;
	TestFalse(TEXT("空规则无默认伤害"), Rules.ResolveDamage(MobaGameplayTags::Damage_True, Damage));
	TestEqual(TEXT("失败输出归零"), Damage, 0.0f);
	Rules.FixedDamage.Add(MobaGameplayTags::Damage_True, 60);
	TestTrue(TEXT("显式配置有效"), Rules.IsValid());
	TestTrue(TEXT("精确已知类型"), Rules.ResolveDamage(MobaGameplayTags::Damage_True, Damage));
	TestEqual(TEXT("可替换测试伤害"), Damage, 60.0f);
	TestFalse(TEXT("根类型不回退True"), Rules.ResolveDamage(MobaGameplayTags::Damage, Damage));
	TestFalse(TEXT("无效类型不回退True"), Rules.ResolveDamage(FGameplayTag(), Damage));
	FGameplayTagContainer Tags(MobaGameplayTags::Damage_True);
	TestTrue(TEXT("完整Spec单类型可解析"), Rules.ResolveDamageTags(Tags, Damage));
	Tags.AddTag(MobaGameplayTags::Damage);
	TestFalse(TEXT("GE冲突标签提交前拒绝"), Rules.ResolveDamageTags(Tags, Damage));
	TestTrue(TEXT("敌人允许"), Rules.AllowsRelation(ETeamRelation::Enemy));
	TestFalse(TEXT("同盟拒绝"), Rules.AllowsRelation(ETeamRelation::Ally));
	TestFalse(TEXT("未知关系拒绝"), Rules.AllowsRelation(ETeamRelation::Undefined));
	TestFalse(TEXT("中立默认拒绝"), Rules.AllowsRelation(ETeamRelation::Neutral));
	Rules.bAllowNeutralTargets = true;
	TestTrue(TEXT("中立策略可替换"), Rules.AllowsRelation(ETeamRelation::Neutral));
	Rules.FixedDamage[MobaGameplayTags::Damage_True] = std::numeric_limits<float>::infinity();
	TestFalse(TEXT("非有限数拒绝"), Rules.IsValid());
	TestFalse(TEXT("非有限数不进入结算"), Rules.ResolveDamage(MobaGameplayTags::Damage_True, Damage));
	return true;
}

namespace
{
class FCombatLifeScenario final : public IAutomationLatentCommand
{
public:
	explicit FCombatLifeScenario(FAutomationTestBase* InTest) : Test(InTest) {}
	virtual bool Update() override
	{
		if (!bSetup)
		{
			const UWorld::InitializationValues Values = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("CombatFixture"), nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL()); World->BeginPlay();
			AMatchGameState* Match = World->SpawnActor<AMatchGameState>();
			World->SetGameState(Match);
			Match->MatchId = FGuid::NewGuid(); Match->ExperienceInstance = FGuid::NewGuid();
			Match->Teams.Teams = {7, 21};
			FTeamRelationEntry Relation; Relation.From = 7; Relation.To = 21; Relation.Relation = ETeamRelation::Enemy;
			Match->Teams.Relations.Add(Relation); Swap(Relation.From, Relation.To); Match->Teams.Relations.Add(Relation);
			UAbilitySetDefinition* Set = NewObject<UAbilitySetDefinition>(World);
			auto& Grant = Set->Abilities.AddDefaulted_GetRef();
			Grant.Ability = USelfCommitAbility::StaticClass(); Grant.Slot = MobaGameplayTags::Input_Slot_Primary;
			UHeroDefinition* Definition = NewObject<UHeroDefinition>(World);
			Definition->AbilitySet = Set; Definition->RespawnDelay = 1;
			TArray<AHeroCharacter*> Heroes;
			TArray<AHeroPlayerController*> Controllers;
			for (int32 Index = 0; Index < 2; ++Index)
			{
				AParticipantState* Participant = World->SpawnActor<AParticipantState>();
				Participant->ParticipantId = FGuid::NewGuid(); Participant->MatchId = Match->MatchId; Participant->TeamId = Index == 0 ? 7 : 21;
				FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				AHeroCharacter* Hero = World->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), FVector(0, Index * 300, 100), FRotator::ZeroRotator, Spawn);
				Test->TestTrue(TEXT("初始化"), Hero->InitializeHero(Definition, Participant));
				AHeroPlayerController* Controller = World->SpawnActor<AHeroPlayerController>();
				Controller->SetPlayerState(Participant); Participant->SetOwner(Controller); Participant->PrimaryHero = Hero;
				Controller->Possess(Hero); Controller->AcceptContentReady();
				Controller->ServerPlayerReady(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision);
				Heroes.Add(Hero); Controllers.Add(Controller);
			}
			AHeroCharacter* Source = Heroes[0]; Target = Heroes[1];
			Abilities = Target->GetHeroAbilitySystem();
			FCombatRules Rules; Rules.FixedDamage.Add(MobaGameplayTags::Damage_True, 60);
			FString Reason;
			Test->TestTrue(TEXT("合法敌对目标"), MobaCombat::ValidateUnitTarget(Source, Target, 900, Rules, Reason));
			Test->TestFalse(TEXT("射程拒绝"), MobaCombat::ValidateUnitTarget(Source, Target, 100, Rules, Reason));
			Test->TestEqual(TEXT("范围分类"), Reason, FString(TEXT("Range")));
			const FVector OriginalTargetLocation = Target->GetActorLocation();
			Target->SetActorLocation(Source->GetActorLocation() + FVector(899, 0, 100));
			Test->TestFalse(TEXT("旧Unit保持三维射程，XY在界内不放宽"), MobaCombat::ValidateUnitTarget(Source, Target, 900, Rules, Reason));
			Test->TestEqual(TEXT("旧Unit三维越距分类"), Reason, FString(TEXT("Range")));
			Target->SetActorLocation(OriginalTargetLocation);
			Test->TestFalse(TEXT("无目标拒绝"), MobaCombat::ValidateUnitTarget(Source, nullptr, 900, Rules, Reason));
			Test->TestFalse(TEXT("自身拒绝"), MobaCombat::ValidateUnitTarget(Source, Source, 900, Rules, Reason));
			Target->TeamId = 7;
			Test->TestFalse(TEXT("友方拒绝"), MobaCombat::ValidateUnitTarget(Source, Target, 900, Rules, Reason));
			Target->TeamId = INDEX_NONE;
			Test->TestFalse(TEXT("中立拒绝"), MobaCombat::ValidateUnitTarget(Source, Target, 900, Rules, Reason));
			Target->TeamId = 21;
			Abilities->AddLooseGameplayTag(MobaGameplayTags::State_Untargetable);
			Test->TestFalse(TEXT("不可目标状态拒绝"), MobaCombat::ValidateUnitTarget(Source, Target, 900, Rules, Reason));
			Abilities->RemoveLooseGameplayTag(MobaGameplayTags::State_Untargetable);
			Abilities->AddLooseGameplayTag(MobaGameplayTags::State_Invulnerable);
			Test->TestFalse(TEXT("免伤状态拒绝"), MobaCombat::ValidateUnitTarget(Source, Target, 900, Rules, Reason));
			Abilities->RemoveLooseGameplayTag(MobaGameplayTags::State_Invulnerable);
			Entity = Target->EntityId;
			const auto CancelHandle = Abilities->GiveAbility(FGameplayAbilitySpec(UCombatCancellationTestAbility::StaticClass(), 1));
			Test->TestTrue(TEXT("死亡前真实能力激活"), Abilities->TryActivateAbility(CancelHandle, false));
			Test->TestTrue(TEXT("能力保持active等待死亡"), Abilities->FindAbilitySpecFromHandle(CancelHandle)->IsActive());
			Slot = Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary);
			SpawnPosition = Target->GetActorLocation();
			const auto AddDuration = [this](TSubclassOf<UGameplayEffect> Class)
			{
				FGameplayEffectSpecHandle Spec = Abilities->MakeOutgoingSpec(Class, 1, Abilities->MakeEffectContext());
				Spec.Data->SetDuration(20, true);
				return Abilities->ApplyGameplayEffectSpecToSelf(*Spec.Data);
			};
			Persistent = AddDuration(UDeathPersistentEffect::StaticClass());
			const auto Cleanup = AddDuration(UDeathRemovableEffect::StaticClass());
			Cooldown = AddDuration(UUnitAttackCooldownEffect::StaticClass());
			Test->TestTrue(TEXT("真实持续GE有效"), Persistent.IsValid() && Cleanup.IsValid() && Cooldown.IsValid());
			Started = Abilities->GetActiveGameplayEffect(Persistent)->StartWorldTime;
			UGameplayEffect* Lethal = NewObject<UGameplayEffect>(World);
			Lethal->DurationPolicy = EGameplayEffectDurationType::Instant;
			auto& Modifier = Lethal->Modifiers.AddDefaulted_GetRef();
			Modifier.Attribute = UHeroAttributeSet::GetIncomingDamageAttribute(); Modifier.ModifierOp = EGameplayModOp::Additive;
			Modifier.ModifierMagnitude = FScalableFloat(200);
			Context = Source->GetHeroAbilitySystem()->MakeEffectContext();
			GMP::FMessageUtils::ListenObjectMessage(Target, MOBA_MESSAGE_HERO_LIFE_CHANGED, World, [this](const FHeroLifeSnapshot& Snapshot)
			{
				if (Snapshot.State == EHeroLifeState::Dead)
				{
					++DeathNotifications;
					Test->TestEqual(TEXT("通知发生在归属提交之后"), Snapshot.DeathCount, uint32(1));
					Test->TestFalse(TEXT("通知重入不能重复死亡"), Target->EnterDeath(Context));
				}
			});
			Abilities->ApplyGameplayEffectToSelf(Lethal, 1, Context);
			Test->TestTrue(TEXT("死亡已提交"), Target->GetLifeSnapshot().State == EHeroLifeState::Dead);
			Test->TestEqual(TEXT("血量不为负"), Target->GetAttributes()->GetHealth(), 0.0f);
			Test->TestEqual(TEXT("唯一通知"), DeathNotifications, 1);
			Test->TestEqual(TEXT("归属参赛者"), Target->GetLifeSnapshot().KillerParticipant, Source->ParticipantId);
			Test->TestEqual(TEXT("归属实体"), Target->GetLifeSnapshot().KillerEntity, Source->EntityId);
			Abilities->ApplyGameplayEffectToSelf(Lethal, 1, Context);
			Test->TestEqual(TEXT("重复致死一次提交"), Target->GetLifeSnapshot().DeathCount, uint32(1));
			Test->TestFalse(TEXT("死亡输入关闭"), Target->CanAcceptPlayerInput());
			Test->TestFalse(TEXT("死亡取消标记能力"), Abilities->FindAbilitySpecFromHandle(CancelHandle)->IsActive());
			Test->TestTrue(TEXT("死亡移动关闭"), Target->GetCharacterMovement()->MovementMode == MOVE_None);
			Test->TestTrue(TEXT("死亡碰撞关闭"), Target->GetCapsuleComponent()->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
			Test->TestFalse(TEXT("死亡目标拒绝"), MobaCombat::ValidateUnitTarget(Source, Target, 900, Rules, Reason));
			Test->TestNull(TEXT("死亡清理GE被移除"), Abilities->GetActiveGameplayEffect(Cleanup));
			Test->TestNotNull(TEXT("持续GE保留原handle"), Abilities->GetActiveGameplayEffect(Persistent));
			Test->TestNotNull(TEXT("冷却GE保留原handle"), Abilities->GetActiveGameplayEffect(Cooldown));

			bSetup = true;
			return false;
		}
		// 每个真实引擎帧最多推进一次 fixture；TimerManager 以 GFrameCounter 防重复 tick。
		World->Tick(LEVELTICK_All, 0.1f);
		if (World->GetTimeSeconds() < 1.5f) return false;

		Test->TestTrue(TEXT("定时复活"), Target->IsAlive());
		Test->TestEqual(TEXT("新生命代次"), Target->GetLifeSnapshot().Generation, uint32(2));
		Test->TestEqual(TEXT("原Entity"), Target->EntityId, Entity);
		Test->TestTrue(TEXT("原ASC与spec"), Target->GetHeroAbilitySystem() == Abilities && Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary) == Slot);
		Test->TestEqual(TEXT("授予来源未重复"), Abilities->GetSourceCount(), 1);
		Test->TestEqual(TEXT("初始化不重复"), Target->InitializationGeneration, uint32(1));
		Test->TestEqual(TEXT("满血恢复"), Target->GetAttributes()->GetHealth(), 100.0f);
		Test->TestTrue(TEXT("出生位置恢复"), FVector::Dist2D(Target->GetActorLocation(), SpawnPosition) < 1);
		Test->TestTrue(TEXT("复活输入可恢复"), Target->CanAcceptPlayerInput());
		const auto* Active = Abilities->GetActiveGameplayEffect(Persistent);
		Test->TestNotNull(TEXT("复活仍持有原持续效果"), Active);
		if (Active)
		{
			Test->TestEqual(TEXT("效果开始时间未重置"), Active->StartWorldTime, Started);
			Test->TestTrue(TEXT("持续期随World继续"), Active->GetTimeRemaining(World->GetTimeSeconds()) < 20);
		}
		Test->TestNotNull(TEXT("复活没有清空冷却"), Abilities->GetActiveGameplayEffect(Cooldown));
		GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_HERO_LIFE_CHANGED, World);
		World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
		return true;

	}
private:
	FAutomationTestBase* Test;
	bool bSetup = false;
	UWorld* World = nullptr;
	AHeroCharacter* Target = nullptr;
	UHeroAbilitySystemComponent* Abilities = nullptr;
	FGuid Entity;
	FGameplayAbilitySpecHandle Slot;
	FVector SpawnPosition;
	FActiveGameplayEffectHandle Persistent;
	FActiveGameplayEffectHandle Cooldown;
	float Started = 0;
	FGameplayEffectContextHandle Context;
	int32 DeathNotifications = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatLifeTest, "Moba.Combat.LifeAndTargeting", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCombatLifeTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FCombatLifeScenario>(this));
	return true;
}

#endif
