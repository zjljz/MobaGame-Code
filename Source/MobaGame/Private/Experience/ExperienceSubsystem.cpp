#include "Experience/ExperienceSubsystem.h"

#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "Items/ItemDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "GMP/GMPThreadUtils.h"
#include "Experience/ExperienceFeatureSubsystem.h"
#include "Experience/MobaAssetManager.h"
#include "Experience/GameConfiguration.h"
#include "Engine/Engine.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/StreamableManager.h"
#include "MobaLogChannels.h"
#include "MobaMessageTags.h"
#include "Misc/PackageName.h"
#include "Streaming/LevelStreamingDelegates.h"

namespace
{
	/**
	 * 强制排队到游戏线程，避免在来源通知栈中推进加载；只在执行时解析弱对象，不延长其寿命。
	 * Callback 自行按值捕获通知参数，并在成员入口保留 generation/step 检查；不保证下一帧执行。
	 */
	void QueueExperienceCallback(TWeakObjectPtr<UExperienceSubsystem> WeakExperienceSubsystem, TFunction<void(UExperienceSubsystem&)> Callback)
	{
		GMP::RunOnGameThread([WeakExperienceSubsystem, Callback = MoveTemp(Callback)]
			{
				if (UExperienceSubsystem* ExperienceSubsystem = WeakExperienceSubsystem.Get())
				{
					Callback(*ExperienceSubsystem);
				}
			}, GMP::ERunType::ForceAsync);
	}
}

void UExperienceSubsystem::Deinitialize()
{
	bDeinitializing = true;
	StopExperience();
	// World teardown 自身负责关卡取消/销毁；此路径只清本 World，原生插件保留到进程退出。
	ReleaseResources();
	ExperienceSnapshot.State = EExperienceState::Unloaded;
	Super::Deinitialize();
}

bool UExperienceSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

EExperienceFailure UExperienceSubsystem::StartExperience(const FMatchLaunchSelection& RequestedSelection, FGuid NetworkInstanceId, bool bRequestFirstModeConfig)
{
	check(IsInGameThread());
	if (bDeinitializing || !GetWorld() || GetWorld()->bIsTearingDown || GetWorld()->IsBeingCleanedUp())
	{
		return EExperienceFailure::WorldUnavailable;
	}

	if (ExperienceSnapshot.State != EExperienceState::Unloaded && ExperienceSnapshot.State != EExperienceState::Failed)
	{
		const bool bSameRequest = !bFrontend && Selection.ModeId == RequestedSelection.ModeId && MapNetworkInstanceId == NetworkInstanceId
			&& bUseFirstModeConfig == bRequestFirstModeConfig && (bRequestFirstModeConfig || Selection.ModeConfigName == RequestedSelection.ModeConfigName);
		if (bSameRequest && ExperienceSnapshot.State != EExperienceState::Stopping)
		{
			return EExperienceFailure::None;
		}

		return EExperienceFailure::Busy;
	}

	//验证AssetManager和GameConfig.
	const UMobaAssetManager* AssetManager = Cast<UMobaAssetManager>(UAssetManager::GetIfInitialized());
	const UGameConfiguration* GameConfig = AssetManager ? AssetManager->GetGameConfiguration() : nullptr;
	FString Reason = TEXT("RootObjectUnavailable");
	if (!GameConfig || !GameConfig->ValidateConfiguration(Reason))
	{
		UE_LOG(LogMobaExperience, Error, TEXT("ExperienceStartRejected Mode=%s Reason=%s"), *RequestedSelection.ModeId.ToString(), *Reason);
		BeginStop(EExperienceFailure::ModeNotAllowed);
		return EExperienceFailure::ModeNotAllowed;
	}

	//验证对应ModeEntry不为空.
	const FEnabledModeEntry* EnabledModeEntry = GameConfig->FindEnabledMode(RequestedSelection.ModeId);
	if (!EnabledModeEntry)
	{
		UE_LOG(LogMobaExperience, Error, TEXT("ExperienceStartRejected Mode=%s Reason=ModeNotEnabled"), *RequestedSelection.ModeId.ToString());
		BeginStop(EExperienceFailure::ModeNotAllowed);
		return EExperienceFailure::ModeNotAllowed;
	}

	//
	if (bRequestFirstModeConfig && (UE_BUILD_SHIPPING || GetWorld()->GetNetMode() == NM_Client))
	{
		UE_LOG(LogMobaExperience, Error, TEXT("ExperienceStartRejected Mode=%s Reason=DevelopmentSelectionNotAllowed"), *RequestedSelection.ModeId.ToString());
		BeginStop(EExperienceFailure::InvalidMode);
		return EExperienceFailure::InvalidMode;
	}

	++ExperienceSnapshot.Generation;
	Selection = RequestedSelection;
	EnabledMode = *EnabledModeEntry;
	bUseFirstModeConfig = bRequestFirstModeConfig;
	MapNetworkInstanceId = NetworkInstanceId;
	ExperienceSnapshot.ModeId = Selection.ModeId;
	ExperienceSnapshot.ModeConfigName = Selection.ModeConfigName;
	ExperienceSnapshot.Failure = EExperienceFailure::None;
	ExperienceSnapshot.State = EExperienceState::Loading;
	bFrontend = false;
	
	StartContentLoadTimeoutTimer();
	
	if (!AcquireFeatures(EnabledMode.DefinitionSourceFeatures, ELoadStep::SourceFeatures))
	{
		return ExperienceSnapshot.Failure;
	}

	NotifyState();
	return EExperienceFailure::None;
}

