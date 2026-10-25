#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecInternal.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectParityTestTypes.h"
#include "CrowdyServerObjectSubsystem.h"
#include "CrowdyServerObjectTestRig.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace CrowdyServerObjectParityTests
{
	using namespace CrowdyServerObjectTests;

	TStrongObjectPtr<UCrowdyServerObjectParityListener> MakeParityListener()
	{
		return TStrongObjectPtr<UCrowdyServerObjectParityListener>(NewObject<UCrowdyServerObjectParityListener>(GetTransientPackage(), NAME_None, RF_Transient));
	}

	TStrongObjectPtr<UCrowdyServerObjectTestListener> MakeParityIntListener()
	{
		return TStrongObjectPtr<UCrowdyServerObjectTestListener>(NewObject<UCrowdyServerObjectTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	}

	FCrowdyServerRecordEvent ParityRecordEventOf(UCrowdyServerObjectParityListener* Listener)
	{
		FCrowdyServerRecordEvent Event;
		Event.BindDynamic(Listener, &UCrowdyServerObjectParityListener::HandleRecord);
		return Event;
	}

	/** Records holding FCrowdyServerObjectTestRecord, watching Health and Label. */
	bool UseParityRecords(FRig& Rig)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Records(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Records->TypeName = TypeName;
		Records->Holds = ECrowdyServerObjectHolds::RecordPerEntity;
		Records->State = FCrowdyServerObjectTestRecord::StaticStruct();
		Records->WatchedFields = {TEXT("Health"), TEXT("Label")};
		TArray<FString> Errors;
		const bool bBaked = Records->Bake(Errors);
		Rig.Test.TestTrue(FString::Printf(TEXT("the records definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
		Rig.Definition = Records;
		return bBaked;
	}

	TArray<FString> ParityRecordVariables()
	{
		return {TEXT("Health"), TEXT("Label")};
	}

	TPair<FString, FInstancedStruct> ParityRecord(const FString& Id, int32 Health, const FString& Label, int32 Secret = 0)
	{
		FCrowdyServerObjectTestRecord Record;
		Record.Health = Health;
		Record.Label = Label;
		Record.Secret = Secret;
		return TPair<FString, FInstancedStruct>(Id, FInstancedStruct::Make(Record));
	}

	void ParityApply(UCrowdyServerObject* Object, const TPair<FString, FInstancedStruct>& Record)
	{
		Object->ApplyRecordsForTest({Record}, {});
	}

	void ParityRemove(UCrowdyServerObject* Object, const FString& Id)
	{
		Object->ApplyRecordsForTest({}, {Id});
	}

	const FCrowdyServerRecordChange* ParityFindChange(const TArray<FCrowdyServerRecordChange>& Changes, const FString& Id)
	{
		return Changes.FindByPredicate([&Id](const FCrowdyServerRecordChange& Each) { return Each.RecordId.Equals(Id, ESearchCase::CaseSensitive); });
	}

	const FProperty* ParityPin(FName Pin)
	{
		return FCrowdyServerObjectTestPins::StaticStruct()->FindPropertyByName(Pin);
	}

	/** Reads Variable of Object into the pin Pin of Pins. */
	bool ParityRead(const UCrowdyServerObject* Object, const UCrowdyServerObjectDefinition* Definition, FName Variable, FCrowdyServerObjectTestPins& Pins, FName Pin)
	{
		const FProperty* Property = ParityPin(Pin);
		return Property && UCrowdyServerObjectLibrary::ReadServerVariableInto(Object, Definition, Variable, Property, Property->ContainerPtrToValuePtr<void>(&Pins));
	}

	/** Turns the rig's State into a List of the same watched values, as an author who chose List would have. */
	bool UseParityListState(FRig& Rig)
	{
		UCrowdyServerObjectDefinition& Definition = *Rig.Definition;
		Definition.StateForm = ECrowdyServerValuesForm::List;
		Definition.StateList.AddProperty(TEXT("Health"), EPropertyBagPropertyType::Int32);
		Definition.StateList.AddProperty(TEXT("bDefeated"), EPropertyBagPropertyType::Bool);
		Definition.StateList.AddProperty(TEXT("Phase"), EPropertyBagPropertyType::Enum, StaticEnum<ECrowdyExecTestPhase>());
		TArray<FString> Errors;
		return Rig.Test.TestTrue(FString::Printf(TEXT("the List State bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Definition.Bake(Errors));
	}

	/** Opens the connection and answers the object's subscribe and first read with FirstRead. */
	bool ParityAnswerRead(FRig& Rig, const TArray<uint8>& FirstRead)
	{
		FSentFrame Subscribe;
		FSentFrame Read;
		if (!Rig.Open() || !Rig.Test.TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
			|| !Rig.Test.TestTrue(TEXT("the object reads"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
		{
			return false;
		}
		Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
		Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, FirstRead);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityBlueprintSurfaceTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.RecordFunctionsAreBlueprintCallable", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityBlueprintSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	const UClass* Class = UCrowdyServerObject::StaticClass();
	const UFunction* Holds = Class->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObject, HoldsRecords));
	const UFunction* Num = Class->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObject, NumRecords));
	const UFunction* Definition = Class->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObject, K2_GetDefinition));
	const UFunction* Watch = Class->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObject, K2_WatchRecord));
	const UFunction* Unwatch = Class->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyServerObject, K2_UnwatchRecord));
	const FMulticastDelegateProperty* Updated = CastField<FMulticastDelegateProperty>(Class->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UCrowdyServerObject, RecordsUpdated)));
	constexpr EFunctionFlags PureCallable = FUNC_BlueprintCallable | FUNC_BlueprintPure;
	TestTrue(TEXT("Holds Records is a pure Blueprint function"), Holds && Holds->HasAllFunctionFlags(PureCallable));
	TestTrue(TEXT("Num Records is a pure Blueprint function"), Num && Num->HasAllFunctionFlags(PureCallable));
	TestTrue(TEXT("Get Definition is a pure Blueprint function"), Definition && Definition->HasAllFunctionFlags(PureCallable));
	TestTrue(TEXT("Watch Record is a Blueprint function that is not pure"), Watch && Watch->HasAllFunctionFlags(FUNC_BlueprintCallable) && !Watch->HasAnyFunctionFlags(FUNC_BlueprintPure));
	TestTrue(TEXT("Stop Watching Record is a Blueprint function that is not pure"), Unwatch && Unwatch->HasAllFunctionFlags(FUNC_BlueprintCallable) && !Unwatch->HasAnyFunctionFlags(FUNC_BlueprintPure));
	TestTrue(TEXT("On Records Updated can be assigned in a graph"), Updated && Updated->HasAnyPropertyFlags(CPF_BlueprintAssignable));
