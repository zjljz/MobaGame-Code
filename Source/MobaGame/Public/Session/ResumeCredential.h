#pragma once

#include "CoreMinimal.h"
#include "ResumeCredential.generated.h"

// 只通过拥有连接的握手RPC传递；不是复制状态、URL参数或日志载荷。
USTRUCT()
struct MOBAGAME_API FResumeCredential
{
	GENERATED_BODY()

public:
	// 票据清理
	void Clear();


public:
	bool IsValid() const { return Match.IsValid() && Participant.IsValid() && Secret.Num() == 32; }


public:
	// 凭证对应的比赛身份 ID，不是比赛对象。
	UPROPERTY()
	FGuid Match;

	// 凭证对应的参赛者身份 ID，不是 PlayerState 对象。
	UPROPERTY()
	FGuid Participant;

	UPROPERTY()
	TArray<uint8> Secret;
};

namespace ResumeCredential
{
	MOBAGAME_API bool Generate(TArray<uint8>& Secret);

	MOBAGAME_API bool Digest(const TArray<uint8>& Secret, TArray<uint8>& Hash);

	MOBAGAME_API bool Matches(const TArray<uint8>& Secret, const TArray<uint8>& Hash);

	MOBAGAME_API bool Protect(const FResumeCredential& Credential, TArray<uint8>& OutProtectedCredentialBytes);

	MOBAGAME_API bool Unprotect(const TArray<uint8>& ProtectedCredentialBytes, FResumeCredential& Credential);
}

