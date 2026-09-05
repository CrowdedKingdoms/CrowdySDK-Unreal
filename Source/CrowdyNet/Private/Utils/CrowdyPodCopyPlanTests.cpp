#include "Utils/CrowdyPodCopyPlanTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "CrowdyNetActorStateWireTestTypes.h"
#include "CrowdyNetLog.h"
#include "HAL/PlatformTime.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Class.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/UnrealType.h"
#include "Utils/CrowdyPodCopyPlan.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UEventPayloadRegistry.h"

namespace CrowdyPodPlanTests
{
	constexpr EAutomationTestFlags TestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Registration is safe to repeat, so every test here can call these at the top without caring
	// whether an earlier one already did.
	constexpr FCrowdyTypeID FlatActorTypeID = 61240;
	constexpr FCrowdyTypeID FlatEventTypeID = 61241;
	constexpr FCrowdyTypeID EmptyActorTypeID = 61242;

	// Sentinel, format version, type tag and class id, ahead of whatever the body writer produced.
	constexpr int32 ActorStateFramingBytes =
		sizeof(FCrowdyTypeID) + sizeof(uint8) + sizeof(FCrowdyTypeID) + sizeof(FCrowdyClassID);

	// A reader that reports a full payload and then fails part-way through it. Nothing on the message path
	// builds one, and it is the only shape that can tell an answer meaning "the plan applies" from an
	// answer meaning "the read succeeded".
	class FCrowdyPodPlanFailingReader : public FMemoryReader
	{
	public:
		FCrowdyPodPlanFailingReader(const TArray<uint8>& Bytes, const int64 InFailAfterBytes)
			: FMemoryReader(Bytes, /*bIsPersistent=*/true)
			, FailAfterBytes(InFailAfterBytes)
		{
		}

		virtual void Serialize(void* Data, int64 Num) override
		{
			if (ServedBytes >= FailAfterBytes)
			{
				SetError();
				return;
			}

			ServedBytes += Num;
			FMemoryReader::Serialize(Data, Num);
		}

	private:
		int64 FailAfterBytes = 0;
		int64 ServedBytes = 0;
	};

	void RegisterFlatState()
	{
		UActorUpdatePayloadRegistry::Get()->RegisterStruct(
			FCrowdyPodPlanFlatState::StaticStruct(), FlatActorTypeID);
		UEventPayloadRegistry::Get()->RegisterStruct(
			FCrowdyPodPlanFlatState::StaticStruct(), FlatEventTypeID);
	}

	template <typename T>
	void AppendLE(TArray<uint8>& Out, T Value)
	{
		const int32 Base = Out.Num();
		Out.SetNumUninitialized(Base + sizeof(T));
		FMemory::Memcpy(Out.GetData() + Base, &Value, sizeof(T));
	}

	int32 OffsetOf(const UScriptStruct* Struct, const TCHAR* FieldName)
	{
		const FProperty* Prop = Struct->FindPropertyByName(FName(FieldName));
		return Prop ? Prop->GetOffset_ForInternal() : INDEX_NONE;
	}

	void FillTransform(FCrowdyPodPlanTransformState& State, const int32 Variant)
	{
		State.Location = FVector(1.25 * Variant, -2.5 - Variant, 3.75 * Variant);
		State.Rotation = FRotator(10.0 + Variant, 20.0 - Variant, 30.0 * Variant);
		State.Speed = 4.5f * Variant;
		State.Health = 100.f - Variant;
		State.Sequence = 42 + Variant;
		State.bAirborne = (Variant % 2) == 1;
		State.Stance = static_cast<uint8>(200 - Variant);
	}

	// A plan no property walk could ever produce. Reading one field's bytes into a different field's
	// memory is the whole point: a decode that comes back matching it can only have run the plan.
	FCrowdyCopyPlan MakeSentinelPlan(const int32 Offset, const int32 Size)
	{
		FCrowdyCopyPlan Plan;
		Plan.Runs.Add({ Offset, Size, false });
		Plan.TotalBytes = Size;
		return Plan;
	}
}