#if WITH_METADATA
	TestTrue(TEXT("Watch Record is named so"), Watch && Watch->GetMetaData(TEXT("DisplayName")) == TEXT("Watch Record"));
	TestTrue(TEXT("Stop Watching Record is named so"), Unwatch && Unwatch->GetMetaData(TEXT("DisplayName")) == TEXT("Stop Watching Record"));
	TestTrue(TEXT("Get Definition is named so"), Definition && Definition->GetMetaData(TEXT("DisplayName")) == TEXT("Get Definition"));
	TestTrue(TEXT("On Records Updated is named so"), Updated && Updated->GetMetaData(TEXT("DisplayName")) == TEXT("On Records Updated"));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityRecordQueriesTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.HoldsRecordsNumRecordsAndGetDefinitionAnswer", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityRecordQueriesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	{
		FRig Rig(*this);
		if (!Rig.IsValid())
		{
			return false;
		}
		TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
		UCrowdyServerObject* Plain = Rig.Acquire(Rig.InstanceId, Owner.Get());
		if (!Plain)
		{
			return false;
		}
		TestFalse(TEXT("an object of one set of variables holds no records"), Plain->HoldsRecords());
		TestEqual(TEXT("so Num Records is 0"), Plain->NumRecords(), 0);
		TestTrue(TEXT("Get Definition gives the definition it was made from"), Plain->K2_GetDefinition() == Rig.Definition.Get());
	}

	FRig Rig(*this);
	if (!UseParityRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TestTrue(TEXT("an object of a record per entity holds records"), Object->HoldsRecords());
	TestEqual(TEXT("with none yet"), Object->NumRecords(), 0);
	Object->ApplyRecordsForTest({ParityRecord(TEXT("a"), 10, TEXT("x")), ParityRecord(TEXT("b"), 20, TEXT("y"))}, {});
	TestEqual(TEXT("Num Records counts the records held"), Object->NumRecords(), 2);
	ParityRemove(Object, TEXT("a"));
	TestEqual(TEXT("and one fewer once a record is removed"), Object->NumRecords(), 1);
	TestTrue(TEXT("Get Definition gives the records definition"), Object->K2_GetDefinition() == Rig.Definition.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityWatchRecordTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.WatchRecordRunsOnItsRecordsChangesAndRemoval", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityWatchRecordTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!UseParityRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectParityListener> First = MakeParityListener();
	TStrongObjectPtr<UCrowdyServerObjectParityListener> Second = MakeParityListener();

	Object->K2_WatchRecord(TEXT("a"), ParityRecordEventOf(First.Get()));
	Object->K2_WatchRecord(TEXT("a"), FCrowdyServerRecordEvent());
	TestEqual(TEXT("an unbound event is not kept"), Object->NumRecordEventWatchersForTest(), 1);
	TestEqual(TEXT("a record not there yet runs nothing at once"), First->WatchedRecordIds.Num(), 0);

	ParityApply(Object, ParityRecord(TEXT("a"), 10, TEXT("x")));
	if (!TestEqual(TEXT("the record arriving runs the watcher once"), First->WatchedRecordIds.Num(), 1))
	{
		return false;
	}
	TestEqualSensitive(TEXT("naming the record"), First->WatchedRecordIds[0], FString(TEXT("a")));
	TestTrue(FString::Printf(TEXT("and every variable of a new record (got %s)"), *Joined(First->WatchedChanged[0])), NamesAre(First->WatchedChanged[0], ParityRecordVariables()));

	ParityApply(Object, ParityRecord(TEXT("a"), 11, TEXT("x")));
	if (!TestEqual(TEXT("a change runs it again"), First->WatchedRecordIds.Num(), 2))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("naming only the variable that changed (got %s)"), *Joined(First->WatchedChanged[1])), NamesAre(First->WatchedChanged[1], {TEXT("Health")}));

	ParityApply(Object, ParityRecord(TEXT("b"), 5, TEXT("z")));
	TestEqual(TEXT("another record's arrival does not run it"), First->WatchedRecordIds.Num(), 2);

	Object->K2_WatchRecord(TEXT("a"), ParityRecordEventOf(Second.Get()));
	if (!TestEqual(TEXT("a watcher bound to a current record runs once, at once"), Second->WatchedRecordIds.Num(), 1))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("naming every variable (got %s)"), *Joined(Second->WatchedChanged[0])), NamesAre(Second->WatchedChanged[0], ParityRecordVariables()));
	TestEqual(TEXT("its catch-up run reaches no other watcher"), First->WatchedRecordIds.Num(), 2);

	Object->K2_WatchRecord(TEXT("a"), ParityRecordEventOf(Second.Get()));
	TestEqual(TEXT("watching the same event on the same record twice keeps one"), Object->NumRecordEventWatchersForTest(), 2);
	TestEqual(TEXT("and runs nothing again"), Second->WatchedRecordIds.Num(), 1);
	Object->K2_WatchRecord(TEXT("b"), ParityRecordEventOf(Second.Get()));
	TestEqual(TEXT("the same event on another record is a watcher of its own"), Object->NumRecordEventWatchersForTest(), 3);
	TestEqual(TEXT("which runs its catch-up"), Second->WatchedRecordIds.Num(), 2);
	TestEqualSensitive(TEXT("for that record"), Second->WatchedRecordIds[1], FString(TEXT("b")));

	ParityApply(Object, ParityRecord(TEXT("a"), 12, TEXT("x")));
	TestEqual(TEXT("a change runs the first watcher once"), First->WatchedRecordIds.Num(), 3);
	TestEqual(TEXT("and the second once, not for the record it watches too"), Second->WatchedRecordIds.Num(), 3);

	Object->K2_UnwatchRecord(TEXT("a"), ParityRecordEventOf(First.Get()));
	Object->K2_UnwatchRecord(TEXT("a"), ParityRecordEventOf(First.Get()));
	TestEqual(TEXT("stopping a watcher drops it, and stopping it again does nothing"), Object->NumRecordEventWatchersForTest(), 2);
	ParityApply(Object, ParityRecord(TEXT("a"), 13, TEXT("x")));
	TestEqual(TEXT("a stopped watcher is not run"), First->WatchedRecordIds.Num(), 3);
	TestEqual(TEXT("one still watching is"), Second->WatchedRecordIds.Num(), 4);

	bool bFoundDuringRemoval = true;
	Second->OnRecord = [Object, &bFoundDuringRemoval]()
	{
		FInstancedStruct Values;
		int64 RecordOwner = 0;
		FString Team;
		bFoundDuringRemoval = Object->K2_FindRecord(TEXT("a"), Values, RecordOwner, Team);
	};
	ParityRemove(Object, TEXT("a"));
	if (!TestEqual(TEXT("a removal runs the watcher once"), Second->WatchedRecordIds.Num(), 5))
	{
		return false;
	}
	TestEqualSensitive(TEXT("naming the record that went"), Second->WatchedRecordIds[4], FString(TEXT("a")));
	TestFalse(TEXT("which Find Record no longer finds when the watcher runs"), bFoundDuringRemoval);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityWatchRecordFromRecordsHandlerRunsOnceTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.WatchRecordFromRecordsHandlerRunsOnce", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityWatchRecordFromRecordsHandlerRunsOnceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!UseParityRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectParityListener> First = MakeParityListener();
	TStrongObjectPtr<UCrowdyServerObjectParityListener> Second = MakeParityListener();
	Object->K2_WatchRecord(TEXT("a"), ParityRecordEventOf(First.Get()));
	ParityApply(Object, ParityRecord(TEXT("a"), 10, TEXT("x")));

	const FDelegateHandle Handler = Object->OnRecordsChanged.AddLambda([Object, &Second](UCrowdyServerObject*, TConstArrayView<FString>, TConstArrayView<FString>, TConstArrayView<TBitArray<>>)
	{
		Object->K2_WatchRecord(TEXT("a"), ParityRecordEventOf(Second.Get()));
	});
	ParityApply(Object, ParityRecord(TEXT("a"), 11, TEXT("x")));
	Object->OnRecordsChanged.Remove(Handler);

	TestEqual(TEXT("the existing watcher ran for the change"), First->WatchedRecordIds.Num(), 2);
	if (!TestEqual(TEXT("a watcher added by a records handler runs once for that change: its catch-up"), Second->WatchedRecordIds.Num(), 1))
	{
		return false;
	}
	ParityApply(Object, ParityRecord(TEXT("a"), 12, TEXT("x")));
	TestEqual(TEXT("and runs for the next change"), Second->WatchedRecordIds.Num(), 2);
	TestEqual(TEXT("which the existing watcher sees too"), First->WatchedRecordIds.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityWatchRecordDestroyedWatcherTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.WatchRecordDropsAWatcherWhoseObjectIsGone", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityWatchRecordDestroyedWatcherTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!UseParityRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	ParityApply(Object, ParityRecord(TEXT("a"), 10, TEXT("x")));
	TStrongObjectPtr<UCrowdyServerObjectParityListener> Gone = MakeParityListener();
	TStrongObjectPtr<UCrowdyServerObjectParityListener> Kept = MakeParityListener();
	Object->K2_WatchRecord(TEXT("a"), ParityRecordEventOf(Gone.Get()));
	Object->K2_WatchRecord(TEXT("a"), ParityRecordEventOf(Kept.Get()));
	TestEqual(TEXT("both watchers ran their catch-up once"), Gone->WatchedRecordIds.Num() + Kept->WatchedRecordIds.Num(), 2);

	Gone->MarkAsGarbage();
	Gone.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	ParityApply(Object, ParityRecord(TEXT("a"), 11, TEXT("x")));
	TestEqual(TEXT("a change after a watcher's object was destroyed still runs the others"), Kept->WatchedRecordIds.Num(), 2);
	TestEqual(TEXT("and drops the destroyed one"), Object->NumRecordEventWatchersForTest(), 1);
	ParityApply(Object, ParityRecord(TEXT("a"), 12, TEXT("x")));
	TestEqual(TEXT("the next change runs the one left once"), Kept->WatchedRecordIds.Num(), 3);
	Object->K2_UnwatchRecord(TEXT("a"), ParityRecordEventOf(Kept.Get()));
	TestFalse(TEXT("stopping the one left leaves nothing listening: the dropped one's listener went with it"), Object->IsRecordWatchedForTest(TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityWatchRecordReleaseTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.WatchRecordWatchersEndWithTheObject", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityWatchRecordReleaseTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!UseParityRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	ParityApply(Object, ParityRecord(TEXT("a"), 10, TEXT("x")));
	TStrongObjectPtr<UCrowdyServerObjectParityListener> Listener = MakeParityListener();
	Object->K2_WatchRecord(TEXT("a"), ParityRecordEventOf(Listener.Get()));
	TestEqual(TEXT("the watcher is kept"), Object->NumRecordEventWatchersForTest(), 1);
	Rig.Subsystem->Release(Object, Owner.Get());
	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestEqual(TEXT("the object is given back"), Object->GetStatus(), ECrowdyServerObjectStatus::Released);
	TestEqual(TEXT("which drops every record watcher"), Object->NumRecordEventWatchersForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityRecordsUpdatedTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.RecordsUpdatedNamesEachRecordsChangedVariables", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityRecordsUpdatedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!UseParityRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectParityListener> Listener = MakeParityListener();
	Object->RecordsUpdated.AddDynamic(Listener.Get(), &UCrowdyServerObjectParityListener::HandleUpdated);
	Object->RecordsChanged.AddDynamic(Listener.Get(), &UCrowdyServerObjectParityListener::HandleChanged);

	Object->ApplyRecordsForTest({ParityRecord(TEXT("a"), 10, TEXT("x")), ParityRecord(TEXT("b"), 20, TEXT("y"))}, {});
	if (!TestEqual(TEXT("two records arriving are announced once"), Listener->UpdatedChanges.Num(), 1) || !TestEqual(TEXT("with both"), Listener->UpdatedChanges[0].Num(), 2))
	{
		return false;
	}
	const FCrowdyServerRecordChange* A = ParityFindChange(Listener->UpdatedChanges[0], TEXT("a"));
	const FCrowdyServerRecordChange* B = ParityFindChange(Listener->UpdatedChanges[0], TEXT("b"));
	TestTrue(TEXT("a new record names every variable"), A && NamesAre(A->ChangedVariables, ParityRecordVariables()));
	TestTrue(TEXT("whichever it is"), B && NamesAre(B->ChangedVariables, ParityRecordVariables()));
	TestEqual(TEXT("none removed"), Listener->UpdatedRemoved[0].Num(), 0);
	TestEqual(TEXT("the existing On Records Changed still runs once"), Listener->ChangedIds.Num(), 1);
	TestEqual(TEXT("naming both ids"), Listener->ChangedIds[0].Num(), 2);

	ParityApply(Object, ParityRecord(TEXT("a"), 11, TEXT("x")));
	if (!TestEqual(TEXT("a change is announced"), Listener->UpdatedChanges.Num(), 2) || !TestEqual(TEXT("for the one record"), Listener->UpdatedChanges[1].Num(), 1))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("naming only the variable that changed (got %s)"), *Joined(Listener->UpdatedChanges[1][0].ChangedVariables)),
		Listener->UpdatedChanges[1][0].RecordId == TEXT("a") && NamesAre(Listener->UpdatedChanges[1][0].ChangedVariables, {TEXT("Health")}));

	ParityApply(Object, ParityRecord(TEXT("a"), 11, TEXT("x"), 99));
	if (!TestEqual(TEXT("a change of a variable nobody watches is announced"), Listener->UpdatedChanges.Num(), 3) || !TestEqual(TEXT("for the record"), Listener->UpdatedChanges[2].Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("naming no variable, as an unwatched one has no name"), Listener->UpdatedChanges[2][0].ChangedVariables.Num(), 0);

	Object->ApplyRecordsForTest({ParityRecord(TEXT("a"), 12, TEXT("x"), 99), ParityRecord(TEXT("b"), 20, TEXT("z"))}, {});
	if (!TestEqual(TEXT("two records changing together are announced once"), Listener->UpdatedChanges.Num(), 4))
	{
		return false;
	}
	const FCrowdyServerRecordChange* HealthOnly = ParityFindChange(Listener->UpdatedChanges[3], TEXT("a"));
	const FCrowdyServerRecordChange* LabelOnly = ParityFindChange(Listener->UpdatedChanges[3], TEXT("b"));
	TestTrue(TEXT("each naming its own changed variable: a its Health"), HealthOnly && NamesAre(HealthOnly->ChangedVariables, {TEXT("Health")}));
	TestTrue(TEXT("and b its Label"), LabelOnly && NamesAre(LabelOnly->ChangedVariables, {TEXT("Label")}));

	ParityRemove(Object, TEXT("b"));
	if (!TestEqual(TEXT("a removal is announced"), Listener->UpdatedChanges.Num(), 5))
	{
		return false;
	}
	TestEqual(TEXT("changing no record"), Listener->UpdatedChanges[4].Num(), 0);
	TestTrue(TEXT("and naming the one removed"), Listener->UpdatedRemoved[4] == TArray<FString>({TEXT("b")}));
	TestTrue(TEXT("the existing event names it too"), Listener->RemovedIds.Num() == 5 && Listener->RemovedIds[4] == TArray<FString>({TEXT("b")}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityCallTimeoutEndsAnUnansweredCallTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.CallTimeoutEndsACallTheServerDoesNotAnswer", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityCallTimeoutEndsAnUnansweredCallTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone(), 5.f);
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	Rig.Tick(4.f);
	TestEqual(TEXT("a call within its limit is still waiting"), Call.Num(), 0);
	Rig.Tick(1.5f);
	if (!TestEqual(TEXT("a call past its limit completes once"), Call.Num(), 1))
	{
		return false;
	}
	const FCrowdyServerCallResult& Result = (*Call.Results)[0];
	TestEqual(TEXT("as a Timeout"), static_cast<int32>(Result.Outcome), static_cast<int32>(ECrowdyServerCallOutcome::Timeout));
	TestEqualSensitive(TEXT("saying how long it waited and that the server may have run it"), Result.Reason,
		FString(TEXT("No answer within 5 seconds; AttackBoss may still have run on the server")));
	TestFalse(TEXT("not retryable, as the platform's own Timeout is not: it may already have run"), Result.bRetryable);

	FCrowdyServerObjectTestAttackReply Answer;
	Answer.HealthLeft = 2750;
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Answer));
	Rig.PumpFor(0.2);
	Rig.Tick(1.f);
	TestEqual(TEXT("an answer arriving later is ignored"), Call.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityHitchTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.ALongFrameCountsAsOneSecondOfACallsTimeout", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityHitchTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone(), 2.f);
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	Rig.Subsystem->TickForTest(3.f);
	TestEqual(TEXT("a three-second hitch does not time out a two-second call whose answer may be waiting unread"), Call.Num(), 0);
	Rig.Tick(1.5f);
	TestEqual(TEXT("the time after it still counts"), Call.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityLateCrashReadsAgainTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.ALateCrashAnswerStillReadsAgain", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityLateCrashReadsAgainTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone(), 1.f);
	FSentFrame Sent;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, Sent)))
	{
		return false;
	}
	Rig.Tick(1.5f);
	if (!TestEqual(TEXT("the call times out"), Call.Num(), 1))
	{
		return false;
	}
	Rig.Reply(Sent.Rid, ECrowdyNativeExecStatus::Trapped, {});
	FSentFrame Read;
	TestTrue(TEXT("a crash reported after the timeout still reads the values again, as they may have rolled back"), Rig.WaitForFrame(KindCall, TEXT("read"), Read));
	TestEqual(TEXT("and reports nothing more"), Call.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityCallTimeoutZeroKeepsTheConnectionsLimitTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.CallTimeoutOfZeroOrLessAddsNoLimit", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityCallTimeoutZeroKeepsTheConnectionsLimitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	FCallSpy Default;
	FCallSpy Zero;
	FCallSpy Negative;
	Object->Call(AttackFunction, AttackParams(1), Default.OnDone());
	Object->Call(AttackFunction, AttackParams(2), Zero.OnDone(), 0.f);
	Object->Call(AttackFunction, AttackParams(3), Negative.OnDone(), -4.f);
	Rig.Tick(25.f);
	TestEqual(TEXT("a call made without a timeout waits on"), Default.Num(), 0);
	TestEqual(TEXT("so does one given 0"), Zero.Num(), 0);
	TestEqual(TEXT("and one given less than 0"), Negative.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityCallTimeoutCountsWaitingToSendTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.CallTimeoutCountsWaitingForAConnection", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityCallTimeoutCountsWaitingToSendTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone(), 2.5f);
	Rig.Tick(2.f);
	TestEqual(TEXT("a call with no connection waits within its limit"), Call.Num(), 0);
	Rig.Tick(1.f);
	if (!TestEqual(TEXT("and times out at its limit, long before the 30 s a call waits to be sent"), Call.Num(), 1))
	{
		return false;
	}
	const FCrowdyServerCallResult& Result = (*Call.Results)[0];
	TestEqual(TEXT("as a Timeout, not Unavailable"), static_cast<int32>(Result.Outcome), static_cast<int32>(ECrowdyServerCallOutcome::Timeout));
	TestEqualSensitive(TEXT("with the seconds written without trailing zeros"), Result.Reason,
		FString(TEXT("No answer within 2.5 seconds; AttackBoss may still have run on the server")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityCallTimeoutCoversRetriesTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.CallTimeoutCoversTheWaitBeforeARetry", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityCallTimeoutCoversRetriesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.AcquireReady(Owner.Get(), Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	if (!Object)
	{
		return false;
	}
	FCallSpy Call;
	Object->Call(AttackFunction, AttackParams(250), Call.OnDone(), 1.2f);
	FSentFrame First;
	if (!TestTrue(TEXT("the call is sent"), Rig.WaitForFrame(KindCall, AttackMethod, First)))
	{
		return false;
	}
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Busy, Utf8Bytes(TEXT("rate limited: retry in 1500 ms")));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a busy reply is retried, not reported"), Call.Num(), 0);
	Rig.Tick(1.f);
	TestEqual(TEXT("within the limit it still waits to retry"), Call.Num(), 0);
	Rig.Tick(0.3f);
	if (!TestEqual(TEXT("a limit shorter than the retry wait ends the call before the retry"), Call.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("as a Timeout"), static_cast<int32>((*Call.Results)[0].Outcome), static_cast<int32>(ECrowdyServerCallOutcome::Timeout));
	TestFalse(TEXT("and the retry is never sent"), Rig.SentAny(KindCall, AttackMethod));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityCallTimeoutListOverloadTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.CallTimeoutAppliesToACallWithListParams", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityCallTimeoutListOverloadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	FCrowdyServerFunction& Attack = Rig.Definition->Functions[0];
	Attack.ParamsForm = ECrowdyServerValuesForm::List;
	Attack.ParamsList.AddProperty(TEXT("Damage"), EPropertyBagPropertyType::Int32);
	Attack.ParamsList.AddProperty(TEXT("Weapon"), EPropertyBagPropertyType::String);
	TArray<FString> Errors;
	if (!TestTrue(FString::Printf(TEXT("the List definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Rig.Definition->Bake(Errors)))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FCallSpy Call;
	Object->Call(AttackFunction, Object->MakeParams(AttackFunction), Call.OnDone(), 2.f);
	Rig.Tick(1.5f);
	TestEqual(TEXT("a List call within its limit is still waiting"), Call.Num(), 0);
	Rig.Tick(1.f);
	if (!TestEqual(TEXT("and times out past it"), Call.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("as a Timeout"), static_cast<int32>((*Call.Results)[0].Outcome), static_cast<int32>(ECrowdyServerCallOutcome::Timeout));
	TestEqualSensitive(TEXT("naming the limit"), (*Call.Results)[0].Reason, FString(TEXT("No answer within 2 seconds; AttackBoss may still have run on the server")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityListStateTypedReadTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.ListStateReadsTypedVariablesOnceReady", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityListStateTypedReadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	// A List travels like the struct of its values, so the messages are written before the definition becomes a List.
	const TArray<uint8> FirstRead = Rig.ReadMessage(5, Boss(3000, true, ECrowdyExecTestPhase::Enraged));
	const TArray<uint8> Push = Rig.PushMessage(Epoch, 6, Boss(2500, true, ECrowdyExecTestPhase::Final), {TEXT("Health"), TEXT("Phase")});
	if (!UseParityListState(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	UCrowdyServerObjectDefinition* Definition = Rig.Definition.Get();
	FCrowdyServerObjectTestPins Pins;
	Pins.Count = 7;
	TestFalse(TEXT("a List's variable is not read before the first read, when it holds only defaults"), ParityRead(Object, Definition, TEXT("Health"), Pins, TEXT("Count")));

	if (!ParityAnswerRead(Rig, FirstRead) || !TestTrue(TEXT("the List State becomes Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	Pins.Count = 0;
	TestTrue(TEXT("an int32 of a List is read"), ParityRead(Object, Definition, TEXT("Health"), Pins, TEXT("Count")));
	TestEqual(TEXT("into the pin"), Pins.Count, 3000);
	Pins.Count = 0;
	TestTrue(TEXT("whatever the name's case"), ParityRead(Object, Definition, TEXT("HEALTH"), Pins, TEXT("Count")) && Pins.Count == 3000);
	TestTrue(TEXT("a bool of a List is read"), ParityRead(Object, Definition, TEXT("bDefeated"), Pins, TEXT("bFlag")));
	TestTrue(TEXT("into the pin"), Pins.bFlag);
	TestTrue(TEXT("an enum of a List is read"), ParityRead(Object, Definition, TEXT("Phase"), Pins, TEXT("Phase")));
	TestEqual(TEXT("into the pin"), static_cast<int32>(Pins.Phase), static_cast<int32>(ECrowdyExecTestPhase::Enraged));

	TestFalse(TEXT("a name the List lacks is not read"), ParityRead(Object, Definition, TEXT("Nope"), Pins, TEXT("Count")));
	TestFalse(TEXT("nor an int32 into a string pin"), ParityRead(Object, Definition, TEXT("Health"), Pins, TEXT("Label")));
	TestFalse(TEXT("nor one of another definition's object"), ParityRead(Object, NewObject<UCrowdyServerObjectDefinition>(), TEXT("Health"), Pins, TEXT("Count")));

	Rig.Push(Push);
	TestTrue(TEXT("a push reaches the List"), Rig.PumpUntil([&]() { return ParityRead(Object, Definition, TEXT("Health"), Pins, TEXT("Count")) && Pins.Count == 2500; }));
	TestTrue(TEXT("changing the enum too"), ParityRead(Object, Definition, TEXT("Phase"), Pins, TEXT("Phase")) && Pins.Phase == ECrowdyExecTestPhase::Final);
	TestTrue(TEXT("and leaving a variable the push left out"), ParityRead(Object, Definition, TEXT("bDefeated"), Pins, TEXT("bFlag")) && Pins.bFlag);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectParityListStateBindingTest,
	"CrowdySDK.CrowdyExec.ServerObjectParity.ListStateBindingRunsNowAndOnEachChangeOfItsVariable", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectParityListStateBindingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectParityTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	const TArray<uint8> FirstRead = Rig.ReadMessage(5, Boss(3000, false, ECrowdyExecTestPhase::Calm));
	const TArray<uint8> HealthPush = Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), {TEXT("Health")});
	const TArray<uint8> PhasePush = Rig.PushMessage(Epoch, 7, Boss(2500, false, ECrowdyExecTestPhase::Enraged), {TEXT("Phase")});
	const TArray<uint8> LaterPush = Rig.PushMessage(Epoch, 8, Boss(2000, false, ECrowdyExecTestPhase::Enraged), {TEXT("Health")});
	if (!UseParityListState(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	UCrowdyServerObjectDefinition* Definition = Rig.Definition.Get();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Early = MakeParityIntListener();
	UCrowdyServerObjectLibrary::BindServerVariableChanged(Object, Definition, TEXT("Health"), Early.Get(), TEXT("HandleInt"));
	TestEqual(TEXT("a binding made before the first read does not run"), Early->IntValues.Num(), 0);

	if (!ParityAnswerRead(Rig, FirstRead) || !TestTrue(TEXT("the List State becomes Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	Rig.PumpFor(0.1);
	TestTrue(TEXT("the first read runs it with the List's value"), Early->IntValues == TArray<int32>({3000}));

	TStrongObjectPtr<UCrowdyServerObjectTestListener> Late = MakeParityIntListener();
	UCrowdyServerObjectLibrary::BindServerVariableChanged(Object, Definition, TEXT("health"), Late.Get(), TEXT("HandleInt"));
	TestTrue(TEXT("a binding made once Ready runs at once, whatever its name's case"), Late->IntValues == TArray<int32>({3000}));
	TestEqual(TEXT("without running the other"), Early->IntValues.Num(), 1);

	Rig.Push(HealthPush);
	TestTrue(TEXT("a push of the variable runs both"), Rig.PumpUntil([&Early, &Late]() { return Early->IntValues.Num() >= 2 && Late->IntValues.Num() >= 2; }));
	Rig.PumpFor(0.1);
	TestTrue(TEXT("with the new value, once each"), Early->IntValues == TArray<int32>({3000, 2500}) && Late->IntValues == TArray<int32>({3000, 2500}));

	Rig.Push(PhasePush);
	TestTrue(TEXT("a push of another variable arrives"), Rig.PumpUntil([Object]()
	{
		const TValueOrError<ECrowdyExecTestPhase, EPropertyBagResult> Phase = Object->GetStateList().GetValueEnum<ECrowdyExecTestPhase>(TEXT("Phase"));
		return Phase.HasValue() && Phase.GetValue() == ECrowdyExecTestPhase::Enraged;
	}));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("and runs neither"), Early->IntValues.Num() + Late->IntValues.Num(), 4);

	UCrowdyServerObjectLibrary::UnbindServerVariableChanged(Object, Definition, TEXT("Health"), Late.Get(), TEXT("HandleInt"));
	Rig.Push(LaterPush);
	TestTrue(TEXT("a binding still held runs on the next push"), Rig.PumpUntil([&Early]() { return Early->IntValues.Num() >= 3; }));
	Rig.PumpFor(0.1);
	TestTrue(TEXT("with its value"), Early->IntValues == TArray<int32>({3000, 2500, 2000}));
	TestEqual(TEXT("an unbound handler is not run"), Late->IntValues.Num(), 2);
	return true;
}

#endif
