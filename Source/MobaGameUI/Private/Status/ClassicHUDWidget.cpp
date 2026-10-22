#include "Status/ClassicHUDWidget.h"

#include "AbilitySystem/HeroAbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Combat/HeroAttributeSet.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Engine/World.h"
#include "Experience/ExperienceSubsystem.h"
#include "Hero/HeroCharacter.h"
#include "Hero/HeroAttributeComponent.h"
#include "InputMappingContext.h"
#include "Items/ItemInventoryComponent.h"
#include "Match/ParticipantState.h"
#include "MobaGameplayTags.h"
#include "MobaLogChannels.h"
#include "MobaMessageTags.h"
#include "Player/HeroPlayerController.h"
#include "Status/ClassicHUDSlotWidget.h"
#include "TimerManager.h"

#define LOCTEXT_NAMESPACE "ClassicHUD"

namespace
{
	FText Number(float Value)
	{
		FNumberFormattingOptions Options;
		Options.SetMaximumFractionalDigits(1);
		return FMath::IsFinite(Value) ? FText::AsNumber(Value, &Options) : LOCTEXT("Unavailable", "—");
	}

	/** 只查询 Spec 的真实 GE 冷却；不依赖按钮点击时间或 UI 自建结束时间。 */
	bool ReadCooldown(UHeroAbilitySystemComponent* Abilities, FGameplayAbilitySpecHandle Handle, bool& bKnown, float& Remaining, float& Duration)
	{
		bKnown = false;
		Remaining = Duration = 0;
		const auto* Spec = Abilities ? Abilities->FindAbilitySpecFromHandle(Handle) : nullptr;
		const auto* Ability = Spec ? (Spec->GetPrimaryInstance() ? Spec->GetPrimaryInstance() : Spec->Ability.Get()) : nullptr;
		if (!Ability || !Abilities->AbilityActorInfo.IsValid())
		{
			return false;
		}

		Ability->GetCooldownTimeRemainingAndDuration(Handle, Abilities->AbilityActorInfo.Get(), Remaining, Duration);
		const auto* Tags = Ability->GetCooldownTags();
		const bool bTagged = (Tags && Abilities->HasAnyMatchingGameplayTags(*Tags));
		bKnown = (FMath::IsFinite(Remaining) && FMath::IsFinite(Duration) && Remaining >= 0 && Duration >= 0 && (!bTagged || Remaining > 0));
		Remaining = bKnown ? FMath::Max(0.f, Remaining) : 0.f;
		return bTagged || Remaining > 0;
	}
}

void UClassicHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(false);
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	if (!StatusCard || !LifeText || !CompactText || !HealthText || !ManaText || !GoldText || !LevelText || !ExperienceText || !ExperienceBar || !HealthBar || !ManaBar
		|| !AbilityBox || !ItemBox || !AttributesButton || !CloseAttributesButton || !AttributesPanel || !AttributesText
		|| !SlotWidgetClass || !ItemSlotWidgetClass || !GetOwningPlayer()
		|| SlotWidgetClass->HasAnyClassFlags(CLASS_Abstract) || ItemSlotWidgetClass->HasAnyClassFlags(CLASS_Abstract))
	{
		UE_LOG(LogMobaControl, Error, TEXT("ClassicHUDInvalid Class=%s Reason=MissingBindingOrEntryClassOrPlayer"), *GetClass()->GetPathName());
		return;
	}

	bConstructed = true;
	AttributesButton->OnClicked.AddUniqueDynamic(this, &ThisClass::ToggleAttributes);
	CloseAttributesButton->OnClicked.AddUniqueDynamic(this, &ThisClass::CloseAttributes);
	CloseAttributes();
	CheckBinding();
	RefreshStatus();
	// 只观察对象身份、连接代次和未齐状态；稳定状态不扫描属性或技能。
	GetWorld()->GetTimerManager().SetTimer(BindingTimer, this, &ThisClass::CheckBinding, .2f, true);
}

