#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/DataValidation.h"
#include "Engine/Engine.h"
#include "Engine/AssetManager.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/SavePackage.h"
#include "Experience/ExperienceFeatureSubsystem.h"
#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Combat/CombatStatusEffects.h"
#include "Combat/CombatEffectParameters.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/TargetDamageAbility.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "MobaGameplayTags.h"

namespace
{
	const TCHAR* EffectNames[] = {TEXT("PhysicalDamage"), TEXT("MagicalDamage"), TEXT("TrueDamage"), TEXT("Heal"), TEXT("Shield"), TEXT("DamageOverTime"), TEXT("HealOverTime"),
		TEXT("StackingBuff"), TEXT("Stun"), TEXT("Root"), TEXT("Silence"), TEXT("Slow"), TEXT("Invulnerable"), TEXT("Untargetable")};
	FString EffectPath(const TCHAR* Name)
	{
		return FString(TEXT("/MobaClassic/Tests/Batch0811/Effects/GE_")) + Name;
	}

	float StaticMagnitude(const FGameplayEffectModifierMagnitude& Magnitude)
	{
		float Value = 0;
		Magnitude.GetStaticMagnitudeIfPossible(1, Value);
		return Value;
	}

	class FCombatEffectAssets final : public IAutomationLatentCommand
	{

	public:
		explicit FCombatEffectAssets(FAutomationTestBase* InTest) : Test(InTest) {}

		~FCombatEffectAssets() override
		{
			if (Lease.IsValid())
			{
				GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>()->Release(URL, Lease);
			}
		}


