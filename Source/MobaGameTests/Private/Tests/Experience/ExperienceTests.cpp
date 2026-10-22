#include "Experience/ExperienceSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Experience/ExperienceDefinitions.h"
#include "Experience/Compatibility/ExperienceContextComponent.h"
#include "Experience/ExperienceFeatureSubsystem.h"
#include "Experience/MobaAssetManager.h"
#include "Tests/Experience/GameConfigurationTestScope.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "MobaMessageTags.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMatchPlayerConfigurationTest, "Moba.Experience.MatchPlayerConfiguration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMatchPlayerConfigurationTest::RunTest(const FString& Parameters)
{
	FMatchPlayerConfiguration Players;
	FString Reason;
	TestTrue(TEXT("现有两人测试人数有效"), Players.IsValid(3, Reason));
	Players.MinimumPlayersToStart = 3;
	TestFalse(TEXT("最低开局人数不能超过容量"), Players.IsValid(3, Reason));
	Players.MinimumPlayersToStart = 1;
	Players.MaximumPlayersPerTeam = 1;
	TestFalse(TEXT("队伍容量不足不能容纳两名参赛者"), Players.IsValid(1, Reason));
	TestTrue(TEXT("两队各一人可容纳两人"), Players.IsValid(2, Reason));
	Players.MaximumPlayersPerTeam = 0;
	TestFalse(TEXT("零队伍容量拒绝"), Players.IsValid(2, Reason));
	Players.MaximumPlayersPerTeam = 2;
	Players.MaximumParticipants = 0;
	TestFalse(TEXT("零比赛容量拒绝"), Players.IsValid(2, Reason));
	TestNull(TEXT("Experience不再选择地图"), FindFProperty<FProperty>(UExperienceDefinition::StaticClass(), TEXT("Map")));
	TestNull(TEXT("Experience不再配置上下文动作"), FindFProperty<FProperty>(UExperienceDefinition::StaticClass(), TEXT("ContextAction")));
	TestNotNull(TEXT("Playlist唯一选择地图"), FindFProperty<FProperty>(UPlaylistDefinition::StaticClass(), TEXT("Map")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExperienceValidationTest, "Moba.Experience.DefinitionValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FExperienceValidationTest::RunTest(const FString& Parameters)
{
	FDataValidationContext PlaylistContext;
	TestTrue(TEXT("缺失 Feature/软引用被校验拒绝"), NewObject<UPlaylistDefinition>()->IsDataValid(PlaylistContext) == EDataValidationResult::Invalid);
	TestTrue(TEXT("Playlist 提供可读错误"), PlaylistContext.GetNumErrors() >= 2);
	FDataValidationContext ExperienceContext;
	TestTrue(TEXT("缺失 Hero/比赛规则被校验拒绝"), NewObject<UExperienceDefinition>()->IsDataValid(ExperienceContext) == EDataValidationResult::Invalid);
	FDataValidationContext MapContext;
	TestTrue(TEXT("缺失 World 被校验拒绝"), NewObject<UMapDefinition>()->IsDataValid(MapContext) == EDataValidationResult::Invalid);
	return true;
}

namespace
{
	bool HasReleasedFeature(const UExperienceFeatureSubsystem* Features, const FString& URL)
	{
		if (Features->GetLeaseCount(URL) != 0)
		{
			return false;
		}

		if (!GIsEditor)
		{
			return Features->GetState(URL) == EExperienceFeatureState::Absent
				&& UGameFeaturesSubsystem::Get().GetGameFeatureDataForRegisteredPluginByURL(URL) == nullptr;
		}

		const EGameFeaturePluginState NativeState = UGameFeaturesSubsystem::Get().GetPluginState(URL);
		return Features->GetState(URL) == EExperienceFeatureState::Loaded
			&& (NativeState == EGameFeaturePluginState::Loaded || NativeState == EGameFeaturePluginState::Active);
	}

	bool HasValidAssetResult(const FValidateAssetsResults& Results, const FString& ObjectPath)
	{
		const FValidateAssetsDetails* Details = Results.AssetsDetails.Find(ObjectPath);
		return Details && Details->Result == EDataValidationResult::Valid;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetValidationCoverageTest, "Moba.Experience.ValidationCoverage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FAssetValidationCoverageTest::RunTest(const FString& Parameters)
	{
		// 验证结果快照可确定性重现漏报，不需要保存坏资产或修改全局验证器。
		const FString RequiredPath = TEXT("/Temp/Task007/Required.Required");
		const FString AdditionalPath = TEXT("/Temp/Task007/Additional.Additional");
		FValidateAssetsResults Results;
		Results.NumRequested = 2;
		Results.NumChecked = 2;
		Results.NumValid = 1;
		Results.NumUnableToValidate = 1;
		Results.AssetsDetails.Add(RequiredPath).Result = EDataValidationResult::NotValidated;
		Results.AssetsDetails.Add(AdditionalPath).Result = EDataValidationResult::Valid;

		TestTrue(TEXT("旧汇总断言会接受必需资产未验证的结果"), Results.NumRequested >= 1 && Results.NumInvalid == 0);
		TestFalse(TEXT("必需资产 NotValidated 被拒绝"), HasValidAssetResult(Results, RequiredPath));
		TestTrue(TEXT("追加资产的 Valid 结果只证明它自身"), HasValidAssetResult(Results, AdditionalPath));

		Results.AssetsDetails.FindChecked(RequiredPath).Result = EDataValidationResult::Valid;
		Results.NumValid = 2;
		Results.NumUnableToValidate = 0;
		TestTrue(TEXT("必需资产实际 Valid 才被接受"), HasValidAssetResult(Results, RequiredPath));

		Results.AssetsDetails.FindChecked(RequiredPath).Result = EDataValidationResult::Invalid;
		Results.NumValid = 1;
		Results.NumInvalid = 1;
		TestFalse(TEXT("必需资产 Invalid 被拒绝"), HasValidAssetResult(Results, RequiredPath));

		Results.AssetsDetails.Remove(RequiredPath);
		Results.NumChecked = 1;
		Results.NumSkipped = 1;
		Results.NumInvalid = 0;
		TestTrue(TEXT("旧汇总断言会接受必需资产被跳过的结果"), Results.NumRequested >= 1 && Results.NumInvalid == 0);
		TestFalse(TEXT("缺少必需资产结果时追加资产不能补足覆盖"), HasValidAssetResult(Results, RequiredPath));
		return true;
	}

	class FMigratedAssetValidation final : public IAutomationLatentCommand
	{

	public:
		explicit FMigratedAssetValidation(FAutomationTestBase* InTest) : Test(InTest) {}

		virtual ~FMigratedAssetValidation() override { ReleaseLease(); }

		virtual bool Update() override
		{
			UExperienceFeatureSubsystem* Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
			if (!bStarted)
			{
				Lease = FGuid::NewGuid();
				if (!Test->TestTrue(TEXT("Classic Feature 租约受理"), Features->Acquire(TEXT("MobaClassic"), Lease, URL)))
				{
					return true;
				}

				bStarted = true;
				StartTime = FPlatformTime::Seconds();
				return false;
			}

			if (FPlatformTime::Seconds() - StartTime > 60.0)
			{
				Test->AddError(FString::Printf(TEXT("资产校验超时 FeatureState=%d Released=%d"), int32(Features->GetState(URL)), bReleased));
				return true;
			}

			if (bReleased)
			{
				return HasReleasedFeature(Features, URL);
			}

			const EExperienceFeatureState State = Features->GetState(URL);
			if (State == EExperienceFeatureState::Failed)
			{
				Test->AddError(TEXT("Classic Feature 加载失败"));
				return true;
			}

			if (State != EExperienceFeatureState::Loaded || UAssetManager::Get().GetAssetRegistry().IsGathering())
			{
				return false;
			}

			static const TCHAR* Packages[] = {TEXT("/Game/Playlists/Playlist_FrameworkTest"), TEXT("/MobaClassic/MobaClassic"), TEXT("/MobaClassic/Definitions/Experience_FrameworkTest"),
				TEXT("/MobaClassic/Definitions/Map_FrameworkTest"), TEXT("/MobaClassic/Definitions/Hero_FrameworkTest"), TEXT("/MobaClassic/Definitions/AbilitySet_FrameworkTest"),
				TEXT("/MobaClassic/Abilities/GA_SelfCommit"), TEXT("/MobaClassic/Abilities/GA_TestUnitAttack"), TEXT("/MobaClassic/Abilities/GE_ManaCost"),
				TEXT("/MobaClassic/Abilities/GE_SelfCooldown"), TEXT("/MobaClassic/Abilities/GE_TestDamage"), TEXT("/MobaClassic/Abilities/GE_TestDeathCleanup"),
				TEXT("/MobaClassic/Abilities/GE_TestPersistent"), TEXT("/MobaClassic/Abilities/GE_UnitAttackCooldown"), TEXT("/MobaClassic/Input/InputProfile_ClickToMove"),
				TEXT("/MobaClassic/Input/InputProfile_WASD")};
			TArray<FAssetData> Assets;
			for (const TCHAR* Package : Packages)
			{
				const FString ObjectPath = FString::Printf(TEXT("%s.%s"), Package, *FPackageName::GetShortName(Package));
				UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath);
				if (Test->TestNotNull(FString::Printf(TEXT("迁移后资产可加载 %s"), Package), Asset))
				{
					Assets.Emplace(Asset);
				}
			}

			Test->TestEqual(TEXT("16 项旧脚本引用资产全部可加载"), Assets.Num(), int32(UE_ARRAY_COUNT(Packages)));
			if (Assets.Num() == int32(UE_ARRAY_COUNT(Packages)))
			{
				UPlaylistDefinition* Playlist = Cast<UPlaylistDefinition>(Assets[0].GetAsset());
				FDataValidationContext PlaylistContext;
				Test->TestTrue(TEXT("挂载后 Playlist 自身原生校验通过"), Playlist && Playlist->IsDataValid(PlaylistContext) == EDataValidationResult::Valid);

				// 通用跨插件引用检查不识别宿主先持有 Feature 身份、挂载后解析软引用的合同。
				// Playlist 用自身 IsDataValid 校验该合同，其余 Feature 资产交原生 ValidatorSubsystem。
				TArray<FAssetData> FeatureAssets = Assets;
				FeatureAssets.RemoveAt(0);
				UEditorValidatorSubsystem* Validator = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
				if (Test->TestNotNull(TEXT("原生 Editor Validator Subsystem"), Validator))
				{
					FValidateAssetsSettings Settings;
					Settings.ValidationUsecase = EDataValidationUsecase::Commandlet;
					Settings.bCollectPerAssetDetails = true;
					FValidateAssetsResults Results;
					Validator->ValidateAssetsWithSettings(FeatureAssets, Settings, Results);
					for (const FAssetData& Asset : FeatureAssets)
					{
						const FString ObjectPath = Asset.GetObjectPathString();
						const FValidateAssetsDetails* Details = Results.AssetsDetails.Find(ObjectPath);
						const FString ActualResult = Details ? UEnum::GetValueAsString(Details->Result) : TEXT("Missing");
						Test->TestTrue(FString::Printf(TEXT("必需 Classic 资产实际验证为 Valid Path=%s Result=%s"), *ObjectPath, *ActualResult), HasValidAssetResult(Results, ObjectPath));
					}

					Test->TestEqual(TEXT("Classic 资产无无效项"), Results.NumInvalid, 0);
					Test->AddInfo(FString::Printf(TEXT("原生 Classic 资产校验 Requested=%d Checked=%d Invalid=%d Warnings=%d Unable=%d；Playlist 由自身 IsDataValid 单列"),
						Results.NumRequested, Results.NumChecked, Results.NumInvalid, Results.NumWarnings, Results.NumUnableToValidate));
				}
			}

			ReleaseLease();
			bReleased = true;
			return false;
		}


	private:
		void ReleaseLease()
		{
			if (!Lease.IsValid())
			{
				return;
			}

			GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>()->Release(URL, Lease);
			Lease.Invalidate();
		}


	private:
		FAutomationTestBase* Test;

		FString URL;

		FGuid Lease;

		double StartTime = 0;

		bool bStarted = false;

		bool bReleased = false;
	};

	class FFeatureImmediateCancellation final : public IAutomationLatentCommand
	{

	public:
		explicit FFeatureImmediateCancellation(FAutomationTestBase* InTest) : Test(InTest) {}

		virtual bool Update() override
		{
			UExperienceFeatureSubsystem* Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
			if (!bStarted)
			{
				const FGuid Lease = FGuid::NewGuid();
				Test->TestTrue(TEXT("唯一Feature租约受理"), Features->Acquire(TEXT("MobaClassic"), Lease, URL));
				Features->Release(URL, Lease);
				bStarted = true;
				StartTime = FPlatformTime::Seconds();
				return false;
			}

			if (HasReleasedFeature(Features, URL))
			{
				Test->TestEqual(TEXT("立即取消不留租约"), Features->GetLeaseCount(URL), 0);
				if (GIsEditor)
				{
					Test->TestTrue(TEXT("Editor 原生 FeatureData 句柄保持驻留"), UAssetManager::Get().GetPrimaryAssetHandle(FPrimaryAssetId(TEXT("GameFeatureData"), TEXT("MobaClassic"))).IsValid());
				}
				else
				{
					Test->TestFalse(TEXT("原生FeatureData无延迟复活句柄"), UAssetManager::Get().GetPrimaryAssetHandle(FPrimaryAssetId(TEXT("GameFeatureData"), TEXT("MobaClassic"))).IsValid());
				}
				return true;
			}

			if (FPlatformTime::Seconds() - StartTime > 30.0)
			{
				Test->AddError(TEXT("唯一使用者立即取消后Feature未释放"));
				return true;
			}

			return false;
		}


	private:
		FAutomationTestBase* Test;

		FString URL;

		bool bStarted = false;

		double StartTime = 0;
	};

	class FExperienceWorldScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FExperienceWorldScenario(FAutomationTestBase* InTest) : Test(InTest) {}

		virtual ~FExperienceWorldScenario() override { Cleanup(); }

		virtual bool Update() override
		{
			if (!WorldA)
			{
				Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
				UGameFeaturesSubsystem::Get().GetPluginURLByName(TEXT("MobaClassic"), URL);
				if (StartTime == 0)
				{
					StartTime = FPlatformTime::Seconds();
				}

				if (Features->GetLeaseCount(URL) != 0 || (Features->GetState(URL) != EExperienceFeatureState::Absent && !HasReleasedFeature(Features, URL)))
				{
					if (FPlatformTime::Seconds() - StartTime > 20.0)
					{
						Test->AddError(TEXT("前序Feature卸载未完成"));
						return true;
					}

					return false;
				}

				Assets = CastChecked<UMobaAssetManager>(&UAssetManager::Get());
				ConfigurationOverride = MakeUnique<FScopedGameConfigurationOverride>(*Assets);
				if (!Test->TestNotNull(TEXT("已加载配置根可供瞬态场景复制"), ConfigurationOverride->Get()))
				{
					return true;
				}

				WorldA = CreateWorld(TEXT("Task002A"));
				WorldB = CreateWorld(TEXT("Task002B"));
				A = WorldA->GetSubsystem<UExperienceSubsystem>();
				B = WorldB->GetSubsystem<UExperienceSubsystem>();
				if (!Test->TestNotNull(TEXT("World A subsystem"), A) || !Test->TestNotNull(TEXT("World B subsystem"), B))
				{
					return true;
				}

				GMP::FMessageUtils::ListenObjectMessage(A, MOBA_MESSAGE_EXPERIENCE_CHANGED, WorldA,
					[this](const FExperienceSnapshot& State)
					{
						if (State.State == EExperienceState::ExperienceReady)
						{
							++ReadyA;
						}
					});
				GMP::FMessageUtils::ListenObjectMessage(B, MOBA_MESSAGE_EXPERIENCE_CHANGED, WorldB,
					[this](const FExperienceSnapshot& State)
					{
						if (State.State == EExperienceState::ExperienceReady)
						{
							++ReadyB;
						}
					});
				StartTime = FPlatformTime::Seconds();
				Test->TestTrue(TEXT("允许 Playlist 受理"), A->StartExperience(PlaylistId) == EExperienceFailure::None);
				const int64 CancelledGeneration = A->GetSnapshot().Generation;
				A->StopExperience();
				Test->TestTrue(TEXT("取消推进 generation"), A->GetSnapshot().Generation > CancelledGeneration);
				Test->TestFalse(TEXT("加载前取消归还所有本卡资源"), A->HasOwnedResources());
				A->StartExperience(PlaylistId);
				return false;
			}

			// fixture 实际推进两个独立 World 的 streaming；不拿任意延时替代 Ready 谓词。
			WorldA->Tick(LEVELTICK_All, 0.016f);
			WorldB->Tick(LEVELTICK_All, 0.016f);
			if (FPlatformTime::Seconds() - StartTime > 90.0)
			{
				Test->AddError(FString::Printf(TEXT("场景超时 Stage=%d A=%s/%s B=%s/%s Feature=%d"), Stage, *UEnum::GetValueAsString(A->GetSnapshot().State),
					*UEnum::GetValueAsString(A->GetSnapshot().Failure), *UEnum::GetValueAsString(B->GetSnapshot().State), *UEnum::GetValueAsString(B->GetSnapshot().Failure),
					static_cast<int32>(Features->GetState(URL))));
				return true;
			}

			if (Stage <= 2 && (A->GetSnapshot().State == EExperienceState::Failed || B->GetSnapshot().State == EExperienceState::Failed))
			{
				Test->AddError(FString::Printf(
					TEXT("真实资源链失败 Stage=%d A=%s B=%s"), Stage, *UEnum::GetValueAsString(A->GetSnapshot().Failure), *UEnum::GetValueAsString(B->GetSnapshot().Failure)));
				return true;
			}

			switch (Stage)
			{
			case -2:
				if (!Ready(A))
				{
					return false;
				}

				// 本场景兼容驻留复用；真实 Feature Loading 取消由独立 fresh-process 用例证明。
				A->StopExperience();
				Stage = -1;
				return false;
			case -1:
				if (!Unloaded(A))
				{
					return false;
				}

				VerifyClean(A, WorldA);
				ReadyA = 0;
				A->StartExperience(PlaylistId);
				B->StartExperience(PlaylistId);
				Stage = 0;
				return false;
			case 0:
				if (!Ready(A) || !Ready(B))
				{
					return false;
				}

				VerifyReady(A, WorldA);
				VerifyReady(B, WorldB);
				Test->TestTrue(TEXT("两 World 共享只读 Definition"), A->GetExperience() == B->GetExperience());
				Test->TestTrue(TEXT("实际 Level 实例独立"), A->GetMapInstance()->GetLoadedLevel() != B->GetMapInstance()->GetLoadedLevel());
				Test->TestEqual(TEXT("Feature 两个租约"), Features->GetLeaseCount(URL), 2);
				Test->TestEqual(TEXT("取消的旧请求没有 Ready 通知"), ReadyA, 1);
				Test->TestEqual(TEXT("B 一次 Ready 通知"), ReadyB, 1);
				{
					const int64 Generation = A->GetSnapshot().Generation;
					A->StartExperience(PlaylistId);
					Test->TestEqual(TEXT("重复 Start 幂等"), A->GetSnapshot().Generation, Generation);
					Test->TestEqual(TEXT("重复 Start 不重复通知"), ReadyA, 1);
					FDataValidationContext Context;
					const UPlaylistDefinition* LoadedPlaylist = Cast<UPlaylistDefinition>(Assets->GetPrimaryAssetPath(PlaylistId).ResolveObject());
					Test->TestTrue(TEXT("真实 Playlist 原生 Validation"), LoadedPlaylist && LoadedPlaylist->IsDataValid(Context) == EDataValidationResult::Valid);
					UPackage* FixturePackage = CreatePackage(TEXT("/MobaClassic/Definitions/Task007PlayerCapacity"));
					UExperienceDefinition* CapacityExperience = DuplicateObject<UExperienceDefinition>(A->GetExperience(), FixturePackage);
					CapacityExperience->SetFlags(RF_Transient);
					CapacityExperience->Players.MaximumParticipants = UMapDefinition::CountPlayerStarts(A->GetMapInstance()->GetLoadedLevel()) + 1;
					CapacityExperience->Players.MaximumPlayersPerTeam = CapacityExperience->Players.MaximumParticipants;
					UPlaylistDefinition* CapacityPlaylist = DuplicateObject<UPlaylistDefinition>(LoadedPlaylist, GetTransientPackage());
					CapacityPlaylist->SetFlags(RF_Transient);
					CapacityPlaylist->Experience = CapacityExperience;
					FDataValidationContext CapacityContext;
					Test->TestTrue(TEXT("比赛容量超过真实地图出生点时组合校验拒绝"), CapacityPlaylist->IsDataValid(CapacityContext) == EDataValidationResult::Invalid);
					Test->TestEqual(TEXT("容量不兼容产生明确单项错误"), CapacityContext.GetNumErrors(), 1);
					FDataValidationContext ExperienceContext;
					Test->TestTrue(TEXT("真实 Experience 原生 Validation"), A->GetExperience()->IsDataValid(ExperienceContext) == EDataValidationResult::Valid);
					FDataValidationContext MapContext;
					Test->TestTrue(TEXT("真实 Map 原生 Validation"), A->GetMap()->IsDataValid(MapContext) == EDataValidationResult::Valid);
				}

				A->StopExperience();
				++Stage;
				break;
			case 1:
				if (!Unloaded(A))
				{
					return false;
				}

				VerifyClean(A, WorldA);
				VerifyReady(B, WorldB);
				Test->TestEqual(TEXT("A 退出只归还一个租约"), Features->GetLeaseCount(URL), 1);
				Test->TestTrue(TEXT("B 使用时 Feature 保持 Loaded"), Features->GetState(URL) == EExperienceFeatureState::Loaded);
				{
					UPackage* SharedPackage = CreatePackage(TEXT("/Game/Task007SharedDefinitions"));
					UExperienceDefinition* SharedExperience = DuplicateObject<UExperienceDefinition>(B->GetExperience(), SharedPackage, TEXT("Experience_Shared"));
					UMapDefinition* SharedMap = DuplicateObject<UMapDefinition>(B->GetMap(), SharedPackage, TEXT("Map_Shared"));
					for (UObject* Fixture : {static_cast<UObject*>(SharedExperience), static_cast<UObject*>(SharedMap)})
					{
						Fixture->SetFlags(RF_Transient);
						Fixture->AddToRoot();
						DefinitionFixtures.Add(Fixture);
						Test->TestTrue(TEXT("注册宿主目录共用Definition"), Assets->RegisterSpecificPrimaryAsset(Fixture->GetPrimaryAssetId(), FAssetData(Fixture)));
					}

					StartFixture(TEXT("SharedDefinitions"), TEXT("MobaClassic"), *SharedExperience->GetPathName(), *SharedMap->GetPathName());
					FDataValidationContext SharedContext;
					Test->TestTrue(TEXT("宿主共用定义无需位于指定Feature目录"), Fixtures.Last()->IsDataValid(SharedContext) == EDataValidationResult::Valid);
				}

				++Stage;
				break;
			case 2:
				if (!Ready(A))
				{
					return false;
				}

				VerifyReady(A, WorldA);
				Test->TestEqual(TEXT("A 第二轮恰好新增一次 Ready"), ReadyA, 2);
				Test->TestTrue(TEXT("实际加载的Experience来自Game目录"), A->GetExperience()->GetPathName().StartsWith(TEXT("/Game/Task007SharedDefinitions.")));
				Test->TestTrue(TEXT("实际加载的MapDef来自Game目录"), A->GetMap()->GetPathName().StartsWith(TEXT("/Game/Task007SharedDefinitions.")));
				A->StopExperience();
				B->StopExperience();
				++Stage;
				break;
			case 3:
				if (!Unloaded(A) || !Unloaded(B) || !HasReleasedFeature(Features, URL))
				{
					return false;
				}

				VerifyClean(A, WorldA);
				VerifyClean(B, WorldB);
				Test->TestEqual(TEXT("最后 World 退出无租约"), Features->GetLeaseCount(URL), 0);
				if (GIsEditor)
				{
					Test->TestTrue(TEXT("Editor 零租约保留原生 Loaded 或 Active"), HasReleasedFeature(Features, URL));
				}
				else
				{
					Test->TestTrue(TEXT("插件已取消注册"), UGameFeaturesSubsystem::Get().GetGameFeatureDataForRegisteredPluginByURL(URL) == nullptr);
				}
				Test->TestTrue(TEXT("任意未允许身份拒绝"), A->StartExperience(FPrimaryAssetId(TEXT("Playlist"), TEXT("NotAllowed"))) == EExperienceFailure::PlaylistNotAllowed);
				{
					const FPrimaryAssetId Missing(TEXT("Playlist"), TEXT("MissingTask002"));
					ConfigurationOverride->Get()->AllowedPlaylists.Add(Missing);
					Test->TestTrue(TEXT("缺失宿主 Playlist 分类"), A->StartExperience(Missing) == EExperienceFailure::MissingPlaylist);
					Test->TestFalse(TEXT("缺失 Playlist 不留资源"), A->HasOwnedResources());
					ConfigurationOverride->Get()->AllowedPlaylists.Remove(Missing);
				}

				StartFixture(TEXT("MissingFeature"), TEXT("Task002MissingFeature"), TEXT("/Task002MissingFeature/Experience.Experience"), TEXT("/Task002MissingFeature/Map.Map"));
				++Stage;
				break;
			case 4:
				if (!Failed(A, EExperienceFailure::MissingFeature))
				{
					return false;
				}

				StartFixture(TEXT("MissingDefinition"), TEXT("MobaClassic"), TEXT("/MobaClassic/Definitions/Task002Missing.Task002Missing"),
					TEXT("/MobaClassic/Definitions/Map_FrameworkTest.Map_FrameworkTest"));
				++Stage;
				break;
			case 5:
				if (!Failed(A, EExperienceFailure::MissingDefinition))
				{
					return false;
				}

				// 失败后立即重入，覆盖原生 Feature unload 在途时的重新 acquire。
				A->StartExperience(PlaylistId);
				++Stage;
				break;
			case 6:
				if (!Ready(A))
				{
					if (A->GetSnapshot().State == EExperienceState::Failed)
					{
						Test->AddError(TEXT("失败后重试不能恢复真实加载"));
						return true;
					}

					return false;
				}

				VerifyReady(A, WorldA);
				Test->TestEqual(TEXT("所有失败都不产生 Ready"), ReadyA, 3);
				// Ready 通知内重入停止路径在最后一轮单独检查。
				A->StopExperience();
				++Stage;
				break;
			case 7:
				if (!Unloaded(A) || !HasReleasedFeature(Features, URL))
				{
					return false;
				}

				VerifyClean(A, WorldA);
				GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, WorldA);
				GMP::FMessageUtils::ListenObjectMessage(A, MOBA_MESSAGE_EXPERIENCE_CHANGED, WorldA,
					[this](const FExperienceSnapshot& State)
					{
						if (State.State == EExperienceState::ExperienceReady)
						{
							++ReadyA;
							A->StopExperience();
							Test->TestTrue(TEXT("通知 payload 是稳定值副本"), State.State == EExperienceState::ExperienceReady);
						}
					});
				A->StartExperience(PlaylistId);
				++Stage;
				break;
			case 8:
				if (ReadyA != 4 || !Unloaded(A))
				{
					return false;
				}

				if (!FixtureLease.IsValid())
				{
					if (!HasReleasedFeature(Features, URL))
					{
						return false;
					}

					FixtureLease = FGuid::NewGuid();
					Features->Acquire(TEXT("MobaClassic"), FixtureLease, URL);
					return false;
				}

				if (Features->GetState(URL) != EExperienceFeatureState::Loaded)
				{
					return false;
				}

				VerifyClean(A, WorldA);
				StartInvalidMapFixture(true);
				++Stage;
				break;
			case 9:
				if (!Failed(A, EExperienceFailure::InvalidMapAssociation))
				{
					return false;
				}

				StartInvalidMapFixture(false);
				++Stage;
				break;
			case 10:
				if (!Failed(A, EExperienceFailure::MapLoadFailed))
				{
					return false;
				}

				VerifyClean(A, WorldA);
				for (UObject* Fixture : DefinitionFixtures)
				{
					Fixture->RemoveFromRoot();
				}

				DefinitionFixtures.Empty();
				Features->Release(URL, FixtureLease);
				FixtureLease.Invalidate();
				++Stage;
				break;
			case 11:
				if (!HasReleasedFeature(Features, URL))
				{
					return false;
				}

				GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, WorldA);
				GMP::FMessageUtils::ListenObjectMessage(A, MOBA_MESSAGE_EXPERIENCE_CHANGED, WorldA,
					[this](const FExperienceSnapshot& State)
					{
						if (State.State == EExperienceState::Unloaded)
						{
							bTeardownRejected = A->StartExperience(PlaylistId) == EExperienceFailure::WorldUnavailable;
						}
					});
				A->StartExperience(PlaylistId);
				WorldA->DestroyWorld(false);
				Test->TestTrue(TEXT("World销毁通知中的重入请求明确拒绝"), bTeardownRejected);
				Test->TestFalse(TEXT("销毁没有遗留新请求资源"), A->HasOwnedResources());
				GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, WorldA);
				GEngine->DestroyWorldContext(WorldA);
				WorldA = nullptr;
				Test->AddInfo(TEXT("真实 Playlist→Feature→Definition→Map streaming→内容就绪；取消、两 World、重入、失败恢复和清理已执行。"));
				return true;
			}

			return false;
		}


	private:
		const FPrimaryAssetId PlaylistId{TEXT("Playlist"), TEXT("Playlist_FrameworkTest")};

		UWorld* CreateWorld(FName Name)
		{
			const UWorld::InitializationValues Values =
				UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(
					false);
			// CreateWorld 已执行 InitializeNewWorld；重复初始化会重复创建 WorldSettings。
			UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("SharedWorld"), CreatePackage(*(TEXT("/Temp/") + Name.ToString())), true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			return World;
		}

		bool Failed(UExperienceSubsystem* System, EExperienceFailure Reason)
		{
			if (System->GetSnapshot().State != EExperienceState::Failed)
			{
				return false;
			}

			Test->TestTrue(TEXT("失败原因分类"), System->GetSnapshot().Failure == Reason);
			Test->TestFalse(TEXT("失败不保留本卡引用"), System->HasOwnedResources());
			return true;
		}

		void VerifyClean(UExperienceSubsystem* System, UWorld* World)
		{
			Test->TestFalse(TEXT("所有本卡引用归还"), System->HasOwnedResources());
			Test->TestNull(TEXT("没有残留组件"), World->GetWorldSettings()->FindComponentByClass<UExperienceContextComponent>());
			Test->TestEqual(TEXT("地图实例已移出 World"), World->GetStreamingLevels().Num(), 0);
		}

		void StartFixture(const TCHAR* Name, const TCHAR* Feature, const TCHAR* ExperiencePath, const TCHAR* MapPath)
		{
			UPackage* Package = CreatePackage(*(FString(TEXT("/Temp/Task002")) + Name));
			UPlaylistDefinition* Fixture = NewObject<UPlaylistDefinition>(Package, Name, RF_Transient);
			Fixture->AddToRoot();
			Fixture->FeatureName = Feature;
			Fixture->Experience = TSoftObjectPtr<UExperienceDefinition>(FSoftObjectPath(ExperiencePath));
			Fixture->Map = TSoftObjectPtr<UMapDefinition>(FSoftObjectPath(MapPath));
			Fixtures.Add(Fixture);
			const FPrimaryAssetId Id(TEXT("Task002Fixture"), Name);
			FAssetBundleData Bundle;
			// UE 的空 bundle 表示删除动态注册；非空 bundle 才真正登记该 fixture。
			Bundle.AddBundleAsset(TEXT("Fixture"), FSoftObjectPath(Fixture).GetAssetPath());
			Test->TestTrue(TEXT("注册瞬态Playlist fixture"), Assets->AddDynamicAsset(Id, FSoftObjectPath(Fixture), Bundle));
			ConfigurationOverride->Get()->AllowedPlaylists.Add(Id);
			A->StartExperience(Id);
		}

		void StartInvalidMapFixture(bool bMismatch)
		{
			const FString Suffix = bMismatch ? TEXT("EmptyWorld") : TEXT("MissingWorld");
			UPackage* Package = CreatePackage(*(TEXT("/MobaClassic/Definitions/Task002") + Suffix));
			UMapDefinition* TestMap = NewObject<UMapDefinition>(Package, *(TEXT("Map") + Suffix), RF_Transient);
			UExperienceDefinition* TestExperience = NewObject<UExperienceDefinition>(Package, *(TEXT("Experience") + Suffix), RF_Transient);
			TestMap->World = TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/MobaClassic/Maps/Task002AbsentWorld.Task002AbsentWorld")));
			if (bMismatch)
			{
				TestMap->World.Reset();
			}

			TestExperience->Teams.Teams = {7};
			for (UObject* Fixture : {static_cast<UObject*>(TestMap), static_cast<UObject*>(TestExperience)})
			{
				Fixture->AddToRoot();
				DefinitionFixtures.Add(Fixture);
				Test->TestTrue(TEXT("注册瞬态 Definition fixture"), Assets->RegisterSpecificPrimaryAsset(Fixture->GetPrimaryAssetId(), FAssetData(Fixture)));
			}

			StartFixture(*Suffix, TEXT("MobaClassic"), *TestExperience->GetPathName(), *TestMap->GetPathName());
		}

		void Cleanup()
		{
			for (UWorld* World : {WorldA, WorldB})
			{
				if (World)
				{
					GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, World);
					GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_FEATURE_CHANGED, World);
					if (UExperienceSubsystem* System = World->GetSubsystem<UExperienceSubsystem>())
					{
						System->StopExperience();
					}

					World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
					World->DestroyWorld(false);
					GEngine->DestroyWorldContext(World);
				}
			}

			WorldA = nullptr;
			WorldB = nullptr;
			if (Assets)
			{
				for (UPlaylistDefinition* Fixture : Fixtures)
				{
					ConfigurationOverride->Get()->AllowedPlaylists.Remove(FPrimaryAssetId(TEXT("Task002Fixture"), Fixture->GetFName()));
					Fixture->RemoveFromRoot();
				}

				Assets->RemovePrimaryAssetType(TEXT("Task002Fixture"));
				for (UObject* Fixture : DefinitionFixtures)
				{
					Fixture->RemoveFromRoot();
				}

				if (FixtureLease.IsValid())
				{
					Features->Release(URL, FixtureLease);
				}
			}
		}


	private:
		bool Ready(UExperienceSubsystem* System) const { return System->GetSnapshot().State == EExperienceState::ExperienceReady; }
		bool Unloaded(UExperienceSubsystem* System) const { return System->GetSnapshot().State == EExperienceState::Unloaded; }

		void VerifyReady(UExperienceSubsystem* System, UWorld* World)
		{
			Test->TestTrue(TEXT("目标 World Ready"), Ready(System));
			Test->TestEqual(TEXT("保存资产读回比赛容量"), System->GetExperience()->Players.MaximumParticipants, 2);
			Test->TestEqual(TEXT("保存资产读回最低开局人数"), System->GetExperience()->Players.MinimumPlayersToStart, 1);
			Test->TestEqual(TEXT("保存资产读回每队人数上限"), System->GetExperience()->Players.MaximumPlayersPerTeam, 2);
			Test->TestNull(TEXT("不再装配冗余上下文组件"), World->GetWorldSettings()->FindComponentByClass<UExperienceContextComponent>());
			ULevel* Level = System->GetMapInstance() ? System->GetMapInstance()->GetLoadedLevel() : nullptr;
			if (Test->TestNotNull(TEXT("真实 streaming level"), Level))
			{
				Test->TestTrue(TEXT("地图可见并属于目标 World"), System->GetMapInstance()->IsLevelVisible() && Level->GetWorld() == World);
				Test->TestTrue(TEXT("地图中真实地面存在"), Level->Actors.ContainsByPredicate([](const AActor* Actor) { return IsValid(Actor) && Actor->IsA<AStaticMeshActor>(); }));
			}
		}


	private:
		FAutomationTestBase* Test;

		UWorld* WorldA = nullptr;

		UWorld* WorldB = nullptr;

		UExperienceSubsystem* A = nullptr;

		UExperienceSubsystem* B = nullptr;

		UMobaAssetManager* Assets = nullptr;

		TUniquePtr<FScopedGameConfigurationOverride> ConfigurationOverride;

		UExperienceFeatureSubsystem* Features = nullptr;

		TArray<UPlaylistDefinition*> Fixtures;

		TArray<UObject*> DefinitionFixtures;

		FString URL;

		FGuid FixtureLease;

		int32 Stage = -2;

		int32 ReadyA = 0;

		int32 ReadyB = 0;

		double StartTime = 0;


		bool bTeardownRejected = false;

	};
}