void UClassicHUDWidget::NativeDestruct()
{
	if (!bConstructed)
	{
		Super::NativeDestruct();
		return;
	}

	bConstructed = false;
	UnbindSources();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BindingTimer);
		World->GetTimerManager().ClearTimer(RefreshTimer);
		World->GetTimerManager().ClearTimer(CooldownTimer);
	}

	AttributesButton->OnClicked.RemoveDynamic(this, &ThisClass::ToggleAttributes);
	CloseAttributesButton->OnClicked.RemoveDynamic(this, &ThisClass::CloseAttributes);
	CloseAttributes();
	AbilityBox->ClearChildren();
	ItemBox->ClearChildren();
	AbilityWidgets.Reset();
	ItemWidgets.Reset();
	PresentedStatus = {};
	Super::NativeDestruct();
}

void UClassicHUDWidget::CheckBinding()
{
	if (!bConstructed)
	{
		return;
	}

	const auto* Controller = Cast<AHeroPlayerController>(GetOwningPlayer());
	auto* Hero = Controller ? Cast<AHeroCharacter>(Controller->GetPawn()) : nullptr;
	auto* Participant = Controller ? Controller->GetPlayerState<AParticipantState>() : nullptr;
	Hero = IsValid(Hero) && !Hero->IsActorBeingDestroyed() ? Hero : nullptr;
	Participant = IsValid(Participant) && !Participant->IsActorBeingDestroyed() ? Participant : nullptr;
	auto* Abilities = IsValid(Hero) ? Hero->GetHeroAbilitySystem() : nullptr;
	auto* AttributeComp = IsValid(Hero) ? Hero->GetAttributeComponent() : nullptr;
	const uint32 Epoch = IsValid(Hero) ? Hero->ConnectionEpoch : 0;
	const uint8 Profile = Controller ? static_cast<uint8>(Controller->GetActiveProfile()) : MAX_uint8;
	if (BoundHero.Get() != Hero || BoundParticipant.Get() != Participant || BoundAbilities.Get() != Abilities || BoundAttributes.Get() != AttributeComp
		|| BoundWorld.Get() != GetWorld() || BoundEpoch != Epoch)
	{
		UnbindSources();
		BoundHero = Hero;
		BoundParticipant = Participant;
		BoundAbilities = Abilities;
		BoundAttributes = AttributeComp;
		BoundWorld = GetWorld();
		BoundEpoch = Epoch;
		BoundProfile = Profile;
		CloseAttributes();
		if (Abilities)
		{
			const FGameplayAttribute Attributes[] = {UHeroAttributeSet::GetHealthAttribute(), UHeroAttributeSet::GetMaxHealthAttribute(),
				UHeroAttributeSet::GetManaAttribute(), UHeroAttributeSet::GetMaxManaAttribute(), UHeroAttributeSet::GetShieldAttribute(),
				UHeroAttributeSet::GetArmorAttribute(), UHeroAttributeSet::GetMagicResistanceAttribute(),
				UHeroAttributeSet::GetDamageTakenMultiplierAttribute(), UHeroAttributeSet::GetMoveSpeedAttribute(), UHeroAttributeSet::GetHeroLevelAttribute(),
				UHeroAttributeSet::GetHealthRegenerationAttribute(), UHeroAttributeSet::GetManaRegenerationAttribute(), UHeroAttributeSet::GetAttackDamageAttribute(),
				UHeroAttributeSet::GetAbilityPowerAttribute(), UHeroAttributeSet::GetCriticalChanceAttribute(), UHeroAttributeSet::GetCriticalDamageMultiplierAttribute(),
				UHeroAttributeSet::GetCooldownReductionAttribute()};
			for (const auto& Attribute : Attributes)
			{
				AttributeHandles.Emplace(Attribute, Abilities->GetGameplayAttributeValueChangeDelegate(Attribute).AddWeakLambda(this,
					[this](const FOnAttributeChangeData&) { QueueRefresh(); }));
			}

			Abilities->OnActiveGameplayEffectAddedDelegateToSelf.AddWeakLambda(this,
				[this](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle) { QueueRefresh(); });
			Abilities->OnAnyGameplayEffectRemovedDelegate().AddWeakLambda(this,
				[this](const FActiveGameplayEffect&) { QueueRefresh(); });
			Abilities->RegisterGenericGameplayTagEvent().AddWeakLambda(this,
				[this](FGameplayTag, int32) { QueueRefresh(); });
			Abilities->AbilitySpecDirtiedCallbacks.AddWeakLambda(this,
				[this](const FGameplayAbilitySpec&) { QueueRefresh(); });
			GMP::FMessageUtils::ListenObjectMessage(Abilities, MOBA_MESSAGE_ABILITY_LOADOUT_CHANGED, this,
				[this](uint32) { QueueRefresh(); });
		}

		if (AttributeComp)
		{
			GMP::FMessageUtils::ListenObjectMessage(AttributeComp, MOBA_MESSAGE_HERO_PROGRESSION_CHANGED, this,
				[this](int64) { QueueRefresh(); });
		}

		if (Hero)
		{
			GMP::FMessageUtils::ListenObjectMessage(Hero, MOBA_MESSAGE_HERO_LIFE_CHANGED, this,
				[this](const FHeroLifeSnapshot&) { QueueRefresh(); });
			GMP::FMessageUtils::ListenObjectMessage(Hero->GetInventory(), MOBA_MESSAGE_ITEM_INVENTORY_CHANGED, this,
				[this](uint32) { QueueRefresh(); });
		}

		if (Participant)
		{
			GMP::FMessageUtils::ListenObjectMessage(Participant, MOBA_MESSAGE_PARTICIPANT_ECONOMY_CHANGED, this,
				[this](uint32) { QueueRefresh(); });
		}

		RefreshStatus();
	}
	else if (BoundProfile != Profile || !PresentedStatus.bOwnedHero || !PresentedStatus.bCombatReady || !PresentedStatus.bAbilitiesReady || !PresentedStatus.bInventoryReady)
	{
		BoundProfile = Profile;
		// 跨 Actor/Spec 复制到达顺序没有共同事件；仅未齐期间重试初始完整快照。
		RefreshStatus();
	}
}

