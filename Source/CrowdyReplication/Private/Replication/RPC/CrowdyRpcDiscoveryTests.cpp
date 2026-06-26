#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/CrowdyRpcTestTarget.h"
#include "Utils/CrowdyBakedRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyRpcDiscoveryTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Mirrors UCrowdyRegistryBaker::MakeBakedRpcEntry so the test bakes a function
	// exactly as the cook would, then reads it back through the runtime lookup.
	FCrowdyBakedRpcFunction BakeEntry(UFunction* Function)
	{
		const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Function);

		FCrowdyBakedRpcFunction Entry;
		Entry.ClassPath    = FSoftClassPath(Function->GetOwnerClass());
		Entry.FunctionName = Function->GetFName();
		Entry.FunctionID   = Info.FunctionID;
		Entry.Recipient    = Info.Recipient;
		Entry.DecayRate    = Info.DecayRate;
		Entry.Distance     = Info.Distance;
		Entry.bParamsPOD   = Info.bParamsPOD;
		return Entry;
	}
}

// Deliverable 2: routing meta strings resolve to the right enum values, and a
// function with no routing meta keeps the API defaults.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcRoutingMetaResolvedTest,
	"CrowdySDK.RPC.RoutingMetaResolved", CrowdyRpcDiscoveryTestFlags)
bool FCrowdyRpcRoutingMetaResolvedTest::RunTest(const FString& Parameters)
{
	UFunction* NoArgs = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("NoArgs_Implementation"));
	TestNotNull(TEXT("NoArgs resolved"), NoArgs);
	if (!NoArgs)
	{
		return false;
	}

	// No routing meta yet: the defaults from FCrowdyFnInfo must come through.
	const FCrowdyFnInfo Defaults = FCrowdyRPC::BuildFnInfo(NoArgs);
	TestTrue(TEXT("default recipient"), Defaults.Recipient == ECrowdyEventRecipient::SpatialMulticast);
	TestTrue(TEXT("default decay"), Defaults.DecayRate == ECrowdyDecayRate::No_Decay);
	TestTrue(TEXT("default distance"), Defaults.Distance == ECrowdyReplicationDistance::Eight_Chunks);

#if WITH_METADATA
	// Stamp the same keys the CROWDY_EVENT macro emits, with values that differ
	// from every default so a pass proves resolution rather than fall-through.
	NoArgs->SetMetaData(CrowdyRpcMetaKeys::Recipient, TEXT("OwningClient"));
	NoArgs->SetMetaData(CrowdyRpcMetaKeys::Decay, TEXT("Exponential_Decay"));
	NoArgs->SetMetaData(CrowdyRpcMetaKeys::Distance, TEXT("Four_Chunks"));

	const FCrowdyFnInfo Resolved = FCrowdyRPC::BuildFnInfo(NoArgs);
	TestTrue(TEXT("recipient resolved"), Resolved.Recipient == ECrowdyEventRecipient::OwningClient);
	TestTrue(TEXT("decay resolved"), Resolved.DecayRate == ECrowdyDecayRate::Exponential_Decay);
	TestTrue(TEXT("distance resolved"), Resolved.Distance == ECrowdyReplicationDistance::Four_Chunks);

	NoArgs->RemoveMetaData(CrowdyRpcMetaKeys::Recipient);
	NoArgs->RemoveMetaData(CrowdyRpcMetaKeys::Decay);
	NoArgs->RemoveMetaData(CrowdyRpcMetaKeys::Distance);
#endif

	return true;
}

// Deliverable 3 + parity exit test: baking a function and reading it back through
// the runtime lookup yields the same id/routing/POD the runtime computes live.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcBakedRoundTripTest,
	"CrowdySDK.RPC.BakedRoundTrip", CrowdyRpcDiscoveryTestFlags)
bool FCrowdyRpcBakedRoundTripTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyRpcTestTarget::StaticClass();
	UFunction* Structs = Class->FindFunctionByName(TEXT("Structs_Implementation"));
	UFunction* Primitives = Class->FindFunctionByName(TEXT("Primitives_Implementation"));
	UFunction* NoArgs = Class->FindFunctionByName(TEXT("NoArgs_Implementation"));
	TestNotNull(TEXT("Structs resolved"), Structs);
	TestNotNull(TEXT("Primitives resolved"), Primitives);
	TestNotNull(TEXT("NoArgs resolved"), NoArgs);
	if (!Structs || !Primitives || !NoArgs)
	{
		return false;
	}

	UCrowdyBakedRegistry* Registry = NewObject<UCrowdyBakedRegistry>();
	TestNotNull(TEXT("registry created"), Registry);
	Registry->RpcFunctions.Add(BakeEntry(Structs));
	Registry->RpcFunctions.Add(BakeEntry(Primitives));
	Registry->RpcFunctions.Add(BakeEntry(NoArgs));

	const FSoftClassPath Path(Class);

	const FCrowdyBakedRpcFunction* Found = Registry->FindRpcFunction(Path, FName(TEXT("Structs_Implementation")));
	TestNotNull(TEXT("baked Structs found"), Found);
	if (Found)
	{
		// Editor<->cooked parity: the frozen id equals the live-computed id.
		TestEqual(TEXT("baked id matches live"), Found->FunctionID, FCrowdyRPC::ComputeFunctionID(Structs));
	}

	// POD flag survives the bake: a no-arg signature is POD, an FString-bearing one is not.
	const FCrowdyBakedRpcFunction* BakedNoArgs = Registry->FindRpcFunction(Path, FName(TEXT("NoArgs_Implementation")));
	const FCrowdyBakedRpcFunction* BakedPrimitives = Registry->FindRpcFunction(Path, FName(TEXT("Primitives_Implementation")));
	TestNotNull(TEXT("baked NoArgs found"), BakedNoArgs);
	TestNotNull(TEXT("baked Primitives found"), BakedPrimitives);
	if (BakedNoArgs) TestTrue(TEXT("no-arg signature is POD"), BakedNoArgs->bParamsPOD);
	if (BakedPrimitives) TestFalse(TEXT("FString signature is not POD"), BakedPrimitives->bParamsPOD);

	// A name that was never baked must miss.
	TestNull(TEXT("unknown function misses"),
		Registry->FindRpcFunction(Path, FName(TEXT("DoesNotExist_Implementation"))));

	return true;
}

// Phase 6 deliverable 1 + exit test: the signature validator accepts supported
// signatures and rejects a return value, an output parameter, and an unsupported
// parameter type, each with a readable message that names the offending parameter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcSignatureValidationTest,
	"CrowdySDK.RPC.SignatureValidation", CrowdyRpcDiscoveryTestFlags)
bool FCrowdyRpcSignatureValidationTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyRpcTestTarget::StaticClass();

	const auto Resolve = [Class](const TCHAR* Name) -> UFunction*
	{
		return Class->FindFunctionByName(Name);
	};

	// Supported signatures — primitives, structs, and no-arg — must pass.
	for (const TCHAR* Name : { TEXT("Primitives_Implementation"),
		TEXT("Structs_Implementation"), TEXT("NoArgs_Implementation") })
	{
		UFunction* Fn = Resolve(Name);
		TestNotNull(FString::Printf(TEXT("%s resolved"), Name), Fn);
		if (Fn)
		{
			TestEqual(FString::Printf(TEXT("%s is valid"), Name),
				FCrowdyRPC::DescribeSignatureProblem(Fn), FString());
		}
	}

	// A return value is rejected.
	if (UFunction* ReturnsValue = Resolve(TEXT("ReturnsValue_Implementation")))
	{
		const FString Problem = FCrowdyRPC::DescribeSignatureProblem(ReturnsValue);
		TestFalse(TEXT("return value rejected"), Problem.IsEmpty());
		TestTrue(TEXT("return-value message mentions returning"), Problem.Contains(TEXT("return")));
	}

	// A non-const output reference is rejected and named.
	if (UFunction* OutParam = Resolve(TEXT("OutParam_Implementation")))
	{
		const FString Problem = FCrowdyRPC::DescribeSignatureProblem(OutParam);
		TestFalse(TEXT("out param rejected"), Problem.IsEmpty());
		TestTrue(TEXT("out-param message names the parameter"), Problem.Contains(TEXT("OutResult")));
	}

	// An object reference is a supported parameter type as of Tier 2.
	if (UFunction* ObjectParam = Resolve(TEXT("ObjectParam_Implementation")))
	{
		TestEqual(TEXT("object param is valid"),
			FCrowdyRPC::DescribeSignatureProblem(ObjectParam), FString());
	}

	return true;
}

