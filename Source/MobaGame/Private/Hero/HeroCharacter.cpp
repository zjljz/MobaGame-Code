#include "Hero/HeroCharacter.h"

#include "Combat/CombatEffectContext.h"
#include "Combat/AbilityTargeting.h"
#include "Items/ItemInventoryComponent.h"

#include "Hero/HeroMovementComponent.h"
#include "Hero/HeroAttributeComponent.h"
#include "Experience/ExperienceSubsystem.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Hero/HeroDefinition.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Match/ParticipantState.h"
#include "Player/HeroPlayerController.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Net/UnrealNetwork.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/GameStateBase.h"
#include "TimerManager.h"
#include "MobaGameplayTags.h"
#include "MobaMessageTags.h"
#include "MobaLogChannels.h"

AHeroCharacter::AHeroCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UHeroMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	AbilitySystem = CreateDefaultSubobject<UHeroAbilitySystemComponent>(TEXT("AbilitySystem"));
	Inventory = CreateDefaultSubobject<UItemInventoryComponent>(TEXT("Inventory"));
	Attributes = CreateDefaultSubobject<UHeroAttributeSet>(TEXT("Attributes"));
	AttributeComponent = CreateDefaultSubobject<UHeroAttributeComponent>(TEXT("AttributeComponent"));
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(GetRootComponent());
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetRelativeScale3D(FVector(0.6, 0.6, 1.6));
	USpringArmComponent* CameraArmComp = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraArm"));
	CameraArmComp->SetupAttachment(GetRootComponent());
	CameraArmComp->SetUsingAbsoluteRotation(true);
	CameraArmComp->SetRelativeRotation(FRotator(-60, 0, 0));
	CameraArmComp->TargetArmLength = 1300;
	CameraArmComp->bDoCollisionTest = false;
	UCameraComponent* CameraComp = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	CameraComp->SetupAttachment(CameraArmComp);
	bUseControllerRotationYaw = false;
	GetCharacterMovement()->bOrientRotationToMovement = true;
	SetNetUpdateFrequency(30);
}

void AHeroCharacter::BeginPlay()
{
	Super::BeginPlay();
	RefreshActorInfo();
	ApplyLifeState();
}

void AHeroCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(RespawnTimer);
	Super::EndPlay(EndPlayReason);
}

void AHeroCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// 远端Hero可能先于客户端bundle完成复制，不能依赖只有拥有端才到达的Controller回调。
	if (GetNetMode() != NM_DedicatedServer && Definition && Definition->BodyMesh.IsValid() && Body->GetStaticMesh() != Definition->BodyMesh.Get())
	{
		ApplyDefinition();
	}
}

bool AHeroCharacter::IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const
{
	// 已有Hero不能先把Feature资产引用复制给尚未挂载内容的新连接。
	const AHeroPlayerController* ViewerController = Cast<AHeroPlayerController>(RealViewer);
	return ViewerController && ViewerController->HasContentReady() && Super::IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation);
}

void AHeroCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	RefreshActorInfo();
}

void AHeroCharacter::UnPossessed()
{
	bInputReady = false;
	Super::UnPossessed();
	RefreshActorInfo();
}

void AHeroCharacter::OnRep_Controller()
{
	Super::OnRep_Controller();
	RefreshActorInfo();
}

void AHeroCharacter::OnRep_Owner()
{
	Super::OnRep_Owner();
	RefreshActorInfo();
}

void AHeroCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHeroCharacter, EntityId);
	DOREPLIFETIME(AHeroCharacter, ParticipantId);
	DOREPLIFETIME(AHeroCharacter, MatchId);
	DOREPLIFETIME(AHeroCharacter, TeamId);
	DOREPLIFETIME(AHeroCharacter, Life);
	DOREPLIFETIME(AHeroCharacter, InitializationGeneration);
	DOREPLIFETIME(AHeroCharacter, RequiredLoadoutRevision);
	DOREPLIFETIME(AHeroCharacter, ConnectionEpoch);
	DOREPLIFETIME(AHeroCharacter, bInputReady);
	DOREPLIFETIME(AHeroCharacter, Definition);
}

