#include "Replication/RPC/CrowdyRpcTestTarget.h"

#include "GameFramework/Actor.h"

void UCrowdyRpcTestTarget::Primitives_Implementation(bool bInFlag, int32 InI32, int64 InI64, float InF, double InD,
	const FString& InStr, FName InName)
{
	++CallCount;
	GotFlag = bInFlag;
	GotI32 = InI32;
	GotI64 = InI64;
	GotF = InF;
	GotD = InD;
	GotStr = InStr;
	GotName = InName;
}

void UCrowdyRpcTestTarget::Structs_Implementation(FVector InVector, FRotator InRotator, FTransform InTransform)
{
	++CallCount;
	GotVector = InVector;
	GotRotator = InRotator;
	GotTransform = InTransform;
}

void UCrowdyRpcTestTarget::NoArgs_Implementation()
{
	++CallCount;
}

void UCrowdyRpcTestTarget::Reordered_Implementation(int32 InI32, bool bInFlag)
{
	++CallCount;
	GotI32 = InI32;
	GotFlag = bInFlag;
}

void UCrowdyRpcTestTarget::MacroEvent_Implementation(int32 InAmmo, FVector InDir)
{
	++CallCount;
	GotI32 = InAmmo;
	GotVector = InDir;
}

bool UCrowdyRpcTestTarget::ReturnsValue_Implementation(int32 InValue)
{
	return InValue != 0;
}

void UCrowdyRpcTestTarget::OutParam_Implementation(int32 InValue, int32& OutResult)
{
	OutResult = InValue;
}

void UCrowdyRpcTestTarget::ObjectParam_Implementation(UObject* InObject)
{
	++CallCount;
	GotObject = InObject;
}

void UCrowdyRpcTestTarget::Containers_Implementation(const TArray<int32>& InInts, const TArray<FVector>& InVecs,
	const TMap<FName, int32>& InMap)
{
	++CallCount;
	GotInts = InInts;
	GotVecs = InVecs;
	GotMap = InMap;
}

void UCrowdyRpcTestTarget::ObjectArray_Implementation(const TArray<UObject*>& InObjects)
{
}

void UCrowdyRpcTestTarget::SingleIntArray_Implementation(const TArray<int32>& In)
{
	++CallCount;
	GotInts = In;
}

void UCrowdyRpcTestTarget::ClassRef_Implementation(UClass* InClass)
{
	++CallCount;
	GotClass = InClass;
}

void UCrowdyRpcTestTarget::ActorRef_Implementation(AActor* InActor)
{
	++CallCount;
	GotObject = InActor;
}

void UCrowdyRpcTestTarget::ClassArray_Implementation(const TArray<UClass*>& InClasses)
{
	++CallCount;
	GotClasses = InClasses;
}

void UCrowdyRpcTestTarget::ActorArray_Implementation(const TArray<AActor*>& InActors)
{
	++CallCount;
	GotActors = InActors;
}

