#include "Player/HeroPlayerController.h"
#include "Combat/AbilityTargeting.h"
#include "Items/ItemInventoryComponent.h"

#include "Match/ParticipantState.h"
#include "Match/MatchGameState.h"
#include "Match/MatchGameMode.h"
#include "Hero/HeroCharacter.h"
#include "Hero/HeroAttributeComponent.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Abilities/GameplayAbility.h"
#include "Combat/TargetDamageAbility.h"
#include "Experience/ExperienceDefinitions.h"
#include "Hero/HeroDefinition.h"
#include "Experience/ExperienceSubsystem.h"
#include "Async/Async.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Engine/LocalPlayer.h"
#include "EngineUtils.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MobaGameplayTags.h"
#include "MobaLogChannels.h"
#include "MobaMessageTags.h"
#include "Session/MobaSessionSubsystem.h"
#include "Session/GameFlowSubsystem.h"
#include "Engine/GameInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/DemoNetDriver.h"

AHeroPlayerController::AHeroPlayerController()
{
	bShowMouseCursor = true;
	PrimaryActorTick.bCanEverTick = true;
}

void AHeroPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (IsLocalController() && GetNetMode() == NM_Client)
	{
		GetGameInstance()->GetSubsystem<UGameFlowSubsystem>()->ObserveMatch(GetWorld());
	}

	if (UExperienceSubsystem* ExperienceSubsystem = GetWorld()->GetSubsystem<UExperienceSubsystem>())
	{
		const TWeakObjectPtr<AHeroPlayerController> WeakThis(this);
		GMP::FMessageUtils::ListenObjectMessage(ExperienceSubsystem, MOBA_MESSAGE_EXPERIENCE_CHANGED, this,
			[WeakThis](const FExperienceSnapshot&)
			{
				// 离开广播栈后读取当前状态，避免重入准入流程；不保留临时消息引用。
				AsyncTask(ENamedThreads::GameThread,
					[WeakThis]
					{
						if (AHeroPlayerController* Controller = WeakThis.Get())
						{
							Controller->RefreshExperienceLoading();
						}
					});
			});
	}

	RefreshExperienceLoading();
}

void AHeroPlayerController::ReceivedPlayer()
{
	Super::ReceivedPlayer();
	RefreshExperienceLoading();
}

void AHeroPlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!IsLocalController())
	{
		return;
	}

	if (GetGameInstance()->GetSubsystem<UGameFlowSubsystem>()->GetSnapshot().State == EGameFlowState::ReturningFrontend)
	{
		return;
	}

	UExperienceSubsystem* ExperienceSubsystem = GetWorld()->GetSubsystem<UExperienceSubsystem>();
	AMatchGameState* MatchGameState = GetWorld()->GetGameState<AMatchGameState>();
	if (!MatchGameState || !MatchGameState->MatchId.IsValid())
	{
		return;
	}

	if (!bHandshakeSent)
	{
		bHandshakeSent = true;
#if WITH_DEV_AUTOMATION_TESTS
		if (!FParse::Param(FCommandLine::Get(), TEXT("MobaManualAdmission")))
#endif
		{
			UMobaSessionSubsystem* SessionSubsystem = GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>();
			FResumeCredential Credential;
			const bool bResume = SessionSubsystem->RequiresResume();
			if (bResume && !SessionSubsystem->LoadCredential(Credential))
			{
				ClientAdmissionResult_Implementation(FGuid(), TEXT("CredentialMissingOrUnreadable"));
				return;
			}

			if (bResume && Credential.Match != MatchGameState->MatchId)
			{
				Credential.Clear();
				ClientAdmissionResult_Implementation(FGuid(), TEXT("WrongMatch"));
				return;
			}

			ServerBeginAdmission(bResume, Credential);
			Credential.Clear();
		}
	}

	if (AdmissionResult != TEXT("Bound") || MatchGameState->bMatchEnded || ExperienceSubsystem->GetSnapshot().State != EExperienceState::ExperienceReady)
	{
		ClearLocalInput();
		return;
	}

	if (!AppliedProfile || !BoundInputComponent.IsValid() || BoundInputComponent.Get() != InputComponent.Get())
	{
		return;
	}

	AHeroCharacter* Hero = Cast<AHeroCharacter>(GetPawn());
	AParticipantState* ParticipantState = GetPlayerState<AParticipantState>();
	if (!IsCurrentHeroBinding(Hero))
	{
		return;
	}

	UHeroAbilitySystemComponent* HeroASC = Hero->GetHeroAbilitySystem();
	if (SentReadyRevision != Hero->RequiredLoadoutRevision || SentReadyEpoch != Hero->ConnectionEpoch)
	{
		bPlayerReadySent = false;
		ClearLocalInput();
	}

	if (!HeroASC->AbilityActorInfo.IsValid() || HeroASC->AbilityActorInfo->PlayerController.Get() != this)
	{
		Hero->RefreshActorInfo();
	}

	if (!IsHeroLoadoutReady(Hero))
	{
		return;
	}

	if (!bPlayerReadySent)
	{
		bPlayerReadySent = true;
		SentReadyRevision = Hero->RequiredLoadoutRevision;
		SentReadyEpoch = Hero->ConnectionEpoch;
		ServerPlayerReady(Hero->EntityId, ParticipantState->ConnectionEpoch, Hero->RequiredLoadoutRevision);
	}

	if (!IsLocallyReady())
	{
		ClearLocalInput();
		return;
	}

	if (!bAnnouncedReady)
	{
		bAnnouncedReady = true;
		GetGameInstance()->GetSubsystem<UGameFlowSubsystem>()->NotifyMatchReady(GetWorld());
		UE_LOG(LogMobaControl, Display, TEXT("ClientPlayerReady Match=%s Instance=%s Participant=%s Entity=%s Epoch=%u Revision=%u ASC=%s"), *ParticipantState->MatchId.ToString(),
			*MatchGameState->ExperienceInstanceId.ToString(), *ParticipantState->ParticipantId.ToString(), *Hero->EntityId.ToString(), Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision,
			*HeroASC->GetPathName());
	}

	const auto* TargetSpec = HeroASC->FindAbilitySpecFromHandle(HeroASC->FindSlot(MobaGameplayTags::Input_Slot_Target));
	const auto* TargetAbility = TargetSpec ? Cast<UTargetDamageAbility>(TargetSpec->Ability) : nullptr;
	FAbilityTargetIntent PreviewTargetIntent;
	if (TargetAbility && BuildCursorIntent(TargetAbility->GetGeometry(), PreviewTargetIntent))
	{
		MobaTargeting::DrawIndicator(GetWorld(), Hero->GetActorLocation(), TargetAbility->GetGeometry(), PreviewTargetIntent);
	}

	if (ActiveProfile == EHeroInputProfile::ClickToMove && ClickPath.IsValidIndex(PathIndex))
	{
		FVector Direction = ClickPath[PathIndex] - Hero->GetActorLocation();
		Direction.Z = 0;
		if (Direction.SizeSquared() < FMath::Square(35.0f))
		{
			++PathIndex;
		}
		else
		{
			Hero->AddMovementInput(Direction.GetSafeNormal());
		}
	}
}

void AHeroPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	
	InstallInputBindings();
}

void AHeroPlayerController::PawnLeavingGame()
{
	if (AMatchGameMode* MatchGameMode = GetWorld()->GetAuthGameMode<AMatchGameMode>(); MatchGameMode && !GetWorld()->bIsTearingDown)
	{
		MatchGameMode->DetachConnection(this);
		return;
	}

	Super::PawnLeavingGame();
}

void AHeroPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_EXPERIENCE_CHANGED, this);
	PrepareForFrontend();

	Super::EndPlay(EndPlayReason);
}

void AHeroPlayerController::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHeroPlayerController, ActiveProfile);
	DOREPLIFETIME(AHeroPlayerController, ProfileRevision);
}

bool AHeroPlayerController::CallRemoteFunction(UFunction* Function, void* Parameters, FOutParmRec* OutParms, FFrame* Stack)
{
	if (Function->GetFName() != GET_FUNCTION_NAME_CHECKED(AHeroPlayerController, ServerBeginAdmission)
		&& Function->GetFName() != GET_FUNCTION_NAME_CHECKED(AHeroPlayerController, ClientStoreCredential))
	{
		return Super::CallRemoteFunction(Function, Parameters, OutParms, Stack);
	}

	// 即使启用录制unicast RPC，票据也只经过实际游戏连接，永不送入DemoNetDriver。
	bool bSent = false;
	if (const FWorldContext* Context = GEngine->GetWorldContextFromWorld(GetWorld()))
	{
		for (const FNamedNetDriver& Driver : Context->ActiveNetDrivers)
		{
			if (Driver.NetDriver && !Driver.NetDriver->IsA<UDemoNetDriver>() && Driver.NetDriver->ShouldReplicateFunction(this, Function))
			{
				Driver.NetDriver->ProcessRemoteFunction(this, Function, Parameters, OutParms, Stack, nullptr);
				bSent = true;
			}
		}
	}

	return bSent;
}

void AHeroPlayerController::RefreshExperienceLoading()
{
	if (!IsLocalController() || (!HasActorBegunPlay() && !IsActorBeginningPlay()) || GetWorld()->bIsTearingDown || IsActorBeingDestroyed())
	{
		return;
	}

	UGameFlowSubsystem* GameFlowSubsystem = GetGameInstance()->GetSubsystem<UGameFlowSubsystem>();
	if (GameFlowSubsystem->GetSnapshot().State == EGameFlowState::ReturningFrontend)
	{
		return;
	}

	AMatchGameState* MatchGameState = GetWorld()->GetGameState<AMatchGameState>();
	if (MatchGameState && MatchGameState->bMatchEnded)
	{
		GameFlowSubsystem->NotifyMatchEnded(GetWorld());
		return;
	}

	UExperienceSubsystem* ExperienceSubsystem = GetWorld()->GetSubsystem<UExperienceSubsystem>();
	if (ExperienceSubsystem && ExperienceSubsystem->GetSnapshot().State == EExperienceState::ExperienceReady)
	{
		InstallInputBindings();
	}

	if (!MatchGameState || !ExperienceSubsystem || !MatchGameState->MatchId.IsValid() || !MatchGameState->ExperienceInstanceId.IsValid())
	{
		return;
	}

	if (!HasAuthority() && !MatchGameState->bMatchEnded && CanLoadContent() && MatchGameState->Selection.ModeId.IsValid() && ExperienceSubsystem->GetSnapshot().State == EExperienceState::Unloaded)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (!FParse::Param(FCommandLine::Get(), TEXT("MobaManualExperience")))
#endif
		{
			const EExperienceFailure Result = ExperienceSubsystem->StartExperience(MatchGameState->Selection, MatchGameState->ExperienceInstanceId);
			if (Result != EExperienceFailure::None && AdmissionRequest.IsValid() && !bAdmissionReadySent)
			{
				bAdmissionReadySent = true;
				ServerAdmissionReady(AdmissionRequest, MatchGameState->ExperienceInstanceId, false);
			}
		}
	}

	// StartExperience 可同步失败并通知；每次读取当前快照，发送标志先于 RPC 提交。
	if (AdmissionRequest.IsValid() && !bAdmissionReadySent)
	{
		const EExperienceState ContentState = ExperienceSubsystem->GetSnapshot().State;
		if (bCredentialStoreFailed || ContentState == EExperienceState::Failed)
		{
			bAdmissionReadySent = true;
			ServerAdmissionReady(AdmissionRequest, MatchGameState->ExperienceInstanceId, false);
		}
		else if (ContentState == EExperienceState::ExperienceReady
#if WITH_DEV_AUTOMATION_TESTS
			&& !FParse::Param(FCommandLine::Get(), TEXT("MobaHoldAdmission"))
#endif
		)
		{
			bAdmissionReadySent = true;
			ServerAdmissionReady(AdmissionRequest, MatchGameState->ExperienceInstanceId, true);
		}
	}

	if (AdmissionResult == TEXT("Bound") && !MatchGameState->bMatchEnded && ExperienceSubsystem->GetSnapshot().State == EExperienceState::ExperienceReady && !bContentReadySent)
	{
		bContentReadySent = true;
		ServerContentReady(MatchGameState->ExperienceInstanceId);
	}
}

