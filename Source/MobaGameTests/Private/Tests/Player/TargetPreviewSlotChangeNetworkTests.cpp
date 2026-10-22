#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Experience/ExperienceSubsystem.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MobaGameplayTags.h"
#include "Match/MatchGameMode.h"
#include "Match/MatchGameState.h"
#include "Match/ParticipantState.h"

namespace
{
/** 显式测试工作区只约束输出根；不修改引擎ProjectDir或内容加载。 */
bool ResolvePreviewServerEvidenceRoot(FString& Workspace, FString& Root)
{
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);
	FPaths::CollapseRelativeDirectories(ProjectDir);
	const bool bExplicitWorkspace = FParse::Value(FCommandLine::Get(), TEXT("MobaPreviewWorkspace="), Workspace);
	if (bExplicitWorkspace)
	{
		if (Workspace.IsEmpty() || FPaths::IsRelative(Workspace))
		{
			return false;
		}

		FPaths::NormalizeDirectoryName(Workspace);
		FPaths::CollapseRelativeDirectories(Workspace);
		const FString ProjectFile = FPaths::Combine(Workspace, TEXT("MobaGame.uproject"));
		FString Descriptor;
		if (FString(FApp::GetProjectName()) != TEXT("MobaGame") || !FFileHelper::LoadFileToString(Descriptor, *ProjectFile)
			|| !Descriptor.Contains(TEXT("\"FileVersion\"")) || !Descriptor.Contains(TEXT("\"Name\": \"MobaGame\""))
			|| (ProjectDir != Workspace && !FPaths::IsUnderDirectory(ProjectDir, FPaths::Combine(Workspace, TEXT("AI")))))
		{
			return false;
		}
	}
	else
	{
		Workspace = ProjectDir;
	}

	Root = FPaths::Combine(Workspace, bExplicitWorkspace ? TEXT("AI/integration-012-015-20261009/final") : TEXT("AI/integration-012-015-20261009/task014"));
	FPaths::NormalizeDirectoryName(Root);
	FPaths::CollapseRelativeDirectories(Root);
	return true;
}


/** 仅在独立网络验收进程交换既有两槽；公开文件只协调阶段，不提供Gameplay管理入口。 */
class FTargetPreviewServerSlotScenario final : public IAutomationLatentCommand
{
public:
	explicit FTargetPreviewServerSlotScenario(FAutomationTestBase* InTest)
		: Test(InTest), Started(FPlatformTime::Seconds())
	{
		FParse::Value(FCommandLine::Get(), TEXT("MobaPreviewNetworkEvidence="), Evidence);
	}

	virtual ~FTargetPreviewServerSlotScenario() override
	{
		if (bSwapped && !bRestoreAttempted)
		{
			Restore();
		}
	}