	private:
		void Save(UObject* Object)
		{
			Object->MarkPackageDirty();
			Object->PostEditChange();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			const FString Filename = FPackageName::LongPackageNameToFilename(Object->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
			Test->TestTrue(TEXT("精确manifest保存"), UPackage::SavePackage(Object->GetOutermost(), Object, *Filename, Args));
		}

		UBlueprint* Blueprint(const FString& Path, UClass* Parent)
		{
			if (auto* Existing = LoadObject<UBlueprint>(nullptr, *(Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path)), nullptr, LOAD_NoWarn))
			{
				Test->TestEqual(TEXT("manifest既有父类"), Existing->ParentClass.Get(), Parent);
				return Existing;
			}

			auto* Package = CreatePackage(*Path);
			auto* BP = FKismetEditorUtilities::CreateBlueprint(
				Parent, Package, *FPackageName::GetLongPackageAssetName(Path), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			FAssetRegistryModule::AssetCreated(BP);
			return BP;
		}


	public:
		bool Update() override
		{
			auto* Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
			if (!Lease.IsValid())
			{
				Lease = FGuid::NewGuid();
				Start = FPlatformTime::Seconds();
				Test->TestTrue(TEXT("Feature acquire"), Features->Acquire(TEXT("MobaClassic"), Lease, URL));
				return false;
			}

			if (FPlatformTime::Seconds() - Start > 60)
			{
				Test->AddError(TEXT("Combat资产Feature加载超时"));
				return true;
			}

			if (Features->GetState(URL) != EExperienceFeatureState::Loaded || UAssetManager::Get().GetAssetRegistry().IsGathering())
			{
				return false;
			}

			if (FParse::Param(FCommandLine::Get(), TEXT("MobaCreateCombatAssets")))
			{
				Create();
			}

			Verify();
			return true;
		}


	private:
		void Verify()
		{
			auto* Experience = LoadObject<UExperienceDefinition>(nullptr, TEXT("/MobaClassic/Definitions/Batch0811/Experience_Batch0811.Experience_Batch0811"));
			if (!Test->TestNotNull(TEXT("Experience磁盘读回"), Experience))
			{
				return;
			}

			Test->TestTrue(TEXT("显式新策略"), Experience->CombatRules.DamagePolicy == ECombatDamagePolicy::Standard);
			Test->TestEqual(TEXT("14 GE完整闭包"), Experience->CombatEffects.Num(), 14);
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(EffectNames); ++Index)
			{
				UClass* Class = LoadClass<UGameplayEffect>(nullptr, *(EffectPath(EffectNames[Index]) + TEXT(".") + TEXT("GE_") + EffectNames[Index] + TEXT("_C")));
				if (!Test->TestNotNull(TEXT("每项真实GE可加载"), Class))
				{
					continue;
				}

				const auto* Effect = Class->GetDefaultObject<UGameplayEffect>();
				Test->TestTrue(TEXT("manifest闭包精确引用"), Experience->CombatEffects.Contains(Class));
				FDataValidationContext Context;
				Test->TestTrue(TEXT("真实GE原生Validation"), Effect->IsDataValid(Context) == EDataValidationResult::Valid);
				if (Index < 3)
				{
					const FGameplayTag Tags[] = {MobaGameplayTags::Damage_Physical, MobaGameplayTags::Damage_Magical, MobaGameplayTags::Damage_True};
					Test->TestTrue(TEXT("三分类真实资产tag"), Effect->GetAssetTags().HasTagExact(Tags[Index]));
					Test->TestEqual(TEXT("示例基础60"), StaticMagnitude(Effect->FindComponent<UCombatDamageParametersComponent>()->BaseDamage), 60.f);
				}

				if (Index == 3)
				{
					Test->TestEqual(TEXT("Heal25保存读回"), StaticMagnitude(Effect->FindComponent<UCombatHealingParametersComponent>()->HealAmount), 25.f);
				}

				if (Index == 4)
				{
					Test->TestEqual(TEXT("Shield30保存读回"), StaticMagnitude(Effect->FindComponent<UCombatShieldEffectComponent>()->ShieldAmount), 30.f);
					float Duration = 0;
					Effect->DurationMagnitude.GetStaticMagnitudeIfPossible(1, Duration);
					Test->TestEqual(TEXT("Shield5秒保存读回"), Duration, 5.f);
				}

				if (Index == 5 || Index == 6)
				{
					Test->TestEqual(TEXT("周期1秒"), Effect->Period.GetValueAtLevel(1), 1.f);
					Test->TestFalse(TEXT("首次不立即执行"), Effect->bExecutePeriodicEffectOnApplication);
					Test->TestTrue(TEXT("按来源聚合"), Effect->GetStackingType() == EGameplayEffectStackingType::AggregateBySource);
					Test->TestTrue(TEXT("刷新不重置周期"), Effect->StackPeriodResetPolicy == EGameplayEffectStackingPeriodPolicy::NeverReset);
				}

				if (Index == 7)
				{
					Test->TestEqual(TEXT("最多3层"), Effect->StackLimitCount, 3);
				}

				if (Index >= 4)
				{
					Test->TestTrue(TEXT("明确死亡清理"), Effect->GetGrantedTags().HasTagExact(MobaGameplayTags::Effect_RemoveOnDeath));
				}

				if (Index >= 8)
				{
					Test->TestTrue(
						TEXT("Stun强驱散，其余Basic"), Effect->GetGrantedTags().HasTagExact(Index == 8 ? MobaGameplayTags::Effect_Dispel_Strong : MobaGameplayTags::Effect_Dispel_Basic));
				}

				if (Index == 8 || Index == 10)
				{
					Test->TestTrue(TEXT("原生GAS阻断任意有对应标签的能力"),
						Effect->GetBlockedAbilityTags().HasTagExact(Index == 8 ? MobaGameplayTags::Ability_Active : MobaGameplayTags::Ability_Spell));
				}
			}

			Test->TestTrue(TEXT("PrimaryAssetId精确登记"), UAssetManager::Get().GetPrimaryAssetIdForPath(FSoftObjectPath(Experience)) == Experience->GetPrimaryAssetId());
			FDataValidationContext Context;
			Test->TestTrue(TEXT("Experience原生Validation"), Experience->IsDataValid(Context) == EDataValidationResult::Valid);
		}

