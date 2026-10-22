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
#include "Combat/TargetDamageAbility.h"
#include "Combat/AbilityTargeting.h"
#include "Tests/Combat/TargetCombatant.h"
#include "Tests/Combat/CombatEffectTestTypes.h"
#include "Tests/AbilitySystem/ManagedAbilityTestTypes.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Hero/HeroDefinition.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "Match/ParticipantState.h"
#include "Match/MatchGameState.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagRequirementsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "MobaGameplayTags.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	class FCombatEffectsScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FCombatEffectsScenario(FAutomationTestBase* InTest) : Test(InTest) {}

		~FCombatEffectsScenario() override
		{
			if (World)
			{
				World->EndPlay(EEndPlayReason::Quit);
				World->DestroyWorld(false);
				GEngine->DestroyWorldContext(World);
			}
		}


	public:
		UGameplayEffect* Status(FGameplayTag State, bool bStrong = false, float Duration = 5)
		{
			auto* Effect = NewObject<UCombatControlEffect>(World);
			Effect->DurationMagnitude = FScalableFloat(Duration);
			FInheritedTagContainer Tags;
			Tags.AddTag(State);
			Tags.AddTag(MobaGameplayTags::Effect_Harmful);
			Tags.AddTag(bStrong ? MobaGameplayTags::Effect_Dispel_Strong : MobaGameplayTags::Effect_Dispel_Basic);
			Effect->FindOrAddComponent<UTargetTagsGameplayEffectComponent>().SetAndApplyTargetTagChanges(Tags);
			if (State == MobaGameplayTags::State_Stun || State == MobaGameplayTags::State_Silence)
			{
				FInheritedTagContainer Blocked;
				Blocked.AddTag(State == MobaGameplayTags::State_Stun ? MobaGameplayTags::Ability_Active : MobaGameplayTags::Ability_Spell);
				Effect->FindOrAddComponent<UBlockAbilityTagsGameplayEffectComponent>().SetAndApplyBlockedAbilityTagChanges(Blocked);
			}

			Objects.Emplace(Effect);
			return Effect;
		}

		FActiveGameplayEffectHandle Apply(
			UAbilitySystemComponent* Source, UAbilitySystemComponent* TargetASC, const UGameplayEffect* Effect, float Damage = 5, ECombatDamageType Type = ECombatDamageType::TrueDamage)
		{
			auto* Snapshot = NewObject<UAbilityDeliverySnapshot>(World);
			Objects.Emplace(Snapshot);
			Snapshot->Entity = FGuid::NewGuid();
			Snapshot->Match = Match->MatchId;
			Snapshot->Damage = Damage;
			Snapshot->DamagePolicy = ECombatDamagePolicy::Standard;
			Snapshot->DamageType = Type;
			auto Context = Source->MakeEffectContext();
			Context.AddSourceObject(Snapshot);
			return TargetASC->ApplyGameplayEffectSpecToSelf(FGameplayEffectSpec(Effect, Context, 1));
		}

		bool Update() override
		{
			if (!World)
			{
				Setup();
				Immediate();
				return false;
			}

			World->Tick(LEVELTICK_All, 0.05f);
			Elapsed += 0.05f;
			if (Elapsed > 0.5f && Phase == 0)
			{
				Test->TestEqual(TEXT("DoT首秒前无tick"), Target->GetHealth(), 100.f);
				Test->TestEqual(TEXT("HoT首秒前无tick"), HealTarget->GetHealth(), 50.f);
				Test->TestEqual(TEXT("最强Slow自然到期恢复另一来源"), Hero->GetAttributes()->GetMoveSpeed(), .6f);
				Phase = 1;
			}

			if (Elapsed > 1.15f && Phase == 1)
			{
				Test->TestEqual(TEXT("两来源各首tick5"), Target->GetHealth(), 90.f);
				Test->TestEqual(TEXT("HoT首tick5"), HealTarget->GetHealth(), 55.f);
				Test->TestEqual(TEXT("剩余Slow自然到期恢复1"), Hero->GetAttributes()->GetMoveSpeed(), 1.f);
				Test->TestEqual(TEXT("不可选中仍受已提交DoT"), Untargetable->GetHealth(), 95.f);
				ImmuneHandle = Apply(Sources[0], Untargetable->GetCombatAbilitySystem(), Status(MobaGameplayTags::State_Invulnerable));
				Phase = 2;
			}

			if (Elapsed > 1.6f && Phase == 2)
			{
				const auto Refreshed = Apply(Sources[0], Target->GetCombatAbilitySystem(), GetDefault<UCombatDamageOverTimeEffect>());
				Test->TestEqual(TEXT("同来源刷新原handle"), Refreshed, DotA);
				Phase = 3;
			}

			if (Elapsed > 2.15f && Phase == 3)
			{
				Test->TestEqual(TEXT("刷新不重置原2秒周期"), Target->GetHealth(), 80.f);
				Test->TestEqual(TEXT("免伤tick不扣血"), Untargetable->GetHealth(), 95.f);
				Test->TestEqual(TEXT("HoT第二tick"), HealTarget->GetHealth(), 60.f);
				Untargetable->GetCombatAbilitySystem()->RemoveActiveGameplayEffect(ImmuneHandle);
				Phase = 4;
			}

			if (Elapsed > 3.2f && Phase == 4)
			{
				Test->TestEqual(TEXT("普通周期三tick共15"), HealTarget->GetHealth(), 65.f);
				Test->TestEqual(TEXT("两来源三tick"), Target->GetHealth(), 70.f);
				Test->TestEqual(TEXT("免伤期限不暂停DoT"), Untargetable->GetHealth(), 90.f);
				Test->TestNull(TEXT("未刷新DoT到期"), Target->GetCombatAbilitySystem()->GetActiveGameplayEffect(DotB));
				Test->TestNull(TEXT("HoT到期"), HealTarget->GetCombatAbilitySystem()->GetActiveGameplayEffect(Hot));
				Phase = 5;
			}

			if (Elapsed > 4.2f && Phase == 5)
			{
				Test->TestEqual(TEXT("刷新来源第四tick，另一来源已到期"), Target->GetHealth(), 65.f);
				Phase = 6;
			}

			if (Elapsed > 5.25f)
			{
				Test->TestNull(TEXT("刷新DoT自然到期"), Target->GetCombatAbilitySystem()->GetActiveGameplayEffect(DotA));
				Test->TestEqual(TEXT("盾5秒到期清池"), ShieldTarget->GetCombatAbilitySystem()->GetNumericAttribute(UHeroAttributeSet::GetShieldAttribute()), 0.f);
				Test->TestNull(TEXT("盾寿命GE到期"), ShieldTarget->GetCombatAbilitySystem()->GetActiveGameplayEffect(ShieldHandle));
				return true;
			}

			return false;
		}

	private:
		ATargetCombatant* Spawn()
		{
			auto* Actor = World->SpawnActor<ATargetCombatant>();
			Actor->Entity = FGuid::NewGuid();
			Actor->Participant = FGuid::NewGuid();
			Actor->Match = Match->MatchId;
			return Actor;
		}

		void Setup()
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
			Match->MatchId = FGuid::NewGuid();
			World->SetGameState(Match);
			Sources = {Spawn()->GetCombatAbilitySystem(), Spawn()->GetCombatAbilitySystem()};
			auto* Set = NewObject<UAbilitySetDefinition>(World);
			Objects.Emplace(Set);
			auto& Entry = Set->Abilities.AddDefaulted_GetRef();
			Entry.Ability = UCostedSelfCombatTestAbility::StaticClass();
			Entry.Slot = MobaGameplayTags::Input_Slot_Primary;
			auto* Def = NewObject<UHeroDefinition>(World);
			Objects.Emplace(Def);
			Def->AbilitySet = Set;
			auto* PS = World->SpawnActor<AParticipantState>();
			PS->ParticipantId = FGuid::NewGuid();
			PS->MatchId = Match->MatchId;
			PS->TeamId = 7;
			Hero = World->SpawnActor<AHeroCharacter>();
			Test->TestTrue(TEXT("Hero初始化"), Hero->InitializeHero(Def, PS));
			auto* PC = World->SpawnActor<AHeroPlayerController>();
			PC->SetPlayerState(PS);
			PS->SetOwner(PC);
			PS->PrimaryHero = Hero;
			PC->Possess(Hero);
			PC->AcceptContentReady();
			PC->ServerPlayerReady(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision);
		}

		void Immediate()
		{
			auto* Ordinary = Spawn();
			for (auto* ASC : {Hero->GetCombatAbilitySystem(), Ordinary->GetCombatAbilitySystem()})
			{
				ASC->SetNumericAttributeBase(UHeroAttributeSet::GetArmorAttribute(), 100);
				ASC->SetNumericAttributeBase(UHeroAttributeSet::GetMagicResistanceAttribute(), -100);
				ASC->SetNumericAttributeBase(UHeroAttributeSet::GetMaxHealthAttribute(), 1000);
				ASC->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 1000);
				Apply(Sources[0], ASC, GetDefault<UCombatDamageEffect>(), 100, ECombatDamageType::Physical);
				Test->TestEqual(TEXT("真实GE物理100抗性后50"), ASC->GetNumericAttribute(UHeroAttributeSet::GetHealthAttribute()), 950.f);
				Apply(Sources[0], ASC, GetDefault<UCombatDamageEffect>(), 100, ECombatDamageType::Magical);
				Test->TestEqual(TEXT("真实GE负魔抗后150"), ASC->GetNumericAttribute(UHeroAttributeSet::GetHealthAttribute()), 800.f);
				auto* Reduction = NewObject<UGameplayEffect>(World);
				Objects.Emplace(Reduction);
				Reduction->DurationPolicy = EGameplayEffectDurationType::Infinite;
				auto& Mod = Reduction->Modifiers.AddDefaulted_GetRef();
				Mod.Attribute = UHeroAttributeSet::GetDamageTakenMultiplierAttribute();
				Mod.ModifierOp = EGameplayModOp::MultiplyCompound;
				Mod.ModifierMagnitude = FScalableFloat(0.9f);
				auto A = Apply(Sources[0], ASC, Reduction);
				auto B = Apply(Sources[1], ASC, Reduction);
				Test->TestTrue(*FString::Printf(TEXT("原生两份乘区0.81 actual=%.9f"), ASC->GetNumericAttribute(UHeroAttributeSet::GetDamageTakenMultiplierAttribute())),
					FMath::IsNearlyEqual(ASC->GetNumericAttribute(UHeroAttributeSet::GetDamageTakenMultiplierAttribute()), .81f, 0.0001f));
				auto OldShield = Apply(Sources[0], ASC, GetDefault<UCombatShieldEffect>());
				Apply(Sources[0], ASC, GetDefault<UCombatDamageEffect>(), 100);
				Test->TestTrue(TEXT("True仍经0.81与30盾后51入血"), FMath::IsNearlyEqual(ASC->GetNumericAttribute(UHeroAttributeSet::GetHealthAttribute()), 749.f));
				Test->TestNull(TEXT("耗尽盾移除GE"), ASC->GetActiveGameplayEffect(OldShield));
				ASC->RemoveActiveGameplayEffect(A);
				ASC->RemoveActiveGameplayEffect(B);
				Apply(Sources[0], ASC, GetDefault<UCombatHealingEffect>());
				Test->TestTrue(TEXT("真实Heal25"), FMath::IsNearlyEqual(ASC->GetNumericAttribute(UHeroAttributeSet::GetHealthAttribute()), 774.f));
				ASC->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 995);
				Apply(Sources[0], ASC, GetDefault<UCombatHealingEffect>());
				Test->TestEqual(TEXT("治疗夹到Max"), ASC->GetNumericAttribute(UHeroAttributeSet::GetHealthAttribute()), 1000.f);
				auto Stack = Apply(Sources[0], ASC, GetDefault<UCombatStackingEffect>());
				for (int32 I = 0; I < 3; ++I)
				{
					Apply(Sources[0], ASC, GetDefault<UCombatStackingEffect>());
				}

				auto Other = Apply(Sources[1], ASC, GetDefault<UCombatStackingEffect>());
				Test->TestEqual(TEXT("原生最多3层"), ASC->GetCurrentStackCount(Stack), 3);
				Test->TestTrue(TEXT("异来源独立handle"), Stack != Other);
				Test->TestEqual(TEXT("3+1层Armor"), ASC->GetNumericAttribute(UHeroAttributeSet::GetArmorAttribute()), 140.f);
				ASC->RemoveActiveGameplayEffect(Stack);
				Test->TestEqual(TEXT("撤一来源留另一层"), ASC->GetNumericAttribute(UHeroAttributeSet::GetArmorAttribute()), 110.f);
				Test->TestEqual(TEXT("Basic驱散正向剩余来源"), MobaCombat::Dispel(ASC, false, true, false), 1);
			}

			ControlAndRejection();
			Target = Spawn();
			HealTarget = Spawn();
			HealTarget->SetHealth(50);
			Untargetable = Spawn();
			ShieldTarget = Spawn();
			DotA = Apply(Sources[0], Target->GetCombatAbilitySystem(), GetDefault<UCombatDamageOverTimeEffect>());
			DotB = Apply(Sources[1], Target->GetCombatAbilitySystem(), GetDefault<UCombatDamageOverTimeEffect>());
			Hot = Apply(Sources[0], HealTarget->GetCombatAbilitySystem(), GetDefault<UCombatHealOverTimeEffect>());
			Apply(Sources[0], Untargetable->GetCombatAbilitySystem(), Status(MobaGameplayTags::State_Untargetable));
			Apply(Sources[0], Untargetable->GetCombatAbilitySystem(), GetDefault<UCombatDamageOverTimeEffect>());
			auto Old = Apply(Sources[0], ShieldTarget->GetCombatAbilitySystem(), GetDefault<UCombatShieldEffect>());
			ShieldHandle = Apply(Sources[1], ShieldTarget->GetCombatAbilitySystem(), GetDefault<UCombatShieldEffect>());
			Test->TestNull(TEXT("同帧替换旧盾"), ShieldTarget->GetCombatAbilitySystem()->GetActiveGameplayEffect(Old));
			Test->TestEqual(TEXT("旧回调不清新30盾"), ShieldTarget->GetCombatAbilitySystem()->GetNumericAttribute(UHeroAttributeSet::GetShieldAttribute()), 30.f);
			for (UClass* Class : {UInvalidDurationShieldEffect::StaticClass(), UInvalidCombatShieldEffect::StaticClass()})
			{
				const auto Invalid = Apply(Sources[0], ShieldTarget->GetCombatAbilitySystem(), Class->GetDefaultObject<UGameplayEffect>());
				Test->TestFalse(TEXT("直接ASC拒绝非法盾/寿命"), Invalid.IsValid());
				Test->TestNotNull(TEXT("失败不移旧盾handle"), ShieldTarget->GetCombatAbilitySystem()->GetActiveGameplayEffect(ShieldHandle));
			}

			auto* Inhibited = NewObject<UCombatShieldEffect>(World);
			Objects.Emplace(Inhibited);
			Inhibited->FindOrAddComponent<UTargetTagRequirementsGameplayEffectComponent>().OngoingTagRequirements.RequireTags.AddTag(MobaGameplayTags::State_Invulnerable);
			Test->TestFalse(TEXT("直接ASC拒绝持续条件抑制盾"), Apply(Sources[0], ShieldTarget->GetCombatAbilitySystem(), Inhibited).IsValid());
			Test->TestEqual(TEXT("被拒绝的盾不写池"), ShieldTarget->GetCombatAbilitySystem()->GetNumericAttribute(UHeroAttributeSet::GetShieldAttribute()), 30.f);
			auto* Friendly = Spawn();
			Friendly->SetHealth(50);
			Apply(Sources[0], Friendly->GetCombatAbilitySystem(), Status(MobaGameplayTags::State_Untargetable));
			Apply(Sources[0], Friendly->GetCombatAbilitySystem(), Status(MobaGameplayTags::State_Invulnerable));
			Apply(Sources[0], Friendly->GetCombatAbilitySystem(), GetDefault<UCombatHealingEffect>());
			Test->TestEqual(TEXT("免伤与不可选中不阻止专门治疗"), Friendly->GetHealth(), 75.f);
			Friendly->bAlive = false;
			Apply(Sources[0], Friendly->GetCombatAbilitySystem(), GetDefault<UCombatHealingEffect>());
			Test->TestEqual(TEXT("死亡拒绝治疗且不复活"), Friendly->GetHealth(), 75.f);
		}

		void ControlAndRejection()
		{
			auto* ASC = Hero->GetHeroAbilitySystem();
			const auto Handle = ASC->FindSlot(MobaGameplayTags::Input_Slot_Primary);
			auto* Spec = ASC->FindAbilitySpecFromHandle(Handle);
			auto* Ability = CastChecked<USelfCombatEffectAbility>(Spec->GetPrimaryInstance());
			for (UClass* Effect : {UInvalidCombatShieldEffect::StaticClass(), URejectedCombatShieldEffect::StaticClass(), UCommitRejectedShieldEffect::StaticClass(),
					 UInvalidDurationShieldEffect::StaticClass()})
			{
				Ability->Effect = Effect;
				Test->TestFalse(TEXT("非法盾/条件拒绝在扣费前"), ASC->TryActivateAbility(Handle));
				Test->TestEqual(TEXT("拒绝不扣Primary"), Hero->GetAttributes()->GetMana(), 100.f);
				Test->TestFalse(TEXT("拒绝无冷却"), ASC->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self));
				Test->TestEqual(TEXT("拒绝无盾"), Hero->GetAttributes()->GetShield(), 0.f);
			}

			Ability->Effect = UCombatShieldEffect::StaticClass();
			FGameplayEffectApplicationQuery Reject;
			Reject.BindLambda([](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { return false; });
			ASC->GameplayEffectApplicationQueries.Add(Reject);
			Test->TestFalse(TEXT("应用免疫query拒绝先于提交"), ASC->TryActivateAbility(Handle));
			Test->TestEqual(TEXT("query拒绝无成本"), Hero->GetAttributes()->GetMana(), 100.f);
			ASC->GameplayEffectApplicationQueries.Reset();
			auto Root = Apply(Sources[0], ASC, Status(MobaGameplayTags::State_Root));
			Test->TestEqual(TEXT("Root移动速度为0"), Hero->GetCharacterMovement()->GetMaxSpeed(), 0.f);
			Test->TestTrue(TEXT("Root仍可施法"), Ability->CanActivateAbility(Handle, ASC->AbilityActorInfo.Get()));
			const auto Holding = ASC->GiveAbility(FGameplayAbilitySpec(UCombatHoldingTestAbility::StaticClass(), 1));
			const auto SpellProbe = ASC->GiveAbility(FGameplayAbilitySpec(UCombatHoldingTestAbility::StaticClass(), 1));
			Test->TestTrue(TEXT("非Self未激活法术前置可用"), ASC->FindAbilitySpecFromHandle(SpellProbe)->GetPrimaryInstance()->CanActivateAbility(SpellProbe, ASC->AbilityActorInfo.Get()));
			Test->TestTrue(TEXT("可中断原生活跃能力启动"), ASC->TryActivateAbility(Holding));
			auto Silence = Apply(Sources[0], ASC, Status(MobaGameplayTags::State_Silence));
			Test->TestFalse(TEXT("Silence禁止法术"), Ability->CanActivateAbility(Handle, ASC->AbilityActorInfo.Get()));
			Test->TestFalse(
				TEXT("非Self未激活法术被Silence阻断"), ASC->FindAbilitySpecFromHandle(SpellProbe)->GetPrimaryInstance()->CanActivateAbility(SpellProbe, ASC->AbilityActorInfo.Get()));
			auto Attack = ASC->GiveAbility(FGameplayAbilitySpec(UTargetDamageAbility::StaticClass(), 1));
			auto* AttackSpec = ASC->FindAbilitySpecFromHandle(Attack);
			Test->TestTrue(TEXT("Silence保留basic attack"), AttackSpec->GetPrimaryInstance()->CanActivateAbility(Attack, ASC->AbilityActorInfo.Get()));
			auto Stun = Apply(Sources[0], ASC, Status(MobaGameplayTags::State_Stun, true));
			Test->TestFalse(TEXT("Stun真正取消可中断主动能力"), ASC->FindAbilitySpecFromHandle(Holding)->IsActive());
			Test->TestFalse(TEXT("非Self原生主动能力同样被Stun阻断"), ASC->TryActivateAbility(Holding));
			Test->TestFalse(TEXT("Stun禁止basic主动"), AttackSpec->GetPrimaryInstance()->CanActivateAbility(Attack, ASC->AbilityActorInfo.Get()));
			auto Cooldown = Apply(Sources[0], ASC, GetDefault<UManagedAbilityCooldownEffect>());
			Test->TestEqual(TEXT("Basic移除Root/Silence保留Stun"), MobaCombat::Dispel(ASC, false, false, true), 2);
			Test->TestNotNull(TEXT("Strong Stun保留"), ASC->GetActiveGameplayEffect(Stun));
			Test->TestFalse(
				TEXT("Silence移除后非Self法术仍被Stun阻断"), ASC->FindAbilitySpecFromHandle(SpellProbe)->GetPrimaryInstance()->CanActivateAbility(SpellProbe, ASC->AbilityActorInfo.Get()));
			Test->TestEqual(TEXT("限制并集仍禁止移动"), Hero->GetCharacterMovement()->GetMaxSpeed(), 0.f);
			Test->TestEqual(TEXT("Strong移除Stun"), MobaCombat::Dispel(ASC, true, true, true), 1);
			Test->TestNotNull(TEXT("冷却不可驱散"), ASC->GetActiveGameplayEffect(Cooldown));
			Test->TestTrue(
				TEXT("Stun移除后非Self法术恢复原生激活资格"), ASC->FindAbilitySpecFromHandle(SpellProbe)->GetPrimaryInstance()->CanActivateAbility(SpellProbe, ASC->AbilityActorInfo.Get()));
			Test->TestTrue(TEXT("控制消失恢复移动"), Hero->GetCharacterMovement()->GetMaxSpeed() > 0);
			auto* Slow = Status(MobaGameplayTags::State_Slow);
			auto& Mod = Slow->Modifiers.AddDefaulted_GetRef();
			Mod.Attribute = UHeroAttributeSet::GetMoveSpeedAttribute();
			Mod.ModifierOp = EGameplayModOp::MultiplyCompound;
			Mod.ModifierMagnitude = FScalableFloat(.6f);
			auto Weak = Apply(Sources[0], ASC, Slow);
			auto* Strong = Status(MobaGameplayTags::State_Slow);
			Strong->Modifiers = Slow->Modifiers;
			Strong->Modifiers[0].ModifierMagnitude = FScalableFloat(.1f);
			auto StrongHandle = Apply(Sources[1], ASC, Strong);
			Test->TestEqual(TEXT("最强Slow下限0.2"), Hero->GetAttributes()->GetMoveSpeed(), .2f);
			ASC->RemoveActiveGameplayEffect(StrongHandle);
			Test->TestEqual(TEXT("强Slow移除恢复剩余0.6"), Hero->GetAttributes()->GetMoveSpeed(), .6f);
			ASC->RemoveActiveGameplayEffect(Weak);
			Test->TestEqual(TEXT("全部Slow移除恢复1"), Hero->GetAttributes()->GetMoveSpeed(), 1.f);
			auto* Equipment = NewObject<UAbilitySetDefinition>(World);
			Objects.Emplace(Equipment);
			Equipment->Effects.Add(UCombatEquipmentTestEffect::StaticClass());
			const FGuid SourceA = FGuid::NewGuid(), SourceB = FGuid::NewGuid();
			Test->TestTrue(TEXT("可撤销装备来源A"), ASC->GrantFromSource(Equipment, SourceA));
			Test->TestTrue(TEXT("可撤销装备来源B"), ASC->GrantFromSource(Equipment, SourceB));
			Test->TestTrue(TEXT("来源被动真实连乘0.81"), FMath::IsNearlyEqual(Hero->GetAttributes()->GetDamageTakenMultiplier(), .81f, .0001f));
			Test->TestEqual(TEXT("Strong不移除装备及冷却"), MobaCombat::Dispel(ASC, true, true, true), 0);
			ASC->RemoveSource(SourceA);
			Test->TestTrue(TEXT("撤A只留B乘区0.9"), FMath::IsNearlyEqual(Hero->GetAttributes()->GetDamageTakenMultiplier(), .9f, .0001f));
			ASC->RemoveSource(SourceB);
			Test->TestEqual(TEXT("装备撤回归1"), Hero->GetAttributes()->GetDamageTakenMultiplier(), 1.f);
			Slow->DurationMagnitude = FScalableFloat(1);
			Strong->DurationMagnitude = FScalableFloat(.3f);
			Apply(Sources[0], ASC, Slow);
			Apply(Sources[1], ASC, Strong);
		}

		FAutomationTestBase* Test;


	public:
		UWorld* World = nullptr;

		AMatchGameState* Match = nullptr;

		AHeroCharacter* Hero = nullptr;

		ATargetCombatant* Target = nullptr;

		ATargetCombatant* HealTarget = nullptr;

		ATargetCombatant* Untargetable = nullptr;

		ATargetCombatant* ShieldTarget = nullptr;

		TArray<UAbilitySystemComponent*> Sources;

		TArray<TStrongObjectPtr<UObject>> Objects;

		FActiveGameplayEffectHandle DotA, DotB, Hot, ShieldHandle, ImmuneHandle;

		float Elapsed = 0;

		int32 Phase = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatEffectsWorldTest, "Moba.Combat.Effects.World", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCombatEffectsWorldTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(FCombatEffectsScenario(this));
	return true;
}
#endif
