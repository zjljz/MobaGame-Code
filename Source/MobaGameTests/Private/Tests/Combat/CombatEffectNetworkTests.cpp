#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Combat/CombatStatusEffects.h"
#include "Combat/CombatEffectParameters.h"
#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Hero/HeroCharacter.h"
#include "Player/HeroPlayerController.h"
#include "Match/ParticipantState.h"
#include "Match/MatchGameState.h"
#include "Experience/ExperienceSubsystem.h"
#include "Experience/ExperienceDefinitions.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/Combat/TargetCombatant.h"
#include "Tests/Combat/CombatEffectTestTypes.h"
#include "MobaGameplayTags.h"
#include <limits>

namespace
{
	class FCombatEffectNetwork final : public IAutomationLatentCommand
	{

	public:
		explicit FCombatEffectNetwork(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds())
		{
			FParse::Value(FCommandLine::Get(), TEXT("MobaEffectRole="), Role);
			FParse::Value(FCommandLine::Get(), TEXT("MobaEffectEvidence="), Evidence);
			FPaths::NormalizeDirectoryName(Evidence);
		}


	private:
		FString Read(const TCHAR* Name) const
		{
			FString S;
			FFileHelper::LoadFileToString(S, *FPaths::Combine(Evidence, Name));
			return S;
		}


	public:
		bool Update() override
		{
			if (Evidence.IsEmpty() || FPaths::IsRelative(Evidence) || !(Evidence.Contains(TEXT("/AI/task-010/")) || Evidence.Contains(TEXT("/AI/task-011/"))))
			{
				return Fail(TEXT("路径边界"));
			}

			if (Test->HasAnyErrors())
			{
				return Fail(TEXT("本阶段断言失败"));
			}

			for (const TCHAR* Peer : {TEXT("server"), TEXT("initial"), TEXT("observer"), TEXT("restart")})
			{
				if (Has(*(FString(Peer) + TEXT("-failed"))))
				{
					return Fail(TEXT("对端失败"));
				}
			}

			if (FPlatformTime::Seconds() - Started > 180)
			{
				return Fail(TEXT("状态谓词超时"));
			}

			if (Role == TEXT("observer") && Has(TEXT("server-passed")))
			{
				Mark(TEXT("observer-passed"));
				return true;
			}

			UWorld* World = nullptr;
			for (const auto& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() && Context.World()->IsGameWorld())
				{
					World = Context.World();
					break;
				}
			}

			if (!World)
			{
				return false;
			}

			if (Role == TEXT("server"))
			{
				return World->GetNetMode() == NM_DedicatedServer && Server(World);
			}

			if (World->GetNetMode() != NM_Client)
			{
				return false;
			}

			auto* PC = Cast<AHeroPlayerController>(World->GetFirstPlayerController());
			auto* Hero = PC ? Cast<AHeroCharacter>(PC->GetPawn()) : nullptr;
			if (!Hero || PC->GetAdmissionResult() != TEXT("Bound"))
			{
				return false;
			}

			if (Role == TEXT("restart"))
			{
				return PC->IsLocallyReady() && Restart(Hero);
			}

			if (!Has(*(Role + TEXT("-identity"))))
			{
				if (!PC->IsLocallyReady())
				{
					return false;
				}

				Mark(*(Role + TEXT("-identity")), Hero->EntityId.ToString());
			}

			auto* Owner = Find(World, Read(TEXT("initial-identity")));
			auto* Observer = Find(World, Read(TEXT("observer-identity")));
			if (!Owner || !Observer)
			{
				return false;
			}

			ATargetCombatant* Ordinary = nullptr;
			for (TActorIterator<ATargetCombatant> It(World); It; ++It)
			{
				Ordinary = *It;
				break;
			}

			if (!Ordinary)
			{
				return false;
			}

			if (Has(TEXT("crit-shot")) && Ordinary->GetHealth() == 150)
			{
				Mark(*(Role + TEXT("-crit")));
			}

			if (Role == TEXT("observer"))
			{
				return Observe(Owner, Ordinary);
			}

			return Initial(PC, Owner, Observer, Ordinary);
		}


