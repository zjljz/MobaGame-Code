#include "Status/StatusPanelSnapshot.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "AbilitySystem/HeroGameplayEffects.h"
#include "AbilitySystem/SelfCommitAbility.h"
#include "ClientPresentationSubsystem.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Experience/ExperienceSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/WorldSettings.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "Items/ItemInventoryComponent.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MobaGameplayTags.h"
#include "Match/MatchGameState.h"
#include "Match/ParticipantState.h"
#include "RenderingThread.h"
#include "Slate/WidgetRenderer.h"
#include "Status/StatusPanelWidget.h"
#include "Subsystems/SubsystemCollection.h"
#include "TextureResource.h"
#include "UObject/UnrealType.h"

namespace
{
	template<typename PropertyType>
	PropertyType* FindFixturePropertyChecked(const UStruct* Owner, FName Name)
	{
		PropertyType* Property = FindFProperty<PropertyType>(Owner, Name);
		checkf(Property, TEXT("Status fixture property missing: %s"), *Name.ToString());
		return Property;
	}

	class FStatusFixture
	{
	public:
		FStatusFixture()
		{
			const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
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
			LocalPlayer = NewObject<ULocalPlayer>(GEngine);
			LocalPlayer->AddToRoot();
			Controller->Player = LocalPlayer;
			LocalPlayer->PlayerController = Controller;
			Controller->SetPlayerState(Participant);
			Participant->SetOwner(Controller);

			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Hero = World->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
			Set = NewObject<UAbilitySetDefinition>(World);
			FAbilityGrantDefinition& Grant = Set->Abilities.AddDefaulted_GetRef();
			Grant.Slot = MobaGameplayTags::Input_Slot_Primary;
			Grant.Ability = USelfCommitAbility::StaticClass();
			Grant.MaxLevel = 5;
			auto* Definition = NewObject<UHeroDefinition>(World);
			Definition->AbilitySet = Set;
			Definition->RespawnDelay = .05f;
			bInitialized = Hero->InitializeHero(Definition, Participant);
			Participant->PrimaryHero = Hero;
			Controller->Possess(Hero);
			Controller->AcceptContentReady();
			Abilities = Hero->GetHeroAbilitySystem();
			Experience = NewObject<UExperienceDefinition>(World);
			Experience->bItemsEnabled = true;
			Experience->InitialGold = 1000;
			Participant->InitializeEconomy(1000);
			// 瞬态World只注入本测试的只读就绪配置，不启动Feature/资产加载。
			auto* Context = World->GetSubsystem<UExperienceSubsystem>();
			FindFixturePropertyChecked<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("Experience"))->SetObjectPropertyValue_InContainer(Context, Experience);
			// Snapshot不是反射属性；仅此fixture拥有的非const对象通过现有访问器模拟就绪。
			auto& State = const_cast<FExperienceSnapshot&>(Context->GetSnapshot());
			State.State = EExperienceState::ExperienceReady;
		}

