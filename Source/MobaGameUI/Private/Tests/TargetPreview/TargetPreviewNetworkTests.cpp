#include "TargetPreview/TargetPreviewWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/TargetDamageAbility.h"
#include "ClientPresentationSubsystem.h"
#include "Experience/ExperienceDefinitions.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Experience/ExperienceSubsystem.h"
#include "Feedback/ClientFeedbackWidget.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Items/ItemInventoryComponent.h"
#include "Layout/Children.h"
#include "Layout/WidgetPath.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MobaGameplayTags.h"
#include "Match/MatchGameState.h"
#include "Match/ParticipantState.h"
#include "UnrealClient.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWindow.h"
#include "Widgets/SViewport.h"

namespace
{
/** 显式测试工作区只约束输出根；不修改引擎ProjectDir或内容加载。 */
bool ResolvePreviewClientEvidenceRoot(FString& Workspace, FString& Root)
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


enum class EPreviewOwnerStep : uint8
{
	Prepare, SelectTarget, MoveStart, MoveHeld, MoveObserved, ClickProfile, ClickStart, ClickObserved, RestoreProfile,
	QStart, QAccepted, QRepeat, QRejected, ItemStart, ItemRejected, RejectObserved, EStart, EAccepted,
	RStart, RAccepted, RespawnObserved, Finish
};

/** 两个真实客户端只驱动既有输入；公开标记只协调测试，不提交Gameplay结果。 */
class FTargetPreviewNetworkScenario final : public IAutomationLatentCommand
{
public:
	explicit FTargetPreviewNetworkScenario(FAutomationTestBase* InTest)
		: Test(InTest), Started(FPlatformTime::Seconds())
	{
		FParse::Value(FCommandLine::Get(), TEXT("MobaPreviewRole="), Role);
		FParse::Value(FCommandLine::Get(), TEXT("MobaPreviewNetworkEvidence="), Evidence);
	}

	virtual ~FTargetPreviewNetworkScenario() override
	{
		ReleaseInput();
		if (ObservedViewport.IsValid() && ViewportInputHandle.IsValid())
		{
			ObservedViewport->OnInputKey().Remove(ViewportInputHandle);
		}
		if (bSavedCursor && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetCursorPos(OriginalCursor);
		}
	}

	virtual bool Update() override
	{
		if (!bValidated && !ValidateRun())
		{
			return true;
		}

		if (bReleaseNextFrame)
		{
			if (bRightHeld && KeyController.IsValid())
			{
				Test->AddInfo(FString::Printf(TEXT("RightPointerNextFrame PressFrame=%llu Frame=%llu ViewportDown=%d PlayerKeyDown=%d Sequence=%u Baseline=%u"),
					RightPressFrame, GFrameCounter, RightViewportDownCount, KeyController->IsInputKeyDown(EKeys::RightMouseButton),
					KeyController->GetIntentResultSequence(), Ack));
			}

			ReleaseInput();
		}

		if (Test->HasAnyErrors())
		{
			return Fail(TEXT("已有失败断言"));
		}

		if (Has(TEXT("owner-failed")) || Has(TEXT("observer-failed")) || Has(TEXT("server-failed")))
		{
			return Fail(TEXT("对端失败，见该角色报告"));
		}

		if (FPlatformTime::Seconds() - Started > 240)
		{
			return Fail(TEXT("等待绑定/复制/输入后果超过240秒"));
		}

		UWorld* World = FindClientWorld();
		UGameInstance* Instance = World ? World->GetGameInstance() : nullptr;
		if (!Instance || Instance->GetLocalPlayers().Num() != 1)
		{
			return false;
		}

		ULocalPlayer* LocalPlayer = Instance->GetLocalPlayerByIndex(0);
		AHeroPlayerController* Controller = Cast<AHeroPlayerController>(LocalPlayer->GetPlayerController(World));
		const FClientTargetContext Context = MobaClientTarget::ReadContext(Controller);
		UClientPresentationSubsystem* Host = LocalPlayer->GetSubsystem<UClientPresentationSubsystem>();
		UTargetPreviewWidget* Preview = Host ? Cast<UTargetPreviewWidget>(Host->FindLayer(TEXT("TargetPreview"))) : nullptr;
		UClientFeedbackWidget* Feedback = Host ? Cast<UClientFeedbackWidget>(Host->FindLayer(TEXT("Feedback"))) : nullptr;
		AMatchGameState* Match = World->GetGameState<AMatchGameState>();
		if (!Context.bValid || !Preview || !Feedback || !Match || !LocalPlayer->ViewportClient || !LocalPlayer->ViewportClient->Viewport)
		{
			return false;
		}

		if (Context.bReady && !AreSlotButtonsArranged(Preview, Context))
		{
			return false;
		}

		if (!bIdentified)
		{
			if (!Context.bReady || Preview->GetCachedGeometry().GetLocalSize().IsNearlyZero()
				|| Preview->GetSelectedSlot() != MobaGameplayTags::Input_Slot_Target)
			{
				return false;
			}

			const UExperienceDefinition* Experience = World->GetSubsystem<UExperienceSubsystem>()->GetExperience();
			if (!Experience || !Experience->bItemsEnabled || !Match->ExperienceInstance.IsValid())
			{
				return Fail(TEXT("需使用已启用物品的Batch0811样例"));
			}

			// 新隔离比赛尚未交易；等初始经济与库存实际到达，避免零值快照恰好同版本。
			if (Context.Participant->GetEconomy().Gold != Experience->InitialGold
				|| Context.Hero->GetInventory()->GetSnapshot().Revision != Context.Participant->GetEconomy().Revision)
			{
				return false;
			}

			OwnEntity = Context.EntityId;
			InitialDeathCount = Context.DeathCount;
			OwnHero = Context.Hero;
			OwnAbilities = Context.Abilities;
			IsolatedIntent = Controller->GetIntentResultSequence();
			IsolatedItem = Controller->GetLastItemResult().Sequence;
			OriginalCursor = FSlateApplication::Get().GetCursorPos();
			bSavedCursor = true;
			Test->TestEqual(TEXT("网络Preview根可选子控件"), Preview->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);
			Test->TestEqual(TEXT("网络Feedback整层穿透"), Feedback->GetVisibility(), ESlateVisibility::HitTestInvisible);
			Signal(*(Role + TEXT("-identity")), Match->MatchId.ToString() + TEXT("|") + Match->ExperienceInstance.ToString() + TEXT("|")
				+ Context.EntityId.ToString() + TEXT("|") + Context.ParticipantId.ToString());
			bIdentified = true;
		}

		if (Role == TEXT("owner") && !bViewportActivated)
		{
			if (!ClickViewport(Controller))
			{
				return Fail(TEXT("owner原生视口激活失败"));
			}

			bViewportActivated = true;
			return false;
		}

		const FString PeerRole = Role == TEXT("owner") ? TEXT("observer") : TEXT("owner");
		FGuid PeerEntity;
		if (!ReadPeerIdentity(PeerRole, Match, PeerEntity))
		{
			return Test->HasAnyErrors();
		}

		AHeroCharacter* Peer = nullptr;
		for (TActorIterator<AHeroCharacter> It(World); It; ++It)
		{
			if (It->EntityId == PeerEntity)
			{
				Peer = *It;
				break;
			}
		}

		if (!Peer || !Peer->GetAttributes())
		{
			return false;
		}

		Test->TestTrue(TEXT("两端实体不同且同一比赛"), Peer->EntityId != Context.EntityId && Peer->MatchId == Context.MatchId);
		Test->TestTrue(TEXT("UI始终读取当前本地拥有端"), Preview->GetOwningPlayer() == Controller && Feedback->GetOwningPlayer() == Controller
			&& Context.Hero.Get() == Controller->GetPawn() && Context.Hero.Get() != Peer);
		if (Test->HasAnyErrors())
		{
			return Fail(TEXT("拥有端/比赛绑定不符合"));
		}

		if (FParse::Param(FCommandLine::Get(), TEXT("MobaTargetPreviewSlotFixture")) && !bSlotComplete)
		{
			return SlotChange(Context, Peer, Preview, Feedback);
		}

		return Role == TEXT("owner") ? Owner(Controller, Context, Peer, Preview, Feedback, Match) : Observer(Controller, Context, Peer, Preview, Feedback);
	}

private:
	bool SlotChange(const FClientTargetContext& Context, AHeroCharacter* Peer, UTargetPreviewWidget* Preview, UClientFeedbackWidget* Feedback)
	{
		const bool bOwnerRole = Role == TEXT("owner");
		UHeroAbilitySystemComponent* Abilities = Context.Abilities.Get();
		if (!Context.bReady || !Abilities->HasReplicatedLoadout(Context.LoadoutRevision))
		{
			return false;
		}

		if (SlotStage == 0)
		{
			if (!Has(TEXT("server-ready")))
			{
				return false;
			}

			if (bOwnerRole && !ClickSlot(Preview, Context, MobaGameplayTags::Input_Slot_Primary))
			{
				return Fail(TEXT("Remap前必须通过真实指针选中Primary"));
			}

			SlotContext = Context;
			SavedSlots = MobaClientTarget::ReadSlots(Context);
			SavedSelection = Preview->GetSelectedSlot();
			SavedPrimary = Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary);
			SavedUnit = Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack);
			if (!SavedPrimary.IsValid() || !SavedUnit.IsValid() || SavedPrimary == SavedUnit || !SavedSelection.IsValid())
			{
				return Fail(TEXT("Remap场景必须有实际选择和两个既有不同技能槽"));
			}

