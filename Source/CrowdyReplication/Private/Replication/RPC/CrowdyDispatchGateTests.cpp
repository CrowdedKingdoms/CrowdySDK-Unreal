// WITH_METADATA as well as the test guard: these cases build their fixtures by writing UFunction metadata,
// and UField::SetMetaData does not exist without it. WITH_DEV_AUTOMATION_TESTS alone is still 1 in a
// Development non-editor build, where this file would then fail to compile.
#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Misc/AutomationTest.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/CrowdyReplicatedEventLibrary.h"
#include "Replication/RPC/CrowdyRpcTestTarget.h"
#include "UObject/StrongObjectPtr.h"

/**
 * A Blueprint "Crowdy Replicates" event replicates because the compiler extension splices a call to
 * UCrowdyReplicatedEventLibrary::CrowdyDispatchReplicatedEvent into the head of its compiled body. An event
 * that lost that gate still LOOKS entirely healthy: it resolves, it carries its routing metadata, it
 * registers, and it receives. What it does not do is send, and it enters no send code at all while failing
 * to, so nothing reports a drop and nothing logs.
 *
 * That failure cost an afternoon of a live two-client session before anything could even name it, which is
 * why the predicate these cases cover exists at all.
 */
namespace
{
	constexpr EAutomationTestFlags CrowdyDispatchGateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	UFunction* GateFunction()
	{
		return UCrowdyReplicatedEventLibrary::StaticClass()->FindFunctionByName(
			GET_FUNCTION_NAME_CHECKED(UCrowdyReplicatedEventLibrary, CrowdyDispatchReplicatedEvent));
	}

	/**
	 * A function on a Blueprint-generated class, standing in for a compiled event.
	 *
	 * A test cannot author a Blueprint graph, so the two things the predicates actually read are built
	 * directly: the owner being a UBlueprintGeneratedClass, and the compiled body's object-reference list.
	 * That list is what the engine fills from a real compile, so a synthetic entry is the same input the
	 * real one produces rather than a stand-in for it.
	 */
	struct FSyntheticEvent
	{
		TStrongObjectPtr<UClass> Class;
		TStrongObjectPtr<UFunction> Function;

		FSyntheticEvent(const TCHAR* Name, const bool bReplicates, const bool bHasScript, const bool bHasGate,
			const bool bBlueprintClass = true)
		{
			// A UFunction's owner class is its outer, so parenting it under one kind of class or the other is
			// the whole of what IsBlueprintReplicatedEvent reads.
			Class.Reset(bBlueprintClass
				? static_cast<UClass*>(NewObject<UBlueprintGeneratedClass>(GetTransientPackage()))
				: NewObject<UClass>(GetTransientPackage()));
			Function.Reset(NewObject<UFunction>(Class.Get(), Name));

			if (bReplicates)
			{
				Function->SetMetaData(CrowdyRpcMetaKeys::Replicates, TEXT(""));
			}

			// Any non-empty script stands for a compiled body: the predicate reads the reference list, not
			// the opcodes, but it refuses a function with no body at all and that refusal is asserted below.
			if (bHasScript)
			{
				Function->Script.Add(0);
			}

			if (bHasGate)
			{
				Function->ScriptAndPropertyObjectReferences.Add(GateFunction());
			}
		}
	};
}

// The pair the whole diagnostic rests on: a compiled body that calls the gate, and one that does not. Both
// are marked replicated and both are on a Blueprint class, so the gate is the only thing that differs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDispatchGateDetectedAndMissingTest,
	"CrowdySDK.RPC.DispatchGateDetectedAndMissing", CrowdyDispatchGateTestFlags)
bool FCrowdyDispatchGateDetectedAndMissingTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("the dispatch gate function resolves"), GateFunction()))
	{
		return false;
	}

	const FSyntheticEvent Gated(TEXT("GatedEvent"), /*bReplicates*/true, /*bHasScript*/true, /*bHasGate*/true);
	const FSyntheticEvent Ungated(TEXT("UngatedEvent"), /*bReplicates*/true, /*bHasScript*/true, /*bHasGate*/false);

	// Asserted together, because either alone is satisfied by a predicate that always answers the same way.
	TestTrue(TEXT("a body that calls the gate is recognised as carrying it"),
		FCrowdyRPC::CarriesDispatchGate(Gated.Function.Get()));
	TestFalse(TEXT("a body that does not call the gate is recognised as missing it"),
		FCrowdyRPC::CarriesDispatchGate(Ungated.Function.Get()));

	// Both are events the check applies to, which is what makes the false above a REPORTABLE fault rather
	// than a function the sweep would have skipped anyway.
	TestTrue(TEXT("the gated one is a Blueprint replicated event"),
		FCrowdyRPC::IsBlueprintReplicatedEvent(Gated.Function.Get()));
	TestTrue(TEXT("and so is the ungated one"),
		FCrowdyRPC::IsBlueprintReplicatedEvent(Ungated.Function.Get()));

	TestFalse(TEXT("a null function carries no gate"), FCrowdyRPC::CarriesDispatchGate(nullptr));

	return true;
}