EExperienceFailure UExperienceSubsystem::StartFrontend()
{
	if (bDeinitializing || !GetWorld() || GetWorld()->bIsTearingDown || GetWorld()->IsBeingCleanedUp()
		|| (ExperienceSnapshot.State != EExperienceState::Unloaded && ExperienceSnapshot.State != EExperienceState::Failed))
	{
		return EExperienceFailure::Busy;
	}

	const UMobaAssetManager* AssetManager = Cast<UMobaAssetManager>(UAssetManager::GetIfInitialized());
	const UGameConfiguration* GameConfig = AssetManager ? AssetManager->GetGameConfiguration() : nullptr;
	++ExperienceSnapshot.Generation;
	ExperienceSnapshot.ModeId = FPrimaryAssetId();
	ExperienceSnapshot.ModeConfigName = NAME_None;
	ExperienceSnapshot.State = EExperienceState::Loading;
	ExperienceSnapshot.Failure = EExperienceFailure::None;
	bFrontend = true;
	bUseFirstModeConfig = false;
	EnabledMode = FEnabledModeEntry();
	MapNetworkInstanceId.Invalidate();
	Selection = FMatchLaunchSelection();
	ModeConfig = FModeConfiguration();
	FString Reason;
	if (!GameConfig || !GameConfig->ValidateConfiguration(Reason))
	{
		BeginStop(EExperienceFailure::MissingDefinition);
		return EExperienceFailure::MissingDefinition;
	}

	ModeConfig.ExperienceDef = GameConfig->FrontendExperience;
	StartContentLoadTimeoutTimer();
	return LoadAssetsBySoftPath({ModeConfig.ExperienceDef.ToSoftObjectPath()}, ELoadStep::Definitions,
		FSimpleDelegate::CreateUObject(this, &UExperienceSubsystem::OnDefinitionsLoaded)) ? EExperienceFailure::None : EExperienceFailure::AssetLoadFailed;
}

void UExperienceSubsystem::StopExperience()
{
	if (ExperienceSnapshot.State == EExperienceState::Unloaded || ExperienceSnapshot.State == EExperienceState::Stopping)
	{
		return;
	}

	BeginStop(EExperienceFailure::None);
}

bool UExperienceSubsystem::IsCurrentLoad(int64 Generation, ELoadStep Step) const
{
	return !bDeinitializing && GetWorld() && !GetWorld()->bIsTearingDown && !GetWorld()->IsBeingCleanedUp() && ExperienceSnapshot.Generation == Generation && LoadStep == Step
		&& (ExperienceSnapshot.State == EExperienceState::Loading || ExperienceSnapshot.State == EExperienceState::Activating);
}

void UExperienceSubsystem::StartContentLoadTimeoutTimer()
{
	const int64 Generation = ExperienceSnapshot.Generation;
	GetWorld()->GetTimerManager().SetTimer(ContentLoadTimeoutTimer, FTimerDelegate::CreateWeakLambda(this, [this, Generation]
		{
			if (ExperienceSnapshot.Generation == Generation && (ExperienceSnapshot.State == EExperienceState::Loading || ExperienceSnapshot.State == EExperienceState::Activating))
			{
				BeginStop(EExperienceFailure::Timeout);
			}
		}), 60.0f, false);
}

