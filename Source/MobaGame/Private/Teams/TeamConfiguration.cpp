#include "Teams/TeamConfiguration.h"

ETeamRelation FTeamConfiguration::Query(int32 FromTeamId, int32 ToTeamId) const
{
	if ((FromTeamId != INDEX_NONE && !Teams.Contains(FromTeamId)) || (ToTeamId != INDEX_NONE && !Teams.Contains(ToTeamId)))
	{
		return ETeamRelation::Undefined;
	}

	if (FromTeamId == INDEX_NONE || ToTeamId == INDEX_NONE)
	{
		return ETeamRelation::Neutral;
	}

	if (FromTeamId == ToTeamId)
	{
		return ETeamRelation::Ally;
	}

	for (const FTeamRelationEntry& TeamRelation : Relations)
	{
		if (TeamRelation.From == FromTeamId && TeamRelation.To == ToTeamId)
		{
			return TeamRelation.Relation;
		}
	}

	return ETeamRelation::Undefined;
}

bool FTeamConfiguration::IsValid(FString& Reason) const
{
	TSet<int32> SeenTeamIds;
	for (int32 TeamId : Teams)
	{
		if (TeamId < 0 || SeenTeamIds.Contains(TeamId))
		{
			Reason = TEXT("队伍 ID 必须非负且唯一。");
			return false;
		}

		SeenTeamIds.Add(TeamId);
	}

	if (Teams.IsEmpty())
	{
		Reason = TEXT("至少需要一个队伍。");
		return false;
	}

	TSet<uint64> SeenTeamPairKeys;
	for (const FTeamRelationEntry& TeamRelation : Relations)
	{
		const uint64 TeamPairKey = (uint64(uint32(TeamRelation.From)) << 32) | uint32(TeamRelation.To);
		if (!SeenTeamIds.Contains(TeamRelation.From) || !SeenTeamIds.Contains(TeamRelation.To) || TeamRelation.From == TeamRelation.To || TeamRelation.Relation == ETeamRelation::Undefined || SeenTeamPairKeys.Contains(TeamPairKey))
		{
			Reason = TEXT("关系必须引用不同的已配置队伍，且每个有向关系只定义一次。");
			return false;
		}

		SeenTeamPairKeys.Add(TeamPairKey);
	}

	for (int32 FromTeamId : Teams)
	{
		for (int32 ToTeamId : Teams)
		{
			if (Query(FromTeamId, ToTeamId) == ETeamRelation::Undefined)
			{
				Reason = TEXT("必须明确所有跨队关系。");
				return false;
			}
		}
	}

	return true;
}