	private:
		void Mark(const TCHAR* Name, const FString& Value = TEXT("ready"))
		{
			if (Test->HasAnyErrors() && !FString(Name).EndsWith(TEXT("failed")))
			{
				return;
			}

			Test->TestTrue(TEXT("保存公开场景标记"), FFileHelper::SaveStringToFile(Value, *FPaths::Combine(Evidence, Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		}

		bool Fail(const TCHAR* Reason)
		{
			const FString Message = FString::Printf(TEXT("CombatEffects role=%s stage=%d %s"), *Role, Stage, Reason);
			Test->AddError(Message);
			Mark(*(Role + TEXT("-failed")), Message);
			return true;
		}

		AHeroCharacter* Find(UWorld* World, const FString& Identity)
		{
			FGuid Id;
			if (!FGuid::Parse(Identity, Id))
			{
				return nullptr;
			}

			for (TActorIterator<AHeroCharacter> It(World); It; ++It)
			{
				if (It->EntityId == Id)
				{
					return *It;
				}
			}

			return nullptr;
		}

		UClass* Effect(const TCHAR* Name)
		{
			const FString Path = FString(TEXT("/MobaClassic/Tests/Batch0811/Effects/GE_")) + Name;
			return LoadClass<UGameplayEffect>(nullptr, *(Path + TEXT(".GE_") + Name + TEXT("_C")));
		}

		FActiveGameplayEffectHandle Apply(UAbilitySystemComponent* Source, UAbilitySystemComponent* Target, const TCHAR* Name, float Duration = 0)
		{
			auto Spec = Source->MakeOutgoingSpec(Effect(Name), 1, Source->MakeEffectContext());
			if (!Spec.IsValid())
			{
				Test->AddError(TEXT("磁盘GE Spec无效"));
				return {};
			}

			if (Duration > 0)
			{
				Spec.Data->SetDuration(Duration, true);
			}

			return Target->ApplyGameplayEffectSpecToSelf(*Spec.Data);
		}

		void Self(AHeroPlayerController* PC, AHeroCharacter* Hero, FGameplayTag Slot) { PC->ServerActivateSlot(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision, Slot); }
		void Attack(AHeroPlayerController* PC, AHeroCharacter* Hero, AHeroCharacter* Target)
		{
			PC->ServerActivateUnitSlot(Hero->EntityId, Hero->ConnectionEpoch, Hero->RequiredLoadoutRevision, MobaGameplayTags::Input_Slot_UnitAttack, Target);
		}

		bool Server(UWorld* World)
		{
			auto* Owner = Find(World, Read(TEXT("initial-identity")));
			auto* Observer = Find(World, Read(TEXT("observer-identity")));
			if (!Owner || !Observer)
			{
				return false;
			}

			auto* ASC = Owner->GetHeroAbilitySystem();
			auto* Source = Observer->GetHeroAbilitySystem();
			switch (Stage)
			{
			case 0:
			{
				if (!Owner->bInputReady || !Observer->bInputReady)
				{
					return false;
				}

				const auto* Experience = World->GetSubsystem<UExperienceSubsystem>()->GetExperience();
				Test->TestTrue(TEXT("实际新公式Experience"), Experience && Experience->CombatRules.DamagePolicy == ECombatDamagePolicy::Standard);
				Test->TestEqual(TEXT("完整效果资源已装载"), Experience->CombatEffects.Num(), 14);
				for (const auto& Class : Experience->CombatEffects)
				{
					Test->TestTrue(TEXT("效果在bundle已加载"), Class.IsValid());
				}

				OrdinaryActor = World->SpawnActor<ATargetCombatant>(ATargetCombatant::StaticClass(), FVector(1500, 0, 100), FRotator::ZeroRotator);
				OrdinaryActor->Entity = FGuid::NewGuid();
				OrdinaryActor->Participant = FGuid::NewGuid();
				OrdinaryActor->Match = Owner->MatchId;
				OrdinaryActor->Team = Owner->TeamId;
				OrdinaryActor->SetActorLocation(Observer->GetActorLocation() + FVector(400, 0, 0));
				auto* OrdinaryASC = OrdinaryActor->GetCombatAbilitySystem();
				OrdinaryASC->SetNumericAttributeBase(UHeroAttributeSet::GetMaxHealthAttribute(), 200);
				OrdinaryActor->SetHealth(200);
				CritAbility = Source->GiveAbility(FGameplayAbilitySpec(UGuaranteedCriticalProjectileTestAbility::StaticClass(), 1));
				auto* Payload = NewObject<UAbilityTargetEvent>(Observer);
				Payload->Intent.Policy = EAbilityTargetPolicy::Direction;
				Payload->Intent.Direction = FVector::ForwardVector;
				FGameplayEventData Event;
				Event.Instigator = Observer;
				Event.OptionalObject = Payload;
				Test->TestTrue(TEXT("真实GA释放确定暴击Projectile"),
					Source->TriggerAbilityFromGameplayEvent(CritAbility, Source->AbilityActorInfo.Get(), MobaGameplayTags::Event_UnitAttack, &Event, *Source));
				OrdinaryASC->SetNumericAttributeBase(UHeroAttributeSet::GetArmorAttribute(), 200);
				Mark(TEXT("crit-shot"));
				Stage = 20;
				return false;
			}
			case 20:
			{
				if (OrdinaryActor->GetHealth() != 150 || !Has(TEXT("initial-crit")) || !Has(TEXT("observer-crit")))
				{
					return false;
				}

				Test->TestEqual(TEXT("真实释放100x1.5，命中读取后改Armor200结算50"), OrdinaryActor->GetHealth(), 150.f);
				Source->ClearAbility(CritAbility);
				OrdinaryActor->SetActorLocation(FVector(1500, 0, 100));
				for (auto* Target : {Owner->GetCombatAbilitySystem(), OrdinaryActor->GetCombatAbilitySystem()})
				{
					Target->SetNumericAttributeBase(UHeroAttributeSet::GetMaxHealthAttribute(), 200);
					Target->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 200);
					Target->SetNumericAttributeBase(UHeroAttributeSet::GetArmorAttribute(), 100);
					Target->SetNumericAttributeBase(UHeroAttributeSet::GetMagicResistanceAttribute(), 100);
					auto Mixed = Source->MakeOutgoingSpec(Effect(TEXT("PhysicalDamage")), 1, Source->MakeEffectContext());
					Mixed.Data->AddDynamicAssetTag(MobaGameplayTags::Damage_True);
					Target->ApplyGameplayEffectSpecToSelf(*Mixed.Data);
					auto Unknown = Source->MakeOutgoingSpec(UCombatDamageEffect::StaticClass(), 1, Source->MakeEffectContext());
					Unknown.Data->AddDynamicAssetTag(MobaGameplayTags::Damage_Unknown);
					Target->ApplyGameplayEffectSpecToSelf(*Unknown.Data);
					auto* Invalid = NewObject<UCombatDamageEffect>(Owner);
					Invalid->FindOrAddComponent<UCombatDamageParametersComponent>().BaseDamage = FScalableFloat(std::numeric_limits<float>::quiet_NaN());
					FGameplayEffectSpec InvalidSpec(Invalid, Source->MakeEffectContext(), 1);
					InvalidSpec.AddDynamicAssetTag(MobaGameplayTags::Damage_True);
					Target->ApplyGameplayEffectSpecToSelf(InvalidSpec);
					Test->TestEqual(TEXT("真实GE混合/未知/非有限基础值均拒绝"), Target->GetNumericAttribute(UHeroAttributeSet::GetHealthAttribute()), 200.f);
					Apply(Source, Target, TEXT("PhysicalDamage"));
					Test->TestEqual(TEXT("网络服务器真实Physical30"), Target->GetNumericAttribute(UHeroAttributeSet::GetHealthAttribute()), 170.f);
					Apply(Source, Target, TEXT("Heal"));
					Apply(Source, Target, TEXT("Shield"));
					Apply(Source, Target, TEXT("MagicalDamage"));
					Test->TestEqual(TEXT("30盾吸收抗性后的30"), Target->GetNumericAttribute(UHeroAttributeSet::GetShieldAttribute()), 0.f);
					Apply(Source, Target, TEXT("TrueDamage"));
					Test->TestEqual(TEXT("True越过抗性"), Target->GetNumericAttribute(UHeroAttributeSet::GetHealthAttribute()), 135.f);
					Apply(Source, Target, TEXT("DamageOverTime"));
					Apply(Source, Target, TEXT("Root"), 30);
				}

				Time = World->GetTimeSeconds();
				SavedOwner = Owner;
				SavedASC = ASC;
				Specs = ASC->GetActivatableAbilities().Num();
				Test->TestEqual(TEXT("新Self能力真实授予"), Specs, 5);
				Mark(TEXT("effects-start"));
				Stage = 1;
				return false;
			}
			case 1:
				if (World->GetTimeSeconds() - Time < 3.3 || !Has(TEXT("initial-effects")) || !Has(TEXT("observer-effects")))
				{
					return false;
				}

				Test->TestEqual(TEXT("Hero三tick"), Owner->GetAttributes()->GetHealth(), 120.f);
				Test->TestEqual(TEXT("非Hero三tick"), OrdinaryActor->GetHealth(), 120.f);
				Position = Owner->GetActorLocation();
				Mark(TEXT("root-go"));
				Stage = 2;
				return false;
			case 2:
				if (!Has(TEXT("root-done")))
				{
					return false;
				}

				Test->TestEqual(TEXT("Root允许SelfHeal真实提交"), Owner->GetAttributes()->GetHealth(), 145.f);
				Test->TestTrue(TEXT("服务器Root抑制移动"), FVector::DistXY(Position, Owner->GetActorLocation()) < 2);
				Apply(Source, ASC, TEXT("Silence"), 30);
				Mark(TEXT("silence-go"));
				Stage = 3;
				return false;
			case 3:
				if (!Has(TEXT("silence-done")))
				{
					return false;
				}

				Test->TestEqual(TEXT("Silence仍允许basic成本20"), Owner->GetAttributes()->GetMana(), 80.f);
				Test->TestEqual(TEXT("Silence basic真实命中"), Observer->GetAttributes()->GetHealth(), 40.f);
				ASC->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(MobaGameplayTags::Cooldown_UnitAttack));
				Apply(Source, ASC, TEXT("Stun"), 30);
				Mark(TEXT("stun-go"));
				Stage = 4;
				return false;
			case 4:
				if (!Has(TEXT("stun-done")))
				{
					return false;
				}

				Test->TestEqual(TEXT("Stun拒绝不多扣Primary"), Owner->GetAttributes()->GetMana(), 80.f);
				Test->TestEqual(TEXT("Stun拒绝不多伤害"), Observer->GetAttributes()->GetHealth(), 40.f);
				Test->TestTrue(TEXT("网络持续移动仍被禁"), FVector::DistXY(Position, Owner->GetActorLocation()) < 2);
				Test->TestEqual(TEXT("Basic仅Root/Silence"), MobaCombat::Dispel(ASC, false, false, true), 2);
				Test->TestTrue(TEXT("Stun并集保留"), ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Stun));
				Mark(TEXT("basic-go"));
				Stage = 5;
				return false;
			case 5:
				if (!Has(TEXT("basic-done")))
				{
					return false;
				}

				Test->TestEqual(TEXT("Strong再移Stun"), MobaCombat::Dispel(ASC, true, false, true), 1);
				MobaCombat::Dispel(OrdinaryActor->GetCombatAbilitySystem(), false, false, true);
				Mark(TEXT("move-go"));
				Stage = 6;
				return false;
			case 6:
				if (!Has(TEXT("move-done")) || FVector::DistXY(Position, Owner->GetActorLocation()) < 30)
				{
					return false;
				}

				Test->TestFalse(TEXT("普通ASC驱散Root"), OrdinaryActor->GetCombatAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::State_Root));
				{
					FGameplayEffectSpec Spec(GetDefault<UDeathPersistentEffect>(), ASC->MakeEffectContext(), 1);
					Spec.SetDuration(120, true);
					Persistent = ASC->ApplyGameplayEffectSpecToSelf(Spec);
				}

				// 持久fixture指定真实时长，不更改资产/CDO。
				{
					FGameplayEffectSpec Spec(GetDefault<UUnitAttackCooldownEffect>(), ASC->MakeEffectContext(), 1);
					Spec.SetDuration(60, true);
					Cooldown = ASC->ApplyGameplayEffectSpecToSelf(Spec);
				}

				Apply(Source, ASC, TEXT("Shield"));
				for (int32 I = 0; I < 4; ++I)
				{
					Apply(Source, ASC, TEXT("TrueDamage"));
				}

				Test->TestFalse(TEXT("真实GE致死"), Owner->IsAlive());
				Test->TestEqual(TEXT("重复GE唯一死亡"), Owner->GetLifeSnapshot().DeathCount, uint32(1));
				Test->TestEqual(TEXT("死亡盾清理"), Owner->GetAttributes()->GetShield(), 0.f);
				Mark(TEXT("dead-go"));
				Stage = 7;
				return false;
			case 7:
				if (!Has(TEXT("initial-dead")) || !Has(TEXT("observer-dead")) || !Owner->IsAlive())
				{
					return false;
				}

				Test->TestEqual(TEXT("原Hero"), Owner, SavedOwner.Get());
				Test->TestEqual(TEXT("原ASC"), ASC, SavedASC.Get());
				Test->TestNotNull(TEXT("死亡保留原冷却handle"), ASC->GetActiveGameplayEffect(Cooldown));
				Test->TestNotNull(TEXT("死亡保留原持久GEhandle"), ASC->GetActiveGameplayEffect(Persistent));
				Test->TestEqual(TEXT("无重授"), ASC->GetActivatableAbilities().Num(), Specs);
				ASC->SetNumericAttributeBase(UHeroAttributeSet::GetHealthAttribute(), 90);
				Apply(Source, ASC, TEXT("Slow"), 60);
				OfflineDot = Apply(ASC, OrdinaryActor->GetCombatAbilitySystem(), TEXT("DamageOverTime"));
				Time = World->GetTimeSeconds();
				Mark(TEXT("disconnect-go"));
				Stage = 8;
				return false;
			case 8:
				if (!Has(TEXT("initial-passed")) || Owner->CanAcceptPlayerInput() || World->GetTimeSeconds() - Time < 3.3)
				{
					return false;
				}

				Test->TestEqual(TEXT("来源退出后DoT继续3次"), OrdinaryActor->GetHealth(), 105.f);
				Test->TestNull(TEXT("离线周期自然到期"), OrdinaryActor->GetCombatAbilitySystem()->GetActiveGameplayEffect(OfflineDot));
				Mark(TEXT("offline-ready"));
				Stage = 9;
				return false;
			case 9:
				if (!Has(TEXT("restart-passed")) || !Owner->CanAcceptPlayerInput())
				{
					return false;
				}

				Test->TestEqual(TEXT("重启绑定原Hero"), Owner, SavedOwner.Get());
				Test->TestEqual(TEXT("重启绑定原ASC"), ASC, SavedASC.Get());
				Test->TestEqual(TEXT("重启Spec不重授"), ASC->GetActivatableAbilities().Num(), Specs);
				Test->TestEqual(TEXT("重启来源不重授"), ASC->GetSourceCount(), 1);
				Test->TestNotNull(TEXT("重启原冷却handle"), ASC->GetActiveGameplayEffect(Cooldown));
				Test->TestNotNull(TEXT("重启原持久GEhandle"), ASC->GetActiveGameplayEffect(Persistent));
				if (!Has(TEXT("observer-offline")))
				{
					return false;
				}

				Mark(TEXT("server-passed"));
				return true;
			}