bool UExperienceSubsystem::PreloadPrimaryAssetBundles(const TArray<FPrimaryAssetId>& AssetIds, const TArray<FName>& BundleNames, bool bRecursive, ELoadStep Step, FSimpleDelegate OnLoaded)
{
	LoadStep = Step;
	const int64 Generation = ExperienceSnapshot.Generation;
	UAssetManager& AssetManager = UAssetManager::Get();
	TSharedPtr<FStreamableHandle> AssetLoadHandle = AssetManager.PreloadPrimaryAssets(AssetIds, BundleNames, bRecursive, FAssetManagerLoadParams());
	if (!AssetLoadHandle)
	{
		BeginStop(EExperienceFailure::AssetLoadFailed);
		return false;
	}

	Handles.Add(AssetLoadHandle);
	FStreamableDelegate LoadCompleteDelegate = FStreamableDelegate::CreateWeakLambda(this,
		[this, Generation, Step, OnLoaded = MoveTemp(OnLoaded)]
		{
			if (IsCurrentLoad(Generation, Step))
			{
				OnLoaded.ExecuteIfBound();
			}
		});
	// 先持有句柄再允许推进；已加载资源也经过同一个完成处理。
	if (AssetLoadHandle->HasLoadCompleted())
	{
		FStreamableHandle::ExecuteDelegate(MoveTemp(LoadCompleteDelegate));
	}
	else
	{
		AssetLoadHandle->BindCompleteDelegate(MoveTemp(LoadCompleteDelegate));
	}

	return true;
}

void UExperienceSubsystem::OnModeLoaded()
{
	ModeDef = EnabledMode.ModeDef.Get();
	if (!ModeDef || ModeDef->GetPrimaryAssetId() != Selection.ModeId || ModeDef->ModeConfigs.IsEmpty())
	{
		UE_LOG(LogMobaExperience, Error, TEXT("ModeLoadRejected Mode=%s Reason=MissingIdentityOrConfigurations"), *Selection.ModeId.ToString());
		BeginStop(EExperienceFailure::InvalidMode);
		return;
	}

	if (bUseFirstModeConfig)
	{
		Selection.ModeConfigName = ModeDef->ModeConfigs[0].ModeConfigName;
	}

	const FModeConfiguration* SelectedModeConfig = ModeDef->FindModeConfig(Selection.ModeConfigName);
	const int32 MatchingModeConfigCount = ModeDef->ModeConfigs.FilterByPredicate([this](const FModeConfiguration& Candidate)
		{ return Candidate.ModeConfigName == Selection.ModeConfigName; }).Num();
	if (Selection.ModeConfigName.IsNone() || !SelectedModeConfig || MatchingModeConfigCount != 1
		|| SelectedModeConfig->ExperienceDef.IsNull() || SelectedModeConfig->MapDef.IsNull() || SelectedModeConfig->RulesDef.IsNull())
	{
		UE_LOG(LogMobaExperience, Error, TEXT("ModeLoadRejected Mode=%s ModeConfig=%s Reason=InvalidModeConfig"),
			*Selection.ModeId.ToString(), *Selection.ModeConfigName.ToString());
		BeginStop(EExperienceFailure::InvalidMode);
		return;
	}

	ExperienceSnapshot.ModeConfigName = Selection.ModeConfigName;
	ModeConfig = *SelectedModeConfig;
	// 此处只读取轻量定义；来源插件仍可停在 Registered，重资源稍后按 bundle 请求。
	LoadAssetsBySoftPath({ModeConfig.ExperienceDef.ToSoftObjectPath(), ModeConfig.MapDef.ToSoftObjectPath(), ModeConfig.RulesDef.ToSoftObjectPath()},
		ELoadStep::Definitions, FSimpleDelegate::CreateUObject(this, &UExperienceSubsystem::OnDefinitionsLoaded));
}

