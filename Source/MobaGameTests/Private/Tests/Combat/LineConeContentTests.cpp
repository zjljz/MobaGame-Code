#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Combat/CombatGameplayEffects.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/TargetDamageAbility.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/BoxComponent.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#include "Engine/AssetManager.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Experience/ExperienceFeatureSubsystem.h"
#include "Experience/ExperienceSubsystem.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"
#include "GameFramework/WorldSettings.h"
#include "Hero/HeroCharacter.h"
#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "MobaGameplayTags.h"
#include "Match/MatchGameState.h"
#include "Match/ParticipantState.h"
#include "Tests/Combat/TargetCombatant.h"
#include "UObject/UnrealType.h"

namespace
{
const TCHAR* FeatureAssetPaths[] = {
	TEXT("/MobaClassic/Tests/LineCone/Abilities/GA_LineInstant.GA_LineInstant"),
	TEXT("/MobaClassic/Tests/LineCone/Abilities/GA_ConeInstant.GA_ConeInstant"),
	TEXT("/MobaClassic/Definitions/LineCone/AbilitySet_LineCone.AbilitySet_LineCone"),
	TEXT("/MobaClassic/Definitions/LineCone/Hero_LineCone.Hero_LineCone"),
	TEXT("/MobaClassic/Definitions/LineCone/Experience_LineCone.Experience_LineCone")
};

const TCHAR* PlaylistPath = TEXT("/Game/Playlists/LineCone/Playlist_LineCone.Playlist_LineCone");

class FLineConeWorld final
{
public:
	explicit FLineConeWorld(FAutomationTestBase* InTest) : Test(InTest)
	{
		const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
			.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
		World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		World->BeginPlay();
		World->GetWorldSettings()->NotifyBeginPlay();
		Match = World->SpawnActor<AMatchGameState>();
		World->SetGameState(Match);
		Match->MatchId = FGuid::NewGuid();
		Match->Teams.Teams = {7, 21, 35};
		FTeamRelationEntry Enemy;
		Enemy.From = 7;
		Enemy.To = 21;
		Enemy.Relation = ETeamRelation::Enemy;
		Match->Teams.Relations.Add(Enemy);
	}

	~FLineConeWorld()
	{
		UExperienceSubsystem* WorldExperience = World->GetSubsystem<UExperienceSubsystem>();
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(false);
		Test->TestFalse(TEXT("LineCone fixture 不遗留 World 引用或句柄"), WorldExperience->HasOwnedResources());
		GEngine->DestroyWorldContext(World);
		Test->TestNull(TEXT("LineCone fixture WorldContext 已移除"), GEngine->GetWorldContextFromWorld(World));
	}


public:
	ATargetCombatant* SpawnTarget(const FVector& Location, int32 Team)
	{
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		auto* Actor = World->SpawnActor<ATargetCombatant>(ATargetCombatant::StaticClass(), Location, FRotator::ZeroRotator, Parameters);
		Actor->Entity = FGuid::NewGuid();
		Actor->Participant = FGuid::NewGuid();
		Actor->Match = Match->MatchId;
		Actor->Team = Team;
		return Actor;
	}

	AActor* SpawnWall(const FVector& Location)
	{
		auto* Actor = World->SpawnActor<AActor>();
		auto* Box = NewObject<UBoxComponent>(Actor);
		Actor->SetRootComponent(Box);
		Actor->AddInstanceComponent(Box);
		Box->SetBoxExtent(FVector(12, 150, 150));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		Actor->SetActorLocation(Location);
		return Actor;
	}


public:
	UWorld* World = nullptr;
	AMatchGameState* Match = nullptr;


private:
	FAutomationTestBase* Test;
};

class FLineConeContentCheck final : public IAutomationLatentCommand
{
public:
	explicit FLineConeContentCheck(FAutomationTestBase* InTest) : Test(InTest) {}

	~FLineConeContentCheck() override
	{
		Release();
	}

