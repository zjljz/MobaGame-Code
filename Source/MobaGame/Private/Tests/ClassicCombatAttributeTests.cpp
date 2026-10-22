#include "Combat/CombatEffectContext.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AbilitySystem/AbilitySetDefinition.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "AbilitySystem/SelfCommitAbility.h"
#include "Combat/CombatCooldown.h"
#include "Combat/CombatDamageExecution.h"
#include "Combat/CombatEffectParameters.h"
#include "Combat/CombatHealingExecution.h"
#include "Combat/HeroAttributeSet.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceSubsystem.h"
#include "GameFramework/WorldSettings.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "Hero/HeroAttributeComponent.h"
#include "Hero/HeroCharacter.h"
#include "Hero/HeroDefinition.h"
#include "Misc/AutomationTest.h"
#include "MobaGameplayTags.h"
#include "MobaMessageTags.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	/**
	 * 不依赖内容资产的战斗接缝夹具；身份与参数仅用于机制测试，不构成正式平衡或完整比赛准入。
	 * 三个真实 Hero/ASC 共用瞬态规则，允许同一来源 Spec 分别命中两个目标。
	 */
	class FClassicCombatAttributeFixture
	{
	public:
		FClassicCombatAttributeFixture()
		{
			const auto WorldValues = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
				.CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &WorldValues);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
			Rules.Reset(NewObject<UMatchRulesDefinition>(World));
			Rules->AttributeRules.ExperienceThresholds = {0, 100};
			Rules->AttributeRules.MaximumCooldownReduction = .4f;
			Rules->AttributeRules.MinimumSlowMultiplier = .1f;
			auto* Experience = World->GetSubsystem<UExperienceSubsystem>();
			FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("RulesDef"))->SetObjectPropertyValue_InContainer(Experience, Rules.Get());
			Definition.Reset(NewObject<UHeroDefinition>(World));
			AbilitySet.Reset(NewObject<UAbilitySetDefinition>(World));
			FAbilityGrantDefinition Grant;
			Grant.Ability = USelfCommitAbility::StaticClass();
			AbilitySet->Abilities.Add(Grant);
			Definition->AbilitySet = AbilitySet.Get();
			Definition->Attributes.MaxHealth.BaseValue = 1000;
			Definition->Attributes.MaxMana.BaseValue = 80;
			Definition->Attributes.AttackDamage.BaseValue = 40;
			Definition->Attributes.AbilityPower.BaseValue = 20;
			Definition->Attributes.CriticalDamageMultiplier.BaseValue = 2;
			Definition->Attributes.CooldownReduction.BaseValue = .2f;
			Definition->Attributes.MoveSpeed.BaseValue = 600;
			const FGuid Match = FGuid::NewGuid();
			Source = SpawnHero(Match, 7);
			Target = SpawnHero(Match, 9);
			OtherTarget = SpawnHero(Match, 9);
		}

		~FClassicCombatAttributeFixture()
		{
			auto* Experience = World->GetSubsystem<UExperienceSubsystem>();
			FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("RulesDef"))->SetObjectPropertyValue_InContainer(Experience, nullptr);
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}


	public:
		bool Initialize(FAutomationTestBase* Test)
		{
			for (auto* Hero : {Source, Target, OtherTarget})
			{
				FString Reason;
				const bool bStarted = Hero->GetAttributeComponent()->BeginInitialization(Definition.Get(), Rules.Get(), Reason);
				if (!Test->TestTrue(FString::Printf(TEXT("战斗夹具开始初始化：%s"), *Reason), bStarted)
					|| !Test->TestTrue(TEXT("战斗夹具完成初始化"), Hero->GetAttributeComponent()->FinishInitialization()))
				{
					return false;
				}
			}

			return true;
		}

		UGameplayEffect* Damage(FGameplayTag DamageTag, float Base, float AttackCoefficient = 0, float PowerCoefficient = 0, bool bCanCritical = false)
		{
			auto* Effect = NewEffect();
			Effect->DurationPolicy = EGameplayEffectDurationType::Instant;
			FGameplayEffectExecutionDefinition Execution;
			Execution.CalculationClass = UCombatDamageExecution::StaticClass();
			Effect->Executions.Add(Execution);
			auto& Parameters = Effect->FindOrAddComponent<UCombatDamageParametersComponent>();
			Parameters.BaseDamage = FScalableFloat(Base);
			Parameters.AttackDamageCoefficient = FScalableFloat(AttackCoefficient);
			Parameters.AbilityPowerCoefficient = FScalableFloat(PowerCoefficient);
			Parameters.bCanCritical = bCanCritical;
			FInheritedTagContainer AssetTags;
			AssetTags.AddTag(DamageTag);
			Effect->FindOrAddComponent<UAssetTagsGameplayEffectComponent>().SetAndApplyAssetTagChanges(AssetTags);
			return Effect;
		}

		UGameplayEffect* Healing()
		{
			auto* Effect = NewEffect();
			Effect->DurationPolicy = EGameplayEffectDurationType::Instant;
			FGameplayEffectExecutionDefinition Execution;
			Execution.CalculationClass = UCombatHealingExecution::StaticClass();
			Effect->Executions.Add(Execution);
			auto& Parameters = Effect->FindOrAddComponent<UCombatHealingParametersComponent>();
			Parameters.HealAmount = FScalableFloat(20);
			Parameters.AttackDamageCoefficient = FScalableFloat(.5f);
			Parameters.AbilityPowerCoefficient = FScalableFloat(1);
			return Effect;
		}

		UGameplayEffect* Shield()
		{
			auto* Effect = NewEffect();
			Effect->DurationPolicy = EGameplayEffectDurationType::HasDuration;
			Effect->DurationMagnitude = FScalableFloat(10);
			auto& Parameters = Effect->FindOrAddComponent<UCombatShieldEffectComponent>();
			Parameters.ShieldAmount = FScalableFloat(30);
			Parameters.AttackDamageCoefficient = FScalableFloat(.5f);
			Parameters.AbilityPowerCoefficient = FScalableFloat(1);
			FInheritedTagContainer TargetTags;
			TargetTags.AddTag(MobaGameplayTags::Effect_Shield);
			TargetTags.AddTag(MobaGameplayTags::Effect_RemoveOnDeath);
			Effect->FindOrAddComponent<UTargetTagsGameplayEffectComponent>().SetAndApplyTargetTagChanges(TargetTags);
			return Effect;
		}

		UGameplayEffect* NewEffect()
		{
			auto* Effect = NewObject<UGameplayEffect>(World);
			Effects.Emplace(Effect);
			return Effect;
		}

		void ConfigureStack(UGameplayEffect* Effect, int32 Maximum)
		{
			// 原生Editor setter没有导出符号；仅给瞬态夹具写反射配置，不改引擎或共享CDO。
			auto* StackingProperty = FindFProperty<FEnumProperty>(UGameplayEffect::StaticClass(), TEXT("StackingType"));
			StackingProperty->GetUnderlyingProperty()->SetIntPropertyValue(StackingProperty->ContainerPtrToValuePtr<void>(Effect), int64(EGameplayEffectStackingType::AggregateBySource));
			Effect->StackLimitCount = Maximum;
		}

		void SetAttribute(AHeroCharacter* Hero, const FGameplayAttribute& Attribute, float Value)
		{
			Hero->GetHeroAbilitySystem()->SetNumericAttributeBase(Attribute, Value);
		}


	private:
		AHeroCharacter* SpawnHero(FGuid Match, int32 Team)
		{
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			auto* Hero = World->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, SpawnParameters);
			Hero->MatchId = Match;
			Hero->EntityId = FGuid::NewGuid();
			Hero->ParticipantId = FGuid::NewGuid();
			Hero->TeamId = Team;
			return Hero;
		}


	public:
		UWorld* World = nullptr;
		AHeroCharacter* Source = nullptr;
		AHeroCharacter* Target = nullptr;
		AHeroCharacter* OtherTarget = nullptr;
		TStrongObjectPtr<UMatchRulesDefinition> Rules;
		TStrongObjectPtr<UHeroDefinition> Definition;
		TStrongObjectPtr<UAbilitySetDefinition> AbilitySet;
		TArray<TStrongObjectPtr<UGameplayEffect>> Effects;
	};

	bool TestCombatValue(FAutomationTestBase* Test, const TCHAR* Label, float Actual, float Expected)
	{
		return Test->TestTrue(FString::Printf(TEXT("%s：实际 %.6f，预期 %.6f"), Label, Actual, Expected), FMath::IsNearlyEqual(Actual, Expected, .001f));
	}

	/** 用原生周期和固定World步长检查刷新接缝；每个引擎帧只推进一次，避免TimerManager跳过重复Tick。 */
	class FClassicCombatPeriodicBoundary final : public IAutomationLatentCommand
	{
	public:
		explicit FClassicCombatPeriodicBoundary(FAutomationTestBase* InTest) : Test(InTest)
		{
		}

		virtual bool Update() override
		{
			if (!Fixture)
			{
				Fixture = MakeUnique<FClassicCombatAttributeFixture>();
				if (!Fixture->Initialize(Test))
				{
					return true;
				}

				Fixture->World->Tick(LEVELTICK_All, .125f);
				Fixture->SetAttribute(Fixture->Source, UHeroAttributeSet::GetCriticalChanceAttribute(), 1);
				Fixture->SetAttribute(Fixture->OtherTarget, UHeroAttributeSet::GetHealthAttribute(), 500);
				DamageEffect = Fixture->Damage(MobaGameplayTags::Damage_Physical, 10, 1, 0, true);
				HealingEffect = Fixture->Healing();
				auto& HealingParameters = HealingEffect->FindOrAddComponent<UCombatHealingParametersComponent>();
				HealingParameters.HealAmount = FScalableFloat(5);
				HealingParameters.AttackDamageCoefficient = FScalableFloat(.25f);
				HealingParameters.AbilityPowerCoefficient = FScalableFloat(0);
				for (auto* Effect : {DamageEffect, HealingEffect})
				{
					Effect->DurationPolicy = EGameplayEffectDurationType::HasDuration;
					Effect->DurationMagnitude = FScalableFloat(2);
					Effect->Period = FScalableFloat(.5f);
					Effect->bExecutePeriodicEffectOnApplication = false;
					Effect->StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::NeverReset;
					Fixture->ConfigureStack(Effect, 2);
				}

				DamageHandle = Apply(DamageEffect, Fixture->Target);
				Apply(DamageEffect, Fixture->Target);
				HealingHandle = Apply(HealingEffect, Fixture->OtherTarget);
				Test->TestTrue(TEXT("真实DoT与HoT已创建原生handle"), DamageHandle.IsValid() && HealingHandle.IsValid());
				TestCombatValue(Test, TEXT("DoT应用时不立即执行"), Fixture->Target->GetAttributes()->GetHealth(), 1000);
				TestCombatValue(Test, TEXT("HoT应用时不立即执行"), Fixture->OtherTarget->GetAttributes()->GetHealth(), 500);
				return false;
			}

			Fixture->World->Tick(LEVELTICK_All, .125f);
			++TickCount;
			if (TickCount == 3)
			{
				TestCombatValue(Test, TEXT("首个周期之前不提前伤害"), Fixture->Target->GetAttributes()->GetHealth(), 1000);
				TestCombatValue(Test, TEXT("首个周期之前不提前治疗"), Fixture->OtherTarget->GetAttributes()->GetHealth(), 500);
				Fixture->SetAttribute(Fixture->Source, UHeroAttributeSet::GetAttackDamageAttribute(), 80);
				Fixture->SetAttribute(Fixture->Target, UHeroAttributeSet::GetArmorAttribute(), 100);
				Apply(DamageEffect, Fixture->Target);
				Apply(HealingEffect, Fixture->OtherTarget);
			}

			if (TickCount == 5)
			{
				// .375秒刷新；原来.5秒的周期应保留，此时越过原周期而尚未到刷新后的新周期。
				TestCombatValue(Test, TEXT("满层刷新用最新来源强度，周期不重置且层数只乘一次"), Fixture->Target->GetAttributes()->GetHealth(), 910);
				TestCombatValue(Test, TEXT("HoT刷新同样保留周期并且两层只乘一次"), Fixture->OtherTarget->GetAttributes()->GetHealth(), 550);
				if (const auto* Active = Fixture->Target->GetHeroAbilitySystem()->GetActiveGameplayEffect(DamageHandle))
				{
					const auto* Context = MobaCombat::GetContext(Active->Spec.GetContext());
					Test->TestEqual(TEXT("周期组仍为两层"), Active->Spec.GetStackCount(), 2);
					Test->TestFalse(TEXT("周期伤害忽略100%英雄暴击率"), Context->Result.bCritical);
				}

				Fixture->Target->GetHeroAbilitySystem()->AddLooseGameplayTag(MobaGameplayTags::State_Invulnerable);
			}

			if (TickCount == 9)
			{
				TestCombatValue(Test, TEXT("免伤期间周期继续但不扣生命"), Fixture->Target->GetAttributes()->GetHealth(), 910);
				TestCombatValue(Test, TEXT("独立HoT继续第二个周期"), Fixture->OtherTarget->GetAttributes()->GetHealth(), 600);
				if (const auto* Active = Fixture->Target->GetHeroAbilitySystem()->GetActiveGameplayEffect(DamageHandle))
				{
					const auto* Context = MobaCombat::GetContext(Active->Spec.GetContext());
					Test->TestFalse(TEXT("受阻周期清除上次成功结果"), Context->Result.bApplied);
					TestCombatValue(Test, TEXT("受阻周期不保留上次金额"), Context->Result.Amount, 0);
				}

				Fixture->Target->GetHeroAbilitySystem()->RemoveLooseGameplayTag(MobaGameplayTags::State_Invulnerable);
				Fixture->SetAttribute(Fixture->Target, UHeroAttributeSet::GetArmorAttribute(), -100);
			}

			if (TickCount < 13)
			{
				return false;
			}

			TestCombatValue(Test, TEXT("第三周期实时负抗性生效且不补免伤期间历史tick"), Fixture->Target->GetAttributes()->GetHealth(), 640);
			TestCombatValue(Test, TEXT("HoT第三周期仍精确增加50"), Fixture->OtherTarget->GetAttributes()->GetHealth(), 650);
			TestCombatValue(Test, TEXT("周期伤害meta清零"), Fixture->Target->GetHeroAbilitySystem()->GetNumericAttribute(UHeroAttributeSet::GetIncomingDamageAttribute()), 0);
			TestCombatValue(Test, TEXT("周期治疗meta清零"), Fixture->OtherTarget->GetHeroAbilitySystem()->GetNumericAttribute(UHeroAttributeSet::GetIncomingHealAttribute()), 0);
			Fixture.Reset();
			return true;
		}


	private:
		FActiveGameplayEffectHandle Apply(UGameplayEffect* Effect, AHeroCharacter* Target)
		{
			const auto Prepared = MobaCombat::PrepareSpec(Fixture->Source->GetHeroAbilitySystem(), Effect, 1);
			auto TargetSpec = MobaCombat::CopyForTarget(*Prepared.Data);
			return Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(TargetSpec);
		}


	private:
		FAutomationTestBase* Test;
		TUniquePtr<FClassicCombatAttributeFixture> Fixture;
		UGameplayEffect* DamageEffect = nullptr;
		UGameplayEffect* HealingEffect = nullptr;
		FActiveGameplayEffectHandle DamageHandle;
		FActiveGameplayEffectHandle HealingHandle;
		int32 TickCount = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicCombatSourceCaptureTest, "Moba.ClassicCombatAttributes.SourceSnapshotAndLiveTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicCombatSourceCaptureTest::RunTest(const FString& Parameters)
{
	FClassicCombatAttributeFixture Fixture;
	if (!Fixture.Initialize(this))
	{
		return false;
	}

	auto* Effect = Fixture.Damage(MobaGameplayTags::Damage_Physical, 10, 2, 1);
	const auto Prepared = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Effect, 1);
	const auto MagicalPrepared = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Fixture.Damage(MobaGameplayTags::Damage_Magical, 10, 2, 1), 1);
	if (!TestTrue(TEXT("真实伤害Spec准备成功"), Prepared.IsValid()))
	{
		return false;
	}

	Fixture.SetAttribute(Fixture.Source, UHeroAttributeSet::GetAttackDamageAttribute(), 80);
	Fixture.SetAttribute(Fixture.Source, UHeroAttributeSet::GetAbilityPowerAttribute(), 60);
	Fixture.SetAttribute(Fixture.Target, UHeroAttributeSet::GetArmorAttribute(), 100);
	Fixture.SetAttribute(Fixture.Target, UHeroAttributeSet::GetDamageTakenMultiplierAttribute(), 2);
	auto FirstTarget = MobaCombat::CopyForTarget(*Prepared.Data);
	Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(FirstTarget);
	TestCombatValue(this, TEXT("来源40AD和20AP冻结，目标100Armor及承伤2实时生效"), Fixture.Target->GetAttributes()->GetHealth(), 890);
	TestCombatValue(this, TEXT("伤害meta已清零"), Fixture.Target->GetHeroAbilitySystem()->GetNumericAttribute(UHeroAttributeSet::GetIncomingDamageAttribute()), 0);
	const FGuid SourceEntity = Fixture.Source->EntityId;
	const FGuid SourceParticipant = Fixture.Source->ParticipantId;
	Fixture.Source->Destroy();
	Fixture.SetAttribute(Fixture.OtherTarget, UHeroAttributeSet::GetArmorAttribute(), -100);
	auto SecondTarget = MobaCombat::CopyForTarget(*Prepared.Data);
	Fixture.OtherTarget->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(SecondTarget);
	TestCombatValue(this, TEXT("来源销毁后仍以冻结110金额和实时负100Armor命中"), Fixture.OtherTarget->GetAttributes()->GetHealth(), 835);
	const auto* Context = MobaCombat::GetContext(SecondTarget.GetContext());
	if (TestNotNull(TEXT("命中仍持有项目上下文"), Context))
	{
		TestEqual(TEXT("稳定实体归因按值保留"), Context->Entity, SourceEntity);
		TestEqual(TEXT("稳定参赛者归因按值保留"), Context->Participant, SourceParticipant);
		TestTrue(TEXT("来源销毁后结算仍成功"), Context->Result.bApplied);
	}

	Fixture.SetAttribute(Fixture.OtherTarget, UHeroAttributeSet::GetMagicResistanceAttribute(), 100);
	auto MagicalTarget = MobaCombat::CopyForTarget(*MagicalPrepared.Data);
	Fixture.OtherTarget->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(MagicalTarget);
	TestCombatValue(this, TEXT("魔法伤害使用命中时100MR且不使用负Armor"), Fixture.OtherTarget->GetAttributes()->GetHealth(), 780);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicCombatCriticalTest, "Moba.ClassicCombatAttributes.CriticalAndContextIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicCombatCriticalTest::RunTest(const FString& Parameters)
{
	FClassicCombatAttributeFixture Fixture;
	if (!Fixture.Initialize(this))
	{
		return false;
	}

	auto* CriticalEffect = Fixture.Damage(MobaGameplayTags::Damage_True, 100, 0, 0, true);
	Fixture.SetAttribute(Fixture.Target, UHeroAttributeSet::GetArmorAttribute(), 1000);
	Fixture.SetAttribute(Fixture.Target, UHeroAttributeSet::GetMagicResistanceAttribute(), 1000);
	const auto ZeroCritical = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), CriticalEffect, 1);
	auto ZeroTarget = MobaCombat::CopyForTarget(*ZeroCritical.Data);
	Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(ZeroTarget);
	TestCombatValue(this, TEXT("英雄0%暴击率确定不暴击"), Fixture.Target->GetAttributes()->GetHealth(), 900);
	TestFalse(TEXT("0%结果无暴击标记"), MobaCombat::GetContext(ZeroTarget.GetContext())->Result.bCritical);

	// 原生聚合器可捕获未约束输入；有效值和捕获值都须用同一100%上限。
	auto* ChanceEffect = Fixture.NewEffect();
	ChanceEffect->DurationPolicy = EGameplayEffectDurationType::Infinite;
	FGameplayModifierInfo ChanceModifier;
	ChanceModifier.Attribute = UHeroAttributeSet::GetCriticalChanceAttribute();
	ChanceModifier.ModifierOp = EGameplayModOp::AddBase;
	ChanceModifier.ModifierMagnitude = FScalableFloat(2);
	ChanceEffect->Modifiers.Add(ChanceModifier);
	FGameplayEffectSpec ChanceSpec(ChanceEffect, Fixture.Source->GetHeroAbilitySystem()->MakeEffectContext(), 1);
	Fixture.Source->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(ChanceSpec);
	TestCombatValue(this, TEXT("显示暴击率夹至100%"), Fixture.Source->GetAttributes()->GetCriticalChance(), 1);
	const auto Prepared = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), CriticalEffect, 1);
	auto FirstTarget = MobaCombat::CopyForTarget(*Prepared.Data);
	auto SecondTarget = MobaCombat::CopyForTarget(*Prepared.Data);
	TestTrue(TEXT("两个目标上下文地址独立"), FirstTarget.GetContext().Get() != SecondTarget.GetContext().Get());
	Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(FirstTarget);
	TestTrue(TEXT("第一个目标100%暴击"), MobaCombat::GetContext(FirstTarget.GetContext())->Result.bCritical);
	TestFalse(TEXT("第一目标结果没有污染第二目标"), MobaCombat::GetContext(SecondTarget.GetContext())->Result.bApplied);
	TestFalse(TEXT("目标结果没有污染来源模板"), MobaCombat::GetContext(Prepared.Data->GetContext())->Result.bApplied);
	Fixture.OtherTarget->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(SecondTarget);
	TestCombatValue(this, TEXT("第一目标暴击只乘一次"), Fixture.Target->GetAttributes()->GetHealth(), 700);
	TestCombatValue(this, TEXT("第二目标独立暴击只乘一次"), Fixture.OtherTarget->GetAttributes()->GetHealth(), 800);
	auto* DefaultSkill = Fixture.Damage(MobaGameplayTags::Damage_True, 100);
	const auto DefaultPrepared = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), DefaultSkill, 1);
	auto DefaultTarget = MobaCombat::CopyForTarget(*DefaultPrepared.Data);
	Fixture.OtherTarget->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(DefaultTarget);
	TestCombatValue(this, TEXT("普通技能默认忽略英雄100%暴击"), Fixture.OtherTarget->GetAttributes()->GetHealth(), 700);
	TestFalse(TEXT("普通技能无暴击结果"), MobaCombat::GetContext(DefaultTarget.GetContext())->Result.bCritical);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicCombatHealingTest, "Moba.ClassicCombatAttributes.HealingAndOverflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicCombatHealingTest::RunTest(const FString& Parameters)
{
	FClassicCombatAttributeFixture Fixture;
	if (!Fixture.Initialize(this))
	{
		return false;
	}

	Fixture.SetAttribute(Fixture.Source, UHeroAttributeSet::GetCriticalChanceAttribute(), 1);
	Fixture.SetAttribute(Fixture.Target, UHeroAttributeSet::GetHealthAttribute(), 980);
	const auto Prepared = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Fixture.Healing(), 1);
	Fixture.SetAttribute(Fixture.Source, UHeroAttributeSet::GetAttackDamageAttribute(), 80);
	Fixture.SetAttribute(Fixture.Source, UHeroAttributeSet::GetAbilityPowerAttribute(), 60);
	auto TargetSpec = MobaCombat::CopyForTarget(*Prepared.Data);
	Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(TargetSpec);
	TestCombatValue(this, TEXT("治疗使用冻结系数且不越过上限"), Fixture.Target->GetAttributes()->GetHealth(), 1000);
	const auto& Result = MobaCombat::GetContext(TargetSpec.GetContext())->Result;
	TestTrue(TEXT("治疗实际应用"), Result.bApplied);
	TestFalse(TEXT("治疗不暴击"), Result.bCritical);
	TestCombatValue(this, TEXT("治疗真实生命变化20"), Result.HealthDelta, 20);
	TestCombatValue(this, TEXT("治疗溢出40"), Result.Overflow, 40);
	TestCombatValue(this, TEXT("治疗meta已清零"), Fixture.Target->GetHeroAbilitySystem()->GetNumericAttribute(UHeroAttributeSet::GetIncomingHealAttribute()), 0);

	auto& Life = *FindFProperty<FStructProperty>(AHeroCharacter::StaticClass(), TEXT("Life"))->ContainerPtrToValuePtr<FHeroLifeSnapshot>(Fixture.OtherTarget);
	Life.State = EHeroLifeState::Dead;
	Fixture.SetAttribute(Fixture.OtherTarget, UHeroAttributeSet::GetHealthAttribute(), 0);
	auto DeadSpec = MobaCombat::CopyForTarget(*Prepared.Data);
	Fixture.OtherTarget->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(DeadSpec);
	TestCombatValue(this, TEXT("死亡目标不被治疗复活"), Fixture.OtherTarget->GetAttributes()->GetHealth(), 0);
	TestFalse(TEXT("死亡治疗不发布已应用结果"), MobaCombat::GetContext(DeadSpec.GetContext())->Result.bApplied);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicCombatShieldTest, "Moba.ClassicCombatAttributes.ShieldCaptureAndReplacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicCombatShieldTest::RunTest(const FString& Parameters)
{
	FClassicCombatAttributeFixture Fixture;
	if (!Fixture.Initialize(this))
	{
		return false;
	}

	auto* Effect = Fixture.Shield();
	const auto Prepared = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Effect, 1);
	Fixture.SetAttribute(Fixture.Source, UHeroAttributeSet::GetAttackDamageAttribute(), 80);
	Fixture.SetAttribute(Fixture.Source, UHeroAttributeSet::GetAbilityPowerAttribute(), 60);
	auto TargetSpec = MobaCombat::CopyForTarget(*Prepared.Data);
	const auto OldHandle = Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(TargetSpec);
	TestTrue(TEXT("原生护盾handle有效"), OldHandle.IsValid());
	TestCombatValue(this, TEXT("护盾通过显式注册的来源捕获冻结为70"), Fixture.Target->GetAttributes()->GetShield(), 70);
	FGameplayEffectSpec MissingCapture(Effect, Fixture.Source->GetHeroAbilitySystem()->MakeEffectContext(), 1);
	const auto InvalidHandle = Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(MissingCapture);
	TestFalse(TEXT("缺少必需捕获的护盾Spec被拒绝"), InvalidHandle.IsValid());
	TestCombatValue(this, TEXT("非法护盾不替换既有盾"), Fixture.Target->GetAttributes()->GetShield(), 70);
	const auto NewPrepared = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Effect, 1);
	auto NewTarget = MobaCombat::CopyForTarget(*NewPrepared.Data);
	const auto NewHandle = Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(NewTarget);
	TestTrue(TEXT("替换护盾使用新handle"), NewHandle.IsValid() && NewHandle != OldHandle);
	TestNull(TEXT("旧护盾来源已移除"), Fixture.Target->GetHeroAbilitySystem()->GetActiveGameplayEffect(OldHandle));
	Fixture.Target->GetHeroAbilitySystem()->RemoveActiveGameplayEffect(OldHandle);
	TestCombatValue(this, TEXT("旧handle移除不能清新盾130"), Fixture.Target->GetAttributes()->GetShield(), 130);
	const auto DamagePrepared = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Fixture.Damage(MobaGameplayTags::Damage_True, 150), 1);
	auto DamageSpec = MobaCombat::CopyForTarget(*DamagePrepared.Data);
	Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(DamageSpec);
	TestCombatValue(this, TEXT("真伤先吸收护盾再扣血"), Fixture.Target->GetAttributes()->GetHealth(), 980);
	TestCombatValue(this, TEXT("耗尽后清零护盾池"), Fixture.Target->GetAttributes()->GetShield(), 0);
	TestCombatValue(this, TEXT("结算返回护盾实际吸收130"), MobaCombat::GetContext(DamageSpec.GetContext())->Result.ShieldAbsorbed, 130);
	TestNull(TEXT("耗尽移除对应护盾handle"), Fixture.Target->GetHeroAbilitySystem()->GetActiveGameplayEffect(NewHandle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicCombatCooldownTest, "Moba.ClassicCombatAttributes.CooldownSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicCombatCooldownTest::RunTest(const FString& Parameters)
{
	FClassicCombatAttributeFixture Fixture;
	if (!Fixture.Initialize(this))
	{
		return false;
	}

	auto* Cooldown = Fixture.NewEffect();
	Cooldown->DurationPolicy = EGameplayEffectDurationType::HasDuration;
	Cooldown->FindOrAddComponent<UAbilityCooldownParametersComponent>().BaseDuration = FScalableFloat(10);
	FCustomCalculationBasedFloat DurationCalculation;
	DurationCalculation.CalculationClassMagnitude = UAbilityCooldownMagnitudeCalculation::StaticClass();
	Cooldown->DurationMagnitude = DurationCalculation;
	const auto FirstSpec = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Cooldown, 1);
	Fixture.SetAttribute(Fixture.Source, UHeroAttributeSet::GetCooldownReductionAttribute(), .4f);
	const auto FirstHandle = Fixture.Source->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(*FirstSpec.Data);
	const auto* FirstActive = Fixture.Source->GetHeroAbilitySystem()->GetActiveGameplayEffect(FirstHandle);
	if (!TestNotNull(TEXT("共享MMC冷却已创建"), FirstActive))
	{
		return false;
	}

	TestCombatValue(this, TEXT("捕获20%CDR后改变来源仍为8秒"), FirstActive->Spec.GetDuration(), 8);
	const auto SecondSpec = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Cooldown, 1);
	const auto SecondHandle = Fixture.Source->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(*SecondSpec.Data);
	const auto* SecondActive = Fixture.Source->GetHeroAbilitySystem()->GetActiveGameplayEffect(SecondHandle);
	if (!TestNotNull(TEXT("下一次冷却已创建"), SecondActive))
	{
		return false;
	}

	TestCombatValue(this, TEXT("下一次捕获40%CDR为6秒"), SecondActive->Spec.GetDuration(), 6);
	TestCombatValue(this, TEXT("已有冷却期限不重算"), Fixture.Source->GetHeroAbilitySystem()->GetActiveGameplayEffect(FirstHandle)->Spec.GetDuration(), 8);
	auto* ExcessReduction = Fixture.NewEffect();
	ExcessReduction->DurationPolicy = EGameplayEffectDurationType::Infinite;
	FGameplayModifierInfo ReductionModifier;
	ReductionModifier.Attribute = UHeroAttributeSet::GetCooldownReductionAttribute();
	ReductionModifier.ModifierOp = EGameplayModOp::AddBase;
	ReductionModifier.ModifierMagnitude = FScalableFloat(2);
	ExcessReduction->Modifiers.Add(ReductionModifier);
	FGameplayEffectSpec ReductionSpec(ExcessReduction, Fixture.Source->GetHeroAbilitySystem()->MakeEffectContext(), 1);
	Fixture.Source->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(ReductionSpec);
	const auto CappedSpec = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Cooldown, 1);
	const auto CappedHandle = Fixture.Source->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(*CappedSpec.Data);
	if (const auto* CappedActive = Fixture.Source->GetHeroAbilitySystem()->GetActiveGameplayEffect(CappedHandle))
	{
		TestCombatValue(this, TEXT("原始捕获CDR超限仍夹至规则40%"), CappedActive->Spec.GetDuration(), 6);
	}
	else
	{
		AddError(TEXT("超限CDR夹取后的冷却未创建"));
	}

	auto* FixedCooldown = Fixture.NewEffect();
	FixedCooldown->DurationPolicy = EGameplayEffectDurationType::HasDuration;
	FixedCooldown->DurationMagnitude = FScalableFloat(10);
	const auto FixedSpec = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), FixedCooldown, 1);
	const auto FixedHandle = Fixture.Source->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(*FixedSpec.Data);
	if (const auto* FixedActive = Fixture.Source->GetHeroAbilitySystem()->GetActiveGameplayEffect(FixedHandle))
	{
		TestCombatValue(this, TEXT("未接技能MMC的原生物品或普攻间隔不吃CDR"), FixedActive->Spec.GetDuration(), 10);
	}
	else
	{
		AddError(TEXT("原生固定期限冷却未创建"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicCombatStackTest, "Moba.ClassicCombatAttributes.StackMaximumStrength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicCombatStackTest::RunTest(const FString& Parameters)
{
	FClassicCombatAttributeFixture Fixture;
	if (!Fixture.Initialize(this))
	{
		return false;
	}

	auto* Buff = Fixture.NewEffect();
	Buff->DurationPolicy = EGameplayEffectDurationType::HasDuration;
	Buff->DurationMagnitude = FScalableFloat(10);
	Fixture.ConfigureStack(Buff, 2);
	FAttributeBasedFloat SourcePower;
	SourcePower.BackingAttribute = MobaCombat::SourceAbilityPowerCapture();
	FGameplayModifierInfo AttackModifier;
	AttackModifier.Attribute = UHeroAttributeSet::GetAttackDamageAttribute();
	AttackModifier.ModifierOp = EGameplayModOp::AddBase;
	AttackModifier.ModifierMagnitude = SourcePower;
	Buff->Modifiers.Add(AttackModifier);
	const auto FirstSpec = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Buff, 1);
	const auto FirstHandle = Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(*FirstSpec.Data);
	const auto SecondSpec = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Buff, 1);
	const auto SecondHandle = Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(*SecondSpec.Data);
	TestTrue(TEXT("同定义和来源合并到同一个原生handle"), FirstHandle.IsValid() && FirstHandle == SecondHandle);
	TestCombatValue(this, TEXT("两层各20强度叠加到80AD"), Fixture.Target->GetAttributes()->GetAttackDamage(), 80);
	Fixture.SetAttribute(Fixture.Source, UHeroAttributeSet::GetAbilityPowerAttribute(), 30);
	const auto RefreshedSpec = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Buff, 1);
	const auto RefreshedHandle = Fixture.Target->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(*RefreshedSpec.Data);
	const auto* Active = Fixture.Target->GetHeroAbilitySystem()->GetActiveGameplayEffect(RefreshedHandle);
	if (TestNotNull(TEXT("满层刷新保留原生活动来源"), Active))
	{
		TestEqual(TEXT("满层刷新不伪造第三层"), Active->Spec.GetStackCount(), 2);
		TestCombatValue(this, TEXT("满层最新30强度真实更新整个组"), Fixture.Target->GetAttributes()->GetAttackDamage(), 100);
	}

	Fixture.Target->GetHeroAbilitySystem()->RemoveActiveGameplayEffect(RefreshedHandle);
	TestCombatValue(this, TEXT("移除层组恢复基础40AD"), Fixture.Target->GetAttributes()->GetAttackDamage(), 40);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicCombatPeriodicTest, "Moba.ClassicCombatAttributes.PeriodicRefreshAndImmunity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicCombatPeriodicTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FClassicCombatPeriodicBoundary(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClassicCombatNotificationTest, "Moba.ClassicCombatAttributes.SettlementNotification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FClassicCombatNotificationTest::RunTest(const FString& Parameters)
{
	FClassicCombatAttributeFixture Fixture;
	if (!Fixture.Initialize(this))
	{
		return false;
	}

	auto* TargetASC = Fixture.Target->GetHeroAbilitySystem();
	const auto Damage = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Fixture.Damage(MobaGameplayTags::Damage_True, 100), 1);
	const auto Heal = MobaCombat::PrepareSpec(Fixture.Source->GetHeroAbilitySystem(), Fixture.Healing(), 1);
	bool bNestedAttempted = false;
	bool bNestedRejected = false;
	const FDelegateHandle HealthHandle = TargetASC->GetGameplayAttributeValueChangeDelegate(UHeroAttributeSet::GetHealthAttribute()).AddLambda(
		[&](const FOnAttributeChangeData& Change)
		{
			if (!bNestedAttempted && Change.NewValue < Change.OldValue)
			{
				bNestedAttempted = true;
				auto NestedHeal = MobaCombat::CopyForTarget(*Heal.Data);
				TargetASC->ApplyGameplayEffectSpecToSelf(NestedHeal);
				bNestedRejected = (MobaCombat::GetContext(NestedHeal.GetContext())->Result.Rejection == TEXT("Busy"));
			}
		});
	int32 NotificationCount = 0;
	GMP::FMessageUtils::ListenObjectMessage(TargetASC, MOBA_MESSAGE_COMBAT_SETTLED, Fixture.Target,
		[&](const FGameplayEffectContextHandle& Context, const FCombatSettlementResult& Result)
		{
			++NotificationCount;
			TestFalse(TEXT("事实通知不位于资源Busy区域"), Fixture.Target->GetAttributes()->IsSettlementBusy());
			TestTrue(TEXT("只通知已经提交的本次结果"), Result.bApplied);
			TestCombatValue(this, TEXT("通知内资源已最终提交"), Fixture.Target->GetAttributes()->GetHealth(), 900);
			TestCombatValue(this, TEXT("通知携带实际生命损失"), Result.HealthDelta, -100);
			TestEqual(TEXT("通知保留按值来源归因"), MobaCombat::GetContext(Context)->Entity, Fixture.Source->EntityId);
		});
	auto TargetSpec = MobaCombat::CopyForTarget(*Damage.Data);
	TargetASC->ApplyGameplayEffectSpecToSelf(TargetSpec);
	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_COMBAT_SETTLED, Fixture.Target);
	TargetASC->GetGameplayAttributeValueChangeDelegate(UHeroAttributeSet::GetHealthAttribute()).Remove(HealthHandle);
	TestTrue(TEXT("资源委托重入路径确已触发且被Busy拒绝"), bNestedAttempted && bNestedRejected);
	TestEqual(TEXT("一次实际资源提交仅发一次GMP事实"), NotificationCount, 1);
	TestCombatValue(this, TEXT("被拒绝的嵌套治疗没有改变资源"), Fixture.Target->GetAttributes()->GetHealth(), 900);
	TestCombatValue(this, TEXT("被拒绝的嵌套治疗没有污染meta"), TargetASC->GetNumericAttribute(UHeroAttributeSet::GetIncomingHealAttribute()), 0);
	return true;
}

#endif
