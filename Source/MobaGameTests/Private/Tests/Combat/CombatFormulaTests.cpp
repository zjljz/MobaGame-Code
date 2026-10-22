#include "Combat/CombatRules.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "MobaGameplayTags.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatFormulaValuesTest, "Moba.Combat.Formula.Values", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCombatFormulaValuesTest::RunTest(const FString& Parameters)
{
	struct FCase
	{

	public:
		const TCHAR* Name;

		ECombatDamageType Type;

		float Armor;

		float MagicResistance;

		float Taken;

		float Expected;
	};
	const FCase Cases[] = {{TEXT("Physical positive armor"), ECombatDamageType::Physical, 100, 0, 1, 50}, {TEXT("Physical negative armor"), ECombatDamageType::Physical, -100, 100, 1, 150},
		{TEXT("Magical positive resistance"), ECombatDamageType::Magical, -100, 100, 1, 50}, {TEXT("Magical negative resistance"), ECombatDamageType::Magical, 100, -100, 1, 150},
		{TEXT("True ignores positive resistance"), ECombatDamageType::TrueDamage, 100, 100, 1, 100},
		{TEXT("True ignores negative resistance"), ECombatDamageType::TrueDamage, -100, -100, 1, 100}, {TEXT("Zero resistance"), ECombatDamageType::Physical, 0, 0, 1, 100},
		{TEXT("Physical reduction"), ECombatDamageType::Physical, 100, 0, 0.9f, 45}, {TEXT("Magical reduction"), ECombatDamageType::Magical, 0, 100, 0.9f, 45},
		{TEXT("True reduction"), ECombatDamageType::TrueDamage, 100, 100, 0.9f, 90}, {TEXT("Zero taken multiplier"), ECombatDamageType::TrueDamage, 0, 0, 0, 0},
		{TEXT("Damage amplification"), ECombatDamageType::Physical, -100, 0, 2, 300}};
	for (const FCase& Case : Cases)
	{
		FCombatDamageParameters Input;
		Input.BaseDamage = 100;
		Input.Armor = Case.Armor;
		Input.MagicResistance = Case.MagicResistance;
		Input.DamageTakenMultiplier = Case.Taken;
		float Damage = -1;
		bool bCritical = true;
		TestTrue(Case.Name, MobaCombat::CalculateDamage(Case.Type, Input, Damage, bCritical));
		TestEqual(Case.Name, Damage, Case.Expected);
		TestFalse(TEXT("Ordinary damage is not critical"), bCritical);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatFormulaCriticalTest, "Moba.Combat.Formula.CriticalSnapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCombatFormulaCriticalTest::RunTest(const FString& Parameters)
{
	struct FCase
	{

	public:
		bool bAllowed;

		float Chance;

		float Sample;

		bool bExpectedCritical;
	};
	const FCase Cases[] = {{false, 0.25f, 0, false}, {true, 0.25f, 0, true}, {true, 0.25f, 0.249f, true}, {true, 0.25f, 0.25f, false}, {true, 0.25f, 1, false}, {true, 0, 0, false},
		{true, 1, 1, true}, {false, 1, 0, false}};
	for (const FCase& Case : Cases)
	{
		FCombatDamageParameters Input;
		Input.BaseDamage = 100;
		Input.Armor = 100;
		Input.DamageTakenMultiplier = 0.9f;
		Input.bCanCritical = Case.bAllowed;
		Input.CriticalChance = Case.Chance;
		Input.CriticalSample = Case.Sample;
		float Damage = 0;
		bool bCritical = false;
		TestTrue(TEXT("Deterministic sample accepted"), MobaCombat::CalculateDamage(ECombatDamageType::Physical, Input, Damage, bCritical));
		TestEqual(TEXT("Critical branch"), bCritical, Case.bExpectedCritical);
		TestEqual(TEXT("Critical precedes resistance and taken multiplier"), Damage, Case.bExpectedCritical ? 67.5f : 45.0f);
	}

	FCombatDamageParameters Release;
	Release.BaseDamage = 100;
	Release.bCanCritical = true;
	Release.CriticalSample = 0.1f;
	float SnapshotDamage = 0;
	bool bCritical = false;
	TestTrue(TEXT("Release snapshot"), MobaCombat::CalculateDamage(ECombatDamageType::Physical, Release, SnapshotDamage, bCritical));
	TestEqual(TEXT("Release stores pre-resistance critical damage"), SnapshotDamage, 150.0f);
	FCombatDamageParameters Hit;
	Hit.BaseDamage = SnapshotDamage;
	Hit.Armor = 100;
	Hit.DamageTakenMultiplier = 0.9f;
	float Damage = 0;
	TestTrue(TEXT("Hit uses current resistance and no repeated critical"), MobaCombat::CalculateDamage(ECombatDamageType::Physical, Hit, Damage, bCritical));
	TestEqual(TEXT("Two-stage result equals direct calculation"), Damage, 67.5f);
	TestFalse(TEXT("No second critical roll"), bCritical);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatFormulaInvalidTest, "Moba.Combat.Formula.InvalidParameters", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCombatFormulaInvalidTest::RunTest(const FString& Parameters)
{
	const float NonFinite[] = {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
	float FCombatDamageParameters::* Fields[] = {&FCombatDamageParameters::BaseDamage, &FCombatDamageParameters::Armor, &FCombatDamageParameters::MagicResistance,
		&FCombatDamageParameters::DamageTakenMultiplier, &FCombatDamageParameters::CriticalChance, &FCombatDamageParameters::CriticalMultiplier, &FCombatDamageParameters::CriticalSample};
	const auto Reject = [this](const FCombatDamageParameters& Input, ECombatDamageType Type = ECombatDamageType::TrueDamage)
	{
		float Damage = 123;
		bool bCritical = true;
		TestFalse(TEXT("Illegal parameter rejected"), MobaCombat::CalculateDamage(Type, Input, Damage, bCritical));
		TestEqual(TEXT("Rejected output cleared"), Damage, 0.0f);
		TestFalse(TEXT("Rejected critical cleared"), bCritical);
	};
	for (auto Field : Fields)
	{
		for (float Value : NonFinite)
		{
			FCombatDamageParameters Input;
			Input.BaseDamage = 100;
			Input.*Field = Value;
			Reject(Input);
		}
	}

	struct FInvalidCase
	{

	public:
		float FCombatDamageParameters::* Field;

		float Value;
	};
	const FInvalidCase InvalidCases[] = {{&FCombatDamageParameters::BaseDamage, 0}, {&FCombatDamageParameters::BaseDamage, -1}, {&FCombatDamageParameters::DamageTakenMultiplier, -1},
		{&FCombatDamageParameters::CriticalChance, -0.1f}, {&FCombatDamageParameters::CriticalChance, 1.1f}, {&FCombatDamageParameters::CriticalMultiplier, 0.5f},
		{&FCombatDamageParameters::CriticalSample, -0.1f}, {&FCombatDamageParameters::CriticalSample, 1.1f}};
	for (const FInvalidCase& Case : InvalidCases)
	{
		FCombatDamageParameters Input;
		Input.BaseDamage = 100;
		Input.*Case.Field = Case.Value;
		Reject(Input);
	}

	FCombatDamageParameters Input;
	Input.BaseDamage = 100;
	Reject(Input, static_cast<ECombatDamageType>(255));
	Input.BaseDamage = MAX_flt;
	Input.DamageTakenMultiplier = 2;
	Reject(Input);
	Input.BaseDamage = 100;
	Input.DamageTakenMultiplier = 1;
	Input.Armor = MAX_flt;
	float Damage = 0;
	bool bCritical = false;
	TestTrue(TEXT("Finite maximal resistance uses stable denominator"), MobaCombat::CalculateDamage(ECombatDamageType::Physical, Input, Damage, bCritical));
	TestTrue(TEXT("Finite resistance yields finite positive damage"), FMath::IsFinite(Damage) && Damage > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatFormulaClassificationTest, "Moba.Combat.Formula.ClassificationAndLegacy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCombatFormulaClassificationTest::RunTest(const FString& Parameters)
{
	FCombatRules Rules;
	TestTrue(TEXT("Old assets default to fixed policy"), Rules.DamagePolicy == ECombatDamagePolicy::LegacyFixed);
	Rules.FixedDamage.Add(MobaGameplayTags::Damage_True, 60);
	TestTrue(TEXT("Legacy configuration remains valid"), Rules.IsValid());
	float Damage = 0;
	TestTrue(TEXT("Legacy true damage remains exact"), Rules.ResolveDamageTags(FGameplayTagContainer(MobaGameplayTags::Damage_True), Damage));
	TestEqual(TEXT("Legacy fixed value"), Damage, 60.0f);
	TestFalse(TEXT("Legacy does not gain implicit physical damage"), Rules.ResolveDamage(MobaGameplayTags::Damage_Physical, Damage));
	Rules.DamagePolicy = ECombatDamagePolicy::Standard;
	Rules.FixedDamage.Empty();
	TestTrue(TEXT("Standard policy does not require legacy values"), Rules.IsValid());
	ECombatDamageType Type = ECombatDamageType::TrueDamage;
	struct FTagCase
	{

	public:
		FGameplayTag Root;

		FGameplayTag Child;

		ECombatDamageType Expected;
	};
	const FTagCase Cases[] = {{MobaGameplayTags::Damage_Physical, MobaGameplayTags::Damage_Physical_Test, ECombatDamageType::Physical},
		{MobaGameplayTags::Damage_Magical, MobaGameplayTags::Damage_Magical_Test, ECombatDamageType::Magical},
		{MobaGameplayTags::Damage_True, MobaGameplayTags::Damage_True_Test, ECombatDamageType::TrueDamage}};
	for (const FTagCase& Case : Cases)
	{
		TestTrue(TEXT("Known root resolves"), Rules.ResolveDamageType(FGameplayTagContainer(Case.Root), Type));
		TestTrue(TEXT("Known root classification"), Type == Case.Expected);
		TestFalse(TEXT("Unregistered child rejected"), Rules.ResolveDamageType(FGameplayTagContainer(Case.Child), Type));
		Rules.DamageTypeMappings.Add(Case.Child, Case.Expected);
		TestTrue(TEXT("Explicit child mapping is valid"), Rules.IsValid());
		TestTrue(TEXT("Registered child resolves"), Rules.ResolveDamageType(FGameplayTagContainer(Case.Child), Type));
		TestTrue(TEXT("Registered child classification"), Type == Case.Expected);
	}

	TestFalse(TEXT("Empty tags rejected"), Rules.ResolveDamageType(FGameplayTagContainer(), Type));
	TestFalse(TEXT("Damage root rejected"), Rules.ResolveDamageType(FGameplayTagContainer(MobaGameplayTags::Damage), Type));
	TestFalse(TEXT("Unknown damage rejected"), Rules.ResolveDamageType(FGameplayTagContainer(MobaGameplayTags::Damage_Unknown), Type));
	FGameplayTagContainer Mixed(MobaGameplayTags::Damage_Physical);
	Mixed.AddTag(MobaGameplayTags::Damage_Magical);
	TestFalse(TEXT("Mixed damage roots rejected"), Rules.ResolveDamageType(Mixed, Type));
	Mixed.Reset();
	Mixed.AddTag(MobaGameplayTags::Damage_Physical);
	Mixed.AddTag(MobaGameplayTags::Damage_Physical_Test);
	TestFalse(TEXT("Multiple damage tags are ambiguous"), Rules.ResolveDamageType(Mixed, Type));
	Rules.DamageTypeMappings[MobaGameplayTags::Damage_Physical_Test] = ECombatDamageType::TrueDamage;
	TestFalse(TEXT("Child cannot change its root classification"), Rules.IsValid());
	TestFalse(TEXT("Invalid child mapping cannot execute"), Rules.ResolveDamageType(FGameplayTagContainer(MobaGameplayTags::Damage_Physical_Test), Type));
	Rules.DamageTypeMappings.Empty();
	Rules.DamageTypeMappings.Add(MobaGameplayTags::Damage_Physical, ECombatDamageType::Physical);
	TestFalse(TEXT("Mappings only configure descendants"), Rules.IsValid());
	Rules.DamageTypeMappings.Empty();
	Rules.DamageTypeMappings.Add(MobaGameplayTags::Damage_Unknown, ECombatDamageType::TrueDamage);
	TestFalse(TEXT("Unknown root cannot masquerade as true"), Rules.IsValid());
	Rules.DamagePolicy = static_cast<ECombatDamagePolicy>(255);
	TestFalse(TEXT("Unknown policy rejected"), Rules.IsValid());
	return true;
}

#endif
