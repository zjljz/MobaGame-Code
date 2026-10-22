#include "Hero/HeroAttributeComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystem/AbilitySetDefinition.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "AbilitySystem/SelfCommitAbility.h"
#include "Combat/HeroAttributeSet.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Experience/ExperienceDefinitions.h"
#include "GameFramework/WorldSettings.h"
#include "GameplayEffect.h"
#include "Hero/HeroCharacter.h"
#include "Hero/HeroDefinition.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	/**
	 * 仅隔离组件合同；直接初始化组件，不生成参赛者或伪造 Hero 的完整初始化代次。
	 * 数值是机制夹具，既不读取开发内容资产，也不构成正式平衡配置。
	 */
	class FHeroAttributeFixture
	{
	public:
		FHeroAttributeFixture()
		{
			const auto WorldValues = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
				.CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &WorldValues);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Hero = World->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SpawnParameters);
			AbilitySystem = Hero->GetHeroAbilitySystem();
			Component = Hero->GetAttributeComponent();
			Definition.Reset(NewObject<UHeroDefinition>(World));
			AbilitySet.Reset(NewObject<UAbilitySetDefinition>(World));
			FAbilityGrantDefinition Grant;
			Grant.Ability = USelfCommitAbility::StaticClass();
			AbilitySet->Abilities.Add(Grant);
			Definition->AbilitySet = AbilitySet.Get();
			Definition->Attributes.MaxHealth = {100, 10};
			Definition->Attributes.MaxMana = {80, 5};
			Definition->Attributes.AttackDamage = {20, 2};
			Definition->Attributes.CriticalDamageMultiplier.BaseValue = 1.5f;
			Definition->Attributes.MoveSpeed.BaseValue = 600;
			Rules.Reset(NewObject<UMatchRulesDefinition>(World));
			Rules->AttributeRules.ExperienceThresholds = {0, 100, 300, 600};
			Rules->AttributeRules.MaximumCooldownReduction = .4f;
			Rules->AttributeRules.MinimumSlowMultiplier = .1f;
		}

		~FHeroAttributeFixture()
		{
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}


	public:
		bool Initialize(FAutomationTestBase* Test)
		{
			FString Reason;
			const bool bInitialized = Component->BeginInitialization(Definition.Get(), Rules.Get(), Reason);
			if (!Test->TestTrue(FString::Printf(TEXT("合法夹具开始初始化：%s"), *Reason), bInitialized))
			{
				return false;
			}

			return Test->TestTrue(TEXT("合法夹具完成初始化"), Component->FinishInitialization());
		}

		FActiveGameplayEffectHandle ApplyModifier(const FGameplayAttribute& Attribute, float Magnitude, float Duration = 0, EGameplayModOp::Type Operation = EGameplayModOp::AddBase)
		{
			auto* Effect = NewObject<UGameplayEffect>(World);
			Effect->DurationPolicy = Duration > 0 ? EGameplayEffectDurationType::HasDuration : EGameplayEffectDurationType::Infinite;
			Effect->DurationMagnitude = FScalableFloat(Duration);
			FGameplayModifierInfo Modifier;
			Modifier.Attribute = Attribute;
			Modifier.ModifierOp = Operation;
			Modifier.ModifierMagnitude = FScalableFloat(Magnitude);
			Effect->Modifiers.Add(Modifier);
			FGameplayEffectSpec Spec(Effect, AbilitySystem->MakeEffectContext(), 1);
			return AbilitySystem->ApplyGameplayEffectSpecToSelf(Spec);
		}


	public:
		UWorld* World = nullptr;
		AHeroCharacter* Hero = nullptr;
		UHeroAbilitySystemComponent* AbilitySystem = nullptr;
		UHeroAttributeComponent* Component = nullptr;
		TStrongObjectPtr<UHeroDefinition> Definition;
		TStrongObjectPtr<UAbilitySetDefinition> AbilitySet;
		TStrongObjectPtr<UMatchRulesDefinition> Rules;
	};

	bool TestValue(FAutomationTestBase* Test, const TCHAR* Label, float Actual, float Expected)
	{
		return Test->TestTrue(FString::Printf(TEXT("%s：实际 %.6f，预期 %.6f"), Label, Actual, Expected), FMath::IsNearlyEqual(Actual, Expected, .001f));
	}

	/**
	 * 每次 Update 只 Tick 一次，尊重 TimerManager 每个 GFrameCounter 只执行一次的合同。
	 * 跨过 GE 到期点的末帧用于检验真实持续时间分界，不能用定时器晚执行的时刻替代。
	 */
	class FHeroRegenerationBoundary final : public IAutomationLatentCommand
	{
	public:
		explicit FHeroRegenerationBoundary(FAutomationTestBase* InTest) : Test(InTest)
		{
		}

		virtual bool Update() override
		{
			if (!Fixture)
			{
				Fixture = MakeUnique<FHeroAttributeFixture>();
				Fixture->Definition->Attributes.HealthRegeneration.BaseValue = 10;
				Fixture->Definition->Attributes.ManaRegeneration.BaseValue = 4;
				if (!Fixture->Initialize(Test))
				{
					return true;
				}

				// 先启动真实 TimerManager，再应用有持续时间的来源，避免待激活定时器的首帧偏移。
				Fixture->World->Tick(LEVELTICK_All, .125f);
				Fixture->Component->FlushRegeneration();
				Fixture->AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 20);
				Fixture->AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetManaAttribute(), 30);
				EffectHandle = Fixture->ApplyModifier(UHeroAttributeSet::GetHealthRegenerationAttribute(), 20, .5f);
				Test->TestTrue(TEXT("实际恢复率 GE 已应用"), EffectHandle.IsValid());
				return false;
			}

			Fixture->World->Tick(LEVELTICK_All, .125f);
			if (++TickCount < 5)
			{
				return false;
			}

			Fixture->Component->FlushRegeneration();
			Test->TestNull(TEXT("恢复率 GE 由原生持续时间自动移除"), Fixture->AbilitySystem->GetActiveGameplayEffect(EffectHandle));
			TestValue(Test, TEXT("恢复率到期前后按各自点每秒积分"), Fixture->Hero->GetAttributes()->GetHealth(), 36.25f);
			TestValue(Test, TEXT("Mana 按实际存活 .625 秒恢复"), Fixture->Hero->GetAttributes()->GetMana(), 32.5f);
			Fixture.Reset();
			return true;
		}


	private:
		FAutomationTestBase* Test;
		TUniquePtr<FHeroAttributeFixture> Fixture;
		FActiveGameplayEffectHandle EffectHandle;
		int32 TickCount = 0;
	};

	class FHeroMaximumBoundary final : public IAutomationLatentCommand
	{
	public:
		explicit FHeroMaximumBoundary(FAutomationTestBase* InTest) : Test(InTest)
		{
		}

		virtual bool Update() override
		{
			if (!Fixture)
			{
				Fixture = MakeUnique<FHeroAttributeFixture>();
				Fixture->Definition->Attributes.HealthRegeneration.BaseValue = 10;
				if (!Fixture->Initialize(Test))
				{
					return true;
				}

				Fixture->World->Tick(LEVELTICK_All, .125f);
				Fixture->Component->FlushRegeneration();
				Fixture->AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 50);
				EffectHandle = Fixture->ApplyModifier(UHeroAttributeSet::GetMaxHealthAttribute(), 100, .5f);
				Test->TestTrue(TEXT("实际上限 GE 已应用"), EffectHandle.IsValid());
				TestValue(Test, TEXT("上限添加立即保持一半比例"), Fixture->Hero->GetAttributes()->GetHealth(), 100);
				return false;
			}

			Fixture->World->Tick(LEVELTICK_All, .125f);
			if (++TickCount < 5)
			{
				return false;
			}

			Fixture->Component->FlushRegeneration();
			Test->TestNull(TEXT("上限 GE 由原生持续时间自动移除"), Fixture->AbilitySystem->GetActiveGameplayEffect(EffectHandle));
			TestValue(Test, TEXT("上限到期回到基础值"), Fixture->Hero->GetAttributes()->GetMaxHealth(), 100);
			TestValue(Test, TEXT("到期点先恢复再保比例，余下时间按新上限恢复"), Fixture->Hero->GetAttributes()->GetHealth(), 53.75f);
			Fixture.Reset();
			return true;
		}


	private:
		FAutomationTestBase* Test;
		TUniquePtr<FHeroAttributeFixture> Fixture;
		FActiveGameplayEffectHandle EffectHandle;
		int32 TickCount = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroAttributeProgressionTest, "Moba.Attributes.InitializationAndProgression", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHeroAttributeProgressionTest::RunTest(const FString& Parameters)
{
	FHeroAttributeFixture Fixture;
	if (!Fixture.Initialize(this))
	{
		return false;
	}

	TestTrue(TEXT("初始化发布 Ready"), Fixture.Component->IsReady());
	TestValue(this, TEXT("一级满生命"), Fixture.Hero->GetAttributes()->GetHealth(), 100);
	TestValue(this, TEXT("一级满 Mana"), Fixture.Hero->GetAttributes()->GetMana(), 80);
	Fixture.AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 50);
	Fixture.AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetManaAttribute(), 40);
	FString Reason;
	TestFalse(TEXT("重复开始初始化被拒绝"), Fixture.Component->BeginInitialization(Fixture.Definition.Get(), Fixture.Rules.Get(), Reason));
	TestFalse(TEXT("重复完成初始化被拒绝"), Fixture.Component->FinishInitialization());
	TestValue(this, TEXT("重复初始化不补血"), Fixture.Hero->GetAttributes()->GetHealth(), 50);
	TestValue(this, TEXT("重复初始化不补 Mana"), Fixture.Hero->GetAttributes()->GetMana(), 40);

	const auto ExperienceResult = Fixture.Component->AddExperience(350);
	TestTrue(TEXT("可信经验被接受"), ExperienceResult.bAccepted);
	TestEqual(TEXT("跨级前为一级"), ExperienceResult.PreviousLevel, 1);
	TestEqual(TEXT("累计350跨至三级"), ExperienceResult.NewLevel, 3);
	TestEqual(TEXT("累计经验保持整数350"), Fixture.Component->GetTotalExperiencePoints(), int64(350));
	TestValue(this, TEXT("唯一等级 Attribute 为三级"), Fixture.Hero->GetAttributes()->GetHeroLevel(), 3);
	TestValue(this, TEXT("三级成长上限120"), Fixture.Hero->GetAttributes()->GetMaxHealth(), 120);
	TestValue(this, TEXT("跨级仅按最终上限保留一半生命"), Fixture.Hero->GetAttributes()->GetHealth(), 60);
	TestValue(this, TEXT("成长更新 AttackDamage 基础值"), Fixture.AbilitySystem->GetNumericAttributeBase(UHeroAttributeSet::GetAttackDamageAttribute()), 24);

	TestFalse(TEXT("负数经验被拒绝"), Fixture.Component->AddExperience(-1).bAccepted);
	TestEqual(TEXT("拒绝后经验未变"), Fixture.Component->GetTotalExperiencePoints(), int64(350));
	const auto CappedResult = Fixture.Component->AddExperience(MAX_int64);
	TestTrue(TEXT("最大int64奖励安全接受"), CappedResult.bAccepted);
	TestEqual(TEXT("只增加封顶所需250"), CappedResult.AddedExperiencePoints, int64(250));
	TestEqual(TEXT("满级累计经验封顶600"), Fixture.Component->GetTotalExperiencePoints(), int64(600));
	TestValue(this, TEXT("满级 Attribute 为四级"), Fixture.Hero->GetAttributes()->GetHeroLevel(), 4);
	TestEqual(TEXT("满级继续奖励不累积"), Fixture.Component->AddExperience(MAX_int64).AddedExperiencePoints, int64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroAttributeSourceTest, "Moba.Attributes.SourceRemovalAndSlow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHeroAttributeSourceTest::RunTest(const FString& Parameters)
{
	FHeroAttributeFixture Fixture;
	if (!Fixture.Initialize(this))
	{
		return false;
	}

	Fixture.AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 50);
	Fixture.AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetManaAttribute(), 20);
	const auto HealthHandle = Fixture.ApplyModifier(UHeroAttributeSet::GetMaxHealthAttribute(), 100);
	const auto ManaHandle = Fixture.ApplyModifier(UHeroAttributeSet::GetMaxManaAttribute(), 40);
	TestTrue(TEXT("原生上限来源句柄有效"), HealthHandle.IsValid() && ManaHandle.IsValid());
	TestValue(this, TEXT("生命来源添加保比例"), Fixture.Hero->GetAttributes()->GetHealth(), 100);
	TestValue(this, TEXT("Mana来源添加保比例"), Fixture.Hero->GetAttributes()->GetMana(), 30);
	Fixture.AbilitySystem->RemoveActiveGameplayEffect(HealthHandle);
	Fixture.AbilitySystem->RemoveActiveGameplayEffect(ManaHandle);
	TestValue(this, TEXT("撤销来源恢复生命上限"), Fixture.Hero->GetAttributes()->GetMaxHealth(), 100);
	TestValue(this, TEXT("撤销来源保持生命比例"), Fixture.Hero->GetAttributes()->GetHealth(), 50);
	TestValue(this, TEXT("撤销来源保持Mana比例"), Fixture.Hero->GetAttributes()->GetMana(), 20);

	const auto WeakerSlow = Fixture.ApplyModifier(UHeroAttributeSet::GetMoveSpeedAttribute(), .7f, 0, EGameplayModOp::MultiplyCompound);
	const auto StrongerSlow = Fixture.ApplyModifier(UHeroAttributeSet::GetMoveSpeedAttribute(), .4f, 0, EGameplayModOp::MultiplyCompound);
	TestTrue(TEXT("两个独立减速来源句柄有效"), WeakerSlow.IsValid() && StrongerSlow.IsValid());
	TestValue(this, TEXT("多个减速只取最强"), Fixture.Hero->GetAttributes()->GetMoveSpeed(), 240);
	Fixture.AbilitySystem->RemoveActiveGameplayEffect(StrongerSlow);
	TestValue(this, TEXT("撤销最强减速后恢复较弱减速"), Fixture.Hero->GetAttributes()->GetMoveSpeed(), 420);
	Fixture.AbilitySystem->RemoveActiveGameplayEffect(WeakerSlow);
	TestValue(this, TEXT("撤销全部来源恢复基础移速"), Fixture.Hero->GetAttributes()->GetMoveSpeed(), 600);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroAttributeNoResourceTest, "Moba.Attributes.NoResource", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHeroAttributeNoResourceTest::RunTest(const FString& Parameters)
{
	FHeroAttributeFixture Fixture;
	Fixture.Definition->ResourceType = EHeroResourceType::None;
	Fixture.Definition->Attributes.MaxMana = {0, 0};
	if (!Fixture.Initialize(this))
	{
		return false;
	}

	Fixture.AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetManaAttribute(), 100);
	Fixture.AbilitySystem->SetNumericAttributeBase(UHeroAttributeSet::GetManaRegenerationAttribute(), 10);
	const auto ManaHandle = Fixture.ApplyModifier(UHeroAttributeSet::GetMaxManaAttribute(), 40);
	TestTrue(TEXT("None资源上限来源保留原生句柄"), ManaHandle.IsValid());
	TestValue(this, TEXT("None的Mana恒为零"), Fixture.Hero->GetAttributes()->GetMana(), 0);
	TestValue(this, TEXT("None的上限恒为零"), Fixture.Hero->GetAttributes()->GetMaxMana(), 0);
	TestValue(this, TEXT("None的恢复率恒为零"), Fixture.Hero->GetAttributes()->GetManaRegeneration(), 0);
	Fixture.Component->AddExperience(MAX_int64);
	TestValue(this, TEXT("None升级不创建Mana"), Fixture.Hero->GetAttributes()->GetMana(), 0);
	Fixture.AbilitySystem->RemoveActiveGameplayEffect(ManaHandle);
	Fixture.Component->StopForDeath();
	Fixture.Component->RestoreForRespawn();
	TestValue(this, TEXT("None复活保持零资源"), Fixture.Hero->GetAttributes()->GetMana(), 0);
	TestValue(this, TEXT("复活恢复当前成长后的满生命"), Fixture.Hero->GetAttributes()->GetHealth(), 130);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroAttributeInvalidConfigurationTest, "Moba.Attributes.InvalidConfiguration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHeroAttributeInvalidConfigurationTest::RunTest(const FString& Parameters)
{
	FHeroAttributeFixture Fixture;
	Fixture.Rules->AttributeRules.ExperienceThresholds = {0, 100, 100};
	FString Reason;
	TestFalse(TEXT("重复累计门槛阻止初始化"), Fixture.Component->BeginInitialization(Fixture.Definition.Get(), Fixture.Rules.Get(), Reason));
	TestFalse(TEXT("非法门槛不发布Ready"), Fixture.Component->IsReady());
	TestFalse(TEXT("非法门槛不能完成半初始化"), Fixture.Component->FinishInitialization());
	Fixture.Rules->AttributeRules.ExperienceThresholds = {0, 100, 300, 600};
	Fixture.Definition->Attributes.MaxHealth.BaseValue = 0;
	TestFalse(TEXT("零生命上限阻止初始化"), Fixture.Component->BeginInitialization(Fixture.Definition.Get(), Fixture.Rules.Get(), Reason));
	TestFalse(TEXT("非法配置不能接受经验"), Fixture.Component->AddExperience(100).bAccepted);
	Fixture.Definition->Attributes.MaxHealth.BaseValue = 100;
	TestTrue(TEXT("修正瞬态夹具后可以首次初始化"), Fixture.Initialize(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroAttributeRegenerationTest, "Moba.Attributes.RegenerationExpiryBoundary", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHeroAttributeRegenerationTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FHeroRegenerationBoundary(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeroAttributeMaximumExpiryTest, "Moba.Attributes.MaximumExpiryBoundary", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHeroAttributeMaximumExpiryTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FHeroMaximumBoundary(this));
	return true;
}


#endif