void UCrowdyRpcTestTarget::ObjectMap_Implementation(const TMap<FName, AActor*>& InMap)
{
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Serialization/MemoryWriter.h" // hand-crafting an untrusted blob
#include "UObject/Stack.h" // FOutParmRec

namespace
{
	constexpr EAutomationTestFlags CrowdyRpcTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The marshal side (typed args into FCrowdyRpcCall) and the apply side (call into a frame, then
// ProcessEvent) are exercised back-to-back so the round-trip covers exactly what the wire carries
// between them. SendChecked itself now routes over the transport, so it can no longer stand in for
// the receive side the way it did before transport existed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcPrimitiveRoundTripTest,
	"CrowdySDK.RPC.PrimitiveRoundTrip", CrowdyRpcTestFlags)
bool FCrowdyRpcPrimitiveRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyRpcTestTarget* Target = NewObject<UCrowdyRpcTestTarget>();
	TestNotNull(TEXT("Target created"), Target);

	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("Primitives_Implementation"));
	TestNotNull(TEXT("Primitives resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	const FString Sent(TEXT("hello crowdy world"));
	const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::Primitives_Implementation,
		true, 42, static_cast<int64>(9000000001), 1.25f, 3.5, Sent, FName(TEXT("MyTag")));
	FCrowdyRPC::ApplyCall(Target, Fn, Info, Call);

	TestEqual(TEXT("invoked exactly once"), Target->CallCount, 1);
	TestEqual(TEXT("bool"), Target->GotFlag, true);
	TestEqual(TEXT("int32"), Target->GotI32, 42);
	TestEqual(TEXT("int64"), Target->GotI64, static_cast<int64>(9000000001));
	TestEqual(TEXT("float"), Target->GotF, 1.25f);
	TestEqual(TEXT("double"), Target->GotD, 3.5);
	TestEqual(TEXT("FString"), Target->GotStr, Sent);
	TestTrue(TEXT("FName"), Target->GotName == FName(TEXT("MyTag")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcStructRoundTripTest,
	"CrowdySDK.RPC.StructRoundTrip", CrowdyRpcTestFlags)
bool FCrowdyRpcStructRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyRpcTestTarget* Target = NewObject<UCrowdyRpcTestTarget>();
	TestNotNull(TEXT("Target created"), Target);

	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("Structs_Implementation"));
	TestNotNull(TEXT("Structs resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	const FVector Vec(1.0, -2.5, 3.25);
	const FRotator Rot(10.0, 20.0, 30.0);
	const FTransform Xform(Rot.Quaternion(), Vec, FVector(2.0, 3.0, 4.0));

	const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::Structs_Implementation, Vec, Rot, Xform);
	FCrowdyRPC::ApplyCall(Target, Fn, Info, Call);

	TestEqual(TEXT("invoked exactly once"), Target->CallCount, 1);
	TestTrue(TEXT("FVector"), Target->GotVector.Equals(Vec));
	TestTrue(TEXT("FRotator"), Target->GotRotator.Equals(Rot));
	TestTrue(TEXT("FTransform"), Target->GotTransform.Equals(Xform));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcContainerRoundTripTest,
	"CrowdySDK.RPC.ContainerRoundTrip", CrowdyRpcTestFlags)
bool FCrowdyRpcContainerRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyRpcTestTarget* Target = NewObject<UCrowdyRpcTestTarget>();
	TestNotNull(TEXT("Target created"), Target);

	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("Containers_Implementation"));
	TestNotNull(TEXT("Containers resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	// A container parameter is never plain-old-data, so the non-POD frame path must engage.
	TestFalse(TEXT("container signature is not POD"), Info.bParamsPOD);

	const TArray<int32> Ints = { 1, 2, 3, -4, 5 };
	const TArray<FVector> Vecs = { FVector(1.0, 2.0, 3.0), FVector(-4.0, -5.0, -6.0) };
	TMap<FName, int32> Map;
	Map.Add(FName(TEXT("Red")), 10);
	Map.Add(FName(TEXT("Blue")), 20);

	const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::Containers_Implementation, Ints, Vecs, Map);
	FCrowdyRPC::ApplyCall(Target, Fn, Info, Call);

	TestEqual(TEXT("invoked exactly once"), Target->CallCount, 1);
	TestTrue(TEXT("int array round-trips"), Target->GotInts == Ints);

	bool bVecsEqual = Target->GotVecs.Num() == Vecs.Num();
	for (int32 i = 0; bVecsEqual && i < Vecs.Num(); ++i)
	{
		bVecsEqual = Target->GotVecs[i].Equals(Vecs[i]);
	}
	TestTrue(TEXT("vector array round-trips"), bVecsEqual);

	TestEqual(TEXT("map size"), Target->GotMap.Num(), 2);
	TestEqual(TEXT("map Red"), Target->GotMap.FindRef(FName(TEXT("Red"))), 10);
	TestEqual(TEXT("map Blue"), Target->GotMap.FindRef(FName(TEXT("Blue"))), 20);

	// Empty containers must round-trip as empty (not stale, not null).
	const TArray<int32> EmptyInts;
	const TArray<FVector> EmptyVecs;
	const TMap<FName, int32> EmptyMap;
	const FCrowdyRpcCall EmptyCall = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::Containers_Implementation, EmptyInts, EmptyVecs, EmptyMap);
	FCrowdyRPC::ApplyCall(Target, Fn, Info, EmptyCall);

	TestEqual(TEXT("invoked again"), Target->CallCount, 2);
	TestEqual(TEXT("int array now empty"), Target->GotInts.Num(), 0);
	TestEqual(TEXT("vector array now empty"), Target->GotVecs.Num(), 0);
	TestEqual(TEXT("map now empty"), Target->GotMap.Num(), 0);

	return true;
}