void AHeroPlayerController::PrepareForFrontend()
{
	if (!IsLocalController())
	{
		return;
	}

	ClearLocalInput();
	if (AppliedProfile && GetLocalPlayer())
	{
		if (auto* InputSubsystem = GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			InputSubsystem->RemoveMappingContext(AppliedProfile->Mapping.Get());
			if (AppliedProfile->TargetMapping.IsValid())
			{
				InputSubsystem->RemoveMappingContext(AppliedProfile->TargetMapping.Get());
			}
		}
	}

	AppliedProfile = nullptr;
	if (UEnhancedInputComponent* EnhancedInputComp = BoundInputComponent.Get())
	{
		EnhancedInputComp->ClearBindingsForObject(this);
	}

	BoundInputComponent.Reset();
}

void AHeroPlayerController::ServerBeginAdmission_Implementation(bool bResume, const FResumeCredential& Credential)
{
	if (auto* MatchGameMode = GetWorld()->GetAuthGameMode<AMatchGameMode>())
	{
		MatchGameMode->BeginAdmission(this, bResume, Credential);
	}
}

void AHeroPlayerController::ServerAdmissionReady_Implementation(FGuid AdmissionRequestId, FGuid ExperienceInstanceId, bool bLoaded)
{
	if (auto* MatchGameMode = GetWorld()->GetAuthGameMode<AMatchGameMode>())
	{
		MatchGameMode->CompleteAdmission(this, AdmissionRequestId, ExperienceInstanceId, bLoaded);
	}
}

void AHeroPlayerController::ClientStoreCredential_Implementation(const FResumeCredential& Credential)
{
	bCredentialStoreFailed = !GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>()->SaveCredential(Credential);
	UE_LOG(LogMobaControl, Display, TEXT("CredentialStorage Protected=%d"), !bCredentialStoreFailed);
	RefreshExperienceLoading();
}

void AHeroPlayerController::ClientAdmissionResult_Implementation(FGuid AdmissionRequestId, FName Result)
{
	// 已提交绑定不被旧握手回调降级；Match结束是明确的撤销事件。
	if (AdmissionResult == TEXT("Bound") && Result != TEXT("MatchEnded") && AdmissionRequestId != AdmissionRequest)
	{
		return;
	}

	if (!Result.IsNone() && Result != TEXT("Bound") && Result != TEXT("ContentNotReady") && Result != TEXT("MatchEnded"))
	{
		GetGameInstance()->GetSubsystem<UGameFlowSubsystem>()->NotifyAdmissionFailure(GetWorld(), Result);
	}

	AdmissionResult = Result;
	if (Result.IsNone())
	{
		AdmissionRequest = AdmissionRequestId;
	}
	else if (Result != TEXT("Bound"))
	{
		AdmissionRequest.Invalidate();
		ClearLocalInput();
	}

	UE_LOG(LogMobaControl, Display, TEXT("AdmissionResult Result=%s Request=%s"), *Result.ToString(), *AdmissionRequestId.ToString());
	RefreshExperienceLoading();
}

void AHeroPlayerController::ClientBindingState_Implementation(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LifeGeneration, uint32 DeathCount)
{
	BoundEntity = HeroEntityId;
	BoundEpoch = ConnectionEpoch;
	BoundLifeGeneration = LifeGeneration;
	BoundDeathCount = DeathCount;
	ClearLocalInput();
}

void AHeroPlayerController::MobaReconnect()
{
	if (!IsLocalController())
	{
		return;
	}

	GetGameInstance()->GetSubsystem<UMobaSessionSubsystem>()->RequireResume();
	ClearLocalInput();
	// 原生非无缝travel关闭旧连接并建立新Controller；URL仅包含服务器地址。
	const FString Address = GetWorld()->URL.Host + TEXT(":") + FString::FromInt(GetWorld()->URL.Port);
	ClientTravel(Address, TRAVEL_Absolute);
}

void AHeroPlayerController::ServerItemRequest_Implementation(const FItemRequest& Request)
{
	auto* Hero = Cast<AHeroCharacter>(GetPawn());
	FItemResult Result;
	Result.Epoch = Request.Epoch;
	Result.Sequence = Request.Sequence;
	Result.Reason = TEXT("Binding");
	if (Hero && Hero->GetInventory())
	{
		Result = Hero->GetInventory()->Execute(this, Request);
	}

	ClientItemResult(Result);
}

void AHeroPlayerController::ClientItemResult_Implementation(const FItemResult& Result)
{
	LastItemResult = Result;
	UE_LOG(LogMobaControl, Display, TEXT("ItemResult Epoch=%u Sequence=%u Revision=%u Accepted=%d Reason=%s"), Result.Epoch, Result.Sequence, Result.Revision, Result.bAccepted,
		*Result.Reason.ToString());
}

void AHeroPlayerController::MobaBuy(const FString& ItemId, int32 Quantity)
{
	RequestItem(EItemOperation::Buy, FName(*ItemId), {}, Quantity);
}

void AHeroPlayerController::MobaSell(const FString& InstanceId)
{
	FGuid ItemInstanceId;
	FGuid::Parse(InstanceId, ItemInstanceId);
	RequestItem(EItemOperation::Sell, NAME_None, ItemInstanceId, 1);
}

void AHeroPlayerController::MobaCombine(const FString& ItemId)
{
	RequestItem(EItemOperation::Combine, FName(*ItemId), {}, 1);
}

void AHeroPlayerController::MobaUseItem(const FString& InstanceId)
{
	FGuid ItemInstanceId;
	FGuid::Parse(InstanceId, ItemInstanceId);
	RequestItem(EItemOperation::Use, NAME_None, ItemInstanceId, 1);
}

void AHeroPlayerController::MobaItems()
{
	const auto* Hero = Cast<AHeroCharacter>(GetPawn());
	const auto* ParticipantState = GetPlayerState<AParticipantState>();
	if (!Hero || !ParticipantState)
	{
		return;
	}

	const auto& Inventory = Hero->GetInventory()->GetSnapshot();
	const auto& Economy = ParticipantState->GetEconomy();
	UE_LOG(LogMobaControl, Display, TEXT("Items Entity=%s Gold=%d Revision=%u EconomyRevision=%u Coherent=%d Slots=%d"), *Hero->EntityId.ToString(), Economy.Gold, Inventory.Revision,
		Economy.Revision, Inventory.Revision == Economy.Revision, Inventory.Items.Num());
	for (const auto& Item : Inventory.Items)
	{
		UE_LOG(LogMobaControl, Display, TEXT("Item Slot=%d Definition=%s Instance=%s Count=%d"), Item.Slot, *Item.ItemId.ToString(), *Item.Id.ToString(), Item.Quantity);
	}
}

