#pragma once

#include "CoreMinimal.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "Tickable.h"
#include "ClientPresentationSubsystem.generated.h"

class UUserWidget;
class UClientUIConfiguration;

USTRUCT()
struct FClientPresentationLayer
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf<UUserWidget> WidgetClass;

	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> Widget;

	int32 ZOrder = 0;
	bool bAllowPointerHit = false;
	bool bMatchOnly = true;
};

/** 每个本地玩家独立持有 UI 层；比赛层离场即撤销，不保存权威 Gameplay 状态。 */
UCLASS(Config=Game)
class MOBAGAMEUI_API UClientPresentationSubsystem : public ULocalPlayerSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	UClientPresentationSubsystem();

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	virtual void Deinitialize() override;

	virtual void BeginDestroy() override;

	virtual void PlayerControllerChanged(APlayerController* NewPlayerController) override;

	virtual void Tick(float DeltaTime) override;

	virtual bool IsTickable() const override;

	virtual UWorld* GetTickableGameObjectWorld() const override;

	virtual TStatId GetStatId() const override;


public:
	// 表现层注册
	/**
	 * 仅 Game Thread；注册描述留存至注销/Deinitialize，实例由挂载器按当前 World 创建。
	 * 同名同类同层级同指针命中策略为幂等成功，冲突/无效类/生命周期回调内重入返回 false。
	 * 成功不等于已显示；无本地 Controller 或视口时留待后续 Tick。
	 * bAllowPointerHit 仅允许子控件参与指针命中，不授予 Gameplay 操作或键盘焦点权限。
	 * bMatchOnly 层仅在当前比赛 World 内容就绪且未离场时挂载；前台流程层显式设为 false。
	 */
	bool RegisterLayer(FName LayerName, TSubclassOf<UUserWidget> WidgetClass, int32 ZOrder = 0, bool bAllowPointerHit = false, bool bMatchOnly = true);

	/** 仅 Game Thread；不存在时也成功，生命周期回调内重入失败。同步撤销当前实例。 */
	bool UnregisterLayer(FName LayerName);


private:
	// 视口与 World 生命周期
	void ApplyMatchInputMode();

	void RefreshLayers();

	void RemoveLayerWidgets();

	void OnWorldBeginTearDown(UWorld* World);

	void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);


public:
	/** 未挂载返回 nullptr；返回值只供本帧观察，不应跨 World/Controller 更换持有强引用。 */
	UUserWidget* FindLayer(FName LayerName) const;

	const UClientUIConfiguration* GetConfiguration() const { return Configuration; }


private:
	// 本地玩家初始化时仅加载一次；四个页面类及子项均由资产硬引用。
	UPROPERTY(Config)
	TSoftObjectPtr<UClientUIConfiguration> ConfigurationAsset;

	UPROPERTY(Transient)
	TObjectPtr<UClientUIConfiguration> Configuration;

	UPROPERTY(Transient)
	TMap<FName, FClientPresentationLayer> Layers;

	TWeakObjectPtr<APlayerController> MountedController;
	TWeakObjectPtr<UWorld> MountedWorld;
	TWeakObjectPtr<UWorld> RetiredWorld;
	FDelegateHandle WorldTearDownHandle;
	FDelegateHandle WorldCleanupHandle;
	bool bInitialized = false;
	bool bChangingLayers = false;
};