// Regression for the Blueprint send path: the VM keeps a const-ref container parameter in the
// out-parm records (the caller's storage), not inline in the event's locals block. Serializing
// from the locals alone would emit an empty array; SerializeParams must read the out-parm record.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcOutParmFrameReadTest,
	"CrowdySDK.RPC.OutParmFrameRead", CrowdyRpcTestFlags)
bool FCrowdyRpcOutParmFrameReadTest::RunTest(const FString& Parameters)
{
	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("SingleIntArray_Implementation"));
	TestNotNull(TEXT("SingleIntArray resolved"), Fn);
	if (!Fn)
	{
		return false;
	}

	FArrayProperty* ArrayParam = nullptr;
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm) && !FCrowdyRPC::IsTrueOutputParam(*It))
		{
			ArrayParam = CastField<FArrayProperty>(*It);
			break;
		}
	}
	TestNotNull(TEXT("array param found"), ArrayParam);
	if (!ArrayParam)
	{
		return false;
	}
	// The fix only matters because the VM routes this param through the out-parm records.
	TestTrue(TEXT("param is by-ref/out (const-ref container)"), ArrayParam->HasAnyPropertyFlags(CPF_OutParm));

	// A zeroed, initialized locals frame, as the VM leaves an out/by-ref param: empty here,
	// with the real value reachable only through the out-parm record.
	const int32 FrameSize = FMath::Max<int32>(Fn->ParmsSize, 1);
	uint8* Locals = static_cast<uint8*>(FMemory_Alloca(FrameSize));
	FMemory::Memzero(Locals, FrameSize);
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->InitializeValue_InContainer(Locals);
		}
	}

	const TArray<int32> Source = { 7, 8, 9 };
	FOutParmRec Rec;
	Rec.Property = ArrayParam;
	Rec.PropAddr = reinterpret_cast<uint8*>(const_cast<TArray<int32>*>(&Source));
	Rec.NextOutParm = nullptr;

	// Reading the locals alone serializes an empty array (version + count 0 = 5 bytes); resolving
	// the out-parm record must serialize the three elements instead.
	TArray<uint8> Blob;
	FCrowdyRPC::SerializeParams(Fn, Locals, Blob, &Rec);
	TestTrue(TEXT("out-parm array serialized non-empty"), Blob.Num() > 5);

	// And it round-trips back to the same values.
	uint8* DestFrame = static_cast<uint8*>(FMemory_Alloca(FrameSize));
	FMemory::Memzero(DestFrame, FrameSize);
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->InitializeValue_InContainer(DestFrame);
		}
	}
	const bool bOk = FCrowdyRPC::DeserializeParams(Fn, Blob, DestFrame);
	TestTrue(TEXT("deserialized"), bOk);
	const TArray<int32>* Got = ArrayParam->ContainerPtrToValuePtr<TArray<int32>>(DestFrame);
	TestEqual(TEXT("element count"), Got->Num(), 3);
	if (Got->Num() == 3)
	{
		TestEqual(TEXT("e0"), (*Got)[0], 7);
		TestEqual(TEXT("e1"), (*Got)[1], 8);
		TestEqual(TEXT("e2"), (*Got)[2], 9);
	}

	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->DestroyValue_InContainer(Locals);
			It->DestroyValue_InContainer(DestFrame);
		}
	}

	return true;
}