bool AHeroPlayerController::ValidateIntent(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision, FString& Reason) const
{
	const AParticipantState* ParticipantState = GetPlayerState<AParticipantState>();
	const AHeroCharacter* Hero = Cast<AHeroCharacter>(GetPawn());
	if (!HasAuthority() || !ParticipantState || !ParticipantState->bConnected || !Hero || ParticipantState->PrimaryHero != Hero || Hero->GetController() != this)
	{
		Reason = TEXT("Binding");
		return false;
	}

	if (HeroEntityId != Hero->EntityId)
	{
		Reason = TEXT("Entity");
		return false;
	}

	if (ConnectionEpoch != ParticipantState->ConnectionEpoch || ConnectionEpoch != Hero->ConnectionEpoch)
	{
		Reason = TEXT("Epoch");
		return false;
	}

	if (LoadoutRevision != Hero->RequiredLoadoutRevision || LoadoutRevision != Hero->GetHeroAbilitySystem()->GetLoadoutRevision())
	{
		Reason = TEXT("Revision");
		return false;
	}

	if (!Hero->IsAlive())
	{
		Reason = TEXT("LifeState");
		return false;
	}

	if (!bContentReady || !Hero->CanAcceptPlayerInput())
	{
		Reason = TEXT("NotReady");
		return false;
	}

	return true;
}

bool AHeroPlayerController::IsLocallyReady() const
{
	if (!IsLocalController() || !AppliedProfile || bProfileSwitchPending || !BoundInputComponent.IsValid() || BoundInputComponent.Get() != InputComponent.Get())
	{
		return false;
	}

	const AMatchGameState* MatchGameState = GetWorld()->GetGameState<AMatchGameState>();
	const UExperienceSubsystem* ExperienceSubsystem = GetWorld()->GetSubsystem<UExperienceSubsystem>();
	if (!MatchGameState || MatchGameState->bMatchEnded || AdmissionResult != TEXT("Bound") || !ExperienceSubsystem || ExperienceSubsystem->GetSnapshot().State != EExperienceState::ExperienceReady)
	{
		return false;
	}

	const AHeroCharacter* Hero = Cast<AHeroCharacter>(GetPawn());
	return IsCurrentHeroBinding(Hero) && Hero->CanAcceptPlayerInput() && IsHeroLoadoutReady(Hero) && bPlayerReadySent
		&& SentReadyRevision == Hero->RequiredLoadoutRevision && SentReadyEpoch == Hero->ConnectionEpoch;
}

void AHeroPlayerController::ServerContentReady_Implementation(FGuid ExperienceInstanceId)
{
	const AMatchGameState* MatchGameState = GetWorld()->GetGameState<AMatchGameState>();
	const auto* ParticipantState = GetPlayerState<AParticipantState>();
	if (!ParticipantState || !ParticipantState->bConnected || ParticipantState->GetOwner() != this || !bContentReady || !MatchGameState || !ExperienceInstanceId.IsValid() || ExperienceInstanceId != MatchGameState->ExperienceInstanceId)
	{
		LogRejected(TEXT("ContentReady"), TEXT("InstanceBinding"));
		return;
	}

	bContentReady = true;
	if (AMatchGameMode* MatchGameMode = GetWorld()->GetAuthGameMode<AMatchGameMode>())
	{
		MatchGameMode->TryPrepareHero(this);
	}
}

void AHeroPlayerController::ServerPlayerReady_Implementation(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision)
{
	AParticipantState* ParticipantState = GetPlayerState<AParticipantState>();
	AHeroCharacter* Hero = Cast<AHeroCharacter>(GetPawn());
	if (!ParticipantState || !ParticipantState->bConnected || !Hero || ParticipantState->PrimaryHero != Hero || !bContentReady || HeroEntityId != Hero->EntityId || ConnectionEpoch != ParticipantState->ConnectionEpoch
		|| ConnectionEpoch != Hero->ConnectionEpoch || LoadoutRevision != Hero->RequiredLoadoutRevision || !Hero->GetHeroAbilitySystem()->HasReplicatedLoadout(LoadoutRevision))
	{
		LogRejected(TEXT("PlayerReady"), TEXT("BindingRevision"));
		return;
	}

	if (Hero->bInputReady)
	{
		return;
	}

	Hero->bInputReady = true;
	Hero->ForceNetUpdate();
	UE_LOG(LogMobaControl, Display, TEXT("ServerPlayerReady Participant=%s Entity=%s Epoch=%u Revision=%u"), *ParticipantState->ParticipantId.ToString(), *HeroEntityId.ToString(), ConnectionEpoch, LoadoutRevision);
}

bool AHeroPlayerController::IsCurrentHeroBinding(const AHeroCharacter* Hero) const
{
	const AMatchGameState* MatchGameState = GetWorld()->GetGameState<AMatchGameState>();
	const AParticipantState* ParticipantState = GetPlayerState<AParticipantState>();
	return MatchGameState && MatchGameState->MatchId.IsValid() && Hero && Hero == GetPawn() && ParticipantState && ParticipantState->PrimaryHero == Hero
		&& ParticipantState->MatchId == MatchGameState->MatchId && Hero->InitializationGeneration > 0 && ParticipantState->ConnectionEpoch == Hero->ConnectionEpoch
		&& Hero->EntityId == BoundEntity && Hero->ConnectionEpoch == BoundEpoch && Hero->GetLifeSnapshot().Generation >= BoundLifeGeneration
		&& Hero->GetLifeSnapshot().DeathCount >= BoundDeathCount;
}

bool AHeroPlayerController::IsHeroLoadoutReady(const AHeroCharacter* Hero) const
{
	if (!Hero)
	{
		return false;
	}

	const UHeroAbilitySystemComponent* HeroASC = Hero->GetHeroAbilitySystem();
	return HeroASC->HasReplicatedLoadout(Hero->RequiredLoadoutRevision) && HeroASC->AbilityActorInfo.IsValid()
		&& HeroASC->AbilityActorInfo->OwnerActor.Get() == Hero && HeroASC->AbilityActorInfo->AvatarActor.Get() == Hero
		&& HeroASC->AbilityActorInfo->PlayerController.Get() == this;
}

