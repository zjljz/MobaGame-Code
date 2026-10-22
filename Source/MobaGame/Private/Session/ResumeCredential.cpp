#include "Session/ResumeCredential.h"

#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <bcrypt.h>
#include <dpapi.h>
THIRD_PARTY_INCLUDES_START
#define UI UI_ST
#include <openssl/crypto.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#endif

void FResumeCredential::Clear()
{
	if (!Secret.IsEmpty())
	{
		FMemory::Memzero(Secret.GetData(), Secret.Num());
	}

	Secret.Reset();
	Match.Invalidate();
	Participant.Invalidate();
}

bool ResumeCredential::Generate(TArray<uint8>& Secret)
{
	Secret.SetNumZeroed(32);
#if PLATFORM_WINDOWS
	if (BCryptGenRandom(nullptr, Secret.GetData(), Secret.Num(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0)
	{
		return true;
	}
#endif
	FMemory::Memzero(Secret.GetData(), Secret.Num());
	Secret.Reset();
	return false;
}

bool ResumeCredential::Digest(const TArray<uint8>& Secret, TArray<uint8>& Hash)
{
	Hash.Reset();
	if (Secret.Num() != 32)
	{
		return false;
	}
#if PLATFORM_WINDOWS
	Hash.SetNumZeroed(32);
	if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(Secret.GetData()), Secret.Num(), Hash.GetData(), Hash.Num()) >= 0)
	{
		return true;
	}
#endif
	Hash.Reset();
	return false;
}

bool ResumeCredential::Matches(const TArray<uint8>& Secret, const TArray<uint8>& Hash)
{
	TArray<uint8> CandidateHash;
	if (Hash.Num() != 32 || !Digest(Secret, CandidateHash))
	{
		return false;
	}
#if PLATFORM_WINDOWS
	return CRYPTO_memcmp(CandidateHash.GetData(), Hash.GetData(), 32) == 0;
#else
	return false;
#endif
}

bool ResumeCredential::Protect(const FResumeCredential& Credential, TArray<uint8>& OutProtectedCredentialBytes)
{
	OutProtectedCredentialBytes.Reset();
	if (!Credential.IsValid())
	{
		return false;
	}
#if PLATFORM_WINDOWS
	TArray<uint8> PlainCredentialBytes;
	FMemoryWriter Writer(PlainCredentialBytes);
	uint32 CredentialFormatVersion = 1;
	FGuid MatchId = Credential.Match, ParticipantId = Credential.Participant;
	Writer << CredentialFormatVersion << MatchId << ParticipantId;
	Writer.Serialize(const_cast<uint8*>(Credential.Secret.GetData()), 32);
	DATA_BLOB InputBlob{static_cast<DWORD>(PlainCredentialBytes.Num()), PlainCredentialBytes.GetData()}, OutputBlob{};
	const bool bSuccess = CryptProtectData(&InputBlob, L"MobaGame resume v1", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &OutputBlob) != 0;
	FMemory::Memzero(PlainCredentialBytes.GetData(), PlainCredentialBytes.Num());
	if (!bSuccess)
	{
		return false;
	}

	OutProtectedCredentialBytes.Append(OutputBlob.pbData, OutputBlob.cbData);
	LocalFree(OutputBlob.pbData);
	return true;
#else
	return false;
#endif
}

bool ResumeCredential::Unprotect(const TArray<uint8>& ProtectedCredentialBytes, FResumeCredential& Credential)
{
	Credential.Clear();
	if (ProtectedCredentialBytes.IsEmpty() || ProtectedCredentialBytes.Num() > 4096)
	{
		return false;
	}
#if PLATFORM_WINDOWS
	DATA_BLOB InputBlob{static_cast<DWORD>(ProtectedCredentialBytes.Num()), const_cast<BYTE*>(ProtectedCredentialBytes.GetData())}, OutputBlob{};
	if (!CryptUnprotectData(&InputBlob, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &OutputBlob))
	{
		return false;
	}

	bool bValid = false;
	if (OutputBlob.cbData == 68)
	{
		TArray<uint8> PlainCredentialBytes;
		PlainCredentialBytes.Append(OutputBlob.pbData, OutputBlob.cbData);
		FMemoryReader Reader(PlainCredentialBytes);
		uint32 CredentialFormatVersion = 0;
		Reader << CredentialFormatVersion << Credential.Match << Credential.Participant;
		Credential.Secret.SetNumUninitialized(32);
		Reader.Serialize(Credential.Secret.GetData(), 32);
		bValid = CredentialFormatVersion == 1 && !Reader.IsError() && Credential.IsValid();
		FMemory::Memzero(PlainCredentialBytes.GetData(), PlainCredentialBytes.Num());
	}

	SecureZeroMemory(OutputBlob.pbData, OutputBlob.cbData);
	LocalFree(OutputBlob.pbData);
	if (!bValid)
	{
		Credential.Clear();
	}

	return bValid;
#else
	return false;
#endif
}

