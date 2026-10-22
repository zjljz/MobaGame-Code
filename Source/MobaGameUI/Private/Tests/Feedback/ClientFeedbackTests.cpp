#include "Feedback/ClientFeedbackModel.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClientFeedbackResultsTest, "Moba.UI.Feedback.Results",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClientFeedbackResultsTest::RunTest(const FString& Parameters)
{
	FClientTargetContext Context;
	Context.bValid = true;
	Context.bReady = true;
	Context.bAlive = true;
	Context.Epoch = 7;
	Context.EntityId = FGuid(1, 0, 0, 1);
	FItemResult Item;
	Item.Epoch = 7;
	Item.Sequence = 10;
	Item.Reason = TEXT("InsufficientGold");
	FClientFeedbackModel Model;
	Model.Update(Context, 20, false, TEXT("Cost"), Item, 0);
	TestTrue(TEXT("绑定不重放旧意图拒绝"), Model.GetIntentText().IsEmpty());
	TestTrue(TEXT("绑定不重放旧物品拒绝"), Model.GetItemText().IsEmpty());
	Item.Sequence = 11;
	Model.Update(Context, 21, false, TEXT("Cooldown"), Item, 1);
	TestFalse(TEXT("新意图拒绝可见"), Model.GetIntentText().IsEmpty());
	TestFalse(TEXT("新物品拒绝可见"), Model.GetItemText().IsEmpty());
	const FString FirstIntent = Model.GetIntentText().ToString();
	Item.Sequence = 10;
	Model.Update(Context, 20, false, TEXT("Cost"), Item, 2);
	TestEqual(TEXT("旧意图不覆盖"), Model.GetIntentText().ToString(), FirstIntent);
	TestEqual(TEXT("旧物品不倒退高水位"), Model.GetItemHighWater(), uint32(11));
	Item.Sequence = 11;
	Model.Update(Context, 21, false, TEXT("Cooldown"), Item, 3.9);
	Model.Update(Context, 21, false, TEXT("Cooldown"), Item, 4.1);
	TestTrue(TEXT("重复意图不延长提示"), Model.GetIntentText().IsEmpty());
	TestTrue(TEXT("重复物品不延长提示"), Model.GetItemText().IsEmpty());
	Item.Sequence = 12;
	Model.Update(Context, 22, false, NAME_None, Item, 5);
	TestFalse(TEXT("空原因仍有兜底反馈"), Model.GetIntentText().IsEmpty());
	Item.Sequence = 13;
	Item.bAccepted = true;
	Model.Update(Context, 23, true, NAME_None, Item, 6);
	TestTrue(TEXT("成功清旧意图拒绝"), Model.GetIntentText().IsEmpty());
	TestTrue(TEXT("成功清旧物品拒绝"), Model.GetItemText().IsEmpty());
	TestEqual(TEXT("成功也消费序号"), Model.GetIntentHighWater(), uint32(23));
	TestEqual(TEXT("未知原因与空原因均保守兜底"), MobaClientFeedback::DescribeReason(TEXT("UnpublishedReason")).ToString(), MobaClientFeedback::DescribeReason(NAME_None).ToString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClientFeedbackEpochTest, "Moba.UI.Feedback.EpochAndLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FClientFeedbackEpochTest::RunTest(const FString& Parameters)
{
	FClientTargetContext Context;
	Context.bValid = true;
	Context.bAlive = true;
	Context.bReady = true;
	Context.Epoch = 2;
	FClientFeedbackModel Model;
	FItemResult Item;
	Item.Epoch = 1;
	Item.Sequence = 999;
	Item.Reason = TEXT("InventoryFull");
	Model.Update(Context, MAX_uint32 - 1, false, TEXT("Cost"), Item, 0);
	Model.Update(Context, MAX_uint32, false, TEXT("Cost"), Item, 1);
	TestTrue(TEXT("旧Epoch拒绝不显示"), Model.GetItemText().IsEmpty());
	TestEqual(TEXT("旧Epoch不污染高水位"), Model.GetItemHighWater(), uint32(0));
	Item.Epoch = 2;
	Item.Sequence = 1;
	Model.Update(Context, 0, false, TEXT("Cooldown"), Item, 2);
	TestEqual(TEXT("能力uint32回绕接受0"), Model.GetIntentHighWater(), uint32(0));
	TestFalse(TEXT("当前Epoch从自身序号接收"), Model.GetItemText().IsEmpty());
	++Context.Profile;
	Model.Update(Context, 0, false, TEXT("Cooldown"), Item, 2.1);
	TestTrue(TEXT("Profile变化清旧意图"), Model.GetIntentText().IsEmpty());
	TestTrue(TEXT("Profile变化清旧物品"), Model.GetItemText().IsEmpty());
	Item.Sequence = 2;
	Model.Update(Context, 1, false, TEXT("Cost"), Item, 3);
	Context.bAlive = false;
	++Context.DeathCount;
	Model.Update(Context, 1, false, TEXT("Cost"), Item, 3.1);
	TestTrue(TEXT("死亡清旧反馈"), Model.GetIntentText().IsEmpty() && Model.GetItemText().IsEmpty());
	Model.Update(Context, 2, false, TEXT("LifeState"), Item, 3.2);
	TestFalse(TEXT("同绑定死亡时的新真实拒绝仍可显示"), Model.GetIntentText().IsEmpty());
	++Context.LoadoutRevision;
	Model.Update(Context, 2, false, TEXT("LifeState"), Item, 3.3);
	TestTrue(TEXT("槽映射变化清反馈"), Model.GetIntentText().IsEmpty());
	++Context.Epoch;
	Model.Update(Context, 3, false, TEXT("Cost"), Item, 4);
	TestTrue(TEXT("新绑定基线不重播"), Model.GetIntentText().IsEmpty());
	TestEqual(TEXT("新Epoch不继承旧物品高水位"), Model.GetItemHighWater(), uint32(0));
	Item.Epoch = Context.Epoch;
	Item.Sequence = 1;
	Model.Update(Context, 4, false, TEXT("Cost"), Item, 5);
	TestFalse(TEXT("相同序号但新Epoch可显示"), Model.GetItemText().IsEmpty());
	Model.Update({}, 4, false, TEXT("Cost"), Item, 5.1);
	TestTrue(TEXT("解绑清两通道"), Model.GetIntentText().IsEmpty() && Model.GetItemText().IsEmpty());
	Model.Update(Context, 4, false, TEXT("Cost"), Item, 6);
	TestTrue(TEXT("重新绑定不回放残存结果"), Model.GetIntentText().IsEmpty() && Model.GetItemText().IsEmpty());
	Model.Reset();
	TestEqual(TEXT("最终销毁清序号"), Model.GetIntentHighWater(), uint32(0));
	return true;
}

#endif