// The near misses the sweep must stay silent about, and there are two independent ones. Reporting either
// would name healthy events as broken, and a project's whole C++ event surface is the first of them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDispatchGateIgnoresNativeAndUnmarkedTest,
	"CrowdySDK.RPC.DispatchGateIgnoresNativeAndUnmarked", CrowdyDispatchGateTestFlags)
bool FCrowdyDispatchGateIgnoresNativeAndUnmarkedTest::RunTest(const FString& Parameters)
{
	UFunction* NativeEvent = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("NoArgs_Implementation"));
	if (!TestNotNull(TEXT("a real native CrowdyEvent resolves"), NativeEvent))
	{
		return false;
	}

	// A C++ CROWDY_EVENT is marked by the macro as a CrowdyEvent and never carries CrowdyReplicates, which
	// the compiler extension stamps on Blueprint events alone. So a native event is excluded by the marker
	// before the class kind is ever consulted, and this records which of the two answers it.
	TestFalse(TEXT("sanity: a native CrowdyEvent does not carry the Blueprint replicates marker"),
		CrowdyRpcMetaKeys::HasReplicatesMeta(NativeEvent));
	TestFalse(TEXT("so a native CrowdyEvent is never asked for a gate"),
		FCrowdyRPC::IsBlueprintReplicatedEvent(NativeEvent));

	// The class-kind half on its own: marked replicated, but declared on a native class. It cannot be
	// reached by authoring today, which is exactly why it is built here rather than found; if hand-written
	// C++ metadata ever spells CrowdyReplicates, the sweep must still not demand a gate of it.
	const FSyntheticEvent NativeMarked(TEXT("NativeMarkedEvent"), /*bReplicates*/true, /*bHasScript*/true,
		/*bHasGate*/false, /*bBlueprintClass*/false);
	TestTrue(TEXT("sanity: the native-owned fixture really does carry the marker"),
		CrowdyRpcMetaKeys::HasReplicatesMeta(NativeMarked.Function.Get()));
	TestFalse(TEXT("a marked event on a NATIVE class is still not a Blueprint replicated event"),
		FCrowdyRPC::IsBlueprintReplicatedEvent(NativeMarked.Function.Get()));

	const FSyntheticEvent Unmarked(TEXT("PlainEvent"), /*bReplicates*/false, /*bHasScript*/true, /*bHasGate*/false);
	TestFalse(TEXT("an unmarked Blueprint function is not a replicated event"),
		FCrowdyRPC::IsBlueprintReplicatedEvent(Unmarked.Function.Get()));

	TestFalse(TEXT("nothing at all is not a replicated event"),
		FCrowdyRPC::IsBlueprintReplicatedEvent(nullptr));

	return true;
}

// A function with no compiled body carries no gate, and must not read as one that does. This is the case a
// predicate written as "the reference list does not contain the gate" gets right by accident and one
// written as "the body was compiled without it" would get wrong.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDispatchGateNeedsACompiledBodyTest,
	"CrowdySDK.RPC.DispatchGateNeedsACompiledBody", CrowdyDispatchGateTestFlags)
bool FCrowdyDispatchGateNeedsACompiledBodyTest::RunTest(const FString& Parameters)
{
	const FSyntheticEvent Bodyless(TEXT("BodylessEvent"), /*bReplicates*/true, /*bHasScript*/false, /*bHasGate*/true);

	// The reference list names the gate, and there is still no body to have called it from. The reference
	// list alone would say yes here, which is why the script check is asked first.
	TestFalse(TEXT("a function with no compiled body carries no gate, whatever its reference list says"),
		FCrowdyRPC::CarriesDispatchGate(Bodyless.Function.Get()));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
