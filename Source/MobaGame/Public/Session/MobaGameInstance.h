#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "MobaGameInstance.generated.h"

UCLASS()
class MOBAGAME_API UMobaGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	// 连接与恢复
	// 断线后仍可从Entry控制台恢复；只接受服务器地址，凭证由Framework本机存储读取。
	UFUNCTION(Exec)
	void MobaResume(const FString& Address);

	UFUNCTION(Exec)
	void MobaJoin(const FString& Address);

	UFUNCTION(Exec)
	void MobaFrontend();


private:
	void Connect(const FString& Address, bool bResume);
};
