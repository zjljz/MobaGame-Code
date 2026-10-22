#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "Teams/TeamConfiguration.h"
#include "Experience/GameConfiguration.h"
#include "MatchGameState.generated.h"

UCLASS()
class MOBAGAME_API AMatchGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AMatchGameState();

	virtual void BeginPlay() override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;


private:
	// Experience 加载条件
	UFUNCTION()
	void OnRep_ExperienceSelection();


public:
	UPROPERTY(ReplicatedUsing = OnRep_ExperienceSelection)
	FGuid MatchId;

	UPROPERTY(ReplicatedUsing = OnRep_ExperienceSelection)
	bool bMatchEnded = false;

	// 同一装载身份复制给所有端，不能用各进程 World 路径生成不同的网络 Level 名称。
	// 各端共享的 Experience 加载实例身份 ID，不是子系统或定义资产对象。
	UPROPERTY(ReplicatedUsing = OnRep_ExperienceSelection)
	FGuid ExperienceInstanceId;

	// 服务器确定并复制的比赛模式与组合名称。
	UPROPERTY(ReplicatedUsing = OnRep_ExperienceSelection)
	FMatchLaunchSelection Selection;

	// 整局队伍关系配置；其 Teams 字段才是队伍 ID 数组。
	UPROPERTY(Replicated)
	FTeamConfiguration Teams;
};

