#include "Tests/AbilitySystem/ManagedAbilityTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Combat/HeroAttributeSet.h"
#include "Tests/Combat/CombatCancellationTestAbility.h"
#include "AbilitySystem/AbilitySetDefinition.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Hero/HeroCharacter.h"
#include "Misc/AutomationTest.h"
#include "MobaGameplayTags.h"
#include "TimerManager.h"

namespace
{
	class FManagedAbilityWorld
	{

	public:
		FManagedAbilityWorld()
		{
			const auto Values =
				UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(
					false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			// 无GameMode的隔离World仍需真实Actor BeginPlay/EndPlay生命周期。
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FManagedAbilityWorld() { Destroy(); }


	public:
		void Destroy()
		{
			if (World)
			{
				World->EndPlay(EEndPlayReason::Quit);
				World->DestroyWorld(false);
				GEngine->DestroyWorldContext(World);
				World = nullptr;
			}
		}

		UHeroAbilitySystemComponent* Spawn()
		{
			FActorSpawnParameters Parameters;
			Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AHeroCharacter* Hero = World->SpawnActor<AHeroCharacter>(AHeroCharacter::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Parameters);
			return Hero->GetHeroAbilitySystem();
		}

		UAbilitySetDefinition* MakeSet(FGameplayTag Slot = {}, TSubclassOf<UGameplayAbility> Ability = UManagedAbilityTestAbility::StaticClass())
		{
			UAbilitySetDefinition* Set = NewObject<UAbilitySetDefinition>(World);
			Set->bAllowCopy = true;
			auto& Grant = Set->Abilities.AddDefaulted_GetRef();
			Grant.Ability = Ability;
			Grant.Slot = Slot;
			Grant.MaxLevel = 3;
			Set->Effects.Add(UManagedAbilityPassiveEffect::StaticClass());
			return Set;
		}

		UWorld* World = nullptr;
	};

	bool ExpectStatus(FAutomationTestBase& Test, const TCHAR* Context, const FAbilitySourceResult& Result, EAbilitySourceResult Status)
	{
		const bool bMatches = Test.TestEqual(Context, static_cast<uint8>(Result.Status), static_cast<uint8>(Status));
		if (Status == EAbilitySourceResult::Failed || Status == EAbilitySourceResult::Busy)
		{
			Test.TestFalse(FString(Context) + TEXT("具有可判别原因"), Result.Reason.IsNone());
		}

		return bMatches;
	}

	FGameplayAbilitySpecHandle FirstHandle(UHeroAbilitySystemComponent* Abilities, FGuid Source)
	{
		const auto* Record = Abilities->FindSource(Source);
		return Record && !Record->Abilities.IsEmpty() ? Record->Abilities[0] : FGameplayAbilitySpecHandle();
	}

	FActiveGameplayEffectHandle FirstEffect(UHeroAbilitySystemComponent* Abilities, FGuid Source)
	{
		const auto* Record = Abilities->FindSource(Source);
		return Record && !Record->Effects.IsEmpty() ? Record->Effects[0] : FActiveGameplayEffectHandle();
	}

	FActiveGameplayEffectHandle AddCooldown(UHeroAbilitySystemComponent* Abilities)
	{
		const auto Spec = Abilities->MakeOutgoingSpec(UManagedAbilityCooldownEffect::StaticClass(), 1, Abilities->MakeEffectContext());
		return Spec.IsValid() ? Abilities->ApplyGameplayEffectSpecToSelf(*Spec.Data) : FActiveGameplayEffectHandle();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManagedAbilityOperationsTest, "Moba.Hero.ManagedAbility.Operations", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FManagedAbilityOperationsTest::RunTest(const FString& Parameters)
{
	FManagedAbilityWorld Fixture;
	auto* Abilities = Fixture.Spawn();
	auto* Set = Fixture.MakeSet(MobaGameplayTags::Input_Slot_Primary);
	const FGuid Source = FGuid::NewGuid();
	ExpectStatus(*this, TEXT("Grant"), Abilities->GrantManagedSource(Set, Source, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	const auto Handle = FirstHandle(Abilities, Source);
	const auto Effect = FirstEffect(Abilities, Source);
	if (!TestNotNull(TEXT("Grant创建真实Spec"), Abilities->FindAbilitySpecFromHandle(Handle)))
	{
		return false;
	}

	TestNotNull(TEXT("Grant创建真实GE"), Abilities->GetActiveGameplayEffect(Effect));
	TestTrue(TEXT("槽关联真实Spec"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary) == Handle);
	const uint32 GrantedState = Abilities->GetStateRevision();
	ExpectStatus(*this, TEXT("相同Grant幂等"), Abilities->GrantManagedSource(Set, Source, GrantedState), EAbilitySourceResult::NoChange);
	TestEqual(TEXT("幂等不推进状态"), Abilities->GetStateRevision(), GrantedState);
	ExpectStatus(*this, TEXT("Grant旧版本拒绝"), Abilities->GrantManagedSource(Set, FGuid::NewGuid(), GrantedState - 1), EAbilitySourceResult::Failed);
	ExpectStatus(*this, TEXT("相同来源不同定义拒绝"), Abilities->GrantManagedSource(Fixture.MakeSet(), Source, GrantedState), EAbilitySourceResult::Failed);
	ExpectStatus(*this, TEXT("槽冲突拒绝"), Abilities->GrantManagedSource(Set, FGuid::NewGuid(), GrantedState), EAbilitySourceResult::Failed);
	TestEqual(TEXT("拒绝无重复Spec"), Abilities->GetActivatableAbilities().Num(), 1);

	const uint32 MappingBeforeUpgrade = Abilities->GetLoadoutRevision();
	ExpectStatus(*this, TEXT("Upgrade"), Abilities->UpgradeManagedAbility(Handle, 2, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	TestEqual(TEXT("原Spec升级"), Abilities->FindAbilitySpecFromHandle(Handle)->Level, 2);
	TestEqual(TEXT("定义保持只读"), Set->Abilities[0].Level, 1);
	TestEqual(TEXT("Upgrade不重授"), FirstHandle(Abilities, Source), Handle);
	TestEqual(TEXT("Upgrade不改变槽版本"), Abilities->GetLoadoutRevision(), MappingBeforeUpgrade);
	ExpectStatus(*this, TEXT("越级上限拒绝"), Abilities->UpgradeManagedAbility(Handle, 4, Abilities->GetStateRevision()), EAbilitySourceResult::Failed);
	TestEqual(TEXT("失败保留等级"), Abilities->FindAbilitySpecFromHandle(Handle)->Level, 2);

	const FGuid ReasonOwnerA = FGuid::NewGuid(), ReasonOwnerB = FGuid::NewGuid();
	TestTrue(TEXT("禁用前可激活"), Abilities->TryActivateAbility(Handle, false));
	ExpectStatus(*this, TEXT("Disable A"), Abilities->SetSourceDisabled(Source, ReasonOwnerA, TEXT("Silence"), true, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	TestFalse(TEXT("禁用直接阻止原生TryActivateAbility"), Abilities->TryActivateAbility(Handle, false));
	ExpectStatus(*this, TEXT("Disable B"), Abilities->SetSourceDisabled(Source, ReasonOwnerB, TEXT("Silence"), true, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	ExpectStatus(*this, TEXT("解除A"), Abilities->SetSourceDisabled(Source, ReasonOwnerA, TEXT("Silence"), false, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	TestFalse(TEXT("另一发起者的同名原因继续阻止激活"), Abilities->TryActivateAbility(Handle, false));
	ExpectStatus(*this, TEXT("空原因拒绝"), Abilities->SetSourceDisabled(Source, ReasonOwnerA, NAME_None, true, Abilities->GetStateRevision()), EAbilitySourceResult::Failed);
	ExpectStatus(*this, TEXT("解除B"), Abilities->SetSourceDisabled(Source, ReasonOwnerB, TEXT("Silence"), false, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	TestTrue(TEXT("全部原因解除可激活"), Abilities->TryActivateAbility(Handle, false));
	TestEqual(TEXT("Disable未撤销Spec"), FirstHandle(Abilities, Source), Handle);
	TestNotNull(TEXT("Disable未撤销来源GE"), Abilities->GetActiveGameplayEffect(Effect));

	const auto Cooldown = AddCooldown(Abilities);
	const auto* CooldownState = Abilities->GetActiveGameplayEffect(Cooldown);
	if (!TestNotNull(TEXT("已提交冷却真实存在"), CooldownState))
	{
		return false;
	}

	const float CooldownStart = CooldownState->StartWorldTime;
	const uint32 BeforeRemap = Abilities->GetStateRevision();
	ExpectStatus(
		*this, TEXT("Remap到空槽"), Abilities->RemapSlots(MobaGameplayTags::Input_Slot_Primary, MobaGameplayTags::Input_Slot_UnitAttack, BeforeRemap), EAbilitySourceResult::Applied);
	TestEqual(TEXT("移槽保留Spec"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack), Handle);
	TestFalse(TEXT("旧槽失效"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary).IsValid());
	ExpectStatus(
		*this, TEXT("重复旧Remap拒绝"), Abilities->RemapSlots(MobaGameplayTags::Input_Slot_UnitAttack, MobaGameplayTags::Input_Slot_Primary, BeforeRemap), EAbilitySourceResult::Failed);
	TestFalse(TEXT("移槽不能绕过冷却"), Abilities->TryActivateAbility(Handle, false));
	auto* Replacement = Fixture.MakeSet(MobaGameplayTags::Input_Slot_Primary);
	ExpectStatus(*this, TEXT("Replace"), Abilities->ReplaceManagedSource(Source, Replacement, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	TestNull(TEXT("Replace回收旧Spec"), Abilities->FindAbilitySpecFromHandle(Handle));
	TestNull(TEXT("Replace回收旧被动"), Abilities->GetActiveGameplayEffect(Effect));
	const auto NewHandle = FirstHandle(Abilities, Source);
	TestTrue(TEXT("Replace创建新Spec"), NewHandle.IsValid() && NewHandle != Handle);
	TestEqual(TEXT("Replace发布新槽"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary), NewHandle);
	TestFalse(TEXT("Replace不能绕过冷却"), Abilities->TryActivateAbility(NewHandle, false));
	CooldownState = Abilities->GetActiveGameplayEffect(Cooldown);
	if (TestNotNull(TEXT("Replace保留已提交冷却"), CooldownState))
	{
		TestEqual(TEXT("冷却未重置"), CooldownState->StartWorldTime, CooldownStart);
	}

	ExpectStatus(*this, TEXT("空Replace拒绝"), Abilities->ReplaceManagedSource(Source, nullptr, Abilities->GetStateRevision()), EAbilitySourceResult::Failed);
	TestEqual(TEXT("失败Replace保留映射"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary), NewHandle);
	ExpectStatus(*this, TEXT("不存在Remove拒绝"), Abilities->RemoveManagedSource(FGuid::NewGuid(), Abilities->GetStateRevision()), EAbilitySourceResult::Failed);
	ExpectStatus(*this, TEXT("Remove"), Abilities->RemoveManagedSource(Source, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	TestEqual(TEXT("Remove清空来源"), Abilities->GetSourceCount(), 0);
	TestEqual(TEXT("Remove清空Spec"), Abilities->GetActivatableAbilities().Num(), 0);
	TestEqual(TEXT("Remove清空槽"), Abilities->GetSlots().Num(), 0);
	TestTrue(TEXT("空映射支持ready"), Abilities->HasReplicatedLoadout(Abilities->GetLoadoutRevision()));
	TestNotNull(TEXT("Remove保留已提交冷却"), Abilities->GetActiveGameplayEffect(Cooldown));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FManagedAbilitySourceIsolationTest, "Moba.Hero.ManagedAbility.SourceIsolationAndCopy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FManagedAbilitySourceIsolationTest::RunTest(const FString& Parameters)
{
	FManagedAbilityWorld Fixture;
	auto* Donor = Fixture.Spawn();
	auto* Recipient = Fixture.Spawn();
	auto* Set = Fixture.MakeSet();
	const FGuid SourceA = FGuid::NewGuid(), SourceB = FGuid::NewGuid(), CopySource = FGuid::NewGuid();
	ExpectStatus(*this, TEXT("无槽来源A"), Donor->GrantManagedSource(Set, SourceA, Donor->GetStateRevision()), EAbilitySourceResult::Applied);
	ExpectStatus(*this, TEXT("相同GA/GE来源B"), Donor->GrantManagedSource(Set, SourceB, Donor->GetStateRevision()), EAbilitySourceResult::Applied);
	const auto HandleA = FirstHandle(Donor, SourceA), HandleB = FirstHandle(Donor, SourceB);
	const auto EffectA = FirstEffect(Donor, SourceA), EffectB = FirstEffect(Donor, SourceB);
	TestTrue(TEXT("不同来源Spec独立"), HandleA.IsValid() && HandleB.IsValid() && HandleA != HandleB);
	TestTrue(TEXT("不同来源GE独立"), EffectA.IsValid() && EffectB.IsValid() && EffectA != EffectB);
	ExpectStatus(*this, TEXT("只撤销A"), Donor->RemoveManagedSource(SourceA, Donor->GetStateRevision()), EAbilitySourceResult::Applied);
	TestNull(TEXT("A Spec回收"), Donor->FindAbilitySpecFromHandle(HandleA));
	TestNull(TEXT("A GE回收"), Donor->GetActiveGameplayEffect(EffectA));
	if (!TestNotNull(TEXT("B Spec保留"), Donor->FindAbilitySpecFromHandle(HandleB)))
	{
		return false;
	}

	TestNotNull(TEXT("B GE保留"), Donor->GetActiveGameplayEffect(EffectB));
	ExpectStatus(*this, TEXT("复制前升级"), Donor->UpgradeManagedAbility(HandleB, 3, Donor->GetStateRevision()), EAbilitySourceResult::Applied);
	const auto DonorCooldown = AddCooldown(Donor);
	ExpectStatus(*this, TEXT("Copy"), Recipient->CopyManagedSource(Donor, SourceB, CopySource, Recipient->GetStateRevision()), EAbilitySourceResult::Applied);
	const auto CopyHandle = FirstHandle(Recipient, CopySource);
	const auto* CopiedSpec = Recipient->FindAbilitySpecFromHandle(CopyHandle);
	if (!TestNotNull(TEXT("Copy真实Spec"), CopiedSpec))
	{
		return false;
	}

	TestEqual(TEXT("Copy继承当前等级"), CopiedSpec->Level, 3);
	TestTrue(TEXT("Copy Spec不共享"), CopyHandle != HandleB);
	TestTrue(TEXT("Copy实例不共享"), CopiedSpec->GetPrimaryInstance() != Donor->FindAbilitySpecFromHandle(HandleB)->GetPrimaryInstance());
	TestTrue(TEXT("Copy GE不共享"), FirstEffect(Recipient, CopySource) != EffectB);
	TestFalse(TEXT("源冷却仍阻止源激活"), Donor->TryActivateAbility(HandleB, false));
	TestTrue(TEXT("源冷却不复制到接收者"), Recipient->TryActivateAbility(CopyHandle, false));
	const auto RecipientCooldown = AddCooldown(Recipient);
	const FGuid SecondCopy = FGuid::NewGuid();
	ExpectStatus(*this, TEXT("接收者冷却中Copy仍授予"), Recipient->CopyManagedSource(Donor, SourceB, SecondCopy, Recipient->GetStateRevision()), EAbilitySourceResult::Applied);
	TestFalse(TEXT("Copy不清接收者同族冷却"), Recipient->TryActivateAbility(FirstHandle(Recipient, SecondCopy), false));
	TestNotNull(TEXT("接收者原冷却handle保留"), Recipient->GetActiveGameplayEffect(RecipientCooldown));
	TestNotNull(TEXT("源原冷却handle保留"), Donor->GetActiveGameplayEffect(DonorCooldown));

	auto* PrivateSet = Fixture.MakeSet();
	PrivateSet->bAllowCopy = false;
	const FGuid PrivateSource = FGuid::NewGuid();
	ExpectStatus(*this, TEXT("不可复制来源Grant"), Donor->GrantManagedSource(PrivateSet, PrivateSource, Donor->GetStateRevision()), EAbilitySourceResult::Applied);
	const int32 RecipientCount = Recipient->GetSourceCount();
	ExpectStatus(*this, TEXT("不可复制策略拒绝"), Recipient->CopyManagedSource(Donor, PrivateSource, FGuid::NewGuid(), Recipient->GetStateRevision()), EAbilitySourceResult::Failed);
	TestEqual(TEXT("失败Copy无新来源"), Recipient->GetSourceCount(), RecipientCount);

	auto* EffectOnly = NewObject<UAbilitySetDefinition>(Fixture.World);
	EffectOnly->Effects.Add(UManagedAbilityPassiveEffect::StaticClass());
	const FGuid EffectSource = FGuid::NewGuid();
	ExpectStatus(*this, TEXT("仅效果来源"), Recipient->GrantManagedSource(EffectOnly, EffectSource, Recipient->GetStateRevision()), EAbilitySourceResult::Applied);
	TestNotNull(TEXT("仅效果来源真实GE"), Recipient->GetActiveGameplayEffect(FirstEffect(Recipient, EffectSource)));
	ExpectStatus(*this, TEXT("仅效果来源撤销"), Recipient->RemoveManagedSource(EffectSource, Recipient->GetStateRevision()), EAbilitySourceResult::Applied);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManagedAbilityRollbackTest, "Moba.Hero.ManagedAbility.Rollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FManagedAbilityRollbackTest::RunTest(const FString& Parameters)
{
	FManagedAbilityWorld Fixture;
	auto* Abilities = Fixture.Spawn();
	const FGuid ExistingSource = FGuid::NewGuid();
	auto* Existing = Fixture.MakeSet(MobaGameplayTags::Input_Slot_Primary);
	ExpectStatus(*this, TEXT("已有来源"), Abilities->GrantManagedSource(Existing, ExistingSource, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	const auto ExistingHandle = FirstHandle(Abilities, ExistingSource);
	const auto ExistingEffect = FirstEffect(Abilities, ExistingSource);
	const uint32 State = Abilities->GetStateRevision(), Mapping = Abilities->GetLoadoutRevision();
	auto* Rejected = Fixture.MakeSet(MobaGameplayTags::Input_Slot_UnitAttack);
	Rejected->Effects.Add(UManagedAbilityRejectedEffect::StaticClass());
	FString ValidationReason;
	TestTrue(TEXT("原生标签条件GE通过纯来源预检"), Rejected->ValidateGrant(ValidationReason));
	const FGuid FailedSource = FGuid::NewGuid();
	const auto RejectedGrant = Abilities->GrantManagedSource(Rejected, FailedSource, State);
	ExpectStatus(*this, TEXT("真实GE应用拒绝使Grant失败"), RejectedGrant, EAbilitySourceResult::Failed);
	TestEqual(TEXT("已经进入真实GE应用阶段"), RejectedGrant.Reason, FName(TEXT("GrantFailed")));
	TestNull(TEXT("失败未发布来源"), Abilities->FindSource(FailedSource));
	TestEqual(TEXT("失败已回收临时Spec"), Abilities->GetActivatableAbilities().Num(), 1);
	TestEqual(TEXT("失败已回收先成功应用的GE"), Abilities->GetActiveEffects(FGameplayEffectQuery()).Num(), 1);
	TestEqual(TEXT("失败未推进状态"), Abilities->GetStateRevision(), State);
	TestEqual(TEXT("失败未推进映射"), Abilities->GetLoadoutRevision(), Mapping);
	TestEqual(TEXT("失败未改原映射"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary), ExistingHandle);
	TestNotNull(TEXT("失败保留原GE"), Abilities->GetActiveGameplayEffect(ExistingEffect));
	TestFalse(TEXT("失败无新槽"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack).IsValid());
	const auto RejectedReplace = Abilities->ReplaceManagedSource(ExistingSource, Rejected, State);
	ExpectStatus(*this, TEXT("真实GE拒绝使Replace失败"), RejectedReplace, EAbilitySourceResult::Failed);
	TestEqual(TEXT("Replace同样在真实GE应用阶段失败"), RejectedReplace.Reason, FName(TEXT("GrantFailed")));
	TestEqual(TEXT("Replace回滚保留原Spec"), FirstHandle(Abilities, ExistingSource), ExistingHandle);
	TestEqual(TEXT("Replace回滚无残留GE"), Abilities->GetActiveEffects(FGameplayEffectQuery()).Num(), 1);
	TestEqual(TEXT("Replace回滚保留版本"), Abilities->GetStateRevision(), State);
	ExpectStatus(*this, TEXT("回滚后可重新使用空槽"), Abilities->GrantManagedSource(Fixture.MakeSet(MobaGameplayTags::Input_Slot_UnitAttack), FailedSource, Abilities->GetStateRevision()),
		EAbilitySourceResult::Applied);
	const auto RecoveredHandle = FirstHandle(Abilities, FailedSource);
	TestTrue(TEXT("回滚不残留InputID阻断"), Abilities->TryActivateAbility(RecoveredHandle, false));
	ExpectStatus(*this, TEXT("Remap交换两个已占用槽"), Abilities->RemapSlots(MobaGameplayTags::Input_Slot_Primary, MobaGameplayTags::Input_Slot_UnitAttack, Abilities->GetStateRevision()),
		EAbilitySourceResult::Applied);
	TestEqual(TEXT("交换后原来源移动到目标槽"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_UnitAttack), ExistingHandle);
	TestEqual(TEXT("交换后目标来源移动到原槽"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary), RecoveredHandle);
	return true;
}

namespace
{
	class FManagedAbilityLifetimeScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FManagedAbilityLifetimeScenario(FAutomationTestBase* InTest) : Test(InTest) {}

		virtual bool Update() override
		{
			if (!Fixture)
			{
				Fixture = MakeUnique<FManagedAbilityWorld>();
				Abilities = Fixture->Spawn();
				auto* Set = Fixture->MakeSet(MobaGameplayTags::Input_Slot_Primary, UManagedAbilityUncancelableAbility::StaticClass());
				auto& Spare = Set->Abilities.AddDefaulted_GetRef();
				Spare.Ability = UManagedAbilityTestAbility::StaticClass();
				Source = FGuid::NewGuid();
				ExpectStatus(*Test, TEXT("负临时期限拒绝"), Abilities->GrantManagedSource(Set, Source, Abilities->GetStateRevision(), -1), EAbilitySourceResult::Failed);
				ExpectStatus(*Test, TEXT("TemporaryGrant"), Abilities->GrantManagedSource(Set, Source, Abilities->GetStateRevision(), 0.3f), EAbilitySourceResult::Applied);
				Handle = FirstHandle(Abilities, Source);
				Effect = FirstEffect(Abilities, Source);
				const auto* Record = Abilities->FindSource(Source);
				if (!Test->TestNotNull(TEXT("临时来源"), Record))
				{
					return true;
				}

				Expiry = Record->ExpiryTimer;
				SpareHandle = Record->Abilities[1];
				Test->TestTrue(TEXT("临时授予有真实计时器"), Fixture->World->GetTimerManager().TimerExists(Expiry));
				if (!Test->TestTrue(TEXT("不可取消能力真实激活"), Abilities->TryActivateAbility(Handle, false)))
				{
					return true;
				}

				auto* Spec = Abilities->FindAbilitySpecFromHandle(Handle);
				if (!Test->TestNotNull(TEXT("活动Spec存在"), Spec))
				{
					return true;
				}

				Test->TestTrue(TEXT("真实active"), Spec->IsActive());
				const uint32 State = Abilities->GetStateRevision(), Mapping = Abilities->GetLoadoutRevision();
				ExpectStatus(*Test, TEXT("不可取消Replace Busy"), Abilities->ReplaceManagedSource(Source, Fixture->MakeSet(), State), EAbilitySourceResult::Busy);
				ExpectStatus(*Test, TEXT("活动Upgrade Busy"), Abilities->UpgradeManagedAbility(Handle, 2, State), EAbilitySourceResult::Busy);
				Test->TestEqual(TEXT("Busy保留映射"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary), Handle);
				Test->TestEqual(TEXT("Busy保留状态版本"), Abilities->GetStateRevision(), State);
				Test->TestEqual(TEXT("Busy保留映射版本"), Abilities->GetLoadoutRevision(), Mapping);
				return false;
			}

			// TimerManager 每个 GFrameCounter 只推进一次；由 Automation 逐真实帧调用。
			Fixture->World->Tick(LEVELTICK_All, 0.1f);
			++Frames;
			if (Frames > 30)
			{
				Test->AddError(TEXT("临时来源在3秒fixture时间内没有完成预期状态转换"));
				return true;
			}

			if (!bEnded)
			{
				const auto* Record = Abilities->FindSource(Source);
				if (!Record || !Record->bPendingRemove)
				{
					return false;
				}

				Test->TestFalse(TEXT("到期禁止来源新输入"), Abilities->IsManagedAbilityEnabled(Handle));
				Test->TestFalse(TEXT("到期原生激活拒绝"), Abilities->TryActivateAbility(Handle, false));
				Test->TestFalse(TEXT("到期阻止同来源原本未活动能力的新激活"), Abilities->TryActivateAbility(SpareHandle, false));
				Test->TestFalse(TEXT("到期计时器已消耗"), Fixture->World->GetTimerManager().TimerExists(Expiry));
				auto* Spec = Abilities->FindAbilitySpecFromHandle(Handle);
				if (!Test->TestNotNull(TEXT("Pending保留活动Spec"), Spec))
				{
					return true;
				}

				Test->TestTrue(TEXT("不能取消的真实能力仍活动"), Spec->IsActive());
				Test->TestNotNull(TEXT("Pending保留来源被动直到结束"), Abilities->GetActiveGameplayEffect(Effect));
				auto* Instance = Cast<UManagedAbilityHoldingAbility>(Spec->GetPrimaryInstance());
				if (!Test->TestNotNull(TEXT("有真实运行实例"), Instance))
				{
					return true;
				}

				Instance->Finish();
				bEnded = true;
				return false;
			}

			if (Abilities->FindSource(Source))
			{
				return false;
			}

			Test->TestNull(TEXT("结束后Spec回收"), Abilities->FindAbilitySpecFromHandle(Handle));
			Test->TestNull(TEXT("结束后来源GE回收"), Abilities->GetActiveGameplayEffect(Effect));
			Test->TestEqual(TEXT("结束后槽回收"), Abilities->GetSlots().Num(), 0);

			const FGuid CancelSource = FGuid::NewGuid();
			auto* CancelSet = Fixture->MakeSet({}, UManagedAbilityHoldingAbility::StaticClass());
			ExpectStatus(*Test, TEXT("可取消来源授予"), Abilities->GrantManagedSource(CancelSet, CancelSource, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
			const auto CancelHandle = FirstHandle(Abilities, CancelSource);
			Test->TestTrue(TEXT("可取消能力真实激活"), Abilities->TryActivateAbility(CancelHandle, false));
			ExpectStatus(
				*Test, TEXT("可取消活动Replace成功"), Abilities->ReplaceManagedSource(CancelSource, Fixture->MakeSet(), Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
			Test->TestNull(TEXT("被替换活动Spec回收"), Abilities->FindAbilitySpecFromHandle(CancelHandle));

			const FGuid TeardownSource = FGuid::NewGuid();
			ExpectStatus(
				*Test, TEXT("销毁前建立长期计时器"), Abilities->GrantManagedSource(Fixture->MakeSet(), TeardownSource, Abilities->GetStateRevision(), 60), EAbilitySourceResult::Applied);
			const auto* TeardownRecord = Abilities->FindSource(TeardownSource);
			if (!Test->TestNotNull(TEXT("销毁来源存在"), TeardownRecord))
			{
				return true;
			}

			const FTimerHandle TeardownTimer = TeardownRecord->ExpiryTimer;
			Test->TestTrue(TEXT("销毁前计时器存在"), Fixture->World->GetTimerManager().TimerExists(TeardownTimer));
			Abilities->GetOwner()->Destroy();
			Test->TestFalse(TEXT("Actor EndPlay移除来源计时器"), Fixture->World->GetTimerManager().TimerExists(TeardownTimer));
			Test->TestEqual(TEXT("Actor EndPlay清空来源"), Abilities->GetSourceCount(), 0);

			auto* WorldAbilities = Fixture->Spawn();
			const FGuid WorldSource = FGuid::NewGuid();
			ExpectStatus(
				*Test, TEXT("World销毁前授予"), WorldAbilities->GrantManagedSource(Fixture->MakeSet(), WorldSource, WorldAbilities->GetStateRevision(), 60), EAbilitySourceResult::Applied);
			Fixture->Destroy();
			Test->TestEqual(TEXT("World teardown清空来源"), WorldAbilities->GetSourceCount(), 0);
			return true;
		}


	private:
		FAutomationTestBase* Test;

		TUniquePtr<FManagedAbilityWorld> Fixture;

		UHeroAbilitySystemComponent* Abilities = nullptr;

		FGuid Source;

		FGameplayAbilitySpecHandle Handle;

		FGameplayAbilitySpecHandle SpareHandle;

		FActiveGameplayEffectHandle Effect;

		FTimerHandle Expiry;

		int32 Frames = 0;

		bool bEnded = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManagedAbilityLifetimeTest, "Moba.Hero.ManagedAbility.ActiveAndTemporaryLifetime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FManagedAbilityLifetimeTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FManagedAbilityLifetimeScenario>(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManagedAbilityLegacyLevelTest, "Moba.Hero.ManagedAbility.LegacyInitialLevel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FManagedAbilityLegacyLevelTest::RunTest(const FString& Parameters)
{
	FManagedAbilityWorld Fixture;
	auto* Abilities = Fixture.Spawn();
	auto* LegacySet = NewObject<UAbilitySetDefinition>(Fixture.World);
	auto& Entry = LegacySet->Abilities.AddDefaulted_GetRef();
	Entry.Ability = UManagedAbilityTestAbility::StaticClass();
	Entry.Slot = MobaGameplayTags::Input_Slot_Primary;
	Entry.Level = 7;
	TestEqual(TEXT("未配置MaxLevel的旧内容使用零默认值"), Entry.MaxLevel, 0);
	const FGuid Source = FGuid::NewGuid();
	ExpectStatus(*this, TEXT("旧初始等级7合法授予"), Abilities->GrantManagedSource(LegacySet, Source, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	const auto Handle = FirstHandle(Abilities, Source);
	const auto* Spec = Abilities->FindAbilitySpecFromHandle(Handle);
	if (!TestNotNull(TEXT("旧配置实际创建Spec"), Spec))
	{
		return false;
	}

	TestEqual(TEXT("旧配置保留初始等级7"), Spec->Level, 7);
	const uint32 State = Abilities->GetStateRevision();
	ExpectStatus(*this, TEXT("零Max只保持初始等级不允许升8"), Abilities->UpgradeManagedAbility(Handle, 8, State), EAbilitySourceResult::Failed);
	TestEqual(TEXT("拒绝升级仍是原Spec7"), Abilities->FindAbilitySpecFromHandle(Handle)->Level, 7);
	TestEqual(TEXT("拒绝升级不改状态版本"), Abilities->GetStateRevision(), State);
	auto* InvalidSet = Fixture.MakeSet();
	InvalidSet->Abilities[0].Level = 7;
	InvalidSet->Abilities[0].MaxLevel = 6;
	FString Reason;
	TestFalse(TEXT("显式上限低于初始等级仍无效"), InvalidSet->ValidateGrant(Reason));
	ExpectStatus(*this, TEXT("显式低上限不能授予"), Abilities->GrantManagedSource(InvalidSet, FGuid::NewGuid(), State), EAbilitySourceResult::Failed);
	TestEqual(TEXT("无失败配置的额外Spec"), Abilities->GetActivatableAbilities().Num(), 1);
	TestEqual(TEXT("无失败配置的额外来源"), Abilities->GetSourceCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManagedAbilityEffectPolicyTest, "Moba.Hero.ManagedAbility.EffectComponentPolicy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FManagedAbilityEffectPolicyTest::RunTest(const FString& Parameters)
{
	FManagedAbilityWorld Fixture;
	auto* Abilities = Fixture.Spawn();
	Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetMaxManaAttribute(), 100);
	Abilities->SetNumericAttributeBase(UHeroAttributeSet::GetManaAttribute(), 100);
	const FGuid Source = FGuid::NewGuid();
	auto* Existing = Fixture.MakeSet(MobaGameplayTags::Input_Slot_Primary, UCombatCancellationTestAbility::StaticClass());
	ExpectStatus(*this, TEXT("已有来源"), Abilities->GrantManagedSource(Existing, Source, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
	const auto OldHandle = FirstHandle(Abilities, Source);
	const auto OldEffect = FirstEffect(Abilities, Source);
	if (!TestTrue(TEXT("真实旧能力处于可取消活动状态"), Abilities->TryActivateAbility(OldHandle, false)))
	{
		return false;
	}

	const auto Cooldown = AddCooldown(Abilities);
	if (!TestNotNull(TEXT("原冷却已应用"), Abilities->GetActiveGameplayEffect(Cooldown)))
	{
		return false;
	}

	const float Start = Abilities->GetActiveGameplayEffect(Cooldown)->StartWorldTime;
	const uint32 State = Abilities->GetStateRevision(), Mapping = Abilities->GetLoadoutRevision();
	UManagedAbilityCustomTagsComponent::ApplicationCount = 0;
	UManagedAbilityRejectRequirement::ApplicationAttempts = 0;
	const TArray<TSubclassOf<UGameplayEffect>> Forbidden = {UManagedAbilityAdditionalEffect::StaticClass(), UManagedAbilityRemoveOtherEffect::StaticClass(),
		UManagedAbilityCancelEffect::StaticClass(), UManagedAbilityGrantAbilityEffect::StaticClass(), UManagedAbilityCustomComponentEffect::StaticClass(),
		UManagedAbilityCustomRequirementEffect::StaticClass(), UManagedAbilityRemovalRequirementEffect::StaticClass(), UManagedAbilityOngoingRequirementEffect::StaticClass()};
	for (const auto Class : Forbidden)
	{
		auto* Set = Fixture.MakeSet(MobaGameplayTags::Input_Slot_UnitAttack);
		Set->Effects.Reset();
		Set->Effects.Add(Class.Get());
		FString Reason;
		TestFalse(FString::Printf(TEXT("%s必须由预检拒绝"), *Class->GetName()), Set->ValidateGrant(Reason));
		const FGuid FailedSource = FGuid::NewGuid();
		const auto Result = Abilities->GrantManagedSource(Set, FailedSource, State);
		ExpectStatus(*this, TEXT("禁止组件授予拒绝"), Result, EAbilitySourceResult::Failed);
		TestTrue(TEXT("失败发生于预检而非应用回滚"), !Result.Reason.IsNone() && Result.Reason != TEXT("GrantFailed"));
		TestNull(TEXT("无失败来源"), Abilities->FindSource(FailedSource));
		TestEqual(TEXT("无额外来源"), Abilities->GetSourceCount(), 1);
		TestEqual(TEXT("不发放额外Spec"), Abilities->GetActivatableAbilities().Num(), 1);
		TestEqual(TEXT("不保留额外GE"), Abilities->GetActiveEffects(FGameplayEffectQuery()).Num(), 2);
		TestEqual(TEXT("无Instant子效果成本"), Abilities->GetNumericAttribute(UHeroAttributeSet::GetManaAttribute()), 100.f);
		TestNotNull(TEXT("原来源被动未删"), Abilities->GetActiveGameplayEffect(OldEffect));
		const auto* OldSpec = Abilities->FindAbilitySpecFromHandle(OldHandle);
		TestTrue(TEXT("原能力未被取消或撤销"), OldSpec && OldSpec->IsActive());
		const auto* OldCooldown = Abilities->GetActiveGameplayEffect(Cooldown);
		if (TestNotNull(TEXT("原冷却未删"), OldCooldown))
		{
			TestEqual(TEXT("原冷却未重置"), OldCooldown->StartWorldTime, Start);
		}

		TestEqual(TEXT("状态版本不变"), Abilities->GetStateRevision(), State);
		TestEqual(TEXT("槽版本不变"), Abilities->GetLoadoutRevision(), Mapping);
		TestEqual(TEXT("原槽不变"), Abilities->FindSlot(MobaGameplayTags::Input_Slot_Primary), OldHandle);
		if (HasAnyErrors())
		{
			return false;
		}
	}

	TestEqual(TEXT("自定义白名单派生从未执行"), UManagedAbilityCustomTagsComponent::ApplicationCount, 0);
	TestEqual(TEXT("自定义ApplicationRequirement从未执行"), UManagedAbilityRejectRequirement::ApplicationAttempts, 0);
	return true;
}

namespace
{
	class FManagedAbilityPendingEventScenario final : public IAutomationLatentCommand
	{

	public:
		explicit FManagedAbilityPendingEventScenario(FAutomationTestBase* InTest) : Test(InTest) {}

		virtual bool Update() override
		{
			if (!Fixture)
			{
				Fixture = MakeUnique<FManagedAbilityWorld>();
				Abilities = Fixture->Spawn();
				Source = FGuid::NewGuid();
				auto* Set = Fixture->MakeSet({}, UManagedAbilityEventHoldingAbility::StaticClass());
				ExpectStatus(*Test, TEXT("事件来源临时授予"), Abilities->GrantManagedSource(Set, Source, Abilities->GetStateRevision(), 0.3f), EAbilitySourceResult::Applied);
				Handle = FirstHandle(Abilities, Source);
				Effect = FirstEffect(Abilities, Source);
				if (!Test->TestTrue(TEXT("原生GameplayEvent入口真实启动"), Trigger()))
				{
					return true;
				}

				const auto* Spec = Abilities->FindAbilitySpecFromHandle(Handle);
				if (!Test->TestNotNull(TEXT("活动事件Spec"), Spec))
				{
					return true;
				}

				InputId = Spec->InputID;
				Test->TestEqual(TEXT("首次事件只有一个实例"), int32(Spec->ActiveCount), 1);
				return false;
			}

			Fixture->World->Tick(LEVELTICK_All, 0.1f);
			if (++Frames > 30)
			{
				Test->AddError(TEXT("Pending GameplayEvent来源3秒内未回收"));
				return true;
			}

			if (!bEnded)
			{
				const auto* Record = Abilities->FindSource(Source);
				if (!Record || !Record->bPendingRemove)
				{
					return false;
				}

				Test->TestFalse(TEXT("Pending同一GameplayEvent必须拒绝"), Trigger());
				auto* Spec = Abilities->FindAbilitySpecFromHandle(Handle);
				if (!Test->TestNotNull(TEXT("原不可取消实例仍在"), Spec))
				{
					return true;
				}

				Test->TestEqual(TEXT("到期事件没有新增执行实例"), int32(Spec->ActiveCount), 1);
				Test->TestTrue(TEXT("Pending原生InputID仍阻断"), Abilities->IsAbilityInputBlocked(InputId));
				const auto Instances = Spec->GetAbilityInstances();
				// 即使断言失败也结束本夹具已经创建的真实实例，避免污染后续测试。
				for (auto* Ability : Instances)
				{
					if (auto* Instance = Cast<UManagedAbilityHoldingAbility>(Ability); Instance && Instance->IsActive())
					{
						Instance->Finish();
					}
				}

				bEnded = true;
				return false;
			}

			if (Abilities->FindSource(Source))
			{
				return false;
			}

			Test->TestNull(TEXT("事件实例结束后Spec回收"), Abilities->FindAbilitySpecFromHandle(Handle));
			Test->TestNull(TEXT("事件来源GE回收"), Abilities->GetActiveGameplayEffect(Effect));
			Test->TestFalse(TEXT("事件来源回收平衡InputID阻断"), Abilities->IsAbilityInputBlocked(InputId));
			const FGuid NewSource = FGuid::NewGuid();
			ExpectStatus(*Test, TEXT("复用ID的新来源"), Abilities->GrantManagedSource(Fixture->MakeSet(), NewSource, Abilities->GetStateRevision()), EAbilitySourceResult::Applied);
			const auto NewHandle = FirstHandle(Abilities, NewSource);
			const auto* NewSpec = Abilities->FindAbilitySpecFromHandle(NewHandle);
			if (!Test->TestNotNull(TEXT("复用ID的新Spec"), NewSpec))
			{
				return true;
			}

			Test->TestEqual(TEXT("已释放InputID可以复用"), NewSpec->InputID, InputId);
			Test->TestTrue(TEXT("复用InputID无残留阻断"), Abilities->TryActivateAbility(NewHandle, false));
			return true;
		}


	private:
		bool Trigger()
		{
			FGameplayEventData Payload;
			Payload.EventTag = MobaGameplayTags::Event_UnitAttack;
			return Abilities->TriggerAbilityFromGameplayEvent(Handle, Abilities->AbilityActorInfo.Get(), Payload.EventTag, &Payload, *Abilities);
		}


	private:
		FAutomationTestBase* Test;

		TUniquePtr<FManagedAbilityWorld> Fixture;

		UHeroAbilitySystemComponent* Abilities = nullptr;

		FGuid Source;

		FGameplayAbilitySpecHandle Handle;

		FActiveGameplayEffectHandle Effect;

		int32 InputId = INDEX_NONE;

		int32 Frames = 0;

		bool bEnded = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManagedAbilityPendingEventTest, "Moba.Hero.ManagedAbility.PendingGameplayEvent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FManagedAbilityPendingEventTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FManagedAbilityPendingEventScenario>(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManagedAbilityReactiveContextTest, "Moba.Hero.ManagedAbility.TagReactiveContext", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FManagedAbilityReactiveContextTest::RunTest(const FString& Parameters)
{
	const TArray<TSubclassOf<UGameplayEffect>> ReactiveEffects = {UManagedAbilityRemovalRequirementEffect::StaticClass(), UManagedAbilityOngoingRequirementEffect::StaticClass()};
	for (const auto Class : ReactiveEffects)
	{
		FManagedAbilityWorld Fixture;
		auto* Abilities = Fixture.Spawn();
		// 已提交效果使用原生Apply入口，不冒充允许进入来源管理的配置。
		const auto EffectSpec = Abilities->MakeOutgoingSpec(Class, 1, Abilities->MakeEffectContext());
		if (!TestTrue(TEXT("非来源GE Spec有效"), EffectSpec.IsValid()))
		{
			return false;
		}

		const auto EffectHandle = Abilities->ApplyGameplayEffectSpecToSelf(*EffectSpec.Data);
		const auto* ExistingEffect = Abilities->GetActiveGameplayEffect(EffectHandle);
		if (!TestNotNull(TEXT("非来源条件GE实际存在"), ExistingEffect))
		{
			return false;
		}

		const bool bOriginallyInhibited = ExistingEffect->bIsInhibited;
		const float OriginalStart = ExistingEffect->StartWorldTime;
		auto* Set = Fixture.MakeSet();
		Set->Effects = {UManagedAbilityGrantedTagEffect::StaticClass()};
		FString Reason;
		TestTrue(TEXT("新来源定义本身合法"), Set->ValidateGrant(Reason));
		const FGuid Source = FGuid::NewGuid();
		const uint32 State = Abilities->GetStateRevision();
		const uint32 Mapping = Abilities->GetLoadoutRevision();
		const auto Result = Abilities->GrantManagedSource(Set, Source, State);
		ExpectStatus(*this, TEXT("已有非来源反应GE时拒绝新tag来源"), Result, EAbilitySourceResult::Failed);
		TestEqual(TEXT("具体上下文拒绝原因"), Result.Reason, FName(TEXT("TagReactiveContext")));
		ExistingEffect = Abilities->GetActiveGameplayEffect(EffectHandle);
		if (!TestNotNull(TEXT("失败未删除已提交GE"), ExistingEffect))
		{
			return false;
		}

		TestEqual(TEXT("失败未改变原抑制状态"), ExistingEffect->bIsInhibited, bOriginallyInhibited);
		TestEqual(TEXT("失败未重置旧GE开始时间"), ExistingEffect->StartWorldTime, OriginalStart);
		TestFalse(TEXT("失败从未授予新tag"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::State_Untargetable));
		TestEqual(TEXT("失败没有新Spec"), Abilities->GetActivatableAbilities().Num(), 0);
		TestEqual(TEXT("失败没有新来源"), Abilities->GetSourceCount(), 0);
		TestEqual(TEXT("失败没有新GE"), Abilities->GetActiveEffects(FGameplayEffectQuery()).Num(), 1);
		TestEqual(TEXT("失败状态版本不变"), Abilities->GetStateRevision(), State);
		TestEqual(TEXT("失败槽版本不变"), Abilities->GetLoadoutRevision(), Mapping);
		if (HasAnyErrors())
		{
			return false;
		}

		TestTrue(TEXT("正常移除已提交反应GE"), Abilities->RemoveActiveGameplayEffect(EffectHandle));
		ExpectStatus(*this, TEXT("移除反应上下文后同请求成功"), Abilities->GrantManagedSource(Set, Source, State), EAbilitySourceResult::Applied);
		TestTrue(TEXT("成功请求真实授予tag"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::State_Untargetable));
		TestTrue(TEXT("成功Spec实际可激活"), Abilities->TryActivateAbility(FirstHandle(Abilities, Source), false));
	}

	{
		FManagedAbilityWorld Fixture;
		auto* Abilities = Fixture.Spawn();
		UManagedAbilityOwnedTagTestAbility::ActivationCount = 0;
		const auto ExistingHandle = Abilities->GiveAbility(FGameplayAbilitySpec(UManagedAbilityOwnedTagTestAbility::StaticClass(), 1));
		if (!TestNotNull(TEXT("已有非来源OwnedTag能力"), Abilities->FindAbilitySpecFromHandle(ExistingHandle)))
		{
			return false;
		}

		auto* Set = Fixture.MakeSet();
		Set->Effects = {UManagedAbilityGrantedTagEffect::StaticClass()};
		const FGuid Source = FGuid::NewGuid();
		const uint32 State = Abilities->GetStateRevision();
		const auto Result = Abilities->GrantManagedSource(Set, Source, State);
		ExpectStatus(*this, TEXT("已有OwnedTag触发能力时拒绝"), Result, EAbilitySourceResult::Failed);
		TestEqual(TEXT("OwnedTag上下文拒绝原因"), Result.Reason, FName(TEXT("TagReactiveContext")));
		TestEqual(TEXT("旧OwnedTag能力从未触发"), UManagedAbilityOwnedTagTestAbility::ActivationCount, 0);
		TestNotNull(TEXT("旧OwnedTag Spec仍存在"), Abilities->FindAbilitySpecFromHandle(ExistingHandle));
		TestEqual(TEXT("拒绝没有额外Spec"), Abilities->GetActivatableAbilities().Num(), 1);
		TestEqual(TEXT("拒绝没有来源"), Abilities->GetSourceCount(), 0);
		TestEqual(TEXT("拒绝没有GE"), Abilities->GetActiveEffects(FGameplayEffectQuery()).Num(), 0);
		TestEqual(TEXT("拒绝未推进状态"), Abilities->GetStateRevision(), State);
		TestFalse(TEXT("拒绝无输入阻断残留"), Abilities->IsAbilityInputBlocked(0));
		if (HasAnyErrors())
		{
			return false;
		}

		Abilities->ClearAbility(ExistingHandle);
		ExpectStatus(*this, TEXT("移除OwnedTag能力后同请求成功"), Abilities->GrantManagedSource(Set, Source, State), EAbilitySourceResult::Applied);
		TestTrue(TEXT("成功后tag已应用"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::State_Untargetable));
		TestEqual(TEXT("被移除的旧能力没有再次激活"), UManagedAbilityOwnedTagTestAbility::ActivationCount, 0);
	}

	{
		FManagedAbilityWorld Fixture;
		auto* Abilities = Fixture.Spawn();
		UManagedAbilityOwnedTagTestAbility::ActivationCount = 0;
		auto* Set = Fixture.MakeSet({}, UManagedAbilityOwnedTagTestAbility::StaticClass());
		Set->Effects = {UManagedAbilityGrantedTagEffect::StaticClass()};
		FString Reason;
		TestTrue(TEXT("自身trigger配置在纯定义预检层合法"), Set->ValidateGrant(Reason));
		const FGuid Source = FGuid::NewGuid();
		const uint32 State = Abilities->GetStateRevision();
		const auto Result = Abilities->GrantManagedSource(Set, Source, State);
		ExpectStatus(*this, TEXT("新Spec注册自身OwnedTag后拒绝"), Result, EAbilitySourceResult::Failed);
		TestEqual(TEXT("stage后分类保持TagReactiveContext"), Result.Reason, FName(TEXT("TagReactiveContext")));
		TestEqual(TEXT("已stage的自身trigger Spec全部回收"), Abilities->GetActivatableAbilities().Num(), 0);
		TestEqual(TEXT("自身trigger从未运行"), UManagedAbilityOwnedTagTestAbility::ActivationCount, 0);
		TestEqual(TEXT("stage拒绝未应用GE"), Abilities->GetActiveEffects(FGameplayEffectQuery()).Num(), 0);
		TestEqual(TEXT("stage拒绝未发布来源"), Abilities->GetSourceCount(), 0);
		TestEqual(TEXT("stage拒绝未推进状态"), Abilities->GetStateRevision(), State);
		TestFalse(TEXT("stage拒绝InputID已平衡"), Abilities->IsAbilityInputBlocked(0));
		if (HasAnyErrors())
		{
			return false;
		}

		// 自带反应者的定义仍应拒绝；改用无反应者定义验证同Source/ID没有残留上下文。
		auto* SafeSet = Fixture.MakeSet();
		SafeSet->Effects = {UManagedAbilityGrantedTagEffect::StaticClass()};
		ExpectStatus(*this, TEXT("正常定义复用失败Source成功"), Abilities->GrantManagedSource(SafeSet, Source, State), EAbilitySourceResult::Applied);
		const auto Handle = FirstHandle(Abilities, Source);
		const auto* Spec = Abilities->FindAbilitySpecFromHandle(Handle);
		if (!TestNotNull(TEXT("复用Source真实Spec"), Spec))
		{
			return false;
		}

		TestEqual(TEXT("stage回滚释放InputID可再次使用"), Spec->InputID, 0);
		TestTrue(TEXT("stage回滚后激活未被残留阻断"), Abilities->TryActivateAbility(Handle, false));
		TestTrue(TEXT("stage回滚后原生tag授予成功"), Abilities->HasMatchingGameplayTag(MobaGameplayTags::State_Untargetable));
		TestEqual(TEXT("回收的自身trigger不再监听tag"), UManagedAbilityOwnedTagTestAbility::ActivationCount, 0);
	}

	return true;
}

#endif
