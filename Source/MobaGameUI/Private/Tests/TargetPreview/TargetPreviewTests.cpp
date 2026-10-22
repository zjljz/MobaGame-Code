#include "TargetPreview/TargetPreviewModel.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Combat/TargetDamageAbility.h"
#include "Feedback/ClientFeedbackWidget.h"
#include "GameplayAbilitySpec.h"
#include "Misc/AutomationTest.h"
#include "MobaGameplayTags.h"
#include "TargetPreview/TargetPreviewWidget.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetPreviewGeometryTest, "Moba.UI.TargetPreview.Geometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FTargetPreviewGeometryTest::RunTest(const FString& Parameters)
{
	FAbilityTargetGeometry Geometry;
	Geometry.Policy = EAbilityTargetPolicy::Line;
	Geometry.Range = 100;
	Geometry.HalfWidth = 20;
	FAbilityTargetIntent Intent;
	Intent.Policy = Geometry.Policy;
	Intent.Direction = FVector(2, 0, 0);
	const FVector Origin(10, 30, 70);
	TArray<FVector> Outline;
	TestTrue(TEXT("Line有效"), MobaTargetPreview::BuildOutline(Origin, Geometry, Intent, Outline));
	TestEqual(TEXT("四边闭合五点"), Outline.Num(), 5);
	if (Outline.Num() != 5)
	{
		return false;
	}

	TestTrue(TEXT("起点左角不向后伸展"), Outline[0].Equals(FVector(10, 50, 70)));
	TestTrue(TEXT("长度100半宽20"), Outline[2].Equals(FVector(110, 10, 70)));
	TestTrue(TEXT("首尾闭合"), Outline[0].Equals(Outline.Last()));
	TestTrue(TEXT("服务器包含矩形边角"), MobaTargeting::Contains(Origin, Outline[2], Geometry, Intent));
	TestFalse(TEXT("服务器拒绝后方"), MobaTargeting::Contains(Origin, FVector(9, 30, 70), Geometry, Intent));
	TestFalse(TEXT("服务器拒绝边外"), MobaTargeting::Contains(Origin, FVector(60, 50.1, 70), Geometry, Intent));
	Intent.Direction = FVector(0, 3, 0);
	TestTrue(TEXT("旋转方向有效"), MobaTargetPreview::BuildOutline(Origin, Geometry, Intent, Outline));
	if (Outline.Num() != 5)
	{
		return false;
	}

	TestTrue(TEXT("旋转终点外角"), Outline[2].Equals(FVector(30, 130, 70)));

	Geometry.Policy = Intent.Policy = EAbilityTargetPolicy::Cone;
	Geometry.HalfAngleDegrees = 60;
	Intent.Direction = FVector::ForwardVector;
	TestTrue(TEXT("Cone有效"), MobaTargetPreview::BuildOutline(Origin, Geometry, Intent, Outline));
	TestEqual(TEXT("32段弧加两条半径"), Outline.Num(), 35);
	if (Outline.Num() != 35)
	{
		return false;
	}

	TestTrue(TEXT("扇形顶点与闭合点都是原点"), Outline[0].Equals(Origin) && Outline.Last().Equals(Origin));
	TestTrue(TEXT("半角60而非总角60"), Outline[1].Equals(Origin + FVector(50, -86.602540378, 0), 0.001));
	TestTrue(TEXT("轴向弧中点射程100"), Outline[17].Equals(Origin + FVector(100, 0, 0), 0.001));
	TestTrue(TEXT("服务器接纳59度内点"), MobaTargeting::Contains(Origin, Origin + FVector::ForwardVector.RotateAngleAxis(59, FVector::UpVector) * 99, Geometry, Intent));
	TestFalse(TEXT("服务器拒绝61度外点"), MobaTargeting::Contains(Origin, Origin + FVector::ForwardVector.RotateAngleAxis(61, FVector::UpVector) * 99, Geometry, Intent));
	TestFalse(TEXT("服务器拒绝半径外"), MobaTargeting::Contains(Origin, Origin + FVector(101, 0, 0), Geometry, Intent));

	Intent.Direction = FVector::ZeroVector;
	TestFalse(TEXT("零方向不保留旧轮廓"), MobaTargetPreview::BuildOutline(Origin, Geometry, Intent, Outline));
	TestTrue(TEXT("失败清空输出"), Outline.IsEmpty());
	Intent.Direction = FVector::ForwardVector;
	Geometry.Range = std::numeric_limits<float>::quiet_NaN();
	Outline.Add(Origin);
	TestFalse(TEXT("NaN拒绝"), MobaTargetPreview::BuildOutline(Origin, Geometry, Intent, Outline));
	TestTrue(TEXT("NaN也清空输出"), Outline.IsEmpty());
	Geometry.Range = 100;
	Geometry.Policy = Intent.Policy = EAbilityTargetPolicy::Direction;
	TestFalse(TEXT("不重造Direction箭头"), MobaTargetPreview::BuildOutline(Origin, Geometry, Intent, Outline));
	Geometry.Policy = Intent.Policy = EAbilityTargetPolicy::Point;
	Intent.Point = Origin;
	TestFalse(TEXT("不重造Point圆"), MobaTargetPreview::BuildOutline(Origin, Geometry, Intent, Outline));

	// 修改瞬态GA对象，不修改共享CDO或资产；确认实际展示来源为GetGeometry。
	UTargetDamageAbility* Ability = NewObject<UTargetDamageAbility>();
	Ability->Range = 450;
	Ability->Targeting.Range = 900;
	TestEqual(TEXT("GetGeometry使用GA实际Range"), Ability->GetGeometry().Range, 450.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetPreviewSelectionTest, "Moba.UI.TargetPreview.SelectionLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FTargetPreviewSelectionTest::RunTest(const FString& Parameters)
{
	FClientTargetContext Context;
	Context.bValid = true;
	Context.bAlive = true;
	Context.Epoch = 3;
	Context.EntityId = FGuid(1, 2, 3, 4);
	Context.LoadoutRevision = 1;
	FTargetPreviewSlot Target;
	Target.Slot = MobaGameplayTags::Input_Slot_Target;
	Target.Handle = FGameplayAbilitySpec(UTargetDamageAbility::StaticClass(), 1).Handle;
	Target.bEnabled = true;
	FTargetPreviewSlot Primary;
	Primary.Slot = MobaGameplayTags::Input_Slot_Primary;
	Primary.Handle = FGameplayAbilitySpec(UTargetDamageAbility::StaticClass(), 1).Handle;
	Primary.bEnabled = true;
	TArray<FTargetPreviewSlot> Slots = {Target, Primary};
	FTargetPreviewSelection Selection;
	Selection.Update(Context, Slots);
	TestFalse(TEXT("复制未就绪不选择"), Selection.GetSelectedSlot().IsValid());
	Context.bReady = true;
	Selection.Update(Context, Slots);
	TestEqual(TEXT("初次就绪选择已有Target"), Selection.GetSelectedSlot(), Target.Slot);
	TestTrue(TEXT("可切到Primary"), Selection.Select(Primary.Slot));
	Selection.Update(Context, Slots);
	TestEqual(TEXT("无变化保留选择"), Selection.GetSelectedSlot(), Primary.Slot);
	++Context.LoadoutRevision;
	Selection.Update(Context, Slots);
	TestFalse(TEXT("映射版本变化清选择且不自动恢复"), Selection.GetSelectedSlot().IsValid());
	Selection.Select(Target.Slot);
	Slots[0].bEnabled = false;
	Selection.Update(Context, Slots);
	TestFalse(TEXT("禁用槽清选择"), Selection.GetSelectedSlot().IsValid());
	TestFalse(TEXT("不能选择禁用槽"), Selection.Select(Target.Slot));
	Slots[0].bEnabled = true;
	Selection.Update(Context, Slots);
	Selection.Select(Target.Slot);
	Slots.RemoveAt(0);
	Selection.Update(Context, Slots);
	TestFalse(TEXT("槽缺失清选择"), Selection.GetSelectedSlot().IsValid());
	Selection.Select(Primary.Slot);
	++Context.Profile;
	Selection.Update(Context, Slots);
	TestFalse(TEXT("Profile变化清选择"), Selection.GetSelectedSlot().IsValid());
	Selection.Select(Primary.Slot);
	Context.bAlive = false;
	++Context.DeathCount;
	Selection.Update(Context, Slots);
	TestFalse(TEXT("死亡清选择"), Selection.GetSelectedSlot().IsValid());
	TestTrue(TEXT("死亡清可选槽副本"), Selection.GetSlots().IsEmpty());
	Context.bAlive = true;
	++Context.LifeGeneration;
	Selection.Update(Context, Slots);
	TestFalse(TEXT("复活不恢复旧选择"), Selection.GetSelectedSlot().IsValid());
	Selection.Select(Primary.Slot);
	Context.bReady = false;
	Selection.Update(Context, Slots);
	TestFalse(TEXT("输入Profile切换pending或同步间隙清理"), Selection.GetSelectedSlot().IsValid());
	Selection.Update({}, {});
	TestTrue(TEXT("解绑无旧槽"), Selection.GetSlots().IsEmpty());
	Context.bReady = true;
	++Context.Epoch;
	Slots.Add(Target);
	Selection.Update(Context, Slots);
	TestEqual(TEXT("新绑定独立初始选择"), Selection.GetSelectedSlot(), Target.Slot);
	Selection.Reset();
	TestFalse(TEXT("销毁清选择"), Selection.GetSelectedSlot().IsValid());
	TestTrue(TEXT("销毁清快照"), Selection.GetSlots().IsEmpty());
	TestFalse(TEXT("Preview不取得键盘焦点"), GetDefault<UTargetPreviewWidget>()->IsFocusable());
	TestFalse(TEXT("Feedback不取得键盘焦点"), GetDefault<UClientFeedbackWidget>()->IsFocusable());
	TestFalse(TEXT("空Controller不产生绑定"), MobaClientTarget::ReadContext(nullptr).bValid);
	return true;
}

#endif