bool UExperienceSubsystem::AcquireFeatures(const TArray<FString>& FeatureNames, ELoadStep Step)
{
	UExperienceFeatureSubsystem* FeatureSubsystem = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
	check(Step == ELoadStep::SourceFeatures || Step == ELoadStep::Feature);
	LoadStep = Step;
	if (!FeatureLease.IsValid())
	{
		FeatureLease = FGuid::NewGuid();
	}

	const int64 Generation = ExperienceSnapshot.Generation;
	const TWeakObjectPtr<UExperienceSubsystem> WeakThis(this);
	FeatureSubscriptions.DisconnectAll();
	GMP::FMessageUtils::ListenObjectMessage(FeatureSubsystem, MOBA_MESSAGE_EXPERIENCE_FEATURE_CHANGED, &FeatureSubscriptions,
		[WeakThis, Generation, Step](const FString& FeatureURL, EExperienceFeatureState State)
		{
			QueueExperienceCallback(WeakThis, [Generation, Step, FeatureURL, State](UExperienceSubsystem& ExperienceSubsystem)
				{
					ExperienceSubsystem.OnFeatureStateChanged(Generation, Step, FeatureURL, State);
				});
		});
	
	bAcquiringFeatures = true;
	TSet<FString> UniqueFeatureNames;
	for (const FString& FeatureName : FeatureNames)
	{
		if (UniqueFeatureNames.Contains(FeatureName))
		{
			continue;
		}

		UniqueFeatureNames.Add(FeatureName);
		FString FeatureURL;
		bool bFeatureRequestAccepted = false;
		if (!FeatureName.IsEmpty())
		{
			if (Step == ELoadStep::SourceFeatures)
			{
				bFeatureRequestAccepted = FeatureSubsystem->AcquireRegistered(FeatureName, FeatureLease, FeatureURL);
			}
			else
			{
				bFeatureRequestAccepted = FeatureSubsystem->AcquireLoaded(FeatureName, FeatureLease, FeatureURL);
			}
		}

		if (!bFeatureRequestAccepted)
		{
			bAcquiringFeatures = false;
			BeginStop(EExperienceFailure::MissingFeature);
			return false;
		}

		// 来源注册升级为完整加载时仍持有同一 GUID；重复名称/URL 不增加租约数量。
		FeatureURLs.AddUnique(FeatureURL);
	}

	bAcquiringFeatures = false;
	OnFeatureStateChanged(Generation, Step, FString(), EExperienceFeatureState::Loaded);
	return ExperienceSnapshot.State != EExperienceState::Failed;
}

void UExperienceSubsystem::OnFeatureStateChanged(int64 Generation, ELoadStep Step, const FString& URL, EExperienceFeatureState State)
{
	if (!IsCurrentLoad(Generation, Step) || bAcquiringFeatures || (!URL.IsEmpty() && !FeatureURLs.Contains(URL)))
	{
		return;
	}

	const auto* FeatureSubsystem = GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>();
	bool bAllSatisfied = true;
	for (const FString& RequiredFeatureURL : FeatureURLs)
	{
		const EExperienceFeatureState CurrentFeatureState = FeatureSubsystem->GetState(RequiredFeatureURL);
		if (CurrentFeatureState == EExperienceFeatureState::Failed)
		{
			BeginStop(EExperienceFailure::FeatureLoadFailed);
			return;
		}

		const bool bFeatureSatisfied = (CurrentFeatureState == EExperienceFeatureState::Loaded
			|| (Step == ELoadStep::SourceFeatures && CurrentFeatureState == EExperienceFeatureState::Registered));
		bAllSatisfied &= bFeatureSatisfied;
	}

	if (bAllSatisfied)
	{
		WaitForRegistry(Step == ELoadStep::SourceFeatures ? ELoadStep::SourceRegistry : ELoadStep::Registry);
	}
}

void UExperienceSubsystem::WaitForRegistry(ELoadStep Step)
{
	FeatureSubscriptions.DisconnectAll();
	LoadStep = Step;
	IAssetRegistry& AssetRegistry = UAssetManager::Get().GetAssetRegistry();
	const int64 Generation = ExperienceSnapshot.Generation;
	if (AssetRegistry.IsGathering())
	{
		const TWeakObjectPtr<UExperienceSubsystem> WeakThis(this);
		RegistryReadyHandle = AssetRegistry.OnKnownGathersComplete().AddLambda([WeakThis, Generation, Step]
			{
				QueueExperienceCallback(WeakThis, [Generation, Step](UExperienceSubsystem& ExperienceSubsystem)
					{
						ExperienceSubsystem.OnRegistryReady(Generation, Step);
					});
			});
	}

	// 已收集完成时直接推进；否则上面的原生完成通知切回 GameThread 后再推进。
	OnRegistryReady(Generation, Step);
}

