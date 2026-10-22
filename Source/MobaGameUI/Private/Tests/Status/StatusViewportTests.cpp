#include "Status/StatusPanelWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ClientPresentationSubsystem.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "Items/ItemInventoryComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace
{
	class FStatusViewportScenario final : public IAutomationLatentCommand
	{
	public:
		explicit FStatusViewportScenario(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
		{
			FParse::Value(FCommandLine::Get(), TEXT("MobaStatusEvidence="), Evidence);
		}

		virtual bool Update() override
		{
			if (Evidence.IsEmpty() || FPlatformTime::Seconds() - Started > 140)
			{
				Test->AddError(FString::Printf(TEXT("Status客户端缺证据目录或谓词超时 Phase=%d"), Phase));
				return true;
			}

			AHeroPlayerController* Controller = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (UWorld* World = Context.World(); World && World->IsGameWorld() && World->GetNetMode() == NM_Client)
				{
					for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
					{
						if (auto* Candidate = Cast<AHeroPlayerController>(It->Get()); Candidate && Candidate->GetLocalPlayer())
						{
							Controller = Candidate;
							break;
						}
					}
				}
			}

			if (!Controller)
			{
				return false;
			}

			auto* Presentation = Controller->GetLocalPlayer()->GetSubsystem<UClientPresentationSubsystem>();
			auto* Widget = Presentation ? Cast<UStatusPanelWidget>(Presentation->FindLayer(TEXT("Status"))) : nullptr;
			if (!Widget)
			{
				return false;
			}

			const FStatusPanelSnapshot State = Widget->GetPresentedStatus();
			if (Phase == 0)
			{
				FString Entities;
				if (!FFileHelper::LoadFileToString(Entities, *(Evidence / TEXT("seeded.txt")))
					|| !State.bCombatReady || !State.bAbilitiesReady || !State.bInventoryReady || State.InventoryRevision == 0)
				{
					return false;
				}

				int32 OtherHeroes = 0;
				for (TActorIterator<AHeroCharacter> It(Controller->GetWorld()); It; ++It)
				{
					if (It->EntityId != State.Entity && Entities.Contains(It->EntityId.ToString(EGuidFormats::Digits)))
					{
						++OtherHeroes;
						// 仅测试检查本客户端复制副本为空；生产Widget从不查询此Hero。
						Test->TestTrue(TEXT("观察端没有服务器已购的另一Hero私有库存"), It->GetInventory()->GetSnapshot().Items.IsEmpty());
					}
				}

				if (OtherHeroes == 0)
				{
					return false;
				}

				Entity = State.Entity;
				Epoch = State.Epoch;
				LifeGeneration = State.LifeGeneration;
				InventoryRevision = State.InventoryRevision;
				Test->TestTrue(TEXT("面板挂在本地玩家视口"), Widget->IsInViewport() && Widget->GetOwningPlayer() == Controller);
				Test->TestEqual(TEXT("Status整棵穿透"), Widget->GetVisibility(), ESlateVisibility::HitTestInvisible);
				Test->TestFalse(TEXT("Status不抢键盘焦点"), Widget->IsFocusable());
				CheckInventory(State);
				Acknowledge(TEXT("seeded"));
				Phase = 1;
			}

			if (Phase == 1 && State.bCombatReady && State.LifeState == EHeroLifeState::Dead)
			{
				Test->TestEqual(TEXT("复制死亡HP"), State.Health, 0.f);
				CheckInventory(State);
				Acknowledge(TEXT("dead"));
				Phase = 2;
			}

			if (Phase == 2 && State.bCombatReady && State.LifeState == EHeroLifeState::Alive && State.LifeGeneration > LifeGeneration)
			{
				Test->TestEqual(TEXT("复活保留实体"), State.Entity, Entity);
				CheckInventory(State);
				Acknowledge(TEXT("respawn"));
				Phase = 3;
			}

			if (Phase == 3 && IFileManager::Get().FileExists(*(Evidence / TEXT("unbound.txt"))) && !State.bOwnedHero)
			{
				Test->TestFalse(TEXT("解绑清私有区块"), State.bInventoryReady);
				Test->TestTrue(TEXT("解绑清技能引用值"), State.Abilities.IsEmpty());
				Test->TestEqual(TEXT("解绑清旧金币"), State.Gold, 0);
				Acknowledge(TEXT("unbound"));
				Phase = 4;
			}

			if (Phase == 4 && State.bCombatReady && State.bInventoryReady && State.Epoch > Epoch)
			{
				const FStatusPanelVisualMetrics Visual = Widget->GetVisualMetrics();
				if (Visual.HostSize.X <= 0 || Visual.PanelSize.X <= 0 || Visual.BodyLineHeight <= 0)
				{
					return false;
				}
				// 等待连续两次真实Paint几何一致；不以固定延迟或Tick缓存冒充稳定绘制。
				const bool bStable = bHasPreviousVisual && Visual.PanelSize.Equals(PreviousVisual.PanelSize, .25)
					&& Visual.PanelOffset.Equals(PreviousVisual.PanelOffset, .25)
					&& FMath::IsNearlyEqual(Visual.BodyLineHeight, PreviousVisual.BodyLineHeight, .25f);
				PreviousVisual = Visual;
				bHasPreviousVisual = true;
				if (!bStable)
				{
					return false;
				}

				Test->TestEqual(TEXT("新连接代次重新读取同一实体"), State.Entity, Entity);
				CheckInventory(State);
				const FVector2D ViewportSize = UWidgetLayoutLibrary::GetViewportSize(Controller);
				const float ViewportDPI = UWidgetLayoutLibrary::GetViewportScale(Controller);
				Test->TestTrue(TEXT("1280视口正文实际行高至少16像素"), Visual.BodyLineHeight >= 16);
				Test->TestTrue(TEXT("六槽物品完整文本期望尺寸在实际分配区域内"), Visual.bItemTextFits);
				Test->TestTrue(TEXT("面板在玩家区域内无裁切"), Visual.PanelOffset.X >= -1 && Visual.PanelOffset.Y >= -1
					&& Visual.PanelOffset.X + Visual.PanelSize.X <= Visual.HostSize.X + 1
					&& Visual.PanelOffset.Y + Visual.PanelSize.Y <= Visual.HostSize.Y + 1);
				const FString Metrics = FString::Printf(TEXT("GeometrySource=PaintSpace\nViewport=%.1fx%.1f\nViewportDPI=%.4f\nHostScale=%.4f\nReadabilityScale=%.4f\nHostPixels=%.1fx%.1f\nPanelOffset=%.1f,%.1f\nPanelPixels=%.1fx%.1f\nBodyLinePixels=%.1f\nItemTextFits=%d\n"),
					ViewportSize.X, ViewportSize.Y, ViewportDPI, Visual.HostScale, Visual.ReadabilityScale,
					Visual.HostSize.X, Visual.HostSize.Y, Visual.PanelOffset.X, Visual.PanelOffset.Y, Visual.PanelSize.X, Visual.PanelSize.Y, Visual.BodyLineHeight, Visual.bItemTextFits ? 1 : 0);
				Test->TestTrue(TEXT("保存实际视口DPI与布局证据"), FFileHelper::SaveStringToFile(Metrics,
					*(Evidence / (TEXT("viewport-") + Entity.ToString(EGuidFormats::Digits) + TEXT(".metrics.txt")))));
				Screenshot = Evidence / (TEXT("viewport-") + Entity.ToString(EGuidFormats::Digits) + TEXT(".png"));
				FScreenshotRequest::RequestScreenshot(Screenshot, true, false);
				Phase = 5;
			}

			if (Phase == 5 && IFileManager::Get().FileSize(*Screenshot) > 0)
			{
				Acknowledge(TEXT("rebound"));
				Phase = 6;
			}

			return Phase == 6 && IFileManager::Get().FileExists(*(Evidence / TEXT("finished.txt")));
		}

	private:
		void CheckInventory(const FStatusPanelSnapshot& State)
		{
			Test->TestTrue(TEXT("生命周期事件后快照完整"), State.bInventoryReady);
			Test->TestEqual(TEXT("真实购买金币1000-100"), State.Gold, 900);
			Test->TestEqual(TEXT("库存revision没有伪造推进"), State.InventoryRevision, InventoryRevision);
			Test->TestEqual(TEXT("真实第一槽物品"), State.Items[0].ItemId, FName(TEXT("ArmorPlate")));
			Test->TestEqual(TEXT("真实数量"), State.Items[0].Quantity, 1);
			for (int32 Slot = 1; Slot < 6; ++Slot)
			{
				Test->TestFalse(TEXT("其他五槽为空"), State.Items[Slot].Instance.IsValid());
			}
		}

		void Acknowledge(const TCHAR* Stage) const
		{
			const FString File = FString(Stage) + TEXT("-") + Entity.ToString(EGuidFormats::Digits) + TEXT(".txt");
			FFileHelper::SaveStringToFile(TEXT("observed"), *(Evidence / File));
		}

		FAutomationTestBase* Test;
		double Started;
		FString Evidence;
		FString Screenshot;
		FGuid Entity;
		uint32 Epoch = 0;
		uint32 LifeGeneration = 0;
		uint32 InventoryRevision = 0;
		FStatusPanelVisualMetrics PreviousVisual;
		bool bHasPreviousVisual = false;
		int32 Phase = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStatusViewportTest, "Moba.UI.Status.LiveViewport", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStatusViewportTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FStatusViewportScenario(this));
	return true;
}

#endif