// Tier 2: a class reference round-trips by path with no world or subsystem in play.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcClassRefRoundTripTest,
	"CrowdySDK.RPC.ClassRefRoundTrip", CrowdyRpcTestFlags)
bool FCrowdyRpcClassRefRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyRpcTestTarget* Target = NewObject<UCrowdyRpcTestTarget>();
	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("ClassRef_Implementation"));
	TestNotNull(TEXT("ClassRef resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	UClass* Sent = UCrowdyRpcTestTarget::StaticClass();
	const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::ClassRef_Implementation, Sent);
	FCrowdyRPC::ApplyCall(Target, Fn, Info, Call);

	TestEqual(TEXT("invoked once"), Target->CallCount, 1);
	TestTrue(TEXT("class resolved by path"), Target->GotClass == Sent);
	return true;
}

// Tier 2: a null object reference round-trips as null (and clears any prior value).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcNullObjectRoundTripTest,
	"CrowdySDK.RPC.NullObjectRoundTrip", CrowdyRpcTestFlags)
bool FCrowdyRpcNullObjectRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyRpcTestTarget* Target = NewObject<UCrowdyRpcTestTarget>();
	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("ObjectParam_Implementation"));
	TestNotNull(TEXT("ObjectParam resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	// Poison the result so a pass proves the null was carried, not left over.
	Target->GotObject = Target;

	UObject* Sent = nullptr;
	const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::ObjectParam_Implementation, Sent);
	FCrowdyRPC::ApplyCall(Target, Fn, Info, Call);

	TestNull(TEXT("null object round-trips as null"), Target->GotObject);
	return true;
}

// Tier 2: an asset reference round-trips by path. Loads a stock engine asset; if it is not
// available in this environment the test no-ops rather than failing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcAssetRefRoundTripTest,
	"CrowdySDK.RPC.AssetRefRoundTrip", CrowdyRpcTestFlags)
bool FCrowdyRpcAssetRefRoundTripTest::RunTest(const FString& Parameters)
{
	UObject* Asset = LoadObject<UObject>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	if (!Asset)
	{
		AddInfo(TEXT("DefaultMaterial not available; skipping asset round-trip."));
		return true;
	}

	UCrowdyRpcTestTarget* Target = NewObject<UCrowdyRpcTestTarget>();
	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("ObjectParam_Implementation"));
	TestNotNull(TEXT("ObjectParam resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::ObjectParam_Implementation, Asset);
	FCrowdyRPC::ApplyCall(Target, Fn, Info, Call);

	TestTrue(TEXT("asset resolved by path"), Target->GotObject == Asset);
	return true;
}

// Tier 2: the canonical type folds in the referenced class, so object parameters of different
// classes (and a class reference) never collapse to the same FunctionID.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcObjectCanonicalDistinctTest,
	"CrowdySDK.RPC.ObjectCanonicalDistinct", CrowdyRpcTestFlags)
