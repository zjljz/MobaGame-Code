#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/NetDriver.h"
#include "EngineUtils.h"
#include "Player/HeroPlayerController.h"
#include "Match/ParticipantState.h"
#include "Match/MatchGameMode.h"
#include "Match/MatchGameState.h"
#include "Experience/ExperienceSubsystem.h"
#include "Experience/MobaAssetManager.h"
#include "Tests/Experience/GameConfigurationTestScope.h"
#include "Experience/ExperienceDefinitions.h"
#include "Session/MobaSessionSubsystem.h"
#include "Hero/HeroCharacter.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "MobaGameplayTags.h"

namespace
{
	class FResumeNetworkScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FResumeNetworkScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
		{
			FParse::Value(FCommandLine::Get(), TEXT("MobaResumeRole="), Role);
			FParse::Value(FCommandLine::Get(), TEXT("MobaResumeEvidence="), Evidence);
		}

		void Signal(const TCHAR* Name, const FString& Value = TEXT("ready")) { Test->TestTrue(TEXT("场景标记写入"), FFileHelper::SaveStringToFile(Value, *FPaths::Combine(Evidence, Name))); }
		virtual bool Update() override
		{
			if (Evidence.IsEmpty() || FPaths::IsRelative(Evidence))
			{
				Test->AddError(TEXT("必须通过MobaResumeEvidence指定项目AI下的绝对证据目录"));
				return true;
			}

			if (FPlatformTime::Seconds() - Started > 320)
			{
				Test->AddError(FString::Printf(TEXT("Resume场景超时 Role=%s Stage=%d"), *Role, Stage));
				return true;
			}

			UWorld* World = nullptr;
			for (const auto& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() && Context.World()->IsGameWorld())
				{
					World = Context.World();
					break;
				}
			}

			if (!World)
			{
				return false;
			}

			if (Role == TEXT("server"))
			{
				return Server(World);
			}

			auto* Controller = Cast<AHeroPlayerController>(World->GetFirstPlayerController());
			if (!Controller)
			{
				return false;
			}

			auto* State = World->GetGameState<AMatchGameState>();
			auto* Hero = Cast<AHeroCharacter>(Controller->GetPawn());
			if (Role == TEXT("revoked"))
			{
				if (Stage == 0)
				{
					if (!FParse::Value(FCommandLine::Get(), TEXT("MobaResumeAddress="), Address) || Address.IsEmpty())
					{
						Test->AddError(TEXT("撤销测试需先在Entry启动并指定MobaResumeAddress"));
						return true;
					}

					World->GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>()->RequireResume();
					GEngine->SetClientTravel(World, *Address, TRAVEL_Absolute);
					Stage = 1;
					return false;
				}

				if (Controller->GetAdmissionResult() != TEXT("MatchEnded"))
				{
					return false;
				}

				Test->TestNull(TEXT("Match结束后旧票据不能控制Hero"), Hero);
				Signal(TEXT("revocation-passed"), TEXT("done"));
				return true;
			}

			if (Role == TEXT("loadfail"))
			{
				if (Stage == 0 && Controller->GetAdmissionRequest().IsValid())
				{
					auto* Assets = CastChecked<UMobaAssetManager>(UAssetManager::GetIfInitialized());
					ConfigurationOverride = MakeUnique<FScopedGameConfigurationOverride>(*Assets);
					if (!Test->TestNotNull(TEXT("已加载配置根可供失败场景复制"), ConfigurationOverride->Get()))
					{
						return true;
					}

					UPackage* Package = CreatePackage(TEXT("/Temp/Task005FailedLoad"));
					Fixture = NewObject<UPlaylistDefinition>(Package, TEXT("MissingContent"), RF_Transient);
					Fixture->AddToRoot();
					Fixture->FeatureName = TEXT("Task005AbsentFeature");
					Fixture->Experience = TSoftObjectPtr<UExperienceDefinition>(FSoftObjectPath(TEXT("/Task005AbsentFeature/Missing.Missing")));
					Fixture->Map = TSoftObjectPtr<UMapDefinition>(FSoftObjectPath(TEXT("/Task005AbsentFeature/Map.Map")));
					const FPrimaryAssetId Id(TEXT("Task005Fixture"), TEXT("MissingContent"));
					FAssetBundleData Bundle;
					Bundle.AddBundleAsset(TEXT("Fixture"), FSoftObjectPath(Fixture).GetAssetPath());
					Assets->AddDynamicAsset(Id, FSoftObjectPath(Fixture), Bundle);
					ConfigurationOverride->Get()->AllowedPlaylists.Add(Id);
					World->GetSubsystem<UExperienceSubsystem>()->StartExperience(Id);
					Stage = 1;
				}

				if (Stage == 1 && Controller->GetAdmissionResult() == TEXT("ContentFailed"))
				{
					Test->TestTrue(TEXT("真实内容加载失败"), World->GetSubsystem<UExperienceSubsystem>()->GetSnapshot().State == EExperienceState::Failed);
					Test->TestNull(TEXT("加载失败没有控制Pawn"), Hero);
					Fixture->RemoveFromRoot();
					Fixture = nullptr;
					return true;
				}

				return false;
			}

