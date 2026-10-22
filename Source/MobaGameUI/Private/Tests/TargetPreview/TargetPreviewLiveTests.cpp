#include "TargetPreview/TargetPreviewWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Combat/TargetDamageAbility.h"
#include "Blueprint/SlateBlueprintLibrary.h"
#include "ClientPresentationSubsystem.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "Experience/ExperienceSubsystem.h"
#include "Feedback/ClientFeedbackWidget.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "InputKeyEventArgs.h"
#include "InputCoreTypes.h"
#include "ImageUtils.h"
#include "Layout/Children.h"
#include "Layout/WidgetPath.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MobaGameplayTags.h"
#include "Match/MatchGameMode.h"
#include "UnrealClient.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWindow.h"
#include "Widgets/SViewport.h"

namespace
{
/** 只在显式启用的单机测试进程操作；不连接服务器、不在用户日常World中运行。 */
class FTargetPreviewLiveScenario final : public IAutomationLatentCommand
{
public:
	explicit FTargetPreviewLiveScenario(FAutomationTestBase* InTest, bool bInLineCone = false)
		: Test(InTest), Started(FPlatformTime::Seconds()), bLineCone(bInLineCone)
	{
		Evidence = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(),
			bLineCone ? TEXT("AI/integration-012-015-20261009/task014/line-cone") : TEXT("AI/integration-012-015-20261009/task014/live"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	virtual ~FTargetPreviewLiveScenario() override
	{
		UGameViewportClient::OnScreenshotCaptured().Remove(ShapeScreenshotHandle);
		ReleaseKey();
		RestoreLayers();
		if (bSavedCursor && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetCursorPos(OriginalCursor);
		}
	}

	virtual bool Update() override
	{
		ReleaseKey();
		if (Test->HasAnyErrors())
		{
			RestoreLayers();
			return true;
		}

		if (FPlatformTime::Seconds() - Started > 150)
		{
			return Fail(TEXT("等待真实就绪或场景谓词超时"));
		}

		UWorld* World = FindStandaloneWorld();
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		if (!GameInstance || GameInstance->GetLocalPlayers().Num() != 1)
		{
			return false;
		}

		ULocalPlayer* LocalPlayer = GameInstance->GetLocalPlayerByIndex(0);
		AHeroPlayerController* Controller = Cast<AHeroPlayerController>(LocalPlayer->GetPlayerController(World));
		UExperienceSubsystem* ExperienceSubsystem = World->GetSubsystem<UExperienceSubsystem>();
		if (Controller && AdmissionWorld.Get() != World && ExperienceSubsystem
			&& ExperienceSubsystem->GetSnapshot().State == EExperienceState::ExperienceReady)
		{
			AMatchGameMode* Match = World->GetAuthGameMode<AMatchGameMode>();
			if (!Match)
			{
				return Fail(TEXT("单机夹具缺少原生比赛入场入口"));
			}

			// 独立单机场景等地图就绪后正常入场；travel后的新比赛也建立新席位，不冒充重连。
			AdmissionWorld = World;
			Match->BeginAdmission(Controller, false, FResumeCredential());
			return false;
		}

		const FClientTargetContext Context = MobaClientTarget::ReadContext(Controller);
		if (!Context.bValid || (!Context.bReady && Stage != 70) || !LocalPlayer->ViewportClient || !LocalPlayer->ViewportClient->Viewport)
		{
			return false;
		}

		UClientPresentationSubsystem* Presentation = LocalPlayer->GetSubsystem<UClientPresentationSubsystem>();
		if (!Presentation)
		{
			return Fail(TEXT("共享挂载器缺失"));
		}

		Host = Presentation;
		if (Stage == 7)
		{
			return CheckDetachedAndRestore();
		}

		UTargetPreviewWidget* Preview = Cast<UTargetPreviewWidget>(Presentation->FindLayer(TEXT("TargetPreview")));
		UClientFeedbackWidget* Feedback = Cast<UClientFeedbackWidget>(Presentation->FindLayer(TEXT("Feedback")));
		if (!Preview || !Feedback || !HasArea(Preview->GetCachedGeometry()))
		{
			return false;
		}

		AHeroCharacter* Hero = Context.Hero.Get();
		UHeroAbilitySystemComponent* Abilities = Context.Abilities.Get();
		// 能力就绪不代表刚重建的Slate子控件已布局；死亡阶段仍继续观察清理，不等可用槽。
		if (Context.bReady && !AreSlotButtonsArranged(Preview, Context))
		{
			return false;
		}

		if (bLineCone)
		{
			return UpdateLineCone(Controller, Context, Preview, Feedback);
		}

		switch (Stage)
		{
		case 0:
			Test->TestEqual(TEXT("预览根仅允许子控件命中"), Preview->GetVisibility(), ESlateVisibility::SelfHitTestInvisible);
			Test->TestEqual(TEXT("反馈整层穿透"), Feedback->GetVisibility(), ESlateVisibility::HitTestInvisible);
			Test->TestTrue(TEXT("两个控件绑定实际本地Controller"), Preview->GetOwningPlayer() == Controller && Feedback->GetOwningPlayer() == Controller);
			OriginalCursor = FSlateApplication::Get().GetCursorPos();
			bSavedCursor = true;
			if (!IFileManager::Get().MakeDirectory(*Evidence, true))
			{
				return Fail(TEXT("无法创建本任务截图目录"));
			}

			Test->AddInfo(FString::Printf(TEXT("Rendered evidence=%s; Standalone only, no multiplayer proof"), *Evidence));
			PendingShot = FPaths::Combine(Evidence, TEXT("initial-widget-state.png"));
			FScreenshotRequest::RequestScreenshot(PendingShot, true, false);
			Stage = 10;
			return false;
		case 10:
			// 保留点击前的实际画面，也让首批动态按钮完成一次真正绘制／命中网格更新。
			if (FScreenshotRequest::IsScreenshotRequested() || !IFileManager::Get().FileExists(*PendingShot))
			{
				return false;
			}

			// 与用户首次进入游戏一致，先在非UI区域完成一次正常视口点击，不直接改捕获或焦点。
			if (!ClickViewport(Controller))
			{
				return Fail(TEXT("原生视口激活点击失败"));
			}

			Stage = 11;
			return false;
		case 11:
			Test->TestEqual(TEXT("视口左键激活不提交技能"), Controller->GetIntentResultSequence(), IntentBeforeSelection);
			Test->TestEqual(TEXT("视口左键激活不提交物品"), Controller->GetLastItemResult().Sequence, ItemBeforeSelection);
			Stage = 1;
			return false;
		case 1:
			IntentBeforeSelection = Controller->GetIntentResultSequence();
			ItemBeforeSelection = Controller->GetLastItemResult().Sequence;
			if (!ClickSlot(Preview, Context, MobaGameplayTags::Input_Slot_Primary))
			{
				return Fail(TEXT("无法通过原生Slate指针选择Primary槽"));
			}

			Stage = 2;
			return false;
		case 2:
			Test->TestEqual(TEXT("真实指针点击改变本地SelectedSlot"), Preview->GetSelectedSlot(), FGameplayTag(MobaGameplayTags::Input_Slot_Primary));
			Test->TestEqual(TEXT("本地选择不提交能力操作"), Controller->GetIntentResultSequence(), IntentBeforeSelection);
			Test->TestEqual(TEXT("本地选择不提交物品操作"), Controller->GetLastItemResult().Sequence, ItemBeforeSelection);
			if (!ClickSlot(Preview, Context, MobaGameplayTags::Input_Slot_Target))
			{
				return Fail(TEXT("无法通过指针选择Target槽"));
			}

			Stage = 3;
			return false;
		case 3:
		{
			const FGameplayAbilitySpec* Spec = Abilities->FindAbilitySpecFromHandle(Abilities->FindSlot(MobaGameplayTags::Input_Slot_Target));
			const UTargetDamageAbility* Ability = Spec ? Cast<UTargetDamageAbility>(Spec->Ability) : nullptr;
			const EAbilityTargetPolicy Expected = Variant == 0 ? EAbilityTargetPolicy::Direction : EAbilityTargetPolicy::Point;
			if (!Ability || Ability->GetGeometry().Policy != Expected)
			{
				return Fail(TEXT("不是既有Direction/Point配置样例"));
			}

			if (!Ability->CheckCooldown(Spec->Handle, Abilities->AbilityActorInfo.Get()))
			{
				return false;
			}

			Test->TestEqual(TEXT("目标槽选择生效"), Preview->GetSelectedSlot(), FGameplayTag(MobaGameplayTags::Input_Slot_Target));
			const FVector AimPoint = Hero->GetActorLocation() + FVector(150, 0, 0);
			FVector2D Screen;
			if (!Controller->ProjectWorldLocationToScreen(AimPoint, Screen, false))
			{
				return Fail(TEXT("实际视口无法投影目标点"));
			}

			Controller->SetMouseLocation(FMath::RoundToInt(Screen.X), FMath::RoundToInt(Screen.Y));
			FAbilityTargetIntent Intent;
			if (!Controller->BuildCursorIntent(Ability->GetGeometry(), Intent))
			{
				return Fail(TEXT("原鼠标意图构造失败"));
			}

			Test->TestTrue(TEXT("鼠标意图与投影点一致"), FVector::DistXY(Intent.Point, AimPoint) < 12);
			Press(Controller);
			Stage = 4;
			return false;
		}
		case 4:
			if (Controller->GetIntentResultSequence() == Ack)
			{
				return false;
			}

			Test->TestTrue(TEXT("原生R输入经正常权威路径接受"), Controller->WasLastIntentAccepted());
			PrimaryAfterAccepted = Hero->GetAttributes()->GetMana();
			Stage = 40;
			return false;
		case 40:
			// 让原生输入系统先处理上一帧Released，再产生第二次Started，避免同帧合并边沿。
			Press(Controller);
			Stage = 5;
			return false;
		case 5:
			if (Controller->GetIntentResultSequence() == Ack || Feedback->GetIntentText().IsEmpty())
			{
				return false;
			}

			Test->TestFalse(TEXT("正常输入在冷却中被拒绝"), Controller->WasLastIntentAccepted());
			Test->TestEqual(TEXT("真实拒绝原因Cooldown"), Controller->GetLastIntentReason(), FName(TEXT("Cooldown")));
			Test->TestEqual(TEXT("拒绝不再次扣资源"), Hero->GetAttributes()->GetMana(), PrimaryAfterAccepted);
			Test->TestFalse(TEXT("真实控件显示拒绝文字"), Feedback->GetIntentText().IsEmpty());
			if (FScreenshotRequest::IsScreenshotRequested())
			{
				return false;
			}

			PendingShot = FPaths::Combine(Evidence, Variant == 0 ? TEXT("direction-feedback.png") : TEXT("point-feedback.png"));
			FScreenshotRequest::RequestScreenshot(PendingShot, true, false);
			Stage = 6;
			return false;
		case 6:
			if (FScreenshotRequest::IsScreenshotRequested() || !IFileManager::Get().FileExists(*PendingShot))
			{
				return false;
			}

			if (Variant == 0)
			{
				const UExperienceDefinition* Experience = World->GetSubsystem<UExperienceSubsystem>()->GetExperience();
				if (!Experience || Experience->HeroVariants.Num() == 0 || !Experience->HeroVariants[0].IsValid()
					|| !Experience->HeroVariants[0]->AbilitySet.IsValid())
				{
					return Fail(TEXT("缺少已加载的既有Point变体"));
				}

				const FAbilitySourceResult Result = Abilities->ReplaceManagedSource(Hero->EntityId, Experience->HeroVariants[0]->AbilitySet.Get(), Abilities->GetStateRevision());
				if (Result.Status != EAbilitySourceResult::Applied)
				{
					return Fail(TEXT("单机夹具切换既有Point来源失败"));
				}

				Variant = 1;
				Stage = 60;
				return false;
			}

			PreviousLifeGeneration = Context.LifeGeneration;
			Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 0);
			if (!Hero->EnterDeath(Abilities->MakeEffectContext()))
			{
				return Fail(TEXT("单机夹具无法触发真实死亡生命周期"));
			}

			Stage = 70;
			return false;
		case 60:
			Test->TestFalse(TEXT("实际来源映射切换清选择"), Preview->GetSelectedSlot().IsValid());
			Test->TestEqual(TEXT("实际来源映射切换清旧轮廓"), Preview->GetOutlinePointCount(), 0);
			Stage = 1;
			return false;
		case 70:
			if (!Hero->IsAlive())
			{
				bObservedDeathCleanup = !Preview->GetSelectedSlot().IsValid() && Preview->GetOutlinePointCount() == 0
					&& Feedback->GetIntentText().IsEmpty() && Feedback->GetItemText().IsEmpty();
				return false;
			}

			if (!Context.bReady || Context.LifeGeneration <= PreviousLifeGeneration)
			{
				return false;
			}

			Test->TestTrue(TEXT("真实死亡期间观察到控件清理"), bObservedDeathCleanup);
			Test->TestFalse(TEXT("实际复活不恢复旧选择"), Preview->GetSelectedSlot().IsValid());
			if (!ClickSlot(Preview, Context, MobaGameplayTags::Input_Slot_Target))
			{
				return Fail(TEXT("复活后无法选择Target"));
			}

			PreviousProfile = uint8(Controller->GetActiveProfile());
			Press(Controller, EKeys::Tab);
			Stage = 71;
			return false;
		case 71:
			if (uint8(Controller->GetActiveProfile()) == PreviousProfile)
			{
				return false;
			}

			Test->TestFalse(TEXT("原生Tab切换Profile后清选择"), Preview->GetSelectedSlot().IsValid());
			Test->TestTrue(TEXT("Profile切换无旧拒绝提示"), Feedback->GetIntentText().IsEmpty() && Feedback->GetItemText().IsEmpty());
			if (!ClickSlot(Preview, Context, MobaGameplayTags::Input_Slot_Target)
				|| Abilities->RemoveManagedSource(Hero->EntityId, Abilities->GetStateRevision()).Status != EAbilitySourceResult::Applied)
			{
				return Fail(TEXT("单机夹具移除自身技能来源失败"));
			}

			Stage = 72;
			return false;
		case 72:
			Test->TestTrue(TEXT("实际空槽快照已到达"), Abilities->GetSlots().IsEmpty());
			Test->TestFalse(TEXT("实际槽缺失清选择"), Preview->GetSelectedSlot().IsValid());
			Test->TestEqual(TEXT("实际槽缺失清轮廓"), Preview->GetOutlinePointCount(), 0);
			if (!Hero->GetDefinition() || !Hero->GetDefinition()->AbilitySet.IsValid()
				|| Abilities->GrantManagedSource(Hero->GetDefinition()->AbilitySet.Get(), Hero->EntityId, Abilities->GetStateRevision()).Status != EAbilitySourceResult::Applied)
			{
				return Fail(TEXT("单机夹具恢复自身既有技能来源失败"));
			}

			Stage = 73;
			return false;
		case 73:
			if (!ClickSlot(Preview, Context, MobaGameplayTags::Input_Slot_Target))
			{
				return Fail(TEXT("来源恢复后无法选择Target"));
			}

			OldPreview = Preview;
			OldFeedback = Feedback;
			bLayersRemoved = true;
			if (!Presentation->UnregisterLayer(TEXT("TargetPreview")) || !Presentation->UnregisterLayer(TEXT("Feedback")))
			{
				return Fail(TEXT("C自身层注销失败"));
			}

			Stage = 7;
			return false;
		case 8:
			Test->TestTrue(TEXT("重挂创建新控件实例"), Preview != OldPreview.Get() && Feedback != OldFeedback.Get());
			Test->TestTrue(TEXT("新反馈不回放旧结果"), Feedback->GetIntentText().IsEmpty() && Feedback->GetItemText().IsEmpty());
			OldPreview = Preview;
			OldFeedback = Feedback;
			OldWorld = World;
			OldController = Controller;
			// 原生open触发真正World/Controller切换；本测试只允许独立单机进程。
			Controller->ConsoleCommand(TEXT("open /Engine/Maps/Entry?game=/Script/MobaGame.MobaGameMode"));
			Stage = 9;
			return false;
		case 9:
			if (World == OldWorld.Get() || Controller == OldController.Get())
			{
				return false;
			}

			Test->TestTrue(TEXT("真正travel后控件与Owner重建"), Preview != OldPreview.Get() && Feedback != OldFeedback.Get()
				&& Preview->GetOwningPlayer() == Controller && Feedback->GetOwningPlayer() == Controller);
			Test->TestTrue(TEXT("travel后不带入旧拒绝"), Feedback->GetIntentText().IsEmpty() && Feedback->GetItemText().IsEmpty());
			if (OldPreview.IsValid())
			{
				Test->TestFalse(TEXT("旧World预览选择已清空"), OldPreview->GetSelectedSlot().IsValid());
				Test->TestEqual(TEXT("旧World轮廓已清空"), OldPreview->GetOutlinePointCount(), 0);
			}

			return true;
		default:
			return Fail(TEXT("未知测试阶段"));
		}
	}

private:
	bool UpdateLineCone(AHeroPlayerController* Controller, const FClientTargetContext& Context,
		UTargetPreviewWidget* Preview, UClientFeedbackWidget* Feedback)
	{
		AHeroCharacter* Hero = Context.Hero.Get();
		UHeroAbilitySystemComponent* Abilities = Context.Abilities.Get();
		const UExperienceSubsystem* Experience = Hero->GetWorld()->GetSubsystem<UExperienceSubsystem>();
		const FGameplayTag Selected = Variant == 0 ? MobaGameplayTags::Input_Slot_Primary : MobaGameplayTags::Input_Slot_Target;
		const FGameplayAbilitySpec* Spec = Abilities->FindAbilitySpecFromHandle(Abilities->FindSlot(Selected));
		const UTargetDamageAbility* Ability = Spec ? Cast<UTargetDamageAbility>(Spec->Ability) : nullptr;
		const FString ExpectedClass = Variant == 0
			? TEXT("/MobaClassic/Tests/LineCone/Abilities/GA_LineInstant.GA_LineInstant_C")
			: TEXT("/MobaClassic/Tests/LineCone/Abilities/GA_ConeInstant.GA_ConeInstant_C");
		if (!Ability || Ability->GetClass()->GetPathName() != ExpectedClass)
		{
			return Fail(TEXT("当前真实槽不是已保存的Line/Cone样例GA"));
		}

		const FAbilityTargetGeometry Geometry = Ability->GetGeometry();
		switch (Stage)
		{
		case 0:
			if (!Experience || !Experience->GetExperience() || !Experience->GetMapInstance() || !Experience->GetMapInstance()->GetLoadedLevel()
				|| Experience->GetSnapshot().PlaylistId.ToString() != TEXT("Playlist:Playlist_LineCone") || !Experience->GetMap()
				|| Experience->GetMap()->World.ToSoftObjectPath().GetLongPackageName() != TEXT("/MobaClassic/Maps/L_FrameworkTest")
				|| !Experience->GetMapInstance()->IsLevelVisible() || !Hero->GetDefinition()
				|| Experience->GetExperience()->GetPathName() != TEXT("/MobaClassic/Definitions/LineCone/Experience_LineCone.Experience_LineCone")
				|| Hero->GetDefinition()->GetPathName() != TEXT("/MobaClassic/Definitions/LineCone/Hero_LineCone.Hero_LineCone")
				|| !Hero->GetDefinition()->AbilitySet.IsValid()
				|| Hero->GetDefinition()->AbilitySet->GetPathName() != TEXT("/MobaClassic/Definitions/LineCone/AbilitySet_LineCone.AbilitySet_LineCone"))
			{
				return Fail(TEXT("须由保存的Playlist_LineCone启动实际Experience/Hero/AbilitySet与可见地图"));
			}

			Test->TestEqual(TEXT("保存样例只有两个实际输入槽"), Abilities->GetSlots().Num(), 2);
			Test->TestTrue(TEXT("两个实际Widget属于本地Controller"), Preview->GetOwningPlayer() == Controller && Feedback->GetOwningPlayer() == Controller);
			OriginalCursor = FSlateApplication::Get().GetCursorPos();
			bSavedCursor = true;
			if (!IFileManager::Get().MakeDirectory(*Evidence, true))
			{
				return Fail(TEXT("无法创建Line/Cone独立截图目录"));
			}

			Test->AddInfo(FString::Printf(TEXT("LineCone World=%s MapInstance=%s Evidence=%s"), *Hero->GetWorld()->GetName(),
				*Experience->GetMapInstance()->GetPathName(), *Evidence));
			if (!ClickViewport(Controller))
			{
				return Fail(TEXT("Line/Cone原生视口激活点击失败"));
			}

			Stage = 99;
			return false;
		case 99:
			Test->TestEqual(TEXT("Line/Cone视口激活不提交技能"), Controller->GetIntentResultSequence(), IntentBeforeSelection);
			Test->TestEqual(TEXT("Line/Cone视口激活不提交物品"), Controller->GetLastItemResult().Sequence, ItemBeforeSelection);
			Stage = 100;
			return false;
		case 100:
			Test->TestEqual(TEXT("实际几何Policy"), Geometry.Policy, Variant == 0 ? EAbilityTargetPolicy::Line : EAbilityTargetPolicy::Cone);
			Test->TestEqual(TEXT("实际投送Instant"), Ability->Delivery, EAbilityDelivery::Instant);
			Test->TestFalse(TEXT("保存样例不是Legacy Unit范围"), Ability->bLegacyUnitRange);
			Test->TestEqual(TEXT("实际Range"), Geometry.Range, 900.f);
			Test->TestEqual(TEXT("实际Line半宽"), Geometry.HalfWidth, 75.f);
			Test->TestEqual(TEXT("实际Cone半角"), Geometry.HalfAngleDegrees, 30.f);
			Test->TestEqual(TEXT("实际高度容差"), Geometry.HeightTolerance, 200.f);
			Test->TestEqual(TEXT("实际目标上限"), Geometry.MaxTargets, 16);
			Test->TestTrue(TEXT("实际样例只允许Enemy"), Geometry.AllowedRelations.Num() == 1 && Geometry.AllowedRelations[0] == ETeamRelation::Enemy);
			if (!ClickSlot(Preview, Context, FGameplayTag()) || !AimShape(Controller, Hero, Geometry))
			{
				return Fail(TEXT("无法真实点击清除预览或取得场景鼠标意图"));
			}

			Stage = 101;
			return false;
		case 101:
			if (Preview->GetSelectedSlot().IsValid() || Preview->GetOutlinePointCount() != 0)
			{
				return Fail(TEXT("清除按钮没有清除实际预览"));
			}

			if (!CaptureShape(Controller, TEXT("clear"), BaselinePixels, BaselineSize))
			{
				return Test->HasAnyErrors();
			}

			BaselineShotOrigin = ShotOrigin;
			IntentBeforeSelection = Controller->GetIntentResultSequence();
			ItemBeforeSelection = Controller->GetLastItemResult().Sequence;
			if (!ClickSlot(Preview, Context, Selected) || !AimShape(Controller, Hero, Geometry))
			{
				return Fail(TEXT("无法真实选择样例槽并恢复场景鼠标位置"));
			}

			Stage = 102;
			return false;
		case 102:
		{
			Test->TestEqual(TEXT("选定真实样例槽"), Preview->GetSelectedSlot(), Selected);
			Test->TestEqual(TEXT("选择未提交能力操作"), Controller->GetIntentResultSequence(), IntentBeforeSelection);
			Test->TestEqual(TEXT("选择未提交物品操作"), Controller->GetLastItemResult().Sequence, ItemBeforeSelection);
			if (Preview->GetOutlinePointCount() != (Variant == 0 ? 5 : 35))
			{
				return false;
			}

			TArray<FColor> Pixels;
			FIntVector Size;
			if (!CaptureShape(Controller, TEXT("preview"), Pixels, Size))
			{
				return Test->HasAnyErrors();
			}

			if (Size != BaselineSize || !ShotOrigin.Equals(BaselineShotOrigin, 0.1)
				|| !CheckShapePixels(Controller, Hero->GetActorLocation(), Geometry, Pixels, Size))
			{
				return Fail(TEXT("真实截图中未观察到完整预期边界的新增轮廓像素"));
			}

			Stage = 103;
			return false;
		}
		case 103:
			if (!Ability->CheckCooldown(Spec->Handle, Abilities->AbilityActorInfo.Get()))
			{
				return false;
			}

			if (!Ability->CheckCost(Spec->Handle, Abilities->AbilityActorInfo.Get()))
			{
				return Fail(TEXT("保存样例的实际资源不足，测试不修改属性"));
			}

			ShapePrimaryBefore = Hero->GetAttributes()->GetMana();
			Press(Controller, Variant == 0 ? EKeys::Q : EKeys::R);
			Stage = 104;
			return false;
		case 104:
			if (Controller->GetIntentResultSequence() == Ack)
			{
				return false;
			}

			Test->TestTrue(TEXT("Line Q或Cone R沿原Controller路径被接受"), Controller->WasLastIntentAccepted());
			Test->TestTrue(TEXT("正常输入实际扣除样例成本"), Hero->GetAttributes()->GetMana() < ShapePrimaryBefore);
			PrimaryAfterAccepted = Hero->GetAttributes()->GetMana();
			Stage = 105;
			return false;
		case 105:
			Press(Controller, Variant == 0 ? EKeys::Q : EKeys::R);
			Stage = 106;
			return false;
		case 106:
		{
			if (Controller->GetIntentResultSequence() == Ack || Feedback->GetIntentText().IsEmpty())
			{
				return false;
			}

			Test->TestFalse(TEXT("样例重复按键真实拒绝"), Controller->WasLastIntentAccepted());
			Test->TestEqual(TEXT("样例重复输入Cooldown"), Controller->GetLastIntentReason(), FName(TEXT("Cooldown")));
			Test->TestEqual(TEXT("样例拒绝不再扣资源"), Hero->GetAttributes()->GetMana(), PrimaryAfterAccepted);
			TArray<FColor> Pixels;
			FIntVector Size;
			if (!CaptureShape(Controller, TEXT("feedback"), Pixels, Size))
			{
				return Test->HasAnyErrors();
			}

			if (Variant == 1)
			{
				return true;
			}

			Variant = 1;
			Stage = 100;
			return false;
		}
		default:
			return Fail(TEXT("未知Line/Cone阶段"));
		}
	}

	bool AimShape(AHeroPlayerController* Controller, AHeroCharacter* Hero, const FAbilityTargetGeometry& Geometry)
	{
		FVector2D Screen;
		const FVector Point = Hero->GetActorLocation() + FVector(0, 450, 0);
		if (!Controller->ProjectWorldLocationToScreen(Point, Screen, false))
		{
			return false;
		}

		Controller->SetMouseLocation(FMath::RoundToInt(Screen.X), FMath::RoundToInt(Screen.Y));
		FString Reason;
		return Controller->BuildCursorIntent(Geometry, ShapeIntent) && FVector::DistXY(ShapeIntent.Point, Point) < 12
			&& MobaTargeting::ValidateIntent(Hero->GetActorLocation(), Geometry, ShapeIntent, Reason);
	}

	bool CaptureShape(AHeroPlayerController* Controller, const TCHAR* Suffix, TArray<FColor>& Pixels, FIntVector& Size)
	{
		const TSharedPtr<SWindow> Window = Controller->GetWorld()->GetGameViewport()->GetWindow();
		if (!Window || !Window->IsVisible() || Window->IsWindowMinimized())
		{
			Test->AddError(TEXT("无法在不可见/最小化窗口取实际轮廓截图"));
			return false;
		}

		const FString Path = FPaths::Combine(Evidence, FString(Variant == 0 ? TEXT("line-") : TEXT("cone-")) + Suffix + TEXT(".png"));
		if (!ShapeScreenshotHandle.IsValid())
		{
			if (FScreenshotRequest::IsScreenshotRequested() || UGameViewportClient::OnScreenshotCaptured().IsBound())
			{
				Test->AddError(TEXT("已有其他截图请求/订阅，不能接管其输出"));
				return false;
			}

			PendingShot = Path;
			ShapeScreenshotController = Controller;
			ShotOrigin = Window->GetCachedGeometry().GetAbsolutePosition();
			CapturedShapePixels.Reset();
			// 在原生帧末读取UI截图；不在Automation Tick里对旧backbuffer再次绘制Slate。
			ShapeScreenshotHandle = UGameViewportClient::OnScreenshotCaptured().AddRaw(this, &FTargetPreviewLiveScenario::OnShapeScreenshot);
			FScreenshotRequest::RequestScreenshot(PendingShot, true, false);
			return false;
		}

		if (CapturedShapePixels.IsEmpty() || FScreenshotRequest::IsScreenshotRequested())
		{
			return false;
		}

		UGameViewportClient::OnScreenshotCaptured().Remove(ShapeScreenshotHandle);
		ShapeScreenshotHandle.Reset();
		if (PendingShot != Path || !ShotOrigin.Equals(Window->GetCachedGeometry().GetAbsolutePosition(), 0.1))
		{
			Test->AddError(TEXT("截图期间阶段或窗口位置改变，拒绝错配像素"));
			return false;
		}

		Pixels = MoveTemp(CapturedShapePixels);
		Size = CapturedShapeSize;
		return true;
	}

	void OnShapeScreenshot(int32 Width, int32 Height, const TArray<FColor>& Pixels)
	{
		if (Width <= 0 || Height <= 0 || int64(Pixels.Num()) != int64(Width) * Height)
		{
			Test->AddError(TEXT("原生截图回调尺寸/像素数不一致"));
			return;
		}

		CapturedShapePixels = Pixels;
		CapturedShapeSize = FIntVector(Width, Height, 0);
		// 先保留原图，再做坐标断言；失败也不能因delegate接管原生写盘而丢掉视觉首因。
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *PendingShot))
		{
			Test->AddError(TEXT("原生截图像素保存失败"));
			return;
		}

