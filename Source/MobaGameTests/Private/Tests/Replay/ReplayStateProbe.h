#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ReplayStateProbe.generated.h"

// 仅测试模块使用：状态经原生属性和移动复制进入 DemoNetDriver，没有播放驱动逻辑。
UCLASS(NotBlueprintable)
class AReplayStateProbe : public AActor
{
	GENERATED_BODY()

public:
	AReplayStateProbe();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;


public:
	UPROPERTY(Replicated)
	int32 SampleIndex = INDEX_NONE;
};