			Signal(*(Role + TEXT("-slot-ready")), LexToString(Context.LoadoutRevision));
			SlotStage = 1;
			return false;
		}

		if (!Context.HasSameBinding(SlotContext))
		{
			return Fail(TEXT("Remap期间拥有端绑定改变"));
		}

		if (!bOwnerRole)
		{
			Test->TestTrue(TEXT("observer自身槽与版本未被owner的Remap改变"), Context.HasSamePresentation(SlotContext)
				&& MobaClientTarget::ReadSlots(Context) == SavedSlots);
			Test->TestEqual(TEXT("observer自身选择保持"), Preview->GetSelectedSlot(), SavedSelection);
			Test->TestTrue(TEXT("observer没有owner私有输入槽"), Peer->GetHeroAbilitySystem()->GetSlots().IsEmpty());
		}

		AHeroPlayerController* Controller = Context.Controller.Get();
		Test->TestEqual(TEXT("Remap不产生一般操作结果"), Controller->GetIntentResultSequence(), IsolatedIntent);
		Test->TestEqual(TEXT("Remap不产生物品结果"), Controller->GetLastItemResult().Sequence, IsolatedItem);
		Test->TestTrue(TEXT("Remap不带入其他玩家拒绝文字"), Feedback->GetIntentText().IsEmpty() && Feedback->GetItemText().IsEmpty());
		if (Test->HasAnyErrors())
		{
			return Fail(TEXT("Remap隔离断言失败"));
		}

		const TCHAR* VersionMarker = SlotStage == 1 ? TEXT("server-slot-changed") : TEXT("server-slot-restored");
		uint32 Required = 0;
		if (SlotStage < 3 && (!LexTryParseString(Required, *Read(VersionMarker)) || !Required))
		{
			return false;
		}