		void Create()
		{
			TArray<TSoftClassPtr<UGameplayEffect>> Classes;
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(EffectNames); ++Index)
			{
				auto* BP = Blueprint(EffectPath(EffectNames[Index]), UGameplayEffect::StaticClass());
				auto* Effect = CastChecked<UGameplayEffect>(BP->GeneratedClass->GetDefaultObject());
				Effect->DurationPolicy = Index < 4 ? EGameplayEffectDurationType::Instant : EGameplayEffectDurationType::HasDuration;
				if (Index >= 4)
				{
					Effect->DurationMagnitude = FScalableFloat(5);
				}

				if (Index < 3 || Index == 5)
				{
					Effect->Executions.SetNum(1);
					Effect->Executions[0].CalculationClass = UCombatDamageExecution::StaticClass();
					Effect->FindOrAddComponent<UCombatDamageParametersComponent>().BaseDamage = FScalableFloat(Index == 5 ? 5 : 60);
				}

				if (Index == 3 || Index == 6)
				{
					Effect->Executions.SetNum(1);
					Effect->Executions[0].CalculationClass = UCombatHealingExecution::StaticClass();
					Effect->FindOrAddComponent<UCombatHealingParametersComponent>().HealAmount = FScalableFloat(Index == 6 ? 5 : 25);
				}

				if (Index == 4)
				{
					Effect->FindOrAddComponent<UCombatShieldEffectComponent>().ShieldAmount = FScalableFloat(30);
				}

				if (Index >= 4 && Index <= 7)
				{
					FInheritedTagContainer Tags;
					Tags.AddTag(MobaGameplayTags::Effect_RemoveOnDeath);
					Tags.AddTag(MobaGameplayTags::Effect_Dispel_Basic);
					Tags.AddTag(Index == 5 ? MobaGameplayTags::Effect_Harmful : MobaGameplayTags::Effect_Beneficial);
					if (Index == 4)
					{
						Tags.AddTag(MobaGameplayTags::Effect_Shield);
					}

					Effect->FindOrAddComponent<UTargetTagsGameplayEffectComponent>().SetAndApplyTargetTagChanges(Tags);
				}

				if (Index >= 5 && Index <= 7)
				{
					// 5.8 Installed未导出setter，保持原生公开字段的兼容写法。
					Effect->StackingType = EGameplayEffectStackingType::AggregateBySource;
					Effect->StackLimitCount = Index == 7 ? 3 : 1;
					Effect->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
					Effect->StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::NeverReset;
					Effect->bExecutePeriodicEffectOnApplication = false;
				}

				if (Index == 5 || Index == 6)
				{
					Effect->DurationMagnitude = FScalableFloat(3);
					Effect->Period = FScalableFloat(1);
					Effect->FindOrAddComponent<UCombatStateEffectComponent>();
				}

				if (Index == 7)
				{
					Effect->Modifiers.SetNum(1);
					auto& Modifier = Effect->Modifiers[0];
					Modifier.Attribute = UHeroAttributeSet::GetArmorAttribute();
					Modifier.ModifierOp = EGameplayModOp::Additive;
					Modifier.ModifierMagnitude = FScalableFloat(10);
				}

				if (Index >= 8)
				{
					Effect->FindOrAddComponent<UCombatStateEffectComponent>();
				}

				if (Index < 3 || Index == 5)
				{
					FInheritedTagContainer AssetTags;
					AssetTags.AddTag(Index == 0 ? MobaGameplayTags::Damage_Physical : Index == 1 ? MobaGameplayTags::Damage_Magical : MobaGameplayTags::Damage_True);
					Effect->FindOrAddComponent<UAssetTagsGameplayEffectComponent>().SetAndApplyAssetTagChanges(AssetTags);
					Effect->FindOrAddComponent<UCombatDamageParametersComponent>().BaseDamage = FScalableFloat(Index == 5 ? 5 : 60);
				}

				if (Index >= 8)
				{
					const FGameplayTag States[] = {MobaGameplayTags::State_Stun, MobaGameplayTags::State_Root, MobaGameplayTags::State_Silence, MobaGameplayTags::State_Slow,
						MobaGameplayTags::State_Invulnerable, MobaGameplayTags::State_Untargetable};
					FInheritedTagContainer Tags;
					Tags.AddTag(States[Index - 8]);
					Tags.AddTag(MobaGameplayTags::Effect_RemoveOnDeath);
					Tags.AddTag(Index >= 12 ? MobaGameplayTags::Effect_Beneficial : MobaGameplayTags::Effect_Harmful);
					Tags.AddTag(Index == 8 ? MobaGameplayTags::Effect_Dispel_Strong : MobaGameplayTags::Effect_Dispel_Basic);
					Effect->FindOrAddComponent<UTargetTagsGameplayEffectComponent>().SetAndApplyTargetTagChanges(Tags);
					if (Index == 8 || Index == 10)
					{
						FInheritedTagContainer Blocked;
						Blocked.AddTag(Index == 8 ? MobaGameplayTags::Ability_Active : MobaGameplayTags::Ability_Spell);
						Effect->FindOrAddComponent<UBlockAbilityTagsGameplayEffectComponent>().SetAndApplyBlockedAbilityTagChanges(Blocked);
					}

					if (Index == 11)
					{
						Effect->Modifiers.Reset();
						auto& Mod = Effect->Modifiers.AddDefaulted_GetRef();
						Mod.Attribute = UHeroAttributeSet::GetMoveSpeedAttribute();
						Mod.ModifierOp = EGameplayModOp::MultiplyCompound;
						Mod.ModifierMagnitude = FScalableFloat(0.6f);
					}
				}

				FKismetEditorUtilities::CompileBlueprint(BP);
				Save(BP);
				Classes.Add(BP->GeneratedClass.Get());
			}

