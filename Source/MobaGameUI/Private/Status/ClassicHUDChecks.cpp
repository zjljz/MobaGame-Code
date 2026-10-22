#include "Status/ClassicHUDWidget.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "ClientPresentationSubsystem.h"
#include "Combat/HeroAttributeSet.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Experience/ExperienceSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/WorldSettings.h"
#include "Hero/HeroAttributeComponent.h"
#include "Hero/HeroCharacter.h"
#include "Hero/HeroDefinition.h"
#include "ImageUtils.h"
#include "Items/ItemInventoryComponent.h"
#include "Match/MatchGameState.h"
#include "Match/ParticipantState.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MobaGameplayTags.h"
#include "Player/HeroPlayerController.h"
#include "Slate/SceneViewport.h"
#include "UObject/UnrealType.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
	UClassicHUDWidget* FindLiveHUD()
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || World->WorldType != EWorldType::PIE || !World->GetFirstPlayerController())
			{
				continue;
			}

			ULocalPlayer* Player = World->GetFirstPlayerController()->GetLocalPlayer();
			auto* Presentation = Player ? Player->GetSubsystem<UClientPresentationSubsystem>() : nullptr;
			if (auto* HUD = Presentation ? Cast<UClassicHUDWidget>(Presentation->FindLayer(TEXT("Status"))) : nullptr)
			{
				return HUD;
			}

			if (UUserWidget* Frontend = Presentation ? Presentation->FindLayer(TEXT("Frontend")) : nullptr)
			{
				Frontend->ProcessEvent(Frontend->FindFunctionChecked(TEXT("StartLocalMatch")), nullptr);
			}
		}

		return nullptr;
	}

	template<typename WidgetType>
	WidgetType* Control(UUserWidget* Root, const TCHAR* Name)
	{
		return CastChecked<WidgetType>(Root->GetWidgetFromName(Name));
	}

	bool SaveViewport(UWorld* World, const FString& Name, FIntPoint Expected, FAutomationTestBase* Test)
	{
		TArray<FColor> Pixels;
		FIntVector Size = FIntVector::ZeroValue;
		const auto Viewport = World->GetGameViewport()->GetGameViewportWidget();
		if (!Test->TestTrue(TEXT("真实游戏视口 Slate 截图"), Viewport.IsValid() && FSlateApplication::Get().TakeScreenshot(Viewport.ToSharedRef(), Pixels, Size)))
		{
			return false;
		}

		Test->TestEqual(TEXT("截图实际宽度"), Size.X, Expected.X);
		Test->TestEqual(TEXT("截图实际高度"), Size.Y, Expected.Y);
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}

		TArray64<uint8> PNG;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, PNG);
		return Test->TestTrue(TEXT("保存原始视口 PNG"), FFileHelper::SaveArrayToFile(PNG, *(FPaths::ProjectDir() / TEXT("AI/classic-hud") / Name)));
	}

	class FClassicCapture final : public IAutomationLatentCommand
	{
	public:
		explicit FClassicCapture(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
		{
		}

		virtual bool Update() override
		{
			UClassicHUDWidget* HUD = FindLiveHUD();
			if (!HUD || !HUD->GetPresentedStatus().bInventoryReady || !HUD->GetPresentedStatus().bCombatReady)
			{
				if (FPlatformTime::Seconds() - Started < 15)
				{
					return false;
				}

				Test->AddError(TEXT("ClassicCapture 需要已经进入 Classic 的单玩家 PIE"));
				return true;
			}

			UWorld* World = HUD->GetWorld();
			auto* Viewport = World->GetGameViewport()->GetGameViewport();
			if (Phase == 0)
			{
				OriginalSize = Viewport->GetSizeXY();
				OriginalMode = Viewport->GetWindowMode();
				World->GetGameViewport()->GetWindow()->SetWindowMode(EWindowMode::Windowed);
				World->GetGameViewport()->GetWindow()->Resize(FVector2D(1920, 1080));
				NextPhase();
				return false;
			}

			if (FPlatformTime::Seconds() - Changed < .6)
			{
				return false;
			}

			const FIntPoint Expected = Phase < 4 ? FIntPoint(1920, 1080) : FIntPoint(2560, 1440);
			if ((Phase == 1 || Phase == 4) && Viewport->GetSizeXY() != Expected && ResizeAttempts++ < 3)
			{
				const auto Window = World->GetGameViewport()->GetWindow();
				const FVector2D Difference(Expected - Viewport->GetSizeXY());
				Window->Resize(Window->GetClientSizeInScreen() + Difference);
				Changed = FPlatformTime::Seconds();
				return false;
			}
			if (Phase == 1 || Phase == 4)
			{
				Test->TestEqual(TEXT("真实渲染视口尺寸"), Viewport->GetSizeXY(), Expected);
				const auto& State = HUD->GetPresentedStatus();
				Test->TestEqual(TEXT("完整技能数量"), Control<UHorizontalBox>(HUD, TEXT("AbilityBox"))->GetChildrenCount(), State.Abilities.Num());
				Test->TestEqual(TEXT("完整背包数量"), Control<UUniformGridPanel>(HUD, TEXT("ItemBox"))->GetChildrenCount(), State.Items.Num());
				for (UWidget* Child : Control<UUniformGridPanel>(HUD, TEXT("ItemBox"))->GetAllChildren())
				{
					const FVector2D ChildSize = Child->GetCachedGeometry().GetLocalSize();
					Test->TestTrue(TEXT("背包格具有有效可见尺寸"), ChildSize.X > 60 && ChildSize.Y > 40);
				}

				for (UWidget* Child : Control<UHorizontalBox>(HUD, TEXT("AbilityBox"))->GetAllChildren())
				{
					auto* Entry = CastChecked<UUserWidget>(Child);
					auto* Name = Control<UTextBlock>(Entry, TEXT("NameText"));
					auto* Detail = Control<UTextBlock>(Entry, TEXT("DetailText"));
					Test->TestTrue(TEXT("技能名容纳在独立区域"), Name->GetDesiredSize().Y <= Name->GetCachedGeometry().GetLocalSize().Y + 1);
					Test->TestTrue(TEXT("技能等级文字不截断"), Detail->GetDesiredSize().Y <= Detail->GetCachedGeometry().GetLocalSize().Y + 1);
				}
				Test->TestFalse(TEXT("HUD 不取键盘焦点"), HUD->IsFocusable());
				Test->TestFalse(TEXT("展开按钮不取键盘焦点"), Control<UButton>(HUD, TEXT("AttributesButton"))->GetIsFocusable());
				const FGeometry Host = HUD->GetCachedGeometry();
				const FGeometry Card = Control<UWidget>(HUD, TEXT("StatusCard"))->GetCachedGeometry();
				const FVector2D HostSize = Host.GetAbsoluteSize();
				const FVector2D Size = Card.GetAbsoluteSize();
				const FVector2D Offset = FVector2D(Card.GetAbsolutePosition()) - FVector2D(Host.GetAbsolutePosition());
				Test->TestTrue(TEXT("主条 22%-78% 居中"), FMath::IsNearlyEqual(Offset.X / HostSize.X, .22, .001) && FMath::IsNearlyEqual(Size.X / HostSize.X, .56, .001));
				Test->TestTrue(TEXT("14% 高度且贴底无间隙"), FMath::IsNearlyEqual(Size.Y / HostSize.Y, .14, .001) && FMath::Abs(Offset.Y + Size.Y - HostSize.Y) < 1);
				Test->TestTrue(TEXT("属性默认收起"), Control<UWidget>(HUD, TEXT("AttributesPanel"))->GetVisibility() == ESlateVisibility::Collapsed);
				SaveViewport(World, Phase == 1 ? TEXT("compact-1920x1080.png") : TEXT("compact-2560x1440.png"), Expected, Test);
				Control<UButton>(HUD, TEXT("AttributesButton"))->OnClicked.Broadcast();
				NextPhase();
				return false;
			}

			if (Phase == 2 || Phase == 5)
			{
				Test->TestTrue(TEXT("真实按钮事件展开属性"), Control<UWidget>(HUD, TEXT("AttributesPanel"))->GetVisibility() == ESlateVisibility::SelfHitTestInvisible);
				const FString Attributes = Control<UTextBlock>(HUD, TEXT("AttributesText"))->GetText().ToString();
				Test->TestTrue(TEXT("列表包含真实字段及换行"), Attributes.Contains(TEXT("护甲")) && Attributes.Contains(TEXT("\n")));
				SaveViewport(World, Phase == 2 ? TEXT("expanded-1920x1080.png") : TEXT("expanded-2560x1440.png"), Expected, Test);
				Control<UButton>(HUD, TEXT("CloseAttributesButton"))->OnClicked.Broadcast();
				Test->TestTrue(TEXT("关闭按钮收起"), Control<UWidget>(HUD, TEXT("AttributesPanel"))->GetVisibility() == ESlateVisibility::Collapsed);
				if (Phase == 5)
				{
					World->GetGameViewport()->GetWindow()->SetWindowMode(OriginalMode);
					World->GetGameViewport()->GetWindow()->Resize(FVector2D(OriginalSize));
					return true;
				}

				NextPhase();
				return false;
			}

			World->GetGameViewport()->GetWindow()->SetWindowMode(EWindowMode::WindowedFullscreen);
			ResizeAttempts = 0;
			NextPhase();
			return false;
		}

	private:
		void NextPhase()
		{
			++Phase;
			Changed = FPlatformTime::Seconds();
		}

		FAutomationTestBase* Test;
		double Started;
		double Changed = 0;
		int32 Phase = 0;
		int32 ResizeAttempts = 0;
		FIntPoint OriginalSize;
		EWindowMode::Type OriginalMode = EWindowMode::Windowed;
	};

	/** 独立瞬态 World；不修改运行比赛、资产或共享 CDO。模板只读复用当前 Classic 已加载配置。 */
	class FClassicFixture
	{
	public:
		explicit FClassicFixture(UClassicHUDWidget* LiveHUD, bool bNoResource = false)
		{
			const auto* LiveHero = CastChecked<AHeroCharacter>(LiveHUD->GetOwningPlayerPawn());
			const auto* LiveRules = LiveHUD->GetWorld()->GetSubsystem<UExperienceSubsystem>()->GetRules();
			const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
			auto* Match = World->SpawnActor<AMatchGameState>();
			World->SetGameState(Match);
			Match->MatchId = FGuid::NewGuid();
			Participant = World->SpawnActor<AParticipantState>();
			Participant->MatchId = Match->MatchId;
			Participant->ParticipantId = FGuid::NewGuid();
			Participant->TeamId = 7;
			Controller = World->SpawnActor<AHeroPlayerController>();
			Controller->SetActorTickEnabled(false);
			Controller->SetAsLocalPlayerController();
			Player = NewObject<ULocalPlayer>(GEngine);
			Player->AddToRoot();
			Controller->Player = Player;
			Player->PlayerController = Controller;
			Controller->SetPlayerState(Participant);
			Participant->SetOwner(Controller);
			Rules = DuplicateObject<UMatchRulesDefinition>(LiveRules, World);
			auto* Context = World->GetSubsystem<UExperienceSubsystem>();
			FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("RulesDef"))->SetObjectPropertyValue_InContainer(Context, Rules);
			const_cast<FExperienceSnapshot&>(Context->GetSnapshot()).State = EExperienceState::ExperienceReady;
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Hero = World->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
			auto* Definition = DuplicateObject<UHeroDefinition>(LiveHero->GetDefinition(), World);
			Definition->RespawnDelay = .1f;
			// UI 通知夹具使用静止资源，避免自然恢复改变定向展示断言。
			Definition->Attributes.HealthRegeneration = {0, 0};
			Definition->Attributes.ManaRegeneration = {0, 0};
			if (bNoResource)
			{
				Definition->ResourceType = EHeroResourceType::None;
				Definition->Attributes.MaxMana = {0, 0};
			}

			bInitialized = Hero->InitializeHero(Definition, Participant);
			Participant->PrimaryHero = Hero;
			Controller->Possess(Hero);
			Controller->AcceptContentReady();
			Participant->InitializeEconomy(Rules->ItemEconomyRules.InitialGold);
			Abilities = Hero->GetHeroAbilitySystem();
			HUD = CreateWidget<UClassicHUDWidget>(Controller, LiveHUD->GetClass());
			SlateHUD = HUD->TakeWidget();
		}

		~FClassicFixture()
		{
			SlateHUD.Reset();
			HUD->ReleaseSlateResources(true);
			Player->PlayerController = nullptr;
			Player->RemoveFromRoot();
			auto* Context = World->GetSubsystem<UExperienceSubsystem>();
			FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("RulesDef"))->SetObjectPropertyValue_InContainer(Context, nullptr);
			const_cast<FExperienceSnapshot&>(Context->GetSnapshot()).State = EExperienceState::Unloaded;
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}

		bool ReplaceHero(bool bNoResource)
		{
			auto* Definition = DuplicateObject<UHeroDefinition>(Hero->GetDefinition(), World);
			Definition->ResourceType = bNoResource ? EHeroResourceType::None : EHeroResourceType::Mana;
			Definition->Attributes.MaxMana = bNoResource ? FHeroAttributeGrowthValue{0, 0} : FHeroAttributeGrowthValue{80, 5};
			Definition->Attributes.ManaRegeneration = {0, 0};
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			auto* Replacement = World->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), FVector(300, 0, 100), FRotator::ZeroRotator, Spawn);
			if (!Replacement->InitializeHero(Definition, Participant))
			{
				return false;
			}

			Hero = Replacement;
			Abilities = Hero->GetHeroAbilitySystem();
			Participant->PrimaryHero = Hero;
			Controller->Possess(Hero);
			Controller->ServerPlayerReady(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision);
			return true;
		}

		void Inventory(int32 Quantity, FName ItemId)
		{
			auto* Inventory = Hero->GetInventory();
			auto& Snapshot = *FindFProperty<FStructProperty>(Inventory->GetClass(), TEXT("Snapshot"))->ContainerPtrToValuePtr<FItemInventorySnapshot>(Inventory);
			auto& Economy = *FindFProperty<FStructProperty>(Participant->GetClass(), TEXT("Economy"))->ContainerPtrToValuePtr<FParticipantEconomy>(Participant);
			Snapshot.Items.Reset();
			if (Quantity > 0)
			{
				FItemInstance Item;
				Item.Id = FGuid::NewGuid();
				Item.ItemId = ItemId;
				Item.Quantity = Quantity;
				Item.Slot = 5;
				Snapshot.Items.Add(Item);
			}

			++Snapshot.Revision;
			Snapshot.AbilityStateRevision = Abilities->GetStateRevision();
			Economy.Revision = Snapshot.Revision;
			Inventory->ProcessEvent(Inventory->FindFunctionChecked(TEXT("OnRep_Snapshot")), nullptr);
			Participant->ProcessEvent(Participant->FindFunctionChecked(TEXT("OnRep_Economy")), nullptr);
		}

		UWorld* World = nullptr;
		ULocalPlayer* Player = nullptr;
		AHeroPlayerController* Controller = nullptr;
		AParticipantState* Participant = nullptr;
		AHeroCharacter* Hero = nullptr;
		UHeroAbilitySystemComponent* Abilities = nullptr;
		UMatchRulesDefinition* Rules = nullptr;
		UClassicHUDWidget* HUD = nullptr;
		TSharedPtr<SWidget> SlateHUD;
		bool bInitialized = false;
	};

	class FClassicLifecycle final : public IAutomationLatentCommand
	{
	public:
		explicit FClassicLifecycle(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
		{
		}

		virtual bool Update() override
		{
			if (!Fixture)
			{
				auto* Live = FindLiveHUD();
				if (!Live || !Live->GetPresentedStatus().bInventoryReady)
				{
					if (FPlatformTime::Seconds() - Started < 15)
					{
						return false;
					}

					Test->AddError(TEXT("Lifecycle 需要当前 Classic 单玩家 PIE 已就绪"));
					return true;
				}

				Fixture = MakeUnique<FClassicFixture>(Live);
				Test->TestTrue(TEXT("瞬态 Hero 初始化"), Fixture->bInitialized);
			}

			auto& FixtureState = *Fixture;
			FixtureState.World->Tick(LEVELTICK_All, .05f);
			const auto& State = FixtureState.HUD->GetPresentedStatus();
			switch (Phase++)
			{
			case 0:
				Test->TestTrue(TEXT("初始完整刷新"), State.bCombatReady && State.bAbilitiesReady && State.bInventoryReady);
				FixtureState.Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetMaxHealthAttribute(), 200);
				FixtureState.Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 150);
				FixtureState.Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetMaxManaAttribute(), 1);
				FixtureState.Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetManaAttribute(), 0);
				return false;
			case 1:
				Test->TestEqual(TEXT("当前值事件刷新"), State.Health, 150.f);
				Test->TestEqual(TEXT("上限事件刷新"), State.MaxHealth, 200.f);
				Test->TestEqual(TEXT("空Mana显示为零比例"), Control<UProgressBar>(FixtureState.HUD, TEXT("ManaBar"))->GetPercent(), 0.f);
				FixtureState.Inventory(3, TEXT("FixtureItem"));
				return false;
			case 2:
				Test->TestEqual(TEXT("添加第六槽"), State.Items[5].Quantity, 3);
				FixtureState.Inventory(2, TEXT("FixtureItem"));
				return false;
			case 3:
				Test->TestEqual(TEXT("数量变化"), State.Items[5].Quantity, 2);
				FixtureState.Inventory(1, TEXT("Replacement"));
				return false;
			case 4:
				Test->TestEqual(TEXT("替换槽位"), State.Items[5].ItemId, FName(TEXT("Replacement")));
				FixtureState.Inventory(0, NAME_None);
				return false;
			case 5:
				Test->TestFalse(TEXT("移除后清空"), State.Items[5].Instance.IsValid());
				for (const auto& Row : State.Abilities)
				{
					const auto* Spec = FixtureState.Abilities->FindAbilitySpecFromHandle(Row.Handle);
					const auto* Ability = Spec ? Spec->Ability.Get() : nullptr;
					const auto* Cooldown = Ability ? Ability->GetCooldownGameplayEffect() : nullptr;
					if (Cooldown)
					{
						CooldownClass = Cooldown->GetClass();
						auto Effect = FixtureState.Abilities->MakeOutgoingSpec(Cooldown->GetClass(), 1, FixtureState.Abilities->MakeEffectContext());
						Effect.Data->SetDuration(3, true);
						ActiveEffect = FixtureState.Abilities->ApplyGameplayEffectSpecToSelf(*Effect.Data);
						break;
					}
				}

				Test->TestTrue(TEXT("真实冷却 GE 已应用"), ActiveEffect.IsValid());
				return false;
			case 6:
				Test->TestTrue(TEXT("冷却开始显示"), State.Abilities.ContainsByPredicate([](const auto& Row) { return Row.bCooldownKnown && Row.CooldownRemaining > 0; }));
				FixtureState.Controller->UnPossess();
				return false;
			case 7:
				FixtureState.Controller->Possess(FixtureState.World->SpawnActor<APawn>());
				return false;
			case 8:
			case 9:
				return false;
			case 10:
				Test->TestFalse(TEXT("解绑及非 Hero Pawn 安全空态"), State.bOwnedHero);
				++FixtureState.Participant->ConnectionEpoch;
				++FixtureState.Hero->ConnectionEpoch;
				FixtureState.Controller->Possess(FixtureState.Hero);
				return false;
			case 11:
			case 12:
			case 13:
				return false;
			case 14:
				Test->TestTrue(TEXT("中途重绑恢复当前冷却"), State.bAbilitiesReady && State.Abilities.ContainsByPredicate([](const auto& Row) { return Row.CooldownRemaining > 0; }));
				FixtureState.Abilities->RemoveActiveGameplayEffect(ActiveEffect);
				return false;
			case 15:
				Test->TestFalse(TEXT("提前结束清除冷却"), State.Abilities.ContainsByPredicate([](const auto& Row) { return Row.CooldownRemaining > 0; }));
				FixtureState.Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 0);
				FixtureState.Hero->EnterDeath(FixtureState.Abilities->MakeEffectContext());
				return false;
			case 16:
			case 17:
			case 18:
				return false;
			case 19:
				Test->TestTrue(TEXT("原 Hero 复活刷新"), State.bCombatReady && State.LifeState == EHeroLifeState::Alive && State.Health > 0);
				if (CooldownClass)
				{
					auto Effect = FixtureState.Abilities->MakeOutgoingSpec(CooldownClass, 1, FixtureState.Abilities->MakeEffectContext());
					Effect.Data->SetDuration(.1f, true);
					FixtureState.Abilities->ApplyGameplayEffectSpecToSelf(*Effect.Data);
				}

				return false;
			case 20:
			case 21:
			case 22:
				return false;
			case 23:
				Test->TestFalse(TEXT("自然到期清除冷却"), State.Abilities.ContainsByPredicate([](const auto& Row) { return Row.CooldownRemaining > 0; }));
				FixtureState.SlateHUD.Reset();
				FixtureState.HUD->ReleaseSlateResources(true);
				Test->TestFalse(TEXT("销毁移除属性回调"), FixtureState.Abilities->GetGameplayAttributeValueChangeDelegate(UHeroAttributeSet::GetHealthAttribute()).IsBoundToObject(FixtureState.HUD));
				FixtureState.Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 120);
				FixtureState.Inventory(1, TEXT("AfterDestruct"));
				return false;
			default:
				Test->TestFalse(TEXT("销毁后没有数据回调残留"), FixtureState.HUD->GetPresentedStatus().bOwnedHero);
				return true;
			}
		}

	private:
		FAutomationTestBase* Test;
		double Started;
		TUniquePtr<FClassicFixture> Fixture;
		UClass* CooldownClass = nullptr;
		FActiveGameplayEffectHandle ActiveEffect;
		int32 Phase = 0;
	};

	/** 使用实际WBP类，只在隔离World提交经验或更换拥有Hero；不直接调用HUD刷新来替代事件证据。 */
	class FClassicProgression final : public IAutomationLatentCommand
	{
	public:
		FClassicProgression(FAutomationTestBase* InTest, bool bInNoResource) : Test(InTest), Started(FPlatformTime::Seconds()), bNoResource(bInNoResource)
		{
		}

		~FClassicProgression() override
		{
			Fixture.Reset();
			if (GEditor && GEditor->PlayWorld)
			{
				GEditor->RequestEndPlayMap();
			}
		}

		virtual bool Update() override
		{
			if (!Fixture)
			{
				auto* Live = FindLiveHUD();
				if (!Live || !Live->GetPresentedStatus().bProgressionReady)
				{
					if (FPlatformTime::Seconds() - Started < 20)
					{
						return false;
					}

					Test->AddError(TEXT("属性HUD测试未在有界时间内取得当前已就绪Classic WBP。"));
					return true;
				}

				Fixture = MakeUnique<FClassicFixture>(Live, bNoResource);
				if (!Test->TestTrue(TEXT("属性HUD瞬态Hero真实InitializeHero成功"), Fixture->bInitialized))
				{
					return true;
				}
			}

			Fixture->World->Tick(LEVELTICK_All, .05f);
			return bNoResource ? UpdateNoResource() : UpdateProgression();
		}


	private:
		bool UpdateProgression()
		{
			const auto& State = Fixture->HUD->GetPresentedStatus();
			switch (Phase++)
			{
			case 0:
				Test->TestTrue(TEXT("构造前已经初始化的数据立即完整显示"), State.bProgressionReady && State.bCombatReady && State.bUsesMana);
				Test->TestEqual(TEXT("初始真实英雄等级"), State.HeroLevel, 1);
				Test->TestEqual(TEXT("门槛表给出最大等级"), State.MaximumHeroLevel, 4);
				Test->TestEqual(TEXT("初始累计经验"), State.TotalExperiencePoints, int64(0));
				Test->TestEqual(TEXT("初级所需经验"), State.RequiredExperiencePoints, int64(100));
				Test->TestEqual(TEXT("初级经验条为零"), Control<UProgressBar>(Fixture->HUD, TEXT("ExperienceBar"))->GetPercent(), 0.f);
				Test->TestTrue(TEXT("真实等级文字已绑定"), Control<UTextBlock>(Fixture->HUD, TEXT("LevelText"))->GetText().ToString().Contains(TEXT("1")));
				Fixture->Hero->GetAttributeComponent()->AddExperience(50);
				return false;
			case 1:
				Test->TestEqual(TEXT("仅XP改变也通过组件事件刷新"), State.TotalExperiencePoints, int64(50));
				Test->TestEqual(TEXT("仅XP变化时等级保持一级"), State.HeroLevel, 1);
				Test->TestEqual(TEXT("本级相对经验50"), State.LevelExperiencePoints, int64(50));
				Test->TestEqual(TEXT("仅XP事件刷新真实经验条"), Control<UProgressBar>(Fixture->HUD, TEXT("ExperienceBar"))->GetPercent(), .5f);
				Test->TestTrue(TEXT("经验文字读当前级内进度"), Control<UTextBlock>(Fixture->HUD, TEXT("ExperienceText"))->GetText().ToString().Contains(TEXT("50")));
				Fixture->Hero->GetAttributeComponent()->AddExperience(300);
				return false;
			case 2:
				Test->TestTrue(TEXT("跨级后Level与XP已收敛"), State.bProgressionReady);
				Test->TestEqual(TEXT("总350显示三级"), State.HeroLevel, 3);
				Test->TestEqual(TEXT("跨级累计经验350"), State.TotalExperiencePoints, int64(350));
				Test->TestEqual(TEXT("三级内进度50"), State.LevelExperiencePoints, int64(50));
				Test->TestEqual(TEXT("三级到四级所需300"), State.RequiredExperiencePoints, int64(300));
				Test->TestTrue(TEXT("跨级经验条使用当前门槛区间"), FMath::IsNearlyEqual(Control<UProgressBar>(Fixture->HUD, TEXT("ExperienceBar"))->GetPercent(), 1.f / 6));
				Test->TestTrue(TEXT("等级文字更新为三级"), Control<UTextBlock>(Fixture->HUD, TEXT("LevelText"))->GetText().ToString().Contains(TEXT("3")));
				Fixture->Hero->GetAttributeComponent()->AddExperience(MAX_int64);
				return false;
			default:
				Test->TestTrue(TEXT("满级就绪且明确标记"), State.bProgressionReady && State.bMaximumLevel);
				Test->TestEqual(TEXT("满级为四级"), State.HeroLevel, 4);
				Test->TestEqual(TEXT("满级累计经验封顶600"), State.TotalExperiencePoints, int64(600));
				Test->TestEqual(TEXT("满级无下一门槛"), State.RequiredExperiencePoints, int64(0));
				Test->TestEqual(TEXT("满级不除零且显示满条"), Control<UProgressBar>(Fixture->HUD, TEXT("ExperienceBar"))->GetPercent(), 1.f);
				Test->TestTrue(TEXT("真实经验文字明确显示满级"), Control<UTextBlock>(Fixture->HUD, TEXT("ExperienceText"))->GetText().ToString().Contains(TEXT("满级")));
				return true;
			}
		}

		bool UpdateNoResource()
		{
			const auto& State = Fixture->HUD->GetPresentedStatus();
			switch (Phase++)
			{
			case 0:
				Test->TestTrue(TEXT("None配置首次展示已就绪"), State.bProgressionReady && State.bCombatReady && !State.bUsesMana);
				Test->TestEqual(TEXT("None隐藏真实Mana条"), Control<UProgressBar>(Fixture->HUD, TEXT("ManaBar"))->GetVisibility(), ESlateVisibility::Collapsed);
				Test->TestEqual(TEXT("None隐藏真实Mana文字"), Control<UTextBlock>(Fixture->HUD, TEXT("ManaText"))->GetVisibility(), ESlateVisibility::Collapsed);
				Test->TestEqual(TEXT("None Mana条安全零比例"), Control<UProgressBar>(Fixture->HUD, TEXT("ManaBar"))->GetPercent(), 0.f);
				Test->TestEqual(TEXT("None正式Mana为零"), Fixture->Hero->GetAttributes()->GetMana(), 0.f);
				Test->TestEqual(TEXT("None正式Mana上限为零"), Fixture->Hero->GetAttributes()->GetMaxMana(), 0.f);
				Test->TestEqual(TEXT("None正式Mana恢复为零"), Fixture->Hero->GetAttributes()->GetManaRegeneration(), 0.f);
				Test->TestTrue(TEXT("属性浮层明确无资源"), Control<UTextBlock>(Fixture->HUD, TEXT("AttributesText"))->GetText().ToString().Contains(TEXT("资源  无")));
				Fixture->Controller->ServerPlayerReady(Fixture->Hero->EntityId, Fixture->Hero->ConnectionEpoch, Fixture->Hero->RequiredLoadoutRevision);
				Test->TestTrue(TEXT("None Hero已通过真实PlayerReady而非输入未就绪拒绝"), Fixture->Hero->CanAcceptPlayerInput());
				Test->TestTrue(TEXT("None配置仍授予现有正成本主槽"), Fixture->Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary).IsValid());
				Test->TestFalse(TEXT("None正Mana成本不能免费激活"), Fixture->Abilities->TryActivateAbility(Fixture->Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary)));
				Test->TestEqual(TEXT("拒绝正成本后资源仍零"), Fixture->Hero->GetAttributes()->GetMana(), 0.f);
				Fixture->Hero->GetAttributeComponent()->AddExperience(50);
				return false;
			case 1:
				Test->TestEqual(TEXT("None也显示真实整数经验"), State.TotalExperiencePoints, int64(50));
				OldHero = Fixture->Hero;
				OldAbilities = Fixture->Abilities;
				if (!Test->TestTrue(TEXT("用现有InitializeHero替换为Mana Hero"), Fixture->ReplaceHero(false)))
				{
					return true;
				}

				return false;
			case 2:
			case 3:
			case 4:
			case 5:
				return false;
			case 6:
				Test->TestEqual(TEXT("同HUD已改绑新Hero实体"), State.Entity, Fixture->Hero->EntityId);
				Test->TestTrue(TEXT("新Mana Hero完整刷新"), State.bProgressionReady && State.bUsesMana && State.bCombatReady);
				Test->TestEqual(TEXT("改绑后读新组件零经验"), State.TotalExperiencePoints, int64(0));
				Test->TestTrue(TEXT("改绑后Mana条重新可见"), Control<UProgressBar>(Fixture->HUD, TEXT("ManaBar"))->GetVisibility() != ESlateVisibility::Collapsed);
				Test->TestFalse(TEXT("旧ASC等级委托已解绑"), OldAbilities->GetGameplayAttributeValueChangeDelegate(UHeroAttributeSet::GetHeroLevelAttribute()).IsBoundToObject(Fixture->HUD));
				OldHero->GetAttributeComponent()->AddExperience(300);
				OldAbilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 70);
				Fixture->Hero->GetAttributeComponent()->AddExperience(25);
				return false;
			case 7:
				Test->TestEqual(TEXT("新组件事件更新25经验"), State.TotalExperiencePoints, int64(25));
				Test->TestEqual(TEXT("旧组件350经验不污染新等级"), State.HeroLevel, 1);
				Test->TestEqual(TEXT("旧组件仍独立保存350经验"), OldHero->GetAttributeComponent()->GetTotalExperiencePoints(), int64(350));
				Fixture->SlateHUD.Reset();
				Fixture->HUD->ReleaseSlateResources(true);
				Test->TestFalse(TEXT("Destruct移除新ASC等级回调"), Fixture->Abilities->GetGameplayAttributeValueChangeDelegate(UHeroAttributeSet::GetHeroLevelAttribute()).IsBoundToObject(Fixture->HUD));
				Fixture->Hero->GetAttributeComponent()->AddExperience(25);
				OldHero->GetAttributeComponent()->AddExperience(25);
				return false;
			default:
				Test->TestFalse(TEXT("销毁后新旧组件通知不恢复拥有展示"), State.bOwnedHero);
				Test->TestFalse(TEXT("销毁后经验保持安全空态"), State.bProgressionReady);
				return true;
			}
		}


	private:
		FAutomationTestBase* Test;
		double Started;
		TUniquePtr<FClassicFixture> Fixture;
		AHeroCharacter* OldHero = nullptr;
		UHeroAbilitySystemComponent* OldAbilities = nullptr;
		int32 Phase = 0;
		bool bNoResource = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicHUDCaptureTest, "Moba.UI.ClassicHUD.LiveCapture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicHUDCaptureTest::RunTest(const FString& Parameters)
{
	FRequestPlaySessionParams Params;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	GEditor->RequestPlaySession(Params);
	ADD_LATENT_AUTOMATION_COMMAND(FClassicCapture(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicHUDLifecycleTest, "Moba.UI.ClassicHUD.TransientLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicHUDLifecycleTest::RunTest(const FString& Parameters)
{
	FRequestPlaySessionParams Params;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	GEditor->RequestPlaySession(Params);
	ADD_LATENT_AUTOMATION_COMMAND(FClassicLifecycle(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicHUDProgressionTest, "Moba.UI.ClassicHUD.ProgressionAndNone.ProgressionEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicHUDProgressionTest::RunTest(const FString& Parameters)
{
	FRequestPlaySessionParams Params;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	GEditor->RequestPlaySession(Params);
	ADD_LATENT_AUTOMATION_COMMAND(FClassicProgression(this, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicHUDNoResourceTest, "Moba.UI.ClassicHUD.ProgressionAndNone.NoResourceAndRebind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicHUDNoResourceTest::RunTest(const FString& Parameters)
{
	FRequestPlaySessionParams Params;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	GEditor->RequestPlaySession(Params);
	ADD_LATENT_AUTOMATION_COMMAND(FClassicProgression(this, true));
	return true;
}

#endif