	virtual bool Update() override
	{
		if (!bValidated && !ValidateDirectory())
		{
			return true;
		}

		if (Test->HasAnyErrors() || Has(TEXT("owner-failed")) || Has(TEXT("observer-failed")))
		{
			return Fail(TEXT("本端或客户端失败"));
		}

		if (FPlatformTime::Seconds() - Started > 120)
		{
			return Fail(TEXT("Dedicated Server场景超过120秒"));
		}

		UWorld* World = FindWorld();
		AMatchGameState* Match = World ? World->GetGameState<AMatchGameState>() : nullptr;
		UExperienceSubsystem* Experience = World ? World->GetSubsystem<UExperienceSubsystem>() : nullptr;
		if (!Match || !Match->MatchId.IsValid() || !Match->ExperienceInstance.IsValid() || !Experience
			|| Experience->GetSnapshot().State != EExperienceState::ExperienceReady)
		{
			return false;
		}

		if (!bReady)
		{
			MatchId = Match->MatchId;
			ExperienceId = Match->ExperienceInstance;
			Signal(TEXT("server-ready"), MatchId.ToString() + TEXT("|") + ExperienceId.ToString());
			bReady = true;
		}

		if (Match->MatchId != MatchId || Match->ExperienceInstance != ExperienceId)
		{
			return Fail(TEXT("测试期间比赛或Experience改变"));
		}

		if (Stage == 0)
		{
			AHeroCharacter* OwnerHero = ResolveHero(World, TEXT("owner-identity"));
			AHeroCharacter* ObserverHero = ResolveHero(World, TEXT("observer-identity"));
			if (!OwnerHero || !ObserverHero || OwnerHero == ObserverHero || !Has(TEXT("owner-slot-ready")) || !Has(TEXT("observer-slot-ready")))
			{
				return false;
			}

			UHeroAbilitySystemComponent* Abilities = OwnerHero->GetHeroAbilitySystem();
			uint32 ClientRevision = 0;
			uint32 ObserverRevision = 0;
			if (!LexTryParseString(ClientRevision, *Read(TEXT("owner-slot-ready"))) || ClientRevision != OwnerHero->RequiredLoadoutRevision
				|| !Abilities->HasReplicatedLoadout(ClientRevision)
				|| !LexTryParseString(ObserverRevision, *Read(TEXT("observer-slot-ready"))) || ObserverRevision != ObserverHero->RequiredLoadoutRevision
				|| !ObserverHero->GetHeroAbilitySystem()->HasReplicatedLoadout(ObserverRevision))
			{
				return Fail(TEXT("客户端准备版本与服务器真实槽不一致"));
			}

			Owner = OwnerHero;
			Observer = ObserverHero;
			OwnerEpoch = OwnerHero->ConnectionEpoch;
			ObserverEpoch = ObserverHero->ConnectionEpoch;
			OriginalSlots = Abilities->GetSlots();
			ObserverSlots = ObserverHero->GetHeroAbilitySystem()->GetSlots();
			ObserverState = ObserverHero->GetHeroAbilitySystem()->GetStateRevision();
			OriginalPrimary = Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary);
			OriginalUnit = Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack);
			if (!OriginalPrimary.IsValid() || !OriginalUnit.IsValid() || OriginalPrimary == OriginalUnit)
			{
				return Fail(TEXT("需要两个既有且不同的技能槽"));
			}

			const FAbilitySourceResult Result = Abilities->RemapSlots(MobaGameplayTags::Input_Slot_Primary,
				MobaGameplayTags::Input_Slot_UnitAttack, Abilities->GetStateRevision());
			if (Result.Status != EAbilitySourceResult::Applied)
			{
				return Fail(TEXT("服务器Remap未Applied"));
			}

			bSwapped = true;
			ChangedState = Abilities->GetStateRevision();
			ChangedRevision = Abilities->GetLoadoutRevision();
			ChangedSlots = Abilities->GetSlots();
			if (ChangedRevision == ClientRevision || OwnerHero->RequiredLoadoutRevision != ChangedRevision
				|| Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary) != OriginalUnit
				|| Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack) != OriginalPrimary)
			{
				return Fail(TEXT("Remap实际映射或公开版本未推进"));
			}

			Signal(TEXT("server-slot-changed"), LexToString(ChangedRevision));
			Stage = 1;
			return false;
		}

		if (Stage == 1)
		{
			if (!Has(TEXT("owner-slot-cleared")) || !Has(TEXT("observer-slot-isolated")))
			{
				return false;
			}

			if (!StableObserver() || !Restore())
			{
				return Fail(TEXT("服务器隔离或精确恢复失败"));
			}

			Signal(TEXT("server-slot-restored"), LexToString(Owner->RequiredLoadoutRevision));
			Stage = 2;
			return false;
		}

		if (Stage == 2)
		{
			if (!Has(TEXT("owner-slot-restored")) || !Has(TEXT("observer-slot-restored")))
			{
				return false;
			}

			if (!StableObserver() || !SameBinding(Owner.Get(), OwnerEpoch)
				|| !SameSlots(Owner->GetHeroAbilitySystem()->GetSlots(), OriginalSlots))
			{
				return Fail(TEXT("客户端恢复确认前服务器映射或绑定改变"));
			}

			Signal(TEXT("server-slot-complete"));
			Stage = 3;
		}

		// 只保持服务器进程存活，让恢复后的普通输入段完成；输入结果由各客户端原生报告证明。
		if (!Has(TEXT("owner-passed")) || !Has(TEXT("observer-passed")))
		{
			return false;
		}

		Signal(TEXT("server-passed"));
		return !Test->HasAnyErrors();
	}


