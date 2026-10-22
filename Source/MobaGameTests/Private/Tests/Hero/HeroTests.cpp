#include "Teams/TeamConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "Hero/HeroDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Player/InputProfileDefinition.h"
#include "Player/HeroPlayerController.h"
#include "Hero/HeroCharacter.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "AbilitySystem/SelfCommitAbility.h"
#include "Match/ParticipantState.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "MobaGameplayTags.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTeamRelationsTest, "Moba.Hero.TeamRelations", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTeamRelationsTest::RunTest(const FString& Parameters)
{
	FTeamConfiguration Teams;
	Teams.Teams = {7, 21, 42};
	for (int32 From : Teams.Teams)
	{
		for (int32 To : Teams.Teams)
		{
			if (From != To)
			{
				FTeamRelationEntry& Entry = Teams.Relations.AddDefaulted_GetRef();
				Entry.From = From;
				Entry.To = To;
				Entry.Relation = From == 7 && To == 21 ? ETeamRelation::Ally : ETeamRelation::Enemy;
			}
		}
	}

	FString Reason;
	TestTrue(TEXT("三个非连续队伍完整配置"), Teams.IsValid(Reason));
	TestTrue(TEXT("同队友方"), Teams.Query(42, 42) == ETeamRelation::Ally);
	TestTrue(TEXT("跨队显式联盟"), Teams.Query(7, 21) == ETeamRelation::Ally);
	TestTrue(TEXT("关系有向"), Teams.Query(21, 7) == ETeamRelation::Enemy);
	TestTrue(TEXT("Neutral 不等同第四个玩家队伍"), Teams.Query(INDEX_NONE, 42) == ETeamRelation::Neutral);
	TestTrue(TEXT("未知队伍不退化为 Neutral"), Teams.Query(99, INDEX_NONE) == ETeamRelation::Undefined);
	Teams.Relations.Pop();
	TestFalse(TEXT("缺失跨队关系无默认敌我猜测"), Teams.IsValid(Reason));
	Teams.Teams.Add(7);
	TestFalse(TEXT("重复队伍拒绝"), Teams.IsValid(Reason));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroDefinitionValidationTest, "Moba.Hero.DefinitionValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHeroDefinitionValidationTest::RunTest(const FString& Parameters)
{
	FDataValidationContext HeroContext, SetContext, InputContext;
	TestTrue(TEXT("无配置Hero拒绝"), NewObject<UHeroDefinition>()->IsDataValid(HeroContext) == EDataValidationResult::Invalid);
	TestTrue(TEXT("空AbilitySet拒绝"), NewObject<UAbilitySetDefinition>()->IsDataValid(SetContext) == EDataValidationResult::Invalid);
	TestTrue(TEXT("缺输入资产拒绝"), NewObject<UInputProfileDefinition>()->IsDataValid(InputContext) == EDataValidationResult::Invalid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroGrantLifetimeTest, "Moba.Hero.GrantLifetime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHeroGrantLifetimeTest::RunTest(const FString& Parameters)
{
	const UWorld::InitializationValues Values =
		UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("HeroGrantFixture"), nullptr, true, ERHIFeatureLevel::Num, &Values);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	AHeroCharacter* Hero = World->SpawnActor<AHeroCharacter>();
	AParticipantState* Participant = World->SpawnActor<AParticipantState>();
	Participant->ParticipantId = FGuid::NewGuid();
	UAbilitySetDefinition* Set = NewObject<UAbilitySetDefinition>(World);
	FAbilityGrantDefinition& Grant = Set->Abilities.AddDefaulted_GetRef();
	Grant.Ability = USelfCommitAbility::StaticClass();
	Grant.Slot = MobaGameplayTags::Input_Slot_Primary;
	UHeroDefinition* Definition = NewObject<UHeroDefinition>(World);
	Definition->AbilitySet = Set;
	TestTrue(TEXT("第一次初始化成功"), Hero->InitializeHero(Definition, Participant));
	AHeroPlayerController* Controller = World->SpawnActor<AHeroPlayerController>();
	Controller->SetPlayerState(Participant);
	Participant->SetOwner(Controller);
	Participant->PrimaryHero = Hero;
	Controller->Possess(Hero);
	FString Rejection;
	TestFalse(TEXT("合法绑定但未ready的意图拒绝"), Controller->ValidateIntent(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision, Rejection));
	TestEqual(TEXT("未ready分类"), Rejection, FString(TEXT("NotReady")));
	TestFalse(TEXT("内容未挂载的连接尚不接收Hero资产引用"), Hero->IsNetRelevantFor(Controller, Hero, Hero->GetActorLocation()));
	UHeroAbilitySystemComponent* Abilities = Hero->GetHeroAbilitySystem();
	const FGuid Entity = Hero->EntityId;
	TestTrue(TEXT("ActorInfo Owner和Avatar均为Hero"), Abilities->AbilityActorInfo->OwnerActor.Get() == Hero && Abilities->AbilityActorInfo->AvatarActor.Get() == Hero);
	TestTrue(TEXT("ActorInfo拥有控制器正确"), Abilities->AbilityActorInfo->PlayerController.Get() == Controller);
	const FGameplayAbilitySpecHandle Handle = Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary);
	TestTrue(TEXT("真实Spec已授予"), Handle.IsValid() && Abilities->FindAbilitySpecFromHandle(Handle));
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Hero->RefreshActorInfo();
		TestTrue(TEXT("重复初始化幂等"), Hero->InitializeHero(Definition, Participant));
	}

	TestEqual(TEXT("实体身份不变"), Hero->EntityId, Entity);
	TestEqual(TEXT("授予仅一次"), Abilities->GetActivatableAbilities().Num(), 1);
	TestEqual(TEXT("来源仅一个"), Abilities->GetSourceCount(), 1);
	TestEqual(TEXT("ActorInfo刷新不推进revision"), Abilities->GetLoadoutRevision(), uint32(1));
	TestFalse(TEXT("未ready不能激活"), Abilities->TryActivateAbility(Handle, false));
	const FGuid OtherSource = FGuid::NewGuid();
	TestFalse(TEXT("冲突槽提前拒绝"), Abilities->GrantFromSource(Set, OtherSource));
	TestEqual(TEXT("拒绝无部分授予"), Abilities->GetActivatableAbilities().Num(), 1);
	TestFalse(TEXT("不存在来源不能移除别人的能力"), Abilities->RemoveSource(OtherSource));
	TestTrue(TEXT("移除自己的来源"), Abilities->RemoveSource(Entity));
	TestEqual(TEXT("移除后没有旧槽"), Abilities->GetSlots().Num(), 0);
	TestTrue(TEXT("第二来源可以占用释放的槽"), Abilities->GrantFromSource(Set, OtherSource));
	TestEqual(TEXT("实体就绪屏障跟随来源操作版本"), Hero->RequiredLoadoutRevision, Abilities->GetLoadoutRevision());
	TestFalse(TEXT("来源变化关闭旧输入"), Hero->bInputReady);
	TestFalse(TEXT("重复移除旧来源不影响新来源"), Abilities->RemoveSource(Entity));
	TestEqual(TEXT("新来源仍有一个能力"), Abilities->GetActivatableAbilities().Num(), 1);
	TestFalse(TEXT("旧revision不可ready"), Abilities->HasReplicatedLoadout(1));
	TestTrue(TEXT("当前revision匹配真实Spec"), Abilities->HasReplicatedLoadout(Abilities->GetLoadoutRevision()));
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

#endif
