#include "Experience/ExperienceFeatureSubsystem.h"

#include "GameFeaturesSubsystem.h"
#include "GameFeaturePluginOperationResult.h"
#include "MobaLogChannels.h"
#include "MobaMessageTags.h"
#include "GameFeatureData.h"
#include "GameFeatureTypes.h"
#include "Engine/AssetManager.h"

void UExperienceFeatureSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Collection.InitializeDependency<UGameFeaturesSubsystem>();
	Super::Initialize(Collection);
}

bool UExperienceFeatureSubsystem::AcquireRegistered(const FString& FeatureName, const FGuid& FeatureLeaseId, FString& OutURL)
{
	return AcquireFeature(FeatureName, FeatureLeaseId, false, OutURL);
}

bool UExperienceFeatureSubsystem::AcquireLoaded(const FString& FeatureName, const FGuid& FeatureLeaseId, FString& OutURL)
{
	return AcquireFeature(FeatureName, FeatureLeaseId, true, OutURL);
}

void UExperienceFeatureSubsystem::Release(const FString& URL, const FGuid& FeatureLeaseId, EExperienceFeatureReleasePolicy Policy)
{
	check(IsInGameThread());
	FEntry* Entry = Entries.Find(URL);
	if (!Entry || !Entry->Leases.Remove(FeatureLeaseId))
	{
		return;
	}

	Entry->LoadedLeases.Remove(FeatureLeaseId);
	// 注册/加载未完成时也立即记录；完成回调不能在 World teardown 后补做卸载。
	Entry->bKeepLoaded |= GIsEditor || Policy == EExperienceFeatureReleasePolicy::KeepLoaded;
	if (Entry->Leases.IsEmpty() && (Entry->State == EExperienceFeatureState::Registered || Entry->State == EExperienceFeatureState::Loaded || Entry->State == EExperienceFeatureState::Failed))
	{
		BeginUnload(URL);
	}
}

bool UExperienceFeatureSubsystem::AcquireFeature(const FString& FeatureName, const FGuid& FeatureLeaseId, bool bRequireLoaded, FString& OutURL)
{
	check(IsInGameThread());
	if (!FeatureLeaseId.IsValid() || !UGameFeaturesSubsystem::Get().GetPluginURLByName(FeatureName, OutURL))
	{
		return false;
	}

	// 无使用者期间插件可能被外部卸载；先核对缓存状态是否仍满足原生状态。
	FEntry& Entry = Entries.FindOrAdd(OutURL);
	if ((Entry.State == EExperienceFeatureState::Registered || Entry.State == EExperienceFeatureState::Loaded) && Entry.Leases.IsEmpty())
	{
		const EGameFeaturePluginState NativeState = UGameFeaturesSubsystem::Get().GetPluginState(OutURL);
		if (NativeState != EGameFeaturePluginState::Loaded && NativeState != EGameFeaturePluginState::Active
			&& !(Entry.State == EExperienceFeatureState::Registered && NativeState == EGameFeaturePluginState::Registered))
		{
			// 零租约期间外部可能卸载；旧的本地快照不能作为下一 World 的就绪事实。
			Entry.State = EExperienceFeatureState::Absent;
			Entry.bOwnsPluginState = false;
		}
	}

	// 新请求或无人使用的失败记录，只接受可请求的原生状态；不接管外部过渡态或未知失败。
	if (Entry.State == EExperienceFeatureState::Absent || (Entry.State == EExperienceFeatureState::Failed && Entry.Leases.IsEmpty()))
	{
		const EGameFeaturePluginState NativeState = UGameFeaturesSubsystem::Get().GetPluginState(OutURL);
		// 这些原生 destination 错误可重新请求；只重试本系统已完成的失败注册/加载。
		const bool bRetryRequest = Entry.State == EExperienceFeatureState::Failed && Entry.bRetryFailedRequest
			&& (NativeState == EGameFeaturePluginState::ErrorMounting || NativeState == EGameFeaturePluginState::ErrorWaitingForDependencies
				|| NativeState == EGameFeaturePluginState::ErrorRegistering || NativeState == EGameFeaturePluginState::ErrorAssetDependencyStreaming
				|| NativeState == EGameFeaturePluginState::ErrorLoading);
		if (!bRetryRequest && NativeState != EGameFeaturePluginState::UnknownStatus && NativeState != EGameFeaturePluginState::StatusKnown
			&& NativeState != EGameFeaturePluginState::Installed && NativeState != EGameFeaturePluginState::Registered
			&& NativeState != EGameFeaturePluginState::Loaded && NativeState != EGameFeaturePluginState::Active)
		{
			UE_LOG(LogMobaExperience, Warning, TEXT("FeatureAcquireRejected URL=%s NativeState=%d"), *OutURL, int32(NativeState));
			return false;
		}

		Entry.State = EExperienceFeatureState::Absent;
	}

	// 登记当前使用者及其最低要求；LoadedLeases 是要求 Loaded 的使用者 ID，不是子插件。
	Entry.Leases.Add(FeatureLeaseId);
	if (bRequireLoaded)
	{
		Entry.LoadedLeases.Add(FeatureLeaseId);
	}

	// 按所有使用者的要求发起注册或加载；已有操作继续等待，Registered 可升级到 Loaded。
	if (Entry.State == EExperienceFeatureState::Absent)
	{
		if (Entry.LoadedLeases.IsEmpty())
		{
			BeginRegister(OutURL);
		}
		else
		{
			BeginLoad(OutURL);
		}
	}
	else if (Entry.State == EExperienceFeatureState::Registered && !Entry.LoadedLeases.IsEmpty())
	{
		BeginLoad(OutURL);
	}

	return true;
}