		~FStatusFixture()
		{
			auto* Context = World->GetSubsystem<UExperienceSubsystem>();
			FindFixturePropertyChecked<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("Experience"))->SetObjectPropertyValue_InContainer(Context, nullptr);
			const_cast<FExperienceSnapshot&>(Context->GetSnapshot()).State = EExperienceState::Unloaded;
			LocalPlayer->PlayerController = nullptr;
			LocalPlayer->RemoveFromRoot();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}

		FStatusPanelSnapshot Read() const { return MobaStatus::ReadOwnedStatus(Controller, Experience); }

		// 只在当前瞬态 fixture 模拟不同复制批次，不更改核心访问级别或 CDO。
		FItemInventorySnapshot& Inventory() const
		{
			const auto* Property = FindFixturePropertyChecked<FStructProperty>(UItemInventoryComponent::StaticClass(), TEXT("Snapshot"));
			return *Property->ContainerPtrToValuePtr<FItemInventorySnapshot>(Hero->GetInventory());
		}

		FParticipantEconomy& Economy() const
		{
			const auto* Property = FindFixturePropertyChecked<FStructProperty>(AParticipantState::StaticClass(), TEXT("Economy"));
			return *Property->ContainerPtrToValuePtr<FParticipantEconomy>(Participant);
		}

		UWorld* World = nullptr;
		ULocalPlayer* LocalPlayer = nullptr;
		AHeroPlayerController* Controller = nullptr;
		AParticipantState* Participant = nullptr;
		AHeroCharacter* Hero = nullptr;
		UHeroAbilitySystemComponent* Abilities = nullptr;
		UAbilitySetDefinition* Set = nullptr;
		UExperienceDefinition* Experience = nullptr;
		bool bInitialized = false;
	};

	class FStatusRespawnCheck final : public IAutomationLatentCommand
	{
	public:
		FStatusRespawnCheck(FAutomationTestBase* InTest, TSharedRef<FStatusFixture> InFixture, uint32 InGeneration)
			: Test(InTest), Fixture(InFixture), Generation(InGeneration), Started(FPlatformTime::Seconds())
		{
		}

		virtual bool Update() override
		{
			// 每次Update位于新的引擎帧，保留TimerManager原生逐帧门槛，不修改GFrameCounter。
			Fixture->World->Tick(LEVELTICK_All, .02f);
			const auto State = Fixture->Read();
			if (!(State.bCombatReady && State.Health > 0 && State.LifeGeneration > Generation))
			{
				if (FPlatformTime::Seconds() - Started > 2)
				{
					Test->AddError(FString::Printf(TEXT("原实体复活当前快照未就绪: Ready=%d HP=%.2f Generation=%u Previous=%u"),
						State.bCombatReady, State.Health, State.LifeGeneration, Generation));
					return true;
				}

				return false;
			}

			Test->TestTrue(TEXT("原实体复活当前快照"), State.bCombatReady && State.Health > 0 && State.LifeGeneration > Generation);
			Fixture->Abilities->RemoveManagedSource(Fixture->Hero->EntityId, Fixture->Abilities->GetStateRevision());
			const auto EmptyState = Fixture->Read();
			Test->TestTrue(TEXT("合法空槽已就绪"), EmptyState.bAbilitiesReady);
			Test->TestTrue(TEXT("撤源不保留旧能力"), EmptyState.Abilities.IsEmpty());
			return true;
		}


	private:
		FAutomationTestBase* Test;
		TSharedRef<FStatusFixture> Fixture;
		uint32 Generation;
		double Started;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStatusOwnershipTest, "Moba.UI.Status.OwnershipAndRevisions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStatusOwnershipTest::RunTest(const FString& Parameters)
{
	FStatusFixture Fixture;
	TestTrue(TEXT("Hero初始化"), Fixture.bInitialized);
	auto State = Fixture.Read();
	TestTrue(TEXT("真实拥有端身份通过"), State.bOwnedHero);
	TestTrue(TEXT("属性可读"), State.bCombatReady);
	TestTrue(TEXT("真实Spec与Loadout一致"), State.bAbilitiesReady);
	TestTrue(TEXT("初始空库存已就绪"), State.bInventoryReady);
	TestEqual(TEXT("初始金币"), State.Gold, 1000);
	Fixture.Economy().Gold = 0;
	TestFalse(TEXT("初始金币默认值不能伪装已同步"), Fixture.Read().bInventoryReady);
	Fixture.Economy().Gold = 1000;

	FItemInstance Item;
	Item.Id = FGuid::NewGuid();
	Item.ItemId = TEXT("FixtureItem");
	Item.Slot = 5;
	Item.Quantity = 3;
	Fixture.Inventory().Items.Add(Item);
	Fixture.Inventory().Revision = 1;
	Fixture.Inventory().AbilityStateRevision = Fixture.Abilities->GetStateRevision();
	State = Fixture.Read();
	TestFalse(TEXT("库存先到不混入旧金币"), State.bInventoryReady);
	TestFalse(TEXT("等待时不泄露旧槽"), State.Items[5].Instance.IsValid());
	Fixture.Economy().Revision = 1;
	Fixture.Economy().Gold = 900;
	State = Fixture.Read();
	TestTrue(TEXT("相同revision完整显示"), State.bInventoryReady);
	TestEqual(TEXT("第六槽数量"), State.Items[5].Quantity, 3);
	TestEqual(TEXT("对应金币"), State.Gold, 900);
	++Fixture.Economy().Revision;
	TestFalse(TEXT("金币先到同样等待"), Fixture.Read().bInventoryReady);
	--Fixture.Economy().Revision;
	++Fixture.Inventory().AbilityStateRevision;
	TestFalse(TEXT("ASC落后库存依赖时等待"), Fixture.Read().bInventoryReady);
	--Fixture.Inventory().AbilityStateRevision;
	const auto Handle = Fixture.Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary);
	TestTrue(TEXT("真实升级提交"), Fixture.Abilities->UpgradeManagedAbility(Handle, 2, Fixture.Abilities->GetStateRevision()).IsSuccess());
	TestTrue(TEXT("独立技能版本前进不永久阻塞库存"), Fixture.Read().bInventoryReady);
	Fixture.Inventory().Items.Add(Item);
	TestFalse(TEXT("重复实例/槽拒绝显示"), Fixture.Read().bInventoryReady);
	Fixture.Inventory().Items.Pop();

	Fixture.Participant->SetOwner(nullptr);
	State = Fixture.Read();
	TestFalse(TEXT("失去Participant拥有关系即清空"), State.bOwnedHero);
	TestEqual(TEXT("旧金币不残留"), State.Gold, 0);
	TestTrue(TEXT("旧技能不残留"), State.Abilities.IsEmpty());
	Fixture.Participant->SetOwner(Fixture.Controller);
	++Fixture.Participant->ConnectionEpoch;
	TestFalse(TEXT("旧Hero epoch不复用"), Fixture.Read().bOwnedHero);
	++Fixture.Hero->ConnectionEpoch;
	TestTrue(TEXT("新epoch双方一致可重读当前状态"), Fixture.Read().bInventoryReady);

	auto* Observer = Fixture.World->SpawnActor<AHeroPlayerController>();
	Observer->SetActorTickEnabled(false);
	Observer->SetAsLocalPlayerController();
	auto* ObserverPlayer = NewObject<ULocalPlayer>(GEngine);
	Observer->Player = ObserverPlayer;
	ObserverPlayer->PlayerController = Observer;
	Observer->SetViewTarget(Fixture.Hero);
	const auto Other = MobaStatus::ReadOwnedStatus(Observer, Fixture.Experience);
	TestFalse(TEXT("观察ViewTarget不能读取私有库存"), Other.bInventoryReady);
	TestFalse(TEXT("观察者不取得拥有端身份"), Other.bOwnedHero);
	Fixture.Controller->UnPossess();
	TestFalse(TEXT("解绑立即清空"), Fixture.Read().bOwnedHero);
	Fixture.Controller->Possess(Fixture.Hero);
	TestTrue(TEXT("重新绑定恢复当前快照"), Fixture.Read().bInventoryReady);
	Fixture.Experience->bItemsEnabled = false;
	Fixture.Inventory().Items.Reset();
	State = Fixture.Read();
	TestTrue(TEXT("NoItems仍显示实际金币"), State.bInventoryReady);
	TestFalse(TEXT("NoItems明确未启用"), State.bItemsEnabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStatusLifeAbilityTest, "Moba.UI.Status.LifeSlotsAndCooldown", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStatusLifeAbilityTest::RunTest(const FString& Parameters)
{
	TSharedRef<FStatusFixture> SharedFixture = MakeShared<FStatusFixture>();
	FStatusFixture& Fixture = SharedFixture.Get();
	if (!TestTrue(TEXT("Hero初始化"), Fixture.bInitialized) || !TestEqual(TEXT("完整初始槽"), Fixture.Read().Abilities.Num(), 1))
	{
		return false;
	}

	const auto Handle = Fixture.Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary);
	const FGuid ReasonOwner = FGuid::NewGuid();
	TestTrue(TEXT("真实禁用"), Fixture.Abilities->SetSourceDisabled(Fixture.Hero->EntityId, ReasonOwner, TEXT("Fixture"), true, Fixture.Abilities->GetStateRevision()).IsSuccess());
	auto State = Fixture.Read();
	if (!TestEqual(TEXT("一个真实槽"), State.Abilities.Num(), 1))
	{
		return false;
	}

	TestFalse(TEXT("显示来源禁用"), State.Abilities[0].bEnabled);
	Fixture.Abilities->SetSourceDisabled(Fixture.Hero->EntityId, ReasonOwner, TEXT("Fixture"), false, Fixture.Abilities->GetStateRevision());
	Fixture.Abilities->UpgradeManagedAbility(Handle, 3, Fixture.Abilities->GetStateRevision());
	TestEqual(TEXT("显示State匹配的真实Spec等级"), Fixture.Read().Abilities[0].Level, 3);
	Fixture.Abilities->RemapSlots(MobaGameplayTags::Input_Slot_Primary, MobaGameplayTags::Input_Slot_Target, Fixture.Abilities->GetStateRevision());
	TestEqual(TEXT("动态槽不残留旧键"), Fixture.Read().Abilities[0].Slot, MobaGameplayTags::Input_Slot_Target.GetTag());
	++Fixture.Hero->RequiredLoadoutRevision;
	State = Fixture.Read();
	TestFalse(TEXT("槽版本未齐等待"), State.bAbilitiesReady);
	TestTrue(TEXT("等待时旧槽清空"), State.Abilities.IsEmpty());
	--Fixture.Hero->RequiredLoadoutRevision;
	FGameplayAbilitySpec* Spec = Fixture.Abilities->FindAbilitySpecFromHandle(Handle);
	if (!TestNotNull(TEXT("当前Spec仍存在"), Spec))
	{
		return false;
	}

	++Spec->Level;
	TestFalse(TEXT("Spec等级与私有States不一致等待"), Fixture.Read().bAbilitiesReady);
	--Spec->Level;

	UGameplayAbility* Ability = Spec->GetPrimaryInstance();
	if (!TestNotNull(TEXT("真实能力实例"), Ability))
	{
		return false;
	}

	const auto* CooldownProperty = FindFixturePropertyChecked<FClassProperty>(UGameplayAbility::StaticClass(), TEXT("CooldownGameplayEffectClass"));
	CooldownProperty->SetObjectPropertyValue_InContainer(Ability, USelfCooldownEffect::StaticClass());
	auto Effect = Fixture.Abilities->MakeOutgoingSpec(USelfCooldownEffect::StaticClass(), 1, Fixture.Abilities->MakeEffectContext());
	Effect.Data->SetDuration(5, true);
	const auto Active = Fixture.Abilities->ApplyGameplayEffectSpecToSelf(*Effect.Data.Get());
	State = Fixture.Read();
	TestTrue(TEXT("读取真实GAS冷却"), State.Abilities[0].bCooldownKnown && State.Abilities[0].CooldownRemaining > 4.5f);
	const float Before = State.Abilities[0].CooldownRemaining;
	Fixture.World->Tick(LEVELTICK_All, .25f);
	TestTrue(TEXT("冷却来自World/GE实际时钟"), Fixture.Read().Abilities[0].CooldownRemaining < Before);
	Fixture.Abilities->RemoveActiveGameplayEffect(Active);
	TestEqual(TEXT("GE移除后冷却清零"), Fixture.Read().Abilities[0].CooldownRemaining, 0.f);
	Fixture.Abilities->AddLooseGameplayTag(MobaGameplayTags::Cooldown_Self);
	TestFalse(TEXT("只有冷却标签而无GE时间时显示同步中"), Fixture.Read().Abilities[0].bCooldownKnown);
	Fixture.Abilities->RemoveLooseGameplayTag(MobaGameplayTags::Cooldown_Self);

	Fixture.Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetShieldAttribute(), 30);
	TestEqual(TEXT("显示真实护盾"), Fixture.Read().Shield, 30.f);
	Fixture.Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 0);
	TestFalse(TEXT("Alive先收到0HP不拼生命状态"), Fixture.Read().bCombatReady);
	Fixture.Hero->EnterDeath(Fixture.Abilities->MakeEffectContext());
	State = Fixture.Read();
	TestTrue(TEXT("死亡仍可读当前属性"), State.bCombatReady);
	TestEqual(TEXT("死亡状态"), State.LifeState, EHeroLifeState::Dead);
	const uint32 Generation = State.LifeGeneration;
	ADD_LATENT_AUTOMATION_COMMAND(FStatusRespawnCheck(this, SharedFixture, Generation));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStatusRegistrationTest, "Moba.UI.Status.LayerRegistration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStatusRegistrationTest::RunTest(const FString& Parameters)
{
	ULocalPlayer* Player = NewObject<ULocalPlayer>(GEngine);
	UClientPresentationSubsystem* Presentation = NewObject<UClientPresentationSubsystem>(Player);
	Presentation->AddToRoot();
	FSubsystemCollection<ULocalPlayerSubsystem> Collection;
	Presentation->Initialize(Collection);
	TestTrue(TEXT("默认只读层注册"), Presentation->RegisterLayer(TEXT("Fixture"), UStatusPanelWidget::StaticClass(), 10));
	TestTrue(TEXT("同合同幂等"), Presentation->RegisterLayer(TEXT("Fixture"), UStatusPanelWidget::StaticClass(), 10, false));
	TestFalse(TEXT("同名不同命中策略冲突"), Presentation->RegisterLayer(TEXT("Fixture"), UStatusPanelWidget::StaticClass(), 10, true));
	TestFalse(TEXT("同名不同层级冲突"), Presentation->RegisterLayer(TEXT("Fixture"), UStatusPanelWidget::StaticClass(), 20));
	Presentation->Tick(0);
	TestNull(TEXT("无视口不假称已挂载"), Presentation->FindLayer(TEXT("Fixture")));
	TestTrue(TEXT("注销"), Presentation->UnregisterLayer(TEXT("Fixture")));
	TestTrue(TEXT("再次注销幂等"), Presentation->UnregisterLayer(TEXT("Fixture")));
	TestTrue(TEXT("注销后显式子控件命中可注册"), Presentation->RegisterLayer(TEXT("Fixture"), UStatusPanelWidget::StaticClass(), 10, true));
	Presentation->PlayerControllerChanged(nullptr);
	TestNull(TEXT("Controller缺失没有旧实例"), Presentation->FindLayer(TEXT("Fixture")));
	Presentation->Deinitialize();
	TestFalse(TEXT("Deinitialize后停止Tick"), Presentation->IsTickable());
	TestFalse(TEXT("Deinitialize后拒绝新注册"), Presentation->RegisterLayer(TEXT("Late"), UStatusPanelWidget::StaticClass()));
	Presentation->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStatusRenderingTest, "Moba.UI.Status.RenderedPanel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStatusRenderingTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
	{
		AddError(TEXT("RenderedPanel必须在启用RHI/Slate的Editor运行，不能用NullRHI替代"));
		return false;
	}

	FStatusFixture Fixture;
	UStatusPanelWidget* Widget = CreateWidget<UStatusPanelWidget>(Fixture.Controller, UStatusPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("原生Status Widget"), Widget))
	{
		return false;
	}

	Widget->AddToRoot();
	const TSharedRef<SWidget> SlateWidget = Widget->TakeWidget();
	Widget->PresentStatus(Fixture.Read());
	TestFalse(TEXT("Status不接受键盘焦点"), Widget->IsFocusable());
	FWidgetRenderer* Renderer = new FWidgetRenderer(true);
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>();
	Target->RenderTargetFormat = RTF_RGBA8;
	// 灰色场景底检查真实填充；透明目标会掩盖只有边框、内部漏底的问题。
	Target->ClearColor = FLinearColor(.55f, .55f, .55f, 1);
	Target->InitAutoFormat(960, 720);
	Target->UpdateResourceImmediate(true);
	Renderer->DrawWidget(Target, SlateWidget, FVector2D(960, 720), 0);
	bool bRendered = TestNotNull(TEXT("真实RHI RenderTarget"), Target);
	if (Target)
	{
		Target->AddToRoot();
		FlushRenderingCommands();
		TArray<FColor> Pixels;
		bRendered &= TestTrue(TEXT("GPU像素实际读回"), Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels));
		int32 GreenPixels = 0;
		int32 BluePixels = 0;
		int32 DarkBackgroundPixels = 0;
		int32 WhiteTextPixels = 0;
		for (const FColor Pixel : Pixels)
		{
			GreenPixels += Pixel.G > Pixel.R + 30 && Pixel.G > Pixel.B + 20 ? 1 : 0;
			BluePixels += Pixel.B > Pixel.R + 40 && Pixel.B > Pixel.G + 15 ? 1 : 0;
			DarkBackgroundPixels += Pixel.R < 80 && Pixel.G < 80 && Pixel.B < 90 && Pixel.A > 240 ? 1 : 0;
			WhiteTextPixels += Pixel.R > 210 && Pixel.G > 210 && Pixel.B > 210 && Pixel.A > 240 ? 1 : 0;
		}
		bRendered &= TestTrue(FString::Printf(TEXT("HP绿色条实际绘制: pixels=%d"), GreenPixels), GreenPixels > 100);
		bRendered &= TestTrue(FString::Printf(TEXT("Primary蓝色条实际绘制: pixels=%d"), BluePixels), BluePixels > 100);
		bRendered &= TestTrue(FString::Printf(TEXT("灰底上存在实心深色面板: pixels=%d"), DarkBackgroundPixels), DarkBackgroundPixels > 50000);
		bRendered &= TestTrue(FString::Printf(TEXT("明确白色正文实际绘制: pixels=%d"), WhiteTextPixels), WhiteTextPixels > 300);
		FString ArtifactPath;
		if (!FParse::Value(FCommandLine::Get(), TEXT("MobaStatusRenderOutput="), ArtifactPath))
		{
			ArtifactPath = FPaths::ProjectDir() / TEXT("AI/TASK-012/rendered");
		}
		ArtifactPath = FPaths::ConvertRelativePathToFull(ArtifactPath);
		UKismetRenderingLibrary::ExportRenderTarget(Fixture.World, Target, ArtifactPath, TEXT("StatusPanel.png"));
		TArray<uint8> ImageBytes;
		const uint8 PngSignature[] = {137, 80, 78, 71, 13, 10, 26, 10};
		const bool bPng = FFileHelper::LoadFileToArray(ImageBytes, *(ArtifactPath / TEXT("StatusPanel.png")))
			&& ImageBytes.Num() >= 8 && FMemory::Memcmp(ImageBytes.GetData(), PngSignature, 8) == 0;
		bRendered &= TestTrue(TEXT("导出内容为真实PNG而非改扩展名的EXR"), bPng);
		Target->RemoveFromRoot();
	}

	Widget->PresentStatus(FStatusPanelSnapshot());
	TestFalse(TEXT("失去上下文不残留金币区块"), Widget->GetPresentedStatus().bInventoryReady);
	TestTrue(TEXT("旧技能值已清空"), Widget->GetPresentedStatus().Abilities.IsEmpty());
	BeginCleanup(Renderer);
	Widget->ReleaseSlateResources(true);
	Widget->RemoveFromRoot();
	return bRendered;
}

#endif