// Containers: a container of supported leaf types is a valid signature; an array of object
// references is valid (Tier 3); a set or map of object references is rejected and names the
// parameter; and the canonical type recurses into the inner type so two arrays that differ only by
// element type never collapse to the same FunctionID.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcContainerSignatureTest,
	"CrowdySDK.RPC.ContainerSignature", CrowdyRpcDiscoveryTestFlags)
bool FCrowdyRpcContainerSignatureTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyRpcTestTarget::StaticClass();

	// Collects the input parameter properties of a function in declaration order.
	const auto InputParams = [](UFunction* Fn) -> TArray<FProperty*>
	{
		TArray<FProperty*> Params;
		if (Fn)
		{
			for (TFieldIterator<FProperty> It(Fn); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Parm) && !FCrowdyRPC::IsTrueOutputParam(*It))
				{
					Params.Add(*It);
				}
			}
		}
		return Params;
	};

	// Containers of supported leaf types are valid.
	UFunction* Containers = Class->FindFunctionByName(TEXT("Containers_Implementation"));
	TestNotNull(TEXT("Containers resolved"), Containers);
	if (Containers)
	{
		TestEqual(TEXT("containers of leaf types are valid"),
			FCrowdyRPC::DescribeSignatureProblem(Containers), FString());
	}

	// Tier 3: an array of object references is now a valid signature the per-element codec encodes
	// each by identity so what Tier 1 rejected here is accepted.
	if (UFunction* ObjectArray = Class->FindFunctionByName(TEXT("ObjectArray_Implementation")))
	{
		TestEqual(TEXT("array of objects is valid"),
			FCrowdyRPC::DescribeSignatureProblem(ObjectArray), FString());
	}

	// A set or map of object references stays rejected (object key/value hashing is out of scope),
	// with a message that names the parameter and the limitation.
	if (UFunction* ObjectMap = Class->FindFunctionByName(TEXT("ObjectMap_Implementation")))
	{
		const FString Problem = FCrowdyRPC::DescribeSignatureProblem(ObjectMap);
		TestFalse(TEXT("map of objects rejected"), Problem.IsEmpty());
		TestTrue(TEXT("message names the parameter"), Problem.Contains(TEXT("InMap")));
		TestTrue(TEXT("message names the set/map limitation"), Problem.Contains(TEXT("set or map")));
	}

	// Regression for the collision the recursion fixes: the int-array and vector-array
	// parameters of Containers must canonicalize to different strings.
	const TArray<FProperty*> Params = InputParams(Containers);
	TestEqual(TEXT("three container params"), Params.Num(), 3);
	if (Params.Num() == 3)
	{
		const FString IntArrayType = FCrowdyRPC::CanonicalParamType(Params[0]);
		const FString VecArrayType = FCrowdyRPC::CanonicalParamType(Params[1]);
		TestNotEqual(TEXT("array element type participates in the canonical type"),
			IntArrayType, VecArrayType);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