bool FCrowdyRpcObjectCanonicalDistinctTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyRpcTestTarget::StaticClass();

	const auto FirstInputParam = [](UFunction* Fn) -> FProperty*
	{
		if (Fn)
		{
			for (TFieldIterator<FProperty> It(Fn); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Parm) && !FCrowdyRPC::IsTrueOutputParam(*It))
				{
					return *It;
				}
			}
		}
		return nullptr;
	};

	FProperty* ObjectParam = FirstInputParam(Class->FindFunctionByName(TEXT("ObjectParam_Implementation")));
	FProperty* ActorParam = FirstInputParam(Class->FindFunctionByName(TEXT("ActorRef_Implementation")));
	FProperty* ClassParam = FirstInputParam(Class->FindFunctionByName(TEXT("ClassRef_Implementation")));
	TestNotNull(TEXT("object param"), ObjectParam);
	TestNotNull(TEXT("actor param"), ActorParam);
	TestNotNull(TEXT("class param"), ClassParam);
	if (ObjectParam && ActorParam && ClassParam)
	{
		const FString ObjectType = FCrowdyRPC::CanonicalParamType(ObjectParam);
		const FString ActorType = FCrowdyRPC::CanonicalParamType(ActorParam);
		const FString ClassType = FCrowdyRPC::CanonicalParamType(ClassParam);
		TestNotEqual(TEXT("UObject and AActor differ"), ObjectType, ActorType);
		TestNotEqual(TEXT("object and class references differ"), ObjectType, ClassType);
	}
	return true;
}

// Tier 3: an array of class references round-trips with no world or subsystem — each element
// resolves by path. A null element survives as null at its index (length preserved), and an empty
// array round-trips empty. This covers the object-array codec mechanics (count + per-element); the
// entity-array case needs a live subsystem and is signed off in the manual PIE matrix.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcClassArrayRoundTripTest,
	"CrowdySDK.RPC.ClassArrayRoundTrip", CrowdyRpcTestFlags)
bool FCrowdyRpcClassArrayRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyRpcTestTarget* Target = NewObject<UCrowdyRpcTestTarget>();
	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("ClassArray_Implementation"));
	TestNotNull(TEXT("ClassArray resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	// An object-array parameter is never plain-old-data, so the non-POD frame path must engage.
	TestFalse(TEXT("object-array signature is not POD"), Info.bParamsPOD);

	// A null element in the middle must decode to null in place so index-based callers stay correct.
	TArray<UClass*> Sent = { UCrowdyRpcTestTarget::StaticClass(), nullptr, UObject::StaticClass() };
	const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::ClassArray_Implementation, Sent);
	FCrowdyRPC::ApplyCall(Target, Fn, Info, Call);

	TestEqual(TEXT("invoked once"), Target->CallCount, 1);
	TestEqual(TEXT("length preserved"), Target->GotClasses.Num(), 3);
	if (Target->GotClasses.Num() == 3)
	{
		TestTrue(TEXT("element 0 resolved"), Target->GotClasses[0] == UCrowdyRpcTestTarget::StaticClass());
		TestNull(TEXT("element 1 is null in place"), Target->GotClasses[1]);
		TestTrue(TEXT("element 2 resolved"), Target->GotClasses[2] == UObject::StaticClass());
	}

	// An empty array round-trips as empty, not stale.
	const TArray<UClass*> Empty;
	const FCrowdyRpcCall EmptyCall = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::ClassArray_Implementation, Empty);
	FCrowdyRPC::ApplyCall(Target, Fn, Info, EmptyCall);

	TestEqual(TEXT("invoked again"), Target->CallCount, 2);
	TestEqual(TEXT("empty array round-trips empty"), Target->GotClasses.Num(), 0);
	return true;
}

// Tier 3: the canonical type folds the container kind together with the element's referenced class,
// so arrays of different object element types — and an array versus a scalar of the same class —
// never collapse to the same FunctionID.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcObjectArrayCanonicalTest,
	"CrowdySDK.RPC.ObjectArrayCanonical", CrowdyRpcTestFlags)
