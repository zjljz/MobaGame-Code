#include "Match/MatchGameMode.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Tests/Session/AdmissionTestController.h"
#include "Experience/ExperienceDefinitions.h"
#include "Experience/ExperienceSubsystem.h"
#include "Match/MatchGameState.h"
#include "Match/ParticipantState.h"
#include "Hero/HeroCharacter.h"
#include "Engine/Engine.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAdmissionStartupTest, "Moba.Session.AdmissionStartup", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAdmissionStartupTest::RunTest(const FString& Parameters)
{
	const UWorld::InitializationValues Values = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AdmissionWorld"), CreatePackage(TEXT("/Temp/AdmissionStartup")), true, ERHIFeatureLevel::Num, &Values);
	if (!TestNotNull(TEXT("创建准入测试 World"), World))
	{
		return false;
	}

	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	// 原生 ProcessEvent 只在 ActorsInitialized 后执行 Controller RPC；不启动比赛 BeginPlay。
	World->InitializeActorsForPlay(FURL());
	TestTrue(TEXT("准入 World 已完成原生 Actor 初始化"), World->AreActorsInitialized());
	AMatchGameMode* Mode = World->SpawnActor<AMatchGameMode>();
	AMatchGameState* State = World->SpawnActor<AMatchGameState>();
	Mode->GameState = State;
	World->SetGameState(State);
	State->MatchId = FGuid::NewGuid();
	State->ExperienceInstance = FGuid::NewGuid();
	UExperienceSubsystem* Experience = World->GetSubsystem<UExperienceSubsystem>();
	FExperienceSnapshot& Snapshot = const_cast<FExperienceSnapshot&>(Experience->GetSnapshot());

	// 独立控制内容状态，覆盖加载未完成时的服务器入口；不预热或修改真实资产。
	UExperienceDefinition* Definition = NewObject<UExperienceDefinition>(Experience);
	Definition->Players.MaximumParticipants = 1;
	Definition->Teams.Teams = {0};
	FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("Experience"))->SetObjectPropertyValue_InContainer(Experience, Definition);
	UAdmissionTestStreaming* Map = NewObject<UAdmissionTestStreaming>(World);
	Map->BindFixtureLevel(World->PersistentLevel);
	FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("MapInstance"))->SetObjectPropertyValue_InContainer(Experience, Map);

	auto CreateConnection = [&]()
	{
		AAdmissionTestController* Controller = World->SpawnActor<AAdmissionTestController>();
		Controller->SetPlayerState(World->SpawnActor<AParticipantState>());
		Mode->PendingConnections.Add(Controller, FPlatformTime::Seconds() + 75.0);
		return Controller;
	};

	AAdmissionTestController* First = CreateConnection();
	// 模拟75秒握手窗口已等待68秒，Ready不能重新获得完整65秒。
	Mode->PendingConnections.FindChecked(First) = FPlatformTime::Seconds() + 7.0;
	const double FirstDeadline = Mode->PendingConnections.FindChecked(First);
	Snapshot.State = EExperienceState::Loading;
	Mode->BeginAdmission(First, false, FResumeCredential());
	TestEqual(TEXT("早到明确等待内容，不是假满员"), First->GetAdmissionResult(), FName(TEXT("ContentNotReady")));
	TestEqual(TEXT("等待不发放凭证"), First->CredentialCount, 0);
	TestEqual(TEXT("一条连接只排队一次"), Mode->ContentPendingConnections.Num(), 1);
	Mode->BeginAdmission(First, true, FResumeCredential());
	TestEqual(TEXT("重复 RPC 不替换首次意图或增加队列"), Mode->ContentPendingConnections.Num(), 1);
	TestEqual(TEXT("重复请求不延长原始截止"), Mode->PendingConnections.FindChecked(First), FirstDeadline);

	Snapshot.State = EExperienceState::Activating;
	AAdmissionTestController* Second = CreateConnection();
	Mode->BeginAdmission(Second, false, FResumeCredential());
	Mode->Tick(0);
	TestEqual(TEXT("Activating 继续等待，不提前发放凭证"), First->CredentialCount + Second->CredentialCount, 0);
	TestEqual(TEXT("两条不同连接各保留一个等待请求"), Mode->ContentPendingConnections.Num(), 2);

	Snapshot.State = EExperienceState::ExperienceReady;
	AAdmissionTestController* ReadyArrival = CreateConnection();
	Mode->BeginAdmission(ReadyArrival, false, FResumeCredential());
	TestEqual(TEXT("Ready 到 Tick 之间新请求仍加入队尾"), Mode->ContentPendingConnections.Num(), 3);
	TestEqual(TEXT("新请求不能抢先发凭证"), ReadyArrival->CredentialCount, 0);
	Mode->Tick(0);
	TestEqual(TEXT("Ready 后新到者不能插队抢最后席位"), ReadyArrival->GetAdmissionResult(), FName(TEXT("NoSeat")));
	TestTrue(TEXT("Ready 自动继续原准入，无需客户端再发握手"), First->GetAdmissionRequest().IsValid());
	TestTrue(TEXT("成功预留没有错误原因"), First->GetAdmissionResult().IsNone());
	TestEqual(TEXT("只发放一次凭证"), First->CredentialCount, 1);
	TestEqual(TEXT("Ready 后退出等待队列"), Mode->ContentPendingConnections.Num(), 0);
	TestEqual(TEXT("创建唯一席位"), Mode->Seats.Num(), 1);
	TestEqual(TEXT("FIFO 后到等待者按实际容量拒绝"), Second->GetAdmissionResult(), FName(TEXT("NoSeat")));
	TestEqual(TEXT("后到满员者未发凭证"), Second->CredentialCount, 0);
	if (!Mode->Seats.IsEmpty())
	{
		TestEqual(TEXT("晚到 Ready 保留原截止，不从此刻重新计65秒"), Mode->Seats[0].Deadline, FirstDeadline);
	}

	const FGuid ReservedRequest = First->GetAdmissionRequest();
	const double ReservedDeadline = Mode->PendingConnections.FindChecked(First);
	Mode->BeginAdmission(First, false, FResumeCredential());
	TestEqual(TEXT("重复已预留请求不再发凭证"), First->CredentialCount, 1);
	TestEqual(TEXT("真实拥有端回调保留原有效 request"), First->GetAdmissionRequest(), ReservedRequest);
	TestTrue(TEXT("重复请求不把拥有端准入状态改为失败"), First->GetAdmissionResult().IsNone());
	TestEqual(TEXT("重复预留不延长截止"), Mode->PendingConnections.FindChecked(First), ReservedDeadline);
	TestEqual(TEXT("重复预留不增加席位"), Mode->Seats.Num(), 1);
	AAdmissionTestController* Full = CreateConnection();
	Mode->BeginAdmission(Full, false, FResumeCredential());
	TestEqual(TEXT("真实容量不足仍为终止性 NoSeat"), Full->GetAdmissionResult(), FName(TEXT("NoSeat")));
	TestEqual(TEXT("满员不排队重试"), Mode->ContentPendingConnections.Num(), 0);

	Mode->ReleaseAdmission(First, TEXT("FixtureReleased"));
	AAdmissionTestController* Normal = CreateConnection();
	const double NormalOriginalDeadline = Mode->PendingConnections.FindChecked(Normal);
	const double BeforeReserve = FPlatformTime::Seconds();
	Mode->BeginAdmission(Normal, false, FResumeCredential());
	const double AfterReserve = FPlatformTime::Seconds();
	const double NormalReservedDeadline = Mode->PendingConnections.FindChecked(Normal);
	TestTrue(TEXT("正常首次预留使用至多65秒窗口"), NormalReservedDeadline >= BeforeReserve + 65.0 && NormalReservedDeadline <= AfterReserve + 65.0);
	TestTrue(TEXT("正常预留收紧原75秒截止"), NormalReservedDeadline < NormalOriginalDeadline);
	Mode->ReleaseAdmission(Normal, TEXT("FixtureReleased"));

	AAdmissionTestController* Reentrant = CreateConnection();
	Reentrant->ReleaseOnReservation = Mode;
	Mode->BeginAdmission(Reentrant, false, FResumeCredential());
	TestEqual(TEXT("拥有端结果回调可同步释放失败席位"), Reentrant->ReentrantReleaseCount, 1);
	TestEqual(TEXT("回调释放后没有残留席位"), Mode->Seats.Num(), 0);
	TestFalse(TEXT("同步失败的真实客户端回调清除 request"), Reentrant->GetAdmissionRequest().IsValid());
	TestEqual(TEXT("同步失败保留实际原因"), Reentrant->GetAdmissionResult(), FName(TEXT("FixtureReentrantRelease")));

	// 已提交席位的恢复重试复用保留对象，只改变本连接的预留；不需要模拟完整战斗。
	AParticipantState* RetainedParticipant = World->SpawnActor<AParticipantState>();
	RetainedParticipant->ParticipantId = FGuid::NewGuid();
	RetainedParticipant->MatchId = State->MatchId;
	RetainedParticipant->bConnected = false;
	RetainedParticipant->PrimaryHero = World->SpawnActor<AHeroCharacter>();
	Mode->Participants.Add(RetainedParticipant);
	FResumeCredential ResumeCredentialFixture;
	ResumeCredentialFixture.Match = State->MatchId;
	ResumeCredentialFixture.Participant = RetainedParticipant->ParticipantId;
	AMatchGameMode::FSeat RetainedSeat;
	RetainedSeat.Participant = RetainedParticipant;
	RetainedSeat.bCommitted = true;
	if (TestTrue(TEXT("建立测试恢复凭证与摘要"), ResumeCredential::Generate(ResumeCredentialFixture.Secret)
		&& ResumeCredential::Digest(ResumeCredentialFixture.Secret, RetainedSeat.Digest)))
	{
		Mode->Seats.Add(MoveTemp(RetainedSeat));
		AAdmissionTestController* Resuming = CreateConnection();
		Mode->BeginAdmission(Resuming, true, ResumeCredentialFixture);
		const FGuid FirstResumeRequest = Resuming->GetAdmissionRequest();
		const double ResumeDeadline = Mode->PendingConnections.FindChecked(Resuming);
		TestTrue(TEXT("已有席位恢复取得有效 request"), FirstResumeRequest.IsValid());
		Mode->ReleaseAdmission(Resuming, TEXT("ContentFailed"));
		TestTrue(TEXT("失败释放保留已提交席位"), Mode->Seats.Num() == 1 && Mode->Seats[0].bCommitted);
		Mode->BeginAdmission(Resuming, true, ResumeCredentialFixture);
		TestTrue(TEXT("同连接恢复重试分配新的有效 request"), Resuming->GetAdmissionRequest().IsValid() && Resuming->GetAdmissionRequest() != FirstResumeRequest);
		TestEqual(TEXT("恢复释放再预留不能续期"), Mode->PendingConnections.FindChecked(Resuming), ResumeDeadline);
		TestEqual(TEXT("恢复不重新发放凭证"), Resuming->CredentialCount, 0);
		Mode->ReleaseAdmission(Resuming, TEXT("FixtureReleased"));
	}
	ResumeCredentialFixture.Clear();

	for (EExperienceState TerminalState : {EExperienceState::Stopping, EExperienceState::Unloaded})
	{
		Snapshot.State = TerminalState;
		AAdmissionTestController* Terminal = CreateConnection();
		Mode->BeginAdmission(Terminal, false, FResumeCredential());
		TestEqual(TEXT("停止或未加载不是可等待状态"), Terminal->GetAdmissionResult(), FName(TEXT("ContentFailed")));
		TestEqual(TEXT("终止状态不排队"), Mode->ContentPendingConnections.Num(), 0);
	}

	Snapshot.State = EExperienceState::ExperienceReady;
	FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("Experience"))->SetObjectPropertyValue_InContainer(Experience, nullptr);
	AAdmissionTestController* MissingDefinition = CreateConnection();
	Mode->BeginAdmission(MissingDefinition, false, FResumeCredential());
	TestEqual(TEXT("Ready 但定义缺失仍失败"), MissingDefinition->GetAdmissionResult(), FName(TEXT("ContentFailed")));
	FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("Experience"))->SetObjectPropertyValue_InContainer(Experience, Definition);
	FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("MapInstance"))->SetObjectPropertyValue_InContainer(Experience, nullptr);
	AAdmissionTestController* MissingMap = CreateConnection();
	Mode->BeginAdmission(MissingMap, false, FResumeCredential());
	TestEqual(TEXT("Ready 但地图实例缺失仍失败"), MissingMap->GetAdmissionResult(), FName(TEXT("ContentFailed")));
	FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("MapInstance"))->SetObjectPropertyValue_InContainer(Experience, Map);
	Map->BindFixtureLevel(nullptr);
	AAdmissionTestController* MissingLevel = CreateConnection();
	Mode->BeginAdmission(MissingLevel, false, FResumeCredential());
	TestEqual(TEXT("Ready 但内容关卡缺失仍失败"), MissingLevel->GetAdmissionResult(), FName(TEXT("ContentFailed")));
	Map->BindFixtureLevel(World->PersistentLevel);

	Snapshot.State = EExperienceState::Loading;
	AAdmissionTestController* Failed = CreateConnection();
	Mode->BeginAdmission(Failed, false, FResumeCredential());
	Snapshot.State = EExperienceState::Failed;
	Mode->Tick(0);
	TestEqual(TEXT("内容加载失败明确结束等待"), Failed->GetAdmissionResult(), FName(TEXT("ContentFailed")));
	TestEqual(TEXT("失败清空待内容队列"), Mode->ContentPendingConnections.Num(), 0);
	TestEqual(TEXT("失败没有发凭证"), Failed->CredentialCount, 0);

	Snapshot.State = EExperienceState::Loading;
	AAdmissionTestController* Disconnected = CreateConnection();
	Mode->BeginAdmission(Disconnected, false, FResumeCredential());
	Mode->ReleaseAdmission(Disconnected, TEXT("Disconnected"));
	TestFalse(TEXT("断开删除待准入连接"), Mode->PendingConnections.Contains(Disconnected));
	TestEqual(TEXT("断开删除待内容请求"), Mode->ContentPendingConnections.Num(), 0);

	AAdmissionTestController* Expired = CreateConnection();
	Mode->BeginAdmission(Expired, false, FResumeCredential());
	Mode->PendingConnections.FindChecked(Expired) = FPlatformTime::Seconds() - 1.0;
	Mode->Tick(0);
	TestFalse(TEXT("等待超时清理连接记录"), Mode->PendingConnections.Contains(Expired));
	TestEqual(TEXT("等待超时清理队列"), Mode->ContentPendingConnections.Num(), 0);
	TestEqual(TEXT("等待超时明确原因"), Expired->GetAdmissionResult(), FName(TEXT("Timeout")));

	AAdmissionTestController* Destroyed = CreateConnection();
	Mode->BeginAdmission(Destroyed, false, FResumeCredential());
	const TWeakObjectPtr<AHeroPlayerController> DestroyedConnection = Destroyed;
	Destroyed->Destroy();
	Mode->Tick(0);
	TestFalse(TEXT("无效弱连接不遗留待准入记录"), Mode->PendingConnections.Contains(DestroyedConnection));
	TestEqual(TEXT("无效弱连接不遗留内容等待"), Mode->ContentPendingConnections.Num(), 0);

	AAdmissionTestController* Ending = CreateConnection();
	Mode->BeginAdmission(Ending, false, FResumeCredential());
	Mode->FinishMatch();
	TestEqual(TEXT("比赛结束清理待内容队列"), Mode->ContentPendingConnections.Num(), 0);
	TestEqual(TEXT("比赛结束不遗留待准入连接"), Mode->PendingConnections.Num(), 0);
	TestEqual(TEXT("比赛结束明确拒绝等待者"), Ending->GetAdmissionResult(), FName(TEXT("MatchEnded")));

	// 瞬态 fixture 借用了宿主 PersistentLevel；先解除借用再让真实 World 清理。
	Map->BindFixtureLevel(nullptr);
	FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("MapInstance"))->SetObjectPropertyValue_InContainer(Experience, nullptr);
	FindFProperty<FObjectPropertyBase>(UExperienceSubsystem::StaticClass(), TEXT("Experience"))->SetObjectPropertyValue_InContainer(Experience, nullptr);
	Snapshot.State = EExperienceState::Unloaded;
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

#endif