			return false;
		}

		bool Observe(AHeroCharacter* Owner, ATargetCombatant* Ordinary)
		{
			if (Has(TEXT("effects-start")) && Owner->GetAttributes()->GetHealth() == 120 && Ordinary->GetHealth() == 120
				&& Ordinary->GetCombatAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::State_Root))
			{
				Test->TestTrue(TEXT("observer普通Actor无需Hero"), !Ordinary->IsA<AHeroCharacter>());
				Test->TestTrue(TEXT("observer Hero复制Root"), Owner->GetHeroAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::State_Root));
				Mark(TEXT("observer-effects"));
			}

			if (Has(TEXT("dead-go")) && !Owner->IsAlive() && Owner->GetAttributes()->GetHealth() == 0)
			{
				Test->TestEqual(TEXT("observer死亡一次"), Owner->GetLifeSnapshot().DeathCount, uint32(1));
				Mark(TEXT("observer-dead"));
			}

			if (Has(TEXT("offline-ready")) && Ordinary->GetHealth() == 105)
			{
				Test->TestFalse(TEXT("observer实际收到普通Actor驱散后无Root"), Ordinary->GetCombatAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::State_Root));
				Mark(TEXT("observer-offline"));
			}

			return false;
		}

		bool Initial(AHeroPlayerController* PC, AHeroCharacter* Owner, AHeroCharacter* Observer, ATargetCombatant* Ordinary)
		{
			auto* ASC = Owner->GetHeroAbilitySystem();
			switch (Stage)
			{
			case 0:
				if (!Has(TEXT("effects-start")) || Owner->GetAttributes()->GetHealth() != 120 || Ordinary->GetHealth() != 120 || !ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Root))
				{
					return false;
				}

				Test->TestTrue(TEXT("owner普通Actor收到Root"), Ordinary->GetCombatAbilitySystem()->HasMatchingGameplayTag(MobaGameplayTags::State_Root));
				Mark(TEXT("initial-effects"));
				Stage = 1;
				return false;
			case 1:
				if (!Has(TEXT("root-go")))
				{
					return false;
				}

				Position = Owner->GetActorLocation();
				Time = FPlatformTime::Seconds();
				Self(PC, Owner, MobaGameplayTags::Input_Slot_Heal);
				Stage = 2;
				return false;
			case 2:
				Owner->AddMovementInput(FVector::ForwardVector);
				if (FPlatformTime::Seconds() - Time < .7 || Owner->GetAttributes()->GetHealth() != 145)
				{
					return false;
				}

				Test->TestTrue(TEXT("owner Root移动抑制"), FVector::DistXY(Position, Owner->GetActorLocation()) < 2);
				Mark(TEXT("root-done"));
				Stage = 3;
				return false;
			case 3:
				if (!Has(TEXT("silence-go")) || !ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Silence))
				{
					return false;
				}

				Ack = PC->GetIntentResultSequence();
				Self(PC, Owner, MobaGameplayTags::Input_Slot_Primary);
				Stage = 4;
				return false;
			case 4:
				if (PC->GetIntentResultSequence() == Ack)
				{
					return false;
				}

				Test->TestFalse(TEXT("Silence法术真实RPC拒绝"), PC->WasLastIntentAccepted());
				Test->TestEqual(TEXT("Silence无成本"), Owner->GetAttributes()->GetMana(), 100.f);
				Test->TestEqual(TEXT("Silence拒绝原因为激活门禁"), PC->GetLastIntentReason(), FName(TEXT("ActivationBlocked")));
				Ack = PC->GetIntentResultSequence();
				Attack(PC, Owner, Observer);
				Stage = 5;
				return false;
			case 5:
				if (PC->GetIntentResultSequence() == Ack || Owner->GetAttributes()->GetMana() != 80 || Observer->GetAttributes()->GetHealth() != 40)
				{
					return false;
				}

				Test->TestTrue(TEXT("Silence basic实际允许"), PC->WasLastIntentAccepted());
				Mark(TEXT("silence-done"));
				Stage = 6;
				return false;
			case 6:
				if (!Has(TEXT("stun-go")) || !ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Stun))
				{
					return false;
				}

				Ack = PC->GetIntentResultSequence();
				Attack(PC, Owner, Observer);
				Time = FPlatformTime::Seconds();
				Stage = 7;
				return false;
			case 7:
				Owner->AddMovementInput(FVector::ForwardVector);
				if (PC->GetIntentResultSequence() == Ack || FPlatformTime::Seconds() - Time < .7)
				{
					return false;
				}

				Test->TestFalse(TEXT("Stun真实RPC拒绝"), PC->WasLastIntentAccepted());
				Test->TestEqual(TEXT("Stun拒绝无成本"), Owner->GetAttributes()->GetMana(), 80.f);
				Test->TestEqual(TEXT("Stun拒绝原因为激活门禁"), PC->GetLastIntentReason(), FName(TEXT("ActivationBlocked")));
				Test->TestTrue(TEXT("Stun不移动"), FVector::DistXY(Position, Owner->GetActorLocation()) < 2);
				Mark(TEXT("stun-done"));
				Stage = 8;
				return false;
			case 8:
				if (!Has(TEXT("basic-go")) || ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Root) || ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Silence))
				{
					return false;
				}

				Test->TestTrue(TEXT("Basic驱散后Stun复制保留"), ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Stun));
				Mark(TEXT("basic-done"));
				Stage = 9;
				return false;
			case 9:
				if (!Has(TEXT("move-go")) || ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Stun))
				{
					return false;
				}

				Owner->AddMovementInput(FVector::ForwardVector);
				if (FVector::DistXY(Position, Owner->GetActorLocation()) < 60)
				{
					return false;
				}

				Mark(TEXT("move-done"));
				Stage = 10;
				return false;
			case 10:
				if (!Has(TEXT("dead-go")) || Owner->IsAlive() || Owner->GetAttributes()->GetHealth() != 0)
				{
					return false;
				}

				Test->TestEqual(TEXT("owner死亡唯一"), Owner->GetLifeSnapshot().DeathCount, uint32(1));
				Test->TestTrue(TEXT("owner死亡冷却复制保留"), ASC->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack));
				Mark(TEXT("initial-dead"));
				Stage = 11;
				return false;
			case 11:
				if (!Has(TEXT("disconnect-go")) || !Owner->IsAlive() || Owner->GetAttributes()->GetHealth() != 90)
				{
					return false;
				}

				Test->TestEqual(TEXT("复活原死亡代次"), Owner->GetLifeSnapshot().DeathCount, uint32(1));
				Mark(TEXT("initial-passed"));
				return true;
			}

			return false;
		}

		bool Restart(AHeroCharacter* Owner)
		{
			auto* ASC = Owner->GetHeroAbilitySystem();
			if (!Has(TEXT("offline-ready")) || Owner->GetAttributes()->GetHealth() != 90 || !ASC->HasMatchingGameplayTag(MobaGameplayTags::State_Slow)
				|| !ASC->HasMatchingGameplayTag(MobaGameplayTags::Cooldown_UnitAttack))
			{
				return false;
			}

			Test->TestEqual(TEXT("重启同实体"), Owner->EntityId.ToString(), Read(TEXT("initial-identity")));
			Test->TestEqual(TEXT("重启读当前HP"), Owner->GetAttributes()->GetHealth(), 90.f);
			Test->TestEqual(TEXT("重启读当前Slow属性"), Owner->GetAttributes()->GetMoveSpeed(), .6f);
			Test->TestEqual(TEXT("重启无新死亡"), Owner->GetLifeSnapshot().DeathCount, uint32(1));
			Test->TestEqual(TEXT("重启当前5Specs"), ASC->GetActivatableAbilities().Num(), 5);
			FGameplayEffectQuery Query = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(MobaGameplayTags::Cooldown_UnitAttack));
			const auto Remaining = ASC->GetActiveEffectsTimeRemaining(Query);
			Test->TestTrue(TEXT("重启剩余冷却在0与60之间"), Remaining.Num() == 1 && Remaining[0] > 0 && Remaining[0] < 60);
			Mark(TEXT("restart-passed"));
			return Has(TEXT("server-passed"));
		}

		FAutomationTestBase* Test;

		bool Has(const TCHAR* Name) const { return IFileManager::Get().FileExists(*FPaths::Combine(Evidence, Name)); }


	private:
		FString Role, Evidence;

		double Started, Time = 0;

		int32 Stage = 0, Specs = 0;

		uint32 Ack = 0;

		FVector Position;

		TWeakObjectPtr<AHeroCharacter> SavedOwner;

		TWeakObjectPtr<UHeroAbilitySystemComponent> SavedASC;

		ATargetCombatant* OrdinaryActor = nullptr;

		FActiveGameplayEffectHandle Persistent, Cooldown, OfflineDot;

		FGameplayAbilitySpecHandle CritAbility;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCombatEffectNetworkTest, "Moba.Network.CombatEffects", EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FCombatEffectNetworkTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(FCombatEffectNetwork(this));
	return true;
}
#endif