	bool Update() override
	{
		auto* Features = GEngine ? GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>() : nullptr;
		if (!Test->TestNotNull(TEXT("Feature subsystem 可用"), Features))
		{
			return true;
		}
		if (!bStarted)
		{
			bStarted = true;
			Started = FPlatformTime::Seconds();
			Lease = FGuid::NewGuid();
			if (!Test->TestTrue(TEXT("只获取本测试的 Classic 租约"), Features->Acquire(TEXT("MobaClassic"), Lease, URL)))
			{
				Release();
				return true;
			}
			Test->TestEqual(TEXT("隔离资产验证只有本轮一个租约"), Features->GetLeaseCount(URL), 1);
			return false;
		}
		if (FPlatformTime::Seconds() - Started > 60)
		{
			Test->AddError(TEXT("LineCone 内容加载或租约清理超时"));
			Release();
			return true;
		}
		if (bReleased)
		{
			const EGameFeaturePluginState NativeState = UGameFeaturesSubsystem::Get().GetPluginState(URL);
			const bool bReleasedState = GIsEditor
				? Features->GetState(URL) == EExperienceFeatureState::Loaded && (NativeState == EGameFeaturePluginState::Loaded || NativeState == EGameFeaturePluginState::Active)
				: Features->GetState(URL) == EExperienceFeatureState::Absent && UGameFeaturesSubsystem::Get().GetGameFeatureDataForRegisteredPluginByURL(URL) == nullptr;
			return Features->GetLeaseCount(URL) == 0 && bReleasedState;
		}
		if (Features->GetState(URL) == EExperienceFeatureState::Failed)
		{
			Test->AddError(TEXT("Classic Feature 加载失败"));
			Release();
			return true;
		}
		if (Features->GetState(URL) != EExperienceFeatureState::Loaded || UAssetManager::Get().GetAssetRegistry().IsGathering())
		{
			return false;
		}

		CheckSavedContent();
		Release();
		Started = FPlatformTime::Seconds();
		return false;
	}


private:
	void Release()
	{
		if (Lease.IsValid() && GEngine)
		{
			if (auto* Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>())
			{
				Features->Release(URL, Lease);
			}
			Lease.Invalidate();
		}
		bReleased = true;
	}

