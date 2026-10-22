#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/TargetDamageAbility.h"
#include "Combat/AbilityProjectile.h"
#include "Components/BoxComponent.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Experience/ExperienceSubsystem.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MobaGameplayTags.h"
#include "Match/MatchGameState.h"
#include "Tests/Combat/TargetCombatant.h"
#include "UnrealClient.h"

namespace
{
// 两种 profile × 两个保存 GA × 八种场景；精确几何边界另由内容 World 测试覆盖。
constexpr int32 CasesPerAbility = 8;
constexpr int32 CaseCount = 32;
const FVector SourcePosition(0, 0, 1000);

class FLineConeInputNetworkScenario final : public IAutomationLatentCommand
{
public:
	explicit FLineConeInputNetworkScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
	{
		FParse::Value(FCommandLine::Get(), TEXT("MobaLineConeRole="), Role);
		FParse::Value(FCommandLine::Get(), TEXT("MobaLineConeEvidence="), Evidence);
		FPaths::NormalizeDirectoryName(Evidence);
		const bool bHadTraversal = Evidence.Contains(TEXT("/../")) || Evidence.EndsWith(TEXT("/.."));
		bSafeEvidence = !Evidence.IsEmpty() && !FPaths::IsRelative(Evidence) && !bHadTraversal
			&& Evidence.Contains(TEXT("/AI/TASK-013/Network/"), ESearchCase::IgnoreCase)
			&& IFileManager::Get().DirectoryExists(*Evidence);
	}

	~FLineConeInputNetworkScenario() override
	{
		ReleaseKey();
		CleanupFixture();
	}

	bool Update() override
	{
		if (!bSafeEvidence || (Role != TEXT("server") && Role != TEXT("owner") && Role != TEXT("observer")))
		{
			Test->AddError(TEXT("LineCone 需要正确角色和本任务已创建的全新 Network 输出目录"));
			return true;
		}
		ReleaseKey();
		if (Test->HasAnyErrors())
		{
			return Fail(TEXT("本地断言失败"));
		}
		if (FPlatformTime::Seconds() - Started > 300)
		{
			return Fail(TEXT("300秒状态谓词超时"));
		}
		for (const TCHAR* Peer : {TEXT("server"), TEXT("owner"), TEXT("observer")})
		{
			if (!Read(FString(Peer) + TEXT("-failed")).IsEmpty())
			{
				return Fail(TEXT("对端失败"));
			}
		}
		UWorld* World = nullptr;
		for (const auto& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() && Context.World()->IsGameWorld()
				&& Context.World()->GetNetMode() == (Role == TEXT("server") ? NM_DedicatedServer : NM_Client))
			{
				World = Context.World();
				break;
			}
		}
		if (!World)
		{
			return false;
		}
		auto* Match = World->GetGameState<AMatchGameState>();
		auto* ExperienceSubsystem = World->GetSubsystem<UExperienceSubsystem>();
		if (!Match || ExperienceSubsystem->GetSnapshot().State != EExperienceState::ExperienceReady)
		{
			return false;
		}
		if (!bContentChecked)
		{
			if (!CheckContent(World, Match, ExperienceSubsystem->GetExperience()))
			{
				return Fail(TEXT("保存内容或启动入口不符"));
			}
			bContentChecked = true;
			Mark(Role + TEXT("-instance"), Match->ExperienceInstance.ToString());
		}
		AHeroPlayerController* Controller = nullptr;
		if (Role != TEXT("server"))
		{
			Controller = Cast<AHeroPlayerController>(World->GetFirstPlayerController());
			auto* Local = Controller ? Cast<AHeroCharacter>(Controller->GetPawn()) : nullptr;
			if (!Local || !Controller->IsLocallyReady() || Controller->GetAdmissionResult() != TEXT("Bound"))
			{
				return false;
			}
			LocalController = Controller;
			if (Read(Role + TEXT("-identity")).IsEmpty())
			{
				Mark(Role + TEXT("-identity"), Local->EntityId.ToString());
			}
		}
		auto* Source = FindHero(World, Read(TEXT("owner-identity")));
		auto* Target = FindHero(World, Read(TEXT("observer-identity")));
		if (!Source || !Target)
		{
			return false;
		}
		if (CaseIndex == CaseCount)
		{
			return Finish();
		}
		return Role == TEXT("server") ? Server(World, Match, Source, Target) : Client(World, Match, Controller, Source, Target);
	}


private:
	// 仅公开身份和场景谓词；测试不读取、构造或传递重连凭证。
	FString Read(const FString& Name) const
	{
		FString Value;
		FFileHelper::LoadFileToString(Value, *FPaths::Combine(Evidence, Name));
		return Value;
	}