void UExperienceFeatureSubsystem::BeginRegister(const FString& URL)
{
	FEntry& Entry = Entries.FindChecked(URL);
	const EGameFeaturePluginState NativeState = UGameFeaturesSubsystem::Get().GetPluginState(URL);
	if (NativeState == EGameFeaturePluginState::Registered || NativeState == EGameFeaturePluginState::Loaded || NativeState == EGameFeaturePluginState::Active)
	{
		// 已有 Loaded/Active 满足注册要求；不向原生状态机发送反向请求。
		Entry.bRetryFailedRequest = false;
		Entry.bOwnsPluginState = false;
		Entry.State = NativeState == EGameFeaturePluginState::Registered ? EExperienceFeatureState::Registered : EExperienceFeatureState::Loaded;
		NotifyState(URL);
		return;
	}

	Entry.bRetryFailedRequest = false;
	Entry.bOwnsPluginState = true;
	Entry.State = EExperienceFeatureState::Registering;
	const uint64 OperationSerial = ++Entry.Operation;
	UGameFeaturesSubsystem::Get().RegisterGameFeaturePlugin(URL,
		FGameFeaturePluginLoadComplete::CreateWeakLambda(this,
			[this, URL, OperationSerial](const UE::GameFeatures::FResult& Result)
			{
				FEntry* CurrentFeatureEntry = Entries.Find(URL);
				if (!CurrentFeatureEntry || CurrentFeatureEntry->Operation != OperationSerial)
				{
					return;
				}

				if (Result.HasError())
				{
					CurrentFeatureEntry->State = EExperienceFeatureState::Failed;
					CurrentFeatureEntry->bRetryFailedRequest = true;
					CurrentFeatureEntry->bOwnsPluginState = false;
					UE_LOG(LogMobaExperience, Warning, TEXT("FeatureRegisterFailed URL=%s Reason=%s"), *URL, *Result.GetError());
					NotifyState(URL);
					return;
				}

				CurrentFeatureEntry->State = EExperienceFeatureState::Registered;
				if (CurrentFeatureEntry->Leases.IsEmpty())
				{
					BeginUnload(URL);
					return;
				}

				// 其他 World 在注册期间可能已要求 Loaded；只串行升级，不重复发起原生操作。
				if (!CurrentFeatureEntry->LoadedLeases.IsEmpty())
				{
					BeginLoad(URL);
					return;
				}

				NotifyState(URL);
			}));
}

void UExperienceFeatureSubsystem::BeginLoad(const FString& URL)
{
	FEntry& Entry = Entries.FindChecked(URL);
	const EGameFeaturePluginState NativeState = UGameFeaturesSubsystem::Get().GetPluginState(URL);
	if (NativeState == EGameFeaturePluginState::Loaded || NativeState == EGameFeaturePluginState::Active)
	{
		// 外部已建立的状态只借用；观测到 Loaded 不代表拥有降级或卸载权。
		Entry.bRetryFailedRequest = false;
		Entry.bOwnsPluginState = false;
		Entry.State = EExperienceFeatureState::Loaded;
		NotifyState(URL);
		return;
	}

	Entry.bRetryFailedRequest = false;
	Entry.bOwnsPluginState = true;
	Entry.State = EExperienceFeatureState::Loading;
	const uint64 OperationSerial = ++Entry.Operation;
	UGameFeaturesSubsystem::Get().LoadGameFeaturePlugin(URL,
		FGameFeaturePluginLoadComplete::CreateWeakLambda(this,
			[this, URL, OperationSerial](const UE::GameFeatures::FResult& Result)
			{
				FEntry* CurrentFeatureEntry = Entries.Find(URL);
				if (!CurrentFeatureEntry || CurrentFeatureEntry->Operation != OperationSerial)
				{
					return;
				}

				CurrentFeatureEntry->State = Result.HasError() ? EExperienceFeatureState::Failed : EExperienceFeatureState::Loaded;
				if (Result.HasError())
				{
					// 失败不构成后续状态的所有权；残留交给原生系统/进程退出，不主动 unregister。
					CurrentFeatureEntry->bRetryFailedRequest = true;
					CurrentFeatureEntry->bOwnsPluginState = false;
					UE_LOG(LogMobaExperience, Warning, TEXT("FeatureLoadFailed URL=%s Reason=%s"), *URL, *Result.GetError());
				}

				if (CurrentFeatureEntry->Leases.IsEmpty())
				{
					BeginUnload(URL);
					return;
				}

				NotifyState(URL);
			}));
}

