#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "Engine/Engine.h"
#include "Engine/AssetManager.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#include "Experience/ExperienceFeatureSubsystem.h"
#include "Experience/ExperienceSubsystem.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"
#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "AbilitySystem/SelfCommitAbility.h"
#include "Combat/TargetDamageAbility.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Hero/HeroCharacter.h"
#include "Match/ParticipantState.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "MobaGameplayTags.h"

namespace
{
	class FTargetAssetValidation final : public IAutomationLatentCommand
	{

	public:
		explicit FTargetAssetValidation(FAutomationTestBase* InTest) : Test(InTest) {}

		~FTargetAssetValidation() override { Release(); }


	public:
		bool Update() override
		{
			auto* Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
			if (!Lease.IsValid() && !bReleased)
			{
				Lease = FGuid::NewGuid();
				Started = FPlatformTime::Seconds();
				if (!Test->TestTrue(TEXT("Classic租约受理"), Features->Acquire(TEXT("MobaClassic"), Lease, URL)))
				{
					return true;
				}

				Test->TestEqual(TEXT("隔离资产验证只有本轮一个租约"), Features->GetLeaseCount(URL), 1);
				return false;
			}

			if (FPlatformTime::Seconds() - Started > 60)
			{
				Test->AddError(TEXT("Managed资产校验加载/清理超时"));
				return true;
			}

			if (bReleased)
			{
				const EGameFeaturePluginState NativeState = UGameFeaturesSubsystem::Get().GetPluginState(URL);
				const bool bReleasedState = GIsEditor
					? Features->GetState(URL) == EExperienceFeatureState::Loaded && (NativeState == EGameFeaturePluginState::Loaded || NativeState == EGameFeaturePluginState::Active)
					: Features->GetState(URL) == EExperienceFeatureState::Absent && UGameFeaturesSubsystem::Get().GetGameFeatureDataForRegisteredPluginByURL(URL) == nullptr;
				if (Features->GetLeaseCount(URL) != 0 || !bReleasedState)
				{
					return false;
				}

				Test->TestEqual(TEXT("资产验证本轮租约已全部归还"), Features->GetLeaseCount(URL), 0);

				if (CompletedCycles == 2)
				{
					return true;
				}

				bReleased = false;
				return false;
			}

			if (Features->GetState(URL) == EExperienceFeatureState::Failed)
			{
				Test->AddError(TEXT("Classic加载失败"));
				return true;
			}

			if (Features->GetState(URL) != EExperienceFeatureState::Loaded || UAssetManager::Get().GetAssetRegistry().IsGathering())
			{
				return false;
			}

			auto* Playlist = LoadObject<UPlaylistDefinition>(nullptr, TEXT("/Game/Playlists/Playlist_Batch0811.Playlist_Batch0811"));
			if (!Test->TestNotNull(TEXT("新Playlist从磁盘加载"), Playlist))
			{
				return true;
			}

			auto* Experience = Playlist->Experience.LoadSynchronous();
			auto* Hero = Experience ? Experience->Hero.LoadSynchronous() : nullptr;
			auto* Set = Hero ? Hero->AbilitySet.LoadSynchronous() : nullptr;
			if (!Test->TestNotNull(TEXT("真实配置链可加载"), Set))
			{
				return true;
			}

			TArray<FAssetData> Assets;
			TArray<UObject*> Objects;
			const TCHAR* Paths[] = {TEXT("/MobaClassic/Tests/Batch0811/Abilities/GA_UnitInstant.GA_UnitInstant"),
				TEXT("/MobaClassic/Tests/Batch0811/Abilities/GA_DirectionProjectile.GA_DirectionProjectile"), TEXT("/MobaClassic/Tests/Batch0811/Abilities/GA_PointArea.GA_PointArea"),
				TEXT("/MobaClassic/Definitions/Batch0811/AbilitySet_TargetVariant.AbilitySet_TargetVariant"),
				TEXT("/MobaClassic/Definitions/Batch0811/Hero_TargetVariant.Hero_TargetVariant"), TEXT("/MobaClassic/Tests/Batch0811/Input/IA_TargetAbility.IA_TargetAbility"),
				TEXT("/MobaClassic/Tests/Batch0811/Input/IMC_Targeting.IMC_Targeting"), TEXT("/MobaClassic/Input/Batch0811/InputProfile_WASD_Batch0811.InputProfile_WASD_Batch0811"),
				TEXT("/MobaClassic/Input/Batch0811/InputProfile_ClickToMove_Batch0811.InputProfile_ClickToMove_Batch0811")};
			for (const TCHAR* Path : Paths)
			{
				UObject* Object = LoadObject<UObject>(nullptr, Path);
				if (!Test->TestNotNull(TEXT("manifest真实磁盘资产"), Object))
				{
					return true;
				}

				Objects.Add(Object);
				Assets.Emplace(Object);
				if (Object->IsA<UPrimaryDataAsset>())
				{
					Test->TestTrue(TEXT("新Definition已登记PrimaryAsset"), UAssetManager::Get().GetPrimaryAssetIdForPath(FSoftObjectPath(Object)).IsValid());
				}
			}

			Assets.Emplace(Experience);
			Assets.Emplace(Set);
			Test->TestEqual(TEXT("附加Hero显式闭包"), Experience->HeroVariants.Num(), 1);
			UHeroDefinition* Variant = CastChecked<UHeroDefinition>(Objects[4]);
			Test->TestEqual(TEXT("闭包指向真实Variant"), Experience->HeroVariants[0].LoadSynchronous(), Variant);
			Test->TestEqual(TEXT("两种实际profile"), Experience->InputProfiles.Num(), 2);
			for (const auto& Profile : Experience->InputProfiles)
			{
				Test->TestTrue(TEXT("Profile新扫描根"), Profile.ToString().Contains(TEXT("/MobaClassic/Input/Batch0811/")));
				FDataValidationContext InputContext;
				Test->TestTrue(TEXT("Input全部映射已读回"), Profile.LoadSynchronous()->IsDataValid(InputContext) == EDataValidationResult::Valid);
				Test->TestFalse(TEXT("TargetMapping已保存"), Profile->TargetMapping.IsNull());
				Test->TestFalse(TEXT("TargetAbility已保存"), Profile->TargetAbility.IsNull());
			}

			const auto Values =
				UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(
					false);
			UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
			int32 Index = 0;
			for (const UHeroDefinition* Definition : {Hero, Variant})
			{
				Definition->AbilitySet.LoadSynchronous();
				for (const auto& Entry : Definition->AbilitySet->Abilities)
				{
					Entry.Ability.LoadSynchronous();
				}

				for (const auto& Effect : Definition->AbilitySet->Effects)
				{
					Effect.LoadSynchronous();
				}

				auto* Participant = World->SpawnActor<AParticipantState>();
				Participant->ParticipantId = FGuid::NewGuid();
				Participant->MatchId = FGuid::NewGuid();
				Participant->TeamId = 7;
				FActorSpawnParameters Spawn;
				Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				auto* Actor = World->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), FVector(Index * 300, 0, 100), FRotator::ZeroRotator, Spawn);
				Test->TestTrue(TEXT("两个磁盘Hero定义实际初始化同一C++Character"), Actor->InitializeHero(Definition, Participant));
				auto* ASC = Actor->GetHeroAbilitySystem();
				const auto* TargetSpec = ASC->FindAbilitySpecFromHandle(ASC->FindSlot(MobaGameplayTags::Input_Slot_Target));
				const auto* Ability = TargetSpec ? Cast<UTargetDamageAbility>(TargetSpec->Ability) : nullptr;
				if (Test->TestNotNull(TEXT("同archetype真实授予"), Ability))
				{
					Test->TestTrue(TEXT("不同组合的目标policy"), Ability->Targeting.Policy == (Index == 0 ? EAbilityTargetPolicy::Direction : EAbilityTargetPolicy::Point));
					Test->TestTrue(TEXT("不同组合的投送"), Ability->Delivery == (Index == 0 ? EAbilityDelivery::Projectile : EAbilityDelivery::Instant));
					FDataValidationContext AbilityContext;
					Test->TestTrue(TEXT("GA默认配置原生Validation"), Ability->IsDataValid(AbilityContext) == EDataValidationResult::Valid);
				}