void UClassicHUDWidget::UnbindSources()
{
	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_ABILITY_LOADOUT_CHANGED, this);
	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_ITEM_INVENTORY_CHANGED, this);
	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_PARTICIPANT_ECONOMY_CHANGED, this);
	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_HERO_LIFE_CHANGED, this);
	GMP::FMessageUtils::UnbindMessage(MOBA_MESSAGE_HERO_PROGRESSION_CHANGED, this);
	if (auto* Abilities = BoundAbilities.Get())
	{
		for (const auto& Binding : AttributeHandles)
		{
			Abilities->GetGameplayAttributeValueChangeDelegate(Binding.Key).Remove(Binding.Value);
		}

		Abilities->OnActiveGameplayEffectAddedDelegateToSelf.RemoveAll(this);
		Abilities->OnAnyGameplayEffectRemovedDelegate().RemoveAll(this);
		Abilities->RegisterGenericGameplayTagEvent().RemoveAll(this);
		Abilities->AbilitySpecDirtiedCallbacks.RemoveAll(this);
	}

	AttributeHandles.Reset();
	BoundHero.Reset();
	BoundParticipant.Reset();
	BoundAbilities.Reset();
	BoundAttributes.Reset();
	BoundWorld.Reset();
	BoundEpoch = 0;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RefreshTimer);
		World->GetTimerManager().ClearTimer(CooldownTimer);
	}
}

void UClassicHUDWidget::QueueRefresh()
{
	if (bConstructed && GetWorld() && !GetWorld()->GetTimerManager().TimerExists(RefreshTimer))
	{
		// 合并同一事务中的通知，避免观察到来源交换的中间态或回调重入。
		RefreshTimer = GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ThisClass::RefreshStatus);
	}
}