void UExperienceSubsystem::OnRegistryReady(int64 Generation, ELoadStep Step)
{
	if (!IsCurrentLoad(Generation, Step))
	{
		return;
	}

	UAssetManager& AssetManager = UAssetManager::Get();
	if (AssetManager.GetAssetRegistry().IsGathering())
	{
		return;
	}

	AssetManager.GetAssetRegistry().OnKnownGathersComplete().Remove(RegistryReadyHandle);
	RegistryReadyHandle.Reset();
	if (Step == ELoadStep::SourceRegistry)
	{
		LoadAssetsBySoftPath({EnabledMode.ModeDef.ToSoftObjectPath()}, ELoadStep::Mode, FSimpleDelegate::CreateUObject(this, &UExperienceSubsystem::OnModeLoaded));
		return;
	}

	// 完整依赖已 Loaded 且登记完成；比赛和前台均在此组装各 World 所需的资源 bundles。
	TArray<FPrimaryAssetId> DefinitionAssetIds{ExperienceDef->GetPrimaryAssetId()};
	if (RulesDef)
	{
		// 本轮只接通开发固定选择；正式系统分配/玩家选择在后续选人流程实现。
		const auto& SelectionRules = RulesDef->HeroSelectionRules;
		if (SelectionRules.Policy != EHeroSelectionPolicy::Fixed)
		{
			BeginStop(EExperienceFailure::UnsupportedSelectionPolicy);
			return;
		}

		if (!SelectionRules.Allows(SelectionRules.FixedHero.ToSoftObjectPath()))
		{
			BeginStop(EExperienceFailure::MissingDefinition);
			return;
		}

		DevelopmentHero = SelectionRules.FixedHero;
		const FPrimaryAssetId HeroId = AssetManager.GetPrimaryAssetIdForPath(DevelopmentHero.ToSoftObjectPath());
		if (!HeroId.IsValid())
		{
			BeginStop(EExperienceFailure::MissingDefinition);
			return;
		}

		DefinitionAssetIds.Add(HeroId);
		DefinitionAssetIds.Add(MapDef->GetPrimaryAssetId());
	}

	// 各 World 自己加载资源：客户端取 Client，专服取 Server，Standalone/Listen Server 两者都取。
	// 这不是网络复制；仅固定英雄加入递归预载，不加载 AllowedHeroes 中的全部候选。
	TArray<FName> BundleNames;
	if (GetWorld()->GetNetMode() != NM_Client)
	{
		BundleNames.Add(TEXT("Server"));
	}

	if (GetWorld()->GetNetMode() != NM_DedicatedServer)
	{
		BundleNames.Add(TEXT("Client"));
	}

	PreloadPrimaryAssetBundles(DefinitionAssetIds, BundleNames, true, ELoadStep::Bundles, FSimpleDelegate::CreateUObject(this, &UExperienceSubsystem::OnBundlesLoaded));
}

void UExperienceSubsystem::OnDefinitionsLoaded()
{
	ExperienceDef = ModeConfig.ExperienceDef.Get();
	MapDef = ModeConfig.MapDef.Get();
	RulesDef = ModeConfig.RulesDef.Get();
	FString Reason;
	if (!ExperienceDef || (!bFrontend && (!MapDef || MapDef->World.IsNull() || !RulesDef || !RulesDef->IsValid(Reason))))
	{
		BeginStop(EExperienceFailure::MissingDefinition);
		return;
	}

	TArray<FString> RequiredFeatureNames = EnabledMode.DefinitionSourceFeatures;
	for (const FString& FeatureName : ExperienceDef->FeatureDependencies)
	{
		RequiredFeatureNames.AddUnique(FeatureName);
	}

	// 仅此处请求一次完整并集的 Loaded；注册来源阶段没有请求 Loaded。
	AcquireFeatures(RequiredFeatureNames, ELoadStep::Feature);
}

void UExperienceSubsystem::OnBundlesLoaded()
{
	// 公共资源/固定 Hero bundle 完成后，按规则补物品资源；无物品则直接进入地图阶段。
	if (!RulesDef || !RulesDef->ItemEconomyRules.bItemsEnabled)
	{
		BeginMapLoading();
		return;
	}

	LoadAssetsBySoftPath({RulesDef->ItemEconomyRules.ItemCatalog.ToSoftObjectPath()}, ELoadStep::ItemCatalog,
		FSimpleDelegate::CreateUObject(this, &UExperienceSubsystem::OnItemCatalogLoaded));
}

bool UExperienceSubsystem::LoadAssetsBySoftPath(const TArray<FSoftObjectPath>& Paths, ELoadStep Step, FSimpleDelegate OnLoaded)
{
	LoadStep = Step;
	const int64 Generation = ExperienceSnapshot.Generation;
	TSharedPtr<FStreamableHandle> AssetLoadHandle = UAssetManager::Get().GetStreamableManager().RequestAsyncLoad(Paths);
	if (!AssetLoadHandle)
	{
		BeginStop(EExperienceFailure::AssetLoadFailed);
		return false;
	}

	Handles.Add(AssetLoadHandle);
	FStreamableDelegate LoadCompleteDelegate = FStreamableDelegate::CreateWeakLambda(this,
		[this, Generation, Step, OnLoaded = MoveTemp(OnLoaded)]
		{
			if (IsCurrentLoad(Generation, Step))
			{
				OnLoaded.ExecuteIfBound();
			}
		});
	// 与 PrimaryAsset 请求一致：先持有句柄，再接入完成通知和已完成快照。
	if (AssetLoadHandle->HasLoadCompleted())
	{
		FStreamableHandle::ExecuteDelegate(MoveTemp(LoadCompleteDelegate));
	}
	else
	{
		AssetLoadHandle->BindCompleteDelegate(MoveTemp(LoadCompleteDelegate));
	}

	return true;
}

