#include "Session/ResumeCredential.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Session/MobaSessionSubsystem.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Engine/GameInstance.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResumeProtectionTest, "Moba.Session.CredentialProtection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FResumeProtectionTest::RunTest(const FString& Parameters)
{
	FResumeCredential Original, Restored;
	Original.Match = FGuid::NewGuid();
	Original.Participant = FGuid::NewGuid();
	TArray<uint8> Hash, Protected, Other;
	TestTrue(TEXT("平台安全随机"), ResumeCredential::Generate(Original.Secret));
	TestTrue(TEXT("第二次平台安全随机"), ResumeCredential::Generate(Other));
	TestFalse(TEXT("两次独立随机值"), Original.Secret == Other);
	TestTrue(TEXT("平台SHA256"), ResumeCredential::Digest(Original.Secret, Hash));
	TestTrue(TEXT("正确票据通过"), ResumeCredential::Matches(Original.Secret, Hash));
	TestFalse(TEXT("错误票据拒绝"), ResumeCredential::Matches(Other, Hash));
	TestTrue(TEXT("用户级DPAPI保护"), ResumeCredential::Protect(Original, Protected));
	TestTrue(TEXT("DPAPI恢复"), ResumeCredential::Unprotect(Protected, Restored));
	TestEqual(TEXT("Match关联保留"), Restored.Match, Original.Match);
	TestEqual(TEXT("Participant关联保留"), Restored.Participant, Original.Participant);
	TestTrue(TEXT("票据完整性"), Restored.Secret == Original.Secret);
	Protected.Last() ^= 0x80;
	TestFalse(TEXT("损坏blob不能恢复"), ResumeCredential::Unprotect(Protected, Restored));
	TestFalse(TEXT("失败清除输出"), Restored.IsValid());
	TestFalse(TEXT("空凭证不能保护"), ResumeCredential::Protect(Restored, Protected));
	auto* Store = NewObject<UMobaSessionSubsystem>(NewObject<UGameInstance>());
	if (FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Store->GetCredentialPath()), FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("AI")))))
	{
		TestTrue(TEXT("保护文件落盘"), Store->SaveCredential(Original));
		TestTrue(TEXT("磁盘恢复"), Store->LoadCredential(Restored));
		TestTrue(TEXT("磁盘恢复同票据"), Restored.Secret == Original.Secret);
#if PLATFORM_WINDOWS
		const FString Path = FPaths::ConvertRelativePathToFull(Store->GetCredentialPath());
		HANDLE Lock = CreateFileW(*Path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		TestTrue(TEXT("保护旧文件的测试锁"), Lock != INVALID_HANDLE_VALUE);
		FResumeCredential Replacement = Original;
		Replacement.Secret = Other;
		if (Lock != INVALID_HANDLE_VALUE)
		{
			TestFalse(TEXT("原子替换受阻报告失败"), Store->SaveCredential(Replacement));
			CloseHandle(Lock);
			TestTrue(TEXT("失败后原文件仍能恢复"), Store->LoadCredential(Restored));
			TestTrue(TEXT("保存失败保留旧票据"), Restored.Secret == Original.Secret);
		}

		Replacement.Clear();
#endif
	}
	else
	{
		AddError(TEXT("持久化测试必须使用本卡AI下独立UserDir"));
	}

	Original.Clear();
	Restored.Clear();
	return true;
}
#endif
