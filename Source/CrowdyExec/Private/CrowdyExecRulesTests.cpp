#include "CrowdyExecTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecInternal.h"
#include "CrowdyServerObjectDefinition.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include <limits>

namespace CrowdyExecRulesTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** A name as the bake prints it, which a build without case-preserving names may spell differently. */
	FString Named(const TCHAR* Name)
	{
		return FName(Name).ToString();
	}

	/** A definition that bakes: FCrowdyExecTestBossState, and Hit sending FCrowdyExecTestHit. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> NewDefinition(const TCHAR* TypeName = TEXT("test_rules"))
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TypeName;
		Definition->State = FCrowdyExecTestBossState::StaticStruct();
		FCrowdyServerFunction& Hit = Definition->Functions.AddDefaulted_GetRef();
		Hit.Name = TEXT("Hit");
		Hit.Params = FCrowdyExecTestHit::StaticStruct();
		return Definition;
	}

	FCrowdyServerTimer& AddTimer(UCrowdyServerObjectDefinition& Definition, FName Name, float Seconds = 60.f)
	{
		FCrowdyServerTimer& Timer = Definition.Timers.AddDefaulted_GetRef();
		Timer.Name = Name;
		Timer.Seconds = Seconds;
		return Timer;
	}

	TArray<FString> BakeErrors(UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Errors;
		Definition.Bake(Errors);
		return Errors;
	}

	/** Compared case-sensitively, since FString's == ignores case. */
	void TestRefuses(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const FString& Expected)
	{
		const TArray<FString> Errors = BakeErrors(Definition);
		const bool bFound = Errors.ContainsByPredicate([&Expected](const FString& Error) { return Error.Equals(Expected, ESearchCase::CaseSensitive); });
		Test.TestTrue(FString::Printf(TEXT("refused with \"%s\" (got: %s)"), *Expected, *FString::Join(Errors, TEXT("; "))), bFound);
	}

	void TestBakes(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const TCHAR* What)
	{
		const TArray<FString> Errors = BakeErrors(Definition);
		Test.TestTrue(FString::Printf(TEXT("%s bakes (%s)"), What, *FString::Join(Errors, TEXT("; "))), Errors.IsEmpty());
	}

	bool HasBakedStruct(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct* Struct)
	{
		return Definition.BakedStructs.ContainsByPredicate([Struct](const FCrowdyExecBakedStruct& Baked) { return Baked.Struct.Get() == Struct; });
	}

#if WITH_EDITOR
	/** Adds Deposit, whose List input Amount has the Value Range Min to Max; an empty bound is left unset. */
	void AddDeposit(UCrowdyServerObjectDefinition& Definition, EPropertyBagPropertyType Type, const TCHAR* Min, const TCHAR* Max)
	{
		FCrowdyServerFunction& Deposit = Definition.Functions.AddDefaulted_GetRef();
		Deposit.Name = TEXT("Deposit");
		Deposit.ParamsForm = ECrowdyServerValuesForm::List;
		FPropertyBagPropertyDesc Amount(TEXT("Amount"), Type);
		if (*Min)
		{
			Amount.SetMetaData(TEXT("ClampMin"), Min);
		}
		if (*Max)
		{
			Amount.SetMetaData(TEXT("ClampMax"), Max);
		}
		Deposit.ParamsList.AddProperties({Amount});
	}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRulesFindFunctionTest, "CrowdySDK.CrowdyExec.Rules.FindFunction", CrowdyExecRulesTests::TestFlags)
bool FCrowdyExecRulesFindFunctionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRulesTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewDefinition();
	TestTrue(TEXT("an own function is found"), Definition->FindFunction(TEXT("Hit")) == &Definition->Functions[0]);
	TestNull(TEXT("a built-in function is not offered with Members From None"), Definition->FindFunction(TEXT("Join")));
	Definition->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	TestNull(TEXT("nor with Members From Crowdy Team"), Definition->FindFunction(TEXT("Join")));

	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	const TConstArrayView<FCrowdyServerFunction> BuiltIns = UCrowdyServerObjectDefinition::GetMemberFunctions();
	TestTrue(TEXT("a built-in function is offered with Members From This Object"), Definition->FindFunction(TEXT("Join")) == &BuiltIns[0]);
	TestTrue(TEXT("and found by its Unreal name"), Definition->FindFunction(TEXT("SetOpenForJoining")) == &BuiltIns[5]);
	TestNull(TEXT("but not by its method"), Definition->FindFunction(TEXT("set_open_for_joining")));
	TestTrue(TEXT("an own function is still found"), Definition->FindFunction(TEXT("Hit")) == &Definition->Functions[0]);
	TestNull(TEXT("an unknown name finds nothing"), Definition->FindFunction(TEXT("Nope")));

	Definition->Functions.AddDefaulted_GetRef().Name = TEXT("Leave");
	TestTrue(TEXT("an own function comes before a built-in function of its name"), Definition->FindFunction(TEXT("Leave")) == &Definition->Functions[1]);

	TestTrue(TEXT("Make Leader is a built-in function"), UCrowdyServerObjectDefinition::IsMemberFunction(TEXT("MakeLeader")));
	TestFalse(TEXT("Hit is not"), UCrowdyServerObjectDefinition::IsMemberFunction(TEXT("Hit")));
	TestFalse(TEXT("nor is a method name"), UCrowdyServerObjectDefinition::IsMemberFunction(TEXT("make_leader")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRulesMemberFunctionsTest, "CrowdySDK.CrowdyExec.Rules.MemberFunctions", CrowdyExecRulesTests::TestFlags)
bool FCrowdyExecRulesMemberFunctionsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRulesTests;
	struct FExpected
	{
		const TCHAR* Name;
		const TCHAR* Method;
		const UScriptStruct* Inputs;
		ECrowdyServerFunctionCaller Caller;
	};
	const UScriptStruct* Member = FCrowdyServerMemberInputs::StaticStruct();
	const FExpected Expected[] = {
		{TEXT("Join"), TEXT("join"), nullptr, ECrowdyServerFunctionCaller::Players},
		{TEXT("Leave"), TEXT("leave"), nullptr, ECrowdyServerFunctionCaller::Players},
		{TEXT("AddMember"), TEXT("add_member"), Member, ECrowdyServerFunctionCaller::ServerOnly},
		{TEXT("RemoveMember"), TEXT("remove_member"), Member, ECrowdyServerFunctionCaller::Leader},
		{TEXT("MakeLeader"), TEXT("make_leader"), Member, ECrowdyServerFunctionCaller::Leader},
		{TEXT("SetOpenForJoining"), TEXT("set_open_for_joining"), FCrowdyServerOpenInputs::StaticStruct(), ECrowdyServerFunctionCaller::Leader}};
	const TConstArrayView<FCrowdyServerFunction> BuiltIns = UCrowdyServerObjectDefinition::GetMemberFunctions();
	if (!TestEqual(TEXT("there are six built-in functions"), BuiltIns.Num(), static_cast<int32>(UE_ARRAY_COUNT(Expected))))
	{
		return false;
	}
	for (int32 Index = 0; Index < BuiltIns.Num(); ++Index)
	{
		const FCrowdyServerFunction& BuiltIn = BuiltIns[Index];
		const FExpected& Want = Expected[Index];
		TestTrue(FString::Printf(TEXT("%s is named"), Want.Name), BuiltIn.Name == FName(Want.Name));
		TestEqualSensitive(FString::Printf(TEXT("%s's method"), Want.Name), *BuiltIn.GetMethodName(), Want.Method);
		TestTrue(FString::Printf(TEXT("%s's inputs"), Want.Name), BuiltIn.GetParamsStruct() == Want.Inputs);
		TestNull(FString::Printf(TEXT("%s replies nothing"), Want.Name), BuiltIn.GetReplyStruct());
		TestTrue(FString::Printf(TEXT("%s's Callable By"), Want.Name), BuiltIn.WhoCanCall == Want.Caller);
	}

	for (const FCrowdyServerFunction& BuiltIn : BuiltIns)
	{
		const FString Method = BuiltIn.GetMethodName();
		TestTrue(FString::Printf(TEXT("%s is reserved while an object keeps its members"), *Method), CrowdyExec::IsReservedMethodName(Method, true));
		TestFalse(FString::Printf(TEXT("%s is free otherwise"), *Method), CrowdyExec::IsReservedMethodName(Method, false));
	}
	TestEqual(TEXT("the reserved built-in methods are the built-in functions'"), static_cast<int32>(UE_ARRAY_COUNT(CrowdyExec::BuiltInMemberMethodNames)), BuiltIns.Num());

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Own = NewDefinition();
	Own->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Own->MaxMembers = 8;
	TestBakes(*this, *Own, TEXT("a This Object definition"));
	TestTrue(TEXT("and bakes the member input struct"), HasBakedStruct(*Own, Member));
	TestTrue(TEXT("and the open input struct"), HasBakedStruct(*Own, FCrowdyServerOpenInputs::StaticStruct()));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Team = NewDefinition();
	Team->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	TestBakes(*this, *Team, TEXT("a Crowdy Team definition"));
	TestFalse(TEXT("without the built-in input structs"), HasBakedStruct(*Team, Member) || HasBakedStruct(*Team, FCrowdyServerOpenInputs::StaticStruct()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRulesMembersTest, "CrowdySDK.CrowdyExec.Rules.MembersRefusals", CrowdyExecRulesTests::TestFlags)
bool FCrowdyExecRulesMembersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRulesTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Readable = NewDefinition();
	Readable->Visibility = ECrowdyServerObjectVisibility::Members;
	TestRefuses(*this, *Readable, TEXT("Readable By Members needs Members From This Object or Crowdy Team"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Callers = NewDefinition();
	Callers->Functions[0].WhoCanCall = ECrowdyServerFunctionCaller::Leader;
	FCrowdyServerFunction& Deposit = Callers->Functions.AddDefaulted_GetRef();
	Deposit.Name = TEXT("Deposit");
	Deposit.WhoCanCall = ECrowdyServerFunctionCaller::Members;
	TestRefuses(*this, *Callers, FString::Printf(TEXT("Function %s: Callable By Leader needs Members From This Object or Crowdy Team"), *Named(TEXT("Hit"))));
	TestRefuses(*this, *Callers, FString::Printf(TEXT("Function %s: Callable By Members needs Members From This Object or Crowdy Team"), *Named(TEXT("Deposit"))));
	Callers->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	Callers->Visibility = ECrowdyServerObjectVisibility::Members;
	TestBakes(*this, *Callers, TEXT("Members and Leader callers and Readable By Members with Members From Crowdy Team"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> OwnedTeam = NewDefinition();
	OwnedTeam->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	OwnedTeam->Visibility = ECrowdyServerObjectVisibility::OwnerOnly;
	TestRefuses(*this, *OwnedTeam, TEXT("Readable By Owner Only cannot be used with Members From Crowdy Team, whose Instance Id is the team id"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Negative = NewDefinition();
	Negative->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Negative->MaxMembers = -1;
	TestRefuses(*this, *Negative, TEXT("Max Members is -1; it must be 0 (no limit) or more"));

	const TCHAR* const ShowRefusal = TEXT("Show Members to Players needs Max Members from 1 to 4096; turn it off for larger groups");
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Shown = NewDefinition();
	Shown->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	TestBakes(*this, *Shown, TEXT("the default Max Members with the list shown"));
	Shown->MaxMembers = 0;
	TestRefuses(*this, *Shown, ShowRefusal);
	Shown->MaxMembers = 4097;
	TestRefuses(*this, *Shown, ShowRefusal);
	Shown->MaxMembers = 4096;
	TestBakes(*this, *Shown, TEXT("a shown members list of at most 4,096"));
	Shown->MaxMembers = 0;
	Shown->bShowMembers = false;
	TestBakes(*this, *Shown, TEXT("no member limit with the list hidden"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Shadow = NewDefinition();
	Shadow->Functions.AddDefaulted_GetRef().Name = TEXT("Join");
	TestBakes(*this, *Shadow, TEXT("a function named Join with Members From None"));
	FCrowdyServerFunction& Enter = Shadow->Functions.AddDefaulted_GetRef();
	Enter.Name = TEXT("Enter");
	Enter.ServerName = TEXT("leave");
	Shadow->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	TestRefuses(*this, *Shadow, FString::Printf(TEXT("Function %s: the name is taken by a built-in function while Members From is This Object; rename it"), *Named(TEXT("Join"))));
	TestRefuses(*this, *Shadow, FString::Printf(TEXT("Function %s: the name is taken by a built-in function while Members From is This Object; rename it"), *Named(TEXT("Enter"))));
	Shadow->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	TestBakes(*this, *Shadow, TEXT("the same functions with Members From Crowdy Team"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRulesOnlyOneInstanceTest, "CrowdySDK.CrowdyExec.Rules.OnlyOneInstanceRefusals", CrowdyExecRulesTests::TestFlags)
bool FCrowdyExecRulesOnlyOneInstanceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRulesTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Owned = NewDefinition();
	Owned->bOnlyOneInstance = true;
	TestBakes(*this, *Owned, TEXT("Only One Instance readable by every player"));
	Owned->Visibility = ECrowdyServerObjectVisibility::OwnerOnly;
	TestRefuses(*this, *Owned, TEXT("Only One Instance cannot be used with Readable By Owner Only, whose Instance Id is each player's user id"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Team = NewDefinition();
	Team->bOnlyOneInstance = true;
	Team->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	TestRefuses(*this, *Team, TEXT("Only One Instance cannot be used with Members From Crowdy Team, whose Instance Id is the team id"));
	Team->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Team->Visibility = ECrowdyServerObjectVisibility::Members;
	TestBakes(*this, *Team, TEXT("Only One Instance keeping its own members, readable by them"));
	TestEqualSensitive(TEXT("the only instance is called main"), FString(UCrowdyServerObjectDefinition::OnlyInstanceId), FString(TEXT("main")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRulesTimersTest, "CrowdySDK.CrowdyExec.Rules.TimerRefusals", CrowdyExecRulesTests::TestFlags)
bool FCrowdyExecRulesTimersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRulesTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Unnamed = NewDefinition();
	AddTimer(*Unnamed, NAME_None);
	TestRefuses(*this, *Unnamed, TEXT("Timers: a timer has no Name"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Unwritable = NewDefinition();
	AddTimer(*Unwritable, TEXT("9Lives"));
	AddTimer(*Unwritable, TEXT("self"));
	TestRefuses(*this, *Unwritable, FString::Printf(TEXT("Timers: %s: '9_lives' cannot be its name in the server code (Rust); rename the timer"), *Named(TEXT("9Lives"))));
	TestRefuses(*this, *Unwritable, FString::Printf(TEXT("Timers: %s: 'self' cannot be its name in the server code (Rust); rename the timer"), *Named(TEXT("self"))));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Twins = NewDefinition();
	AddTimer(*Twins, TEXT("RulesEndMatch"));
	AddTimer(*Twins, TEXT("rules_end_match"));
	TestRefuses(*this, *Twins, TEXT("Timers: two timers are named 'rules_end_match'"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Clash = NewDefinition();
	AddTimer(*Clash, TEXT("Hit"));
	TestRefuses(*this, *Clash, FString::Printf(TEXT("Timers: %s: a function is also named 'hit'"), *Named(TEXT("Hit"))));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> BuiltIn = NewDefinition();
	AddTimer(*BuiltIn, TEXT("Join"));
	TestBakes(*this, *BuiltIn, TEXT("a timer named Join with Members From None"));
	BuiltIn->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	TestRefuses(*this, *BuiltIn, FString::Printf(TEXT("Timers: %s: a function is also named 'join'"), *Named(TEXT("Join"))));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Event = NewDefinition();
	AddTimer(*Event, TEXT("OnPlayerLeft"));
	TestRefuses(*this, *Event, FString::Printf(TEXT("Timers: %s: 'on_player_left' is kept for the On Player Joined and On Player Left events"), *Named(TEXT("OnPlayerLeft"))));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Fast = NewDefinition();
	AddTimer(*Fast, TEXT("RulesTick"), 0.001f);
	TestRefuses(*this, *Fast, FString::Printf(TEXT("Timers: %s: Seconds must be at least 0.01"), *Named(TEXT("RulesTick"))));
	Fast->Timers[0].Seconds = 0.01f;
	TestBakes(*this, *Fast, TEXT("a timer of 0.01 seconds"));

	const FString TooLong = FString::Printf(TEXT("Timers: %s: Seconds must be at most 2592000"), *Named(TEXT("RulesTick")));
	Fast->Timers[0].Seconds = 2592001.f;
	TestRefuses(*this, *Fast, TooLong);
	Fast->Timers[0].Seconds = std::numeric_limits<float>::infinity();
	TestRefuses(*this, *Fast, TooLong);
	Fast->Timers[0].Seconds = 2592000.f;
	TestBakes(*this, *Fast, TEXT("a timer of 30 days"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Reserved = NewDefinition();
	AddTimer(*Reserved, TEXT("OnTimer"));
	AddTimer(*Reserved, TEXT("Read"));
	TestRefuses(*this, *Reserved, FString::Printf(TEXT("Timers: %s: 'on_timer' is reserved in the server code; rename the timer"), *Named(TEXT("OnTimer"))));
	TestRefuses(*this, *Reserved, FString::Printf(TEXT("Timers: %s: 'read' is reserved in the server code; rename the timer"), *Named(TEXT("Read"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRulesFunctionLimitsTest, "CrowdySDK.CrowdyExec.Rules.FunctionNameAndCooldownRefusals", CrowdyExecRulesTests::TestFlags)
bool FCrowdyExecRulesFunctionLimitsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRulesTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Reserved = NewDefinition();
	FCrowdyServerFunction& Session = Reserved->Functions.AddDefaulted_GetRef();
	Session.Name = TEXT("RulesSession");
	Session.ServerName = TEXT("on_session");
	Reserved->Functions.AddDefaulted_GetRef().Name = TEXT("Read");
	TestRefuses(*this, *Reserved, FString::Printf(TEXT("Function %s: 'on_session' is reserved"), *Named(TEXT("RulesSession"))));
	TestRefuses(*this, *Reserved, FString::Printf(TEXT("Function %s: 'read' is reserved"), *Named(TEXT("Read"))));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Slow = NewDefinition();
	const FString TooLong = FString::Printf(TEXT("Function %s: Cooldown must be at most 2592000 seconds"), *Named(TEXT("Hit")));
	Slow->Functions[0].CooldownSeconds = 2592001.f;
	TestRefuses(*this, *Slow, TooLong);
	Slow->Functions[0].CooldownSeconds = std::numeric_limits<float>::infinity();
	TestRefuses(*this, *Slow, TooLong);
	Slow->Functions[0].CooldownSeconds = 2592000.f;
	TestBakes(*this, *Slow, TEXT("a cooldown of 30 days"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRulesCanCallTest, "CrowdySDK.CrowdyExec.Rules.CanCallRefusals", CrowdyExecRulesTests::TestFlags)
bool FCrowdyExecRulesCanCallTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRulesTests;
#if WITH_EDITORONLY_DATA
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Empty = NewDefinition();
	Empty->CanCall.Add(nullptr);
	TestRefuses(*this, *Empty, TEXT("Can Call: an entry is empty; choose a Server Object definition or remove the entry"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Itself = NewDefinition();
	Itself->CanCall.Add(Itself.Get());
	TestRefuses(*this, *Itself, TEXT("Can Call: a type cannot call itself"));

	const FName CalleeName = MakeUniqueObjectName(GetTransientPackage(), UCrowdyServerObjectDefinition::StaticClass(), TEXT("RulesNoTypeName"));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Nameless(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), CalleeName, RF_Transient));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Caller = NewDefinition();
	Caller->CanCall.Add(Nameless.Get());
	TestRefuses(*this, *Caller, FString::Printf(TEXT("Can Call: %s has no Type Name"), *Nameless->GetName()));

	Nameless->TypeName = TEXT("Rules Callee");
	TestRefuses(*this, *Caller, FString::Printf(TEXT("Can Call: %s has the Type Name 'Rules Callee', which must be lowercase letters, digits and underscores, start with a letter, and be at most 48 characters"),
		*Nameless->GetName()));

	Nameless->TypeName = TEXT("test_rules_callee");
	TestBakes(*this, *Caller, TEXT("a Can Call entry with a Type Name"));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRulesValueRangeTest, "CrowdySDK.CrowdyExec.Rules.ValueRangeRefusals", CrowdyExecRulesTests::TestFlags)
bool FCrowdyExecRulesValueRangeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRulesTests;
#if WITH_EDITOR
	const FString Amount = FString::Printf(TEXT("Function %s: %s"), *Named(TEXT("Deposit")), *Named(TEXT("Amount")));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Reversed = NewDefinition();
	AddDeposit(*Reversed, EPropertyBagPropertyType::Int32, TEXT("100"), TEXT("1"));
	TestRefuses(*this, *Reversed, Amount + TEXT(": Value Range minimum 100 is above its maximum 1"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Words = NewDefinition();
	AddDeposit(*Words, EPropertyBagPropertyType::Int32, TEXT("lots"), TEXT(""));
	TestRefuses(*this, *Words, Amount + TEXT(": Value Range 'lots' is not a number"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Fraction = NewDefinition();
	AddDeposit(*Fraction, EPropertyBagPropertyType::Int32, TEXT("1.5"), TEXT(""));
	TestRefuses(*this, *Fraction, Amount + TEXT(": Value Range '1.5' is not a whole number, and the input is"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Text = NewDefinition();
	AddDeposit(*Text, EPropertyBagPropertyType::String, TEXT("1"), TEXT("5"));
	TestRefuses(*this, *Text, Amount + TEXT(" has a Value Range, which only a number input can have"));

	// The editor never saves these, so a List input's bound is held to its form: no '+', no exponent, whole for a whole input.
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Signed = NewDefinition();
	AddDeposit(*Signed, EPropertyBagPropertyType::Double, TEXT("+5"), TEXT(""));
	TestRefuses(*this, *Signed, Amount + TEXT(": Value Range '+5' is not a number"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Exponent = NewDefinition();
	AddDeposit(*Exponent, EPropertyBagPropertyType::Double, TEXT(""), TEXT("1e3"));
	TestRefuses(*this, *Exponent, Amount + TEXT(": Value Range '1e3' is not a number"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> PointZero = NewDefinition();
	AddDeposit(*PointZero, EPropertyBagPropertyType::Int32, TEXT("1.0"), TEXT(""));
	TestRefuses(*this, *PointZero, Amount + TEXT(": Value Range '1.0' is not a whole number, and the input is"));

	// Both are 2^53 once they pass through a double, so only an exact comparison sees the minimum above the maximum.
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Close = NewDefinition();
	AddDeposit(*Close, EPropertyBagPropertyType::Int64, TEXT("9007199254740993"), TEXT("9007199254740992"));
	TestRefuses(*this, *Close, Amount + TEXT(": Value Range minimum 9007199254740993 is above its maximum 9007199254740992"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Fine = NewDefinition();
	AddDeposit(*Fine, EPropertyBagPropertyType::Double, TEXT("0.5"), TEXT("2.5"));
	AddDeposit(*Fine, EPropertyBagPropertyType::Int32, TEXT("-3"), TEXT(""));
	Fine->Functions[2].Name = TEXT("Withdraw");
	TestBakes(*this, *Fine, TEXT("List inputs with a Value Range and with only a minimum"));
#endif

#if WITH_METADATA
	// FCrowdyExecTestHit is shared with other tests, so the metadata set here is removed again before returning.
	FProperty* Damage = FindFProperty<FProperty>(FCrowdyExecTestHit::StaticStruct(), TEXT("Damage"));
	FProperty* Source = FindFProperty<FProperty>(FCrowdyExecTestHit::StaticStruct(), TEXT("Source"));
	if (!TestNotNull(TEXT("FCrowdyExecTestHit has Damage"), Damage) || !TestNotNull(TEXT("and Source"), Source))
	{
		return false;
	}
	Damage->SetMetaData(TEXT("ClampMin"), TEXT("100"));
	Damage->SetMetaData(TEXT("ClampMax"), TEXT("1"));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Struct = NewDefinition();
	TestRefuses(*this, *Struct, FString::Printf(TEXT("Function %s: Damage: Value Range minimum 100 is above its maximum 1"), *Named(TEXT("Hit"))));
	Damage->SetMetaData(TEXT("ClampMin"), TEXT("1.5"));
	TestRefuses(*this, *Struct, FString::Printf(TEXT("Function %s: Damage: Value Range '1.5' is not a whole number, and the input is"), *Named(TEXT("Hit"))));

	// UE metadata writes a whole bound as 0.0, and a name field may carry a ClampMax of its own that no Value Range reads.
	Damage->SetMetaData(TEXT("ClampMin"), TEXT("0.0"));
	Damage->SetMetaData(TEXT("ClampMax"), TEXT("100"));
	Source->SetMetaData(TEXT("ClampMax"), TEXT("5"));
	TestBakes(*this, *Struct, TEXT("a struct input with a whole bound written 0.0 and a clamp on a name field"));
	Damage->RemoveMetaData(TEXT("ClampMin"));
	Damage->RemoveMetaData(TEXT("ClampMax"));
	Source->RemoveMetaData(TEXT("ClampMax"));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRulesFullDefinitionTest, "CrowdySDK.CrowdyExec.Rules.FullDefinitionBakes", CrowdyExecRulesTests::TestFlags)
bool FCrowdyExecRulesFullDefinitionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRulesTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Callee = NewDefinition(TEXT("test_rules_callee"));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Team = NewDefinition(TEXT("test_rules_team"));
	Team->Visibility = ECrowdyServerObjectVisibility::Members;
	Team->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Team->MaxMembers = 5;
	Team->bRemoveMembersWhoLeave = false;
	Team->Functions[0].WhoCanCall = ECrowdyServerFunctionCaller::Members;
	Team->Functions[0].CooldownSeconds = 2.f;
	FCrowdyServerFunction& Promote = Team->Functions.AddDefaulted_GetRef();
	Promote.Name = TEXT("Promote");
	Promote.Params = FCrowdyServerMemberInputs::StaticStruct();
	Promote.WhoCanCall = ECrowdyServerFunctionCaller::Leader;
#if WITH_EDITOR
	AddDeposit(*Team, EPropertyBagPropertyType::Int32, TEXT("1"), TEXT("100"));
#endif
	FCrowdyServerTimer& EndMatch = AddTimer(*Team, TEXT("RulesEndMatch"), 10.f);
	EndMatch.Repeat = ECrowdyServerTimerRepeat::After;
	EndMatch.bStartAutomatically = false;
	AddTimer(*Team, TEXT("RulesTick"), 1.f);
	Team->bOnPlayerJoined = true;
	Team->bOnPlayerLeft = true;
#if WITH_EDITORONLY_DATA
	Team->CanCall.Add(Callee.Get());
#endif
	TestBakes(*this, *Team, TEXT("a definition using every rule"));
	TestTrue(TEXT("with the member input struct baked once"),
		Team->BakedStructs.FilterByPredicate([](const FCrowdyExecBakedStruct& Baked) { return Baked.Struct.Get() == FCrowdyServerMemberInputs::StaticStruct(); }).Num() == 1);
	TestNotNull(TEXT("and Make Leader offered beside its own functions"), Team->FindFunction(TEXT("MakeLeader")));
	return true;
}

#endif
