#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecTestTypes.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectLink.h"
#include "CrowdyServerObjectSubsystem.h"
#include "CrowdyServerObjectTestRig.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyServerFunctionLibraryTests
{
	/** A library with Attack (a struct in, a struct out) and Roll (nothing either way), both for players. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> NewLibraryDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TEXT("test_library");
		Definition->Kind = ECrowdyServerObjectKind::FunctionLibrary;
		FCrowdyServerFunction& Attack = Definition->Functions.AddDefaulted_GetRef();
		Attack.Name = TEXT("Attack");
		Attack.Params = FCrowdyServerObjectTestAttackParams::StaticStruct();
		Attack.Reply = FCrowdyServerObjectTestAttackReply::StaticStruct();
		Definition->Functions.AddDefaulted_GetRef().Name = TEXT("Roll");
		return Definition;
	}

	/** The library's settings from when it was a Server Object, all of which a library ignores. */
	void LeaveServerObjectSettings(UCrowdyServerObjectDefinition& Definition)
	{
		Definition.State = FCrowdyExecTestBossState::StaticStruct();
		Definition.WatchedFields = {TEXT("Health")};
		Definition.OwnerFields = {TEXT("Health")};
		Definition.Holds = ECrowdyServerObjectHolds::RecordPerEntity;
		Definition.Visibility = ECrowdyServerObjectVisibility::OwnerOnly;
		Definition.MembersFrom = ECrowdyServerMembersSource::ThisObject;
		Definition.bOnPlayerJoined = true;
		FCrowdyServerMessage& Message = Definition.Messages.AddDefaulted_GetRef();
		Message.Name = TEXT("Boss");
	}

	TArray<FString> LibraryBakeErrors(UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Errors;
		Definition.Bake(Errors);
		return Errors;
	}

	bool LibraryHasError(const TArray<FString>& Errors, const FString& Expected)
	{
		return Errors.ContainsByPredicate([&Expected](const FString& Error) { return Error.Equals(Expected, ESearchCase::CaseSensitive); });
	}

	void TestLibraryRefuses(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const FString& Expected)
	{
		const TArray<FString> Errors = LibraryBakeErrors(Definition);
		Test.TestTrue(FString::Printf(TEXT("refused with \"%s\" (got: %s)"), *Expected, *FString::Join(Errors, TEXT("; "))), LibraryHasError(Errors, Expected));
	}

	void TestLibraryBakes(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const TCHAR* What)
	{
		const TArray<FString> Errors = LibraryBakeErrors(Definition);
		Test.TestTrue(FString::Printf(TEXT("%s bakes (%s)"), What, *FString::Join(Errors, TEXT("; "))), Errors.IsEmpty());
	}

	/** Makes the rig's definition a library that still holds everything it held as a Server Object. */
	bool UseLibraryKind(CrowdyServerObjectTests::FRig& Rig)
	{
		Rig.Definition->Kind = ECrowdyServerObjectKind::FunctionLibrary;
		Rig.Definition->Visibility = ECrowdyServerObjectVisibility::OwnerOnly;
		Rig.Definition->Holds = ECrowdyServerObjectHolds::RecordPerEntity;
		Rig.Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
		Rig.InstanceId = FString();
		TArray<FString> Errors;
		const bool bBaked = Rig.Definition->Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the library bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	/** Makes the rig's definition a library whose only function, Ping, takes and returns nothing, so it bakes no structs. */
	bool UseStructlessLibrary(CrowdyServerObjectTests::FRig& Rig)
	{
		Rig.Definition->Kind = ECrowdyServerObjectKind::FunctionLibrary;
		Rig.Definition->State = nullptr;
		Rig.Definition->WatchedFields.Reset();
		Rig.Definition->Functions.Reset();
		Rig.Definition->Functions.AddDefaulted_GetRef().Name = TEXT("Ping");
		Rig.InstanceId = FString();
		TArray<FString> Errors;
		const bool bBaked = Rig.Definition->Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the library bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked)
			&& Rig.Test.TestTrue(TEXT("and has no field tables"), Rig.Definition->BakedStructs.IsEmpty());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionLibraryOnlyInstanceIdTest, "CrowdySDK.CrowdyExec.FunctionLibrary.OnlyInstanceIdIsEmpty", CrowdyServerObjectTests::TestFlags)
bool FCrowdyFunctionLibraryOnlyInstanceIdTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerFunctionLibraryTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = NewLibraryDefinition();
	TestTrue(TEXT("a library has only one instance"), Library->HasOnlyOneInstance());
	TestFalse(TEXT("without Only One Instance being on"), Library->bOnlyOneInstance);
	TestTrue(TEXT("its Instance Id is empty"), Library->GetOnlyInstanceId().IsEmpty());
	Library->bOnlyOneInstance = true;
	TestTrue(TEXT("and stays empty with Only One Instance on, not main"), Library->GetOnlyInstanceId().IsEmpty());

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Object(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Object->TypeName = TEXT("test_object");
	TestFalse(TEXT("a Server Object picks its instance"), Object->HasOnlyOneInstance());
	Object->bOnlyOneInstance = true;
	TestEqualSensitive(TEXT("with Only One Instance its Instance Id is main"), Object->GetOnlyInstanceId(), FString(UCrowdyServerObjectDefinition::OnlyInstanceId));
	Object->TypeName = UCrowdyServerObjectDefinition::RootTypeName;
	TestTrue(TEXT("and the root's is empty"), Object->GetOnlyInstanceId().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionLibraryReachabilityTest, "CrowdySDK.CrowdyExec.FunctionLibrary.ReachabilityCountsOnlyPlayerFunctions", CrowdyServerObjectTests::TestFlags)
bool FCrowdyFunctionLibraryReachabilityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerFunctionLibraryTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = NewLibraryDefinition();
	LeaveServerObjectSettings(*Library);
	Library->Messages[0].bPlayersCanListen = true;
	for (FCrowdyServerFunction& Function : Library->Functions)
	{
		Function.WhoCanCall = ECrowdyServerFunctionCaller::ServerOnly;
	}
	TestFalse(TEXT("a library of server-only functions is closed to players, whatever else its asset holds"), Library->IsReachableByPlayers());
	Library->Functions[0].WhoCanCall = ECrowdyServerFunctionCaller::DevelopersOnly;
	TestFalse(TEXT("developer-only functions are no way in either"), Library->IsReachableByPlayers());
	Library->Functions[1].WhoCanCall = ECrowdyServerFunctionCaller::Players;
	TestTrue(TEXT("one function players can call opens it"), Library->IsReachableByPlayers());

	Library->Functions[1].WhoCanCall = ECrowdyServerFunctionCaller::ServerOnly;
	Library->Kind = ECrowdyServerObjectKind::ServerObject;
	TestTrue(TEXT("the same asset as a Server Object is reachable through what the library ignored"), Library->IsReachableByPlayers());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionLibraryHiddenSettingsTest, "CrowdySDK.CrowdyExec.FunctionLibrary.HiddenSettingsAreIgnored", CrowdyServerObjectTests::TestFlags)
bool FCrowdyFunctionLibraryHiddenSettingsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerFunctionLibraryTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = NewLibraryDefinition();
	LeaveServerObjectSettings(*Library);
	Library->Messages[0].bPlayersCanListen = true;

	TestFalse(TEXT("a library has no restricted readers"), Library->HasRestrictedReaders());
	TestFalse(TEXT("and no player messages"), Library->HasPlayerMessages());
	TestNull(TEXT("a message it still holds is not found by name"), Library->FindMessage(TEXT("Boss")));
	TestNull(TEXT("or by topic"), Library->FindMessageByTopic(TEXT("boss")));
	TestNull(TEXT("it has no State"), Library->GetStateStruct());
	TestNull(TEXT("and offers no built-in member function"), Library->FindFunction(TEXT("Join")));
	TestNotNull(TEXT("its own functions are found"), Library->FindFunction(TEXT("Attack")));
	TArray<FCrowdyServerNamedList> Lists;
	Library->GetLists(Lists);
	TestTrue(TEXT("and it has no List of a State or a message"), Lists.IsEmpty());

	Library->Kind = ECrowdyServerObjectKind::ServerObject;
	TestTrue(TEXT("as a Server Object the same asset has restricted readers"), Library->HasRestrictedReaders());
	TestTrue(TEXT("player messages"), Library->HasPlayerMessages());
	TestNotNull(TEXT("its message"), Library->FindMessage(TEXT("Boss")));
	TestTrue(TEXT("its State"), Library->GetStateStruct() == FCrowdyExecTestBossState::StaticStruct());
	TestNotNull(TEXT("and the built-in Join"), Library->FindFunction(TEXT("Join")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionLibraryValidationTest, "CrowdySDK.CrowdyExec.FunctionLibrary.ValidationMessages", CrowdyServerObjectTests::TestFlags)
bool FCrowdyFunctionLibraryValidationTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerFunctionLibraryTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = NewLibraryDefinition();
	TestLibraryBakes(*this, *Library, TEXT("a library of two player functions"));

	Library->Functions[0].WhoCanCall = ECrowdyServerFunctionCaller::Members;
	TestLibraryRefuses(*this, *Library, TEXT("Attack: Members and Leader need members, and a Server Function Library has none."));
	Library->Functions[0].WhoCanCall = ECrowdyServerFunctionCaller::Players;
	Library->Functions[1].WhoCanCall = ECrowdyServerFunctionCaller::Leader;
	const TArray<FString> LeaderErrors = LibraryBakeErrors(*Library);
	TestTrue(TEXT("Leader is refused the same way"), LibraryHasError(LeaderErrors, TEXT("Roll: Members and Leader need members, and a Server Function Library has none.")));
	TestFalse(TEXT("and does not ask for Members From, which a library does not have"), LeaderErrors.ContainsByPredicate([](const FString& Error) { return Error.Contains(TEXT("Members From")); }));
	Library->Functions[1].WhoCanCall = ECrowdyServerFunctionCaller::Players;

	Library->Functions[0].CooldownSeconds = 5.f;
	TestLibraryRefuses(*this, *Library, TEXT("Attack: a cooldown is saved data, and a Server Function Library saves none."));
	Library->Functions[0].CooldownSeconds = 0.f;
	TestLibraryBakes(*this, *Library, TEXT("the library with those settings undone"));

#if WITH_EDITORONLY_DATA
	Library->MinInstances = 3;
	Library->MaxInstances = 2;
	TestLibraryRefuses(*this, *Library, TEXT("Max Instances must be at least Min Instances."));
	Library->MaxInstances = 3;
	TestLibraryBakes(*this, *Library, TEXT("equal Min Instances and Max Instances"));
	Library->MaxInstances = 8;
	TestLibraryBakes(*this, *Library, TEXT("a Max Instances above Min Instances"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Object(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Object->TypeName = TEXT("test_listener");
	Object->State = FCrowdyExecTestBossState::StaticStruct();
	Object->ListenTo.Add(Library.Get());
	TestLibraryRefuses(*this, *Object, TEXT("test_library is a Server Function Library and sends no messages."));
	Object->ListenTo.Reset();
	Object->CanCall.Add(Library.Get());
	TestLibraryBakes(*this, *Object, TEXT("a Server Object that calls a library"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Other = NewLibraryDefinition();
	Other->TypeName = TEXT("test_other_library");
	Other->ListenTo.Add(Library.Get());
	TestLibraryBakes(*this, *Other, TEXT("a library whose hidden Listen To names a library"));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionLibraryNoStateTest, "CrowdySDK.CrowdyExec.FunctionLibrary.ValidatesWithoutState", CrowdyServerObjectTests::TestFlags)
bool FCrowdyFunctionLibraryNoStateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerFunctionLibraryTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = NewLibraryDefinition();
	TestLibraryBakes(*this, *Library, TEXT("a library with no State"));
	TestTrue(TEXT("its tables hold the functions' structs"), Library->BakedStructs.Num() == 2);

	Library->WatchedFields = {TEXT("Nope")};
	Library->Visibility = ECrowdyServerObjectVisibility::Members;
	Library->Holds = ECrowdyServerObjectHolds::RecordPerEntity;
	Library->MaxRecords = 0;
	Library->OwnerFields = {TEXT("Nope")};
	Library->Timers.AddDefaulted();
	Library->Messages.AddDefaulted();
	Library->AreaSizeChunks = 0;
	TestLibraryBakes(*this, *Library, TEXT("a library that still holds settings that would not bake as a Server Object"));

	Library->Kind = ECrowdyServerObjectKind::ServerObject;
	const TArray<FString> Errors = LibraryBakeErrors(*Library);
	TestTrue(TEXT("as a Server Object the same asset needs a State struct"), LibraryHasError(Errors, TEXT("Choose a State struct")));
	TestTrue(TEXT("and has more to fix"), Errors.Num() > 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionLibraryAcquireTest, "CrowdySDK.CrowdyExec.FunctionLibrary.AcquireIsReadyWithoutSubscribeOrRead", CrowdyServerObjectTests::TestFlags)
bool FCrowdyFunctionLibraryAcquireTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerFunctionLibraryTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !UseLibraryKind(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(TEXT("ignored-id"), Owner.Get());
	if (!Object || !Rig.Open())
	{
		return false;
	}
	TestTrue(TEXT("a library is acquired by the empty Instance Id, whatever is asked for"), Object->GetInstanceId().IsEmpty());
	TestTrue(TEXT("it is Ready as soon as the connection opens"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }));
	TestFalse(TEXT("it never subscribes to state, though its asset still watches variables and members"), Rig.SentAny(KindSubscribe, TEXT("state")));
	TestFalse(TEXT("and never reads"), Rig.SentAny(KindCall, TEXT("read")));
	TestFalse(TEXT("it holds no State"), Object->GetState().IsValid());
	TestFalse(TEXT("and no records"), Object->HoldsRecords());

	UCrowdyServerObject* Again = Rig.Acquire(TEXT("another-id"), Owner.Get());
	TestTrue(TEXT("any other Instance Id gives the same object"), Again == Object);
	TestEqual(TEXT("which is the only one"), Rig.Subsystem->NumObjectsForTest(), 1);

	UCrowdyServerObjectLink* ById = Rig.Subsystem->FindLink(Owner.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::InstanceId, TEXT("some-id"), 0);
	UCrowdyServerObjectLink* ByPlayer = Rig.Subsystem->FindLink(Owner.Get(), Rig.Definition.Get(), ECrowdyServerObjectFind::SignedInPlayer, FString(), 0);
	TestTrue(TEXT("every way of finding it gives one Link"), ById && ById == ByPlayer);
	TestTrue(TEXT("which holds the library's object"), ById && ById->GetServerObject() == Object);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionLibraryCallTest, "CrowdySDK.CrowdyExec.FunctionLibrary.CallSendsTheEmptyKey", CrowdyServerObjectTests::TestFlags)
bool FCrowdyFunctionLibraryCallTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerFunctionLibraryTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !UseLibraryKind(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(FString(), Owner.Get());
	if (!Object || !Rig.Open())
	{
		return false;
	}
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	TestEqualSensitive(TEXT("to the library's type"), Sent.NodeType, FString(TypeName));
	TestTrue(TEXT("with the empty key, so the platform picks an instance"), Sent.Key.IsEmpty());
	TestTrue(TEXT("and it is addressed as the rig addresses its one object"), Rig.IsAddressed(Sent));

	FCrowdyServerObjectTestAttackReply Reply;
	Reply.HealthLeft = 17;
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Reply));
	if (!TestTrue(TEXT("the answer completes the call"), Rig.PumpUntil([&Call]() { return Call.Num() == 1; })))
	{
		return false;
	}
	const FCrowdyServerCallResult& Result = (*Call.Results)[0];
	TestTrue(TEXT("with success"), Result.IsSuccess());
	const FCrowdyServerObjectTestAttackReply* Answer = Result.Reply.GetPtr<FCrowdyServerObjectTestAttackReply>();
	TestEqual(TEXT("and the function's reply"), Answer ? Answer->HealthLeft : -1, 17);

	FCallSpy Join;
	Object->Call(TEXT("Join"), FInstancedStruct(), Join.OnDone());
	TestEqual(TEXT("a built-in member function is not offered, though the asset still keeps members"), Join.Num(), 1);
	TestEqual(TEXT("the call is a bad request"), static_cast<int32>(Join.Num() == 1 ? (*Join.Results)[0].Outcome : ECrowdyServerCallOutcome::Success),
		static_cast<int32>(ECrowdyServerCallOutcome::BadRequest));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionLibraryCallLibraryFunctionTest, "CrowdySDK.CrowdyExec.FunctionLibrary.CallLibraryFunctionAnswersOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyFunctionLibraryCallLibraryFunctionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerFunctionLibraryTests;
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = NewLibraryDefinition();
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Object(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Object->TypeName = TEXT("test_object");
	Object->State = FCrowdyExecTestBossState::StaticStruct();
	Object->Functions.AddDefaulted_GetRef().Name = TEXT("Attack");

	auto ExpectOnce = [this](const TCHAR* What, const FCallSpy& Spy, ECrowdyServerCallOutcome Outcome, const TCHAR* ReasonPart)
	{
		if (!TestEqual(FString::Printf(TEXT("%s runs OnDone once"), What), Spy.Num(), 1))
		{
			return;
		}
		const FCrowdyServerCallResult& Result = (*Spy.Results)[0];
		TestEqual(FString::Printf(TEXT("%s gives the outcome"), What), static_cast<int32>(Result.Outcome), static_cast<int32>(Outcome));
		TestTrue(FString::Printf(TEXT("%s says why (%s)"), What, *Result.Reason), Result.Reason.Contains(ReasonPart));
	};

	FCallSpy NoDefinition;
	UCrowdyServerObjectLibrary::CallLibraryFunction(Widget.Get(), nullptr, TEXT("Attack"), FInstancedStruct(), NoDefinition.OnDone());
	ExpectOnce(TEXT("a null definition"), NoDefinition, ECrowdyServerCallOutcome::BadRequest, TEXT("not a Server Function Library"));

	FCallSpy NotLibrary;
	UCrowdyServerObjectLibrary::CallLibraryFunction(Widget.Get(), Object.Get(), TEXT("Attack"), FInstancedStruct(), NotLibrary.OnDone());
	ExpectOnce(TEXT("a Server Object"), NotLibrary, ECrowdyServerCallOutcome::BadRequest, TEXT("not a Server Function Library"));

	FCallSpy Unknown;
	UCrowdyServerObjectLibrary::CallLibraryFunction(Widget.Get(), Library.Get(), TEXT("Nope"), FInstancedStruct(), Unknown.OnDone());
	ExpectOnce(TEXT("an unknown function"), Unknown, ECrowdyServerCallOutcome::BadRequest, TEXT("Nope"));

	FCallSpy NoWorld;
	UCrowdyServerObjectLibrary::CallLibraryFunction(Widget.Get(), Library.Get(), TEXT("Roll"), FInstancedStruct(), NoWorld.OnDone());
	ExpectOnce(TEXT("a call from an object in no world"), NoWorld, ECrowdyServerCallOutcome::Unavailable, TEXT("world"));

	FCallSpy ListNull;
	UCrowdyServerObjectLibrary::CallLibraryFunction(Widget.Get(), nullptr, TEXT("Attack"), FInstancedPropertyBag(), ListNull.OnDone());
	ExpectOnce(TEXT("a null definition with List inputs"), ListNull, ECrowdyServerCallOutcome::BadRequest, TEXT("not a Server Function Library"));

	FCallSpy ListUnknown;
	UCrowdyServerObjectLibrary::CallLibraryFunction(Widget.Get(), Library.Get(), TEXT("Nope"), FInstancedPropertyBag(), ListUnknown.OnDone());
	ExpectOnce(TEXT("an unknown function with List inputs"), ListUnknown, ECrowdyServerCallOutcome::BadRequest, TEXT("Nope"));

	FCallSpy ListNoWorld;
	UCrowdyServerObjectLibrary::CallLibraryFunction(Widget.Get(), Library.Get(), TEXT("Roll"), FInstancedPropertyBag(), ListNoWorld.OnDone());
	ExpectOnce(TEXT("a List call from an object in no world"), ListNoWorld, ECrowdyServerCallOutcome::Unavailable, TEXT("world"));

	UCrowdyServerObjectLibrary::CallLibraryFunction(Widget.Get(), Library.Get(), TEXT("Roll"), FInstancedStruct(), TFunction<void(const FCrowdyServerCallResult&)>());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionLibraryStructlessTest, "CrowdySDK.CrowdyExec.FunctionLibrary.WithoutStructsResolvesAnEmptyLayoutAndCalls", CrowdyServerObjectTests::TestFlags)
bool FCrowdyFunctionLibraryStructlessTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerFunctionLibraryTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !UseStructlessLibrary(Rig))
	{
		return false;
	}
	FString LayoutError;
	const FCrowdyExecLayout* Layout = Rig.Definition->ResolveLayout(LayoutError);
	if (!TestNotNull(FString::Printf(TEXT("a library with no structs resolves a layout (%s)"), *LayoutError), Layout))
	{
		return false;
	}
	TestTrue(TEXT("with no structs"), Layout->Structs.IsEmpty());
	TestEqual(TEXT("and no State"), Layout->StateStruct, static_cast<int32>(INDEX_NONE));

	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(FString(), Owner.Get());
	if (!Object || !Rig.Open())
	{
		return false;
	}
	TestTrue(TEXT("it is Ready as soon as the connection opens"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }));
	FCallSpy Call;
	Object->Call(TEXT("Ping"), FInstancedStruct(), Call.OnDone());
	FSentFrame Sent;
	if (!TestTrue(TEXT("its function is called"), Rig.WaitForFrame(KindCall, TEXT("ping"), Sent)))
	{
		return false;
	}
	TestTrue(TEXT("under the empty key"), Sent.Key.IsEmpty());
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Ok, {});
	if (!TestTrue(TEXT("the answer completes the call"), Rig.PumpUntil([&Call]() { return Call.Num() == 1; })))
	{
		return false;
	}
	TestTrue(TEXT("with success"), (*Call.Results)[0].IsSuccess());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFunctionLibraryCallSucceedsAndKeepsTheLibraryTest, "CrowdySDK.CrowdyExec.FunctionLibrary.CallLibraryFunctionAnswersAndTheSubsystemKeepsTheLibrary", CrowdyServerObjectTests::TestFlags)