		Test->AddInfo(FString::Printf(TEXT("RHI screenshot=%s Size=%dx%d"), *PendingShot, Width, Height));
		AHeroPlayerController* Controller = ShapeScreenshotController.Get();
		UTargetPreviewWidget* Preview = Host.IsValid() ? Cast<UTargetPreviewWidget>(Host->FindLayer(TEXT("TargetPreview"))) : nullptr;
		const TSharedPtr<SWindow> Window = Controller ? Controller->GetWorld()->GetGameViewport()->GetWindow() : nullptr;
		FVector2D Screen, Desktop, OldLocal, PaintLocal;
		if (!Controller || !Controller->GetPawn() || !Preview || !Window
			|| !Controller->ProjectWorldLocationToScreen(Controller->GetPawn()->GetActorLocation(), Screen, false))
		{
			Test->AddError(TEXT("截图回调缺少当前真实视口/英雄/绘制geometry"));
			return;
		}

		const FGeometry& PaintGeometry = Preview->GetPaintSpaceGeometry();
		USlateBlueprintLibrary::ScreenToWidgetAbsolute(Controller, Screen, Desktop);
		USlateBlueprintLibrary::ScreenToWidgetLocal(Controller, PaintGeometry, Screen, OldLocal);
		USlateBlueprintLibrary::ScreenToWidgetLocal(Controller, PaintGeometry, Screen, PaintLocal, true);
		const FVector2D ExpectedWindow = Desktop - FVector2D(Window->GetPositionInScreen());
		const FVector2D ActualWindow = PaintGeometry.LocalToAbsolute(PaintLocal);
		const FVector2D OldWindow = PaintGeometry.LocalToAbsolute(OldLocal);
		Test->AddInfo(FString::Printf(TEXT("ShapeCoordinates Stage=%d WindowDesktop=%s TickAbs=%s PaintAbs=%s HeroScreen=%s ExpectedWindow=%s FixedWindow=%s PreviousWindow=%s"),
			Stage, *FVector2D(Window->GetPositionInScreen()).ToString(), *Preview->GetTickSpaceGeometry().GetAbsolutePosition().ToString(),
			*PaintGeometry.GetAbsolutePosition().ToString(), *Screen.ToString(), *ExpectedWindow.ToString(), *ActualWindow.ToString(), *OldWindow.ToString()));
		Test->TestTrue(TEXT("真实Paint geometry投影回到窗口像素且不重复桌面偏移"), ActualWindow.Equals(ExpectedWindow, 0.1));
	}