void AHeroPlayerController::InstallInputBindings()
{
	if (!IsLocalController() || !GetLocalPlayer() || GetWorld()->bIsTearingDown || IsActorBeingDestroyed())
	{
		return;
	}

	UEnhancedInputComponent* EnhancedInputComp = Cast<UEnhancedInputComponent>(InputComponent);
	const UExperienceSubsystem* ExperienceSubsystem = GetWorld()->GetSubsystem<UExperienceSubsystem>();
	if (!EnhancedInputComp || !ExperienceSubsystem || ExperienceSubsystem->GetSnapshot().State != EExperienceState::ExperienceReady)
	{
		return;
	}

	if (BoundInputComponent.Get() == EnhancedInputComp)
	{
		if (!AppliedProfile)
		{
			ApplyProfile();
		}

		return;
	}

	const UExperienceDefinition* ExperienceDef = ExperienceSubsystem->GetExperience();
	if (!ExperienceDef || ExperienceDef->InputProfiles.IsEmpty())
	{
		return;
	}

	for (const auto& InputProfileDef : ExperienceDef->InputProfiles)
	{
		if (!InputProfileDef.IsValid() || !InputProfileDef->IsLoaded())
		{
			return;
		}
	}

	TSet<const UInputAction*> BoundInputActions;
	for (const auto& InputProfileDef : ExperienceDef->InputProfiles)
	{
		if (InputProfileDef->Move.IsValid() && !BoundInputActions.Contains(InputProfileDef->Move.Get()))
		{
			EnhancedInputComp->BindAction(InputProfileDef->Move.Get(), ETriggerEvent::Triggered, this, &ThisClass::OnMove);
			BoundInputActions.Add(InputProfileDef->Move.Get());
		}

		if (InputProfileDef->Click.IsValid() && !BoundInputActions.Contains(InputProfileDef->Click.Get()))
		{
			EnhancedInputComp->BindAction(InputProfileDef->Click.Get(), ETriggerEvent::Started, this, &ThisClass::OnClick);
			BoundInputActions.Add(InputProfileDef->Click.Get());
		}

		if (!BoundInputActions.Contains(InputProfileDef->SwitchProfile.Get()))
		{
			EnhancedInputComp->BindAction(InputProfileDef->SwitchProfile.Get(), ETriggerEvent::Started, this, &ThisClass::OnSwitch);
			BoundInputActions.Add(InputProfileDef->SwitchProfile.Get());
		}

		if (!BoundInputActions.Contains(InputProfileDef->PrimaryAbility.Get()))
		{
			EnhancedInputComp->BindAction(InputProfileDef->PrimaryAbility.Get(), ETriggerEvent::Started, this, &ThisClass::OnAbility);
			BoundInputActions.Add(InputProfileDef->PrimaryAbility.Get());
		}

		if (InputProfileDef->TargetAbility.IsValid() && !BoundInputActions.Contains(InputProfileDef->TargetAbility.Get()))
		{
			EnhancedInputComp->BindAction(InputProfileDef->TargetAbility.Get(), ETriggerEvent::Started, this, &ThisClass::OnTargetAbility);
			BoundInputActions.Add(InputProfileDef->TargetAbility.Get());
		}

		if (!BoundInputActions.Contains(InputProfileDef->UnitAttack.Get()))
		{
			EnhancedInputComp->BindAction(InputProfileDef->UnitAttack.Get(), ETriggerEvent::Started, this, &ThisClass::OnUnitAttack);
			BoundInputActions.Add(InputProfileDef->UnitAttack.Get());
		}
	}

	BoundInputComponent = EnhancedInputComp;
	ApplyProfile();
}

void AHeroPlayerController::ClearLocalInput()
{
	ClickPath.Reset();
	PathIndex = 0;
	if (APawn* ControlledPawn = GetPawn())
	{
		ControlledPawn->ConsumeMovementInputVector();
	}

	FlushPressedKeys();
}

void AHeroPlayerController::ServerSelectProfile_Implementation(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision, EHeroInputProfile Profile)
{
	FString Reason;
	if (!ValidateIntent(HeroEntityId, ConnectionEpoch, LoadoutRevision, Reason))
	{
		LogRejected(TEXT("Profile"), Reason);
		return;
	}

	const UExperienceDefinition* ExperienceDef = GetWorld()->GetSubsystem<UExperienceSubsystem>()->GetExperience();
	if (!ExperienceDef || !ExperienceDef->AllowedInputProfiles.Contains(Profile))
	{
		LogRejected(TEXT("Profile"), TEXT("NotAllowed"));
		return;
	}

	ActiveProfile = Profile;
	++ProfileRevision;
	CastChecked<AHeroCharacter>(GetPawn())->GetCharacterMovement()->StopMovementImmediately();
	ForceNetUpdate();
	if (IsLocalController())
	{
		OnRep_Profile();
	}

	ClientIntentResult(++SentIntentResultSequence, TEXT("Profile"), true, NAME_None);
}

void AHeroPlayerController::ServerClickMove_Implementation(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision, FVector_NetQuantize Destination)
{
	FString Reason;
	if (!ValidateIntent(HeroEntityId, ConnectionEpoch, LoadoutRevision, Reason))
	{
		LogRejected(TEXT("Click"), Reason);
		return;
	}

	AHeroCharacter* Hero = CastChecked<AHeroCharacter>(GetPawn());
	if (ActiveProfile != EHeroInputProfile::ClickToMove || Destination.ContainsNaN() || !Hero->GetDefinition()
		|| FVector::DistSquared(Hero->GetActorLocation(), Destination) > FMath::Square(Hero->GetDefinition()->MaxClickDistance))
	{
		LogRejected(TEXT("Click"), TEXT("ProfileRange"));
		return;
	}

	UNavigationSystemV1* NavigationSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	FNavLocation ProjectedNavLocation;
	if (!NavigationSystem || !NavigationSystem->ProjectPointToNavigation(Destination, ProjectedNavLocation, FVector(100, 100, 250)))
	{
		LogRejected(TEXT("Click"), TEXT("Navigation"));
		return;
	}

	UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(GetWorld(), Hero->GetActorLocation(), ProjectedNavLocation.Location, Hero);
	if (!Path || !Path->IsValid() || Path->IsPartial() || Path->PathPoints.Num() < 2)
	{
		LogRejected(TEXT("Click"), TEXT("Path"));
		return;
	}

	ClientClickPath(ConnectionEpoch, ProfileRevision, Path->PathPoints);
	ClientIntentResult(++SentIntentResultSequence, TEXT("Click"), true, NAME_None);
	UE_LOG(LogMobaControl, Display, TEXT("ClickAccepted Entity=%s Epoch=%u Points=%d Goal=%s"), *HeroEntityId.ToString(), ConnectionEpoch, Path->PathPoints.Num(), *ProjectedNavLocation.Location.ToCompactString());
}