void UExperienceSubsystem::OnItemCatalogLoaded()
{
	const UItemCatalogDefinition* ItemCatalogDef = RulesDef->ItemEconomyRules.ItemCatalog.Get();
	if (!ItemCatalogDef || ItemCatalogDef->Items.IsEmpty() || ItemCatalogDef->Items.Num() > 128)
	{
		BeginStop(EExperienceFailure::MissingDefinition);
		return;
	}

	TArray<FSoftObjectPath> ItemAssetPaths;
	for (const auto& ItemDef : ItemCatalogDef->Items)
	{
		if (ItemDef.IsNull())
		{
			BeginStop(EExperienceFailure::MissingDefinition);
			return;
		}

		ItemAssetPaths.AddUnique(ItemDef.ToSoftObjectPath());
	}

	LoadAssetsBySoftPath(ItemAssetPaths, ELoadStep::Items, FSimpleDelegate::CreateUObject(this, &UExperienceSubsystem::OnItemsLoaded));
}

void UExperienceSubsystem::OnItemsLoaded()
{
	UAssetManager& AssetManager = UAssetManager::Get();
	TArray<FSoftObjectPath> AbilitySetAssetPaths;
	for (const auto& ItemDef : RulesDef->ItemEconomyRules.ItemCatalog->Items)
	{
		if (!ItemDef.Get() || ItemDef->AbilitySet.IsNull() || AssetManager.GetPrimaryAssetIdForPath(ItemDef.ToSoftObjectPath()) != ItemDef->GetPrimaryAssetId())
		{
			BeginStop(EExperienceFailure::MissingDefinition);
			return;
		}

		AbilitySetAssetPaths.AddUnique(ItemDef->AbilitySet.ToSoftObjectPath());
	}

	LoadAssetsBySoftPath(AbilitySetAssetPaths, ELoadStep::ItemSets, FSimpleDelegate::CreateUObject(this, &UExperienceSubsystem::OnItemSetsLoaded));
}

void UExperienceSubsystem::OnItemSetsLoaded()
{
	TArray<FSoftObjectPath> AbilityAndEffectClassPaths;
	for (const auto& ItemDef : RulesDef->ItemEconomyRules.ItemCatalog->Items)
	{
		const UAbilitySetDefinition* AbilitySetDef = ItemDef->AbilitySet.Get();
		if (!AbilitySetDef)
		{
			BeginStop(EExperienceFailure::MissingDefinition);
			return;
		}

		for (const auto& Entry : AbilitySetDef->Abilities)
		{
			AbilityAndEffectClassPaths.AddUnique(Entry.Ability.ToSoftObjectPath());
		}

		for (const auto& EffectClass : AbilitySetDef->Effects)
		{
			AbilityAndEffectClassPaths.AddUnique(EffectClass.ToSoftObjectPath());
		}
	}

	if (AbilityAndEffectClassPaths.IsEmpty())
	{
		BeginStop(EExperienceFailure::MissingDefinition);
		return;
	}

	LoadAssetsBySoftPath(AbilityAndEffectClassPaths, ELoadStep::ItemAbilities, FSimpleDelegate::CreateUObject(this, &UExperienceSubsystem::OnItemAbilitiesLoaded));
}

void UExperienceSubsystem::OnItemAbilitiesLoaded()
{
	FString Reason;
	if (!RulesDef->ItemEconomyRules.ItemCatalog->ValidateCatalog(Reason))
	{
		UE_LOG(LogMobaExperience, Warning, TEXT("ItemCatalogRejected Reason=%s"), *Reason);
		BeginStop(EExperienceFailure::MissingDefinition);
		return;
	}

	BeginMapLoading();
}