bool FCrowdyRpcObjectArrayCanonicalTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyRpcTestTarget::StaticClass();

	const auto FirstInputParam = [](UFunction* Fn) -> FProperty*
	{
		if (Fn)
		{
			for (TFieldIterator<FProperty> It(Fn); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Parm) && !FCrowdyRPC::IsTrueOutputParam(*It))
				{
					return *It;
				}
			}
		}
		return nullptr;
	};

	FProperty* ActorArray = FirstInputParam(Class->FindFunctionByName(TEXT("ActorArray_Implementation")));
	FProperty* ClassArray = FirstInputParam(Class->FindFunctionByName(TEXT("ClassArray_Implementation")));
	FProperty* ActorScalar = FirstInputParam(Class->FindFunctionByName(TEXT("ActorRef_Implementation")));
	TestNotNull(TEXT("actor array param"), ActorArray);
	TestNotNull(TEXT("class array param"), ClassArray);
	TestNotNull(TEXT("actor scalar param"), ActorScalar);
	if (ActorArray && ClassArray && ActorScalar)
	{
		const FString ActorArrayType = FCrowdyRPC::CanonicalParamType(ActorArray);
		const FString ClassArrayType = FCrowdyRPC::CanonicalParamType(ClassArray);
		const FString ActorScalarType = FCrowdyRPC::CanonicalParamType(ActorScalar);
		TestNotEqual(TEXT("arrays of different element types differ"), ActorArrayType, ClassArrayType);
		TestNotEqual(TEXT("array and scalar of the same class differ"), ActorArrayType, ActorScalarType);
	}
	return true;
}

// Tier 3: a forged element count is untrusted input and must drop the whole call rather than drive a
// huge allocation. Mirrors VersionMismatchDropped, but at the array level — the count alone trips the
// bound, before any element bytes are read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcObjectArrayBogusCountDroppedTest,
	"CrowdySDK.RPC.ObjectArrayBogusCountDropped", CrowdyRpcTestFlags)
bool FCrowdyRpcObjectArrayBogusCountDroppedTest::RunTest(const FString& Parameters)
{
	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("ClassArray_Implementation"));
	TestNotNull(TEXT("ClassArray resolved"), Fn);
	if (!Fn)
	{
		return false;
	}

	// The version byte the deserializer expects, then an out-of-range element count for the single
	// array parameter. No element bytes follow; the count alone must trip the guard.
	TArray<uint8> Blob;
	{
		FMemoryWriter Writer(Blob, /*bIsPersistent=*/true);
		uint8 Version = CrowdyRpcParamBlobVersion;
		Writer << Version;
		int32 BogusCount = MAX_int32;
		Writer << BogusCount;
	}

	const int32 FrameSize = FMath::Max<int32>(Fn->ParmsSize, 1);
	uint8* Frame = static_cast<uint8*>(FMemory_Alloca(FrameSize));
	FMemory::Memzero(Frame, FrameSize);
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->InitializeValue_InContainer(Frame);
		}
	}

	// The drop emits a warning (expected here); warnings do not fail automation tests.
	const bool bAccepted = FCrowdyRPC::DeserializeParams(Fn, Blob, Frame);
	TestFalse(TEXT("forged element count dropped"), bAccepted);

	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			It->DestroyValue_InContainer(Frame);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcNoArgsTest,
	"CrowdySDK.RPC.NoArgs", CrowdyRpcTestFlags)
bool FCrowdyRpcNoArgsTest::RunTest(const FString& Parameters)
{
	UCrowdyRpcTestTarget* Target = NewObject<UCrowdyRpcTestTarget>();
	TestNotNull(TEXT("Target created"), Target);

	UFunction* Fn = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("NoArgs_Implementation"));
	TestNotNull(TEXT("NoArgs resolved"), Fn);
	if (!Fn)
	{
		return false;
	}
	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	const FCrowdyRpcCall Call = FCrowdyRPC::MarshalCall(Fn, Info,
		&UCrowdyRpcTestTarget::NoArgs_Implementation);
	FCrowdyRPC::ApplyCall(Target, Fn, Info, Call);

	TestEqual(TEXT("invoked exactly once"), Target->CallCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcFunctionIdStabilityTest,
	"CrowdySDK.RPC.FunctionIdStability", CrowdyRpcTestFlags)