void AHeroPlayerController::ServerActivateSlot_Implementation(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision, FGameplayTag Slot)
{
	FString Reason;
	if (!ValidateIntent(HeroEntityId, ConnectionEpoch, LoadoutRevision, Reason))
	{
		LogRejected(TEXT("Ability"), Reason);
		return;
	}

	UHeroAbilitySystemComponent* HeroASC = CastChecked<AHeroCharacter>(GetPawn())->GetHeroAbilitySystem();
	const FGameplayAbilitySpecHandle Handle = HeroASC->FindSlot(Slot);
	const FGameplayAbilitySpec* Spec = HeroASC->FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->Ability)
	{
		LogRejected(TEXT("Ability"), TEXT("Slot"));
		return;
	}

	if (!HeroASC->IsManagedAbilityEnabled(Handle))
	{
		LogRejected(TEXT("Ability"), TEXT("Disabled"));
		return;
	}

	const UGameplayAbility* Ability = Spec->GetPrimaryInstance() ? Spec->GetPrimaryInstance() : Spec->Ability.Get();
	if (Ability->IsA<UTargetDamageAbility>())
	{
		LogRejected(TEXT("Ability"), TEXT("TargetRequired"));
		return;
	}

	if (!Ability->CheckCooldown(Handle, HeroASC->AbilityActorInfo.Get()))
	{
		LogRejected(TEXT("Ability"), TEXT("Cooldown"));
		return;
	}

	if (!Ability->CheckCost(Handle, HeroASC->AbilityActorInfo.Get()))
	{
		LogRejected(TEXT("Ability"), TEXT("Cost"));
		return;
	}

	if (!HeroASC->TryActivateAbility(Handle, false))
	{
		LogRejected(TEXT("Ability"), TEXT("ActivationBlocked"));
	}
	else
	{
		ClientIntentResult(++SentIntentResultSequence, TEXT("Ability"), true, NAME_None);
	}
}

void AHeroPlayerController::ServerActivateTargetSlot_Implementation(FGuid HeroEntityId, uint32 ConnectionEpoch, uint32 LoadoutRevision, FGameplayTag Slot, const FAbilityTargetIntent& Intent)
{
	FString Reason;
	if (!ValidateIntent(HeroEntityId, ConnectionEpoch, LoadoutRevision, Reason))
	{
		LogRejected(TEXT("UnitAttack"), Reason);
		return;
	}

	AHeroCharacter* SourceHero = CastChecked<AHeroCharacter>(GetPawn());
	UHeroAbilitySystemComponent* HeroASC = SourceHero->GetHeroAbilitySystem();
	const FGameplayAbilitySpecHandle Handle = HeroASC->FindSlot(Slot);
	const FGameplayAbilitySpec* Spec = HeroASC->FindAbilitySpecFromHandle(Handle);
	const auto* Ability = Spec ? Cast<UTargetDamageAbility>(Spec->GetPrimaryInstance() ? Spec->GetPrimaryInstance() : Spec->Ability.Get()) : nullptr;
	if (!Ability)
	{
		LogRejected(TEXT("UnitAttack"), TEXT("Slot"));
		return;
	}

	if (!HeroASC->IsManagedAbilityEnabled(Handle))
	{
		LogRejected(TEXT("UnitAttack"), TEXT("Disabled"));
		return;
	}

	if (!Ability->CheckCooldown(Handle, HeroASC->AbilityActorInfo.Get()))
	{
		LogRejected(TEXT("UnitAttack"), TEXT("Cooldown"));
		return;
	}

	if (!Ability->CheckCost(Handle, HeroASC->AbilityActorInfo.Get()))
	{
		LogRejected(TEXT("UnitAttack"), TEXT("Cost"));
		return;
	}

	auto* TargetEvent = NewObject<UAbilityTargetEvent>(this);
	TargetEvent->Intent = Intent;
	TargetEvent->RequestController = this;
	TargetEvent->ConnectionEpoch = ConnectionEpoch;
	FGameplayEventData Event;
	Event.Instigator = SourceHero;
	Event.Target = Intent.Unit;
	Event.OptionalObject = TargetEvent;
	const bool bActivated = HeroASC->TriggerAbilityFromGameplayEvent(Handle, HeroASC->AbilityActorInfo.Get(), MobaGameplayTags::Event_UnitAttack, &Event, *HeroASC);
	if (!bActivated)
	{
		LogRejected(TEXT("UnitAttack"), TEXT("ActivationBlocked"));
	}
}

void AHeroPlayerController::CompleteTargetAbility(const AHeroCharacter* SourceHero, uint32 ConnectionEpoch, bool bAccepted, const FString& Reason)
{
	if (GetPawn() != SourceHero || !SourceHero || SourceHero->ConnectionEpoch != ConnectionEpoch)
	{
		return;
	}

	if (!bAccepted)
	{
		LogRejected(TEXT("UnitAttack"), Reason);
		return;
	}

	ClientIntentResult(++SentIntentResultSequence, TEXT("UnitAttack"), true, NAME_None);
}

bool AHeroPlayerController::BuildCursorIntent(const FAbilityTargetGeometry& Geometry, FAbilityTargetIntent& Intent) const
{
	if (!GetPawn())
	{
		return false;
	}

	Intent.Policy = Geometry.Policy;
	if (Intent.Policy == EAbilityTargetPolicy::None || Intent.Policy == EAbilityTargetPolicy::Self)
	{
		return true;
	}

	FHitResult Hit;
	if (Intent.Policy == EAbilityTargetPolicy::Unit)
	{
		const TArray<TEnumAsByte<EObjectTypeQuery>> TargetObjectTypes = {UEngineTypes::ConvertToObjectType(ECC_Pawn)};
		if (!GetHitResultUnderCursorForObjects(TargetObjectTypes, false, Hit))
		{
			return false;
		}

		Intent.Unit = Hit.GetActor();
		return true;
	}

	FVector RayOrigin, RayDirection;
	if (!DeprojectMousePositionToWorld(RayOrigin, RayDirection) || FMath::Abs(RayDirection.Z) < UE_SMALL_NUMBER)
	{
		return false;
	}

	const FVector Origin = GetPawn()->GetActorLocation();
	const double RayPlaneParameter = (Origin.Z - RayOrigin.Z) / RayDirection.Z;
	if (RayPlaneParameter < 0)
	{
		return false;
	}

	Intent.Point = RayOrigin + RayDirection * RayPlaneParameter;
	Intent.Direction = (Intent.Point - Origin).GetSafeNormal2D();
	return true;
}