	bool Mark(const FString& Name, const FString& Value = TEXT("ready"))
	{
		if (!bSafeEvidence || (Test->HasAnyErrors() && !Name.EndsWith(TEXT("failed"))))
		{
			return false;
		}
		return Test->TestTrue(TEXT("保存公开场景标记"), FFileHelper::SaveStringToFile(Value, *FPaths::Combine(Evidence, Name),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	bool Has(const FString& Name) const
	{
		return Read(Name) == TEXT("ready");
	}

	FString Step(const TCHAR* Suffix) const
	{
		return FString::Printf(TEXT("case-%02d-%s"), CaseIndex, Suffix);
	}

	bool Fail(const TCHAR* Reason)
	{
		const FString Message = FString::Printf(TEXT("LineCone Role=%s Case=%d Phase=%d Reason=%s"), *Role, CaseIndex, Phase, Reason);
		Test->AddError(Message);
		Mark(Role + TEXT("-failed"), Message);
		ReleaseKey();
		CleanupFixture();
		return true;
	}

	void Advance()
	{
		++CaseIndex;
		Phase = 0;
		bProfileRequested = false;
		bSawCooldown = false;
	}

	bool Finish()
	{
		if (Role == TEXT("server"))
		{
			CleanupFixture();
			Mark(TEXT("server-passed"));
			// 客户端确认收到最终完成标记后，服务器才能随 TestExit 正常退出。
			return Has(TEXT("owner-passed")) && Has(TEXT("observer-passed"));
		}
		if (!Has(TEXT("server-passed")))
		{
			return false;
		}
		Mark(Role + TEXT("-passed"));
		return true;
	}


	AHeroCharacter* FindHero(UWorld* World, const FString& Identity) const
	{
		FGuid Id;
		if (FGuid::Parse(Identity, Id))
		{
			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				if (It->EntityId == Id)
				{
					return *It;
				}
			}
		}
		return nullptr;
	}

	ATargetCombatant* FindWitness(UWorld* World) const
	{
		FGuid Id;
		if (FGuid::Parse(Read(Step(TEXT("witness"))), Id))
		{
			for (TActorIterator<ATargetCombatant> It(World); It; ++It)
			{
				if (It->Entity == Id)
				{
					return *It;
				}
			}
		}
		return nullptr;
	}

	bool CheckContent(UWorld* World, const AMatchGameState* Match, const UExperienceDefinition* Experience)
	{
		if (!Experience || !Experience->Hero.IsValid() || !Experience->Hero->AbilitySet.IsValid())
		{
			return false;
		}
		Test->TestEqual(TEXT("真实新 Playlist 启动"), Match->PlaylistId, FPrimaryAssetId(TEXT("Playlist"), TEXT("Playlist_LineCone")));
		Test->TestEqual(TEXT("保存 Experience"), Experience->GetPathName(), FString(TEXT("/MobaClassic/Definitions/LineCone/Experience_LineCone.Experience_LineCone")));
		Test->TestEqual(TEXT("保存 Hero"), Experience->Hero->GetPathName(), FString(TEXT("/MobaClassic/Definitions/LineCone/Hero_LineCone.Hero_LineCone")));
		Test->TestEqual(TEXT("保存 Set"), Experience->Hero->AbilitySet->GetPathName(), FString(TEXT("/MobaClassic/Definitions/LineCone/AbilitySet_LineCone.AbilitySet_LineCone")));
		Test->TestTrue(TEXT("动态地图实际存在"), World->GetSubsystem<UExperienceSubsystem>()->GetMapInstance() != nullptr);
		Test->TestTrue(TEXT("Experience Instance 有效"), Match->ExperienceInstance.IsValid());
		if (Role != TEXT("server"))
		{
			Test->TestFalse(TEXT("正常输入与渲染不用 NullRHI"), FParse::Param(FCommandLine::Get(), TEXT("NullRHI")));
			Test->TestTrue(TEXT("客户端真实视口"), World->GetGameViewport() && World->GetGameViewport()->Viewport);
		}
		return !Test->HasAnyErrors();
	}

	int32 EnemyTeam(const AMatchGameState* Match, int32 SourceTeam) const
	{
		for (int32 Team : Match->Teams.Teams)
		{
			if (Match->Teams.Query(SourceTeam, Team) == ETeamRelation::Enemy)
			{
				return Team;
			}
		}
		return INDEX_NONE;
	}

	FVector TargetOffset() const
	{
		switch (CaseIndex % CasesPerAbility)
		{
		case 1: return FVector(950, 0, 0);
		case 2: return IsCone() ? FVector(300, 300, 0) : FVector(300, 110, 0);
		case 3: return FVector(-150, 0, 0);
		case 4: return FVector(300, 0, 260);
		case 7: return FVector(450, 0, 0);
		default: return FVector(300, 0, 0);
		}
	}

	int32 TargetTeam(const AMatchGameState* Match, int32 SourceTeam) const
	{
		const int32 Variant = CaseIndex % CasesPerAbility;
		return Variant == 5 ? SourceTeam : (Variant == 6 ? int32(INDEX_NONE) : EnemyTeam(Match, SourceTeam));
	}

	// 仅调整本场临时服务器状态，退出时还原位置/队伍/移动并销毁本测试瞬态 Actor。
	bool Prepare(UWorld* World, const AMatchGameState* Match, AHeroCharacter* Source, AHeroCharacter* Target)
	{
		if (!bFixtureStarted)
		{
			ServerSource = Source;
			ServerTarget = Target;
			OriginalSourceTransform = Source->GetActorTransform();
			OriginalTargetTransform = Target->GetActorTransform();
			OriginalSourceMovement = Source->GetCharacterMovement()->MovementMode;
			OriginalTargetMovement = Target->GetCharacterMovement()->MovementMode;
			OriginalTargetTeam = Target->TeamId;
			bFixtureStarted = true;
		}
		DestroyCaseActors();
		Source->GetCharacterMovement()->DisableMovement();
		Target->GetCharacterMovement()->DisableMovement();
		Source->SetActorLocation(SourcePosition, false, nullptr, ETeleportType::TeleportPhysics);
		Target->SetActorLocation(SourcePosition + TargetOffset(), false, nullptr, ETeleportType::TeleportPhysics);
		Target->TeamId = TargetTeam(Match, Source->TeamId);
		Source->GetHeroAbilitySystem()->SetNumericAttributeBase(UHeroAttributeSet::GetManaAttribute(), 100);
		Target->GetHeroAbilitySystem()->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 100);
		Source->ForceNetUpdate();
		Target->ForceNetUpdate();
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		auto* Control = World->SpawnActor<ATargetCombatant>(ATargetCombatant::StaticClass(), SourcePosition + FVector(150, 0, 0), FRotator::ZeroRotator, Spawn);
		if (!Test->TestNotNull(TEXT("界内敌方正对照"), Control))
		{
			return false;
		}
		Witness = Control;
		Control->Entity = FGuid::NewGuid();
		Control->Participant = FGuid::NewGuid();
		Control->Match = Match->MatchId;
		Control->Team = EnemyTeam(Match, Source->TeamId);
		Control->SetReplicateMovement(true);
		Control->ForceNetUpdate();
		if (CaseIndex % CasesPerAbility == 7)
		{
			auto* Actor = World->SpawnActor<AActor>();
			Wall = Actor;
			auto* Box = NewObject<UBoxComponent>(Actor);
			Actor->SetRootComponent(Box);
			Actor->AddInstanceComponent(Box);
			Box->SetBoxExtent(FVector(12, 100, 100));
			Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Box->SetCollisionObjectType(ECC_WorldStatic);
			Box->SetCollisionResponseToAllChannels(ECR_Block);
			Box->RegisterComponent();
			Actor->SetActorLocation(SourcePosition + FVector(300, 0, 0));
			FCollisionQueryParams Query(SCENE_QUERY_STAT(LineConeTestWall), false, Source);
			Query.AddIgnoredActor(Target);
			Query.AddIgnoredActor(Control);
			FHitResult Hit;
			World->LineTraceSingleByChannel(Hit, SourcePosition, Target->GetActorLocation(), ECC_Visibility, Query);
			Test->TestTrue(TEXT("权威 Visibility 命中本测试墙"), Hit.GetActor() == Actor);
		}
		return Mark(Step(TEXT("witness")), Control->Entity.ToString()) && Mark(Step(TEXT("prepared")));
	}

	void DestroyCaseActors()
	{
		if (Witness.IsValid())
		{
			Witness->Destroy();
		}
		if (Wall.IsValid())
		{
			Wall->Destroy();
		}
		Witness.Reset();
		Wall.Reset();
	}

	void CleanupFixture()
	{
		DestroyCaseActors();
		if (!bFixtureStarted)
		{
			return;
		}
		if (ServerSource.IsValid())
		{
			ServerSource->SetActorTransform(OriginalSourceTransform, false, nullptr, ETeleportType::TeleportPhysics);
			ServerSource->GetCharacterMovement()->SetMovementMode(OriginalSourceMovement);
		}
		if (ServerTarget.IsValid())
		{
			ServerTarget->TeamId = OriginalTargetTeam;
			ServerTarget->SetActorTransform(OriginalTargetTransform, false, nullptr, ETeleportType::TeleportPhysics);
			ServerTarget->GetCharacterMovement()->SetMovementMode(OriginalTargetMovement);
			ServerTarget->ForceNetUpdate();
		}
		bFixtureStarted = false;
	}

	bool Server(UWorld* World, const AMatchGameState* Match, AHeroCharacter* Source, AHeroCharacter* Target)
	{
		auto* Abilities = Source->GetHeroAbilitySystem();
		if (Phase >= 2 && Source->GetAttributes()->GetMana() == 80)
		{
			bSawCooldown |= Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack);
		}
		if (Phase == 0)
		{
			if (!Source->bInputReady || !Target->bInputReady || Abilities->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack)
				|| Read(TEXT("owner-instance")).IsEmpty() || Read(TEXT("observer-instance")).IsEmpty())
			{
				return false;
			}
			Test->TestEqual(TEXT("Owner 同一装载身份"), Read(TEXT("owner-instance")), Match->ExperienceInstance.ToString());
			Test->TestEqual(TEXT("Observer 同一装载身份"), Read(TEXT("observer-instance")), Match->ExperienceInstance.ToString());
			if (EnemyTeam(Match, Source->TeamId) == INDEX_NONE)
			{
				return Fail(TEXT("既有队伍规则无敌方"));
			}
			if (!Prepare(World, Match, Source, Target))
			{
				return Fail(TEXT("瞬态场景准备失败"));
			}
			Phase = 1;
		}
		else if (Phase == 1 && Has(Step(TEXT("owner-ready"))) && Has(Step(TEXT("observer-ready"))))
		{
			Mark(Step(TEXT("fire")));
			Phase = 2;
		}
		else if (Phase == 2 && Has(Step(TEXT("input-accepted"))) && Source->GetAttributes()->GetMana() == 80)
		{
			Test->TestEqual(TEXT("服务器候选目标真实伤害/过滤"), Target->GetAttributes()->GetHealth(), ExpectedHealth());
			Test->TestTrue(TEXT("每次施放都有界内对照真实伤害"), Witness.IsValid() && Witness->GetHealth() == 40);
			Test->TestTrue(TEXT("观察到原生冷却"), bSawCooldown);
			const FGameplayTag Slot = IsCone() ? MobaGameplayTags::Input_Slot_Target : MobaGameplayTags::Input_Slot_Primary;
			const auto* Spec = Abilities->FindAbilitySpecFromHandle(Abilities->FindSlot(Slot));
			const auto* Instance = Spec ? Cast<UTargetDamageAbility>(Spec->GetPrimaryInstance()) : nullptr;
			Test->TestTrue(TEXT("真实 GA 实例已提交投送"), Instance && Instance->WasDeliveryCommitted() && Instance->Delivery == EAbilityDelivery::Instant);
			for (TActorIterator<AAbilityProjectile> It(World); It; ++It)
			{
				Test->TestTrue(TEXT("Instant 没有来源投射物"), It->SourceEntity != Source->EntityId);
			}
			Mark(Step(TEXT("server-result")));
			Phase = 3;
		}
		else if (Phase == 3 && Has(Step(TEXT("owner-observed"))) && Has(Step(TEXT("observer-observed"))))
		{
			Mark(Step(TEXT("complete")));
			Advance();
		}
		return false;
	}


	void Press(AHeroPlayerController* Controller, const FKey& Key)
	{
		HeldKey = Key;
		LocalController = Controller;
		Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, Key, IE_Pressed, FPlatformTime::Cycles64()));
	}

	void ReleaseKey()
	{
		if (HeldKey.IsValid() && LocalController.IsValid())
		{
			LocalController->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, HeldKey, IE_Released, FPlatformTime::Cycles64()));
		}
		HeldKey = FKey();
	}

	bool Aim(AHeroPlayerController* Controller, AHeroCharacter* Source)
	{
		const FGameplayTag Slot = IsCone() ? MobaGameplayTags::Input_Slot_Target : MobaGameplayTags::Input_Slot_Primary;
		auto* Abilities = Source->GetHeroAbilitySystem();
		const auto* Spec = Abilities->FindAbilitySpecFromHandle(Abilities->FindSlot(Slot));
		const auto* Ability = Spec ? Cast<UTargetDamageAbility>(Spec->Ability) : nullptr;
		if (!Ability)
		{
			return false;
		}
		const FString ExpectedClass = IsCone() ? TEXT("/MobaClassic/Tests/LineCone/Abilities/GA_ConeInstant.GA_ConeInstant_C")
			: TEXT("/MobaClassic/Tests/LineCone/Abilities/GA_LineInstant.GA_LineInstant_C");
		Test->TestEqual(TEXT("正常槽来自保存 GA"), Ability->GetClass()->GetPathName(), ExpectedClass);
		Test->TestTrue(TEXT("保存策略与 Instant"), Ability->Delivery == EAbilityDelivery::Instant
			&& Ability->GetGeometry().Policy == (IsCone() ? EAbilityTargetPolicy::Cone : EAbilityTargetPolicy::Line));
		FVector2D Screen;
		if (!Controller->ProjectWorldLocationToScreen(SourcePosition + FVector(400, 0, 0), Screen))
		{
			return false;
		}
		Controller->SetMouseLocation(FMath::RoundToInt(Screen.X), FMath::RoundToInt(Screen.Y));
		return true;
	}

	bool Client(UWorld* World, const AMatchGameState* Match, AHeroPlayerController* Controller, AHeroCharacter* Source, AHeroCharacter* Target)
	{
		// complete 已由三端结果确认；下一场可销毁旧对照，推进不再依赖旧 Actor 存活。
		if (Phase == 5 && Has(Step(TEXT("complete"))))
		{
			Advance();
			return false;
		}
		if (Phase >= 1 && Source->GetAttributes()->GetMana() == 80)
		{
			bSawCooldown |= Source->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack);
		}
		auto* Control = FindWitness(World);
		if (!Control || !Has(Step(TEXT("prepared"))))
		{
			return false;
		}
		if (Phase == 0)
		{
			if (Source->GetAttributes()->GetMana() != 100 || Target->GetAttributes()->GetHealth() != 100 || Control->GetHealth() != 100
				|| Target->TeamId != TargetTeam(Match, Source->TeamId) || Control->Team != EnemyTeam(Match, Source->TeamId)
				|| FVector::Dist(Source->GetActorLocation(), SourcePosition) > 3 || FVector::Dist(Target->GetActorLocation(), SourcePosition + TargetOffset()) > 3)
			{
				return false;
			}
			if (Role == TEXT("owner") && Controller->GetActiveProfile() != ExpectedProfile())
			{
				if (!bProfileRequested)
				{
					Press(Controller, EKeys::Tab);
					bProfileRequested = true;
				}
				return false;
			}
			Mark(Step(Role == TEXT("owner") ? TEXT("owner-ready") : TEXT("observer-ready")));
			Phase = 1;
		}
		if (Role == TEXT("owner") && Phase == 1 && Has(Step(TEXT("fire"))))
		{
			if (!Aim(Controller, Source))
			{
				return false;
			}
			Phase = 2;
			return false;
		}
		if (Role == TEXT("owner") && Phase == 2)
		{
			const auto* Spec = Source->GetHeroAbilitySystem()->FindAbilitySpecFromHandle(Source->GetHeroAbilitySystem()->FindSlot(
				IsCone() ? MobaGameplayTags::Input_Slot_Target : MobaGameplayTags::Input_Slot_Primary));
			const auto* Ability = Spec ? Cast<UTargetDamageAbility>(Spec->Ability) : nullptr;
			FAbilityTargetIntent Intent;
			if (!Ability || !Controller->BuildCursorIntent(Ability->GetGeometry(), Intent)
				|| FVector::DotProduct(Intent.Direction.GetSafeNormal2D(), FVector::ForwardVector) < 0.999)
			{
				Phase = 1;
				return false;
			}
			Ack = Controller->GetIntentResultSequence();
			Press(Controller, IsCone() ? EKeys::R : EKeys::Q);
			Phase = 3;
			return false;
		}
		if (Role == TEXT("owner") && Phase == 3 && Controller->GetIntentResultSequence() != Ack)
		{
			Test->TestTrue(TEXT("Q/R 经正常输入路由获服务器接受"), Controller->WasLastIntentAccepted());
			Test->TestTrue(TEXT("正常 profile 保持"), Controller->GetActiveProfile() == ExpectedProfile());
			Mark(Step(TEXT("input-accepted")));
			Phase = 4;
		}
		if ((Role == TEXT("observer") && Phase == 1) || (Role == TEXT("owner") && Phase == 4))
		{
			if (!Has(Step(TEXT("server-result"))) || Source->GetAttributes()->GetMana() != 80 || Target->GetAttributes()->GetHealth() != ExpectedHealth()
				|| Control->GetHealth() != 40 || !bSawCooldown)
			{
				return false;
			}
			Test->TestEqual(TEXT("公开目标 HP 复制"), Target->GetAttributes()->GetHealth(), ExpectedHealth());
			Test->TestEqual(TEXT("公开正对照 HP 复制"), Control->GetHealth(), 40.0f);
			if (Role == TEXT("owner") && CaseIndex % CasesPerAbility == 0)
			{
				FScreenshotRequest::RequestScreenshot(FPaths::Combine(Evidence, Step(TEXT("owner-render.png"))), false, false);
			}
			Mark(Step(Role == TEXT("owner") ? TEXT("owner-observed") : TEXT("observer-observed")));
			Phase = 5;
		}
		return false;
	}