			if (Role == TEXT("attacker"))
			{
				return Attacker(World, Controller, Hero);
			}

			if (Role == TEXT("missing"))
			{
				if (Controller->GetAdmissionResult().IsNone())
				{
					return false;
				}

				Test->TestEqual(TEXT("丢失本机票据明确失败"), Controller->GetAdmissionResult(), FName(TEXT("CredentialMissingOrUnreadable")));
				Test->TestNull(TEXT("失败没有Hero"), Hero);
				return true;
			}

			if (Role == TEXT("probes"))
			{
				return Probes(World, Controller, State);
			}

			if (Role == TEXT("networkloss") && Stage == 1)
			{
				if (World->GetNetMode() == NM_Client || Read(TEXT("detached")) == DetachedBefore)
				{
					return false;
				}

				World->GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>()->RequireResume();
				GEngine->SetClientTravel(World, *Address, TRAVEL_Absolute);
				Stage = 2;
				return false;
			}

			if (Role == TEXT("release"))
			{
				if (!Controller->GetAdmissionRequest().IsValid())
				{
					return false;
				}

				Test->TestNull(TEXT("预留尚无Pawn"), Hero);
				Signal(TEXT("release-reserved"));
				return true;
			}

			if (Role == TEXT("dead") && Stage == 0)
			{
				if (!Controller->GetAdmissionRequest().IsValid() || World->GetSubsystem<UExperienceSubsystem>()->GetSnapshot().State != EExperienceState::ExperienceReady)
				{
					return false;
				}

				Signal(TEXT("attack-command"), TEXT("1"));
				Stage = 1;
				return false;
			}

			if (Role == TEXT("dead") && Stage == 1)
			{
				if (Read(TEXT("attack-done")) != TEXT("1"))
				{
					return false;
				}

				Controller->ServerAdmissionReady(Controller->GetAdmissionRequest(), State->ExperienceInstance, true);
				Stage = 2;
				return false;
			}

			if (Role == TEXT("initial") && Stage == 3)
			{
				// latent command跨原生断线World继续；由服务器真实Detached标记决定何时重新连接。
				if (World->GetNetMode() == NM_Client || Read(TEXT("detached")).IsEmpty())
				{
					return false;
				}

				World->GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>()->RequireResume();
				GEngine->SetClientTravel(World, *Address, TRAVEL_Absolute);
				Stage = 4;
				return false;
			}

			if (!Hero || !State || !Hero->bInputReady || Controller->GetAdmissionResult() != TEXT("Bound"))
			{
				return false;
			}

			auto* Abilities = Hero->GetHeroAbilitySystem();
			if (!Abilities->HasReplicatedLoadout(Hero->RequiredLoadoutRevision))
			{
				return false;
			}