void UExperienceSubsystem::BeginMapLoading()
{
	if (bFrontend)
	{
		GetWorld()->GetTimerManager().ClearTimer(ContentLoadTimeoutTimer);
		ExperienceSnapshot.State = EExperienceState::ExperienceReady;
		NotifyState();
		return;
	}

	if (!GEngine->GetWorldContextFromWorld(GetWorld()))
	{
		BeginStop(EExperienceFailure::MissingWorldContext);
		return;
	}

	if (!FPackageName::DoesPackageExist(MapDef->World.GetLongPackageName()))
	{
		BeginStop(EExperienceFailure::MapLoadFailed);
		return;
	}

	bool bMapInstanceCreated = false;
	// 联机实例身份由服务器复制；独立测试 World 仍以完整路径和本地代次隔离。
	const FString MapInstanceName = MapNetworkInstanceId.IsValid() ? TEXT("ExperienceMap_") + MapNetworkInstanceId.ToString(EGuidFormats::Digits)
		: FString::Printf(TEXT("ExperienceMap_%08x_%lld"), GetTypeHash(GetWorld()->GetPathName()), ExperienceSnapshot.Generation);
	MapInstance = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(GetWorld(), MapDef->World, FVector::ZeroVector,
															FRotator::ZeroRotator, bMapInstanceCreated, MapInstanceName);
	if (!bMapInstanceCreated || !MapInstance)
	{
		BeginStop(EExperienceFailure::MapLoadFailed);
		return;
	}

	LoadStep = ELoadStep::Map;
	ExperienceSnapshot.State = EExperienceState::Activating;
	StreamingStateHandle = FLevelStreamingDelegates::OnLevelStreamingStateChanged.AddUObject(this, &UExperienceSubsystem::OnMapStreamingStateChanged);
	QueueMapStateCheck();
	NotifyState();
}

void UExperienceSubsystem::OnMapStreamingStateChanged(UWorld* World, const ULevelStreaming* LevelStreaming, ULevel* Level, ELevelStreamingState PreviousState, ELevelStreamingState State)
{
	if (World != GetWorld() || LevelStreaming != MapInstance)
	{
		return;
	}

	if (State == ELevelStreamingState::LoadedVisible || State == ELevelStreamingState::FailedToLoad || State == ELevelStreamingState::Removed)
	{
		QueueMapStateCheck();
	}
}

void UExperienceSubsystem::QueueMapStateCheck()
{
	if (bDeinitializing || GetWorld()->GetTimerManager().TimerExists(MapEventTimer))
	{
		return;
	}

	const int64 Generation = ExperienceSnapshot.Generation;
	const TWeakObjectPtr<ULevelStreamingDynamic> WeakMapStreaming(MapInstance);
	// 原生通知仍在 streaming 转换栈内；下一帧只处理此次事件，不循环检查。
	MapEventTimer = GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this,
		[this, Generation, WeakMapStreaming]
		{
			MapEventTimer.Invalidate();
			if (!bDeinitializing && ExperienceSnapshot.Generation == Generation && WeakMapStreaming.Get() == MapInstance)
			{
				HandleMapState();
			}
		}));
}

void UExperienceSubsystem::HandleMapState()
{
	if (ExperienceSnapshot.State == EExperienceState::Stopping)
	{
		if (MapInstance && MapInstance->GetLevelStreamingState() == ELevelStreamingState::FailedToLoad && !MapInstance->GetLoadedLevel())
		{
			// 原生 FailedToLoad 不自动 removal；只移除本 World 本次失败的无 Level 对象。
			GetWorld()->RemoveStreamingLevel(MapInstance);
		}

		if (!MapInstance || (!MapInstance->IsLevelLoaded() && !GetWorld()->GetStreamingLevels().Contains(MapInstance)))
		{
			FinishStop();
		}

		return;
	}

	if (!IsCurrentLoad(ExperienceSnapshot.Generation, ELoadStep::Map) || !MapInstance)
	{
		return;
	}

	if (MapInstance->GetLevelStreamingState() == ELevelStreamingState::FailedToLoad)
	{
		BeginStop(EExperienceFailure::MapLoadFailed);
		return;
	}

	if (!MapInstance->IsLevelLoaded() || !MapInstance->IsLevelVisible())
	{
		return;
	}

	FString Reason;
	if (!RulesDef->Players.IsValid(RulesDef->TeamRules.Teams.Num(), Reason) || UMapDefinition::CountPlayerStarts(MapInstance->GetLoadedLevel()) < RulesDef->Players.MaximumParticipants)
	{
		BeginStop(EExperienceFailure::InvalidMapAssociation);
		return;
	}

	GetWorld()->GetTimerManager().ClearTimer(ContentLoadTimeoutTimer);
	ExperienceSnapshot.State = EExperienceState::ExperienceReady;
	NotifyState();
}

void UExperienceSubsystem::ClearLoadSubscriptions()
{
	GetWorld()->GetTimerManager().ClearTimer(ContentLoadTimeoutTimer);
	FeatureSubscriptions.DisconnectAll();
	if (RegistryReadyHandle.IsValid())
	{
		UAssetManager::Get().GetAssetRegistry().OnKnownGathersComplete().Remove(RegistryReadyHandle);
		RegistryReadyHandle.Reset();
	}
}

