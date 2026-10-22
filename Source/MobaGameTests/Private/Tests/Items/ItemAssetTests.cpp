#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/DataValidation.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Algo/Count.h"
#include "Engine/Engine.h"
#include "Engine/AssetManager.h"
#include "Engine/Blueprint.h"
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#include "GameFeatureData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Experience/ExperienceFeatureSubsystem.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"
#include "Experience/ExperienceDefinitions.h"
#include "Items/ItemDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Combat/CombatStatusEffects.h"
#include "Combat/HeroAttributeSet.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "MobaGameplayTags.h"

namespace
{
	const TCHAR* ItemNames[] = {TEXT("ArmorPlate"), TEXT("FocusCharm"), TEXT("GuardOrb"), TEXT("HealingPotion"), TEXT("CompositeGuard")};
	const int32 ItemPrices[] = {100, 150, 200, 50, 350};
	const FString DefinitionRoot(TEXT("/MobaClassic/Definitions/Batch0811/"));
	const FString ItemRoot(TEXT("/MobaClassic/Tests/Batch0811/Items/"));
	const FString AbilityRoot(TEXT("/MobaClassic/Tests/Batch0811/Abilities/"));
	const FString EffectRoot(TEXT("/MobaClassic/Tests/Batch0811/Effects/"));