private:
	bool ValidateDirectory()
	{
		FPaths::NormalizeDirectoryName(Evidence);
		FPaths::CollapseRelativeDirectories(Evidence);
		FString Workspace, Root;
		if (!ResolvePreviewServerEvidenceRoot(Workspace, Root))
		{
			Test->AddError(TEXT("目标预览fixture工作区越界"));
			return false;
		}

		Test->AddInfo(FString::Printf(TEXT("PreviewBoundary ProjectDir=%s Workspace=%s Root=%s"),
			*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), *Workspace, *Root));
		FPaths::NormalizeDirectoryName(Root);
		if (Evidence.IsEmpty() || FPaths::IsRelative(Evidence) || !Evidence.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase)
			|| Has(TEXT("server-ready")) || Has(TEXT("server-failed")) || Has(TEXT("server-passed")))
		{
			Test->AddError(TEXT("Server证据必须为本树TASK-014下全新绝对目录"));
			return false;
		}

		if (!IFileManager::Get().MakeDirectory(*Evidence, true))
		{
			Test->AddError(TEXT("无法创建Server证据目录"));
			return false;
		}

		bValidated = true;
		return true;
	}

	UWorld* FindWorld() const
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World && World->IsGameWorld() && World->GetNetMode() == NM_DedicatedServer && !World->bIsTearingDown)
			{
				return World;
			}
		}

		return nullptr;
	}

	AHeroCharacter* ResolveHero(UWorld* World, const TCHAR* Identity) const
	{
		TArray<FString> Fields;
		Read(Identity).ParseIntoArray(Fields, TEXT("|"));
		FGuid Match, Experience, Entity, Participant;
		if (Fields.Num() != 4 || !FGuid::Parse(Fields[0], Match) || !FGuid::Parse(Fields[1], Experience)
			|| !FGuid::Parse(Fields[2], Entity) || !FGuid::Parse(Fields[3], Participant) || Match != MatchId || Experience != ExperienceId)
		{
			return nullptr;
		}

		for (TActorIterator<AHeroCharacter> It(World); It; ++It)
		{
			if (It->EntityId == Entity && It->ParticipantId == Participant && SameBinding(*It, It->ConnectionEpoch)
				&& It->CanAcceptPlayerInput())
			{
				return *It;
			}
		}

		return nullptr;
	}

	bool SameBinding(AHeroCharacter* Hero, uint32 Epoch) const
	{
		if (!IsValid(Hero) || Hero->IsActorBeingDestroyed() || !Hero->GetWorld() || Hero->GetWorld()->bIsTearingDown
			|| Hero->GetNetMode() != NM_DedicatedServer || Hero->MatchId != MatchId || !Epoch || Hero->ConnectionEpoch != Epoch)
		{
			return false;
		}

		const AMatchGameMode* Mode = Hero->GetWorld()->GetAuthGameMode<AMatchGameMode>();
		const AMatchGameState* Match = Hero->GetWorld()->GetGameState<AMatchGameState>();
		const AHeroPlayerController* Controller = Cast<AHeroPlayerController>(Hero->GetController());
		const AParticipantState* Participant = Controller ? Controller->GetPlayerState<AParticipantState>() : nullptr;
		return Mode && Match && Match->MatchId == MatchId && Match->ExperienceInstance == ExperienceId && Participant && Mode->OwnsParticipant(Participant)
			&& IsValid(Hero->GetHeroAbilitySystem()) && Controller->GetPawn() == Hero
			&& Participant->bConnected && Participant->GetOwner() == Controller && Participant->PrimaryHero == Hero
			&& Participant->ParticipantId == Hero->ParticipantId && Participant->MatchId == MatchId && Participant->ConnectionEpoch == Epoch;
	}

	bool SameSlots(const TArray<FHeroAbilitySlot>& Actual, const TArray<FHeroAbilitySlot>& Expected) const
	{
		if (Actual.Num() != Expected.Num())
		{
			return false;
		}

		for (const FHeroAbilitySlot& Entry : Expected)
		{
			if (!Actual.ContainsByPredicate([&Entry](const FHeroAbilitySlot& Value)
				{ return Value.Slot == Entry.Slot && Value.Handle == Entry.Handle && Value.Source == Entry.Source; }))
			{
				return false;
			}
		}

		return true;
	}

	bool StableObserver() const
	{
		return SameBinding(Observer.Get(), ObserverEpoch) && Observer->GetHeroAbilitySystem()->GetStateRevision() == ObserverState
			&& SameSlots(Observer->GetHeroAbilitySystem()->GetSlots(), ObserverSlots);
	}

	bool Restore()
	{
		bRestoreAttempted = true;
		if (!bSwapped || !SameBinding(Owner.Get(), OwnerEpoch))
		{
			Test->AddError(TEXT("CleanupIncomplete: 原绑定已不在，不能覆盖新状态"));
			return false;
		}

		UHeroAbilitySystemComponent* Abilities = Owner->GetHeroAbilitySystem();
		if (Abilities->GetStateRevision() != ChangedState || Abilities->GetLoadoutRevision() != ChangedRevision
			|| !SameSlots(Abilities->GetSlots(), ChangedSlots))
		{
			Test->AddError(TEXT("CleanupIncomplete: 映射或版本已被其他操作改变"));
			return false;
		}

		const FAbilitySourceResult Result = Abilities->RemapSlots(MobaGameplayTags::Input_Slot_Primary,
			MobaGameplayTags::Input_Slot_UnitAttack, ChangedState);
		if (Result.Status != EAbilitySourceResult::Applied || !SameSlots(Abilities->GetSlots(), OriginalSlots)
			|| Owner->RequiredLoadoutRevision != Abilities->GetLoadoutRevision() || Abilities->GetLoadoutRevision() == ChangedRevision)
		{
			Test->AddError(TEXT("CleanupIncomplete: 恢复结果或真实映射不符"));
			return false;
		}

		bSwapped = false;
		Test->AddInfo(TEXT("Server原Spec handle映射已精确恢复"));
		return true;
	}

	bool Has(const TCHAR* Name) const
	{
		return IFileManager::Get().FileExists(*FPaths::Combine(Evidence, Name));
	}

	FString Read(const TCHAR* Name) const
	{
		FString Value;
		if (Has(Name))
		{
			FFileHelper::LoadFileToString(Value, *FPaths::Combine(Evidence, Name));
		}

		return Value;
	}

	void Signal(const TCHAR* Name, const FString& Value = TEXT("ready"))
	{
		Test->TestTrue(TEXT("Server公开标记保存到任务AI"), FFileHelper::SaveStringToFile(Value,
			*FPaths::Combine(Evidence, Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	bool Fail(const TCHAR* Reason)
	{
		Test->AddError(FString::Printf(TEXT("TargetPreviewServerSlot Stage=%d Reason=%s"), Stage, Reason));
		if (bSwapped && !bRestoreAttempted)
		{
			Restore();
		}

		Signal(TEXT("server-failed"), Reason);
		return true;
	}


private:
	FAutomationTestBase* Test;
	double Started;
	FString Evidence;
	FGuid MatchId;
	FGuid ExperienceId;
	TWeakObjectPtr<AHeroCharacter> Owner;
	TWeakObjectPtr<AHeroCharacter> Observer;
	TArray<FHeroAbilitySlot> OriginalSlots;
	TArray<FHeroAbilitySlot> ChangedSlots;
	TArray<FHeroAbilitySlot> ObserverSlots;
	FGameplayAbilitySpecHandle OriginalPrimary;
	FGameplayAbilitySpecHandle OriginalUnit;
	uint32 OwnerEpoch = 0;
	uint32 ObserverEpoch = 0;
	uint32 ObserverState = 0;
	uint32 ChangedState = 0;
	uint32 ChangedRevision = 0;
	int32 Stage = 0;
	bool bValidated = false;
	bool bReady = false;
	bool bSwapped = false;
	bool bRestoreAttempted = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetPreviewServerSlotTest, "Moba.UIFixture.TargetPreview.ServerSlotChange",
	EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FTargetPreviewServerSlotTest::RunTest(const FString& Parameters)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("MobaTargetPreviewSlotFixture")) || !IsRunningDedicatedServer())
	{
		AddError(TEXT("仅允许显式MobaTargetPreviewSlotFixture的独立Dedicated Server测试"));
		return false;
	}

	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FTargetPreviewServerSlotScenario>(this));
	return true;
}

#endif
