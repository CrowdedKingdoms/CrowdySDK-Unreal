#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Replication/Executor/ActorUpdateExecutor.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UActorUpdatePayloadRegistry.h"

// Covers the executor the SDK ships so a project gets a replicated actor without authoring a state
// struct or an executor class. The struct it declares is now a wire type like any other, so it is
// exercised through the real serializer rather than only inspected in memory.
namespace
{
	constexpr EAutomationTestFlags CrowdyDefaultExecutorTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Any non-zero id. These tests pin the struct and the executor, not which class sent a frame; zero is
	// the one class id the actor-state framing refuses outright.
	constexpr FCrowdyClassID DefaultExecutorTestClassID = 0xDEFA0175u;
}

// The type the executor declares is what registers it for wire serialization, so a wrong or null answer
// here is an entity that replicates nothing while looking configured.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDefaultExecutorDeclaresItsStructTest,
	"CrowdySDK.Entity.DefaultExecutorDeclaresItsStruct", CrowdyDefaultExecutorTestFlags)
bool FCrowdyDefaultExecutorDeclaresItsStructTest::RunTest(const FString& Parameters)
{
	UCrowdyDefaultActorUpdateExecutor* Executor = NewObject<UCrowdyDefaultActorUpdateExecutor>();

	TestEqual(TEXT("the SDK default executor declares FCrowdyActorState"),
		Executor->GetStateStruct_Implementation(), FCrowdyActorState::StaticStruct());

	return true;
}

// The guard that matters most in this class, and the one whose absence would be silent. The replicator
// compares each snapshot against the last one it sent, so returning a default-constructed
// FCrowdyActorState for an owner-less component would read as an actor that had genuinely moved to the
// world origin, and it would be sent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDefaultExecutorRefusesOwnerlessSnapshotTest,
	"CrowdySDK.Entity.DefaultExecutorRefusesOwnerlessSnapshot", CrowdyDefaultExecutorTestFlags)
bool FCrowdyDefaultExecutorRefusesOwnerlessSnapshotTest::RunTest(const FString& Parameters)
{
	UCrowdyDefaultActorUpdateExecutor* Executor = NewObject<UCrowdyDefaultActorUpdateExecutor>();

	const FInstancedStruct Snapshot = Executor->GetActorState_Implementation(nullptr);

	TestFalse(TEXT("a component with no owner yields no snapshot at all, not a zeroed transform"),
		Snapshot.IsValid());

	return true;
}

// The struct the SDK now ships has to survive the same framing every authored struct does. Asserted
// against authored literals rather than against a second encode, so a defect shared by the writer and
// the reader cannot pass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDefaultActorStateRoundTripsTest,
	"CrowdySDK.Entity.DefaultActorStateRoundTrips", CrowdyDefaultExecutorTestFlags)
bool FCrowdyDefaultActorStateRoundTripsTest::RunTest(const FString& Parameters)
{
	UActorUpdatePayloadRegistry::Get()->RegisterStructAuto(FCrowdyActorState::StaticStruct());

	FCrowdyActorState State;
	State.Location = FVector(-1234.5, 6789.0, 42.25);
	State.Rotation = FRotator(10.0, -20.0, 30.0);

	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("the SDK default state serializes"),
		USerializationFunctionLibrary::SerializeActorState(FInstancedStruct::Make(State), DefaultExecutorTestClassID, Bytes)))
	{
		return false;
	}

	FInstancedStruct Decoded;
	FCrowdyTypeID DecodedTypeID = CROWDY_INVALID_TYPE_ID;
	FCrowdyClassID DecodedClassID = CROWDY_INVALID_CLASS_ID;
	if (!TestTrue(TEXT("and decodes again"),
		USerializationFunctionLibrary::DeserializeActorState(Bytes, Decoded, DecodedTypeID, DecodedClassID)))
	{
		return false;
	}

	const FCrowdyActorState* Result = Decoded.GetPtr<FCrowdyActorState>();
	if (!TestNotNull(TEXT("as its own struct type"), Result))
	{
		return false;
	}

	TestEqual(TEXT("location survives"), Result->Location, State.Location);
	TestEqual(TEXT("rotation survives"), Result->Rotation, State.Rotation);

	// The reason this struct is safe to share across every entity in a project at all: the class rides
	// the message, so a shared struct no longer forces a shared class.
	TestEqual(TEXT("and the class the frame named is recovered independently of the struct"),
		static_cast<int64>(DecodedClassID), static_cast<int64>(DefaultExecutorTestClassID));

	return true;
}

// Velocity is deliberately absent, and this pins the decision rather than leaving it to be rediscovered
// as an oversight. Carrying it raw would cost 24 bytes on every update from every entity to hold
// something nothing reads yet, and the quantized form it wants is a wire break that the drain
// measurement has not justified. The framing tolerates appended fields, so adding it later is free.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDefaultActorStateCarriesOnlyATransformTest,
	"CrowdySDK.Entity.DefaultActorStateCarriesOnlyATransform", CrowdyDefaultExecutorTestFlags)
bool FCrowdyDefaultActorStateCarriesOnlyATransformTest::RunTest(const FString& Parameters)
{
	int32 PropertyCount = 0;
	for (TFieldIterator<FProperty> It(FCrowdyActorState::StaticStruct()); It; ++It)
	{
		++PropertyCount;
	}

	TestEqual(TEXT("FCrowdyActorState carries exactly Location and Rotation"), PropertyCount, 2);

	return true;
}

// The replication loop skips the BlueprintNativeEvent thunk for an executor that does not override the
// event, so a wrong answer here either costs the saving it exists for or, the dangerous direction, calls
// _Implementation on an executor whose real answer lives in a Blueprint graph. Both answers are pinned.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecutorNativeStatePathTest,
	"CrowdySDK.Entity.ExecutorResolvesTheNativeStatePath", CrowdyDefaultExecutorTestFlags)
bool FCrowdyExecutorNativeStatePathTest::RunTest(const FString& Parameters)
{
	UCrowdyDefaultActorUpdateExecutor* Executor = NewObject<UCrowdyDefaultActorUpdateExecutor>();

	TestTrue(TEXT("an executor with no Blueprint override answers natively"),
		CrowdyActorUpdateExecutor::AnswersStateNatively(Executor));
	TestFalse(TEXT("and a null executor answers no"),
		CrowdyActorUpdateExecutor::AnswersStateNatively(nullptr));

	const UFunction* const NativeEntry = Executor->GetClass()->FindFunctionByName(TEXT("GetActorState"));
	if (TestNotNull(TEXT("the event resolves to a function at all"), NativeEntry))
	{
		TestTrue(TEXT("which is the native one the C++ declaration produced"),
			CrowdyActorUpdateExecutor::IsNativeStateFunction(NativeEntry));
	}

	// A Blueprint override of the event is a script function on the generated class. One is stood up here
	// rather than loaded, because a headless run has no Blueprint asset to override it with, and without a
	// negative case the predicate could answer "native" unconditionally and still read green.
	const TStrongObjectPtr<UFunction> ScriptEntry(
		NewObject<UFunction>(GetTransientPackage(), TEXT("CrowdyExecutorScriptStateEntry")));
	TestFalse(TEXT("a script function is not the native entry"),
		CrowdyActorUpdateExecutor::IsNativeStateFunction(ScriptEntry.Get()));
	TestFalse(TEXT("and neither is nothing at all"),
		CrowdyActorUpdateExecutor::IsNativeStateFunction(nullptr));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