	FString ObjectPath(const FString& Package)
	{
		return Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package);
	}

	class FItemAssets final : public IAutomationLatentCommand
	{

	public:
		explicit FItemAssets(FAutomationTestBase* InTest) : Test(InTest) {}

		~FItemAssets() override { Release(); }


	public:
		bool Update() override
		{
			auto* Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
			if (bReleased)
			{
				if (FPlatformTime::Seconds() - Started > 60)
				{
					Test->AddError(TEXT("Item Feature租约释放/驻留检查超时"));
					return true;
				}

				const EGameFeaturePluginState NativeState = UGameFeaturesSubsystem::Get().GetPluginState(URL);
				const bool bReleasedState = GIsEditor
					? Features->GetState(URL) == EExperienceFeatureState::Loaded && (NativeState == EGameFeaturePluginState::Loaded || NativeState == EGameFeaturePluginState::Active)
					: Features->GetState(URL) == EExperienceFeatureState::Absent && UGameFeaturesSubsystem::Get().GetGameFeatureDataForRegisteredPluginByURL(URL) == nullptr;
				if (Features->GetLeaseCount(URL) != 0 || !bReleasedState)
				{
					return false;
				}

				Test->TestEqual(TEXT("资产验证本轮租约已全部归还"), Features->GetLeaseCount(URL), 0);

				if (VerifiedCycles == 2)
				{
					return true;
				}

				bReleased = false;
			}

			if (!Lease.IsValid())
			{
				Lease = FGuid::NewGuid();
				Started = FPlatformTime::Seconds();
				if (!Test->TestTrue(TEXT("Item Feature acquire"), Features->Acquire(TEXT("MobaClassic"), Lease, URL)))
				{
					return true;
				}

				Test->TestEqual(TEXT("隔离资产验证只有本轮一个租约"), Features->GetLeaseCount(URL), 1);
				return false;
			}

			if (FPlatformTime::Seconds() - Started > 60)
			{
				Test->AddError(TEXT("Item Feature加载超时"));
				return true;
			}

			if (Features->GetState(URL) == EExperienceFeatureState::Failed)
			{
				Test->AddError(TEXT("Item Feature加载失败"));
				return true;
			}

			if (Features->GetState(URL) != EExperienceFeatureState::Loaded || UAssetManager::Get().GetAssetRegistry().IsGathering())
			{
				return false;
			}

			if (!bCreated && FParse::Param(FCommandLine::Get(), TEXT("MobaCreateItemAssets")))
			{
				bCreated = true;
				// 宿主Playlist跨Feature软引用由自身合同校验；所有Feature资产在读回阶段显式原生Validation。
				// 不排队全局保存校验，避免卸载竞态及通用引用限制误判既定宿主挂载合同。
				{
					FScopedDisableValidateOnSave Scope;
					if (!Create())
					{
						return true;
					}
				}

				Release();
				return false;
			}

			Verify();
			++VerifiedCycles;
			Release();
			return false;
		}


	private:
		void Release()
		{
			if (!Lease.IsValid())
			{
				return;
			}

			GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>()->Release(URL, Lease);
			Lease.Invalidate();
			bReleased = true;
			Started = FPlatformTime::Seconds();
		}

		bool Save(UObject* Object)
		{
			Object->MarkPackageDirty();
			Object->PostEditChange();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			const FString Filename = FPackageName::LongPackageNameToFilename(Object->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
			return Test->TestTrue(TEXT("仅保存批准manifest"), UPackage::SavePackage(Object->GetOutermost(), Object, *Filename, Args));
		}

		template <typename T> T* Data(const FString& Path)
		{
			if (UObject* Existing = LoadObject<UObject>(nullptr, *ObjectPath(Path), nullptr, LOAD_NoWarn))
			{
				T* Typed = Cast<T>(Existing);
				Test->TestNotNull(TEXT("manifest资产类型一致"), Typed);
				return Typed;
			}

			auto* Object = NewObject<T>(CreatePackage(*Path), *FPackageName::GetLongPackageAssetName(Path), RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Object);
			return Object;
		}

		UBlueprint* Blueprint(const FString& Path, UClass* Parent)
		{
			if (auto* Existing = LoadObject<UBlueprint>(nullptr, *ObjectPath(Path), nullptr, LOAD_NoWarn))
			{
				if (!Test->TestEqual(TEXT("manifest蓝图父类一致"), Existing->ParentClass.Get(), Parent))
				{
					return nullptr;
				}

				return Existing;
			}

			auto* BP = FKismetEditorUtilities::CreateBlueprint(
				Parent, CreatePackage(*Path), *FPackageName::GetLongPackageAssetName(Path), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (BP)
			{
				FAssetRegistryModule::AssetCreated(BP);
			}

			return BP;
		}

		bool Create()
		{
			if (!CreateItemScan())
			{
				return false;
			}

			auto* Experience = LoadObject<UExperienceDefinition>(nullptr, *ObjectPath(DefinitionRoot + TEXT("Experience_Batch0811")));
			auto* BasePlaylist = LoadObject<UPlaylistDefinition>(nullptr, TEXT("/Game/Playlists/Playlist_Batch0811.Playlist_Batch0811"));
			if (!Test->TestNotNull(TEXT("既有Experience必须存在"), Experience) || !Test->TestNotNull(TEXT("既有Playlist必须存在"), BasePlaylist))
			{
				return false;
			}

			UClass* PassiveClasses[2] = {};
			for (int32 Index = 0; Index < 2; ++Index)
			{
				auto* BP = Blueprint(EffectRoot + (Index ? TEXT("GE_ItemDamageReduction") : TEXT("GE_ItemArmor")), UGameplayEffect::StaticClass());
				if (!BP)
				{
					return false;
				}

				auto* Effect = CastChecked<UGameplayEffect>(BP->GeneratedClass->GetDefaultObject());
				Effect->DurationPolicy = EGameplayEffectDurationType::Infinite;
				Effect->Modifiers.Reset();
				auto& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
				Modifier.Attribute = Index ? UHeroAttributeSet::GetDamageTakenMultiplierAttribute() : UHeroAttributeSet::GetArmorAttribute();
				Modifier.ModifierOp = Index ? EGameplayModOp::MultiplyCompound : EGameplayModOp::Additive;
				Modifier.ModifierMagnitude = FScalableFloat(Index ? 0.9f : 10.f);
				FInheritedTagContainer Tags;
				Tags.AddTag(MobaGameplayTags::Effect_Dispel_Undispellable);
				Effect->FindOrAddComponent<UTargetTagsGameplayEffectComponent>().SetAndApplyTargetTagChanges(Tags);
				FKismetEditorUtilities::CompileBlueprint(BP);
				if (!Save(BP))
				{
					return false;
				}

				PassiveClasses[Index] = BP->GeneratedClass;
			}

			UClass* AbilityClasses[2] = {};
			for (int32 Index = 0; Index < 2; ++Index)
			{
				auto* CooldownBP = Blueprint(EffectRoot + (Index ? TEXT("GE_HealItemCooldown") : TEXT("GE_ShieldItemCooldown")), UGameplayEffect::StaticClass());
				if (!CooldownBP)
				{
					return false;
				}

				auto* Cooldown = CastChecked<UGameplayEffect>(CooldownBP->GeneratedClass->GetDefaultObject());
				Cooldown->DurationPolicy = EGameplayEffectDurationType::HasDuration;
				Cooldown->DurationMagnitude = FScalableFloat(5);
				FInheritedTagContainer Tags;
				Tags.AddTag(Index ? MobaGameplayTags::Cooldown_ItemHeal : MobaGameplayTags::Cooldown_ItemShield);
				Cooldown->FindOrAddComponent<UTargetTagsGameplayEffectComponent>().SetAndApplyTargetTagChanges(Tags);
				FKismetEditorUtilities::CompileBlueprint(CooldownBP);
				if (!Save(CooldownBP))
				{
					return false;
				}

				auto* BP = Blueprint(AbilityRoot + (Index ? TEXT("GA_ItemHeal") : TEXT("GA_ItemShield")), USelfCombatEffectAbility::StaticClass());
				if (!BP)
				{
					return false;
				}

				auto* Ability = CastChecked<USelfCombatEffectAbility>(BP->GeneratedClass->GetDefaultObject());
				Ability->Effect = LoadClass<UGameplayEffect>(nullptr, *(ObjectPath(EffectRoot + (Index ? TEXT("GE_Heal") : TEXT("GE_Shield"))) + TEXT("_C")));
				if (!Test->TestNotNull(TEXT("复用TASK-010实际GE"), Ability->Effect.Get()))
				{
					return false;
				}

				auto* CooldownProperty = FindFProperty<FClassProperty>(UGameplayAbility::StaticClass(), TEXT("CooldownGameplayEffectClass"));
				auto* CostProperty = FindFProperty<FClassProperty>(UGameplayAbility::StaticClass(), TEXT("CostGameplayEffectClass"));
				if (!CooldownProperty || !CostProperty)
				{
					return false;
				}

				CooldownProperty->SetObjectPropertyValue_InContainer(Ability, CooldownBP->GeneratedClass);
				CostProperty->SetObjectPropertyValue_InContainer(Ability, nullptr);
				FKismetEditorUtilities::CompileBlueprint(BP);
				if (!Save(BP))
				{
					return false;
				}

				AbilityClasses[Index] = BP->GeneratedClass;
			}

			TArray<UItemDefinition*> Items;
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(ItemNames); ++Index)
			{
				auto* Set = Data<UAbilitySetDefinition>(DefinitionRoot + TEXT("AbilitySet_") + ItemNames[Index]);
				auto* Item = Data<UItemDefinition>(ItemRoot + TEXT("Item_") + ItemNames[Index]);
				if (!Set || !Item)
				{
					return false;
				}

				Set->bAllowCopy = false;
				Set->Abilities.Reset();
				Set->Effects.Reset();
				if (Index == 0 || Index == 4)
				{
					Set->Effects.Add(PassiveClasses[0]);
				}

				if (Index == 1 || Index == 4)
				{
					Set->Effects.Add(PassiveClasses[1]);
				}

				if (Index == 2 || Index == 3)
				{
					auto& Entry = Set->Abilities.AddDefaulted_GetRef();
					Entry.Ability = AbilityClasses[Index - 2];
					Entry.Level = 1;
					Entry.MaxLevel = 1;
				}

				if (!Save(Set))
				{
					return false;
				}

				Item->ItemId = ItemNames[Index];
				Item->Price = ItemPrices[Index];
				Item->bConsumable = Index == 3;
				Item->MaxStack = Index == 3 ? 3 : 1;
				Item->AbilitySet = Set;
				Item->Recipe.Reset();
				if (Index == 4)
				{
					Item->Recipe.Add(Items[0]);
					Item->Recipe.Add(Items[1]);
				}

				if (!Save(Item))
				{
					return false;
				}

				Items.Add(Item);
			}

			auto* Catalog = Data<UItemCatalogDefinition>(ItemRoot + TEXT("ItemCatalog_Batch0811"));
			if (!Catalog)
			{
				return false;
			}

			Catalog->Items.Reset();
			for (auto* Item : Items)
			{
				Catalog->Items.Add(Item);
			}

			if (!Save(Catalog))
			{
				return false;
			}

			Experience->bItemsEnabled = true;
			Experience->ItemCatalog = Catalog;
			Experience->InitialGold = 1000;
			Experience->ShopCenter = FVector::ZeroVector;
			Experience->ShopRadius = 1000;
			if (!Save(Experience))
			{
				return false;
			}

			auto* NoItems = Data<UExperienceDefinition>(DefinitionRoot + TEXT("Experience_NoItems"));
			auto* Playlist = Data<UPlaylistDefinition>(TEXT("/Game/Playlists/Playlist_NoItems"));
			if (!NoItems || !Playlist)
			{
				return false;
			}

			NoItems->Hero = Experience->Hero;
			NoItems->InputProfiles = Experience->InputProfiles;
			NoItems->HeroVariants = Experience->HeroVariants;
			NoItems->AllowedInputProfiles = Experience->AllowedInputProfiles;
			NoItems->Teams = Experience->Teams;
			NoItems->Players = Experience->Players;
			NoItems->CombatRules = Experience->CombatRules;
			NoItems->CombatEffects = Experience->CombatEffects;
			NoItems->bItemsEnabled = false;
			NoItems->ItemCatalog.Reset();
			NoItems->InitialGold = 1000;
			NoItems->ShopCenter = FVector::ZeroVector;
			NoItems->ShopRadius = 1000;
			Playlist->FeatureName = BasePlaylist->FeatureName;
			Playlist->Map = BasePlaylist->Map;
			Playlist->Experience = NoItems;
			return Save(NoItems) && Save(Playlist);
		}

		void Invalid(UObject* Object, const TCHAR* Label)
		{
			FDataValidationContext Context;
			Test->TestTrue(Label, Object->IsDataValid(Context) == EDataValidationResult::Invalid && Context.GetNumErrors() > 0);
		}

		void Verify()
		{
			if (!VerifyItemScan())
			{
				return;
			}

			auto* Catalog = LoadObject<UItemCatalogDefinition>(nullptr, *ObjectPath(ItemRoot + TEXT("ItemCatalog_Batch0811")));
			if (!Test->TestNotNull(TEXT("manifest Catalog可加载"), Catalog))
			{
				return;
			}

			FDataValidationContext Context;
			if (!Test->TestTrue(TEXT("Catalog完整依赖原生Validation"), Catalog->IsDataValid(Context) == EDataValidationResult::Valid))
			{
				return;
			}

			Test->TestEqual(TEXT("五件精确目录"), Catalog->Items.Num(), 5);
			TArray<UItemDefinition*> Items;
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(ItemNames); ++Index)
			{
				auto* Item = const_cast<UItemDefinition*>(Catalog->FindItem(ItemNames[Index]));
				if (!Test->TestNotNull(TEXT("目录ID可查"), Item))
				{
					return;
				}

				Items.Add(Item);
				const FPrimaryAssetId ExpectedItem(TEXT("Item"), *FString::Printf(TEXT("Item_%s"), ItemNames[Index]));
				Test->TestEqual(TEXT("目录短键对应唯一Item资产名"), Item->GetFName(), ExpectedItem.PrimaryAssetName);
				Test->TestEqual(TEXT("Item固定PrimaryAsset身份"), Item->GetPrimaryAssetId(), ExpectedItem);
				Test->TestEqual(TEXT("Item原生扫描正向身份"), UAssetManager::Get().GetPrimaryAssetIdForPath(FSoftObjectPath(Item)), ExpectedItem);
				Test->TestEqual(TEXT("Item原生扫描反向唯一路径"), UAssetManager::Get().GetPrimaryAssetPath(ExpectedItem), FSoftObjectPath(Item));
				Test->TestEqual(TEXT("Item精确manifest路径"), Item->GetOutermost()->GetName(), ItemRoot + TEXT("Item_") + ItemNames[Index]);
				Test->TestEqual(TEXT("固定价格"), Item->Price, ItemPrices[Index]);
				Test->TestEqual(TEXT("每槽数量"), Item->MaxStack, Index == 3 ? 3 : 1);
				Test->TestEqual(TEXT("消耗品策略"), Item->bConsumable, Index == 3);
				const auto* Set = Item->AbilitySet.Get();
				if (!Test->TestNotNull(TEXT("每件实际AbilitySet"), Set))
				{
					return;
				}

				const int32 ExpectedAbilities = Index == 2 || Index == 3 ? 1 : 0;
				const int32 ExpectedEffects = Index == 4 ? 2 : Index < 2 ? 1 : 0;
				if (!Test->TestEqual(TEXT("每件主动数量"), Set->Abilities.Num(), ExpectedAbilities) || !Test->TestEqual(TEXT("每件被动数量"), Set->Effects.Num(), ExpectedEffects))
				{
					return;
				}

				const FPrimaryAssetId Expected(TEXT("AbilitySet"), *FString::Printf(TEXT("AbilitySet_%s"), ItemNames[Index]));
				Test->TestEqual(TEXT("现有Feature扫描根原生登记"), UAssetManager::Get().GetPrimaryAssetIdForPath(FSoftObjectPath(Set)), Expected);
				Test->TestEqual(TEXT("唯一PrimaryAsset反向解析"), UAssetManager::Get().GetPrimaryAssetPath(Expected), FSoftObjectPath(Set));
				Test->TestEqual(TEXT("PrimaryAsset扫描路径"), Set->GetOutermost()->GetName(), DefinitionRoot + TEXT("AbilitySet_") + ItemNames[Index]);
			}

			Test->TestEqual(TEXT("Composite恰需两件"), Items[4]->Recipe.Num(), 2);
			Test->TestTrue(TEXT("Composite精确配方"), Items[4]->Recipe.Contains(Items[0]) && Items[4]->Recipe.Contains(Items[1]));
			const auto* Armor = Items[0]->AbilitySet.Get()->Effects[0].Get()->GetDefaultObject<UGameplayEffect>();
			const auto* Reduction = Items[1]->AbilitySet.Get()->Effects[0].Get()->GetDefaultObject<UGameplayEffect>();
			float ArmorValue = 0, ReductionValue = 0;
			Armor->Modifiers[0].ModifierMagnitude.GetStaticMagnitudeIfPossible(1, ArmorValue);
			Reduction->Modifiers[0].ModifierMagnitude.GetStaticMagnitudeIfPossible(1, ReductionValue);
			Test->TestEqual(TEXT("Plate+10Armor"), ArmorValue, 10.f);
			Test->TestEqual(TEXT("Charm0.9"), ReductionValue, 0.9f);
			Test->TestTrue(TEXT("Charm使用MultiplyCompound"), Reduction->Modifiers[0].ModifierOp == EGameplayModOp::MultiplyCompound);
			Test->TestEqual(TEXT("Composite两GE"), Items[4]->AbilitySet.Get()->Effects.Num(), 2);
			auto* Experience = LoadObject<UExperienceDefinition>(nullptr, *ObjectPath(DefinitionRoot + TEXT("Experience_Batch0811")));
			auto* NoItems = LoadObject<UExperienceDefinition>(nullptr, *ObjectPath(DefinitionRoot + TEXT("Experience_NoItems")));
			auto* Playlist = LoadObject<UPlaylistDefinition>(nullptr, TEXT("/Game/Playlists/Playlist_NoItems.Playlist_NoItems"));
			auto* EnabledPlaylist = LoadObject<UPlaylistDefinition>(nullptr, TEXT("/Game/Playlists/Playlist_Batch0811.Playlist_Batch0811"));
			if (!Test->TestNotNull(TEXT("Enabled Playlist"), EnabledPlaylist))
			{
				return;
			}

			if (!Test->TestNotNull(TEXT("Enabled Experience"), Experience) || !Test->TestNotNull(TEXT("NoItems Experience"), NoItems)
				|| !Test->TestNotNull(TEXT("NoItems Playlist"), Playlist))
			{
				return;
			}

			Test->TestTrue(TEXT("启用引用精确目录"), Experience->bItemsEnabled && Experience->ItemCatalog.Get() == Catalog);
			Test->TestEqual(TEXT("首次金币配置"), Experience->InitialGold, 1000);
			Test->TestEqual(TEXT("商店中心"), Experience->ShopCenter, FVector::ZeroVector);
			Test->TestEqual(TEXT("商店半径"), Experience->ShopRadius, 1000.f);
			Test->TestTrue(TEXT("Disabled资产不带目录引用"), !NoItems->bItemsEnabled && NoItems->ItemCatalog.IsNull());
			Test->TestTrue(TEXT("NoItems复用已有Map及Hero"), Playlist->Map == EnabledPlaylist->Map && NoItems->Hero == Experience->Hero);
			Test->TestTrue(TEXT("NoItems Playlist精确路由"), Playlist->Experience.Get() == NoItems && Playlist->Map == EnabledPlaylist->Map);
			auto* InvalidEconomy = DuplicateObject<UExperienceDefinition>(NoItems, GetTransientPackage());
			InvalidEconomy->InitialGold = -1;
			Invalid(InvalidEconomy, TEXT("禁用交易也拒绝负初始金币配置"));
			for (UObject* Asset : {static_cast<UObject*>(Experience), static_cast<UObject*>(NoItems), static_cast<UObject*>(Playlist)})
			{
				FDataValidationContext AssetContext;
				Test->TestTrue(TEXT("Experience与Playlist原生Validation"), Asset->IsDataValid(AssetContext) == EDataValidationResult::Valid);
				const FPrimaryAssetId Id = Asset->GetPrimaryAssetId();
				Test->TestTrue(TEXT("重新加载后PrimaryAsset登记"), Id.IsValid() && UAssetManager::Get().GetPrimaryAssetIdForPath(FSoftObjectPath(Asset)) == Id);
			}

			TArray<FAssetData> Assets;
			for (auto* Item : Items)
			{
				Assets.Emplace(Item);
				Assets.Emplace(Item->AbilitySet.Get());
			}

			Assets.Emplace(Catalog);
			Assets.Emplace(Experience);
			Assets.Emplace(NoItems);
			Assets.Emplace(LoadObject<UGameFeatureData>(nullptr, TEXT("/MobaClassic/MobaClassic.MobaClassic")));
			for (const TCHAR* Name : {TEXT("ItemArmor"), TEXT("ItemDamageReduction"), TEXT("ShieldItemCooldown"), TEXT("HealItemCooldown")})
			{
				Assets.Emplace(LoadObject<UObject>(nullptr, *ObjectPath(EffectRoot + TEXT("GE_") + Name)));
			}

			for (const TCHAR* Name : {TEXT("ItemShield"), TEXT("ItemHeal")})
			{
				Assets.Emplace(LoadObject<UObject>(nullptr, *ObjectPath(AbilityRoot + TEXT("GA_") + Name)));
			}

			FValidateAssetsSettings Settings;
			Settings.ValidationUsecase = EDataValidationUsecase::Commandlet;
			FValidateAssetsResults Results;
			GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>()->ValidateAssetsWithSettings(Assets, Settings, Results);
			Test->TestEqual(TEXT("20项Feature资产原生Validator全部检查"), Results.NumChecked, 20);
			Test->TestEqual(TEXT("Feature原生Validator无Invalid"), Results.NumInvalid, 0);
			Test->TestEqual(TEXT("Feature原生Validator无Unable"), Results.NumUnableToValidate, 0);
			NegativeCases(Catalog, Items);
		}

		void NegativeCases(const UItemCatalogDefinition* Catalog, const TArray<UItemDefinition*>& Items)
		{
			auto* Item = DuplicateObject<UItemDefinition>(Items[0], GetTransientPackage());
			Item->Price = 0;
			Invalid(Item, TEXT("零价拒绝"));
			Item->Price = MAX_int32;
			Invalid(Item, TEXT("溢出风险价格拒绝"));
			Item->Price = 100;
			Item->MaxStack = 2;
			Invalid(Item, TEXT("装备非法堆叠拒绝"));
			Item->MaxStack = 1;
			Item->Recipe.Add(Item);
			Invalid(Item, TEXT("配方自身环拒绝"));
			auto* Other = DuplicateObject<UItemDefinition>(Items[1], GetTransientPackage());
			Item->Recipe = {Other};
			Other->Recipe = {Item};
			Invalid(Item, TEXT("配方间接环拒绝"));
			Other->Recipe.Reset();
			Invalid(Item, TEXT("组件总价高于成品拒绝"));
			Item->Recipe.Reset();
			Item->AbilitySet.Reset();
			Invalid(Item, TEXT("缺失AbilitySet拒绝"));
			auto* BadSet = DuplicateObject<UAbilitySetDefinition>(Items[2]->AbilitySet.Get(), GetTransientPackage());
			Item->AbilitySet = BadSet;
			BadSet->Abilities[0].Slot = MobaGameplayTags::Input_Slot_Primary;
			Invalid(Item, TEXT("物品不得占技能槽"));
			BadSet->Abilities[0].Slot = FGameplayTag();
			BadSet->Effects = Items[0]->AbilitySet->Effects;
			Invalid(Item, TEXT("本卡即时主动来源不得混入被动"));
			BadSet->Effects.Reset();
			BadSet->Abilities[0].Ability = USelfCommitAbility::StaticClass();
			Invalid(Item, TEXT("非固定效果能力拒绝"));
			auto* BadCatalog = DuplicateObject<UItemCatalogDefinition>(Catalog, GetTransientPackage());
			BadCatalog->Items.Add(Items[0]);
			Invalid(BadCatalog, TEXT("重复目录ID拒绝"));
			BadCatalog->Items = {Items[4], Items[0]};
			Invalid(BadCatalog, TEXT("目录外配方组件拒绝"));
			auto* OtherOrb = DuplicateObject<UItemDefinition>(Items[2], GetTransientPackage());
			OtherOrb->ItemId = TEXT("OtherOrb");
			BadCatalog->Items = {Items[2], OtherOrb};
			Invalid(BadCatalog, TEXT("不同定义不得共用冷却族"));
			BadCatalog->Items = {TSoftObjectPtr<UItemDefinition>(FSoftObjectPath(TEXT("/MobaClassic/Tests/Batch0811/Items/Missing.Missing")))};
			FString Reason;
			Test->TestFalse(TEXT("运行时不加载缺失引用"), BadCatalog->ValidateCatalog(Reason));
			Test->TestEqual(TEXT("缺失引用原因"), Reason, FString(TEXT("ItemNotLoaded")));
			auto* Potion = DuplicateObject<UItemDefinition>(Items[3], GetTransientPackage());
			Potion->MaxStack = 1;
			Invalid(Potion, TEXT("消耗品必须叠3"));
			auto* Passive = Items[0]->AbilitySet.Get()->Effects[0].Get()->GetDefaultObject<UGameplayEffect>();
			{
				TGuardValue<EGameplayEffectDurationType> Guard(Passive->DurationPolicy, EGameplayEffectDurationType::Instant);
				Invalid(const_cast<UItemDefinition*>(Items[0]), TEXT("来源Instant不可逆效果拒绝"));
			}

			{
				TGuardValue<FGameplayEffectModifierMagnitude> Guard(Passive->Modifiers[0].ModifierMagnitude, FGameplayEffectModifierMagnitude(FSetByCallerFloat()));
				Invalid(const_cast<UItemDefinition*>(Items[0]), TEXT("来源动态幅度拒绝"));
			}
		}

		FString ScanSnapshot(const TArray<FPrimaryAssetTypeInfo>& Types) const
		{
			FString Snapshot;
			int32 Index = 0;
			for (const auto& Type : Types)
			{
				if (Type.PrimaryAssetType == TEXT("Item"))
				{
					continue;
				}

				Snapshot += FString::Printf(TEXT("[%d] Type=%s Base=%s Blueprint=%d EditorOnly=%d Priority=%d ChunkId=%d Recursive=%d CookRule=%d\n"), Index++,
					*Type.PrimaryAssetType.ToString(), *Type.GetAssetBaseClass().ToSoftObjectPath().ToString(), Type.bHasBlueprintClasses, Type.bIsEditorOnly, Type.Rules.Priority,
					Type.Rules.ChunkId, Type.Rules.bApplyRecursively, static_cast<int32>(Type.Rules.CookRule));
				Snapshot += FString::Printf(TEXT("Directories=%d SpecificAssets=%d\n"), Type.GetDirectories().Num(), Type.GetSpecificAssets().Num());
				for (const auto& Directory : Type.GetDirectories())
				{
					Snapshot += TEXT("Directory=") + Directory.Path + TEXT("\n");
				}

				for (const auto& Asset : Type.GetSpecificAssets())
				{
					Snapshot += TEXT("SpecificAsset=") + Asset.ToString() + TEXT("\n");
				}
			}

			return Snapshot;
		}

		FString BaselinePath() const
		{
			const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Project, TEXT("AI/task-011/gfd-scan-before.txt")));
			FPaths::NormalizeFilename(Path);
			if (!FPaths::CollapseRelativeDirectories(Path) || !FPaths::IsUnderDirectory(Path, FPaths::Combine(Project, TEXT("AI/task-011"))))
			{
				return FString();
			}

			return Path;
		}


	private:
		bool CreateItemScan()
		{
			auto* Data = LoadObject<UGameFeatureData>(nullptr, TEXT("/MobaClassic/MobaClassic.MobaClassic"));
			if (!Test->TestNotNull(TEXT("批准的唯一既有GameFeatureData"), Data))
			{
				return false;
			}

			auto& Types = Data->GetPrimaryAssetTypesToScan();
			const int32 ItemCount = static_cast<int32>(Algo::CountIf(Types, [](const auto& Type) { return Type.PrimaryAssetType == TEXT("Item"); }));
			if (!Test->TestTrue(TEXT("Item扫描不存在或唯一"), ItemCount <= 1) || !Test->TestEqual(TEXT("追加前保留五个既有扫描条目"), Types.Num() - ItemCount, 5))
			{
				return false;
			}

			const auto* AbilitySet = Types.FindByPredicate([](const auto& Type) { return Type.PrimaryAssetType == TEXT("AbilitySet"); });
			if (!Test->TestNotNull(TEXT("Rules取现场AbilitySet条目"), AbilitySet))
			{
				return false;
			}

			const FPrimaryAssetRules Rules = AbilitySet->Rules;
			const FString Before = ScanSnapshot(Types);
			const FString Path = BaselinePath();
			if (!Test->TestFalse(TEXT("过程快照路径限本项目AI/task-011"), Path.IsEmpty()))
			{
				return false;
			}

			if (IFileManager::Get().FileExists(*Path))
			{
				if (!Test->TestTrue(TEXT("读取原五条首次逻辑快照"), FFileHelper::LoadFileToString(OriginalScanSnapshot, *Path))
					|| !Test->TestEqual(TEXT("重跑不得覆盖原扫描基线"), Before, OriginalScanSnapshot))
				{
					return false;
				}
			}
			else
			{
				OriginalScanSnapshot = Before;
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
				if (!Test->TestTrue(TEXT("记录旧五条完整逻辑字段"), FFileHelper::SaveStringToFile(Before, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
				{
					return false;
				}
			}

			Test->AddInfo(TEXT("制作前GameFeatureData扫描逻辑快照：\n") + Before);
			if (ItemCount == 0)
			{
				FPrimaryAssetTypeInfo Item(TEXT("Item"), UItemDefinition::StaticClass(), false, false);
				FDirectoryPath Directory;
				Directory.Path = TEXT("/MobaClassic/Tests/Batch0811/Items");
				Item.GetDirectories().Add(Directory);
				Item.Rules = Rules;
				Types.Add(MoveTemp(Item));
				if (!Test->TestEqual(TEXT("追加后旧五条完整逻辑字段不变"), ScanSnapshot(Types), Before) || !Save(Data))
				{
					return false;
				}
			}

			// 同进程检查保存后逻辑；真正持久化读回由后续Feature重载及独立只读进程证明。
			return VerifyItemScan();
		}

		bool VerifyItemScan()
		{
			const auto* Data = LoadObject<UGameFeatureData>(nullptr, TEXT("/MobaClassic/MobaClassic.MobaClassic"));
			if (!Test->TestNotNull(TEXT("GameFeatureData保存/重载可读取"), Data))
			{
				return false;
			}

			const auto& Types = Data->GetPrimaryAssetTypesToScan();
			if (!Test->TestEqual(TEXT("总扫描条目精确为六"), Types.Num(), 6)
				|| !Test->TestEqual(TEXT("Item扫描条目唯一"), static_cast<int32>(Algo::CountIf(Types, [](const auto& Type) { return Type.PrimaryAssetType == TEXT("Item"); })), 1))
			{
				return false;
			}

			const auto* Item = Types.FindByPredicate([](const auto& Type) { return Type.PrimaryAssetType == TEXT("Item"); });
			const auto* AbilitySet = Types.FindByPredicate([](const auto& Type) { return Type.PrimaryAssetType == TEXT("AbilitySet"); });
			if (!Test->TestNotNull(TEXT("AbilitySet扫描保持存在"), AbilitySet))
			{
				return false;
			}

			bool bValid = Test->TestEqual(TEXT("Item精确原生BaseClass"), Item->GetAssetBaseClass().ToSoftObjectPath().ToString(), FString(TEXT("/Script/MobaGame.ItemDefinition")));
			bValid &= Test->TestFalse(TEXT("Item非BlueprintClass扫描"), Item->bHasBlueprintClasses);
			bValid &= Test->TestFalse(TEXT("Item非EditorOnly"), Item->bIsEditorOnly);
			bValid &= Test->TestEqual(TEXT("Item扫描目录唯一"), Item->GetDirectories().Num(), 1);
			if (Item->GetDirectories().Num() == 1)
			{
				bValid &= Test->TestEqual(TEXT("Item精确扫描根"), Item->GetDirectories()[0].Path, FString(TEXT("/MobaClassic/Tests/Batch0811/Items")));
			}

			bValid &= Test->TestTrue(TEXT("Item SpecificAssets为空"), Item->GetSpecificAssets().IsEmpty());
			bValid &= Test->TestTrue(TEXT("Item完整Rules等于AbilitySet"), Item->Rules == AbilitySet->Rules);
			if (OriginalScanSnapshot.IsEmpty())
			{
				const FString Path = BaselinePath();
				if (!Path.IsEmpty() && IFileManager::Get().FileExists(*Path))
				{
					bValid &= Test->TestTrue(TEXT("独立进程读取制作前五条扫描快照"), FFileHelper::LoadFileToString(OriginalScanSnapshot, *Path));
				}
				else
				{
					// 干净贡献者checkout没有本机过程证据，仍验证本次两个生命周期内的配置稳定性。
					OriginalScanSnapshot = ScanSnapshot(Types);
					Test->AddInfo(TEXT("未发现本机制作前快照：本次只证明当前六条配置及两次重载稳定，不证明历史五条未改。"));
				}
			}

			bValid &= Test->TestEqual(TEXT("保存/重载后旧五条逐字段及顺序不变"), ScanSnapshot(Types), OriginalScanSnapshot);
			return bValid;
		}


	private:
		FAutomationTestBase* Test;

		FGuid Lease;

		FString URL;

		double Started = 0;

		bool bReleased = false;

		bool bCreated = false;

		int32 VerifiedCycles = 0;

		FString OriginalScanSnapshot;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FItemAssetTest, "Moba.Items.AssetValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FItemAssetTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(FItemAssets(this));
	return true;
}
#endif
