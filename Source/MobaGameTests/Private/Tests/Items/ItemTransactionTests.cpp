#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/CombatStatusEffects.h"
#include "Experience/ExperienceDefinitions.h"
#include "Items/ItemDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Hero/HeroDefinition.h"
#include "Experience/ExperienceSubsystem.h"
#include "Hero/HeroCharacter.h"
#include "Items/ItemInventoryComponent.h"
#include "Player/HeroPlayerController.h"
#include "Match/ParticipantState.h"
#include "Match/MatchGameState.h"
#include "Tests/AbilitySystem/ManagedAbilityTestTypes.h"
#include "MobaGameplayTags.h"

namespace
{
	class FItemTransactionScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FItemTransactionScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds()) {}

		~FItemTransactionScenario() override
		{
			if (World)
			{
				World->EndPlay(EEndPlayReason::Quit);
				World->DestroyWorld(false);
				GEngine->DestroyWorldContext(World);
			}
		}


	public:
		bool Update() override
		{
			if (FPlatformTime::Seconds() - Started > 90)
			{
				Test->AddError(TEXT("Item World加载/清理超时"));
				return true;
			}

			if (!World)
			{
				const auto Values = UWorld::InitializationValues()
										.AllowAudioPlayback(false)
										.CreatePhysicsScene(true)
										.CreateNavigation(false)
										.CreateAISystem(false)
										.ShouldSimulatePhysics(false)
										.SetTransactional(false);
				World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
				GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
				World->InitializeActorsForPlay(FURL());
				World->BeginPlay();
				World->GetWorldSettings()->NotifyBeginPlay();
				Experience = World->GetSubsystem<UExperienceSubsystem>();
				Test->TestTrue(TEXT("实际Experience受理"), Experience->StartExperience(FPrimaryAssetId(TEXT("Playlist:Playlist_Batch0811"))) == EExperienceFailure::None);
				return false;
			}

			World->Tick(LEVELTICK_All, .02f);
			if (bFinished)
			{
				return Experience->GetSnapshot().State == EExperienceState::Unloaded;
			}

			if (Experience->GetSnapshot().State == EExperienceState::Failed)
			{
				Test->AddError(TEXT("Item Experience加载失败"));
				return true;
			}

			if (Experience->GetSnapshot().State != EExperienceState::ExperienceReady)
			{
				return false;
			}

			Run();
			bFinished = true;
			Experience->StopExperience();
			return false;
		}


	private:
		void Fresh()
		{
			if (Hero)
			{
				Controller->UnPossess();
				Hero->Destroy();
				Controller->Destroy();
				Participant->Destroy();
			}

			Participant = World->SpawnActor<AParticipantState>();
			Participant->ParticipantId = FGuid::NewGuid();
			Participant->MatchId = MatchId;
			Participant->TeamId = 7;
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Hero = World->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), FVector(0, 0, 150), FRotator::ZeroRotator, Spawn);
			Test->TestTrue(TEXT("真实批次Hero一次初始化"), Hero->InitializeHero(Experience->GetExperience()->Hero.Get(), Participant));
			Controller = World->SpawnActor<AHeroPlayerController>();
			Controller->SetPlayerState(Participant);
			Participant->SetOwner(Controller);
			Participant->PrimaryHero = Hero;
			Controller->Possess(Hero);
			Controller->AcceptContentReady();
			Controller->ServerPlayerReady(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision);
			Participant->InitializeEconomy(1000);
			Inventory = Hero->GetInventory();
			Abilities = Hero->GetHeroAbilitySystem();
			Sequence = 0;
			BaseSources = Abilities->GetSourceCount();
		}


	private:
		FItemRequest Request(EItemOperation Op, FName Id = NAME_None, FGuid Instance = {}, int32 Quantity = 1)
		{
			FItemRequest R;
			R.Entity = Hero->EntityId;
			R.Epoch = Hero->ConnectionEpoch;
			R.Sequence = ++Sequence;
			R.Revision = Inventory->GetSnapshot().Revision;
			R.Operation = Op;
			R.ItemId = Id;
			R.Instance = Instance;
			R.Quantity = Quantity;
			return R;
		}

		FItemResult Buy(const TCHAR* Id, int32 Quantity = 1) { return Inventory->Execute(Controller, Request(EItemOperation::Buy, Id, {}, Quantity)); }
		FItemResult Sell(FGuid Id) { return Inventory->Execute(Controller, Request(EItemOperation::Sell, NAME_None, Id)); }
		FItemResult Use(FGuid Id) { return Inventory->Execute(Controller, Request(EItemOperation::Use, NAME_None, Id)); }
		FGuid First(const TCHAR* Id)
		{
			const auto* Item = Inventory->GetSnapshot().Items.FindByPredicate([Id](const auto& Entry) { return Entry.ItemId == Id; });
			Test->TestNotNull(TEXT("要求的库存实例存在"), Item);
			return Item ? Item->Id : FGuid();
		}

		FString State() const
		{
			FString Value = FString::Printf(TEXT("%d/%u/%u/%u/%u/%d/%.5f/%.5f/%.5f/%.5f"), Participant->GetEconomy().Gold, Participant->GetEconomy().Revision,
				Inventory->GetSnapshot().Revision, Abilities->GetStateRevision(), Abilities->GetLoadoutRevision(), Abilities->GetSourceCount(), Hero->GetAttributes()->GetHealth(),
				Hero->GetAttributes()->GetShield(), Hero->GetAttributes()->GetArmor(), Hero->GetAttributes()->GetDamageTakenMultiplier());
			for (const auto& Item : Inventory->GetSnapshot().Items)
			{
				Value += FString::Printf(TEXT("|%s:%s:%d:%d:%s"), *Item.Id.ToString(), *Item.ItemId.ToString(), Item.Slot, Item.Quantity, *Item.ActiveAbility.ToString());
				const auto* Source = Abilities->FindSource(Item.Id);
				if (Source)
				{
					for (const auto Handle : Source->Abilities)
					{
						Value += TEXT("/A") + Handle.ToString();
					}

					for (const auto Handle : Source->Effects)
					{
						Value += TEXT("/E") + Handle.ToString();
					}
				}
			}

			for (auto Handle : Abilities->GetActiveEffects(FGameplayEffectQuery()))
			{
				Value += TEXT("#") + Handle.ToString();
			}

			FGameplayTagContainer Tags;
			Abilities->GetOwnedGameplayTags(Tags);
			Value += Tags.ToString();
			return Value;
		}

		void Reject(FItemRequest R, FName Expected)
		{
			const FString Before = State();
			const auto Result = Inventory->Execute(Controller, R);
			Test->TestFalse(TEXT("请求拒绝"), Result.bAccepted);
			Test->TestEqual(TEXT("准确拒绝原因"), Result.Reason, Expected);
			Test->TestEqual(TEXT("失败Gold/实例/来源/GE/版本/属性/冷却完全不变"), State(), Before);
		}

		void Run()
		{
			MatchId = FGuid::NewGuid();
			auto* Match = World->SpawnActor<AMatchGameState>();
			Match->MatchId = MatchId;
			World->SetGameState(Match);
			Fresh();
			Test->TestTrue(TEXT("购买Plate"), Buy(TEXT("ArmorPlate")).bAccepted);
			const FGuid Plate = First(TEXT("ArmorPlate"));
			Test->TestEqual(TEXT("1000买100剩900"), Participant->GetEconomy().Gold, 900);
			Test->TestEqual(TEXT("Plate真实Armor+10"), Hero->GetAttributes()->GetArmor(), 10.f);
			Test->TestEqual(TEXT("唯一来源增加1"), Abilities->GetSourceCount(), BaseSources + 1);
			Test->TestTrue(TEXT("单个卖出"), Sell(Plate).bAccepted);
			Test->TestEqual(TEXT("卖回50剩950"), Participant->GetEconomy().Gold, 950);
			Test->TestNull(TEXT("只撤本来源"), Abilities->FindSource(Plate));
			Test->TestFalse(TEXT("初始金币不能二次发放"), Participant->InitializeEconomy(1000));
			Buy(TEXT("ArmorPlate"), 2);
			const FGuid One = First(TEXT("ArmorPlate"));
			Sell(One);
			Test->TestEqual(TEXT("卖一留另一Armor"), Hero->GetAttributes()->GetArmor(), 10.f);
			Test->TestEqual(TEXT("另一实例仍在"), Inventory->GetSnapshot().Items.Num(), 1);
			Buy(TEXT("FocusCharm"), 2);
			Test->TestTrue(FString::Printf(TEXT("两个0.9真实MultiplyCompound=0.81 actual=%.8f"), Hero->GetAttributes()->GetDamageTakenMultiplier()),
				FMath::IsNearlyEqual(Hero->GetAttributes()->GetDamageTakenMultiplier(), .81f, .0001f));
			Sell(First(TEXT("FocusCharm")));
			Test->TestTrue(TEXT("卖一个留0.9"), FMath::IsNearlyEqual(Hero->GetAttributes()->GetDamageTakenMultiplier(), .9f));
			const int32 GoldBeforeCombine = Participant->GetEconomy().Gold;
			Test->TestTrue(TEXT("组合Plate+Charm"), Inventory->Execute(Controller, Request(EItemOperation::Combine, TEXT("CompositeGuard"))).bAccepted);
			Test->TestEqual(TEXT("合成只追加100"), Participant->GetEconomy().Gold, GoldBeforeCombine - 100);
			Test->TestEqual(TEXT("合成只剩成品实例"), Inventory->GetSnapshot().Items.Num(), 1);
			Test->TestEqual(TEXT("成品实际Armor"), Hero->GetAttributes()->GetArmor(), 10.f);
			Test->TestTrue(TEXT("成品实际减伤"), FMath::IsNearlyEqual(Hero->GetAttributes()->GetDamageTakenMultiplier(), .9f));

			Fresh();
			Buy(TEXT("ArmorPlate"), 3);
			Buy(TEXT("ArmorPlate"), 2);
			Buy(TEXT("FocusCharm"));
			Test->TestEqual(TEXT("真实六槽前置"), Inventory->GetSnapshot().Items.Num(), 6);
			Reject(Request(EItemOperation::Buy, TEXT("HealingPotion")), TEXT("InventoryFull"));
			Test->TestTrue(TEXT("满包组件腾槽合成成功"), Inventory->Execute(Controller, Request(EItemOperation::Combine, TEXT("CompositeGuard"))).bAccepted);
			Test->TestEqual(TEXT("满包合成变5槽"), Inventory->GetSnapshot().Items.Num(), 5);
			Test->TestEqual(TEXT("满包合成Gold250"), Participant->GetEconomy().Gold, 250);
			Reject(Request(EItemOperation::Combine, TEXT("CompositeGuard")), TEXT("MissingComponent"));
			Reject(Request(EItemOperation::Buy, TEXT("GuardOrb"), {}, 2), TEXT("InsufficientGold"));

			Fresh();
			Reject(Request(EItemOperation::Buy, TEXT("CompositeGuard")), TEXT("RecipeRequired"));
			auto Duplicate = Request(EItemOperation::Buy, TEXT("ArmorPlate"));
			const auto Original = Inventory->Execute(Controller, Duplicate);
			const FString After = State();
			const auto Replay = Inventory->Execute(Controller, Duplicate);
			Test->TestEqual(TEXT("重复返回原提交revision"), Replay.Revision, Original.Revision);
			Test->TestEqual(TEXT("重发只结算一次"), State(), After);
			for (int32 Index = 0; Index < 40; ++Index)
			{
				Reject(Request(EItemOperation::Buy, TEXT("unknown")), TEXT("ItemNotAllowed"));
			}

			Test->TestEqual(TEXT("结果缓存32有界"), Inventory->GetCachedResultCount(), 32);
			Reject(Duplicate, TEXT("OldSequence"));
			auto Invalid = Request(EItemOperation::Buy, TEXT("ArmorPlate"));
			Invalid.Entity = FGuid::NewGuid();
			Reject(Invalid, TEXT("Entity"));
			Invalid = Request(EItemOperation::Buy, TEXT("ArmorPlate"));
			--Invalid.Epoch;
			Reject(Invalid, TEXT("Epoch"));
			Invalid = Request(EItemOperation::Buy, TEXT("ArmorPlate"));
			--Invalid.Revision;
			Reject(Invalid, TEXT("InventoryRevision"));
			Reject(Request(EItemOperation::Buy, TEXT("ArmorPlate"), {}, 0), TEXT("Quantity"));
			Hero->SetActorLocation(FVector(2000, 0, 150));
			Reject(Request(EItemOperation::Buy, TEXT("ArmorPlate")), TEXT("ShopRange"));
			Hero->SetActorLocation(FVector(0, 0, 150));
			auto* MutableExperience = const_cast<UExperienceDefinition*>(Experience->GetExperience());
			{
				TGuardValue<bool> Disabled(MutableExperience->bItemsEnabled, false);
				Reject(Request(EItemOperation::Buy, TEXT("ArmorPlate")), TEXT("ItemsDisabled"));
			}

			const auto NewEpoch = Hero->ConnectionEpoch + 2;
			Participant->ConnectionEpoch = NewEpoch;
			Hero->ConnectionEpoch = NewEpoch;
			Invalid = Request(EItemOperation::Buy, TEXT("ArmorPlate"));
			Invalid.Sequence = 1;
			Test->TestTrue(TEXT("新Epoch合法低序号重新开始"), Inventory->Execute(Controller, Invalid).bAccepted);
			Sequence = 1;
			Buy(TEXT("FocusCharm"));

			// 第二个组件应用被拒绝，先前已暂存的真实GE必须撤回且不推进任何来源版本。
			FName ReentrantReason;
			int32 Applications = 0;
			FGameplayEffectApplicationQuery Inject;
			Inject.BindLambda(
				[&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&)
				{
					auto Nested = Request(EItemOperation::Buy, TEXT("GuardOrb"));
					ReentrantReason = Inventory->Execute(Controller, Nested).Reason;
					return ++Applications < 2;
				});
			Abilities->GameplayEffectApplicationQueries.Add(Inject);
			Reject(Request(EItemOperation::Combine, TEXT("CompositeGuard")), TEXT("GrantFailed"));
			Test->TestEqual(TEXT("真实GAS应用回调重入被锁拒绝"), ReentrantReason, FName(TEXT("Busy")));
			Test->TestEqual(TEXT("先暂存1GE再注入第2失败"), Applications, 2);
			Abilities->GameplayEffectApplicationQueries.Reset();

			Fresh();
			Buy(TEXT("ArmorPlate"));
			Buy(TEXT("FocusCharm"));
			Buy(TEXT("GuardOrb"));
			Test->TestTrue(TEXT("标签反应上下文前置Shield"), Use(First(TEXT("GuardOrb"))).bAccepted);
			Reject(Request(EItemOperation::Sell, NAME_None, First(TEXT("ArmorPlate"))), TEXT("TagReactiveContext"));
			Reject(Request(EItemOperation::Combine, TEXT("CompositeGuard")), TEXT("TagReactiveContext"));

			Fresh();
			Buy(TEXT("GuardOrb"));
			const FGuid BusySource = First(TEXT("GuardOrb"));
			auto* HoldingSet = NewObject<UAbilitySetDefinition>(World);
			HoldingSet->Abilities.AddDefaulted_GetRef().Ability = UManagedAbilityHoldingAbility::StaticClass();
			Test->TestTrue(TEXT("活动来源夹具替换"), Abilities->ReplaceManagedSource(BusySource, HoldingSet, Abilities->GetStateRevision()).IsSuccess());
			const auto HoldingHandle = Abilities->FindSource(BusySource)->Abilities[0];
			Test->TestTrue(TEXT("真实原生能力保持激活"), Abilities->TryActivateAbility(HoldingHandle));
			Reject(Request(EItemOperation::Sell, NAME_None, BusySource), TEXT("ActiveAbility"));
			CastChecked<UManagedAbilityHoldingAbility>(Abilities->FindAbilitySpecFromHandle(HoldingHandle)->GetPrimaryInstance())->Finish();
			Test->TestTrue(TEXT("能力结束后出售成功"), Sell(BusySource).bAccepted);

			Fresh();
			Buy(TEXT("GuardOrb"), 2);
			Buy(TEXT("HealingPotion"), 3);
			const FGuid Orb = First(TEXT("GuardOrb")), Potion = First(TEXT("HealingPotion"));
			const FGuid OtherOrb = Inventory->GetSnapshot().Items[1].Id;
			Reject(Request(EItemOperation::Use, NAME_None, Potion), TEXT("FullHealth"));
			Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 40);
			Test->TestTrue(TEXT("Orb实际GA提交"), Use(Orb).bAccepted);
			Test->TestEqual(TEXT("真实盾30"), Hero->GetAttributes()->GetShield(), 30.f);
			Reject(Request(EItemOperation::Use, NAME_None, OtherOrb), TEXT("Cooldown"));
			Test->TestTrue(TEXT("不同定义冷却独立Heal成功"), Use(Potion).bAccepted);
			Test->TestEqual(TEXT("即时Heal25"), Hero->GetAttributes()->GetHealth(), 65.f);
			Test->TestEqual(TEXT("3叠成功后只扣1"), Inventory->GetSnapshot().Items.Last().Quantity, 2);
			Reject(Request(EItemOperation::Use, NAME_None, Potion), TEXT("Cooldown"));
			Test->TestTrue(TEXT("卖Orb"), Sell(Orb).bAccepted);
			Test->TestTrue(TEXT("有Shield时无标签来源Orb可重买"), Buy(TEXT("GuardOrb")).bAccepted);
			Reject(Request(EItemOperation::Use, NAME_None, Inventory->GetSnapshot().Items.Last().Id), TEXT("Cooldown"));
			Reject(Request(EItemOperation::Buy, TEXT("ArmorPlate")), TEXT("TagReactiveContext"));
			Test->TestTrue(TEXT("Shield/冷却状态仍存在"), Hero->GetAttributes()->GetShield() == 30 && Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_ItemHeal));

			Fresh();
			Buy(TEXT("HealingPotion"));
			const FGuid Single = First(TEXT("HealingPotion"));
			Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 50);
			const auto Reactive = Abilities->GiveAbility(FGameplayAbilitySpec(UManagedAbilityOwnedTagTestAbility::StaticClass(), 1));
			Reject(Request(EItemOperation::Use, NAME_None, Single), TEXT("TagReactiveContext"));
			Test->TestFalse(TEXT("反应上下文拒绝不提交冷却"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_ItemHeal));
			Abilities->ClearAbility(Reactive);
			Test->TestTrue(TEXT("最后一件完成即时提交"), Use(Single).bAccepted);
			Test->TestEqual(TEXT("最后一件Heal真实75"), Hero->GetAttributes()->GetHealth(), 75.f);
			Test->TestTrue(TEXT("消耗后无实例"), Inventory->GetSnapshot().Items.IsEmpty());
			Test->TestNull(TEXT("最后一件来源同步清理"), Abilities->FindSource(Single));
			Test->TestTrue(TEXT("消耗清理不撤冷却"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_ItemHeal));
			Buy(TEXT("HealingPotion"));
			Reject(Request(EItemOperation::Use, NAME_None, First(TEXT("HealingPotion"))), TEXT("Cooldown"));
			Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 0);
			Hero->EnterDeath(Abilities->MakeEffectContext());
			Reject(Request(EItemOperation::Use, NAME_None, First(TEXT("HealingPotion"))), TEXT("LifeState"));
		}

		FAutomationTestBase* Test;


	private:
		UWorld* World = nullptr;

		UExperienceSubsystem* Experience = nullptr;

		AHeroCharacter* Hero = nullptr;

		AHeroPlayerController* Controller = nullptr;

		AParticipantState* Participant = nullptr;

		UItemInventoryComponent* Inventory = nullptr;

		UHeroAbilitySystemComponent* Abilities = nullptr;

		FGuid MatchId;

		uint32 Sequence = 0;

		int32 BaseSources = 0;

		bool bFinished = false;

		double Started;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FItemTransactionTest, "Moba.Items.Transactions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FItemTransactionTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FItemTransactionScenario>(this));
	return true;
}
#endif