void AHeroPlayerController::ClientClickPath_Implementation(uint32 ConnectionEpoch, uint32 InProfileRevision, const TArray<FVector>& Points)
{
	const AHeroCharacter* Hero = Cast<AHeroCharacter>(GetPawn());
	if (!Hero || !Hero->CanAcceptPlayerInput() || Hero->ConnectionEpoch != ConnectionEpoch || ActiveProfile != EHeroInputProfile::ClickToMove || ProfileRevision != InProfileRevision
		|| bProfileSwitchPending)
	{
		return;
	}

	ClickPath = Points;
	PathIndex = 1;
}

void AHeroPlayerController::ClientIntentResult_Implementation(uint32 Sequence, FName Operation, bool bAccepted, FName Reason)
{
	LastIntentResultSequence = Sequence;
	bLastIntentAccepted = bAccepted;
	LastIntentReason = Reason;
	if (Operation == TEXT("Profile") && !bAccepted)
	{
		bProfileSwitchPending = false;
		ApplyProfile();
	}

	UE_LOG(LogMobaControl, Display, TEXT("IntentResult Sequence=%u Operation=%s Accepted=%d Reason=%s"), Sequence, *Operation.ToString(), bAccepted, *Reason.ToString());
}

void AHeroPlayerController::OnRep_Profile()
{
	bProfileSwitchPending = false;
	if (BoundInputComponent.IsValid() && BoundInputComponent.Get() == InputComponent.Get())
	{
		ApplyProfile();
	}
}

void AHeroPlayerController::ApplyProfile()
{
	if (!IsLocalController() || !GetLocalPlayer())
	{
		return;
	}

	UEnhancedInputLocalPlayerSubsystem* InputSubsystem = GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	const UExperienceDefinition* ExperienceDef = GetWorld()->GetSubsystem<UExperienceSubsystem>()->GetExperience();
	if (!InputSubsystem || !ExperienceDef || GetGameInstance()->GetSubsystem<UGameFlowSubsystem>()->GetSnapshot().State == EGameFlowState::ReturningFrontend)
	{
		return;
	}

	ClearLocalInput();
	if (AppliedProfile)
	{
		InputSubsystem->RemoveMappingContext(AppliedProfile->Mapping.Get());
		if (AppliedProfile->TargetMapping.IsValid())
		{
			InputSubsystem->RemoveMappingContext(AppliedProfile->TargetMapping.Get());
		}

		AppliedProfile = nullptr;
	}

	for (const auto& InputProfileDef : ExperienceDef->InputProfiles)
	{
		if (InputProfileDef.IsValid() && InputProfileDef->Profile == ActiveProfile && InputProfileDef->IsLoaded())
		{
			AppliedProfile = InputProfileDef.Get();
			FModifyContextOptions Options;
			Options.bIgnoreAllPressedKeysUntilRelease = true;
			InputSubsystem->AddMappingContext(InputProfileDef->Mapping.Get(), 0, Options);
			if (InputProfileDef->TargetMapping.IsValid())
			{
				InputSubsystem->AddMappingContext(InputProfileDef->TargetMapping.Get(), 1, Options);
			}

			UE_LOG(LogMobaControl, Display, TEXT("InputProfile Applied=%d Revision=%u Mapping=%s"), int32(ActiveProfile), ProfileRevision, *InputProfileDef->Mapping->GetName());
			break;
		}
	}
}

void AHeroPlayerController::OnMove(const FInputActionValue& Value)
{
	if (!IsLocallyReady() || ActiveProfile != EHeroInputProfile::WASD)
	{
		return;
	}

	AHeroCharacter* Hero = Cast<AHeroCharacter>(GetPawn());
	const FVector2D Axes = Value.Get<FVector2D>().GetClampedToMaxSize(1);
	Hero->AddMovementInput(FVector::ForwardVector, Axes.X);
	Hero->AddMovementInput(FVector::RightVector, Axes.Y);
}

void AHeroPlayerController::OnClick(const FInputActionValue& Value)
{
	if (!IsLocallyReady() || ActiveProfile != EHeroInputProfile::ClickToMove)
	{
		return;
	}

	FHitResult Hit;
	if (GetHitResultUnderCursor(ECC_Visibility, false, Hit))
	{
		MobaClick(Hit.Location.X, Hit.Location.Y, Hit.Location.Z);
	}
}

void AHeroPlayerController::OnSwitch(const FInputActionValue& Value)
{
	MobaProfile();
}

void AHeroPlayerController::OnAbility(const FInputActionValue& Value)
{
	MobaAbility();
}

void AHeroPlayerController::OnUnitAttack(const FInputActionValue& Value)
{
	RequestSlotInput(MobaGameplayTags::Input_Slot_UnitAttack);
}

void AHeroPlayerController::OnTargetAbility(const FInputActionValue& Value)
{
	RequestSlotInput(MobaGameplayTags::Input_Slot_Target);
}

void AHeroPlayerController::RequestSlotInput(FGameplayTag Slot)
{
	if (!IsLocallyReady())
	{
		return;
	}

	AHeroCharacter* Hero = CastChecked<AHeroCharacter>(GetPawn());
	UHeroAbilitySystemComponent* HeroASC = Hero->GetHeroAbilitySystem();
	const auto* Spec = HeroASC->FindAbilitySpecFromHandle(HeroASC->FindSlot(Slot));
	if (!Spec || !Spec->Ability || !HeroASC->IsManagedAbilityEnabled(Spec->Handle))
	{
		return;
	}

	// 槽只标识输入位置；重映射后按当前能力选择既有请求，服务器仍独立校验。
	if (!Spec->Ability->IsA<UTargetDamageAbility>())
	{
		ServerActivateSlot(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision, Slot);
		return;
	}

	const auto* Ability = CastChecked<UTargetDamageAbility>(Spec->Ability);
	FAbilityTargetIntent Intent;
	if (BuildCursorIntent(Ability->GetGeometry(), Intent))
	{
		ServerActivateTargetSlot(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision, Slot, Intent);
	}
}