private:
	bool IsCone() const { return (CaseIndex / CasesPerAbility) % 2 == 1; }
	float ExpectedHealth() const { return CaseIndex % CasesPerAbility == 0 ? 40.0f : 100.0f; }
	EHeroInputProfile ExpectedProfile() const { return CaseIndex < 16 ? EHeroInputProfile::WASD : EHeroInputProfile::ClickToMove; }


private:
	FAutomationTestBase* Test = nullptr;
	FString Role;
	FString Evidence;
	double Started = 0;
	int32 CaseIndex = 0;
	int32 Phase = 0;
	uint32 Ack = 0;
	bool bSafeEvidence = false;
	bool bContentChecked = false;
	bool bProfileRequested = false;
	bool bSawCooldown = false;
	FKey HeldKey;
	TWeakObjectPtr<AHeroPlayerController> LocalController;

	TWeakObjectPtr<AHeroCharacter> ServerSource;
	TWeakObjectPtr<AHeroCharacter> ServerTarget;
	TWeakObjectPtr<ATargetCombatant> Witness;
	TWeakObjectPtr<AActor> Wall;
	FTransform OriginalSourceTransform;
	FTransform OriginalTargetTransform;
	EMovementMode OriginalSourceMovement = MOVE_None;
	EMovementMode OriginalTargetMovement = MOVE_None;
	int32 OriginalTargetTeam = INDEX_NONE;
	bool bFixtureStarted = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLineConeInputNetworkTest, "Moba.Network.LineConeInput",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FLineConeInputNetworkTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FLineConeInputNetworkScenario>(this));
	return true;
}

#endif