namespace
{
	class FExperienceCallbackTimeout final : public IAutomationLatentCommand
	{

	public:
		explicit FExperienceCallbackTimeout(FAutomationTestBase* InTest) : Test(InTest) {}

		virtual ~FExperienceCallbackTimeout() override
		{
			if (World)
			{
				GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, World);
				System->StopExperience();
				World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
				World->DestroyWorld(false);
				GEngine->DestroyWorldContext(World);
			}
		}

		virtual bool Update() override
		{
			if (!World)
			{
				const UWorld::InitializationValues Values = UWorld::InitializationValues()
																.AllowAudioPlayback(false)
																.CreatePhysicsScene(true)
																.CreateNavigation(false)
																.CreateAISystem(false)
																.ShouldSimulatePhysics(false)
																.SetTransactional(false);
				World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("CallbackTimeout"), CreatePackage(TEXT("/Temp/Task007CallbackTimeout")), true, ERHIFeatureLevel::Num, &Values);
				GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
				System = World->GetSubsystem<UExperienceSubsystem>();
				World->GetTimerManager().Tick(0.0f);
				GMP::FMessageUtils::ListenObjectMessage(System, MOBA_MESSAGE_EXPERIENCE_CHANGED, World,
					[this](const FExperienceSnapshot& State)
					{
						if (State.State == EExperienceState::ExperienceReady)
						{
							++ReadyCount;
						}
					});
				Test->TestTrue(TEXT("超时场景真实请求受理"), System->StartExperience({TEXT("Playlist"), TEXT("Playlist_FrameworkTest")}) == EExperienceFailure::None);
				Started = FPlatformTime::Seconds();
				return false;
			}

