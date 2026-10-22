#include "Experience/ExperienceSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Combat/CombatGameplayEffects.h"
#include "Combat/TargetDamageAbility.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceFeatureSubsystem.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeatureTypes.h"
#include "Hero/HeroDefinition.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "MobaGameplayTags.h"
#include "MobaMessageTags.h"

namespace
{
	class FLineConeExperienceLoading final : public IAutomationLatentCommand
	{
	public:
		explicit FLineConeExperienceLoading(FAutomationTestBase* InTest) : Test(InTest) {}

		virtual ~FLineConeExperienceLoading() override
		{
			if (World)
			{
				GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, World);
				World->GetSubsystem<UExperienceSubsystem>()->StopExperience();
				// 正常路径等待实际移除；失败路径由 World teardown 取消，不能无界 flush 或轮询。
				World->DestroyWorld(false);
				GEngine->DestroyWorldContext(World);
			}
		}

		virtual bool Update() override
		{
			if (!World)
			{
				// 专用 fresh 进程强制冷启动；同进程套件可复用已加载对象，但不据此宣称冷启动证据。
				const bool bRequireCold = FParse::Param(FCommandLine::Get(), TEXT("MobaLineConeRequireCold"));
				Test->AddInfo(FString::Printf(TEXT("LineCone loading mode=%s"), bRequireCold ? TEXT("ColdRequired") : TEXT("Reusable")));
				if (bRequireCold)
				{
					Test->TestNull(TEXT("fresh 进程尚未预热 LineCone Hero"), FSoftObjectPath(TEXT("/MobaClassic/Definitions/LineCone/Hero_LineCone.Hero_LineCone")).ResolveObject());
					Test->TestNull(TEXT("fresh 进程尚未预热 LineCone AbilitySet"), FSoftObjectPath(TEXT("/MobaClassic/Definitions/LineCone/AbilitySet_LineCone.AbilitySet_LineCone")).ResolveObject());
					Test->TestNull(TEXT("fresh 进程尚未预热 Line GA"), FSoftObjectPath(TEXT("/MobaClassic/Tests/LineCone/Abilities/GA_LineInstant.GA_LineInstant_C")).ResolveObject());
					Test->TestNull(TEXT("fresh 进程尚未预热 Cone GA"), FSoftObjectPath(TEXT("/MobaClassic/Tests/LineCone/Abilities/GA_ConeInstant.GA_ConeInstant_C")).ResolveObject());
				}

				const UWorld::InitializationValues Values = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
					.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
				World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("LineConeExperienceLoading"), nullptr, true, ERHIFeatureLevel::Num, &Values);
				GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
				auto* System = World->GetSubsystem<UExperienceSubsystem>();
				GMP::FMessageUtils::ListenObjectMessage(System, MOBA_MESSAGE_EXPERIENCE_CHANGED, World,
					[this](const FExperienceSnapshot& Snapshot) { Notifications.Add(Snapshot); });
				StepStarted = FPlatformTime::Seconds();
				Test->TestTrue(TEXT("真实通知链受理取消前请求"), System->StartExperience(PlaylistId()) == EExperienceFailure::None);
				CancelledGeneration = System->GetSnapshot().Generation;
				Test->TestTrue(TEXT("取消发生在加载中"), System->GetSnapshot().State == EExperienceState::Loading);
				System->StopExperience();
				return false;
			}

			World->Tick(LEVELTICK_All, 0.016f);
			auto* System = World->GetSubsystem<UExperienceSubsystem>();
			const FExperienceSnapshot Snapshot = System->GetSnapshot();
			if (Test->HasAnyErrors() || Snapshot.State == EExperienceState::Failed || FPlatformTime::Seconds() - StepStarted > 60)
			{
				Test->AddError(FString::Printf(TEXT("LineCone 加载回归失败 Cycle=%d Stopping=%d State=%d Failure=%d"), CompletedLoads, bWaitingForStop,
					int32(Snapshot.State), int32(Snapshot.Failure)));
				return true;
			}

