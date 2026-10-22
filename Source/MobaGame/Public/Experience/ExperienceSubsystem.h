#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Experience/ExperienceTypes.h"
#include "Experience/GameConfiguration.h"
#include "TimerManager.h"
#include "GMP/GMPSignals.inl"
#include "ExperienceSubsystem.generated.h"

struct FStreamableHandle;
class UModeDefinition;
class UMatchRulesDefinition;
class UHeroDefinition;
class UExperienceDefinition;
class UMapDefinition;
class ULevelStreamingDynamic;
class ULevelStreaming;
class ULevel;
enum class EExperienceFeatureState : uint8;
enum class ELevelStreamingState : uint8;

UCLASS()
class MOBAGAME_API UExperienceSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

private:
	enum class ELoadStep : uint8
	{
		SourceFeatures,
		SourceRegistry,
		Mode,
		Feature,
		Registry,
		Definitions,
		Bundles,
		ItemCatalog,
		Items,
		ItemSets,
		ItemAbilities,
		Map
	};


public:
	virtual void Deinitialize() override;


protected:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;


public:
	// Experience 请求
	/**
	 * 只接受宿主允许的 Mode 及该 Mode 内的完整组合；同一在途/就绪请求幂等。
	 * 切换须先 Stop 并等待 Unloaded；返回值只表示已受理，不代表 Ready。
	 * bRequestFirstModeConfig 只供非 Shipping 服务器开发入口选首项，客户端必须携带实际配置名。
	 */
	EExperienceFailure StartExperience(const FMatchLaunchSelection& RequestedSelection, FGuid NetworkInstanceId = FGuid(), bool bRequestFirstModeConfig = false);

	/**
	 * 直接从宿主根加载前台 Experience，复用公共资源流程，但不经过 Mode 或比赛地图/规则。
	 * 当前前台为空装配，在已有 World 完成 Ready；没有创建前台地图或英雄的含义。
	 */
	EExperienceFailure StartFrontend();

	/**
	 * Editor/PIE 只释放本 World 资源，内容插件保留 Loaded 跨会话复用。
	 * 非 Editor Game 正常停止仍等待地图移出 World 后归还可卸载租约。
	 */
	void StopExperience();


private:
	// 资源加载与阶段推进
	bool IsCurrentLoad(int64 Generation, ELoadStep Step) const;

	/**
	 * 为本次 Generation 启动 60 秒的一次性超时计时器，覆盖从请求开始到内容 Ready 的全部阶段。
	 * 超时只终止仍在加载的本代请求，Ready 或停止时清除；不是每个阶段各计 60 秒。
	 */
	void StartContentLoadTimeoutTimer();

	/**
	 * 按已登记的 PrimaryAssetId 异步预载资源及所选 bundle；调用方决定 Server/Client 与递归范围。
	 * Registry 就绪只说明登记已完成，本函数才请求这些 bundle 的实际资源；完成回调仍核对加载代次/阶段。
	 */
	bool PreloadPrimaryAssetBundles(const TArray<FPrimaryAssetId>& AssetIds, const TArray<FName>& BundleNames, bool bRecursive, ELoadStep Step, FSimpleDelegate OnLoaded);

	void OnModeLoaded();

	/**
	 * SourceFeatures 阶段只申请 Registered，以便读取插件内定义；Feature 阶段申请完整并集的 Loaded。
	 * 两阶段共用 GUID 和 URL 集合；原生依赖交给 UE，不主动 Activate，也不递归遍历 .uplugin。
	 */
	bool AcquireFeatures(const TArray<FString>& FeatureNames, ELoadStep Step);

	void OnFeatureStateChanged(int64 Generation, ELoadStep Step, const FString& URL, EExperienceFeatureState State);

	// 来源登记完成后读取 Mode；完整 Loaded 并集登记完成后预载 bundles。阶段随回调捕获，避免旧通知推进新阶段。
	void WaitForRegistry(ELoadStep Step);

	void OnRegistryReady(int64 Generation, ELoadStep Step);

	void OnDefinitionsLoaded();

	void OnBundlesLoaded();

	/**
	 * 按软路径异步加载指定对象，用于 Mode、Definition、前台及物品资源。
	 * 此处没有选择 Server/Client bundle，完成后的流程由 Step 和 OnLoaded 指定。
	 */
	bool LoadAssetsBySoftPath(const TArray<FSoftObjectPath>& Paths, ELoadStep Step, FSimpleDelegate OnLoaded);

	void OnItemCatalogLoaded();

	void OnItemsLoaded();

	void OnItemSetsLoaded();

	void OnItemAbilitiesLoaded();

	void BeginMapLoading();

	void OnMapStreamingStateChanged(UWorld* World, const ULevelStreaming* LevelStreaming, ULevel* Level, ELevelStreamingState PreviousState, ELevelStreamingState State);

	void QueueMapStateCheck();

	void HandleMapState();


	// 停止与资源释放
	void ClearLoadSubscriptions();

	/**
	 * 终止本 World 的整条内容流程，使旧代次失效并取消本次加载订阅和未完成请求。
	 * 有地图时等待本次实例移出 World，再释放自己持有的资源和插件使用登记；不会强卸其他持有者的内容。
	 * Failure 为 None 时最终进入 Unloaded，否则进入 Failed；清理不保证同步完成。
	 */
	void BeginStop(EExperienceFailure Failure);

	void FinishStop();

	void ReleaseResources();


	// 状态通知
	void NotifyState() const;