				++Index;
			}

			World->EndPlay(EEndPlayReason::Quit);
			UExperienceSubsystem* WorldExperience = World->GetSubsystem<UExperienceSubsystem>();
			World->DestroyWorld(false);
			Test->TestFalse(TEXT("资产测试 World 不遗留 Experience 引用或句柄"), WorldExperience->HasOwnedResources());
			GEngine->DestroyWorldContext(World);
			Test->TestNull(TEXT("资产测试 WorldContext 已移除"), GEngine->GetWorldContextFromWorld(World));
			FDataValidationContext Context;
			Test->TestTrue(TEXT("宿主Playlist原生验证"), Playlist->IsDataValid(Context) == EDataValidationResult::Valid);
			FValidateAssetsSettings Settings;
			Settings.ValidationUsecase = EDataValidationUsecase::Commandlet;
			FValidateAssetsResults Results;
			GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>()->ValidateAssetsWithSettings(Assets, Settings, Results);
			Test->TestEqual(TEXT("十一项本卡资产全部Checked"), Results.NumChecked, 11);
			Test->TestEqual(TEXT("无Invalid"), Results.NumInvalid, 0);
			Test->TestEqual(TEXT("无Unable"), Results.NumUnableToValidate, 0);
			Test->TestEqual(TEXT("无Validation警告"), Results.NumWarnings, 0);
			++CompletedCycles;
			Release();
			return false;
		}


	private:
		void Release()
		{
			if (Lease.IsValid())
			{
				GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>()->Release(URL, Lease);
				Lease.Invalidate();
			}

			bReleased = true;
		}


	private:
		FAutomationTestBase* Test;

		FGuid Lease;

		FString URL;

		double Started = 0;

		bool bReleased = false;

		int32 CompletedCycles = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetAssetValidationTest, "Moba.Targeting.AssetValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTargetAssetValidationTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FTargetAssetValidation>(this));
	return true;
}
#endif