bool FCrowdyFunctionLibraryCallSucceedsAndKeepsTheLibraryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerFunctionLibraryTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !UseLibraryKind(Rig))
	{
		return false;
	}
	// The world-to-subsystem lookup needs a world with a registered game instance subsystem, which a rig cannot build, so the subsystem's own entry point is called.
	FCallSpy First;
	Rig.Subsystem->CallLibraryFunction(Rig.Definition.Get(), AttackFunction, AttackParams(250), First.OnDone());
	TestEqual(TEXT("the first call makes the subsystem hold the library"), Rig.Subsystem->NumObjectsForTest(), 1);
	if (!Rig.Open())
	{
		return false;
	}
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent once the library is connected"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	TestTrue(TEXT("with the empty key"), Sent.Key.IsEmpty());

	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestEqual(TEXT("the library outlives the grace period, as the subsystem is its owner"), Rig.Subsystem->NumObjectsForTest(), 1);
	TestEqual(TEXT("and the call is still waiting"), First.Num(), 0);

	FCrowdyServerObjectTestAttackReply Reply;
	Reply.HealthLeft = 17;
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Reply));
	if (!TestTrue(TEXT("the answer completes the call"), Rig.PumpUntil([&First]() { return First.Num() == 1; })))
	{
		return false;
	}
	const FCrowdyServerCallResult& FirstResult = (*First.Results)[0];
	TestTrue(TEXT("with success"), FirstResult.IsSuccess());
	const FCrowdyServerObjectTestAttackReply* Answer = FirstResult.Reply.GetPtr<FCrowdyServerObjectTestAttackReply>();
	TestEqual(TEXT("and the function's reply"), Answer ? Answer->HealthLeft : -1, 17);

	FCallSpy Second;
	Rig.Subsystem->CallLibraryFunction(Rig.Definition.Get(), AttackFunction, AttackParams(5), Second.OnDone());
	FSentFrame SecondSent;
	if (!TestTrue(TEXT("a second call is sent over the same connection"), Rig.WaitForFrame(KindCall, AttackMethod, SecondSent)))
	{
		return false;
	}
	Rig.Reply(SecondSent.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Reply));
	TestTrue(TEXT("and answered"), Rig.PumpUntil([&Second]() { return Second.Num() == 1; }));
	TestEqual(TEXT("the second call ran OnDone once as well"), Second.Num(), 1);
	TestEqual(TEXT("and the subsystem still holds exactly one library"), Rig.Subsystem->NumObjectsForTest(), 1);
	TestEqual(TEXT("the first call's OnDone did not run again"), First.Num(), 1);
	return true;
}

#endif