bool AHeroCharacter::InitializeHero(const UHeroDefinition* InHeroDef, const AParticipantState* InPlayerState)
{
	if (!IsInGameThread() || !HasAuthority() || !InHeroDef || !InPlayerState || !InPlayerState->ParticipantId.IsValid())
	{
		return false;
	}

	if (InitializationGeneration != 0)
	{
		return Definition == InHeroDef && ParticipantId == InPlayerState->ParticipantId && MatchId == InPlayerState->MatchId;
	}

	FString Reason;
	if (!InHeroDef->ValidateInitialization(Reason))
	{
		UE_LOG(LogMobaCombat, Warning, TEXT("HeroInitFailed Definition=%s Reason=%s"), *InHeroDef->GetPathName(), *Reason);
		return false;
	}

	RefreshActorInfo();
	const auto* Rules = GetWorld()->GetSubsystem<UExperienceSubsystem>()->GetRules();
	if (!AttributeComponent->BeginInitialization(InHeroDef, Rules, Reason))
	{
		UE_LOG(LogMobaCombat, Warning, TEXT("HeroInitFailed Definition=%s Reason=%s"), *InHeroDef->GetPathName(), *Reason);
		return false;
	}

	const FGuid InitialSourceId = FGuid::NewGuid();
	const FAbilitySourceResult GrantResult = AbilitySystem->GrantInitialSource(InHeroDef->AbilitySet.Get(), InitialSourceId);
	if (!GrantResult.IsSuccess())
	{
		AttributeComponent->AbortInitialization();
		UE_LOG(LogMobaCombat, Warning, TEXT("HeroInitFailed Definition=%s Reason=%s"), *InHeroDef->GetPathName(), *GrantResult.Reason.ToString());
		return false;
	}

	EntityId = InitialSourceId;
	ParticipantId = InPlayerState->ParticipantId;
	MatchId = InPlayerState->MatchId;
	TeamId = InPlayerState->TeamId;
	SpawnTransform = GetActorTransform();
	Definition = InHeroDef;
	ConnectionEpoch = InPlayerState->ConnectionEpoch;
	if (!AttributeComponent->FinishInitialization())
	{
		AttributeComponent->AbortInitialization();
		return false;
	}

	InitializationGeneration = 1;
	RequiredLoadoutRevision = AbilitySystem->GetLoadoutRevision();
	ApplyDefinition();
	return true;
}

void AHeroCharacter::RefreshActorInfo()
{
	AbilitySystem->InitAbilityActorInfo(this, this);
	ApplyDefinition();
}

void AHeroCharacter::CommitLoadoutRevision(uint32 Revision)
{
	if (!HasAuthority())
	{
		return;
	}

	RequiredLoadoutRevision = Revision;
	bInputReady = false;
	ConsumeMovementInputVector();
	GetCharacterMovement()->StopMovementImmediately();
	ForceNetUpdate();
}

bool AHeroCharacter::CanAcceptPlayerInput() const
{
	const AHeroPlayerController* PlayerController = Cast<AHeroPlayerController>(GetController());
	const AParticipantState* OwningPlayerState = PlayerController ? PlayerController->GetPlayerState<AParticipantState>() : nullptr;
	return IsAlive() && bInputReady && InitializationGeneration > 0 && OwningPlayerState && OwningPlayerState->bConnected && OwningPlayerState->PrimaryHero == this
		&& OwningPlayerState->ParticipantId == ParticipantId && OwningPlayerState->ConnectionEpoch == ConnectionEpoch;
}

void AHeroCharacter::ApplyDefinition()
{
	if (Definition)
	{
		if (GetNetMode() != NM_DedicatedServer && Definition->BodyMesh.IsValid())
		{
			Body->SetStaticMesh(Definition->BodyMesh.Get());
		}
	}
}

void AHeroCharacter::OnRep_Definition()
{
	ApplyDefinition();
}