	/** 只读已保存资产，不创建、修补、强制登记或修改 CDO 来使验收通过。 */
	void CheckSavedContent()
	{
		TArray<UObject*> Objects;
		TArray<FAssetData> Assets;
		for (const TCHAR* Path : FeatureAssetPaths)
		{
			UObject* Object = LoadObject<UObject>(nullptr, Path);
			if (!Test->TestNotNull(FString(TEXT("磁盘内容存在: ")) + Path, Object))
			{
				return;
			}
			Objects.Add(Object);
			Assets.Emplace(Object);
		}
		auto* Playlist = LoadObject<UPlaylistDefinition>(nullptr, PlaylistPath);
		auto* Set = Cast<UAbilitySetDefinition>(Objects[2]);
		auto* Hero = Cast<UHeroDefinition>(Objects[3]);
		auto* Experience = Cast<UExperienceDefinition>(Objects[4]);
		if (!Test->TestNotNull(TEXT("Playlist 类型"), Playlist) || !Test->TestNotNull(TEXT("AbilitySet 类型"), Set)
			|| !Test->TestNotNull(TEXT("Hero 类型"), Hero) || !Test->TestNotNull(TEXT("Experience 类型"), Experience))
		{
			return;
		}
		for (UPrimaryDataAsset* Definition : TArray<UPrimaryDataAsset*>{Set, Hero, Experience, Playlist})
		{
			const FPrimaryAssetId Id = Definition->GetPrimaryAssetId();
			Test->TestEqual(TEXT("PrimaryAsset 原生登记身份"), UAssetManager::Get().GetPrimaryAssetIdForPath(FSoftObjectPath(Definition)), Id);
			Test->TestEqual(TEXT("PrimaryAsset 路径回读"), UAssetManager::Get().GetPrimaryAssetPath(Id), FSoftObjectPath(Definition));
		}
		Test->TestEqual(TEXT("Playlist Feature"), Playlist->FeatureName, FString(TEXT("MobaClassic")));
		Test->TestEqual(TEXT("Playlist 到新 Experience"), Playlist->Experience.LoadSynchronous(), Experience);
		Test->TestEqual(TEXT("Experience 到新 Hero"), Experience->Hero.LoadSynchronous(), Hero);
		Test->TestEqual(TEXT("Hero 到新 AbilitySet"), Hero->AbilitySet.LoadSynchronous(), Set);
		Test->TestEqual(TEXT("复用旧 MapDefinition"), Playlist->Map.ToString(), FString(TEXT("/MobaClassic/Definitions/Map_FrameworkTest.Map_FrameworkTest")));
		const UMapDefinition* Map = Playlist->Map.LoadSynchronous();
		FString PlayerReason;
		Test->TestTrue(TEXT("Experience 人数合同有效"), Experience->Players.IsValid(Experience->Teams.Teams.Num(), PlayerReason));
		if (Test->TestNotNull(TEXT("Playlist 唯一选定地图"), Map) && Test->TestNotNull(TEXT("地图实际资产可读回"), Map->World.LoadSynchronous()))
		{
			Test->TestTrue(TEXT("地图实际出生点满足比赛容量"), UMapDefinition::CountPlayerStarts(Map->World->PersistentLevel) >= Experience->Players.MaximumParticipants);
		}
		Test->TestFalse(TEXT("新 Set 不扩展 Copy"), Set->bAllowCopy);
		Test->TestEqual(TEXT("不增加被动授予"), Set->Effects.Num(), 0);
		if (!Test->TestEqual(TEXT("精确两个技能槽"), Set->Abilities.Num(), 2))
		{
			return;
		}

		TArray<const UTargetDamageAbility*> Abilities;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const auto* Blueprint = Cast<UBlueprint>(Objects[Index]);
			if (!Test->TestNotNull(TEXT("GA 为已编译 Blueprint"), Blueprint) || !Test->TestNotNull(TEXT("生成类已保存"), Blueprint->GeneratedClass.Get()))
			{
				return;
			}
			Test->TestEqual(TEXT("复用同一原生直接父类"), Blueprint->ParentClass.Get(), UTargetDamageAbility::StaticClass());
			const auto* Ability = Cast<UTargetDamageAbility>(Blueprint->GeneratedClass->GetDefaultObject());
			if (!Test->TestNotNull(TEXT("GA CDO 类型"), Ability))
			{
				return;
			}
			Abilities.Add(Ability);
			const auto Geometry = Ability->GetGeometry();
			Test->TestTrue(TEXT("两种策略"), Geometry.Policy == (Index == 0 ? EAbilityTargetPolicy::Line : EAbilityTargetPolicy::Cone));
			Test->TestTrue(TEXT("只允许 Instant"), Ability->Delivery == EAbilityDelivery::Instant);
			Test->TestFalse(TEXT("关闭旧 Unit 范围兼容"), Ability->bLegacyUnitRange);
			Test->TestEqual(TEXT("实际射程"), Geometry.Range, 900.0f);
			Test->TestEqual(TEXT("序列化射程不分歧"), Ability->Targeting.Range, Ability->Range);
			Test->TestEqual(TEXT("高度容差"), Geometry.HeightTolerance, 200.0f);
			Test->TestEqual(TEXT("半宽"), Geometry.HalfWidth, 75.0f);
			Test->TestEqual(TEXT("半角"), Geometry.HalfAngleDegrees, 30.0f);
			Test->TestEqual(TEXT("目标上限"), Geometry.MaxTargets, 16);
			Test->TestTrue(TEXT("仅敌方"), Geometry.AllowedRelations == TArray<ETeamRelation>{ETeamRelation::Enemy});
			Test->TestEqual(TEXT("既有伤害类型"), Ability->DamageType, FGameplayTag(MobaGameplayTags::Damage_True));
			Test->TestEqual(TEXT("既有伤害 GE"), GetPathNameSafe(Ability->DamageEffect.Get()), FString(TEXT("/MobaClassic/Abilities/GE_TestDamage.GE_TestDamage_C")));
			Test->TestEqual(TEXT("既有成本 GE"), GetPathNameSafe(Ability->GetCostGameplayEffect()), FString(TEXT("/MobaClassic/Abilities/GE_ManaCost.Default__GE_ManaCost_C")));
			Test->TestEqual(TEXT("既有冷却 GE"), GetPathNameSafe(Ability->GetCooldownGameplayEffect()), FString(TEXT("/MobaClassic/Abilities/GE_UnitAttackCooldown.Default__GE_UnitAttackCooldown_C")));
			Test->TestEqual(TEXT("稳定槽配置"), Set->Abilities[Index].Slot,
				FGameplayTag(Index == 0 ? MobaGameplayTags::Input_Slot_Primary : MobaGameplayTags::Input_Slot_Target));
			Test->TestEqual(TEXT("授予真实生成类"), Set->Abilities[Index].Ability.LoadSynchronous(), Blueprint->GeneratedClass.Get());
			Test->TestEqual(TEXT("初始等级"), Set->Abilities[Index].Level, 1);
			Test->TestEqual(TEXT("等级上限"), Set->Abilities[Index].MaxLevel, 1);
			FDataValidationContext AbilityContext;
			Test->TestTrue(TEXT("GA CDO 原生验证"), Ability->IsDataValid(AbilityContext) == EDataValidationResult::Valid);
			CheckWorldGeometry(Geometry);
		}
		FString GrantReason;
		Test->TestTrue(TEXT("实际 GA 已加载且满足授予合同"), Set->ValidateGrant(GrantReason));
		CheckPreservedConfiguration(Hero, Experience);
		CheckHeroGrant(Hero, Abilities);