	bool CheckShapePixels(AHeroPlayerController* Controller, const FVector& Origin, const FAbilityTargetGeometry& Geometry,
		const TArray<FColor>& Pixels, const FIntVector& Size)
	{
		const FVector Direction = ShapeIntent.Direction.GetSafeNormal2D();
		TArray<TArray<FVector>> Edges;
		if (Variant == 0)
		{
			const FVector Side(-Direction.Y * Geometry.HalfWidth, Direction.X * Geometry.HalfWidth, 0);
			const FVector Corners[] = {Origin + Side, Origin + Direction * Geometry.Range + Side,
				Origin + Direction * Geometry.Range - Side, Origin - Side};
			for (int32 Edge = 0; Edge < 4; ++Edge)
			{
				TArray<FVector>& Samples = Edges.AddDefaulted_GetRef();
				for (int32 Sample = 1; Sample <= 3; ++Sample)
				{
					Samples.Add(FMath::Lerp(Corners[Edge], Corners[(Edge + 1) % 4], Sample * 0.25f));
				}
			}
		}
		else
		{
			for (float Sign : {-1.f, 1.f})
			{
				const FVector Ray = Direction.RotateAngleAxis(Sign * Geometry.HalfAngleDegrees, FVector::UpVector);
				TArray<FVector>& Samples = Edges.AddDefaulted_GetRef();
				for (int32 Sample = 1; Sample <= 3; ++Sample)
				{
					Samples.Add(Origin + Ray * Geometry.Range * (Sample * 0.25f));
				}
			}

			TArray<FVector>& Arc = Edges.AddDefaulted_GetRef();
			for (int32 Sample = 1; Sample <= 7; ++Sample)
			{
				const float Angle = FMath::Lerp(-Geometry.HalfAngleDegrees, Geometry.HalfAngleDegrees, Sample / 8.f);
				Arc.Add(Origin + Direction.RotateAngleAxis(Angle, FVector::UpVector) * Geometry.Range);
			}
		}

		bool bAllEdges = true;
		for (int32 Edge = 0; Edge < Edges.Num(); ++Edge)
		{
			int32 Hits = 0;
			for (const FVector& Point : Edges[Edge])
			{
				FVector2D Screen, Absolute;
				if (!Controller->ProjectWorldLocationToScreen(Point, Screen, false))
				{
					continue;
				}

				USlateBlueprintLibrary::ScreenToWidgetAbsolute(Controller, Screen, Absolute);
				const FIntPoint Center(FMath::RoundToInt(Absolute.X - ShotOrigin.X), FMath::RoundToInt(Absolute.Y - ShotOrigin.Y));
				bool bFound = false;
				for (int32 Y = Center.Y - 4; Y <= Center.Y + 4; ++Y)
				{
					for (int32 X = Center.X - 4; X <= Center.X + 4; ++X)
					{
						if (X < 0 || Y < 0 || X >= Size.X || Y >= Size.Y)
						{
							continue;
						}

						const FColor Color = Pixels[Y * Size.X + X];
						const FColor Before = BaselinePixels[Y * Size.X + X];
						const int32 Delta = FMath::Abs(int32(Color.R) - Before.R) + FMath::Abs(int32(Color.G) - Before.G) + FMath::Abs(int32(Color.B) - Before.B);
						bFound |= Color.G > 140 && Color.B > 170 && int32(Color.B) > int32(Color.R) + 50 && Delta > 60;
					}
				}

				Hits += bFound ? 1 : 0;
			}

			const int32 Required = Edges[Edge].Num() == 7 ? 5 : 2;
			Test->AddInfo(FString::Printf(TEXT("ShapePixels Shape=%d Edge=%d Observed=%d/%d Required=%d"), Variant, Edge, Hits, Edges[Edge].Num(), Required));
			bAllEdges &= Hits >= Required;
		}

		return bAllEdges;
	}