public:
	// 资源持有检查
	bool HasOwnedResources() const;


public:
	const FExperienceSnapshot& GetSnapshot() const { return ExperienceSnapshot; }
	const UExperienceDefinition* GetExperience() const { return ExperienceDef; }
	const UMapDefinition* GetMap() const { return MapDef; }
	const UMatchRulesDefinition* GetRules() const { return RulesDef; }
	const UHeroDefinition* GetDevelopmentHero() const;
	ULevelStreamingDynamic* GetMapInstance() const { return MapInstance; }


private:
#if WITH_DEV_AUTOMATION_TESTS
	friend struct FExperienceResidencyTestAccess;
#endif

	FExperienceSnapshot ExperienceSnapshot;

	ELoadStep LoadStep = ELoadStep::Mode;

	bool bDeinitializing = false;

	// 当前 World 对 GameFeature 的使用者身份 ID；所有插件请求共享此 ID，停止时释放。
	FGuid FeatureLease;

	// 地图加载实例的网络身份 ID，用于各端生成一致的流送实例名称。
	FGuid MapNetworkInstanceId;

	TArray<FString> FeatureURLs;

	FMatchLaunchSelection Selection;

	FEnabledModeEntry EnabledMode;

	FModeConfiguration ModeConfig;

	bool bUseFirstModeConfig = false;

	bool bFrontend = false;

	// 仅在同步提交 Feature 请求的循环中为 true，用于阻止重入推进；不代表整个异步加载期间。
	bool bAcquiringFeatures = false;

	// 当前加载持有的资源流送句柄；停止时取消未完成请求并释放已完成资源。
	TArray<TSharedPtr<FStreamableHandle>> Handles;

	FTimerHandle ContentLoadTimeoutTimer;

	FTimerHandle MapEventTimer;

	FDelegateHandle RegistryReadyHandle;

	FDelegateHandle StreamingStateHandle;

	// 进程级通知使用 GMP 原生 collection；弱引用回到本 World，避免对象通知的跨 World 过滤。
	GMP::FSigCollection FeatureSubscriptions;

	// 当前已加载的模式定义资产，不是运行时 GameMode。
	UPROPERTY(Transient)
	TObjectPtr<UModeDefinition> ModeDef;

	// 当前模式组合的比赛规则定义资产。
	UPROPERTY(Transient)
	TObjectPtr<UMatchRulesDefinition> RulesDef;

	// 开发启动选中的 HeroDefinition 资产软引用，不是角色实例。
	UPROPERTY(Transient)
	TSoftObjectPtr<UHeroDefinition> DevelopmentHero;

	// 当前已加载的 ExperienceDefinition 资产，与本子系统对象区分。
	UPROPERTY(Transient)
	TObjectPtr<UExperienceDefinition> ExperienceDef;

	// 当前地图定义资产，World 软引用保存在该定义中。
	UPROPERTY(Transient)
	TObjectPtr<UMapDefinition> MapDef;

	// 当前地图的动态 LevelStreaming 实例，不是 World 或 MapDefinition。
	UPROPERTY(Transient)
	TObjectPtr<ULevelStreamingDynamic> MapInstance;
};