void UExperienceFeatureSubsystem::BeginUnload(const FString& URL)
{
	FEntry& Entry = Entries.FindChecked(URL);
	check(Entry.Leases.IsEmpty());
	if (Entry.bKeepLoaded || !Entry.bOwnsPluginState)
	{
		return;
	}

	// 外部在租约期间改变原生状态时放弃卸载权；包括正在激活，不能发出反向状态请求。
	const EGameFeaturePluginState NativeState = UGameFeaturesSubsystem::Get().GetPluginState(URL);
	if (NativeState == EGameFeaturePluginState::Active || NativeState == EGameFeaturePluginState::Activating
		|| (Entry.State == EExperienceFeatureState::Loaded && NativeState != EGameFeaturePluginState::Loaded)
		|| (Entry.State == EExperienceFeatureState::Registered && NativeState != EGameFeaturePluginState::Registered))
	{
		Entry.bOwnsPluginState = false;
		return;
	}

	Entry.bRetryFailedRequest = false;
	const bool bWasLoaded = Entry.State == EExperienceFeatureState::Loaded;
	Entry.State = EExperienceFeatureState::Unloading;
	const uint64 OperationSerial = ++Entry.Operation;
	if (bWasLoaded)
	{
		if (const UGameFeatureData* FeatureData = UGameFeaturesSubsystem::Get().GetGameFeatureDataForRegisteredPluginByURL(URL))
		{
			// 仅最后一个进程租约：先取消 FeatureData 原始 bundle 句柄及其延迟回调。
			// UE5.8 原生 Unloading 先替换 bundle，旧 combined child 可能因此留到同帧 unmount GC。
			// 状态机自身仍持有 FeatureData；后续原生卸载负责完整的 unregister/unmount。
			UAssetManager::Get().UnloadPrimaryAsset(FeatureData->GetPrimaryAssetId());
		}
	}

	UGameFeaturesSubsystem::Get().UnloadGameFeaturePlugin(URL,
		FGameFeaturePluginUnloadComplete::CreateWeakLambda(this,
			[this, URL, OperationSerial](const UE::GameFeatures::FResult& Result)
			{
				FEntry* CurrentFeatureEntry = Entries.Find(URL);
				if (!CurrentFeatureEntry || CurrentFeatureEntry->Operation != OperationSerial)
				{
					return;
				}

				if (Result.HasError())
				{
					CurrentFeatureEntry->bOwnsPluginState = false;
					CurrentFeatureEntry->State = EExperienceFeatureState::Failed;
					UE_LOG(LogMobaExperience, Error, TEXT("FeatureUnloadFailed URL=%s Reason=%s"), *URL, *Result.GetError());
					NotifyState(URL);
					return;
				}

				CurrentFeatureEntry->bOwnsPluginState = false;
				CurrentFeatureEntry->State = EExperienceFeatureState::Absent;
				if (!CurrentFeatureEntry->Leases.IsEmpty())
				{
					if (CurrentFeatureEntry->LoadedLeases.IsEmpty())
					{
						BeginRegister(URL);
					}
					else
					{
						BeginLoad(URL);
					}
					return;
				}

				NotifyState(URL);
			}),
		false);
}

void UExperienceFeatureSubsystem::NotifyState(const FString& URL) const
{
	// 通知可重入申请/归还租约；派发稳定值副本后不再访问原 Entry。
	const FString CommittedURL = URL;
	const EExperienceFeatureState CommittedState = GetState(URL);
	GMP::FMessageUtils::SendObjectMessage(this, MOBA_MESSAGE_EXPERIENCE_FEATURE_CHANGED, CommittedURL, CommittedState);
}

EExperienceFeatureState UExperienceFeatureSubsystem::GetState(const FString& URL) const
{
	const FEntry* Entry = Entries.Find(URL);
	return Entry ? Entry->State : EExperienceFeatureState::Absent;
}

int32 UExperienceFeatureSubsystem::GetLeaseCount(const FString& URL) const
{
	const FEntry* Entry = Entries.Find(URL);
	return Entry ? Entry->Leases.Num() : 0;
}