			if (bWaitingForStop)
			{
				if (Snapshot.State != EExperienceState::Unloaded)
				{
					return false;
				}

				Test->TestFalse(TEXT("取消/停止释放句柄地图计时器订阅与租约"), System->HasOwnedResources());
				Test->TestNull(TEXT("停止清空 Experience"), System->GetExperience());
				Test->TestNull(TEXT("停止清空 MapDefinition"), System->GetMap());
				Test->TestNull(TEXT("停止清空 Streaming 实例"), System->GetMapInstance());
				Test->TestTrue(TEXT("停止通知实际到达"), HasNotification(Snapshot.Generation, EExperienceState::Unloaded));
				Test->TestFalse(TEXT("已取消代次没有延迟 Ready"), HasNotification(CancelledGeneration, EExperienceState::ExperienceReady));
				Test->TestTrue(TEXT("取消淘汰旧代次"), Snapshot.Generation > CancelledGeneration);
				if (CompletedLoads > 0)
				{
					FString URL;
					UGameFeaturesSubsystem::Get().GetPluginURLByName(TEXT("MobaClassic"), URL);
					const UExperienceFeatureSubsystem* Features = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
					Test->TestEqual(TEXT("停止后进程租约归零"), Features->GetLeaseCount(URL), 0);
					if (GIsEditor)
					{
						const EGameFeaturePluginState State = UGameFeaturesSubsystem::Get().GetPluginState(URL);
						Test->TestTrue(TEXT("Editor 停止后插件至少 Loaded"), State == EGameFeaturePluginState::Loaded || State == EGameFeaturePluginState::Active);
					}
					else if (Features->GetState(URL) != EExperienceFeatureState::Absent)
					{
						return false;
					}
					else
					{
						Test->TestNull(TEXT("Game 停止后插件已取消注册"), UGameFeaturesSubsystem::Get().GetGameFeatureDataForRegisteredPluginByURL(URL));
					}
				}

				if (CompletedLoads == 2)
				{
					Test->AddInfo(TEXT("LineCone cancel/reload: 两次真实 Ready 与完整停止均已检查"));
					return true;
				}

				StepStarted = FPlatformTime::Seconds();
				Test->TestTrue(TEXT("受理冷加载或再次加载"), System->StartExperience(PlaylistId()) == EExperienceFailure::None);
				RequestedGeneration = System->GetSnapshot().Generation;
				Test->TestTrue(TEXT("同次在途请求幂等"), System->StartExperience(PlaylistId()) == EExperienceFailure::None);
				Test->TestEqual(TEXT("幂等不创建新代次"), System->GetSnapshot().Generation, RequestedGeneration);
				bWaitingForStop = false;
				return false;
			}

			if (Snapshot.State != EExperienceState::ExperienceReady)
			{
				return false;
			}