			if (Role == TEXT("initial"))
			{
				if (Stage == 0)
				{
					Entity = Hero->EntityId;
					Participant = Hero->ParticipantId;
					Epoch = Hero->ConnectionEpoch;
					Revision = Hero->RequiredLoadoutRevision;
					const auto Handles = Abilities->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Effect_Persistent)));
					if (Handles.Num() != 1)
					{
						return false;
					}

					PersistentStart = Abilities->GetActiveGameplayEffect(Handles[0])->StartServerWorldTime;
					Signal(TEXT("victim-identity"), Entity.ToString());
					Controller->MobaAbility();
					Stage = 1;
					return false;
				}

				if (Stage == 1)
				{
					if (Hero->GetAttributes()->GetMana() != 80 || !Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self))
					{
						return false;
					}

					Signal(TEXT("initial-ready"));
					Stage = 2;
					return false;
				}

				if (Stage == 2)
				{
					if (Read(TEXT("attacker-ready")).IsEmpty())
					{
						return false;
					}

					if (!bSecondAbilitySent)
					{
						if (Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self))
						{
							return false;
						}

						Controller->MobaAbility();
						bSecondAbilitySent = true;
						return false;
					}

					if (Hero->GetAttributes()->GetMana() != 60 || !Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self))
					{
						return false;
					}

					const auto Cooldowns = Abilities->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Cooldown_Self)));
					if (Cooldowns.Num() != 1)
					{
						return false;
					}

					CooldownStart = Abilities->GetActiveGameplayEffect(Cooldowns[0])->StartServerWorldTime;
					Address = World->URL.Host + TEXT(":") + FString::FromInt(World->URL.Port);
					GEngine->Exec(World, TEXT("disconnect"));
					Stage = 3;
					return false;
				}

				if (Stage == 4)
				{
					Test->TestEqual(TEXT("同进程原Entity"), Hero->EntityId, Entity);
					Test->TestEqual(TEXT("同进程原Participant"), Hero->ParticipantId, Participant);
					Test->TestTrue(TEXT("新控制代次"), Hero->ConnectionEpoch > Epoch);
					Test->TestEqual(TEXT("重连不重授revision"), Hero->RequiredLoadoutRevision, Revision);
					Test->TestEqual(TEXT("重连不重置资源"), Hero->GetAttributes()->GetMana(), 60.f);
					const auto Cooldowns = Abilities->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Cooldown_Self)));
					Test->TestEqual(TEXT("断线前短冷却继续"), Cooldowns.Num(), 1);
					if (Cooldowns.Num() == 1)
					{
						Test->TestEqual(TEXT("短冷却原服务器开始时间"), Abilities->GetActiveGameplayEffect(Cooldowns[0])->StartServerWorldTime, CooldownStart);
					}

					const auto Handles = Abilities->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Effect_Persistent)));
					Test->TestEqual(TEXT("持久效果恰好一个"), Handles.Num(), 1);
					if (Handles.Num() == 1)
					{
						Test->TestEqual(TEXT("原GE起始时间"), Abilities->GetActiveGameplayEffect(Handles[0])->StartServerWorldTime, PersistentStart);
					}

					Controller->ServerActivateSlot(Hero->EntityId, Epoch, Revision, MobaGameplayTags::Input_Slot_Primary);
					Stage = 5;
					return false;
				}

				if (Stage == 5)
				{
					if (Controller->GetLastIntentReason() != TEXT("Epoch"))
					{
						return false;
					}

					Test->TestFalse(TEXT("旧epoch RPC拒绝"), Controller->WasLastIntentAccepted());
					Signal(TEXT("same-process-passed"));
					return true;
				}
			}
			else
			{
				FGuid Expected;
				FGuid::Parse(Read(TEXT("victim-identity")), Expected);
				Test->TestEqual(TEXT("进程重启恢复原实体"), Hero->EntityId, Expected);
				Test->TestEqual(TEXT("初始化仅一次"), Hero->InitializationGeneration, uint32(1));
				Test->TestEqual(TEXT("两个原稳定槽"), Abilities->GetActivatableAbilities().Num(), 2);
				Test->TestTrue(TEXT("ActorInfo新Controller"), Abilities->AbilityActorInfo.IsValid() && Abilities->AbilityActorInfo->PlayerController == Controller);
				if (Role == TEXT("restart"))
				{
					Test->TestEqual(TEXT("重启仍为60资源"), Hero->GetAttributes()->GetMana(), 60.f);
					if (Stage == 0)
					{
						Controller->ServerAdmissionReady(FGuid::NewGuid(), State->ExperienceInstance, true);
						// 一个Game进程中的第二个原生LocalPlayer形成独立ChildConnection，检验有效旧连接不可抢占。
						FString Error;
						ULocalPlayer* Extra = World->GetGameInstance()->CreateLocalPlayer(1, Error, true);
						Test->TestNotNull(TEXT("原生第二本地连接"), Extra);
						Stage = 1;
						return false;
					}

					auto* Other = SecondController(World);
					if (!Other || Other->GetAdmissionResult().IsNone())
					{
						return false;
					}

					Test->TestEqual(TEXT("有效旧连接拒绝抢占"), Other->GetAdmissionResult(), FName(TEXT("ConnectionActive")));
					Test->TestEqual(TEXT("过期ready结果不能降级已提交绑定"), Controller->GetAdmissionResult(), FName(TEXT("Bound")));
					Test->TestTrue(TEXT("原连接控制保持"), Controller->IsLocallyReady());
				}
				else if (Role == TEXT("dead"))
				{
					Test->TestTrue(TEXT("重连仍Dead"), Hero->GetLifeSnapshot().State == EHeroLifeState::Dead);
					const float Remaining = Hero->GetLifeSnapshot().RespawnServerTime - State->GetServerWorldTimeSeconds();
					Test->TestTrue(TEXT("原剩余复活时间"), Remaining > 0 && Remaining < 4);
					Test->TestFalse(TEXT("Dead不开放玩家输入"), Controller->IsLocallyReady());
					Test->TestEqual(TEXT("未强制复活Health"), Hero->GetAttributes()->GetHealth(), 0.f);
					Signal(TEXT("dead-bound"));
				}
				else if (Role == TEXT("alive"))
				{
					Test->TestTrue(TEXT("离线已复活"), Hero->IsAlive());
					Test->TestEqual(TEXT("没有第二次复活"), Hero->GetLifeSnapshot().Generation, uint32(2));
					Test->TestEqual(TEXT("死亡唯一一次"), Hero->GetLifeSnapshot().DeathCount, uint32(1));
					Test->TestFalse(TEXT("未重授死亡清理GE"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::Effect_RemoveOnDeath));
					Test->TestEqual(TEXT("原复活规则资源"), Hero->GetAttributes()->GetMana(), 100.f);
				}
				else if (Role == TEXT("networkloss"))
				{
					if (Stage == 0)
					{
						Address = World->URL.Host + TEXT(":") + FString::FromInt(World->URL.Port);
						DetachedBefore = Read(TEXT("detached"));
						Epoch = Hero->ConnectionEpoch;
						Test->AddExpectedError(TEXT("UEngine::BroadcastNetworkFailure: FailureType = ConnectionTimeout"), EAutomationExpectedErrorFlags::Contains, 1);
						FPacketSimulationSettings Loss;
						Loss.PktLoss = 100;
						Loss.PktIncomingLoss = 100;
						World->GetNetDriver()->SetPacketSimulationSettings(Loss);
						Test->AddInfo(TEXT("原生双向100%丢包开始，等待真实网络timeout与服务器Detach"));
						Stage = 1;
						return false;
					}

					Test->TestTrue(TEXT("网络超时后新Epoch"), Hero->ConnectionEpoch > Epoch);
					Test->TestEqual(TEXT("真实断网后资源未重置"), Hero->GetAttributes()->GetMana(), 60.f);
				}

				Controller->MobaStatus();
				return true;
			}

			return false;
		}

	private:
		AHeroPlayerController* SecondController(UWorld* World)
		{
			for (const auto& Player : World->GetGameInstance()->GetLocalPlayers())
			{
				if (Player != World->GetGameInstance()->GetFirstGamePlayer())
				{
					return Cast<AHeroPlayerController>(Player->PlayerController);
				}
			}

			return nullptr;
		}

		bool Probes(UWorld* World, AHeroPlayerController* Controller, AMatchGameState* State)
		{
			if (!State || !State->MatchId.IsValid())
			{
				return false;
			}

			FResumeCredential Ticket;
			if (!World->GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>()->LoadCredential(Ticket))
			{
				Test->AddError(TEXT("探针缺受保护票据"));
				return true;
			}

			if (Stage == 0)
			{
				Ticket.Secret[0] ^= 1;
				Controller->ServerBeginAdmission(true, Ticket);
				Stage = 1;
			}
			else if (Stage == 1 && Controller->GetAdmissionResult() == TEXT("CredentialInvalid"))
			{
				Ticket.Match = FGuid::NewGuid();
				Controller->ServerBeginAdmission(true, Ticket);
				Stage = 2;
			}
			else if (Stage == 2 && Controller->GetAdmissionResult() == TEXT("WrongMatch"))
			{
				Ticket.Clear();
				Controller->ServerBeginAdmission(true, Ticket);
				Stage = 3;
			}
			else if (Stage == 3 && Controller->GetAdmissionResult() == TEXT("CredentialInvalid"))
			{
				Controller->ServerBeginAdmission(true, Ticket);
				Stage = 4;
			}
			else if (Stage == 4 && Controller->GetAdmissionRequest().IsValid())
			{
				Request = Controller->GetAdmissionRequest();
				FString Error;
				Test->TestNotNull(TEXT("并发原生ChildConnection"), World->GetGameInstance()->CreateLocalPlayer(1, Error, true));
				Stage = 5;
			}
			else if (Stage == 5)
			{
				if (auto* Other = SecondController(World); Other && Other->GetPlayerState<AParticipantState>())
				{
					Other->ServerBeginAdmission(true, Ticket);
					Stage = 6;
				}
			}
			else if (Stage == 6)
			{
				auto* Other = SecondController(World);
				if (Other && Other->GetAdmissionResult() == TEXT("SeatReserved"))
				{
					Test->TestNull(TEXT("并发输家没有Hero"), Other->GetPawn());
					Controller->ServerAdmissionReady(Request, State->ExperienceInstance, false);
					Stage = 7;
				}
			}
			else if (Stage == 7 && Controller->GetAdmissionResult() == TEXT("ContentFailed"))
			{
				Controller->ServerAdmissionReady(Request, State->ExperienceInstance, true);
				Stage = 8;
			}
			else if (Stage == 8 && Controller->GetAdmissionResult() == TEXT("StaleRequest"))
			{
				Test->TestNull(TEXT("旧ready不能取得控制"), Controller->GetPawn());
				if (auto* Other = SecondController(World))
				{
					Other->ServerBeginAdmission(true, Ticket);
					Stage = 9;
				}
			}
			else if (Stage == 9)
			{
				auto* Other = SecondController(World);
				if (Other && Other->GetAdmissionRequest().IsValid())
				{
					Test->AddInfo(TEXT("无效/错Match/空凭证拒绝、并发预留、加载失败释放、旧ready失效、另连接重新预留均通过"));
					Ticket.Clear();
					return true;
				}
			}

			Ticket.Clear();
			return false;
		}

		bool Attacker(UWorld* World, AHeroPlayerController* Controller, AHeroCharacter* Hero)
		{
			if (Read(TEXT("finish")) == TEXT("done"))
			{
				return true;
			}

			if (!Hero || !Controller->IsLocallyReady())
			{
				return false;
			}

			if (Stage == 0)
			{
				Signal(TEXT("attacker-ready"));
				Stage = 1;
			}

			if (Read(TEXT("prepare-finish")) == TEXT("yes"))
			{
				if (Stage == 1)
				{
					FString Error;
					Test->TestNotNull(TEXT("结束前待准入连接"), World->GetGameInstance()->CreateLocalPlayer(1, Error, true));
					Stage = 2;
				}

				if (auto* Other = SecondController(World); Other && Other->GetAdmissionResult() == TEXT("ConnectionActive"))
				{
					if (Read(TEXT("end-pending-ready")).IsEmpty())
					{
						Signal(TEXT("end-pending-ready"));
					}
				}

				return false;
			}

			AHeroCharacter* Victim = nullptr;
			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				if (*It != Hero)
				{
					Victim = *It;
					break;
				}
			}

			if (!Victim)
			{
				return false;
			}

			const FString Command = Read(TEXT("attack-command"));
			if (Command == TEXT("1"))
			{
				if (Victim->GetLifeSnapshot().State == EHeroLifeState::Dead)
				{
					if (Read(TEXT("attack-done")) != TEXT("1"))
					{
						Signal(TEXT("attack-done"), TEXT("1"));
					}

					return false;
				}

				if (Read(TEXT("attack-done")) == TEXT("1"))
				{
					return false;
				}

				if (Victim->bInputReady)
				{
					Test->AddError(TEXT("攻击时被测Hero尚未断线"));
					return true;
				}

				if (!Hero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack) && FPlatformTime::Seconds() - LastAttack > 0.3)
				{
					Controller->ServerActivateUnitSlot(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_UnitAttack, Victim);
					LastAttack = FPlatformTime::Seconds();
				}
			}

			return false;
		}

		bool Server(UWorld* World)
		{
			auto* Mode = World->GetAuthGameMode<AMatchGameMode>();
			if (!Mode)
			{
				return false;
			}

			if (Read(TEXT("finish")) == TEXT("requested"))
			{
				Mode->FinishMatch();
				Test->TestFalse(TEXT("Match关闭"), Mode->IsMatchOpen());
				Test->TestEqual(TEXT("结束清理包括待准入的PlayerArray"), World->GetGameState<AMatchGameState>()->PlayerArray.Num(), 0);
				for (TActorIterator<AHeroCharacter> It(World); It; ++It)
				{
					Test->AddError(TEXT("结束残留Hero"));
				}

				for (TActorIterator<AParticipantState> It(World); It; ++It)
				{
					Test->TestFalse(TEXT("结束不保留原参赛者"), Mode->OwnsParticipant(*It));
				}

				Signal(TEXT("finish"), TEXT("done"));
				Stage = 2;
				return false;
			}

			if (Stage == 2)
			{
				if (Read(TEXT("revocation-passed")) == TEXT("done"))
				{
					return true;
				}

				return false;
			}

			FGuid Expected;
			if (!FGuid::Parse(Read(TEXT("victim-identity")), Expected))
			{
				return false;
			}

			AHeroCharacter* Victim = nullptr;
			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				if (It->EntityId == Expected)
				{
					Victim = *It;
					break;
				}
			}

			if (!Victim)
			{
				Test->AddError(TEXT("服务器原Hero丢失"));
				return true;
			}

			auto* Abilities = Victim->GetHeroAbilitySystem();
			AParticipantState* ParticipantState = nullptr;
			for (TActorIterator<AParticipantState> It(World); It; ++It)
			{
				if (It->ParticipantId == Victim->ParticipantId)
				{
					ParticipantState = *It;
					break;
				}
			}

			if (Stage == 0)
			{
				OriginalHero = Victim;
				OriginalASC = Abilities;
				OriginalParticipant = ParticipantState;
				const auto Handles = Abilities->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Effect_Persistent)));
				if (Handles.Num() != 1)
				{
					return false;
				}

				PersistentHandle = Handles[0];
				PersistentStart = Abilities->GetActiveGameplayEffect(PersistentHandle)->StartServerWorldTime;
				PersistentEnd = PersistentStart + Abilities->GetActiveGameplayEffect(PersistentHandle)->Spec.GetDuration();
				Slot = Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary);
				Revision = Victim->RequiredLoadoutRevision;
				Stage = 1;
			}

			int32 MatchingParticipants = 0;
			for (TActorIterator<AParticipantState> It(World); It; ++It)
			{
				MatchingParticipants += It->ParticipantId == Victim->ParticipantId ? 1 : 0;
			}

			if (MatchingParticipants != 1)
			{
				Test->AddError(TEXT("重复参赛者PlayerState"));
				return true;
			}

			if (Victim != OriginalHero || Abilities != OriginalASC || ParticipantState != OriginalParticipant || Abilities->GetSourceCount() != 1
				|| Victim->RequiredLoadoutRevision != Revision || Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary) != Slot || Victim->InitializationGeneration != 1)
			{
				Test->AddError(TEXT("服务器原PS/Hero/ASC/来源/Spec/Revision不变量失败"));
				return true;
			}

			if (const auto* Effect = Abilities->GetActiveGameplayEffect(PersistentHandle))
			{
				if (Effect->StartServerWorldTime != PersistentStart)
				{
					Test->AddError(TEXT("服务器GE被重置"));
					return true;
				}
			}
			else if (World->GetTimeSeconds() + 0.1f < PersistentEnd)
			{
				Test->AddError(TEXT("服务器原持久GE提前丢失"));
				return true;
			}

			if (ParticipantState && !ParticipantState->bConnected)
			{
				const FString Value = FString::Printf(TEXT("%u"), ParticipantState->ConnectionEpoch);
				if (Read(TEXT("detached")) != Value)
				{
					Signal(TEXT("detached"), Value);
				}

				if (Victim->GetLifeSnapshot().Generation == 2 && Victim->IsAlive() && Read(TEXT("offline-respawned")).IsEmpty())
				{
					Signal(TEXT("offline-respawned"));
				}
			}

			return false;
		}

		FAutomationTestBase* Test;


	public:
		FString Read(const TCHAR* Name) const
		{
			FString Value;
			FFileHelper::LoadFileToString(Value, *FPaths::Combine(Evidence, Name));
			return Value;
		}


	public:
		double Started, LastAttack = 0;

		FString Role, Evidence, Address, DetachedBefore;

		int32 Stage = 0;

		FGuid Entity, Participant, Request;

		uint32 Epoch = 0, Revision = 0;

		float PersistentStart = 0, PersistentEnd = 0, CooldownStart = 0;

		bool bSecondAbilitySent = false;

		TWeakObjectPtr<AHeroCharacter> OriginalHero;

		TWeakObjectPtr<UHeroAbilitySystemComponent> OriginalASC;

		TWeakObjectPtr<AParticipantState> OriginalParticipant;

		FActiveGameplayEffectHandle PersistentHandle;

		FGameplayAbilitySpecHandle Slot;

		UPlaylistDefinition* Fixture = nullptr;

		TUniquePtr<FScopedGameConfigurationOverride> ConfigurationOverride;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResumeNetworkTest, "Moba.Network.Resume", EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FResumeNetworkTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FResumeNetworkScenario(this));
	return true;
}
#endif