		FDataValidationContext PlaylistContext;
		Test->TestTrue(TEXT("宿主 Playlist 自身原生验证"), Playlist->IsDataValid(PlaylistContext) == EDataValidationResult::Valid);
		auto* Validator = GEditor ? GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>() : nullptr;
		if (!Test->TestNotNull(TEXT("原生 Validator"), Validator))
		{
			return;
		}
		FValidateAssetsSettings Settings;
		Settings.ValidationUsecase = EDataValidationUsecase::Commandlet;
		Settings.bCollectPerAssetDetails = true;
		FValidateAssetsResults Results;
		Validator->ValidateAssetsWithSettings(Assets, Settings, Results);
		for (const TCHAR* Path : FeatureAssetPaths)
		{
			const auto* Detail = Results.AssetsDetails.Find(FString(Path));
			if (Test->TestNotNull(FString(TEXT("必需资产有独立验证结果: ")) + Path, Detail))
			{
				Test->TestTrue(FString(TEXT("必需资产 Valid: ")) + Path, Detail->Result == EDataValidationResult::Valid);
			}
		}
		Test->TestEqual(TEXT("无 Invalid"), Results.NumInvalid, 0);
		Test->TestEqual(TEXT("无 UnableToValidate"), Results.NumUnableToValidate, 0);
		Test->AddInfo(FString::Printf(TEXT("LineCone Validation Requested=%d Checked=%d Warnings=%d"), Results.NumRequested, Results.NumChecked, Results.NumWarnings));
	}