// The plan reproduces the property walk's bytes, and reading it back reproduces the walk's struct, over
// several value sets. One value set could agree by accident on a field the plan happens to skip.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanParityTest,
	"CrowdySDK.Wire.PodCopyPlanMatchesSerializeBin", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanParityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	const UScriptStruct* Struct = FCrowdyPodPlanTransformState::StaticStruct();

	FString Reason;
	const FCrowdyCopyPlan Plan = CrowdyPodCopyPlan::Build(Struct, Reason);
	if (!TestTrue(FString::Printf(TEXT("a plan builds for a plain-old-data state struct (%s)"), *Reason),
		Plan.IsValid()))
	{
		return false;
	}

	for (int32 Variant = 1; Variant <= 3; ++Variant)
	{
		FCrowdyPodPlanTransformState Source;
		FillTransform(Source, Variant);

		TArray<uint8> Reflective;
		{
			FMemoryWriter Writer(Reflective, /*bIsPersistent=*/true);
			Struct->SerializeBin(Writer, &Source);
		}

		FCrowdyPodPlanTransformState PlanSource;
		FillTransform(PlanSource, Variant);

		TArray<uint8> Planned;
		{
			FMemoryWriter Writer(Planned, /*bIsPersistent=*/true);
			TestTrue(TEXT("the plan writes to a plain binary archive"),
				CrowdyPodCopyPlan::Apply(Plan, Writer, &PlanSource));
		}

		TestEqual(FString::Printf(TEXT("variant %d: the plan writes the same number of bytes"), Variant),
			Planned.Num(), Reflective.Num());
		TestTrue(FString::Printf(TEXT("variant %d: the plan writes the same bytes"), Variant),
			Planned == Reflective);

		// Zeroed before either read, because the padding a default constructor leaves indeterminate would
		// otherwise decide the byte compare below.
		FCrowdyPodPlanTransformState ReflectiveOut;
		FMemory::Memzero(&ReflectiveOut, sizeof(ReflectiveOut));
		{
			FMemoryReader Reader(Reflective, /*bIsPersistent=*/true);
			Struct->SerializeBin(Reader, &ReflectiveOut);
		}

		FCrowdyPodPlanTransformState PlannedOut;
		FMemory::Memzero(&PlannedOut, sizeof(PlannedOut));
		{
			FMemoryReader Reader(Reflective, /*bIsPersistent=*/true);
			TestTrue(FString::Printf(TEXT("variant %d: the plan reads back"), Variant),
				CrowdyPodCopyPlan::Apply(Plan, Reader, &PlannedOut));
		}

		// A byte compare rather than CompareScriptStruct: the two reads have to agree octet for octet
		// over the span the plan claims, which per-property Identical cannot see.
		TestEqual(FString::Printf(TEXT("variant %d: both reads land the same memory"), Variant),
			FMemory::Memcmp(&ReflectiveOut, &PlannedOut, Struct->GetStructureSize()), 0);

		TestTrue(FString::Printf(TEXT("variant %d: both reads reproduce the sent struct"), Variant),
			Struct->CompareScriptStruct(&ReflectiveOut, &Source, 0)
			&& Struct->CompareScriptStruct(&PlannedOut, &Source, 0));
	}

	return true;
}

// The refusals, each its own assertion against its own fixture. A plan builder that accepted one of
// these would put a field on the wire whose width is not the width the archive gives it, which is the
// failure the whole idea has to be safe from.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanRefusesVariableWidthTest,
	"CrowdySDK.Wire.PodCopyPlanRefusesVariableWidthLeaves", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanRefusesVariableWidthTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	// Built first, then asserted: reading the reason inside an argument to the call that fills it leaves
	// the two unsequenced. The reason is checked against the rule that should have refused, not only
	// against the member's name, because the catch-all at the end of the accept list names the member too
	// and a check on the name alone cannot tell the two apart.
	const auto Refuses = [this](const TCHAR* What, const UScriptStruct* Struct, const TCHAR* NamedMember,
		const TCHAR* NamedRule = nullptr)
	{
		FString Reason;
		const bool bValid = CrowdyPodCopyPlan::Build(Struct, Reason).IsValid();
		TestFalse(FString::Printf(TEXT("%s is refused (%s)"), What, *Reason), bValid);
		TestTrue(FString::Printf(TEXT("and the refusal of %s names '%s'"), What, NamedMember),
			Reason.Contains(NamedMember));

		if (NamedRule != nullptr)
		{
			TestTrue(FString::Printf(TEXT("and it is the '%s' rule that refused %s"), NamedRule, What),
				Reason.Contains(NamedRule));
		}
	};

	Refuses(TEXT("a struct with a string member"), FCrowdyPodPlanStringState::StaticStruct(), TEXT("Label"));

	// An enum class is an FEnumProperty and a TEnumAsByte is an FByteProperty that names an enum. They
	// serialize the same way and are refused for the same reason, but they reach the builder down
	// different branches, so refusing one says nothing about the other.
	Refuses(TEXT("a struct with an enum class member"), FCrowdyPodPlanEnumState::StaticStruct(),
		TEXT("Stance"), TEXT("is an enum"));
	Refuses(TEXT("a struct with a TEnumAsByte member"), FCrowdyPodPlanLegacyEnumState::StaticStruct(),
		TEXT("LegacyStance"), TEXT("is an enum"));

	Refuses(TEXT("a struct with a dynamic array member"), FCrowdyPodPlanArrayState::StaticStruct(),
		TEXT("Samples"));

	// The one no reasoning from leaf widths would catch: every leaf inside the nested struct is copyable,
	// and the archive still writes its property names rather than its bytes.
	Refuses(TEXT("a struct nesting a struct with no serializer of its own"),
		FCrowdyPodPlanNestedPlainState::StaticStruct(), TEXT("Inner"));

	// The refusal a struct's properties can never justify: every leaf inside this one is copyable and
	// tiles its memory exactly, and its own serializer still writes a version byte ahead of them. Only
	// running that serializer can tell, so this is what says the probe is doing the work.
	Refuses(TEXT("a struct nesting one whose own serializer is not a copy"),
		FCrowdyPodPlanVersionedState::StaticStruct(), TEXT("not a straight copy"));

	Refuses(TEXT("a struct with a bitfield member"), FCrowdyPodPlanBitfieldState::StaticStruct(),
		TEXT("bFlag"));
	Refuses(TEXT("a struct with a fixed-size array member"), FCrowdyPodPlanStaticArrayState::StaticStruct(),
		TEXT("Samples"));
	Refuses(TEXT("a struct with a transient member"), FCrowdyPodPlanTransientState::StaticStruct(),
		TEXT("Scratch"));

	return true;
}

