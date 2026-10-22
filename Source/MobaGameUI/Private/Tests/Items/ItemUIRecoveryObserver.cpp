#include "Tests/Items/ItemUIRecoveryObserver.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ClientPresentationSubsystem.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Feedback/ClientFeedbackWidget.h"
#include "Framework/Application/SlateApplication.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "Items/ItemInventoryComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MobaGameplayTags.h"
#include "Match/ParticipantState.h"
#include "Status/StatusPanelWidget.h"
#include "TargetPreview/TargetPreviewWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWindow.h"

namespace
{
/** 显式测试工作区只约束输出根；不修改引擎ProjectDir或内容加载。 */
bool ResolveItemObserverEvidenceRoot(FString& Workspace, FString& Root)
{
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);
	FPaths::CollapseRelativeDirectories(ProjectDir);
	const bool bExplicitWorkspace = FParse::Value(FCommandLine::Get(), TEXT("MobaItemUIWorkspace="), Workspace);
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

	Root = FPaths::Combine(Workspace, bExplicitWorkspace ? TEXT("AI/task-011/integration-012-015-20261009") : TEXT("AI/task-011/integration-012-015-20261009"));
	FPaths::NormalizeDirectoryName(Root);
	FPaths::CollapseRelativeDirectories(Root);
	return true;
}