bool AHeroCharacter::EnterDeath(const FGameplayEffectContextHandle& Context)
{
	if (!HasAuthority() || !IsAlive() || !InitializationGeneration || Attributes->GetHealth() > 0 || !Definition)
	{
		return false;
	}

	Life.State = EHeroLifeState::Dying;
	AttributeComponent->StopForDeath();
	++Life.DeathCount;
	const AHeroCharacter* InstigatorHero = Cast<AHeroCharacter>(Context.GetOriginalInstigator());
	const AHeroCharacter* CauserHero = Cast<AHeroCharacter>(Context.GetEffectCauser());
	Life.KillerParticipant = InstigatorHero ? InstigatorHero->ParticipantId : FGuid();
	Life.KillerEntity = InstigatorHero ? InstigatorHero->EntityId : FGuid();
	Life.CauserEntity = CauserHero ? CauserHero->EntityId : FGuid();
	if (const auto* DeliverySnapshot = MobaCombat::GetContext(Context))
	{
		Life.KillerParticipant = DeliverySnapshot->Participant;
		Life.KillerEntity = DeliverySnapshot->Entity;
		Life.CauserEntity = DeliverySnapshot->Entity;
	}

	ApplyLifeState();
	// 状态与唯一归属已提交，取消回调或效果移除引起重入也不能重复结算。
	const FGameplayTagContainer CancelTags(MobaGameplayTags::Ability_CancelOnDeath);
	AbilitySystem->CancelAbilities(&CancelTags);
	AbilitySystem->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(MobaGameplayTags::Effect_RemoveOnDeath));
	Life.State = EHeroLifeState::Dead;
	const AGameStateBase* GameState = GetWorld()->GetGameState();
	Life.RespawnServerTime = (GameState ? GameState->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds()) + Definition->RespawnDelay;
	ApplyLifeState();
	const uint32 Generation = Life.Generation;
	GetWorldTimerManager().SetTimer(RespawnTimer, FTimerDelegate::CreateWeakLambda(this, 
		[this, Generation]()
		{
			Respawn(Generation);
		}), Definition->RespawnDelay, false);
	
	UE_LOG(LogMobaCombat, Display, TEXT("DeathCommitted Entity=%s Generation=%u Count=%u Killer=%s KillerEntity=%s ASC=%s Sources=%d"), *EntityId.ToString(), Life.Generation,
		Life.DeathCount, *Life.KillerParticipant.ToString(), *Life.KillerEntity.ToString(), *AbilitySystem->GetPathName(), AbilitySystem->GetSourceCount());
	ForceNetUpdate();
	NotifyLifeState();
	return true;
}

void AHeroCharacter::Respawn(uint32 ExpectedGeneration)
{
	if (!HasAuthority() || GetWorld()->bIsTearingDown || Life.State != EHeroLifeState::Dead || Life.Generation != ExpectedGeneration || !Definition)
	{
		return;
	}

	Life.State = EHeroLifeState::Respawning;
	ApplyLifeState();
	SetActorTransform(SpawnTransform, false, nullptr, ETeleportType::TeleportPhysics);
	AttributeComponent->RestoreForRespawn();
	++Life.Generation;
	Life.State = EHeroLifeState::Alive;
	Life.RespawnServerTime = 0;
	ApplyLifeState();
	UE_LOG(LogMobaCombat, Display, TEXT("RespawnCommitted Entity=%s Generation=%u Count=%u Health=%.2f ASC=%s Sources=%d ConnectedInput=%d"), *EntityId.ToString(), Life.Generation,
		Life.DeathCount, Attributes->GetHealth(), *AbilitySystem->GetPathName(), AbilitySystem->GetSourceCount(), CanAcceptPlayerInput());
	ForceNetUpdate();
	NotifyLifeState();
}

void AHeroCharacter::ApplyLifeState()
{
	AbilitySystem->SetLooseGameplayTagCount(MobaGameplayTags::State_Life_Alive, Life.State == EHeroLifeState::Alive ? 1 : 0);
	AbilitySystem->SetLooseGameplayTagCount(MobaGameplayTags::State_Life_Dying, Life.State == EHeroLifeState::Dying ? 1 : 0);
	AbilitySystem->SetLooseGameplayTagCount(MobaGameplayTags::State_Life_Dead, Life.State == EHeroLifeState::Dead ? 1 : 0);
	AbilitySystem->SetLooseGameplayTagCount(MobaGameplayTags::State_Life_Respawning, Life.State == EHeroLifeState::Respawning ? 1 : 0);
	if (!IsAlive())
	{
		ConsumeMovementInputVector();
		GetCharacterMovement()->StopMovementImmediately();
		GetCharacterMovement()->DisableMovement();
		if (auto* PlayerController = Cast<AHeroPlayerController>(GetController()))
		{
			PlayerController->ClearLocalInput();
		}
	}
	else if (GetCharacterMovement()->MovementMode == MOVE_None)
	{
		GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	}

	GetCapsuleComponent()->SetCollisionEnabled(IsAlive() ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	// 测试形体仍留场，以压扁外形表示死亡；不引入专属动画或 HUD。
	Body->SetRelativeScale3D(IsAlive() ? FVector(0.6, 0.6, 1.6) : FVector(0.6, 0.6, 0.2));
}

void AHeroCharacter::NotifyLifeState()
{
	const FHeroLifeSnapshot Snapshot = Life;
	GMP::FMessageUtils::SendObjectMessage(this, MOBA_MESSAGE_HERO_LIFE_CHANGED, Snapshot);
}

void AHeroCharacter::OnRep_Life()
{
	ApplyLifeState();
	NotifyLifeState();
}

UAbilitySystemComponent* AHeroCharacter::GetAbilitySystemComponent() const
{
	return AbilitySystem;
}