// A Blueprint-authored struct is recompiled into the same object: its properties are destroyed and its
// offsets re-linked in place, so a cache keyed on object identity still answers while every offset
// behind that answer has moved, and no reload notification is raised for it. It is refused outright and
// keeps the reflective walk, which reads the property chain afresh on every call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanRefusesABlueprintStructTest,
	"CrowdySDK.Wire.PodCopyPlanRefusesABlueprintAuthoredStruct", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanRefusesABlueprintStructTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	UUserDefinedStruct* Struct = NewObject<UUserDefinedStruct>(GetTransientPackage());
	if (!TestNotNull(TEXT("the fixture struct was created"), Struct))
	{
		return false;
	}

	FGCObjectScopeGuard StructGuard(Struct);

	// Two plain integers, added and linked the way the struct compiler does it. This is deliberately a
	// struct that WOULD get a plan: without the refusal the assertions below fail because a plan builds,
	// not because some other rule happened to fire first.
	Struct->AddCppProperty(new FIntProperty(Struct, FName(TEXT("Second"))));
	Struct->AddCppProperty(new FIntProperty(Struct, FName(TEXT("First"))));
	Struct->Bind();
	Struct->StaticLink(/*bRelinkExistingProperties=*/true);

	if (!TestEqual(TEXT("the fixture occupies two integers"), Struct->GetStructureSize(),
		static_cast<int32>(2 * sizeof(int32))))
	{
		return false;
	}

	// The premise, checked rather than assumed: the property walk really would move these eight bytes,
	// so a plan is genuinely on offer here.
	uint8 Scratch[2 * sizeof(int32)] = {};
	TArray<uint8> Walked;
	{
		FMemoryWriter Writer(Walked, /*bIsPersistent=*/true);
		Struct->SerializeBin(Writer, Scratch);
	}

	TestEqual(TEXT("and the property walk moves them"), Walked.Num(), Struct->GetStructureSize());

	FString Reason;
	const bool bValid = CrowdyPodCopyPlan::Build(Struct, Reason).IsValid();

	TestFalse(FString::Printf(TEXT("a Blueprint-authored struct gets no plan (%s)"), *Reason), bValid);

	// Named against the rule, not against the struct: several other refusals would also name this fixture.
	TestTrue(FString::Printf(TEXT("and it is the Blueprint-authored rule that refused it (%s)"), *Reason),
		Reason.Contains(TEXT("Blueprint-authored")));

	TestNull(TEXT("and the cache hands out nothing for it either"), CrowdyPodCopyPlan::Find(Struct));

	CrowdyPodCopyPlan::InvalidateAll();
	return true;
}