void AHeroPlayerController::RequestItem(EItemOperation Operation, FName ItemId, FGuid ItemInstanceId, int32 Quantity)
{
	const auto* Hero = Cast<AHeroCharacter>(GetPawn());
	const auto* ParticipantState = GetPlayerState<AParticipantState>();
	if (!Hero || !ParticipantState)
	{
		return;
	}

	FItemRequest Request;
	Request.Entity = Hero->EntityId;
	Request.Epoch = ParticipantState->ConnectionEpoch;
	Request.Sequence = ++NextItemSequence;
	Request.Revision = Hero->GetInventory()->GetSnapshot().Revision;
	Request.Operation = Operation;
	Request.ItemId = ItemId;
	Request.Instance = ItemInstanceId;
	Request.Quantity = Quantity;
	ServerItemRequest(Request);
}

void AHeroPlayerController::LogRejected(const TCHAR* Operation, const FString& Reason)
{
	ClientIntentResult(++SentIntentResultSequence, Operation, false, FName(*Reason));
	const FString Key = FString(Operation) + Reason;
	const double Now = FPlatformTime::Seconds();
	if (const double* Previous = LastRejectedTimes.Find(Key); Previous && Now - *Previous < 1.0)
	{
		return;
	}

	LastRejectedTimes.Add(Key, Now);
	UE_LOG(LogMobaControl, Display, TEXT("IntentRejected Controller=%s Operation=%s Reason=%s"), *GetName(), Operation, *Reason);
}

void AHeroPlayerController::MobaStatus()
{
#if !UE_BUILD_SHIPPING
	for (TActorIterator<AHeroCharacter> It(GetWorld()); It; ++It)
	{
		const AHeroCharacter* Hero = *It;
		const FHeroLifeSnapshot& Life = Hero->GetLifeSnapshot();
		UE_LOG(LogMobaCombat, Display, TEXT("LifeSnapshot Entity=%s State=%d Generation=%u Deaths=%u Killer=%s Persistent=%d Cleanup=%d AttackCooldown=%d"), *Hero->EntityId.ToString(),
			int32(Life.State), Life.Generation, Life.DeathCount, *Life.KillerParticipant.ToString(),
			Hero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Effect_Persistent),
			Hero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Effect_RemoveOnDeath),
			Hero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack));
		UE_LOG(LogMobaControl, Display,
			TEXT("HeroSnapshot Observer=%s Entity=%s Participant=%s LocalRole=%d Epoch=%u Init=%u Revision=%u Ready=%d Position=%s Primary=%.2f Health=%.2f Cooldown=%d ASC=%s"), *GetName(),
			*Hero->EntityId.ToString(), *Hero->ParticipantId.ToString(), int32(Hero->GetLocalRole()), Hero->ConnectionEpoch, Hero->InitializationGeneration, Hero->RequiredLoadoutRevision,
			Hero->bInputReady, *Hero->GetActorLocation().ToCompactString(), Hero->GetAttributes()->GetMana(), Hero->GetAttributes()->GetHealth(),
			Hero->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_Self), *Hero->GetHeroAbilitySystem()->GetPathName());
	}
#endif
}

void AHeroPlayerController::MobaExperience(int64 Amount)
{
#if !UE_BUILD_SHIPPING
	if (auto* Hero = Cast<AHeroCharacter>(GetPawn()); HasAuthority() && Hero)
	{
		const auto Result = Hero->GetAttributeComponent()->AddExperience(Amount);
		UE_LOG(LogMobaCombat, Display, TEXT("ExperienceProbe Accepted=%d Added=%lld Total=%lld Level=%d"), Result.bAccepted, Result.AddedExperiencePoints,
			Hero->GetAttributeComponent()->GetTotalExperiencePoints(), Result.NewLevel);
	}
#endif
}

void AHeroPlayerController::MobaProfile()
{
	if (!IsLocallyReady())
	{
		return;
	}

	AHeroCharacter* Hero = CastChecked<AHeroCharacter>(GetPawn());
	ClearLocalInput();
	bProfileSwitchPending = true;
	ServerSelectProfile(
		Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision, ActiveProfile == EHeroInputProfile::WASD ? EHeroInputProfile::ClickToMove : EHeroInputProfile::WASD);
}

void AHeroPlayerController::MobaAbility()
{
	RequestSlotInput(MobaGameplayTags::Input_Slot_Primary);
}

void AHeroPlayerController::MobaAttack(const FString& TargetEntityIdText)
{
	if (!IsLocallyReady())
	{
		return;
	}

	FGuid TargetEntityId;
	if (!FGuid::Parse(TargetEntityIdText, TargetEntityId))
	{
		return;
	}

	AHeroCharacter* Hero = CastChecked<AHeroCharacter>(GetPawn());
	for (TActorIterator<AHeroCharacter> It(GetWorld()); It; ++It)
	{
		if (It->EntityId == TargetEntityId)
		{
			FAbilityTargetIntent Intent;
			Intent.Unit = *It;
			ServerActivateTargetSlot(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_UnitAttack, Intent);
			return;
		}
	}
}

void AHeroPlayerController::MobaClick(float X, float Y, float Z)
{
	if (!IsLocallyReady())
	{
		return;
	}

	AHeroCharacter* Hero = CastChecked<AHeroCharacter>(GetPawn());
	ServerClickMove(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision, FVector(X, Y, Z));
}

void AHeroPlayerController::MobaRejectProbe()
{
#if !UE_BUILD_SHIPPING
	if (AHeroCharacter* Hero = Cast<AHeroCharacter>(GetPawn()))
	{
		ServerActivateSlot(FGuid::NewGuid(), Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_Primary);
		ServerActivateSlot(Hero->EntityId, Hero->ConnectionEpoch - 1, Hero->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_Primary);
		ServerActivateSlot(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision + 1, MobaGameplayTags::Input_Slot_Primary);
		ServerPlayerReady(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision);
		ServerPlayerReady(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision);
	}
	else
	{
		ServerActivateSlot(FGuid(), 0, 0, MobaGameplayTags::Input_Slot_Primary);
	}
#endif
}