void UClassicHUDWidget::RefreshStatus()
{
	if (!bConstructed)
	{
		return;
	}

	const auto* Experience = GetWorld()->GetSubsystem<UExperienceSubsystem>();
	const auto* Controller = Cast<AHeroPlayerController>(GetOwningPlayer());
	const auto* Hero = Controller ? Cast<AHeroCharacter>(Controller->GetPawn()) : nullptr;
	const auto* Participant = Controller ? Controller->GetPlayerState<AParticipantState>() : nullptr;
	Hero = IsValid(Hero) && !Hero->IsActorBeingDestroyed() ? Hero : nullptr;
	Participant = IsValid(Participant) && !Participant->IsActorBeingDestroyed() ? Participant : nullptr;
	if (BoundHero.Get() != Hero || BoundParticipant.Get() != Participant)
	{
		CheckBinding();
		return;
	}

	const auto* Rules = Experience && Experience->GetSnapshot().State == EExperienceState::ExperienceReady ? Experience->GetRules() : nullptr;
	PresentedStatus = MobaStatus::ReadOwnedStatus(Cast<AHeroPlayerController>(GetOwningPlayer()), Rules);
	const auto& State = PresentedStatus;
	const FText Empty = LOCTEXT("Empty", "—");
	LevelText->SetText(State.bProgressionReady ? FText::Format(LOCTEXT("HeroLevel", "英雄\nLv.{0}"), FText::AsNumber(State.HeroLevel)) : Empty);
	ExperienceText->SetText(!State.bProgressionReady ? LOCTEXT("ExperienceSync", "经验同步中")
		: (State.bMaximumLevel ? FText::Format(LOCTEXT("MaximumExperience", "满级 · 总经验 {0}"), FText::AsNumber(State.TotalExperiencePoints))
			: FText::Format(LOCTEXT("Experience", "经验 {0} / {1}"), FText::AsNumber(State.LevelExperiencePoints), FText::AsNumber(State.RequiredExperiencePoints))));
	ExperienceText->SetToolTipText(State.bProgressionReady ? FText::Format(LOCTEXT("TotalExperience", "累计经验 {0}"), FText::AsNumber(State.TotalExperiencePoints)) : Empty);
	ExperienceBar->SetPercent(State.bProgressionReady ? State.ExperienceProgress : 0);
	LifeText->SetText(!State.bOwnedHero ? LOCTEXT("Waiting", "等待英雄") : (!State.bCombatReady ? LOCTEXT("Sync", "同步中")
		: (State.LifeState == EHeroLifeState::Alive ? LOCTEXT("Alive", "存活") : LOCTEXT("Dead", "阵亡 · 等待复活"))));
	HealthText->SetText(State.bCombatReady ? FText::Format(LOCTEXT("Resource", "{0} / {1}"), Number(State.Health), Number(State.MaxHealth)) : Empty);
	ManaText->SetText(State.bCombatReady ? FText::Format(LOCTEXT("Mana", "Mana  {0} / {1}"), Number(State.Mana), Number(State.MaxMana)) : Empty);
	HealthBar->SetPercent(State.bCombatReady && State.MaxHealth > 0 ? FMath::Clamp(State.Health / State.MaxHealth, 0.f, 1.f) : 0.f);
	ManaBar->SetPercent(State.bCombatReady && State.MaxMana > 0 ? FMath::Clamp(State.Mana / State.MaxMana, 0.f, 1.f) : 0.f);
	ManaBar->SetVisibility(State.bCombatReady && !State.bUsesMana ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	ManaText->SetVisibility(State.bCombatReady && !State.bUsesMana ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	GoldText->SetText(State.bInventoryReady ? FText::Format(LOCTEXT("Gold", "金币  {0}"), FText::AsNumber(State.Gold)) : Empty);
	const auto* Attributes = State.bCombatReady && BoundHero.IsValid() ? BoundHero->GetAttributes() : nullptr;
	CompactText->SetText(Attributes ? FText::Format(LOCTEXT("Compact", "护甲  {0}\n魔抗  {1}\n护盾  {2}"),
		Number(Attributes->GetArmor()), Number(Attributes->GetMagicResistance()), Number(State.Shield)) : Empty);
	const FText ResourceLine = !State.bUsesMana ? LOCTEXT("ResourceNone", "资源  无")
		: FText::Format(LOCTEXT("ResourceMana", "Mana  {0} / {1} · 恢复 {2}/s"), Number(State.Mana), Number(State.MaxMana), Number(Attributes ? Attributes->GetManaRegeneration() : 0));
	AttributesText->SetText(Attributes ? FText::Format(LOCTEXT("CompleteAttributes",
		"生命  {0} / {1} · 恢复 {2}/s\n{3}\n攻击力  {4}\n法术强度  {5}\n护甲  {6}\n魔法抗性  {7}\n暴击率  {8}%\n暴击伤害  ×{9}\n冷却缩减  {10}%\n移动速度  {11}\n承伤倍率  ×{12}\n护盾  {13}"),
		Number(State.Health), Number(State.MaxHealth), Number(Attributes->GetHealthRegeneration()), ResourceLine,
		Number(Attributes->GetAttackDamage()), Number(Attributes->GetAbilityPower()), Number(Attributes->GetArmor()), Number(Attributes->GetMagicResistance()),
		Number(Attributes->GetCriticalChance() * 100), Number(Attributes->GetCriticalDamageMultiplier()), Number(Attributes->GetCooldownReduction() * 100),
		Number(Attributes->GetMoveSpeed()), Number(Attributes->GetDamageTakenMultiplier()), Number(State.Shield)) : Empty);

	const int32 Count = State.bAbilitiesReady ? State.Abilities.Num() : 0;
	while (AbilityWidgets.Num() > Count)
	{
		AbilityWidgets.Last()->RemoveFromParent();
		AbilityWidgets.Pop();
	}

	while (AbilityWidgets.Num() < Count)
	{
		auto* Entry = CreateWidget<UClassicHUDSlotWidget>(GetOwningPlayer(), SlotWidgetClass);
		if (!Entry)
		{
			return;
		}

		auto* EntrySlot = AbilityBox->AddChildToHorizontalBox(Entry);
		EntrySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		EntrySlot->SetPadding(FMargin(3, 0));
		AbilityWidgets.Add(Entry);
	}

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const auto& Row = State.Abilities[Index];
		AbilityWidgets[Index]->Present(Row.Name, FText::Format(LOCTEXT("AbilityLevel", "Lv.{0}{1}"), FText::AsNumber(Row.Level),
			Row.bEnabled ? FText::GetEmpty() : LOCTEXT("Disabled", " · 禁用")), ReadInputKey(Row.Slot), Row.bEnabled && State.LifeState == EHeroLifeState::Alive);
	}

	while (ItemWidgets.Num() < State.Items.Num())
	{
		auto* Entry = CreateWidget<UClassicHUDSlotWidget>(GetOwningPlayer(), ItemSlotWidgetClass);
		if (!Entry)
		{
			return;
		}

		const int32 Index = ItemWidgets.Num();
		auto* EntrySlot = ItemBox->AddChildToUniformGrid(Entry, Index / 3, Index % 3);
		EntrySlot->SetHorizontalAlignment(HAlign_Fill);
		EntrySlot->SetVerticalAlignment(VAlign_Fill);
		ItemWidgets.Add(Entry);
	}

	for (int32 Index = 0; Index < ItemWidgets.Num(); ++Index)
	{
		const auto& Item = State.Items[Index];
		FGameplayAbilitySpecHandle Active;
		if (State.bInventoryReady && BoundHero.IsValid())
		{
			const auto* Instance = BoundHero->GetInventory()->GetSnapshot().Items.FindByPredicate([&Item](const FItemInstance& Value) { return Value.Id == Item.Instance; });
			Active = Instance ? Instance->ActiveAbility : FGameplayAbilitySpecHandle();
		}

		const bool bUsable = (State.bInventoryReady && State.bItemsEnabled && Active.IsValid() && BoundAbilities.IsValid()
			&& BoundAbilities->IsManagedAbilityEnabled(Active) && State.LifeState == EHeroLifeState::Alive);
		ItemWidgets[Index]->Present(!State.bInventoryReady ? Empty : (!State.bItemsEnabled ? LOCTEXT("ItemsOff", "未启用")
			: (Item.Instance.IsValid() ? FText::FromName(Item.ItemId) : LOCTEXT("EmptySlot", "空"))),
			Item.Instance.IsValid() ? FText::Format(LOCTEXT("Quantity", "×{0}"), FText::AsNumber(Item.Quantity)) : FText::GetEmpty(),
			FText::Format(LOCTEXT("SlotNumber", "槽{0}"), FText::AsNumber(Index + 1)), State.bInventoryReady, bUsable ? Item.Instance : FGuid());
	}

	RefreshCooldowns();
}

void UClassicHUDWidget::RefreshCooldowns()
{
	if (!bConstructed)
	{
		return;
	}

	bool bTimed = false;
	for (int32 Index = 0; Index < AbilityWidgets.Num(); ++Index)
	{
		bool bKnown;
		float Remaining, Duration;
		bTimed |= ReadCooldown(BoundAbilities.Get(), PresentedStatus.Abilities[Index].Handle, bKnown, Remaining, Duration);
		AbilityWidgets[Index]->PresentCooldown(bKnown, Remaining, Duration);
	}

	for (int32 Index = 0; Index < ItemWidgets.Num(); ++Index)
	{
		bool bKnown = true;
		float Remaining = 0, Duration = 0;
		if (PresentedStatus.bInventoryReady && BoundHero.IsValid())
		{
			const FGuid Id = PresentedStatus.Items[Index].Instance;
			const auto* Item = BoundHero->GetInventory()->GetSnapshot().Items.FindByPredicate([Id](const FItemInstance& Value) { return Value.Id == Id; });
			if (Item && Item->ActiveAbility.IsValid())
			{
				bTimed |= ReadCooldown(BoundAbilities.Get(), Item->ActiveAbility, bKnown, Remaining, Duration);
			}
		}

		ItemWidgets[Index]->PresentCooldown(bKnown, Remaining, Duration);
	}

	auto& Timers = GetWorld()->GetTimerManager();
	if (bTimed && !Timers.TimerExists(CooldownTimer))
	{
		Timers.SetTimer(CooldownTimer, this, &ThisClass::RefreshCooldowns, .1f, true);
	}
	else if (!bTimed)
	{
		Timers.ClearTimer(CooldownTimer);
	}
}

FText UClassicHUDWidget::ReadInputKey(FGameplayTag AbilitySlot) const
{
	const auto* Experience = GetWorld()->GetSubsystem<UExperienceSubsystem>();
	const auto* Definition = Experience ? Experience->GetExperience() : nullptr;
	const auto* Controller = Cast<AHeroPlayerController>(GetOwningPlayer());
	if (Definition && Controller)
	{
		for (const auto& ProfileRef : Definition->InputProfiles)
		{
			const auto* Profile = ProfileRef.Get();
			if (!Profile || Profile->Profile != Controller->GetActiveProfile())
			{
				continue;
			}

			const UInputAction* Action = AbilitySlot == MobaGameplayTags::Input_Slot_Primary ? Profile->PrimaryAbility.Get()
				: (AbilitySlot == MobaGameplayTags::Input_Slot_UnitAttack ? Profile->UnitAttack.Get()
					: (AbilitySlot == MobaGameplayTags::Input_Slot_Target ? Profile->TargetAbility.Get() : nullptr));
			for (const auto* Mapping : {Profile->Mapping.Get(), Profile->TargetMapping.Get()})
			{
				if (Action && Mapping)
				{
					for (const auto& KeyMapping : Mapping->GetMappings())
					{
						if (KeyMapping.Action == Action)
						{
							return KeyMapping.Key.GetDisplayName();
						}
					}
				}
			}
		}
	}

	// 没有键位映射的真实槽保留显示，不把槽名当成快捷键。
	return LOCTEXT("Unmapped", "—");
}

void UClassicHUDWidget::ToggleAttributes()
{
	AttributesPanel->SetVisibility(AttributesPanel->GetVisibility() == ESlateVisibility::Collapsed ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
}

void UClassicHUDWidget::CloseAttributes()
{
	AttributesPanel->SetVisibility(ESlateVisibility::Collapsed);
}

#undef LOCTEXT_NAMESPACE