// A struct with nothing to copy is refused, and that refusal is only correct if the fallback still puts
// the same bytes on the wire, so both halves are asserted here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanRefusesAStructWithNothingToCopyTest,
	"CrowdySDK.Wire.PodCopyPlanRefusesAStructWithNothingToCopy", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanRefusesAStructWithNothingToCopyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	UScriptStruct* Struct = FCrowdyPodPlanEmptyState::StaticStruct();
	UActorUpdatePayloadRegistry::Get()->RegisterStruct(Struct, EmptyActorTypeID);
	CrowdyPodCopyPlan::InvalidateAll();

	FString Reason;
	const bool bValid = CrowdyPodCopyPlan::Build(Struct, Reason).IsValid();

	TestFalse(FString::Printf(TEXT("a struct with no properties gets no plan (%s)"), *Reason), bValid);
	TestTrue(FString::Printf(TEXT("and it is the nothing-to-copy rule that refused it (%s)"), *Reason),
		Reason.Contains(TEXT("has no properties to copy")));
	TestNull(TEXT("and the cache hands out nothing for it either"), CrowdyPodCopyPlan::Find(Struct));

	FCrowdyPodPlanEmptyState Source;

	TArray<uint8> Walked;
	{
		FMemoryWriter Writer(Walked, /*bIsPersistent=*/true);
		Struct->SerializeBin(Writer, &Source);
	}

	TArray<uint8> OutBytes;
	const bool bWritten = USerializationFunctionLibrary::SerializeActorState(
		FInstancedStruct::Make(Source), CrowdyActorStateWireTestClassID, OutBytes);

	FInstancedStruct Decoded;
	const bool bDecoded = bWritten && USerializationFunctionLibrary::DeserializeActorState(OutBytes, Decoded);

	CrowdyPodCopyPlan::InvalidateAll();

	if (!TestTrue(TEXT("the payload serializes through the framing"), bWritten)
		|| !TestTrue(TEXT("and the framing is complete"), OutBytes.Num() >= ActorStateFramingBytes))
	{
		return false;
	}

	// The half that makes the refusal correct rather than merely safe: what the fallback wrote is what the
	// property walk writes, octet for octet.
	const TArray<uint8> Body(OutBytes.GetData() + ActorStateFramingBytes, OutBytes.Num() - ActorStateFramingBytes);
	TestTrue(TEXT("a refused struct still writes exactly what the property walk writes"), Body == Walked);

	TestTrue(TEXT("and it round-trips back"), bDecoded);
	TestNotNull(TEXT("as the registered struct"), Decoded.GetPtr<FCrowdyPodPlanEmptyState>());

	return true;
}

// What a true from Apply is allowed to mean. The up-front length check cannot see an archive that fails
// part-way, so the answer is taken from the archive at the end rather than assumed from the fact that
// the plan was applicable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanRefusesAFailedArchiveTest,
	"CrowdySDK.Wire.PodCopyPlanRefusesAnArchiveThatFailedUnderIt", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanRefusesAFailedArchiveTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	const UScriptStruct* Struct = FCrowdyPodPlanTransformState::StaticStruct();

	FString Reason;
	const FCrowdyCopyPlan Plan = CrowdyPodCopyPlan::Build(Struct, Reason);
	if (!TestTrue(FString::Printf(TEXT("a plan builds (%s)"), *Reason), Plan.IsValid()))
	{
		return false;
	}

	// More than one run, or the failure would have nowhere to land: the archive is asked for bytes once
	// per run and this fixture's normalised bool splits the copy.
	if (!TestTrue(TEXT("the plan asks the archive for bytes more than once"), Plan.Runs.Num() > 1))
	{
		return false;
	}

	FCrowdyPodPlanTransformState Source;
	FillTransform(Source, 1);

	TArray<uint8> Bytes;
	{
		FMemoryWriter Writer(Bytes, /*bIsPersistent=*/true);
		Struct->SerializeBin(Writer, &Source);
	}

	// The control: the same plan over the same bytes, through an archive that does not fail.
	FCrowdyPodPlanTransformState Healthy;
	{
		FMemoryReader Reader(Bytes, /*bIsPersistent=*/true);
		TestTrue(TEXT("an archive that stays good is taken"), CrowdyPodCopyPlan::Apply(Plan, Reader, &Healthy));
	}

	FCrowdyPodPlanTransformState Failed;
	{
		FCrowdyPodPlanFailingReader Reader(Bytes, /*FailAfterBytes=*/1);

		// The length the plan wants is all there, so the up-front guard has nothing to refuse on.
		TestTrue(TEXT("the failing archive still declares a payload long enough for the plan"),
			Reader.TotalSize() - Reader.Tell() >= Plan.TotalBytes);

		TestFalse(TEXT("an archive that failed part-way is not reported as a clean read"),
			CrowdyPodCopyPlan::Apply(Plan, Reader, &Failed));
		TestTrue(TEXT("and the archive is the thing that says so"), Reader.IsError());
	}

	return true;
}