class FItemUIRecoveryObserver
{
public:
	void Register()
	{
		if (!FParse::Param(FCommandLine::Get(), TEXT("MobaItemUIRecovery")) || IsRunningDedicatedServer() || IsRunningCommandlet())
		{
			return;
		}

		FParse::Value(FCommandLine::Get(), TEXT("MobaItemRole="), Role);
		FParse::Value(FCommandLine::Get(), TEXT("MobaItemEvidence="), Evidence);
		FParse::Value(FCommandLine::Get(), TEXT("MobaItemUIRunToken="), Token);
		FPaths::NormalizeDirectoryName(Evidence);
		FPaths::CollapseRelativeDirectories(Evidence);
		FString Root;
		const bool bWorkspaceValid = ResolveItemObserverEvidenceRoot(Workspace, Root);
		FGuid ParsedToken;
		bArgumentsValid = bWorkspaceValid && (Role == TEXT("initial") || Role == TEXT("observer") || Role == TEXT("restart"))
			&& !FPaths::IsRelative(Evidence) && Evidence.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase)
			&& FGuid::ParseExact(Token, EGuidFormats::Digits, ParsedToken) && ParsedToken.IsValid();
		StartHandle = FAutomationTestFramework::Get().OnTestStartEvent.AddRaw(this, &FItemUIRecoveryObserver::OnStart);
		EndHandle = FAutomationTestFramework::Get().OnTestEndEvent.AddRaw(this, &FItemUIRecoveryObserver::OnEnd);
	}

	void Unregister()
	{
		Stop();
		FAutomationTestFramework::Get().OnTestStartEvent.Remove(StartHandle);
		FAutomationTestFramework::Get().OnTestEndEvent.Remove(EndHandle);
		StartHandle.Reset();
		EndHandle.Reset();
	}

	bool IsIdle() const
	{
		return !Test && !TickerHandle.IsValid() && !PreviousController.IsValid() && !PreviousPreview.IsValid()
			&& !PreviousFeedback.IsValid() && !PreviousStatus.IsValid() && SeedEpoch == 0 && ProbeEpoch == 0 && !bSavedCursor;
	}

	bool CheckInvalidCleanup(FAutomationTestBase* Current)
	{
		// 路径在本卡AI内，但不符合Items fixture允许根；结束回调必须只清理而不创建文件。
		Evidence = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(),
			TEXT("AI/integration-012-015-20261009/task014/invalid-cleanup"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		Role = TEXT("initial");
		Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		bArgumentsValid = false;
		Test = Current;
		OnEnd(Current);
		return IsIdle() && !FPaths::DirectoryExists(Evidence) && !FPaths::FileExists(FPaths::Combine(Evidence, TEXT("ui-initial-cleaned")));
	}

private:
	void OnStart(FAutomationTestBase* Current)
	{
		if (FAutomationTestFramework::Get().GetCurrentTestFullPath() != TEXT("Moba.Network.Items"))
		{
			return;
		}

		Stop();
		Test = Current;
		if (!bArgumentsValid)
		{
			Test->AddError(TEXT("UIItemRecoveryInvalidArguments"));
			return;
		}

		Test->AddInfo(FString::Printf(TEXT("UIItemBoundary ProjectDir=%s Workspace=%s Role=%s Token=%s"),
			*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), *Workspace, *Role, *Token));
		Test->AddInfo(FString::Printf(TEXT("UIItemObserverCaptured Role=%s Token=%s Test=Moba.Network.Items"), *Role, *Token));
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FItemUIRecoveryObserver::Tick));
	}

	void OnEnd(FAutomationTestBase* Current)
	{
		if (Current != Test)
		{
			return;
		}

		// 引擎已计算成功状态；这里只撤销回调，不补报验收断言。
		Stop();
		if (bArgumentsValid)
		{
			FFileHelper::SaveStringToFile(Token + TEXT("|TickerRemoved|WeakReferencesCleared"),
				*FPaths::Combine(Evidence, TEXT("ui-") + Role + TEXT("-cleaned")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
	}

	void Stop()
	{
		if (TickerHandle.IsValid())
		{
			FTSTicker::RemoveTicker(TickerHandle);
			TickerHandle.Reset();
		}

		Test = nullptr;
		if (bSavedCursor && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetCursorPos(SavedCursor);
		}

		bSavedCursor = false;
		PreviousController.Reset();
		PreviousPreview.Reset();
		PreviousFeedback.Reset();
		PreviousStatus.Reset();
		SeedEpoch = 0;
		ProbeEpoch = 0;
		ProbeRevision = 0;
		bBaselineObserved = false;
	}

	FString Read(const TCHAR* Name) const
	{
		FString Value;
		FFileHelper::LoadFileToString(Value, *FPaths::Combine(Evidence, Name));
		return Value;
	}

	bool Mark(const TCHAR* Name)
	{
		if (!bArgumentsValid)
		{
			return false;
		}

		if (!FFileHelper::SaveStringToFile(Token, *FPaths::Combine(Evidence, Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Test->AddError(FString::Printf(TEXT("UIItemRecoveryMarkerFailed Name=%s"), Name));
			return false;
		}

		Test->AddInfo(FString::Printf(TEXT("UIItemRecoveryObserved Role=%s Phase=%s Token=%s"), *Role, Name, *Token));
		return true;
	}

	void CollectButtons(const TSharedRef<SWidget>& Widget, TArray<TSharedPtr<SButton>>& Buttons) const
	{
		if (Widget->GetType() == TEXT("SButton"))
		{
			Buttons.Add(StaticCastSharedRef<SButton>(Widget));
		}

		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			CollectButtons(Children->GetChildAt(Index), Buttons);
		}
	}

	bool SelectPrimary(UTargetPreviewWidget* Preview, const FClientTargetContext& Context)
	{
		const TArray<FTargetPreviewSlot> Slots = MobaClientTarget::ReadSlots(Context);
		const int32 Index = Slots.IndexOfByPredicate([](const FTargetPreviewSlot& Slot) { return Slot.Slot == MobaGameplayTags::Input_Slot_Primary; });
		TArray<TSharedPtr<SButton>> Buttons;
		if (!Preview->GetCachedWidget())
		{
			return false;
		}

		CollectButtons(Preview->GetCachedWidget().ToSharedRef(), Buttons);
		if (!Buttons.IsValidIndex(Index) || !Slots[Index].bEnabled || !FSlateApplication::IsInitialized())
		{
			return false;
		}

		const TSharedPtr<SButton> Button = Buttons[Index];
		const FGeometry& Geometry = Button->GetCachedGeometry();
		if (Geometry.GetLocalSize().X <= 0 || Geometry.GetLocalSize().Y <= 0)
		{
			return false;
		}

		FSlateApplication& Slate = FSlateApplication::Get();
		if (!bSavedCursor)
		{
			SavedCursor = Slate.GetCursorPos();
			bSavedCursor = true;
		}

		const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Button.ToSharedRef());
		const FVector2D Position = Geometry.LocalToAbsolute(Geometry.GetLocalSize() * .5);
		const FVector2D Previous = Slate.GetCursorPos();
		Slate.SetCursorPos(Position);
		const TSet<FKey> Up;
		const TSet<FKey> Down = {EKeys::LeftMouseButton};
		Slate.ProcessMouseMoveEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			Position, Previous, Up, EKeys::Invalid, 0, FModifierKeysState()));
		Slate.ProcessMouseButtonDownEvent(Window ? Window->GetNativeWindow() : nullptr,
			FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex, Position, Position, Down, EKeys::LeftMouseButton, 0, FModifierKeysState()));
		Slate.ProcessMouseButtonUpEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			Position, Position, Up, EKeys::LeftMouseButton, 0, FModifierKeysState()));
		return Preview->GetSelectedSlot() == MobaGameplayTags::Input_Slot_Primary;
	}

	bool StatusMatches(UStatusPanelWidget* Status, const FClientTargetContext& Context) const
	{
		const FStatusPanelSnapshot& Snapshot = Status->GetPresentedStatus();
		const AHeroCharacter* Hero = Context.Hero.Get();
		const UHeroAbilitySystemComponent* Abilities = Context.Abilities.Get();
		const FItemInventorySnapshot& Inventory = Hero->GetInventory()->GetSnapshot();
		if (!Snapshot.bOwnedHero || !Snapshot.bCombatReady || !Snapshot.bAbilitiesReady || !Snapshot.bInventoryReady
			|| Snapshot.Entity != Context.EntityId || Snapshot.Epoch != Context.Epoch || Snapshot.LifeState != Hero->GetLifeSnapshot().State
			|| Snapshot.LoadoutRevision != Context.LoadoutRevision || Snapshot.AbilityStateRevision != Context.StateRevision
			|| Snapshot.InventoryRevision != Inventory.Revision || Snapshot.Gold != Context.Participant->GetEconomy().Gold
			|| !FMath::IsNearlyEqual(Snapshot.Health, Hero->GetAttributes()->GetHealth()) || Snapshot.Abilities.Num() != Abilities->GetSlots().Num())
		{
			return false;
		}

		for (const FStatusAbilityRow& Row : Snapshot.Abilities)
		{
			if (Row.Handle != Abilities->FindSlot(Row.Slot))
			{
				return false;
			}
		}

		for (int32 Slot = 0; Slot < 6; ++Slot)
		{
			const FItemInstance* Item = Inventory.Items.FindByPredicate([Slot](const FItemInstance& Value) { return Value.Slot == Slot; });
			const FStatusItemRow& Row = Snapshot.Items[Slot];
			if (Item ? (Row.Instance != Item->Id || Row.ItemId != Item->ItemId || Row.Quantity != Item->Quantity)
				: (Row.Instance.IsValid() || !Row.ItemId.IsNone() || Row.Quantity != 0))
			{
				return false;
			}
		}

		return true;
	}

	bool Tick(float DeltaTime)
	{
		check(IsInGameThread());
		if (!Test || FAutomationTestFramework::Get().GetCurrentTest() != Test || Test->HasAnyErrors())
		{
			TickerHandle.Reset();
			return false;
		}

		UWorld* World = nullptr;
		for (const FWorldContext& Entry : GEngine->GetWorldContexts())
		{
			if (Entry.World() && Entry.World()->IsGameWorld() && !Entry.World()->bIsTearingDown)
			{
				World = Entry.World();
				break;
			}
		}

		if (!World)
		{
			return true;
		}

		if (Role == TEXT("initial") && SeedEpoch && World->GetNetMode() != NM_Client && !Read(TEXT("loss-detached")).IsEmpty())
		{
			if ((!PreviousPreview.IsValid() || (!PreviousPreview->IsInViewport() && !PreviousPreview->GetSelectedSlot().IsValid() && PreviousPreview->GetOutlinePointCount() == 0))
				&& (!PreviousFeedback.IsValid() || (!PreviousFeedback->IsInViewport() && PreviousFeedback->GetIntentText().IsEmpty() && PreviousFeedback->GetItemText().IsEmpty()))
				&& (!PreviousStatus.IsValid() || (!PreviousStatus->IsInViewport() && !PreviousStatus->GetPresentedStatus().bOwnedHero))
				&& Read(TEXT("ui-loss-cleared")) != Token)
			{
				Mark(TEXT("ui-loss-cleared"));
			}

			return true;
		}

		auto* Controller = Cast<AHeroPlayerController>(World->GetFirstPlayerController());
		const FClientTargetContext Context = MobaClientTarget::ReadContext(Controller);
		if (World->GetNetMode() != NM_Client || !Context.bReady || !Context.bAlive)
		{
			return true;
		}

		UClientPresentationSubsystem* Host = Controller->GetLocalPlayer()->GetSubsystem<UClientPresentationSubsystem>();
		auto* Preview = Host ? Cast<UTargetPreviewWidget>(Host->FindLayer(TEXT("TargetPreview"))) : nullptr;
		auto* Feedback = Host ? Cast<UClientFeedbackWidget>(Host->FindLayer(TEXT("Feedback"))) : nullptr;
		auto* Status = Host ? Cast<UStatusPanelWidget>(Host->FindLayer(TEXT("Status"))) : nullptr;
		if (!Preview || !Feedback || !Status || Preview->GetOwningPlayer() != Controller || Feedback->GetOwningPlayer() != Controller
			|| Status->GetOwningPlayer() != Controller || !StatusMatches(Status, Context))
		{
			return true;
		}

		if (Role == TEXT("observer"))
		{
			if (!Feedback->GetIntentText().IsEmpty() || !Feedback->GetItemText().IsEmpty())
			{
				Test->AddError(TEXT("UIItemObserverReceivedOwnerFeedback"));
			}
			else if (!Read(TEXT("restart-passed")).IsEmpty() && Read(TEXT("ui-observer-isolated")) != Token)
			{
				Mark(TEXT("ui-observer-isolated"));
			}

			return true;
		}

		if (Role == TEXT("initial") && !SeedEpoch)
		{
			if (FParse::Param(FCommandLine::Get(), TEXT("MobaItemUIInjectFailure")) && !Read(TEXT("commands-go")).IsEmpty()
				&& !Read(TEXT("observer-identity")).IsEmpty())
			{
				Test->AddInfo(TEXT("UIItemInjectionReady ServerCommandsGo=1 BothIdentities=1 CurrentClientReady=1 ThreeLayers=1 StatusCoherent=1"));
				Test->AddError(TEXT("UIItemRecoveryInjectedFailure"));
				return true;
			}

			if (Read(TEXT("ui-initial-mounted")) != Token)
			{
				Mark(TEXT("ui-initial-mounted"));
			}

			if (!Read(TEXT("loss-go")).IsEmpty() && SelectPrimary(Preview, Context))
			{
				PreviousController = Controller;
				PreviousPreview = Preview;
				PreviousFeedback = Feedback;
				PreviousStatus = Status;
				SeedEpoch = Context.Epoch;
				if (!FFileHelper::SaveStringToFile(Token + TEXT("|") + LexToString(SeedEpoch),
					*FPaths::Combine(Evidence, TEXT("ui-seed-epoch")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					Test->AddError(TEXT("UIItemSeedEpochWriteFailed"));
					return true;
				}

				Mark(TEXT("ui-loss-seeded"));
			}

			return true;
		}

		const TCHAR* Phase = Role == TEXT("restart") ? TEXT("ui-restart-restored") : TEXT("ui-same-process-restored");
		if (Read(Phase) == Token || (Role == TEXT("initial") && Context.Epoch <= SeedEpoch))
		{
			return true;
		}

		if (Role == TEXT("restart"))
		{
			FString SavedToken, SavedEpochText;
			uint32 SavedEpoch = 0;
			if (!Read(TEXT("ui-seed-epoch")).Split(TEXT("|"), &SavedToken, &SavedEpochText) || SavedToken != Token
				|| !LexTryParseString(SavedEpoch, *SavedEpochText) || Context.Epoch <= SavedEpoch)
			{
				Test->AddError(TEXT("UIItemRestartEpochOrRunTokenMismatch"));
				return true;
			}
		}

		if (Context.EntityId.ToString() != Read(TEXT("initial-identity")) || (Role == TEXT("initial")
			&& (Controller == PreviousController.Get() || Preview == PreviousPreview.Get() || Feedback == PreviousFeedback.Get() || Status == PreviousStatus.Get())))
		{
			Test->AddError(TEXT("UIItemRecoveryWrongBinding"));
			return true;
		}

		if (!bBaselineObserved)
		{
			if (!Feedback->GetIntentText().IsEmpty() || !Feedback->GetItemText().IsEmpty() || Preview->GetSelectedSlot() == MobaGameplayTags::Input_Slot_Primary)
			{
				Test->AddError(TEXT("UIItemRecoveryReplayedOldPresentation"));
				return true;
			}

			bBaselineObserved = true;
			ProbeEpoch = Context.Epoch;
			ProbeRevision = Context.Hero->GetInventory()->GetSnapshot().Revision;
			FItemRequest Request;
			Request.Entity = Context.EntityId;
			Request.Epoch = Context.Epoch;
			Request.Revision = ProbeRevision;
			Request.Sequence = 1;
			Request.ItemId = TEXT("ArmorPlate");
			Request.Quantity = 0;
			Controller->ServerItemRequest(Request);
			return true;
		}

		const FItemResult Result = Controller->GetLastItemResult();
		if (Result.Epoch == ProbeEpoch && Result.Sequence == 1 && !Feedback->GetItemText().IsEmpty())
		{
			Test->TestFalse(TEXT("UI恢复后的真实Quantity探针拒绝"), Result.bAccepted);
			Test->TestEqual(TEXT("UI恢复后的真实Quantity原因"), Result.Reason, FName(TEXT("Quantity")));
			Test->TestEqual(TEXT("UI拒绝探针不改变库存版本"), Context.Hero->GetInventory()->GetSnapshot().Revision, ProbeRevision);
			Test->TestEqual(TEXT("UI拒绝探针不改变Gold"), Context.Participant->GetEconomy().Gold, 0);
			if (!Test->HasAnyErrors())
			{
				Mark(Phase);
			}
		}

		return true;
	}

private:
	FDelegateHandle StartHandle;
	FDelegateHandle EndHandle;
	FTSTicker::FDelegateHandle TickerHandle;
	FAutomationTestBase* Test = nullptr;
	FString Role;
	FString Evidence;
	FString Workspace;
	FString Token;
	TWeakObjectPtr<AHeroPlayerController> PreviousController;
	TWeakObjectPtr<UTargetPreviewWidget> PreviousPreview;
	TWeakObjectPtr<UClientFeedbackWidget> PreviousFeedback;
	TWeakObjectPtr<UStatusPanelWidget> PreviousStatus;
	uint32 SeedEpoch = 0;
	uint32 ProbeEpoch = 0;
	uint32 ProbeRevision = 0;
	bool bBaselineObserved = false;
	bool bArgumentsValid = false;
	bool bSavedCursor = false;
	FVector2D SavedCursor = FVector2D::ZeroVector;
};

FItemUIRecoveryObserver Observer;
}

void MobaItemUIRecovery::RegisterObserver()
{
	Observer.Register();
}

void MobaItemUIRecovery::UnregisterObserver()
{
	Observer.Unregister();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FItemUIObserverIdleTest, "Moba.UI.TargetPreview.ItemObserverIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FItemUIObserverIdleTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Items用例结束后无owned ticker/test pointer/旧Widget引用"), Observer.IsIdle());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FItemUIObserverInvalidCleanupTest, "Moba.UI.TargetPreview.ItemObserverInvalidCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FItemUIObserverInvalidCleanupTest::RunTest(const FString& Parameters)
{
	FItemUIRecoveryObserver InvalidObserver;
	TestTrue(TEXT("无效Evidence的结束边界只清理且不创建目录/标记"), InvalidObserver.CheckInvalidCleanup(this));
	return true;
}

#endif