	void CheckPreservedConfiguration(const UHeroDefinition* Hero, const UExperienceDefinition* Experience)
	{
		const auto* OriginalHero = LoadObject<UHeroDefinition>(nullptr, TEXT("/MobaClassic/Definitions/Hero_FrameworkTest.Hero_FrameworkTest"));
		const auto* OriginalExperience = LoadObject<UExperienceDefinition>(nullptr, TEXT("/MobaClassic/Definitions/Experience_FrameworkTest.Experience_FrameworkTest"));
		if (!Test->TestNotNull(TEXT("形体复用来源存在"), OriginalHero) || !Test->TestNotNull(TEXT("规则复用来源存在"), OriginalExperience))
		{
			return;
		}
		for (TFieldIterator<FProperty> It(UHeroDefinition::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (It->GetFName() != GET_MEMBER_NAME_CHECKED(UHeroDefinition, AbilitySet))
			{
				Test->TestTrue(FString(TEXT("Hero 保留源字段: ")) + It->GetName(), It->Identical_InContainer(Hero, OriginalHero));
			}
		}
		for (TFieldIterator<FProperty> It(UExperienceDefinition::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (It->GetFName() != GET_MEMBER_NAME_CHECKED(UExperienceDefinition, Hero)
				&& It->GetFName() != GET_MEMBER_NAME_CHECKED(UExperienceDefinition, InputProfiles))
			{
				Test->TestTrue(FString(TEXT("Experience 保留源字段: ")) + It->GetName(), It->Identical_InContainer(Experience, OriginalExperience));
			}
		}
		Test->TestEqual(TEXT("当前总人数容量"), Experience->Players.MaximumParticipants, 2);
		Test->TestEqual(TEXT("当前最低开局配置"), Experience->Players.MinimumPlayersToStart, 1);
		Test->TestEqual(TEXT("当前每队容量"), Experience->Players.MaximumPlayersPerTeam, 2);
		Test->TestEqual(TEXT("仅复用两种输入 Profile"), Experience->InputProfiles.Num(), 2);
		Test->TestEqual(TEXT("允许集合与实际 Profile 数量对应"), Experience->AllowedInputProfiles.Num(), 2);
		const TCHAR* ProfilePaths[] = {
			TEXT("/MobaClassic/Input/Batch0811/InputProfile_WASD_Batch0811.InputProfile_WASD_Batch0811"),
			TEXT("/MobaClassic/Input/Batch0811/InputProfile_ClickToMove_Batch0811.InputProfile_ClickToMove_Batch0811")
		};
		for (const TCHAR* Path : ProfilePaths)
		{
			Test->TestTrue(TEXT("复用已有输入引用"), Experience->InputProfiles.ContainsByPredicate([Path](const auto& Profile) { return Profile.ToString() == Path; }));
		}

		TSet<EHeroInputProfile> Profiles;
		for (const auto& ProfileReference : Experience->InputProfiles)
		{
			const UInputProfileDefinition* Profile = ProfileReference.LoadSynchronous();
			if (!Test->TestNotNull(TEXT("实际输入 Profile 可加载"), Profile))
			{
				continue;
			}

			Test->TestFalse(TEXT("实际输入种类不重复"), Profiles.Contains(Profile->Profile));
			Profiles.Add(Profile->Profile);
			Test->TestTrue(TEXT("实际输入在 Experience 允许集合"), Experience->AllowedInputProfiles.Contains(Profile->Profile));
			FDataValidationContext ProfileContext;
			Test->TestTrue(TEXT("实际 Profile 映射与动作有效"), Profile->IsDataValid(ProfileContext) == EDataValidationResult::Valid);
		}

		Test->TestTrue(TEXT("两种实际输入均保留"), Profiles.Contains(EHeroInputProfile::WASD) && Profiles.Contains(EHeroInputProfile::ClickToMove));
	}

	void CheckHeroGrant(const UHeroDefinition* Definition, const TArray<const UTargetDamageAbility*>& Abilities)
	{
		FLineConeWorld Fixture(Test);
		auto* Participant = Fixture.World->SpawnActor<AParticipantState>();
		Participant->ParticipantId = FGuid::NewGuid();
		Participant->MatchId = Fixture.Match->MatchId;
		Participant->TeamId = 7;
		auto* Hero = Fixture.World->SpawnActor<AHeroCharacter>();
		if (!Test->TestNotNull(TEXT("同一 AHeroCharacter"), Hero) || !Test->TestTrue(TEXT("新定义实际初始化"), Hero->InitializeHero(Definition, Participant)))
		{
			return;
		}
		auto* AbilitySystem = Hero->GetHeroAbilitySystem();
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const FGameplayTag Slot = Index == 0 ? MobaGameplayTags::Input_Slot_Primary : MobaGameplayTags::Input_Slot_Target;
			const auto* Spec = AbilitySystem->FindAbilitySpecFromHandle(AbilitySystem->FindSlot(Slot));
			if (Test->TestNotNull(TEXT("稳定槽实际授予"), Spec))
			{
				Test->TestEqual(TEXT("授予来自保存 GA"), Spec->Ability->GetClass(), Abilities[Index]->GetClass());
			}
		}
	}

	void CheckWorldGeometry(const FAbilityTargetGeometry& Geometry)
	{
		FLineConeWorld Fixture(Test);
		const FVector Origin(0, 0, 100);
		auto* Source = Fixture.SpawnTarget(Origin, 7);
		auto* Target = Fixture.SpawnTarget(Origin + FVector(300, 0, 0), 21);
		FAbilityTargetIntent Intent;
		Intent.Policy = Geometry.Policy;
		Intent.Direction = FVector::ForwardVector;
		TArray<AActor*> Targets;
		FString Reason;
		const auto Check = [&](const TCHAR* Label, bool bExpected)
		{
			const FString Prefix = FString::Printf(TEXT("%s %s: "), Geometry.Policy == EAbilityTargetPolicy::Line ? TEXT("Line") : TEXT("Cone"), Label);
			Test->TestTrue(Prefix + TEXT("合法区域意图可提交"), MobaTargeting::Resolve(Source, Geometry, Intent, Targets, Reason));
			Test->TestEqual(Prefix + TEXT("实际目标数量"), Targets.Num(), bExpected ? 1 : 0);
			Test->TestEqual(Prefix + TEXT("目标身份"), Targets.Contains(Target), bExpected);
		};
		Check(TEXT("界内敌人"), true);
		Target->SetActorLocation(Origin + FVector(900, 0, 200));
		Check(TEXT("射程与高度边界"), true);
		Target->SetActorLocation(Origin + FVector(901, 0, 0));
		Check(TEXT("超射程"), false);
		Target->SetActorLocation(Origin + FVector(300, 0, 201));
		Check(TEXT("超高度"), false);
		Target->SetActorLocation(Origin + FVector(-1, 0, 0));
		Check(TEXT("背后"), false);
		if (Geometry.Policy == EAbilityTargetPolicy::Line)
		{
			Target->SetActorLocation(Origin + FVector(900, 75, 0));
			Check(TEXT("矩形端点角"), true);
			Target->SetActorLocation(Origin + FVector(300, 76, 0));
			Check(TEXT("半宽外"), false);
		}
		else
		{
			const double Angle = FMath::DegreesToRadians(30.0);
			Target->SetActorLocation(Origin + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * 300);
			Check(TEXT("半角边界"), true);
			const double OutsideAngle = FMath::DegreesToRadians(31.0);
			Target->SetActorLocation(Origin + FVector(FMath::Cos(OutsideAngle), FMath::Sin(OutsideAngle), 0) * 300);
			Check(TEXT("半角外"), false);
		}
		Target->SetActorLocation(Origin + FVector(300, 0, 0));
		const int32 RejectedTeams[] = {7, INDEX_NONE, 35, 999};
		for (int32 Team : RejectedTeams)
		{
			Target->Team = Team;
			Check(TEXT("友方中立未定义未知队伍"), false);
		}
		Target->Team = 21;
		auto* Wall = Fixture.SpawnWall(Origin + FVector(150, 0, 0));
		Test->TestTrue(TEXT("真实 Visibility 阻挡"), Fixture.World->LineTraceTestByChannel(Origin, Target->GetActorLocation(), ECC_Visibility));
		Check(TEXT("墙后敌人"), false);
		Wall->Destroy();
		Check(TEXT("移除瞬态墙恢复"), true);
	}


private:
	FAutomationTestBase* Test = nullptr;
	FGuid Lease;
	FString URL;
	double Started = 0;
	bool bStarted = false;
	bool bReleased = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLineConeContentTest, "Moba.Content.LineConeInstant", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLineConeContentTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FLineConeContentCheck>(this));
	return true;
}

#endif