// A bool reaches the archive as 0 or 1 rather than as its byte, in BOTH directions. Nothing a C++ caller
// writes can put another value in a bool, but a forged octet on the wire can, and a plan that copied it
// raw would leave a bool holding neither 0 nor 1 and then re-emit that octet on the next send.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanNormalisesABoolTest,
	"CrowdySDK.Wire.PodCopyPlanNormalisesABoolByte", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanNormalisesABoolTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	const UScriptStruct* Struct = FCrowdyPodPlanTransformState::StaticStruct();

	FString Reason;
	const FCrowdyCopyPlan Plan = CrowdyPodCopyPlan::Build(Struct, Reason);
	if (!TestTrue(FString::Printf(TEXT("a plan builds (%s)"), *Reason), Plan.IsValid()))
	{
		return false;
	}

	const int32 BoolOffset = OffsetOf(Struct, TEXT("bAirborne"));
	if (!TestNotEqual(TEXT("the fixture has a bool member"), BoolOffset, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	// Poked through memory, because no assignment can put this value in a bool.
	FCrowdyPodPlanTransformState Source;
	FillTransform(Source, 1);
	reinterpret_cast<uint8*>(&Source)[BoolOffset] = 0x37;

	TArray<uint8> Planned;
	{
		FMemoryWriter Writer(Planned, /*bIsPersistent=*/true);
		TestTrue(TEXT("the plan writes"), CrowdyPodCopyPlan::Apply(Plan, Writer, &Source));
	}

	FCrowdyPodPlanTransformState WalkSource;
	FillTransform(WalkSource, 1);
	reinterpret_cast<uint8*>(&WalkSource)[BoolOffset] = 0x37;

	TArray<uint8> Reflective;
	{
		FMemoryWriter Writer(Reflective, /*bIsPersistent=*/true);
		Struct->SerializeBin(Writer, &WalkSource);
	}

	TestTrue(TEXT("a struct holding a byte that is neither 0 nor 1 still writes the same bytes either way"),
		Planned == Reflective);

	// The wire offset of the bool: everything ahead of it is copied verbatim, so its position is the sum
	// of the widths before it, which for this fixture is the whole struct up to the bool.
	if (!TestTrue(TEXT("the write produced a body"), Planned.Num() > BoolOffset))
	{
		return false;
	}

	TestEqual(TEXT("and the octet it wrote is normalised"), static_cast<int32>(Planned[BoolOffset]), 1);

	// The other direction: a forged octet read through the plan has to land the same value the walk
	// would have landed, not the octet.
	TArray<uint8> Forged = Reflective;
	Forged[BoolOffset] = 0x37;

	FCrowdyPodPlanTransformState WalkedIn;
	{
		FMemoryReader Reader(Forged, /*bIsPersistent=*/true);
		Struct->SerializeBin(Reader, &WalkedIn);
	}

	FCrowdyPodPlanTransformState PlannedIn;
	{
		FMemoryReader Reader(Forged, /*bIsPersistent=*/true);
		TestTrue(TEXT("the plan reads"), CrowdyPodCopyPlan::Apply(Plan, Reader, &PlannedIn));
	}

	TestEqual(TEXT("a forged bool octet reads back as the walk reads it"),
		static_cast<int32>(reinterpret_cast<const uint8*>(&PlannedIn)[BoolOffset]),
		static_cast<int32>(reinterpret_cast<const uint8*>(&WalkedIn)[BoolOffset]));
	TestEqual(TEXT("which is one, not the octet"),
		static_cast<int32>(reinterpret_cast<const uint8*>(&PlannedIn)[BoolOffset]), 1);

	return true;
}

// A payload that ends inside the struct has to keep the property walk, which stops at the first field
// that does not fit and leaves that field and every later one at its default. A merged span cannot make
// that decision, so the plan refuses the archive rather than copying a partial field.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanShortPayloadTest,
	"CrowdySDK.Wire.PodCopyPlanRefusesAShortArchive", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanShortPayloadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	const UScriptStruct* Struct = FCrowdyPodPlanFlatState::StaticStruct();

	FString Reason;
	const FCrowdyCopyPlan Plan = CrowdyPodCopyPlan::Build(Struct, Reason);
	if (!TestTrue(FString::Printf(TEXT("a plan builds (%s)"), *Reason), Plan.IsValid()))
	{
		return false;
	}

	TArray<uint8> Short;
	AppendLE(Short, static_cast<int32>(111));

	FCrowdyPodPlanFlatState Out;
	{
		FMemoryReader Reader(Short, /*bIsPersistent=*/true);
		TestFalse(TEXT("a plan needing more bytes than remain refuses the archive"),
			CrowdyPodCopyPlan::Apply(Plan, Reader, &Out));
		TestEqual(TEXT("and nothing was consumed, so the walk starts where it would have"),
			static_cast<int32>(Reader.Tell()), 0);
	}

	TestEqual(TEXT("and nothing was written into the struct"), Out.A, FCrowdyPodPlanFlatState().A);

	TArray<uint8> Exact;
	AppendLE(Exact, static_cast<int32>(111));
	AppendLE(Exact, static_cast<int32>(222));

	FCrowdyPodPlanFlatState Filled;
	{
		FMemoryReader Reader(Exact, /*bIsPersistent=*/true);
		TestTrue(TEXT("a payload that covers the struct is taken"),
			CrowdyPodCopyPlan::Apply(Plan, Reader, &Filled));
	}

	TestEqual(TEXT("first field"), Filled.A, 111);
	TestEqual(TEXT("second field"), Filled.B, 222);

	return true;
}