	UWorld* FindStandaloneWorld() const
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (UWorld* World = Context.World(); World && World->WorldType == EWorldType::Game && World->GetNetMode() == NM_Standalone && !World->bIsTearingDown)
			{
				return World;
			}
		}

		return nullptr;
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

	bool AreSlotButtonsArranged(UTargetPreviewWidget* Preview, const FClientTargetContext& Context)
	{
		TArray<TSharedPtr<SButton>> Buttons;
		if (Preview->GetCachedWidget())
		{
			CollectButtons(Preview->GetCachedWidget().ToSharedRef(), Buttons);
		}

		const int32 Expected = MobaClientTarget::ReadSlots(Context).Num() + 1;
		const bool bArranged = Buttons.Num() == Expected && !Buttons.ContainsByPredicate(
			[this](const TSharedPtr<SButton>& Button) { return !Button || !HasArea(Button->GetCachedGeometry()); });
		if (!bArranged && LayoutWaitStage != Stage)
		{
			LayoutWaitStage = Stage;
			Test->AddInfo(FString::Printf(TEXT("WaitingActualSlotLayout Stage=%d Buttons=%d Expected=%d"), Stage, Buttons.Num(), Expected));
		}

		return bArranged;
	}

	bool ClickViewport(AHeroPlayerController* Controller)
	{
		const TSharedPtr<SViewport> Viewport = Controller->GetWorld()->GetGameViewport()->GetGameViewportWidget();
		if (!Viewport)
		{
			return false;
		}

		FSlateApplication& Slate = FSlateApplication::Get();
		const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Viewport.ToSharedRef());
		const FVector2D Position = Viewport->GetCachedGeometry().LocalToAbsolute(Viewport->GetCachedGeometry().GetLocalSize() * FVector2D(0.5, 0.65));
		const TSharedPtr<SWidget> Captor = Slate.GetCursorUser()->GetCursorCaptor();
		Test->AddInfo(FString::Printf(TEXT("ViewportActivation CaptorBefore=%s FakeTouch=%d"), Captor ? *Captor->GetTypeAsString() : TEXT("None"), Slate.IsFakingTouchEvents()));
		IntentBeforeSelection = Controller->GetIntentResultSequence();
		ItemBeforeSelection = Controller->GetLastItemResult().Sequence;
		const FVector2D Previous = Slate.GetCursorPos();
		Slate.SetCursorPos(Position);
		const TSet<FKey> Down = {EKeys::LeftMouseButton};
		const TSet<FKey> Up;
		Slate.ProcessMouseMoveEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			Position, Previous, Up, EKeys::Invalid, 0, FModifierKeysState()));
		Slate.ProcessMouseButtonDownEvent(Window ? Window->GetNativeWindow() : nullptr,
			FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex, Position, Position, Down, EKeys::LeftMouseButton, 0, FModifierKeysState()));
		Slate.ProcessMouseButtonUpEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			Position, Position, Up, EKeys::LeftMouseButton, 0, FModifierKeysState()));
		const TSharedPtr<SWidget> CaptorAfter = Slate.GetCursorUser()->GetCursorCaptor();
		Test->AddInfo(FString::Printf(TEXT("ViewportActivation CaptorAfter=%s"), CaptorAfter ? *CaptorAfter->GetTypeAsString() : TEXT("None")));
		return true;
	}

	bool ClickSlot(UTargetPreviewWidget* Preview, const FClientTargetContext& Context, FGameplayTag DesiredSlot)
	{
		const TArray<FTargetPreviewSlot> Slots = MobaClientTarget::ReadSlots(Context);
		const int32 Index = DesiredSlot.IsValid()
			? Slots.IndexOfByPredicate([DesiredSlot](const FTargetPreviewSlot& Entry) { return Entry.Slot == DesiredSlot; }) : Slots.Num();
		TArray<TSharedPtr<SButton>> Buttons;
		if (!Preview->GetCachedWidget())
		{
			return false;
		}

		CollectButtons(Preview->GetCachedWidget().ToSharedRef(), Buttons);
		if (!Buttons.IsValidIndex(Index) || (DesiredSlot.IsValid() && (!Slots.IsValidIndex(Index) || !Slots[Index].bEnabled)))
		{
			return false;
		}

		const TSharedPtr<SButton> Button = Buttons[Index];
		const FGeometry& Geometry = Button->GetCachedGeometry();
		if (!HasArea(Geometry))
		{
			return false;
		}

		FSlateApplication& Slate = FSlateApplication::Get();
		const TSharedPtr<SWidget> FocusBefore = Slate.GetKeyboardFocusedWidget();
		const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Button.ToSharedRef());
		const FVector2D Position = Geometry.LocalToAbsolute(Geometry.GetLocalSize() * 0.5f);
		const FVector2D PreviousPosition = Slate.GetCursorPos();
		Slate.SetCursorPos(Position);
		const TSet<FKey> DownKeys = {EKeys::LeftMouseButton};
		const TSet<FKey> UpKeys;
		const FWidgetPath HitPath = Slate.LocateWindowUnderMouse(Position, Slate.GetInteractiveTopLevelWindows(), false, Slate.GetUserIndexForMouse());
		FString HitTypes;
		bool bHitTarget = false;
		for (int32 HitIndex = 0; HitIndex < HitPath.Widgets.Num(); ++HitIndex)
		{
			const FArrangedWidget& Hit = HitPath.Widgets[HitIndex];
			HitTypes += Hit.Widget->GetTypeAsString() + TEXT("/");
			bHitTarget |= Hit.Widget == Button;
		}

		Test->AddInfo(FString::Printf(TEXT("PointerProbe Slot=%s Position=%s Size=%s WindowVisible=%d WindowMinimized=%d HitTarget=%d Path=%s"),
			*DesiredSlot.ToString(), *Position.ToString(), *Geometry.GetLocalSize().ToString(), Window && Window->IsVisible(),
			Window && Window->IsWindowMinimized(), bHitTarget, *HitTypes));
		// 真实鼠标先移动建立Hover；SButton的DownAndUp点击要求释放时仍处于Hover。
		Slate.ProcessMouseMoveEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			Position, PreviousPosition, UpKeys, EKeys::Invalid, 0, FModifierKeysState()));
		Test->TestTrue(TEXT("原生鼠标移动命中所选按钮"), Button->IsHovered());
		Slate.ProcessMouseButtonDownEvent(Window ? Window->GetNativeWindow() : nullptr,
			FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
				Position, Position, DownKeys, EKeys::LeftMouseButton, 0, FModifierKeysState()));
		Slate.ProcessMouseButtonUpEvent(FPointerEvent(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex,
			Position, Position, UpKeys, EKeys::LeftMouseButton, 0, FModifierKeysState()));
		Test->TestTrue(TEXT("选择按钮没有转移键盘焦点"), Slate.GetKeyboardFocusedWidget() == FocusBefore);
		return true;
	}

	void Press(AHeroPlayerController* Controller, FKey Key = EKeys::R)
	{
		Ack = Controller->GetIntentResultSequence();
		KeyController = Controller;
		HeldKey = Key;
		Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, HeldKey, IE_Pressed, FPlatformTime::Cycles64()));
		bKeyHeld = true;
	}

	void ReleaseKey()
	{
		if (bKeyHeld && KeyController.IsValid())
		{
			KeyController->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, HeldKey, IE_Released, FPlatformTime::Cycles64()));
		}

		bKeyHeld = false;
	}

	bool CheckDetachedAndRestore()
	{
		if (OldPreview.IsValid())
		{
			Test->TestFalse(TEXT("真实卸载清选择"), OldPreview->GetSelectedSlot().IsValid());
			Test->TestEqual(TEXT("真实卸载清轮廓"), OldPreview->GetOutlinePointCount(), 0);
		}

		if (OldFeedback.IsValid())
		{
			Test->TestTrue(TEXT("真实卸载清反馈"), OldFeedback->GetIntentText().IsEmpty() && OldFeedback->GetItemText().IsEmpty());
		}

		RestoreLayers();
		Stage = 8;
		return false;
	}

	void RestoreLayers()
	{
		if (bLayersRemoved && Host.IsValid())
		{
			const bool bPreview = Host->RegisterLayer(TEXT("TargetPreview"), UTargetPreviewWidget::StaticClass(), 0, true);
			const bool bFeedback = Host->RegisterLayer(TEXT("Feedback"), UClientFeedbackWidget::StaticClass(), 20);
			Test->TestTrue(TEXT("测试恢复自身层登记"), bPreview && bFeedback);
			bLayersRemoved = false;
		}
	}

	bool Fail(const TCHAR* Reason)
	{
		Test->AddError(FString::Printf(TEXT("TargetPreviewLive Stage=%d Variant=%d Reason=%s"), Stage, Variant, Reason));
		ReleaseKey();
		RestoreLayers();
		return true;
	}