			Test->TestEqual(TEXT("Ready 属于本次请求"), Snapshot.Generation, RequestedGeneration);
			Test->TestTrue(TEXT("Ready GMP 通知实际到达"), HasNotification(RequestedGeneration, EExperienceState::ExperienceReady));
			CheckLoadedClosure(System);
			++CompletedLoads;
			StepStarted = FPlatformTime::Seconds();
			System->StopExperience();
			bWaitingForStop = true;
			return false;
		}


	private:
		bool HasNotification(int64 Generation, EExperienceState State) const
		{
			return Notifications.ContainsByPredicate([Generation, State](const FExperienceSnapshot& Snapshot) { return Snapshot.Generation == Generation && Snapshot.State == State; });
		}

		void CheckLoadedClosure(const UExperienceSubsystem* System)
		{
			const UExperienceDefinition* Experience = System->GetExperience();
			const UMapDefinition* Map = System->GetMap();
			const ULevelStreamingDynamic* Streaming = System->GetMapInstance();
			if (!Test->TestNotNull(TEXT("Ready 持有 Experience"), Experience) || !Test->TestNotNull(TEXT("Playlist 持有唯一 Map"), Map)
				|| !Test->TestNotNull(TEXT("实际动态地图实例"), Streaming))
			{
				return;
			}

			Test->TestEqual(TEXT("加载本卡 Experience"), Experience->GetPathName(), FString(TEXT("/MobaClassic/Definitions/LineCone/Experience_LineCone.Experience_LineCone")));
			Test->TestEqual(TEXT("Playlist 唯一地图引用"), Map->GetPathName(), FString(TEXT("/MobaClassic/Definitions/Map_FrameworkTest.Map_FrameworkTest")));
			Test->TestTrue(TEXT("地图实际 loaded visible"), Streaming->IsLevelLoaded() && Streaming->IsLevelVisible());
			FString Reason;
			Test->TestTrue(TEXT("人数规则有效"), Experience->Players.IsValid(Experience->Teams.Teams.Num(), Reason));
			Test->TestEqual(TEXT("总容量保持两个参赛者"), Experience->Players.MaximumParticipants, 2);
			Test->TestTrue(TEXT("LoadedLevel 真实出生点满足容量"), UMapDefinition::CountPlayerStarts(Streaming->GetLoadedLevel()) >= Experience->Players.MaximumParticipants);
			Test->TestFalse(TEXT("本样例禁用物品"), Experience->bItemsEnabled);
			Test->TestTrue(TEXT("禁用物品不引用目录"), Experience->ItemCatalog.IsNull());
			Test->TestTrue(TEXT("Ready 仍持有资源"), System->HasOwnedResources());

			const UHeroDefinition* Hero = Experience->Hero.Get();
			if (!Test->TestNotNull(TEXT("异步闭包已加载 Hero"), Hero) || !Test->TestNotNull(TEXT("异步闭包已加载形体"), Hero->BodyMesh.Get()))
			{
				return;
			}

			const UAbilitySetDefinition* AbilitySet = Hero->AbilitySet.Get();
			if (!Test->TestNotNull(TEXT("异步闭包已加载 AbilitySet"), AbilitySet) || !Test->TestEqual(TEXT("精确两个 GA"), AbilitySet->Abilities.Num(), 2))
			{
				return;
			}

			UAssetManager& AssetManager = UAssetManager::Get();
			const UPlaylistDefinition* Playlist = Cast<UPlaylistDefinition>(AssetManager.GetPrimaryAssetPath(PlaylistId()).ResolveObject());
			if (Test->TestNotNull(TEXT("宿主 Playlist 已解析"), Playlist))
			{
				Test->TestTrue(TEXT("Playlist 对应已解析定义"), Playlist->Experience.Get() == Experience && Playlist->Map.Get() == Map);
			}

			for (const UPrimaryDataAsset* Definition : TArray<const UPrimaryDataAsset*>{Playlist, Experience, Hero, AbilitySet})
			{
				if (Definition)
				{
					Test->TestEqual(TEXT("四 PrimaryAsset 扫描身份实际登记"), AssetManager.GetPrimaryAssetIdForPath(FSoftObjectPath(Definition)), Definition->GetPrimaryAssetId());
				}
			}

			Test->TestTrue(TEXT("异步 GA 闭包满足来源授予"), AbilitySet->ValidateGrant(Reason));
			for (int32 Index = 0; Index < 2; ++Index)
			{
				const UClass* AbilityClass = AbilitySet->Abilities[Index].Ability.Get();
				const auto* Ability = AbilityClass ? Cast<UTargetDamageAbility>(AbilityClass->GetDefaultObject()) : nullptr;
				if (!Test->TestNotNull(TEXT("异步闭包已加载保存 GA 类与 CDO"), Ability))
				{
					continue;
				}

				Test->TestEqual(TEXT("直接父类保持通用 archetype"), AbilityClass->GetSuperClass(), UTargetDamageAbility::StaticClass());
				Test->TestTrue(TEXT("两种几何与 Instant 保持"), Ability->GetGeometry().Policy == (Index == 0 ? EAbilityTargetPolicy::Line : EAbilityTargetPolicy::Cone)
					&& Ability->Delivery == EAbilityDelivery::Instant);
				Test->TestEqual(TEXT("保存技能槽"), AbilitySet->Abilities[Index].Slot, FGameplayTag(Index == 0 ? MobaGameplayTags::Input_Slot_Primary : MobaGameplayTags::Input_Slot_Target));
				Test->TestNotNull(TEXT("GA 硬引用伤害 GE 已加载"), Ability->DamageEffect.Get());
				Test->TestNotNull(TEXT("GA 成本 GE 已加载"), Ability->GetCostGameplayEffect());
				Test->TestNotNull(TEXT("GA 冷却 GE 已加载"), Ability->GetCooldownGameplayEffect());
			}

			Test->TestEqual(TEXT("两种客户端 Profile"), Experience->InputProfiles.Num(), 2);
			Test->TestEqual(TEXT("两种允许输入"), Experience->AllowedInputProfiles.Num(), 2);
			TSet<EHeroInputProfile> Profiles;
			for (const auto& ProfileReference : Experience->InputProfiles)
			{
				const UInputProfileDefinition* Profile = ProfileReference.Get();
				if (!Test->TestNotNull(TEXT("Client bundle 已加载 Profile"), Profile))
				{
					continue;
				}

				Test->TestFalse(TEXT("实际 Profile 唯一"), Profiles.Contains(Profile->Profile));
				Profiles.Add(Profile->Profile);
				Test->TestTrue(TEXT("Profile 在允许集合且必需动作/IMC 已加载"), Experience->AllowedInputProfiles.Contains(Profile->Profile) && Profile->IsLoaded());
				Test->TestNotNull(TEXT("Target IMC 闭包已加载"), Profile->TargetMapping.Get());
				Test->TestNotNull(TEXT("Target 动作闭包已加载"), Profile->TargetAbility.Get());
				if (Profile->Mapping.IsValid() && Profile->TargetMapping.IsValid())
				{
					Test->TestTrue(TEXT("Q 指向 Primary 动作"), HasMapping(Profile->Mapping.Get(), Profile->PrimaryAbility.Get(), EKeys::Q));
					Test->TestTrue(TEXT("R 指向 Target 动作"), HasMapping(Profile->TargetMapping.Get(), Profile->TargetAbility.Get(), EKeys::R));
					Test->TestTrue(TEXT("Tab 指向 Profile 切换"), HasMapping(Profile->Mapping.Get(), Profile->SwitchProfile.Get(), EKeys::Tab));
				}
			}

			Test->TestTrue(TEXT("WASD 与 ClickToMove 闭包完整"), Profiles.Contains(EHeroInputProfile::WASD) && Profiles.Contains(EHeroInputProfile::ClickToMove));
		}

		static bool HasMapping(const UInputMappingContext* Mapping, const UInputAction* Action, const FKey& Key)
		{
			return Action && Mapping->GetMappings().ContainsByPredicate([Action, Key](const FEnhancedActionKeyMapping& Entry) { return Entry.Action == Action && Entry.Key == Key; });
		}


	private:
		static FPrimaryAssetId PlaylistId() { return {TEXT("Playlist"), TEXT("Playlist_LineCone")}; }


	private:
		FAutomationTestBase* Test;

		UWorld* World = nullptr;

		TArray<FExperienceSnapshot> Notifications;

		double StepStarted = 0;

		int64 CancelledGeneration = 0;

		int64 RequestedGeneration = 0;

		int32 CompletedLoads = 0;

		bool bWaitingForStop = true;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLineConeExperienceLoadingTest, "Moba.Experience.LineConeLoading", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLineConeExperienceLoadingTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FLineConeExperienceLoading>(this));
	return true;
}

#endif