// The rule that keeps the plan off the per-entity loop: a struct is walked once, and a struct that was
// refused is remembered as refused rather than being walked again to be told the same no.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanBuiltOncePerStructTest,
	"CrowdySDK.Wire.PodCopyPlanIsBuiltOncePerStruct", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanBuiltOncePerStructTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	RegisterFlatState();
	CrowdyPodCopyPlan::InvalidateAll();

	TestEqual(TEXT("the cache starts empty"), CrowdyPodCopyPlan::GetBuildCount(), 0);

	TArray<uint8> Payload;
	AppendActorStateFraming(Payload, FlatActorTypeID, CrowdyActorStateWireTestClassID);
	AppendLE(Payload, static_cast<int32>(-42));
	AppendLE(Payload, static_cast<int32>(777));

	for (int32 Index = 0; Index < 8; ++Index)
	{
		FInstancedStruct Decoded;
		if (!TestTrue(TEXT("the frame decodes"),
			USerializationFunctionLibrary::DeserializeActorState(Payload, Decoded)))
		{
			return false;
		}
	}

	// Counts the plan builds, not the decodes: a builder that walked the properties per message would
	// answer eight here and nothing else in the suite would notice.
	TestEqual(TEXT("eight decodes of one struct built one plan"), CrowdyPodCopyPlan::GetBuildCount(), 1);
	TestNotNull(TEXT("and that struct did get a plan"),
		CrowdyPodCopyPlan::Find(FCrowdyPodPlanFlatState::StaticStruct()));
	TestEqual(TEXT("and asking again did not build a second"), CrowdyPodCopyPlan::GetBuildCount(), 1);

	// A refusal is a cached answer too, or every message of a refused type re-walks its properties.
	const UScriptStruct* Refused = FCrowdyPodPlanStringState::StaticStruct();
	TestNull(TEXT("a struct with a string member has no plan"), CrowdyPodCopyPlan::Find(Refused));
	TestEqual(TEXT("which cost one walk"), CrowdyPodCopyPlan::GetBuildCount(), 2);
	TestNull(TEXT("asking again still has no plan"), CrowdyPodCopyPlan::Find(Refused));
	TestEqual(TEXT("and cost no second walk"), CrowdyPodCopyPlan::GetBuildCount(), 2);

	CrowdyPodCopyPlan::InvalidateAll();
	return true;
}