		if (SlotStage < 3)
		{
			if ((bOwnerRole ? Context.LoadoutRevision : Peer->RequiredLoadoutRevision) != Required)
			{
				return false;
			}

			if (bOwnerRole)
			{
				TArray<FTargetPreviewSlot> ExpectedSlots = SavedSlots;
				if (SlotStage == 1)
				{
					for (FTargetPreviewSlot& Entry : ExpectedSlots)
					{
						if (Entry.Slot == MobaGameplayTags::Input_Slot_Primary)
						{
							Entry.Handle = SavedUnit;
						}
						else if (Entry.Slot == MobaGameplayTags::Input_Slot_UnitAttack)
						{
							Entry.Handle = SavedPrimary;
						}
					}
				}

				Test->TestTrue(TEXT("owner实际私有复制映射符合服务器阶段"), MobaClientTarget::ReadSlots(Context) == ExpectedSlots);
				// Actor复制与Slate Tick不保证同帧；在场景上限内等待实际控件消费新版本。
				if (Preview->GetSelectedSlot().IsValid() || Preview->GetOutlinePointCount() != 0)
				{
					return false;
				}

				Test->TestFalse(TEXT("owner实际Widget旧选择清理且恢复时不自动重选"), Preview->GetSelectedSlot().IsValid());
				Test->TestEqual(TEXT("owner实际Widget旧轮廓清理"), Preview->GetOutlinePointCount(), 0);
				Test->TestTrue(TEXT("owner新版本已离开原始映射版本"), Required != SlotContext.LoadoutRevision);
			}

			if (Test->HasAnyErrors())
			{
				return Fail(TEXT("Remap复制或实际Widget清理失败"));
			}

			Signal(SlotStage == 1 ? (bOwnerRole ? TEXT("owner-slot-cleared") : TEXT("observer-slot-isolated"))
				: (bOwnerRole ? TEXT("owner-slot-restored") : TEXT("observer-slot-restored")));
			++SlotStage;
			return false;
		}

		if (Has(TEXT("server-slot-complete")))
		{
			bSlotComplete = true;
			Test->AddInfo(TEXT("网络Remap交换/私有复制/实际UI清理/observer隔离/精确恢复已观察；开始普通输入段"));
		}