			const FString Base(TEXT("/MobaClassic/Tests/Batch0811/Abilities/"));
			for (int32 Index = 0; Index < 2; ++Index)
			{
				auto* BP = Blueprint(Base + (Index ? TEXT("GA_SelfShield") : TEXT("GA_SelfHeal")), USelfCombatEffectAbility::StaticClass());
				auto* Ability = CastChecked<USelfCombatEffectAbility>(BP->GeneratedClass->GetDefaultObject());
				Ability->Effect = Classes[Index ? 4 : 3].Get();
				FKismetEditorUtilities::CompileBlueprint(BP);
				Save(BP);
			}

			const TCHAR* Names[] = {TEXT("GA_UnitInstant"), TEXT("GA_DirectionProjectile"), TEXT("GA_PointArea")};
			for (int32 Index = 0; Index < 3; ++Index)
			{
				const FString Path = Base + Names[Index];
				auto* BP = LoadObject<UBlueprint>(nullptr, *(Path + TEXT(".") + Names[Index]));
				auto* Ability = CastChecked<UTargetDamageAbility>(BP->GeneratedClass->GetDefaultObject());
				Ability->DamageEffect = Classes[Index].Get();
				Ability->DamageType = Index == 0 ? MobaGameplayTags::Damage_Physical : Index == 1 ? MobaGameplayTags::Damage_Magical : MobaGameplayTags::Damage_True;
				FGameplayTagContainer Tags(MobaGameplayTags::Ability_Active);
				Tags.AddTag(MobaGameplayTags::Ability_CancelOnDeath);
				Tags.AddTag(Index == 0 ? MobaGameplayTags::Ability_BasicAttack : MobaGameplayTags::Ability_Spell);
				Ability->EditorGetAssetTags() = Tags;
				FKismetEditorUtilities::CompileBlueprint(BP);
				Save(BP);
			}

			auto* Set = LoadObject<UAbilitySetDefinition>(nullptr, TEXT("/MobaClassic/Definitions/Batch0811/AbilitySet_Managed.AbilitySet_Managed"));
			for (int32 Index = 0; Index < 2; ++Index)
			{
				const FGameplayTag Tag = Index ? MobaGameplayTags::Input_Slot_Shield : MobaGameplayTags::Input_Slot_Heal;
				if (!Set->Abilities.ContainsByPredicate([Tag](const auto& Entry) { return Entry.Slot == Tag; }))
				{
					auto& Entry = Set->Abilities.AddDefaulted_GetRef();
					Entry.Slot = Tag;
					Entry.MaxLevel = 3;
					Entry.Ability = TSoftClassPtr<UGameplayAbility>(FSoftObjectPath(Base + (Index ? TEXT("GA_SelfShield.GA_SelfShield_C") : TEXT("GA_SelfHeal.GA_SelfHeal_C"))));
				}
			}

			Save(Set);
			auto* Experience = LoadObject<UExperienceDefinition>(nullptr, TEXT("/MobaClassic/Definitions/Batch0811/Experience_Batch0811.Experience_Batch0811"));
			Experience->CombatRules.DamagePolicy = ECombatDamagePolicy::Standard;
			Experience->CombatEffects = Classes;
			Save(Experience);
		}


	private:
		FAutomationTestBase* Test;

		FGuid Lease;

		FString URL;

		double Start = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatEffectAssetTest, "Moba.Combat.Effects.AssetValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCombatEffectAssetTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(FCombatEffectAssets(this));
	return true;
}
#endif