void UExperienceSubsystem::BeginStop(EExperienceFailure Failure)
{
	++ExperienceSnapshot.Generation;
	ExperienceSnapshot.Failure = Failure;
	ExperienceSnapshot.State = EExperienceState::Stopping;
	ClearLoadSubscriptions();
	GetWorld()->GetTimerManager().ClearTimer(MapEventTimer);
	for (const TSharedPtr<FStreamableHandle>& AssetLoadHandle : Handles)
	{
		if (AssetLoadHandle && !AssetLoadHandle->HasLoadCompleted())
		{
			AssetLoadHandle->CancelHandle();
		}
	}

	if (!MapInstance)
	{
		FinishStop();
		return;
	}

	MapInstance->SetShouldBeVisible(false);
	MapInstance->SetShouldBeLoaded(false);
	MapInstance->SetIsRequestingUnloadAndRemoval(true);
	QueueMapStateCheck();
	NotifyState();
}

void UExperienceSubsystem::FinishStop()
{
	ReleaseResources();
	ExperienceSnapshot.State = ExperienceSnapshot.Failure == EExperienceFailure::None ? EExperienceState::Unloaded : EExperienceState::Failed;
	NotifyState();
}

void UExperienceSubsystem::ReleaseResources()
{
	ClearLoadSubscriptions();
	GetWorld()->GetTimerManager().ClearTimer(MapEventTimer);
	FLevelStreamingDelegates::OnLevelStreamingStateChanged.Remove(StreamingStateHandle);
	StreamingStateHandle.Reset();
	for (const TSharedPtr<FStreamableHandle>& AssetLoadHandle : Handles)
	{
		if (AssetLoadHandle)
		{
			if (AssetLoadHandle->HasLoadCompleted())
			{
				AssetLoadHandle->ReleaseHandle();
			}
			else
			{
				AssetLoadHandle->CancelHandle();
			}
		}
	}

	Handles.Empty();
	MapInstance = nullptr;
	ExperienceDef = nullptr;
	MapDef = nullptr;
	ModeDef = nullptr;
	RulesDef = nullptr;
	DevelopmentHero.Reset();
	ModeConfig = FModeConfiguration();
	EnabledMode = FEnabledModeEntry();
	bUseFirstModeConfig = false;
	if (FeatureLease.IsValid())
	{
		// Editor 的资产编辑器与其他 World 可以继续持有插件内容；包括编辑器内 Standalone。
		// Deinitialize 没有正常 Stop 的地图移除屏障，所有 World 类型都禁止在此同步 unregister。
		const bool bKeepLoaded = GIsEditor || bDeinitializing || GetWorld()->bIsTearingDown || GetWorld()->IsBeingCleanedUp() || GetWorld()->WorldType == EWorldType::PIE;
		for (const FString& URL : FeatureURLs)
		{
			GEngine->GetEngineSubsystem<UExperienceFeatureSubsystem>()->Release(URL, FeatureLease,
				bKeepLoaded ? EExperienceFeatureReleasePolicy::KeepLoaded : EExperienceFeatureReleasePolicy::UnloadWhenUnused);
		}

		FeatureLease.Invalidate();
		FeatureURLs.Empty();
	}
}

void UExperienceSubsystem::NotifyState() const
{
	UE_LOG(LogMobaExperience, Display, TEXT("Experience World=%s Generation=%lld State=%s Failure=%s Mode=%s Instance=%s MapPackage=%s"), 
		*GetWorld()->GetName(), ExperienceSnapshot.Generation, *UEnum::GetValueAsString(ExperienceSnapshot.State), *UEnum::GetValueAsString(ExperienceSnapshot.Failure), *ExperienceSnapshot.ModeId.ToString(),
		*MapNetworkInstanceId.ToString(), MapInstance ? *MapInstance->GetWorldAssetPackageName() : TEXT("None"));
	
	// GMP 同步回调可重入 Stop/Start；提交全部状态后派发值副本，通知后不推进旧请求。
	const FExperienceSnapshot CommittedExperienceSnapshot = ExperienceSnapshot;
	GMP::FMessageUtils::SendObjectMessage(this, MOBA_MESSAGE_EXPERIENCE_CHANGED, CommittedExperienceSnapshot);
}

bool UExperienceSubsystem::HasOwnedResources() const
{
	return !Handles.IsEmpty() || FeatureLease.IsValid() || MapInstance || ExperienceDef || MapDef || ModeDef || RulesDef || RegistryReadyHandle.IsValid() || StreamingStateHandle.IsValid()
		|| ContentLoadTimeoutTimer.IsValid() || MapEventTimer.IsValid();
}

const UHeroDefinition* UExperienceSubsystem::GetDevelopmentHero() const
{
	return DevelopmentHero.Get();
}

