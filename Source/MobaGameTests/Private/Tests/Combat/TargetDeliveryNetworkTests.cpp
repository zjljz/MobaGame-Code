#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "InputKeyEventArgs.h"
#include "Player/HeroPlayerController.h"
#include "Hero/HeroCharacter.h"
#include "Match/ParticipantState.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/TargetDamageAbility.h"
#include "Hero/HeroDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceSubsystem.h"
#include "Combat/AbilityProjectile.h"
#include "MobaGameplayTags.h"

namespace
{
	class FTargetNetworkScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FTargetNetworkScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
		{
			FParse::Value(FCommandLine::Get(), TEXT("MobaTargetRole="), Role);
			FParse::Value(FCommandLine::Get(), TEXT("MobaTargetEvidence="), Evidence);
			FPaths::NormalizeDirectoryName(Evidence);
		}


	public:
		bool Update() override
		{
			if (Evidence.IsEmpty() || FPaths::IsRelative(Evidence) || !(Evidence.Contains(TEXT("/AI/task-009/")) || Evidence.Contains(TEXT("/AI/task-011/"))))
			{
				return Fail(TEXT("输出目录边界"));
			}

			if (Test->HasAnyErrors())
			{
				return Fail(TEXT("断言失败"));
			}

			for (const TCHAR* Peer : {TEXT("server-failed"), TEXT("owner-failed"), TEXT("observer-failed")})
			{
				if (Has(Peer))
				{
					return Fail(TEXT("对端失败"));
				}
			}

			if (FPlatformTime::Seconds() - Started > 180)
			{
				return Fail(TEXT("有界谓词超时"));
			}

			if (Role == TEXT("observer") && Has(TEXT("server-passed")))
			{
				Mark(TEXT("observer-passed"));
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
				return World->GetNetMode() == NM_DedicatedServer && Server(World);
			}

			if (World->GetNetMode() != NM_Client)
			{
				return false;
			}

			auto* Controller = Cast<AHeroPlayerController>(World->GetFirstPlayerController());
			auto* Local = Controller ? Cast<AHeroCharacter>(Controller->GetPawn()) : nullptr;
			if (!Local || !Local->EntityId.IsValid() || Controller->GetAdmissionResult() != TEXT("Bound"))
			{
				return false;
			}

			if (bKeyHeld)
			{
				Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::R, IE_Released, FPlatformTime::Cycles64()));
				bKeyHeld = false;
			}

			if (!Has(*(Role + TEXT("-identity"))))
			{
				if (!Controller->IsLocallyReady())
				{
					return false;
				}

				Mark(*(Role + TEXT("-identity")), Local->EntityId.ToString());
			}

			AHeroCharacter* Source = Find(World, Read(TEXT("owner-identity")));
			AHeroCharacter* Target = Find(World, Read(TEXT("observer-identity")));
			if (!Source || !Target)
			{
				return false;
			}

			return Role == TEXT("owner") ? Owner(Controller, Source, Target) : Observer(World, Source, Target);
		}


	private:
		FString Read(const TCHAR* Name) const
		{
			FString Value;
			FFileHelper::LoadFileToString(Value, *FPaths::Combine(Evidence, Name));
			return Value;
		}

		void Mark(const TCHAR* Name, const FString& Value = TEXT("ready"))
		{
			if (Test->HasAnyErrors() && !FString(Name).EndsWith(TEXT("failed")))
			{
				return;
			}

			Test->TestTrue(TEXT("公开场景标记保存"), FFileHelper::SaveStringToFile(Value, *FPaths::Combine(Evidence, Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		}

		bool Fail(const TCHAR* Reason)
		{
			const FString Message = FString::Printf(TEXT("TargetNetwork role=%s stage=%d reason=%s"), *Role, Stage, Reason);
			Test->AddError(Message);
			Mark(*(Role + TEXT("-failed")), Message);
			return true;
		}

		AHeroCharacter* Find(UWorld* World, const FString& Identity)
		{
			FGuid Id;
			if (!FGuid::Parse(Identity, Id))
			{
				return nullptr;
			}

			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				if (It->EntityId == Id)
				{
					return *It;
				}
			}

			return nullptr;
		}

		bool HasProjectile(UWorld* World, FGuid Source)
		{
			for (TActorIterator<AAbilityProjectile> It(World); It; ++It)
			{
				if (It->SourceEntity == Source)
				{
					return true;
				}
			}

			return false;
		}

		void Capture(const TCHAR* Name)
		{
			FString Directory;
			FParse::Value(FCommandLine::Get(), TEXT("MobaScreenshotDirectory="), Directory);
			FPaths::NormalizeDirectoryName(Directory);
			if (!Directory.IsEmpty() && (Directory.Contains(TEXT("/AI/task-009/")) || Directory.Contains(TEXT("/AI/task-011/"))))
			{
				FScreenshotRequest::RequestScreenshot(FPaths::Combine(Directory, FString(Name) + TEXT(".png")), false, false);
			}
		}

		bool Aim(AHeroPlayerController* Controller, AHeroCharacter* Source, AHeroCharacter* Target, const TCHAR* Shot)
		{
			const auto* Spec = Source->GetHeroAbilitySystem()->FindAbilitySpecFromHandle(Source->GetHeroAbilitySystem()->FindSlot(MobaGameplayTags::Input_Slot_Target));
			const auto* Ability = Spec ? Cast<UTargetDamageAbility>(Spec->Ability) : nullptr;
			if (!Ability || !Controller->IsLocallyReady())
			{
				return false;
			}

			FVector2D Screen;
			if (!Controller->ProjectWorldLocationToScreen(Target->GetActorLocation(), Screen))
			{
				return false;
			}

			Controller->SetMouseLocation(FMath::RoundToInt(Screen.X), FMath::RoundToInt(Screen.Y));
			FAbilityTargetIntent Intent;
			if (!Controller->BuildCursorIntent(Ability->GetGeometry(), Intent))
			{
				return false;
			}

			Test->TestEqual(TEXT("客户端指示射程与数据"), Ability->GetGeometry().Range, 900.f);
			Test->TestEqual(TEXT("客户端指示高度与数据"), Ability->GetGeometry().HeightTolerance, 200.f);
			Test->TestTrue(TEXT("鼠标点位与目标几何相符"), FVector::DistXY(Intent.Point, Target->GetActorLocation()) < 10);
			Capture(Shot);
			return true;
		}

		void Press(AHeroPlayerController* Controller)
		{
			Ack = Controller->GetIntentResultSequence();
			Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::R, IE_Pressed, FPlatformTime::Cycles64()));
			bKeyHeld = true;
		}

		void SetHealth(AHeroCharacter* Hero, float Value)
		{
			Hero->GetHeroAbilitySystem()->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), Value);
			Hero->ForceNetUpdate();
		}

		void Lethal(AHeroCharacter* Hero)
		{
			UGameplayEffect* Effect = NewObject<UGameplayEffect>(Hero);
			Effect->DurationPolicy = EGameplayEffectDurationType::Instant;
			auto& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
			Modifier.Attribute = UHeroAttributeSet::GetIncomingDamageAttribute();
			Modifier.ModifierOp = EGameplayModOp::Additive;
			Modifier.ModifierMagnitude = FScalableFloat(1000);
			FGameplayEffectSpec Spec(Effect, Hero->GetHeroAbilitySystem()->MakeEffectContext(), 1);
			Hero->GetHeroAbilitySystem()->ApplyGameplayEffectSpecToSelf(Spec);
		}

		bool Server(UWorld* World)
		{
			auto* Source = Find(World, Read(TEXT("owner-identity")));
			auto* Target = Find(World, Read(TEXT("observer-identity")));
			if (!Source || !Target)
			{
				return false;
			}

			auto* ASC = Source->GetHeroAbilitySystem();
			const auto* Experience = World->GetSubsystem<UExperienceSubsystem>()->GetExperience();
			if (!Experience)
			{
				return false;
			}

			switch (Stage)
			{
			case 0:
				if (!Source->bInputReady || !Target->bInputReady)
				{
					return false;
				}

				Test->TestEqual(TEXT("相同比赛"), Source->MatchId, Target->MatchId);
				Test->TestEqual(TEXT("Variant实际递归加载"), Experience->HeroVariants.Num(), 1);
				Test->TestTrue(TEXT("Variant已加载"), Experience->HeroVariants[0].IsValid() && Experience->HeroVariants[0]->AbilitySet.IsValid());
				Mark(TEXT("setup"));
				Stage = 1;
				return false;
			case 1:
				if (!Has(TEXT("probes")))
				{
					return false;
				}

				Test->TestEqual(TEXT("服务端拒绝后没有伤害"), Target->GetAttributes()->GetHealth(), 100.f);
				Test->TestEqual(TEXT("服务端拒绝后无成本"), Source->GetAttributes()->GetMana(), 100.f);
				Mark(TEXT("unit-go"));
				Stage = 2;
				return false;
			case 2:
				if (!Has(TEXT("unit-owner")) || !Has(TEXT("unit-observer")))
				{
					return false;
				}

				Test->TestEqual(TEXT("Instant真实权威伤害"), Target->GetAttributes()->GetHealth(), 40.f);
				Test->TestEqual(TEXT("Instant真实成本"), Source->GetAttributes()->GetMana(), 80.f);
				SetHealth(Target, 100);
				Test->TestTrue(TEXT("真实资产替换Point变体"),
					ASC->ReplaceManagedSource(Source->EntityId, Experience->HeroVariants[0]->AbilitySet.Get(), ASC->GetStateRevision()).Status == EAbilitySourceResult::Applied);
				Mark(TEXT("point-go"), FString::FromInt(Source->RequiredLoadoutRevision));
				Stage = 3;
				return false;
			case 3:
				if (!Has(TEXT("point-owner")) || !Has(TEXT("point-observer")))
				{
					return false;
				}

				Test->TestEqual(TEXT("Point权威重新搜集命中"), Target->GetAttributes()->GetHealth(), 40.f);
				Test->TestEqual(TEXT("Point成本"), Source->GetAttributes()->GetMana(), 60.f);
				SetHealth(Target, 60);
				Test->TestTrue(TEXT("恢复Direction资产"),
					ASC->ReplaceManagedSource(Source->EntityId, Source->GetDefinition()->AbilitySet.Get(), ASC->GetStateRevision()).Status == EAbilitySourceResult::Applied);
				Mark(TEXT("death-go"), FString::FromInt(Source->RequiredLoadoutRevision));
				Stage = 4;
				return false;
			case 4:
				if (!HasProjectile(World, Source->EntityId))
				{
					return false;
				}

				Test->TestEqual(TEXT("Projectile释放扣费"), Source->GetAttributes()->GetMana(), 40.f);
				Lethal(Source);
				Test->TestFalse(TEXT("源在命中前死亡"), Source->IsAlive());
				Mark(TEXT("source-dead"));
				Stage = 5;
				return false;
			case 5:
				if (Target->IsAlive() || !Has(TEXT("death-owner")) || !Has(TEXT("death-observer")))
				{
					return false;
				}

				Test->TestEqual(TEXT("死亡源Projectile稳定Participant归属"), Target->GetLifeSnapshot().KillerParticipant, Source->ParticipantId);
				Test->TestEqual(TEXT("死亡源Projectile仅一次"), Target->GetLifeSnapshot().DeathCount, uint32(1));
				Stage = 6;
				return false;
			case 6:
				if (!Source->IsAlive() || !Target->IsAlive())
				{
					return false;
				}

				SetHealth(Target, 60);
				Mark(TEXT("offline-go"));
				Stage = 7;
				return false;
			case 7:
				if (!HasProjectile(World, Source->EntityId))
				{
					return false;
				}

				Mark(TEXT("offline-launched"));
				Stage = 8;
				return false;
			case 8:
			{
				auto* Participant = Cast<AParticipantState>(Source->GetPlayerState());
				// UnPossess后Actor上的PlayerState可能为空；按稳定Participant在服务器查找。
				if (!Participant)
				{
					for (TActorIterator<AParticipantState> It(World); It; ++It)
					{
						if (It->ParticipantId == Source->ParticipantId)
						{
							Participant = *It;
						}
					}
				}

				if (!Participant || Participant->bConnected)
				{
					return false;
				}

				Test->TestTrue(TEXT("客户端退出时投送仍在飞行"), HasProjectile(World, Source->EntityId));
				Mark(TEXT("source-offline"));
				Stage = 9;
				return false;
			}
			case 9:
				if (Target->IsAlive() || !Has(TEXT("offline-observer")))
				{
					return false;
				}

				Test->TestEqual(TEXT("断线后归属仍是原Participant"), Target->GetLifeSnapshot().KillerParticipant, Source->ParticipantId);
				Test->TestEqual(TEXT("两次投射物只有两次死亡"), Target->GetLifeSnapshot().DeathCount, uint32(2));
				Test->TestFalse(TEXT("已命中无投射物残留"), HasProjectile(World, Source->EntityId));
				Mark(TEXT("server-passed"));
				return true;
			}

			return false;
		}

		bool Owner(AHeroPlayerController* Controller, AHeroCharacter* Source, AHeroCharacter* Target)
		{
			auto* ASC = Source->GetHeroAbilitySystem();
			if (Stage == 0)
			{
				if (!Has(TEXT("setup")))
				{
					return false;
				}

				Stage = 1;
				return false;
			}

			if (Stage >= 1 && Stage <= 6)
			{
				if (!bProbePending)
				{
					FAbilityTargetIntent Intent;
					Intent.Policy = EAbilityTargetPolicy::Direction;
					Intent.Direction = FVector::ForwardVector;
					FGuid Entity = Source->EntityId;
					uint32 Epoch = Source->ConnectionEpoch;
					uint32 Revision = Source->RequiredLoadoutRevision;
					if (Stage == 1)
					{
						Entity = Target->EntityId;
					}

					if (Stage == 2)
					{
						--Epoch;
					}

					if (Stage == 3)
					{
						--Revision;
					}

					if (Stage == 4)
					{
						Intent.Policy = EAbilityTargetPolicy::Unit;
					}

					if (Stage == 5)
					{
						Intent.Direction = FVector::ZeroVector;
					}

					if (Stage == 6)
					{
						Intent.Policy = EAbilityTargetPolicy::Point;
						Intent.Point = FVector(100000, 0, 0);
					}

					Ack = Controller->GetIntentResultSequence();
					Controller->ServerActivateTargetSlot(Entity, Epoch, Revision, Stage == 6 ? FGameplayTag() : MobaGameplayTags::Input_Slot_Target, Intent);
					bProbePending = true;
					return false;
				}

				if (Controller->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestFalse(TEXT("真实RPC拒绝篡改载荷/代次"), Controller->WasLastIntentAccepted());
				Test->TestEqual(TEXT("拒绝不伤害"), Target->GetAttributes()->GetHealth(), 100.f);
				bProbePending = false;
				++Stage;
				if (Stage == 7)
				{
					Mark(TEXT("probes"));
				}

				return false;
			}

			switch (Stage)
			{
			case 7:
				if (!Has(TEXT("unit-go")))
				{
					return false;
				}

				Ack = Controller->GetIntentResultSequence();
				Controller->ServerActivateUnitSlot(Source->EntityId, Source->ConnectionEpoch, Source->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_UnitAttack, Target);
				Stage = 8;
				return false;
			case 8:
				if (Controller->GetIntentResultSequence() == Ack || Target->GetAttributes()->GetHealth() != 40)
				{
					return false;
				}

				Test->TestTrue(TEXT("Unit真实RPC成功"), Controller->WasLastIntentAccepted());
				Mark(TEXT("unit-owner"));
				Stage = 9;
				return false;
			case 9:
				if (!Has(TEXT("point-go")) || Source->RequiredLoadoutRevision != FCString::Atoi(*Read(TEXT("point-go"))) || !Controller->IsLocallyReady()
					|| ASC->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack) || Target->GetAttributes()->GetHealth() != 100)
				{
					return false;
				}

				if (!bPointProbeDone)
				{
					if (!bProbePending)
					{
						FAbilityTargetIntent Intent;
						Intent.Policy = EAbilityTargetPolicy::Point;
						Intent.Point = Source->GetActorLocation() + FVector(100000, 0, 0);
						Intent.Unit = Target;
						Ack = Controller->GetIntentResultSequence();
						Controller->ServerActivateTargetSlot(Source->EntityId, Source->ConnectionEpoch, Source->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_Target, Intent);
						bProbePending = true;
						return false;
					}

					if (Controller->GetIntentResultSequence() == Ack)
					{
						return false;
					}

					Test->TestFalse(TEXT("真实Point越距载荷拒绝"), Controller->WasLastIntentAccepted());
					Test->TestEqual(TEXT("越距点未被静默钳制"), Controller->GetLastIntentReason(), FName(TEXT("PointRange")));
					Test->TestEqual(TEXT("非法Point不扣成本"), Source->GetAttributes()->GetMana(), 80.f);
					bPointProbeDone = true;
					bProbePending = false;
				}

				if (!Aim(Controller, Source, Target, TEXT("point-indicator")))
				{
					return false;
				}

				Stage = 10;
				return false;
			case 10:
				Press(Controller);
				Stage = 11;
				return false;
			case 11:
				if (Controller->GetIntentResultSequence() == Ack || Target->GetAttributes()->GetHealth() != 40)
				{
					return false;
				}

				Test->TestTrue(TEXT("原生R与鼠标Point施放成功"), Controller->WasLastIntentAccepted());
				Mark(TEXT("point-owner"));
				Stage = 12;
				return false;
			case 12:
				if (!Has(TEXT("death-go")) || Source->RequiredLoadoutRevision != FCString::Atoi(*Read(TEXT("death-go"))) || !Controller->IsLocallyReady()
					|| ASC->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack) || Target->GetAttributes()->GetHealth() != 60)
				{
					return false;
				}

				if (!Aim(Controller, Source, Target, TEXT("direction-indicator")))
				{
					return false;
				}

				Stage = 13;
				return false;
			case 13:
				Press(Controller);
				Stage = 14;
				return false;
			case 14:
				if (Controller->GetIntentResultSequence() == Ack || Source->IsAlive() || Target->IsAlive())
				{
					return false;
				}

				Test->TestTrue(TEXT("原生Direction释放成功后源死亡"), Controller->WasLastIntentAccepted());
				Test->TestEqual(TEXT("Owner收到命中与归属"), Target->GetLifeSnapshot().KillerParticipant, Source->ParticipantId);
				Mark(TEXT("death-owner"));
				Stage = 15;
				return false;
			case 15:
				if (!Has(TEXT("offline-go")) || !Controller->IsLocallyReady() || Target->GetAttributes()->GetHealth() != 60)
				{
					return false;
				}

				if (!Aim(Controller, Source, Target, TEXT("offline-indicator")))
				{
					return false;
				}

				Stage = 16;
				return false;
			case 16:
				Press(Controller);
				Stage = 17;
				return false;
			case 17:
				if (Controller->GetIntentResultSequence() == Ack || !Has(TEXT("offline-launched")))
				{
					return false;
				}

				Test->TestTrue(TEXT("退出前投送已提交"), Controller->WasLastIntentAccepted());
				Mark(TEXT("owner-passed"));
				Controller->ConsoleCommand(TEXT("disconnect"));
				return true;
			}

			return false;
		}

		bool Observer(UWorld* World, AHeroCharacter* Source, AHeroCharacter* Target)
		{
			if (HasProjectile(World, Source->EntityId) && !bSawProjectile)
			{
				bSawProjectile = true;
				Capture(TEXT("projectile"));
				Mark(TEXT("projectile-visible"));
			}

			if (Stage == 0 && Has(TEXT("unit-go")) && Target->GetAttributes()->GetHealth() == 40)
			{
				Mark(TEXT("unit-observer"));
				Stage = 1;
			}

			if (Stage == 1 && Has(TEXT("point-go")) && Target->GetAttributes()->GetHealth() == 100)
			{
				Stage = 2;
			}

			if (Stage == 2 && Target->GetAttributes()->GetHealth() == 40)
			{
				Mark(TEXT("point-observer"));
				Stage = 3;
			}

			if (Stage == 3 && Has(TEXT("source-dead")) && !Target->IsAlive() && !Source->IsAlive())
			{
				Test->TestTrue(TEXT("Observer实际看见复制Projectile"), bSawProjectile);
				Test->TestEqual(TEXT("Observer死亡来源归属"), Target->GetLifeSnapshot().KillerParticipant, Source->ParticipantId);
				Mark(TEXT("death-observer"));
				Stage = 4;
			}

			if (Stage == 4 && Has(TEXT("offline-go")) && Target->IsAlive())
			{
				Stage = 5;
			}

			if (Stage == 5 && Has(TEXT("source-offline")) && !Target->IsAlive() && Target->GetLifeSnapshot().DeathCount == 2)
			{
				Test->TestFalse(TEXT("Observer看到源断线输入关闭"), Source->bInputReady);
				Test->TestEqual(TEXT("Observer断线来源归属"), Target->GetLifeSnapshot().KillerParticipant, Source->ParticipantId);
				Mark(TEXT("offline-observer"));
				Stage = 6;
			}

			return false;
		}

		FAutomationTestBase* Test;


	private:
		bool Has(const TCHAR* Name) const { return IFileManager::Get().FileExists(*FPaths::Combine(Evidence, Name)); }


	private:
		FString Role, Evidence;

		double Started;

		int32 Stage = 0;

		uint32 Ack = 0;

		bool bKeyHeld = false;

		bool bProbePending = false;

		bool bSawProjectile = false;

		bool bPointProbeDone = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTargetNetworkTest, "Moba.Network.TargetDelivery", EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FTargetNetworkTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FTargetNetworkScenario>(this));
	return true;
}
#endif