bool FCrowdyRpcFunctionIdStabilityTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyRpcTestTarget::StaticClass();
	UFunction* Primitives = Class->FindFunctionByName(TEXT("Primitives_Implementation"));
	UFunction* Structs = Class->FindFunctionByName(TEXT("Structs_Implementation"));
	UFunction* NoArgs = Class->FindFunctionByName(TEXT("NoArgs_Implementation"));
	UFunction* Reordered = Class->FindFunctionByName(TEXT("Reordered_Implementation"));

	TestNotNull(TEXT("Primitives resolved"), Primitives);
	TestNotNull(TEXT("Structs resolved"), Structs);
	TestNotNull(TEXT("NoArgs resolved"), NoArgs);
	TestNotNull(TEXT("Reordered resolved"), Reordered);

	if (!Primitives || !Structs || !NoArgs || !Reordered)
	{
		return false;
	}

	// Deterministic: hashing the same stable signature yields the same id, which is
	// what guarantees stability across separate runs and machines.
	const int64 PrimitivesId = FCrowdyRPC::ComputeFunctionID(Primitives);
	TestEqual(TEXT("id is deterministic"), FCrowdyRPC::ComputeFunctionID(Primitives), PrimitivesId);

	// Distinct signatures must produce distinct ids.
	TestNotEqual(TEXT("differs from struct sig"), PrimitivesId, FCrowdyRPC::ComputeFunctionID(Structs));
	TestNotEqual(TEXT("differs from empty sig"), PrimitivesId, FCrowdyRPC::ComputeFunctionID(NoArgs));
	TestNotEqual(TEXT("differs from reordered sig"), PrimitivesId, FCrowdyRPC::ComputeFunctionID(Reordered));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcVersionMismatchDroppedTest,
	"CrowdySDK.RPC.VersionMismatchDropped", CrowdyRpcTestFlags)
bool FCrowdyRpcVersionMismatchDroppedTest::RunTest(const FString& Parameters)
{
	UFunction* NoArgs = UCrowdyRpcTestTarget::StaticClass()->FindFunctionByName(TEXT("NoArgs_Implementation"));
	TestNotNull(TEXT("NoArgs resolved"), NoArgs);
	if (!NoArgs)
	{
		return false;
	}

	// A no-arg call serializes to just the version byte.
	uint8 SourceFrame = 0;
	TArray<uint8> Blob;
	FCrowdyRPC::SerializeParams(NoArgs, &SourceFrame, Blob);
	TestEqual(TEXT("blob is version byte only"), Blob.Num(), 1);

	uint8 DestFrame = 0;
	TestTrue(TEXT("matching version accepted"), FCrowdyRPC::DeserializeParams(NoArgs, Blob, &DestFrame));

	// A mismatched version is untrusted input and must be dropped, never dispatched.
	// The drop emits a warning (expected here); warnings do not fail automation tests.
	Blob[0] = 0xFF;
	TestFalse(TEXT("mismatched version dropped"), FCrowdyRPC::DeserializeParams(NoArgs, Blob, &DestFrame));
	return true;
}

// Compile-time safety cannot be asserted at runtime: enabling this block must fail to compile.
// Flip to 1 locally to confirm the static_asserts fire.
#if 0
void CrowdyRpcCompileFailDemo(UCrowdyRpcTestTarget* Target)
{
	// Wrong argument count.
	FCrowdyRPC::SendChecked(Target, TEXT("NoArgs_Implementation"),
		&UCrowdyRpcTestTarget::NoArgs_Implementation, 5);

	// Wrong argument type (FString is not convertible to int32).
	FCrowdyRPC::SendChecked(Target, TEXT("Reordered_Implementation"),
		&UCrowdyRpcTestTarget::Reordered_Implementation, FString(TEXT("x")), true);
}
#endif

#endif // WITH_DEV_AUTOMATION_TESTS