private:
	FAutomationTestBase* Test;
	double Started;
	FString Evidence;
	FString PendingShot;
	FVector2D OriginalCursor = FVector2D::ZeroVector;
	TWeakObjectPtr<UClientPresentationSubsystem> Host;
	TWeakObjectPtr<UTargetPreviewWidget> OldPreview;
	TWeakObjectPtr<UClientFeedbackWidget> OldFeedback;
	TWeakObjectPtr<AHeroPlayerController> OldController;
	TWeakObjectPtr<AHeroPlayerController> KeyController;
	TWeakObjectPtr<UWorld> OldWorld;
	TWeakObjectPtr<UWorld> AdmissionWorld;
	uint32 Ack = 0;
	uint32 IntentBeforeSelection = 0;
	uint32 ItemBeforeSelection = 0;
	uint32 PreviousLifeGeneration = 0;
	uint8 PreviousProfile = 0;
	FKey HeldKey = EKeys::R;
	float PrimaryAfterAccepted = 0;
	int32 Stage = 0;
	int32 Variant = 0;
	int32 LayoutWaitStage = INDEX_NONE;
	bool bSavedCursor = false;
	bool bKeyHeld = false;
	bool bLayersRemoved = false;
	bool bObservedDeathCleanup = false;
	bool bLineCone = false;
	FAbilityTargetIntent ShapeIntent;
	TArray<FColor> BaselinePixels;
	FIntVector BaselineSize = FIntVector::ZeroValue;
	FVector2D ShotOrigin = FVector2D::ZeroVector;
	FVector2D BaselineShotOrigin = FVector2D::ZeroVector;
	FDelegateHandle ShapeScreenshotHandle;
	TWeakObjectPtr<AHeroPlayerController> ShapeScreenshotController;
	TArray<FColor> CapturedShapePixels;
	FIntVector CapturedShapeSize = FIntVector::ZeroValue;
	float ShapePrimaryBefore = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetPreviewLiveTest, "Moba.UI.TargetPreview.RenderedInputAndLifecycle",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FTargetPreviewLiveTest::RunTest(const FString& Parameters)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("MobaTargetPreviewFixture")) || !FParse::Param(FCommandLine::Get(), TEXT("MobaManualAdmission"))
		|| FParse::Param(FCommandLine::Get(), TEXT("NullRHI"))
		|| !FSlateApplication::IsInitialized())
	{
		AddError(TEXT("仅允许显式MobaTargetPreviewFixture、MobaManualAdmission且带渲染的独立单机测试进程"));
		return false;
	}

	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FTargetPreviewLiveScenario>(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetPreviewLineConeLiveTest, "Moba.UI.TargetPreview.RenderedLineConeInput",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FTargetPreviewLineConeLiveTest::RunTest(const FString& Parameters)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("MobaTargetPreviewLineConeFixture"))
		|| !FParse::Param(FCommandLine::Get(), TEXT("MobaManualAdmission")) || FParse::Param(FCommandLine::Get(), TEXT("NullRHI"))
		|| !FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Line/Cone用例仅允许显式LineConeFixture、ManualAdmission且带RHI的独立单机进程"));
		return false;
	}

	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FTargetPreviewLiveScenario>(this, true));
	return true;
}

#endif
