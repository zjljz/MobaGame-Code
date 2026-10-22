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
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"
#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "AbilitySystem/SelfCommitAbility.h"

namespace
{
	class FManagedAssetValidation final : public IAutomationLatentCommand
	{

	public:
		explicit FManagedAssetValidation(FAutomationTestBase* InTest) : Test(InTest) {}

		~FManagedAssetValidation() override { Release(); }


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

			Test->TestEqual(TEXT("新Experience身份"), Experience->GetFName(), FName(TEXT("Experience_Batch0811")));
			Test->TestEqual(TEXT("新Hero身份"), Hero->GetFName(), FName(TEXT("Hero_Managed")));
			Test->TestEqual(TEXT("新AbilitySet身份"), Set->GetFName(), FName(TEXT("AbilitySet_Managed")));
			Test->TestTrue(TEXT("复制策略持久化"), Set->bAllowCopy);
			Test->TestEqual(TEXT("兼容Self/Unit/Target并扩展Heal/Shield槽"), Set->Abilities.Num(), 5);
			TArray<FAssetData> Assets = {FAssetData(Experience), FAssetData(Hero), FAssetData(Set)};
			for (const auto& Entry : Set->Abilities)
			{
				UClass* Ability = Entry.Ability.LoadSynchronous();
				Test->TestNotNull(TEXT("GA类可加载"), Ability);
				Test->TestEqual(TEXT("等级上限持久化"), Entry.MaxLevel, 3);
			}

			UObject* Blueprint = LoadObject<UObject>(nullptr, TEXT("/MobaClassic/Tests/Batch0811/Abilities/GA_ManagedSelf.GA_ManagedSelf"));
			if (Test->TestNotNull(TEXT("新GA Blueprint保存读回"), Blueprint))
			{
				Assets.Emplace(Blueprint);
			}

			if (Set->Abilities.Num() == 5)
			{
				Test->TestTrue(TEXT("新GA引用真实资产"), Set->Abilities[0].Ability.ToSoftObjectPath().ToString().Contains(TEXT("GA_ManagedSelf")));
				Test->TestTrue(TEXT("新GA复用SelfCommit父类"), Set->Abilities[0].Ability.Get()->IsChildOf(USelfCommitAbility::StaticClass()));
			}

			for (UObject* Definition : {static_cast<UObject*>(Experience), static_cast<UObject*>(Hero), static_cast<UObject*>(Set)})
			{
				Test->TestTrue(TEXT("PrimaryAsset扫描闭包存在"), UAssetManager::Get().GetPrimaryAssetIdForPath(FSoftObjectPath(Definition)).IsValid());
			}

			FDataValidationContext Context;
			Test->TestTrue(TEXT("宿主Playlist原生验证"), Playlist->IsDataValid(Context) == EDataValidationResult::Valid);
			FValidateAssetsSettings Settings;
			Settings.ValidationUsecase = EDataValidationUsecase::Commandlet;
			FValidateAssetsResults Results;
			GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>()->ValidateAssetsWithSettings(Assets, Settings, Results);
			Test->TestEqual(TEXT("四个Feature资产全部Checked"), Results.NumChecked, 4);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManagedAssetValidationTest, "Moba.Hero.ManagedAssetValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FManagedAssetValidationTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FManagedAssetValidation>(this));
	return true;
}
#endif