		return false;
	}


	bool ValidateRun()
	{
		if ((Role != TEXT("owner") && Role != TEXT("observer")) || Evidence.IsEmpty() || FPaths::IsRelative(Evidence))
		{
			Test->AddError(TEXT("必须指定owner/observer和本任务下新的绝对Evidence目录"));
			return false;
		}

		Evidence = FPaths::ConvertRelativePathToFull(Evidence);
		FPaths::NormalizeDirectoryName(Evidence);
		FPaths::CollapseRelativeDirectories(Evidence);
		FString Workspace, Root;
		if (!ResolvePreviewClientEvidenceRoot(Workspace, Root))
		{
			Test->AddError(TEXT("目标预览fixture工作区越界"));
			return false;
		}

		Test->AddInfo(FString::Printf(TEXT("PreviewBoundary ProjectDir=%s Workspace=%s Root=%s"),
			*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), *Workspace, *Root));
		FPaths::NormalizeDirectoryName(Root);
		if (!Evidence.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase)
			|| Has(*(Role + TEXT("-identity"))) || Has(*(Role + TEXT("-failed"))) || Has(*(Role + TEXT("-passed"))))
		{
			Test->AddError(TEXT("Evidence越界或当前角色已留存结果；不得覆盖旧运行"));
			return false;
		}

		if (!IFileManager::Get().MakeDirectory(*Evidence, true))
		{
			Test->AddError(TEXT("无法创建本任务证据目录"));
			return false;
		}

		bValidated = true;
		return true;
	}

	UWorld* FindClientWorld() const
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (UWorld* World = Context.World(); World && World->IsGameWorld() && World->GetNetMode() == NM_Client && !World->bIsTearingDown)
			{
				return World;
			}
		}

		return nullptr;
	}

	bool ReadPeerIdentity(const FString& PeerRole, const AMatchGameState* Match, FGuid& Entity)
	{
		TArray<FString> Fields;
		Read(*(PeerRole + TEXT("-identity"))).ParseIntoArray(Fields, TEXT("|"));
		FGuid PeerMatch, PeerExperience, PeerParticipant;
		if (Fields.Num() != 4 || !FGuid::Parse(Fields[0], PeerMatch) || !FGuid::Parse(Fields[1], PeerExperience)
			|| !FGuid::Parse(Fields[2], Entity) || !FGuid::Parse(Fields[3], PeerParticipant))
		{
			return false;
		}

		if (PeerMatch != Match->MatchId || PeerExperience != Match->ExperienceInstance || !Entity.IsValid() || !PeerParticipant.IsValid())
		{
			Fail(TEXT("对端不是同一实际Match/Experience，拒绝旧标记"));
			return false;
		}

		return true;
	}

	bool Owner(AHeroPlayerController* Controller, const FClientTargetContext& Context, AHeroCharacter* Peer,
		UTargetPreviewWidget* Preview, UClientFeedbackWidget* Feedback, const AMatchGameState* Match)
	{
		if (!Context.bReady)
		{
			return false;
		}

		AHeroCharacter* Hero = Context.Hero.Get();
		switch (OwnerStep)
		{
		case EPreviewOwnerStep::Prepare:
			if (!Has(TEXT("observer-ready")))
			{
				return false;
			}

			if (Match->Teams.Query(Hero->TeamId, Peer->TeamId) != ETeamRelation::Enemy || Controller->GetActiveProfile() != EHeroInputProfile::WASD)
			{
				return Fail(TEXT("需两名敌对样例Hero并从WASD开始"));
			}

			SelectionAck = Controller->GetIntentResultSequence();
			if (!ClickSlot(Preview, Context, MobaGameplayTags::Input_Slot_Primary))
			{
				return Fail(TEXT("网络实际指针选槽失败"));
			}

			OwnerStep = EPreviewOwnerStep::SelectTarget;
			return false;
		case EPreviewOwnerStep::SelectTarget:
			Test->TestEqual(TEXT("实际点击从默认Target改选Primary"), Preview->GetSelectedSlot(), FGameplayTag(MobaGameplayTags::Input_Slot_Primary));
			if (!ClickSlot(Preview, Context, MobaGameplayTags::Input_Slot_Target))
			{
				return Fail(TEXT("网络实际指针切回Target失败"));
			}

			OwnerStep = EPreviewOwnerStep::MoveStart;
			return false;
		case EPreviewOwnerStep::MoveStart:
			Test->TestEqual(TEXT("移动前实际选中Target"), Preview->GetSelectedSlot(), FGameplayTag(MobaGameplayTags::Input_Slot_Target));
			Test->TestEqual(TEXT("选槽不提交操作"), Controller->GetIntentResultSequence(), SelectionAck);
			MoveOrigin = Hero->GetActorLocation();
			Signal(*FString::Printf(TEXT("move-%d-start"), MoveIndex), MoveOrigin.ToString());
			PressKey(Controller, MoveKey(MoveIndex), false);
			OwnerStep = EPreviewOwnerStep::MoveHeld;
			return false;
		case EPreviewOwnerStep::MoveHeld:
			if (FVector::DotProduct(Hero->GetActorLocation() - MoveOrigin, MoveDirection(MoveIndex)) < 100)
			{
				return false;
			}

			ReleaseInput();
			OwnerStep = EPreviewOwnerStep::MoveObserved;
			return false;
		case EPreviewOwnerStep::MoveObserved:
			if (!Has(*FString::Printf(TEXT("observer-move-%d"), MoveIndex)))
			{
				return false;
			}

			++MoveIndex;
			if (MoveIndex < 4)
			{
				OwnerStep = EPreviewOwnerStep::MoveStart;
				return false;
			}

			PressKey(Controller, EKeys::Tab, true);
			OwnerStep = EPreviewOwnerStep::ClickProfile;
			return false;
		case EPreviewOwnerStep::ClickProfile:
			if (Controller->GetActiveProfile() != EHeroInputProfile::ClickToMove)
			{
				return false;
			}

			Test->TestFalse(TEXT("真实网络Profile切换清预览选择"), Preview->GetSelectedSlot().IsValid());
			OwnerStep = EPreviewOwnerStep::ClickStart;
			return false;
		case EPreviewOwnerStep::ClickStart:
		{
			MoveOrigin = Hero->GetActorLocation();
			if (!Aim(Controller, MoveOrigin + FVector(0, 180, 0)))
			{
				return Fail(TEXT("右键目标投影失败"));
			}

			FHitResult Hit;
			if (!Controller->GetHitResultUnderCursor(ECC_Visibility, false, Hit) || FVector::DistXY(Hit.Location, MoveOrigin) < 100
				|| FVector::DistXY(Hit.Location, MoveOrigin) > 700)
			{
				return Fail(TEXT("右键命中不是有界可移动场景点"));
			}

			ClickDestination = Hit.Location;
			Signal(TEXT("click-start"), MoveOrigin.ToString());
			Ack = Controller->GetIntentResultSequence();
			if (!PressRightPointer(Controller, Preview))
			{
				return Fail(TEXT("原生Slate右键事件未提交"));
			}

			OwnerStep = EPreviewOwnerStep::ClickObserved;
			return false;
		}
		case EPreviewOwnerStep::ClickObserved:
			if (Controller->GetIntentResultSequence() == Ack || FVector::DistXY(Hero->GetActorLocation(), MoveOrigin) < 100
				|| FVector::DistXY(Hero->GetActorLocation(), ClickDestination) > 80 || !Has(TEXT("observer-click")))
			{
				return false;
			}

			Test->TestTrue(TEXT("右键穿过C非交互区域并被服务器接受"), Controller->WasLastIntentAccepted());
			PressKey(Controller, EKeys::Tab, true);
			OwnerStep = EPreviewOwnerStep::RestoreProfile;
			return false;
		case EPreviewOwnerStep::RestoreProfile:
			if (Controller->GetActiveProfile() != EHeroInputProfile::WASD)
			{
				return false;
			}

			if (!ClickSlot(Preview, Context, MobaGameplayTags::Input_Slot_Target))
			{
				return Fail(TEXT("恢复WASD后指针选槽失败"));
			}

			OwnerStep = EPreviewOwnerStep::QStart;
			return false;
		case EPreviewOwnerStep::QStart:
			if (!CanCast(Context, MobaGameplayTags::Input_Slot_Primary))
			{
				return false;
			}

			PrimaryBefore = Hero->GetAttributes()->GetMana();
			Ack = Controller->GetIntentResultSequence();
			PressKey(Controller, EKeys::Q, true);
			OwnerStep = EPreviewOwnerStep::QAccepted;
			return false;
		case EPreviewOwnerStep::QAccepted:
			if (Controller->GetIntentResultSequence() == Ack || Hero->GetAttributes()->GetMana() >= PrimaryBefore)
			{
				return false;
			}

			Test->TestTrue(TEXT("Q经正常权威路径接受并收到成本"), Controller->WasLastIntentAccepted());
			PrimaryAfter = Hero->GetAttributes()->GetMana();
			OwnerStep = EPreviewOwnerStep::QRepeat;
			return false;
		case EPreviewOwnerStep::QRepeat:
			Ack = Controller->GetIntentResultSequence();
			PressKey(Controller, EKeys::Q, true);
			OwnerStep = EPreviewOwnerStep::QRejected;
			return false;
		case EPreviewOwnerStep::QRejected:
			if (Controller->GetIntentResultSequence() == Ack || Feedback->GetIntentText().IsEmpty())
			{
				return false;
			}

			Test->TestFalse(TEXT("owner正常Q的真实拒绝"), Controller->WasLastIntentAccepted());
			Test->TestEqual(TEXT("owner服务器原因Cooldown"), Controller->GetLastIntentReason(), FName(TEXT("Cooldown")));
			Test->TestEqual(TEXT("重复Q拒绝不扣成本"), Hero->GetAttributes()->GetMana(), PrimaryAfter);
			Signal(TEXT("owner-intent-rejected"));
			OwnerStep = EPreviewOwnerStep::ItemStart;
			return false;
		case EPreviewOwnerStep::ItemStart:
			if (Hero->GetInventory()->GetSnapshot().Revision != Context.Participant->GetEconomy().Revision)
			{
				return false;
			}

			ItemAck = Controller->GetLastItemResult().Sequence;
			GoldBefore = Context.Participant->GetEconomy().Gold;
			InventoryBefore = Hero->GetInventory()->GetSnapshot().Revision;
			// 沿已有Exec方法的真实RequestItem/RPC链；没有添加管理或交易UI接口。
			Controller->MobaBuy(TEXT("ArmorPlate"), 0);
			OwnerStep = EPreviewOwnerStep::ItemRejected;
			return false;
		case EPreviewOwnerStep::ItemRejected:
		{
			const FItemResult Result = Controller->GetLastItemResult();
			if (Result.Sequence == ItemAck || Feedback->GetItemText().IsEmpty())
			{
				return false;
			}

			Test->TestEqual(TEXT("实际物品反馈属于当前Epoch"), Result.Epoch, Context.Epoch);
			Test->TestFalse(TEXT("无效数量交易被服务器拒绝"), Result.bAccepted);
			Test->TestEqual(TEXT("无效数量原因"), Result.Reason, FName(TEXT("Quantity")));
			Test->TestEqual(TEXT("拒绝交易不改金币"), Context.Participant->GetEconomy().Gold, GoldBefore);
			Test->TestEqual(TEXT("拒绝交易不发布库存版本"), Hero->GetInventory()->GetSnapshot().Revision, InventoryBefore);
			Signal(TEXT("owner-item-rejected"));
			if (!RequestCapture(TEXT("owner-feedback")))
			{
				return false;
			}

			OwnerStep = EPreviewOwnerStep::RejectObserved;
			return false;
		}
		case EPreviewOwnerStep::RejectObserved:
			if (!Has(TEXT("observer-isolated")) || !CaptureFinished())
			{
				return false;
			}

			OwnerStep = EPreviewOwnerStep::EStart;
			return false;
		case EPreviewOwnerStep::EStart:
			if (!Peer->IsAlive() || !CanCast(Context, MobaGameplayTags::Input_Slot_UnitAttack))
			{
				return false;
			}

			if (!Aim(Controller, Peer->GetActorLocation()))
			{
				return Fail(TEXT("E鼠标目标投影失败"));
			}

			PeerHealthBefore = Peer->GetAttributes()->GetHealth();
			AttackDeathsBefore = Peer->GetLifeSnapshot().DeathCount;
			Signal(TEXT("e-before"), FString::SanitizeFloat(PeerHealthBefore));
			Ack = Controller->GetIntentResultSequence();
			PressKey(Controller, EKeys::E, true);
			OwnerStep = EPreviewOwnerStep::EAccepted;
			return false;
		case EPreviewOwnerStep::EAccepted:
			if (Controller->GetIntentResultSequence() == Ack
				|| (Peer->GetAttributes()->GetHealth() >= PeerHealthBefore && Peer->GetLifeSnapshot().DeathCount == AttackDeathsBefore) || !Has(TEXT("observer-e")))
			{
				return false;
			}

			Test->TestTrue(TEXT("E经正常输入接受，两个客户端观察到伤害"), Controller->WasLastIntentAccepted());
			OwnerStep = EPreviewOwnerStep::RStart;
			return false;
		case EPreviewOwnerStep::RStart:
			if (RCasts > 0 && Peer->GetLifeSnapshot().DeathCount > AttackDeathsBefore)
			{
				OwnerStep = EPreviewOwnerStep::RespawnObserved;
				return false;
			}

			if (!Peer->IsAlive() || !CanCast(Context, MobaGameplayTags::Input_Slot_Target))
			{
				return false;
			}

			if (RCasts >= 3 || !Aim(Controller, Peer->GetActorLocation()))
			{
				return Fail(TEXT("R场景超出三次有界施放或目标无法投影"));
			}

			PeerHealthBefore = Peer->GetAttributes()->GetHealth();
			AttackDeathsBefore = Peer->GetLifeSnapshot().DeathCount;
			Signal(*FString::Printf(TEXT("r-%d-before"), RCasts), FString::SanitizeFloat(PeerHealthBefore));
			Ack = Controller->GetIntentResultSequence();
			PressKey(Controller, EKeys::R, true);
			OwnerStep = EPreviewOwnerStep::RAccepted;
			return false;
		case EPreviewOwnerStep::RAccepted:
			if (Controller->GetIntentResultSequence() == Ack
				|| (Peer->GetAttributes()->GetHealth() >= PeerHealthBefore && Peer->GetLifeSnapshot().DeathCount == AttackDeathsBefore)
				|| !Has(*FString::Printf(TEXT("observer-r-%d"), RCasts)))
			{
				return false;
			}

			Test->TestTrue(TEXT("R经正常输入接受并实际投送伤害"), Controller->WasLastIntentAccepted());
			++RCasts;
			OwnerStep = Peer->IsAlive() ? EPreviewOwnerStep::RStart : EPreviewOwnerStep::RespawnObserved;
			return false;
		case EPreviewOwnerStep::RespawnObserved:
			if (!Has(TEXT("observer-death-cleared")) || !Has(TEXT("observer-respawned")) || !Peer->IsAlive())
			{
				return false;
			}

			OwnerStep = EPreviewOwnerStep::Finish;
			return false;
		case EPreviewOwnerStep::Finish:
			if (Test->HasAnyErrors())
			{
				return Fail(TEXT("owner最终断言失败"));
			}

			Signal(TEXT("owner-passed"));
			return true;
		default:
			return Fail(TEXT("未知owner阶段"));
		}
	}

	bool Observer(AHeroPlayerController* Controller, const FClientTargetContext& Context, AHeroCharacter* OwnerHero,
		UTargetPreviewWidget* Preview, UClientFeedbackWidget* Feedback)
	{
		Test->TestEqual(TEXT("observer本地能力结果不接收owner结果"), Controller->GetIntentResultSequence(), IsolatedIntent);
		Test->TestEqual(TEXT("observer本地物品结果不接收owner结果"), Controller->GetLastItemResult().Sequence, IsolatedItem);
		Test->TestTrue(TEXT("observer无owner拒绝文字"), Feedback->GetIntentText().IsEmpty() && Feedback->GetItemText().IsEmpty());
		Test->TestTrue(TEXT("observer仍持有自己的Hero/ASC"), Context.Hero == OwnHero && Context.Abilities == OwnAbilities && Context.EntityId == OwnEntity);
		Test->TestTrue(TEXT("observer未收到owner的私有输入槽"), OwnerHero->GetHeroAbilitySystem()->GetSlots().IsEmpty());
		if (Test->HasAnyErrors())
		{
			return Fail(TEXT("observer隔离失败"));
		}

		if (!Has(TEXT("observer-ready")))
		{
			Signal(TEXT("observer-ready"));
		}

		for (int32 Index = 0; Index < 4; ++Index)
		{
			FVector Start;
			if (!ObservedMoves[Index] && Start.InitFromString(Read(*FString::Printf(TEXT("move-%d-start"), Index)))
				&& FVector::DotProduct(OwnerHero->GetActorLocation() - Start, MoveDirection(Index)) >= 60)
			{
				ObservedMoves[Index] = true;
				Signal(*FString::Printf(TEXT("observer-move-%d"), Index));
			}
		}

		FVector ClickStart;
		if (!bObservedClick && ClickStart.InitFromString(Read(TEXT("click-start"))) && FVector::DistXY(OwnerHero->GetActorLocation(), ClickStart) >= 60)
		{
			bObservedClick = true;
			Signal(TEXT("observer-click"));
		}

		if (!bObservedIsolation && Has(TEXT("owner-intent-rejected")) && Has(TEXT("owner-item-rejected")))
		{
			if (!RequestCapture(TEXT("observer-isolation")))
			{
				return false;
			}

			bObservedIsolation = true;
		}

		if (bObservedIsolation && !Has(TEXT("observer-isolated")) && CaptureFinished())
		{
			Signal(TEXT("observer-isolated"));
		}

		AHeroCharacter* Hero = Context.Hero.Get();
		const FString EBefore = Read(TEXT("e-before"));
		if (!bObservedE && !EBefore.IsEmpty() && Hero->GetAttributes()->GetHealth() < FCString::Atof(*EBefore))
		{
			bObservedE = true;
			Signal(TEXT("observer-e"));
		}

		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FString Before = Read(*FString::Printf(TEXT("r-%d-before"), Index));
			if (!Before.IsEmpty() && !Has(*FString::Printf(TEXT("observer-r-%d"), Index)) && Hero->GetAttributes()->GetHealth() < FCString::Atof(*Before))
			{
				Signal(*FString::Printf(TEXT("observer-r-%d"), Index));
			}
		}

		if (!bObservedDeath && !Context.bAlive && Context.DeathCount > InitialDeathCount && !Preview->GetSelectedSlot().IsValid()
			&& Preview->GetOutlinePointCount() == 0 && Feedback->GetIntentText().IsEmpty() && Feedback->GetItemText().IsEmpty())
		{
			bObservedDeath = true;
			Signal(TEXT("observer-death-cleared"));
		}

		if (bObservedDeath && Context.bReady && Context.bAlive && !bObservedRespawn)
		{
			Test->TestFalse(TEXT("网络实际复活不恢复旧选择"), Preview->GetSelectedSlot().IsValid());
			bObservedRespawn = true;
			Signal(TEXT("observer-respawned"));
		}

		if (!Has(TEXT("owner-passed")))
		{
			return false;
		}

		Test->TestTrue(TEXT("四向WASD复制均实际观察"), ObservedMoves[0] && ObservedMoves[1] && ObservedMoves[2] && ObservedMoves[3]);
		Test->TestTrue(TEXT("右键移动/隔离/E伤害/死亡复活均实际观察"), bObservedClick && bObservedIsolation && bObservedE && bObservedDeath && bObservedRespawn);
		if (Test->HasAnyErrors())
		{
			return Fail(TEXT("observer最终覆盖不完整"));
		}

		Signal(TEXT("observer-passed"));
		return true;
	}

	bool CanCast(const FClientTargetContext& Context, FGameplayTag AbilitySlot) const
	{
		UHeroAbilitySystemComponent* Abilities = Context.Abilities.Get();
		const FGameplayAbilitySpec* Spec = Abilities->FindAbilitySpecFromHandle(Abilities->FindSlot(AbilitySlot));
		return Spec && Spec->Ability && Abilities->IsManagedAbilityEnabled(Spec->Handle)
			&& Spec->Ability->CheckCooldown(Spec->Handle, Abilities->AbilityActorInfo.Get()) && Spec->Ability->CheckCost(Spec->Handle, Abilities->AbilityActorInfo.Get());
	}

	bool Aim(AHeroPlayerController* Controller, const FVector& Point) const
	{
		FVector2D Screen;
		if (!Controller->ProjectWorldLocationToScreen(Point, Screen, false))
		{
			return false;
		}

		Controller->SetMouseLocation(FMath::RoundToInt(Screen.X), FMath::RoundToInt(Screen.Y));
		return true;
	}

	FKey MoveKey(int32 Index) const
	{
		const FKey Keys[] = {EKeys::W, EKeys::A, EKeys::S, EKeys::D};
		return Keys[Index];
	}

	FVector MoveDirection(int32 Index) const
	{
		const FVector Directions[] = {FVector::ForwardVector, -FVector::RightVector, -FVector::ForwardVector, FVector::RightVector};
		return Directions[Index];
	}

	void PressKey(AHeroPlayerController* Controller, FKey Key, bool bReleaseOnNextFrame)
	{
		KeyController = Controller;
		HeldKey = Key;
		bKeyHeld = true;
		bReleaseNextFrame = bReleaseOnNextFrame;
		Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, Key, IE_Pressed, FPlatformTime::Cycles64()));
	}

	void ReleaseInput()
	{
		if (bKeyHeld && KeyController.IsValid())
		{
			KeyController->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, HeldKey, IE_Released, FPlatformTime::Cycles64()));
		}

		if (bRightHeld && FSlateApplication::IsInitialized())
		{
			const TSet<FKey> Empty;
			FSlateApplication& Slate = FSlateApplication::Get();
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
				PointerPosition, PointerPosition, Empty, EKeys::RightMouseButton, 0, FModifierKeysState()));
		}

		bKeyHeld = false;
		bRightHeld = false;
		bReleaseNextFrame = false;
	}

	void ObserveViewportInput(const FInputKeyEventArgs& Args)
	{
		if (Args.Key == EKeys::RightMouseButton)
		{
			RightViewportDownCount += Args.Event == IE_Pressed ? 1 : 0;
			Test->AddInfo(FString::Printf(TEXT("RightViewportInput Event=%d Device=%d Frame=%llu"),
				static_cast<int32>(Args.Event), Args.InputDevice.GetId(), GFrameCounter));
		}
	}

	bool PressRightPointer(AHeroPlayerController* Controller, UTargetPreviewWidget* Preview)
	{
		const TSharedPtr<SWidget> Root = Preview->GetCachedWidget();
		UGameViewportClient* ViewportClient = Controller->GetWorld()->GetGameViewport();
		const TSharedPtr<SViewport> Viewport = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
		if (!Root || !Viewport)
		{
			return false;
		}

		FSlateApplication& Slate = FSlateApplication::Get();
		const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Root.ToSharedRef());
		PointerPosition = Slate.GetCursorPos();
		const FWidgetPath HitPath = Slate.LocateWindowUnderMouse(PointerPosition, Slate.GetInteractiveTopLevelWindows(), false, Slate.GetUserIndexForMouse());
		FString HitTypes;
		bool bHitViewport = false;
		for (int32 HitIndex = 0; HitIndex < HitPath.Widgets.Num(); ++HitIndex)
		{
			const FArrangedWidget& Hit = HitPath.Widgets[HitIndex];
			HitTypes += Hit.Widget->GetTypeAsString() + TEXT("/");
			bHitViewport |= Hit.Widget == Viewport;
		}

		const TSharedPtr<SWidget> Captor = Slate.GetCursorUser()->GetCursorCaptor();
		Test->AddInfo(FString::Printf(TEXT("RightPointerProbe Position=%s WindowVisible=%d WindowMinimized=%d HitViewport=%d CaptureMode=%d Captor=%s SlateActive=%d IgnoreInput=%d Path=%s"),
			*PointerPosition.ToString(), Window && Window->IsVisible(), Window && Window->IsWindowMinimized(), bHitViewport,
			static_cast<int32>(ViewportClient->GetMouseCaptureMode()), Captor ? *Captor->GetTypeAsString() : TEXT("None"),
			Slate.IsActive(), ViewportClient->IgnoreInput(), *HitTypes));
		Test->TestTrue(TEXT("右键真实命中本端SViewport"), bHitViewport);
		if (!bHitViewport)
		{
			return false;
		}

		// 只观察UE输入边界，不消费或改写输入；析构移除本测试订阅。
		ObservedViewport = ViewportClient;
		ViewportInputHandle = ViewportClient->OnInputKey().AddRaw(this, &FTargetPreviewNetworkScenario::ObserveViewportInput);
		KeyController = Controller;
		RightPressFrame = GFrameCounter;
		const TSet<FKey> Buttons = {EKeys::RightMouseButton};
		const TSet<FKey> Empty;
		Slate.ProcessMouseMoveEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			PointerPosition, PointerPosition, Empty, EKeys::Invalid, 0, FModifierKeysState()));
		const bool bHandled = Slate.ProcessMouseButtonDownEvent(Window ? Window->GetNativeWindow() : nullptr,
			FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
				PointerPosition, PointerPosition, Buttons, EKeys::RightMouseButton, 0, FModifierKeysState()));
		Test->AddInfo(FString::Printf(TEXT("RightPointerDispatch SlateHandled=%d ViewportDown=%d PlayerKeyDown=%d Sequence=%u Baseline=%u"),
			bHandled, RightViewportDownCount, Controller->IsInputKeyDown(EKeys::RightMouseButton), Controller->GetIntentResultSequence(), Ack));
		bRightHeld = true;
		bReleaseNextFrame = true;
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

	bool HasArea(const FGeometry& Geometry) const
	{
		const FVector2D Size = Geometry.GetLocalSize();
		return FMath::IsFinite(Size.X) && FMath::IsFinite(Size.Y) && Size.X > UE_SMALL_NUMBER && Size.Y > UE_SMALL_NUMBER;
	}

	bool AreSlotButtonsArranged(UTargetPreviewWidget* Preview, const FClientTargetContext& Context) const
	{
		TArray<TSharedPtr<SButton>> Buttons;
		if (Preview->GetCachedWidget())
		{
			CollectButtons(Preview->GetCachedWidget().ToSharedRef(), Buttons);
		}

		return HasArea(Preview->GetCachedGeometry()) && Buttons.Num() == MobaClientTarget::ReadSlots(Context).Num() + 1
			&& !Buttons.ContainsByPredicate([this](const TSharedPtr<SButton>& Button) { return !Button || !HasArea(Button->GetCachedGeometry()); });
	}

	bool ClickViewport(AHeroPlayerController* Controller)
	{
		const TSharedPtr<SViewport> Viewport = Controller->GetWorld()->GetGameViewport()->GetGameViewportWidget();
		if (!Viewport || !HasArea(Viewport->GetCachedGeometry()))
		{
			return false;
		}

		FSlateApplication& Slate = FSlateApplication::Get();
		const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Viewport.ToSharedRef());
		const FVector2D Position = Viewport->GetCachedGeometry().LocalToAbsolute(Viewport->GetCachedGeometry().GetLocalSize() * FVector2D(0.5, 0.65));
		const FVector2D Previous = Slate.GetCursorPos();
		const TSharedPtr<SWidget> Captor = Slate.GetCursorUser()->GetCursorCaptor();
		Test->AddInfo(FString::Printf(TEXT("OwnerViewportActivation CaptorBefore=%s"), Captor ? *Captor->GetTypeAsString() : TEXT("None")));
		Slate.SetCursorPos(Position);
		const TSet<FKey> Down = {EKeys::LeftMouseButton};
		const TSet<FKey> Up;
		Slate.ProcessMouseMoveEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			Position, Previous, Up, EKeys::Invalid, 0, FModifierKeysState()));
		Slate.ProcessMouseButtonDownEvent(Window ? Window->GetNativeWindow() : nullptr,
			FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex, Position, Position, Down, EKeys::LeftMouseButton, 0, FModifierKeysState()));
		Slate.ProcessMouseButtonUpEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			Position, Position, Up, EKeys::LeftMouseButton, 0, FModifierKeysState()));
		return true;
	}

	bool ClickSlot(UTargetPreviewWidget* Preview, const FClientTargetContext& Context, FGameplayTag DesiredSlot)
	{
		const TArray<FTargetPreviewSlot> Slots = MobaClientTarget::ReadSlots(Context);
		const int32 Index = Slots.IndexOfByPredicate([DesiredSlot](const FTargetPreviewSlot& Entry) { return Entry.Slot == DesiredSlot; });
		TArray<TSharedPtr<SButton>> Buttons;
		if (!Preview->GetCachedWidget())
		{
			return false;
		}

		CollectButtons(Preview->GetCachedWidget().ToSharedRef(), Buttons);
		if (!Buttons.IsValidIndex(Index))
		{
			return false;
		}

		FSlateApplication& Slate = FSlateApplication::Get();
		const TSharedPtr<SButton> Button = Buttons[Index];
		const FGeometry& Geometry = Button->GetCachedGeometry();
		if (!HasArea(Geometry))
		{
			return false;
		}

		const TSharedPtr<SWidget> Focus = Slate.GetKeyboardFocusedWidget();
		const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Button.ToSharedRef());
		const FVector2D Position = Geometry.LocalToAbsolute(Geometry.GetLocalSize() * 0.5f);
		const FVector2D Previous = Slate.GetCursorPos();
		Slate.SetCursorPos(Position);
		const TSet<FKey> Down = {EKeys::LeftMouseButton};
		const TSet<FKey> Up;
		Slate.ProcessMouseMoveEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			Position, Previous, Up, EKeys::Invalid, 0, FModifierKeysState()));
		Test->TestTrue(TEXT("网络原生指针命中按钮"), Button->IsHovered());
		Slate.ProcessMouseButtonDownEvent(Window ? Window->GetNativeWindow() : nullptr,
			FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex, Position, Position, Down, EKeys::LeftMouseButton, 0, FModifierKeysState()));
		Slate.ProcessMouseButtonUpEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			Position, Position, Up, EKeys::LeftMouseButton, 0, FModifierKeysState()));
		Test->TestTrue(TEXT("网络选槽不转移键盘焦点"), Slate.GetKeyboardFocusedWidget() == Focus);
		Test->TestEqual(TEXT("实际点击选中指定槽"), Preview->GetSelectedSlot(), DesiredSlot);
		return !Test->HasAnyErrors();
	}

	bool RequestCapture(const TCHAR* Name)
	{
		if (FScreenshotRequest::IsScreenshotRequested())
		{
			return false;
		}

		PendingShot = FPaths::Combine(Evidence, FString(Name) + TEXT(".png"));
		if (IFileManager::Get().FileExists(*PendingShot))
		{
			Fail(TEXT("截图已存在，拒绝复用旧证据"));
			return false;
		}

		FScreenshotRequest::RequestScreenshot(PendingShot, true, false);
		return true;
	}

	bool CaptureFinished() const
	{
		return !PendingShot.IsEmpty() && !FScreenshotRequest::IsScreenshotRequested() && IFileManager::Get().FileExists(*PendingShot);
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
		if (!bValidated || (Test->HasAnyErrors() && !FString(Name).EndsWith(TEXT("-failed"))))
		{
			return;
		}

		Test->TestTrue(TEXT("公开场景标记写入任务AI"), FFileHelper::SaveStringToFile(Value, *FPaths::Combine(Evidence, Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	bool Fail(const TCHAR* Reason)
	{
		ReleaseInput();
		const FString Message = FString::Printf(TEXT("TargetPreviewNetwork Role=%s Step=%d Move=%d R=%d Reason=%s"), *Role, int32(OwnerStep), MoveIndex, RCasts, Reason);
		Test->AddError(Message);
		Signal(*(Role + TEXT("-failed")), Message);
		return true;
	}


private:
	FAutomationTestBase* Test;
	double Started;
	FString Role;
	FString Evidence;
	FString PendingShot;
	FVector2D OriginalCursor = FVector2D::ZeroVector;
	FVector2D PointerPosition = FVector2D::ZeroVector;
	FVector MoveOrigin = FVector::ZeroVector;
	FVector ClickDestination = FVector::ZeroVector;
	FGuid OwnEntity;
	TWeakObjectPtr<AHeroCharacter> OwnHero;
	TWeakObjectPtr<UHeroAbilitySystemComponent> OwnAbilities;
	TWeakObjectPtr<AHeroPlayerController> KeyController;
	TWeakObjectPtr<UGameViewportClient> ObservedViewport;
	FDelegateHandle ViewportInputHandle;
	FClientTargetContext SlotContext;
	TArray<FTargetPreviewSlot> SavedSlots;
	FGameplayTag SavedSelection;
	FGameplayAbilitySpecHandle SavedPrimary;
	FGameplayAbilitySpecHandle SavedUnit;
	FKey HeldKey;
	EPreviewOwnerStep OwnerStep = EPreviewOwnerStep::Prepare;
	uint32 IsolatedIntent = 0;
	uint32 IsolatedItem = 0;
	uint32 InitialDeathCount = 0;
	uint32 SelectionAck = 0;
	uint32 Ack = 0;
	uint32 ItemAck = 0;
	uint32 InventoryBefore = 0;
	uint32 AttackDeathsBefore = 0;
	int32 GoldBefore = 0;
	int32 MoveIndex = 0;
	int32 RCasts = 0;
	int32 SlotStage = 0;
	int32 RightViewportDownCount = 0;
	uint64 RightPressFrame = 0;
	float PrimaryBefore = 0;
	float PrimaryAfter = 0;
	float PeerHealthBefore = 0;
	bool ObservedMoves[4] = {};
	bool bValidated = false;
	bool bIdentified = false;
	bool bSavedCursor = false;
	bool bKeyHeld = false;
	bool bRightHeld = false;
	bool bReleaseNextFrame = false;
	bool bObservedClick = false;
	bool bObservedIsolation = false;
	bool bObservedE = false;
	bool bObservedDeath = false;
	bool bObservedRespawn = false;
	bool bSlotComplete = false;
	bool bViewportActivated = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetPreviewNetworkTest, "Moba.UI.TargetPreview.NetworkInputAndIsolation",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FTargetPreviewNetworkTest::RunTest(const FString& Parameters)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("MobaTargetPreviewNetworkFixture"))
		|| FParse::Param(FCommandLine::Get(), TEXT("NullRHI")) || !FSlateApplication::IsInitialized())
	{
		AddError(TEXT("必须显式启用MobaTargetPreviewNetworkFixture并使用带渲染客户端"));
		return false;
	}

	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FTargetPreviewNetworkScenario>(this));
	return true;
}

#endif