// Proves the production decoder READS the cached plan rather than merely agreeing with it. A plan that
// lands one field's bytes in another field's memory is something no property walk can produce, so the
// decoded values name which path ran.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanActorStateDecodeUsesThePlanTest,
	"CrowdySDK.Wire.ActorStateDecodeUsesTheBakedPlan", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanActorStateDecodeUsesThePlanTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	RegisterFlatState();
	CrowdyPodCopyPlan::InvalidateAll();

	const UScriptStruct* Struct = FCrowdyPodPlanFlatState::StaticStruct();
	const int32 SecondFieldOffset = OffsetOf(Struct, TEXT("B"));
	if (!TestNotEqual(TEXT("the fixture has a second field"), SecondFieldOffset, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	CrowdyPodCopyPlan::InstallForTests(Struct, MakeSentinelPlan(SecondFieldOffset, sizeof(int32)));

	TArray<uint8> Payload;
	AppendActorStateFraming(Payload, FlatActorTypeID, CrowdyActorStateWireTestClassID);
	AppendLE(Payload, static_cast<int32>(-42));
	AppendLE(Payload, static_cast<int32>(777));

	FInstancedStruct Decoded;
	const bool bDecoded = USerializationFunctionLibrary::DeserializeActorState(Payload, Decoded);

	const FCrowdyPodPlanFlatState* Value = Decoded.GetPtr<FCrowdyPodPlanFlatState>();
	const int32 First = Value ? Value->A : INDEX_NONE;
	const int32 Second = Value ? Value->B : INDEX_NONE;

	CrowdyPodCopyPlan::InvalidateAll();

	if (!TestTrue(TEXT("the frame decodes"), bDecoded) || !TestNotNull(TEXT("as the registered struct"), Value))
	{
		return false;
	}

	// Under the property walk these would be -42 and 777.
	TestEqual(TEXT("the first field keeps its default, because the sentinel plan never writes it"),
		First, FCrowdyPodPlanFlatState().A);
	TestEqual(TEXT("and the leading wire octets landed where the sentinel plan sent them"), Second, -42);

	return true;
}

// The mirror of the test above. A plan applied on one side of a pair and not the other is a wire break,
// so the writer needs its own proof that it takes the plan, not an inference from the reader's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanActorStateEncodeUsesThePlanTest,
	"CrowdySDK.Wire.ActorStateEncodeUsesTheBakedPlan", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanActorStateEncodeUsesThePlanTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	RegisterFlatState();
	CrowdyPodCopyPlan::InvalidateAll();

	const UScriptStruct* Struct = FCrowdyPodPlanFlatState::StaticStruct();
	const int32 SecondFieldOffset = OffsetOf(Struct, TEXT("B"));
	if (!TestNotEqual(TEXT("the fixture has a second field"), SecondFieldOffset, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	// Writes only the second field, so both the length and the octets say which path ran.
	CrowdyPodCopyPlan::InstallForTests(Struct, MakeSentinelPlan(SecondFieldOffset, sizeof(int32)));

	FCrowdyPodPlanFlatState Source;
	Source.A = -42;
	Source.B = 777;

	TArray<uint8> OutBytes;
	const bool bWritten = USerializationFunctionLibrary::SerializeActorState(
		FInstancedStruct::Make(Source), CrowdyActorStateWireTestClassID, OutBytes);

	CrowdyPodCopyPlan::InvalidateAll();

	if (!TestTrue(TEXT("the payload serializes"), bWritten))
	{
		return false;
	}

	// Sentinel, format version, type tag and class id, then whatever the body writer produced.
	constexpr int32 FramingBytes = sizeof(FCrowdyTypeID) + sizeof(uint8) + sizeof(FCrowdyTypeID) + sizeof(FCrowdyClassID);

	// Under the property walk this would be FramingBytes + 8.
	if (!TestEqual(TEXT("the body is the sentinel plan's width, not the struct's"), OutBytes.Num(),
		FramingBytes + static_cast<int32>(sizeof(int32))))
	{
		return false;
	}

	int32 Body = 0;
	FMemory::Memcpy(&Body, OutBytes.GetData() + FramingBytes, sizeof(int32));
	TestEqual(TEXT("and the octets are the field the sentinel plan names"), Body, 777);

	return true;
}

// The event framing's flat tail is a second pair of call sites, and it takes the plan the same way. One
// sentinel plan, both directions, so neither half can be the one that was left behind.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanEventStateUsesThePlanTest,
	"CrowdySDK.Wire.EventStateUsesTheBakedPlan", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanEventStateUsesThePlanTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	RegisterFlatState();
	CrowdyPodCopyPlan::InvalidateAll();

	const UScriptStruct* Struct = FCrowdyPodPlanFlatState::StaticStruct();
	const int32 SecondFieldOffset = OffsetOf(Struct, TEXT("B"));
	if (!TestNotEqual(TEXT("the fixture has a second field"), SecondFieldOffset, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	CrowdyPodCopyPlan::InstallForTests(Struct, MakeSentinelPlan(SecondFieldOffset, sizeof(int32)));

	FCrowdyPodPlanFlatState Source;
	Source.A = -42;
	Source.B = 777;

	TArray<uint8> OutBytes;
	const bool bWritten = USerializationFunctionLibrary::SerializeEventState(
		FInstancedStruct::Make(Source), OutBytes);

	FInstancedStruct Decoded;
	const bool bDecoded = bWritten && USerializationFunctionLibrary::DeserializeEventState(OutBytes, Decoded);

	const FCrowdyPodPlanFlatState* Value = Decoded.GetPtr<FCrowdyPodPlanFlatState>();
	const int32 First = Value ? Value->A : INDEX_NONE;
	const int32 Second = Value ? Value->B : INDEX_NONE;
	const int32 WrittenBytes = OutBytes.Num();

	CrowdyPodCopyPlan::InvalidateAll();

	if (!TestTrue(TEXT("the payload serializes"), bWritten))
	{
		return false;
	}

	// Under the property walk this would be the tag plus 8.
	TestEqual(TEXT("the event body is the sentinel plan's width"), WrittenBytes,
		static_cast<int32>(sizeof(FCrowdyTypeID) + sizeof(int32)));

	if (!TestTrue(TEXT("the payload decodes"), bDecoded) || !TestNotNull(TEXT("as the registered struct"), Value))
	{
		return false;
	}

	TestEqual(TEXT("the first field kept its default on both sides"), First, FCrowdyPodPlanFlatState().A);
	TestEqual(TEXT("and the field the sentinel plan carried survived the round trip"), Second, 777);

	return true;
}

// The measurement the slice is sized from. It asserts nothing about time: a machine under load would
// make an assertion here fail for a reason that has nothing to do with the code.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPodCopyPlanCostTest,
	"CrowdySDK.Wire.PodCopyPlanCost", CrowdyPodPlanTests::TestFlags)
bool FCrowdyPodCopyPlanCostTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPodPlanTests;

	const UScriptStruct* Struct = FCrowdyPodPlanTransformState::StaticStruct();

	FString Reason;
	const FCrowdyCopyPlan Plan = CrowdyPodCopyPlan::Build(Struct, Reason);
	if (!TestTrue(FString::Printf(TEXT("a plan builds (%s)"), *Reason), Plan.IsValid()))
	{
		return false;
	}

	FCrowdyPodPlanTransformState Source;
	FillTransform(Source, 1);

	constexpr int32 WarmUpIterations = 20000;
	constexpr int32 Iterations = 200000;

	// One buffer for every timed loop, cleared with Reset so the allocation is made once and both sides
	// are measured against the same allocation policy.
	TArray<uint8> Buffer;

	const auto ReflectiveWrite = [&]()
	{
		Buffer.Reset();
		FMemoryWriter Writer(Buffer, /*bIsPersistent=*/true);
		Struct->SerializeBin(Writer, &Source);
	};

	const auto PlannedWrite = [&]()
	{
		Buffer.Reset();
		FMemoryWriter Writer(Buffer, /*bIsPersistent=*/true);
		CrowdyPodCopyPlan::Apply(Plan, Writer, &Source);
	};

	FCrowdyPodPlanTransformState Sink;

	const auto ReflectiveRead = [&]()
	{
		FMemoryReader Reader(Buffer, /*bIsPersistent=*/true);
		Struct->SerializeBin(Reader, &Sink);
	};

	const auto PlannedRead = [&]()
	{
		FMemoryReader Reader(Buffer, /*bIsPersistent=*/true);
		CrowdyPodCopyPlan::Apply(Plan, Reader, &Sink);
	};

	// Every loop above builds an archive the real call sites build once per message whatever the body
	// costs, so the body's own cost is what is left after this. Without it the plan's figure is read as
	// the copy alone and it is not.
	const auto ArchiveOnly = [&]()
	{
		FMemoryReader Reader(Buffer, /*bIsPersistent=*/true);
		Reader.Seek(0);
	};

	// What the plan adds per message that the reflective walk never paid: one cache lookup.
	const UScriptStruct* const LookupStruct = Struct;
	const auto PlanLookup = [&LookupStruct]()
	{
		CrowdyPodCopyPlan::Find(LookupStruct);
	};

	const auto TimeNanoseconds = [](auto&& Body, const int32 Count) -> double
	{
		const double Start = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Body();
		}
		return (FPlatformTime::Seconds() - Start) * 1000000000.0 / static_cast<double>(Count);
	};

	for (int32 Index = 0; Index < WarmUpIterations; ++Index)
	{
		ReflectiveWrite();
		PlannedWrite();
		ReflectiveRead();
		PlannedRead();
	}

	ReflectiveWrite();
	const double ReflectiveReadNs = TimeNanoseconds(ReflectiveRead, Iterations);
	const double PlannedReadNs = TimeNanoseconds(PlannedRead, Iterations);
	const double ReflectiveWriteNs = TimeNanoseconds(ReflectiveWrite, Iterations);
	const double PlannedWriteNs = TimeNanoseconds(PlannedWrite, Iterations);
	const double ArchiveOnlyNs = TimeNanoseconds(ArchiveOnly, Iterations);
	const double PlanLookupNs = TimeNanoseconds(PlanLookup, Iterations);

	// Guards against timing an empty loop: a plan that wrote nothing would look free.
	TestEqual(TEXT("both sides moved the same number of bytes"), Plan.TotalBytes, Buffer.Num());

	UE_LOG(LogCrowdyNet, Display,
		TEXT("[PodCopyPlan] %d runs over %d bytes. write: SerializeBin %.1f ns, plan %.1f ns (%.1fx). read: SerializeBin %.1f ns, plan %.1f ns (%.1fx). archive alone %.1f ns, cache lookup %.1f ns."),
		Plan.Runs.Num(), Plan.TotalBytes,
		ReflectiveWriteNs, PlannedWriteNs, PlannedWriteNs > 0.0 ? ReflectiveWriteNs / PlannedWriteNs : 0.0,
		ReflectiveReadNs, PlannedReadNs, PlannedReadNs > 0.0 ? ReflectiveReadNs / PlannedReadNs : 0.0,
		ArchiveOnlyNs, PlanLookupNs);

	AddInfo(FString::Printf(
		TEXT("PodCopyPlan write SerializeBin=%.1fns plan=%.1fns read SerializeBin=%.1fns plan=%.1fns archive=%.1fns lookup=%.1fns"),
		ReflectiveWriteNs, PlannedWriteNs, ReflectiveReadNs, PlannedReadNs, ArchiveOnlyNs, PlanLookupNs));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