			if (!bTimeoutTriggered)
			{
				// 推进原生单次计时器的虚拟时间，不真实等待一分钟，也不调用子系统 Tick。
				bTimeoutTriggered = true;
				World->GetTimerManager().Tick(61.0f);
			}
			else
			{
				World->Tick(LEVELTICK_All, 0.016f);
			}

			if (FPlatformTime::Seconds() - Started > 20.0)
			{
				Test->AddError(TEXT("通知驱动超时清理未完成"));
				return true;
			}

			if (System->GetSnapshot().State != EExperienceState::Failed)
			{
				return false;
			}

			Test->TestTrue(TEXT("单次Timer产生明确Timeout"), System->GetSnapshot().Failure == EExperienceFailure::Timeout);
			Test->TestEqual(TEXT("超时与旧完成回调不能产生Ready"), ReadyCount, 0);
			Test->TestFalse(TEXT("超时清理资产、租约、计时器与订阅"), System->HasOwnedResources());
			return true;
		}


	private:
		FAutomationTestBase* Test;

		UWorld* World = nullptr;

		UExperienceSubsystem* System = nullptr;

		int32 ReadyCount = 0;

		bool bTimeoutTriggered = false;

		double Started = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExperienceCallbackTimeoutTest, "Moba.Experience.CallbackTimeout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FExperienceCallbackTimeoutTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FExperienceCallbackTimeout>(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMigratedAssetValidationTest, "Moba.Experience.MigratedAssetValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMigratedAssetValidationTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FMigratedAssetValidation>(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFeatureImmediateCancellationTest, "Moba.Experience.FeatureImmediateCancellation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFeatureImmediateCancellationTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FFeatureImmediateCancellation>(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExperienceWorldLifecycleTest, "Moba.Experience.WorldLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FExperienceWorldLifecycleTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FExperienceWorldScenario>(this));
	return true;
}

#endif
