#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "ExperienceFeatureSubsystem.generated.h"

enum class EExperienceFeatureReleasePolicy : uint8
{
	UnloadWhenUnused,
	KeepLoaded
};

UENUM()
enum class EExperienceFeatureState : uint8
{
	Absent,
	Registering,
	Registered,
	Loading,
	Loaded,
	Unloading,
	Failed
};

/**
 * Engine 生命周期内，跟踪经本系统申请的插件内容及使用登记，供多个 GameInstance/World 共享。
 * 租约就是以 GUID 标识的一份使用登记，没有到期时间；同一插件仍有其他登记时不会由本系统卸载。
 * 原生 GameFeature 状态机由 UE 管理；本系统按所有权请求 Registered、Loaded 或卸载，不统一接管所有插件或主动 Activate。
 */
UCLASS()
class MOBAGAME_API UExperienceFeatureSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;


public:
	// Feature 租约
	bool AcquireRegistered(const FString& FeatureName, const FGuid& FeatureLeaseId, FString& OutURL);

	bool AcquireLoaded(const FString& FeatureName, const FGuid& FeatureLeaseId, FString& OutURL);

	/**
	 * KeepLoaded 将已达到的注册/加载状态保留到进程退出，不为驻留主动升级；后续租约不能撤销该决定。
	 * 仅本系统发起注册/加载、没有驻留要求且最后租约已归还时，才允许正常卸载。
	 */
	void Release(const FString& URL, const FGuid& FeatureLeaseId, EExperienceFeatureReleasePolicy Policy = EExperienceFeatureReleasePolicy::UnloadWhenUnused);


private:
	// Feature 注册、加载与卸载
	bool AcquireFeature(const FString& FeatureName, const FGuid& FeatureLeaseId, bool bRequireLoaded, FString& OutURL);

	void BeginRegister(const FString& URL);

	void BeginLoad(const FString& URL);

	void BeginUnload(const FString& URL);


	// 状态通知
	void NotifyState(const FString& URL) const;


public:
	// 租约状态查询
	EExperienceFeatureState GetState(const FString& URL) const;

	int32 GetLeaseCount(const FString& URL) const;


private:
	struct FEntry
	{
		// 一轮内容请求对应一个持有者 ID；相同 URL 下重复登记同一 ID 不增加持有者数量。
		// 持有此 Feature 的使用者身份 ID 集合。
		TSet<FGuid> Leases;
		// 同一持有者可从 Registered 升级到 Loaded；归还前不降低其要求。
		// 要求至少达到 Loaded 状态的使用者 ID；提交请求时即加入，不表示加载已经完成。
		TSet<FGuid> LoadedLeases;
		EExperienceFeatureState State = EExperienceFeatureState::Absent;

		// 原生异步操作代次；新操作递增，完成回调用它拒绝过期结果。
		uint64 Operation = 0;
		bool bOwnsPluginState = false;
		bool bKeepLoaded = false;
		// 仅记录本系统注册/加载完成失败；不是卸载权，也不允许接管外部错误/过渡态。
		bool bRetryFailedRequest = false;
	};

	TMap<FString, FEntry> Entries;
};

