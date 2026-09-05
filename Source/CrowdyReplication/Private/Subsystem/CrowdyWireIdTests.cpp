#include "Subsystem/CrowdyIdRegistryTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Core/CrowdyCategory/FCrowdyIDConflict.h"
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Core/FCrowdyTypeID.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Hash/CityHash.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Misc/Crc.h"
#include "Misc/ScopeExit.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/UObjectIterator.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "Utils/UEventPayloadRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyWireIdTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// An ID no struct in this registry currently holds, so a test can force a
	// collision on it without displacing a real payload type.
	template <typename RegistryType>
	FCrowdyTypeID FindUnusedTypeID(const RegistryType* Registry)
	{
		for (uint32 Candidate = 60000; Candidate > 1024; --Candidate)
		{
			const FCrowdyTypeID ID = static_cast<FCrowdyTypeID>(Candidate);
			if (!Registry->Resolve(ID))
				return ID;
		}
		return CROWDY_INVALID_TYPE_ID;
	}

	const FCrowdyIDConflict* FindConflict(const TArray<FCrowdyIDConflict>& Conflicts, const uint32 ID, const FString& RejectedPath)
	{
		return Conflicts.FindByPredicate([&](const FCrowdyIDConflict& Conflict)
		{
			return Conflict.ID == ID && Conflict.RejectedPath == RejectedPath;
		});
	}

	// Fills the probe so that neither framing can read a pointer or a length out of it: everything is
	// zero except a few marked words, and each marked word is the lower half of an eight-octet field
	// so that reading it as a floating point number yields a tiny value rather than a special one.
	//
	// The three middle markers are what tell the two framings apart. They sit where the spawn framing
	// would deposit the transform it read from a different offset, so applying that framing here moves
	// them. The first and last only say the payload was carried whole.
	void FillSpawnFramingProbe(FCrowdyIdSpawnFramingProbe& Probe)
	{
		FMemory::Memzero(Probe.Words, sizeof(Probe.Words));
		Probe.Words[0] = 0x11111111;
		Probe.Words[16] = 0x5A5A5A5A;
		Probe.Words[18] = 0x33333333;
		Probe.Words[20] = 0x77777777;
		Probe.Words[47] = 0x0F0F0F0F;
	}

	// Tag followed by the probe's own flat encoding, which is what an ordinary payload looks like on
	// the wire whatever ID it happens to carry.
	void MakeFlatProbeFrame(const FCrowdyTypeID TypeID, const FCrowdyIdSpawnFramingProbe& Probe, TArray<uint8>& OutBytes)
	{
		FCrowdyIdSpawnFramingProbe Copy = Probe;

		OutBytes.Reset();
		FMemoryWriter Writer(OutBytes, true);

		FCrowdyTypeID Tag = TypeID;
		Writer << Tag;
		FCrowdyIdSpawnFramingProbe::StaticStruct()->SerializeBin(Writer, &Copy);
	}
}

// The struct type ID is a frozen wire value: it is the first field of every event payload frame, so
// changing it renumbers the protocol. The literal below is the CRC-32 of the UTF-8 bytes of the path
// name, folded into the 1..65535 range. The path is asserted alongside it, because moving or renaming
// the struct would change the ID for a reason that has nothing to do with the hashing rule.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdStructGoldenVectorTest,
	"CrowdySDK.Replication.WireIdStructGoldenVector", CrowdyWireIdTestFlags)
bool FCrowdyWireIdStructGoldenVectorTest::RunTest(const FString& Parameters)
{
	const UScriptStruct* Delta = FCrowdyStateDelta::StaticStruct();

	TestEqual(TEXT("The golden vector is pinned to this exact path"),
		Delta->GetPathName(), FString(TEXT("/Script/CrowdyReplication.CrowdyStateDelta")));

	TestEqual(TEXT("FCrowdyStateDelta type ID"),
		static_cast<int32>(FCrowdyTypeIDGenerator::GenerateFromStruct(Delta)), 54889);

	return true;
}

// The class ID rides spawn events as a plain 32-bit wire field. Same reasoning as the struct golden
// vector; AActor is used because its path is fixed by the engine rather than by this plugin.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdClassGoldenVectorTest,
	"CrowdySDK.Replication.WireIdClassGoldenVector", CrowdyWireIdTestFlags)
bool FCrowdyWireIdClassGoldenVectorTest::RunTest(const FString& Parameters)
{
	const UClass* Class = AActor::StaticClass();

	TestEqual(TEXT("The golden vector is pinned to this exact path"),
		Class->GetPathName(), FString(TEXT("/Script/Engine.Actor")));

	TestEqual(TEXT("AActor class ID"),
		static_cast<int64>(FCrowdyTypeIDGenerator::GenerateFromClass(Class)),
		static_cast<int64>(2810328196u));

	return true;
}

// The encoding rule itself. The sample carries a character whose UTF-8 encoding is two bytes and
// whose UTF-16 encoding is one code unit, so hashing the raw TCHAR bytes and hashing the UTF-8 bytes
// give different answers on this platform. Pinning the UTF-8 answer as a literal, and asserting the
// two routes actually diverge, keeps the assertion from being true by construction.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdHashesUtf8BytesTest,
	"CrowdySDK.Replication.WireIdHashesUtf8Bytes", CrowdyWireIdTestFlags)
bool FCrowdyWireIdHashesUtf8BytesTest::RunTest(const FString& Parameters)
{
	// Spelled out as bytes rather than typed, so the source file stays plain ASCII and the test says
	// exactly which byte sequence the expected value belongs to.
	const ANSICHAR Utf8Bytes[] = { 'A', static_cast<ANSICHAR>(0xC3), static_cast<ANSICHAR>(0xA9), 'B', '\0' };
	const FString Sample(UTF8_TO_TCHAR(Utf8Bytes));

	TestEqual(TEXT("Hash of the UTF-8 bytes 41 C3 A9 42"),
		static_cast<int64>(FCrowdyTypeIDGenerator::HashNameUtf8(Sample)),
		static_cast<int64>(1980952385u));

	if constexpr (sizeof(TCHAR) != 1)
	{
		const uint32 RawTCharHash = FCrc::MemCrc32(*Sample, Sample.Len() * sizeof(TCHAR));
		TestTrue(TEXT("Hashing raw TCHAR bytes would give a different answer here"),
			FCrowdyTypeIDGenerator::HashNameUtf8(Sample) != RawTCharHash);
	}

	return true;
}

// The 64-bit half of the same encoding rule. Every baked function ID, property ID and layout hash is
// derived from it, and those are frozen into the cooked registry, so a change here would invalidate a
// packaged build's tables without anything failing to compile. The literal is the CityHash64 of the
// four UTF-8 octets below; the same assertion is made through GenerateFromString, which is the form
// the rest of the SDK calls.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdStringHashGoldenVectorTest,
	"CrowdySDK.Replication.WireIdStringHashGoldenVector", CrowdyWireIdTestFlags)
bool FCrowdyWireIdStringHashGoldenVectorTest::RunTest(const FString& Parameters)
{
	// Spelled out as bytes rather than typed, so the source file stays plain ASCII and the test says
	// exactly which byte sequence the expected value belongs to.
	const ANSICHAR Utf8Bytes[] = { 'A', static_cast<ANSICHAR>(0xC3), static_cast<ANSICHAR>(0xA9), 'B', '\0' };
	const FString Sample(UTF8_TO_TCHAR(Utf8Bytes));

	const uint64 Expected = 6651074663127736315ull;

	TestTrue(TEXT("64-bit hash of the UTF-8 bytes 41 C3 A9 42"),
		FCrowdyTypeIDGenerator::HashNameUtf8_64(Sample) == Expected);

	TestTrue(TEXT("GenerateFromString reports the same value"),
		FCrowdyTypeIDGenerator::GenerateFromString(Sample) == static_cast<int64>(Expected));

	if constexpr (sizeof(TCHAR) != 1)
	{
		const uint64 RawTCharHash = CityHash64(reinterpret_cast<const char*>(*Sample),
			static_cast<uint32>(Sample.Len() * sizeof(TCHAR)));
		TestTrue(TEXT("Hashing raw TCHAR bytes would give a different answer here"),
			FCrowdyTypeIDGenerator::HashNameUtf8_64(Sample) != RawTCharHash);
	}

	return true;
}

// Range invariants. The two probe structs exist for this test: their path names were chosen so one
// lands on the bottom edge of the mapping and the other on the top, which pins the modulus and the
// offset instead of just observing whatever value comes out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdRangeInvariantsTest,
	"CrowdySDK.Replication.WireIdRangeInvariants", CrowdyWireIdTestFlags)
bool FCrowdyWireIdRangeInvariantsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("A path hashing to a multiple of the modulus maps to the first usable ID"),
		static_cast<int32>(FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyIdProbeLow7989::StaticStruct())), 1);

	TestEqual(TEXT("A path hashing to one below a multiple of the modulus maps to the last usable ID"),
		static_cast<int32>(FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyIdProbeHigh1372::StaticStruct())), 65535);

	int32 StructsChecked = 0;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		const FCrowdyTypeID ID = FCrowdyTypeIDGenerator::GenerateFromStruct(*It);
		if (ID == CROWDY_INVALID_TYPE_ID)
		{
			AddError(FString::Printf(TEXT("Struct '%s' produced the reserved invalid type ID"), *It->GetPathName()));
			break;
		}
		++StructsChecked;
	}
	TestTrue(TEXT("The sweep saw a meaningful number of structs"), StructsChecked > 100);

	int32 ClassesChecked = 0;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (FCrowdyTypeIDGenerator::GenerateFromClass(*It) == CROWDY_INVALID_CLASS_ID)
		{
			AddError(FString::Printf(TEXT("Class '%s' produced the reserved invalid class ID"), *It->GetPathName()));
			break;
		}
		++ClassesChecked;
	}
	TestTrue(TEXT("The sweep saw a meaningful number of classes"), ClassesChecked > 100);

	return true;
}

// Two payload structs claiming one ID is an authoring mistake, not a reason to take the process down.
// The type that got there first keeps the ID, the other is left unregistered so that serializing it
// fails at the sender instead of arriving and being decoded as the incumbent, and the pair is recorded
// with the remedy attached.
//
// The registry here is built for the test rather than taken from the process, because a conflict left
// in the shared one would be re-reported on every later map load.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdEventConflictIsNonFatalTest,
	"CrowdySDK.Replication.WireIdEventConflictIsNonFatal", CrowdyWireIdTestFlags)
bool FCrowdyWireIdEventConflictIsNonFatalTest::RunTest(const FString& Parameters)
{
	UEventPayloadRegistry* Registry = NewObject<UEventPayloadRegistry>();
	FGCObjectScopeGuard RegistryGuard(Registry);

	const FCrowdyTypeID ID = FindUnusedTypeID(Registry);
	if (!TestTrue(TEXT("Found an unused type ID to contest"), ID != CROWDY_INVALID_TYPE_ID))
		return false;

	UScriptStruct* Incumbent = FCrowdyIdConflictProbeA::StaticStruct();
	UScriptStruct* Rejected = FCrowdyIdConflictProbeB::StaticStruct();

	Registry->RegisterStruct(Incumbent, ID);
	Registry->RegisterStruct(Rejected, ID);

	TestTrue(TEXT("The incumbent still owns the ID"), Registry->Resolve(ID) == Incumbent);

	FCrowdyTypeID RejectedID = CROWDY_INVALID_TYPE_ID;
	TestFalse(TEXT("The rejected struct has no ID, so sending it fails"),
		Registry->GetID(Rejected, RejectedID));

	const TArray<FCrowdyIDConflict> Conflicts = Registry->GetConflicts();
	const FCrowdyIDConflict* Conflict = FindConflict(Conflicts, ID, Rejected->GetPathName());
	if (!TestNotNull(TEXT("The conflict was recorded"), Conflict))
		return false;

	TestEqual(TEXT("The record names the incumbent"), Conflict->IncumbentPath, Incumbent->GetPathName());
	TestTrue(TEXT("The record names the remedy"), Conflict->Remedy.Contains(TEXT("IDOverrides")));

	return true;
}

// The actor-update registry carries the same policy as the event registry, and for the same reason
// the registry under test is built here rather than taken from the process.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdActorUpdateConflictIsNonFatalTest,
	"CrowdySDK.Replication.WireIdActorUpdateConflictIsNonFatal", CrowdyWireIdTestFlags)
bool FCrowdyWireIdActorUpdateConflictIsNonFatalTest::RunTest(const FString& Parameters)
{
	UActorUpdatePayloadRegistry* Registry = NewObject<UActorUpdatePayloadRegistry>();
	FGCObjectScopeGuard RegistryGuard(Registry);

	const FCrowdyTypeID ID = FindUnusedTypeID(Registry);
	if (!TestTrue(TEXT("Found an unused type ID to contest"), ID != CROWDY_INVALID_TYPE_ID))
		return false;

	UScriptStruct* Incumbent = FCrowdyIdConflictProbeA::StaticStruct();
	UScriptStruct* Rejected = FCrowdyIdConflictProbeB::StaticStruct();

	Registry->RegisterStruct(Incumbent, ID);
	Registry->RegisterStruct(Rejected, ID);

	TestTrue(TEXT("The incumbent still owns the ID"), Registry->Resolve(ID) == Incumbent);

	FCrowdyTypeID RejectedID = CROWDY_INVALID_TYPE_ID;
	TestFalse(TEXT("The rejected struct has no ID, so sending it fails"),
		Registry->GetID(Rejected, RejectedID));

	const TArray<FCrowdyIDConflict> Conflicts = Registry->GetConflicts();
	const FCrowdyIDConflict* Conflict = FindConflict(Conflicts, ID, Rejected->GetPathName());
	if (!TestNotNull(TEXT("The conflict was recorded"), Conflict))
		return false;

	TestEqual(TEXT("The record names the incumbent"), Conflict->IncumbentPath, Incumbent->GetPathName());
	TestTrue(TEXT("The record names the remedy"), Conflict->Remedy.Contains(TEXT("IDOverrides")));

	return true;
}

// One entry point reports every conflict from every registry in one place, so the whole set is visible
// at once instead of one line per unlucky load order. This one has to use the process registries,
// because that is what the report reads, so it plants a conflict in each and then clears all three.
//
// The class registry stops accepting registrations once it is sealed, which happens when a game
// instance starts. In a run where that has already happened the class half cannot be contested and is
// left uncovered; the two payload registries are always covered.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdConflictReportCoversAllRegistriesTest,
	"CrowdySDK.Replication.WireIdConflictReportCoversAllRegistries", CrowdyWireIdTestFlags)
bool FCrowdyWireIdConflictReportCoversAllRegistriesTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("[CrowdyIDConflicts]"), EAutomationExpectedErrorFlags::Contains, 0);

	UEventPayloadRegistry* Events = UEventPayloadRegistry::Get();
	UActorUpdatePayloadRegistry* ActorUpdates = UActorUpdatePayloadRegistry::Get();
	UCrowdyClassRegistry* Classes = UCrowdyClassRegistry::Get();

	ON_SCOPE_EXIT
	{
		Events->ClearConflicts();
		ActorUpdates->ClearConflicts();
		Classes->ClearConflicts();
	};

	UScriptStruct* Incumbent = FCrowdyIdConflictProbeD::StaticStruct();
	UScriptStruct* Rejected = FCrowdyIdConflictProbeC::StaticStruct();

	const FCrowdyTypeID EventID = FindUnusedTypeID(Events);
	const FCrowdyTypeID ActorUpdateID = FindUnusedTypeID(ActorUpdates);
	if (!TestTrue(TEXT("Found unused type IDs to contest"),
		EventID != CROWDY_INVALID_TYPE_ID && ActorUpdateID != CROWDY_INVALID_TYPE_ID))
		return false;

	Events->RegisterStruct(Incumbent, EventID);
	Events->RegisterStruct(Rejected, EventID);

	ActorUpdates->RegisterStruct(Incumbent, ActorUpdateID);
	ActorUpdates->RegisterStruct(Rejected, ActorUpdateID);

	const bool bClassRegistryAcceptsRegistrations = !Classes->IsSealed();
	if (bClassRegistryAcceptsRegistrations)
	{
		AddExpectedErrorPlain(TEXT("[CrowdyClassRegistry] ClassID collision"), EAutomationExpectedErrorFlags::Contains, 0);

		const FCrowdyClassID ClassID = FCrowdyTypeIDGenerator::GenerateFromClass(AActor::StaticClass());
		Classes->RegisterClass(ClassID, FSoftClassPath(AActor::StaticClass()));
		Classes->RegisterClass(ClassID, FSoftClassPath(APawn::StaticClass()));

		TestTrue(TEXT("The class conflict was planted"), Classes->GetConflicts().Num() > 0);
	}
	else
	{
		AddInfo(TEXT("The class registry is already sealed in this run, so its half of the report is not covered here."));
	}

	const int32 Expected =
		Events->GetConflicts().Num() +
		ActorUpdates->GetConflicts().Num() +
		Classes->GetConflicts().Num();

	TestTrue(TEXT("Both payload conflicts exist"), Expected >= 2);
	TestEqual(TEXT("The report accounts for every registry's conflicts"),
		CrowdyIDConflicts::ReportAll(), Expected);

	return true;
}

// A class whose registration was refused over an ID clash must not go on to send that ID anyway. The
// spawn event also carries the class path, and the receiver only consults it when the ID resolves to
// nothing, so handing back the incumbent's ID would have every remote client spawn the wrong class
// instead of falling back to the path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdRefusedClassHasNoIDTest,
	"CrowdySDK.Replication.WireIdRefusedClassHasNoID", CrowdyWireIdTestFlags)
bool FCrowdyWireIdRefusedClassHasNoIDTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("[CrowdyClassRegistry] ClassID collision"), EAutomationExpectedErrorFlags::Contains, 0);

	UCrowdyClassRegistry* Registry = NewObject<UCrowdyClassRegistry>();
	FGCObjectScopeGuard RegistryGuard(Registry);

	UClass* Incumbent = AActor::StaticClass();
	UClass* Rejected = APawn::StaticClass();

	const FCrowdyClassID ID = FCrowdyTypeIDGenerator::GenerateFromClass(Incumbent);

	TestTrue(TEXT("The first class takes the ID"), Registry->RegisterClass(ID, FSoftClassPath(Incumbent)));
	TestFalse(TEXT("The second class is refused"), Registry->RegisterClass(ID, FSoftClassPath(Rejected)));

	TestEqual(TEXT("The incumbent still reports the contested ID"),
		static_cast<int64>(Registry->GetID(Incumbent)), static_cast<int64>(ID));

	TestEqual(TEXT("The refused class reports no ID at all"),
		static_cast<int64>(Registry->GetID(Rejected)), static_cast<int64>(CROWDY_INVALID_CLASS_ID));

	TestFalse(TEXT("So a receiver resolves nothing from it and falls back to the class path"),
		Registry->Resolve(Registry->GetID(Rejected)).IsValid());

	const FCrowdyIDConflict* Conflict =
		FindConflict(Registry->GetConflicts(), ID, FSoftClassPath(Rejected).ToString());
	TestNotNull(TEXT("The conflict was recorded"), Conflict);

	// Resolving the clash by assigning an explicit ID puts the class back in service.
	Registry->ClearConflicts();
	TestEqual(TEXT("Clearing the conflict restores the path-derived ID"),
		static_cast<int64>(Registry->GetID(Rejected)),
		static_cast<int64>(FCrowdyTypeIDGenerator::GenerateFromClass(Rejected)));

	return true;
}

// The entity spawn and destroy events are the only payloads written field by field instead of as flat
// structs. Which framing to use is decided from the payload's own type, not from the type ID it was
// registered under, so that a type sharing that ID is never read as a spawn event: the receive buffer
// is sized for the type the ID resolved to, and the spawn fields are far wider than most payloads.
//
// Both cases put an ordinary payload on the spawn event's ID in a registry emptied for the test, which
// is the situation the ID is chosen to avoid but which nothing prevents outright.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdSpawnFramingFollowsTypeOnSendTest,
	"CrowdySDK.Replication.WireIdSpawnFramingFollowsTypeOnSend", CrowdyWireIdTestFlags)
bool FCrowdyWireIdSpawnFramingFollowsTypeOnSendTest::RunTest(const FString& Parameters)
{
	UEventPayloadRegistry* Registry = UEventPayloadRegistry::Get();
	Registry->Reset();
	ON_SCOPE_EXIT { Registry->Reset(); };

	const FCrowdyTypeID SpawnID =
		FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyEntitySpawnEvent::StaticStruct());

	Registry->RegisterStruct(FCrowdyIdSpawnFramingProbe::StaticStruct(), SpawnID);
	if (!TestTrue(TEXT("The probe holds the spawn event's ID"),
		Registry->Resolve(SpawnID) == FCrowdyIdSpawnFramingProbe::StaticStruct()))
		return false;

	FCrowdyIdSpawnFramingProbe Probe;
	FillSpawnFramingProbe(Probe);

	TArray<uint8> Encoded;
	if (!TestTrue(TEXT("The probe serializes"),
		USerializationFunctionLibrary::SerializeEventState(FInstancedStruct::Make(Probe), Encoded)))
		return false;

	TArray<uint8> Flat;
	MakeFlatProbeFrame(SpawnID, Probe, Flat);

	TestTrue(TEXT("The probe went out flat, not in the spawn event's framing"), Encoded == Flat);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdSpawnFramingFollowsTypeOnReceiveTest,
	"CrowdySDK.Replication.WireIdSpawnFramingFollowsTypeOnReceive", CrowdyWireIdTestFlags)
bool FCrowdyWireIdSpawnFramingFollowsTypeOnReceiveTest::RunTest(const FString& Parameters)
{
	UEventPayloadRegistry* Registry = UEventPayloadRegistry::Get();
	Registry->Reset();
	ON_SCOPE_EXIT { Registry->Reset(); };

	const FCrowdyTypeID SpawnID =
		FCrowdyTypeIDGenerator::GenerateFromStruct(FCrowdyEntitySpawnEvent::StaticStruct());

	Registry->RegisterStruct(FCrowdyIdSpawnFramingProbe::StaticStruct(), SpawnID);
	if (!TestTrue(TEXT("The probe holds the spawn event's ID"),
		Registry->Resolve(SpawnID) == FCrowdyIdSpawnFramingProbe::StaticStruct()))
		return false;

	FCrowdyIdSpawnFramingProbe Probe;
	FillSpawnFramingProbe(Probe);

	TArray<uint8> Frame;
	MakeFlatProbeFrame(SpawnID, Probe, Frame);

	FInstancedStruct Decoded;
	if (!TestTrue(TEXT("The frame decodes"),
		USerializationFunctionLibrary::DeserializeEventState(Frame, Decoded)))
		return false;

	if (!TestTrue(TEXT("It decoded as the probe"),
		Decoded.GetScriptStruct() == FCrowdyIdSpawnFramingProbe::StaticStruct()))
		return false;

	const FCrowdyIdSpawnFramingProbe& Result = Decoded.Get<FCrowdyIdSpawnFramingProbe>();
	TestTrue(TEXT("Every word survived, so the spawn fields were not written over it"),
		FMemory::Memcmp(Result.Words, Probe.Words, sizeof(Probe.Words)) == 0);

	return true;
}

// GetID remembers its answer per class once the registry is sealed, so the send loop stops rebuilding a class
// path string per call. What that memo owes is that it still separates classes: an answer remembered under a
// key that does not tell two classes apart hands the first class's ID to the second, and every receiver then
// resolves that entity's state to the wrong class. The calls below are interleaved for exactly that reason,
// since a memo that keeps one answer only fails once a second class asks.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdSealedRegistryMemoIsPerClassTest,
	"CrowdySDK.Replication.WireIdSealedRegistryMemoIsPerClass", CrowdyWireIdTestFlags)
bool FCrowdyWireIdSealedRegistryMemoIsPerClassTest::RunTest(const FString& Parameters)
{
	// The memo answers only on the game thread, so a run somewhere else would exercise the path this test
	// does not name and report a pass for coverage it never had.
	if (!TestTrue(TEXT("This test has to run on the game thread to reach the memo at all"), IsInGameThread()))
		return false;

	AddExpectedErrorPlain(TEXT("[CrowdyClassRegistry] ClassID collision"), EAutomationExpectedErrorFlags::Contains, 0);

	UCrowdyClassRegistry* Registry = NewObject<UCrowdyClassRegistry>();
	FGCObjectScopeGuard RegistryGuard(Registry);

	UClass* Registered = AActor::StaticClass();
	UClass* Refused = APawn::StaticClass();
	UClass* Unregistered = UObject::StaticClass();

	// An ID nothing derives, so the registered class's answer cannot be mistaken for its own path hash. The
	// derived IDs are asserted valid first, or two classes that both derive nothing would compare equal and
	// the interleaving below would prove nothing.
	const FCrowdyClassID ExplicitID = 4242;
	if (!TestTrue(TEXT("Every class in this test derives a distinct, valid ID to begin with"),
		FCrowdyTypeIDGenerator::GenerateFromClass(Registered) != CROWDY_INVALID_CLASS_ID
			&& FCrowdyTypeIDGenerator::GenerateFromClass(Unregistered) != CROWDY_INVALID_CLASS_ID
			&& FCrowdyTypeIDGenerator::GenerateFromClass(Refused) != CROWDY_INVALID_CLASS_ID
			&& FCrowdyTypeIDGenerator::GenerateFromClass(Registered) != FCrowdyTypeIDGenerator::GenerateFromClass(Unregistered)
			&& ExplicitID != FCrowdyTypeIDGenerator::GenerateFromClass(Registered)
			&& ExplicitID != FCrowdyTypeIDGenerator::GenerateFromClass(Unregistered)
			&& ExplicitID != FCrowdyTypeIDGenerator::GenerateFromClass(Refused)))
		return false;

	TestTrue(TEXT("The first class takes the explicit ID"),
		Registry->RegisterClass(ExplicitID, FSoftClassPath(Registered)));
	TestFalse(TEXT("The second class is refused it"),
		Registry->RegisterClass(ExplicitID, FSoftClassPath(Refused)));

	Registry->Seal();

	// Two passes over the same three classes: the first fills the memo, the second reads it, and a key that
	// does not separate classes gives itself away on the second pass at the latest.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		TestEqual(*FString::Printf(TEXT("Pass %d: the registered class reports its explicit ID"), Pass),
			static_cast<int64>(Registry->GetID(Registered)), static_cast<int64>(ExplicitID));

		TestEqual(*FString::Printf(TEXT("Pass %d: the unregistered class reports its path hash"), Pass),
			static_cast<int64>(Registry->GetID(Unregistered)),
			static_cast<int64>(FCrowdyTypeIDGenerator::GenerateFromClass(Unregistered)));

		TestEqual(*FString::Printf(TEXT("Pass %d: the refused class reports no ID at all"), Pass),
			static_cast<int64>(Registry->GetID(Refused)), static_cast<int64>(CROWDY_INVALID_CLASS_ID));
	}

	// A refusal is an answer like any other, so it is remembered like one; clearing the clash has to drop it
	// or the class stays unusable for the rest of the process while the conflict reads as resolved.
	Registry->ClearConflicts();
	TestEqual(TEXT("Clearing the conflict restores the path-derived ID through the memo too"),
		static_cast<int64>(Registry->GetID(Refused)),
		static_cast<int64>(FCrowdyTypeIDGenerator::GenerateFromClass(Refused)));

	// The other way the memo can go stale: the classes themselves are rebuilt under it.
	Registry->InvalidateIDMemo();
	TestEqual(TEXT("And a memo invalidation re-resolves rather than losing the registered answer"),
		static_cast<int64>(Registry->GetID(Registered)), static_cast<int64>(ExplicitID));

	return true;
}

// The event payload registry answers the same question keyed on the struct the caller already holds, which is
// what takes the path string off the outbound event path. Same obligation as the class memo: it has to keep
// two structs apart, or an event is sent under another type's ID and decoded as that type on arrival.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdEventRegistryAnswersPerStructTest,
	"CrowdySDK.Replication.WireIdEventRegistryAnswersPerStruct", CrowdyWireIdTestFlags)
bool FCrowdyWireIdEventRegistryAnswersPerStructTest::RunTest(const FString& Parameters)
{
	UEventPayloadRegistry* Registry = NewObject<UEventPayloadRegistry>();
	FGCObjectScopeGuard RegistryGuard(Registry);

	UScriptStruct* First = FCrowdyIdConflictProbeA::StaticStruct();
	UScriptStruct* Second = FCrowdyIdConflictProbeB::StaticStruct();

	Registry->RegisterStruct(First, 4243);
	Registry->RegisterStruct(Second, 4244);

	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		FCrowdyTypeID FirstID = CROWDY_INVALID_TYPE_ID;
		FCrowdyTypeID SecondID = CROWDY_INVALID_TYPE_ID;

		TestTrue(*FString::Printf(TEXT("Pass %d: the first struct resolves"), Pass),
			Registry->GetID(First, FirstID));
		TestTrue(*FString::Printf(TEXT("Pass %d: the second struct resolves"), Pass),
			Registry->GetID(Second, SecondID));

		TestEqual(*FString::Printf(TEXT("Pass %d: the first struct keeps its own ID"), Pass),
			static_cast<int64>(FirstID), static_cast<int64>(4243));
		TestEqual(*FString::Printf(TEXT("Pass %d: the second struct keeps its own ID"), Pass),
			static_cast<int64>(SecondID), static_cast<int64>(4244));
	}

	// An unregistered struct still has to miss. A lookup that answers from the wrong map, or that treats a
	// miss as an absence of the question rather than of the answer, would give it one.
	FCrowdyTypeID Unregistered = CROWDY_INVALID_TYPE_ID;
	TestFalse(TEXT("A struct nobody registered resolves to nothing"),
		Registry->GetID(FCrowdyIdConflictProbeC::StaticStruct(), Unregistered));

	return true;
}

// Registering a struct has to KEEP it, because the receive path resolves a wire type id back to a
// UScriptStruct and reads forged bytes against its property list. A TObjectPtr in an unreflected container
// is not a reference, so before the maps were reflected a registered struct could be collected and Resolve
// would hand the decoder freed memory.
//
// The probe is a struct created at runtime and then dropped, deliberately, because a native USTRUCT proves
// nothing here: those live in the permanent object pool and survive any collection whatever the registry
// does, which is exactly why this went unnoticed. Liveness is read through a weak pointer rather than
// through the registry's own entry, so the unfixed build reports a failure instead of dereferencing a
// freed pointer and taking the run down with it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireIdRegistriesKeepTheirStructsAliveTest,
	"CrowdySDK.Replication.WireIdRegistriesKeepTheirStructsAlive", CrowdyWireIdTestFlags)
bool FCrowdyWireIdRegistriesKeepTheirStructsAliveTest::RunTest(const FString& Parameters)
{
	UEventPayloadRegistry* Events = NewObject<UEventPayloadRegistry>();
	FGCObjectScopeGuard EventsGuard(Events);
	UActorUpdatePayloadRegistry* ActorUpdates = NewObject<UActorUpdatePayloadRegistry>();
	FGCObjectScopeGuard ActorUpdatesGuard(ActorUpdates);

	UScriptStruct* EventProbe = NewObject<UScriptStruct>(GetTransientPackage());
	UScriptStruct* ActorUpdateProbe = NewObject<UScriptStruct>(GetTransientPackage());
	if (!TestNotNull(TEXT("the event probe was created"), EventProbe)
		|| !TestNotNull(TEXT("the actor-update probe was created"), ActorUpdateProbe))
	{
		return false;
	}

	// The premise the whole test rests on: these are genuinely collectable. A rooted probe would survive
	// regardless and the test would pass while proving nothing.
	if (!TestFalse(TEXT("the event probe is not rooted, so a collection can actually take it"),
			EventProbe->IsRooted())
		|| !TestFalse(TEXT("nor is the actor-update probe"), ActorUpdateProbe->IsRooted()))
	{
		return false;
	}

	const FCrowdyTypeID EventProbeID = 4245;
	const FCrowdyTypeID ActorUpdateProbeID = 4246;
	Events->RegisterStruct(EventProbe, EventProbeID);
	ActorUpdates->RegisterStruct(ActorUpdateProbe, ActorUpdateProbeID);

	// Watched weakly, and every strong local reference dropped, so the registry is the only thing left that
	// could be keeping either alive.
	const FWeakObjectPtr WatchEvent(EventProbe);
	const FWeakObjectPtr WatchActorUpdate(ActorUpdateProbe);
	EventProbe = nullptr;
	ActorUpdateProbe = nullptr;

	// A purging collection is what happens roughly once a minute in a running game.
	CollectGarbage(RF_NoFlags, /*bPerformFullPurge*/ true);

	const bool bEventSurvived = WatchEvent.IsValid();
	const bool bActorUpdateSurvived = WatchActorUpdate.IsValid();
	TestTrue(TEXT("the registered event payload survived a collection, which only a real reference can do"),
		bEventSurvived);
	TestTrue(TEXT("and so did the registered actor-update payload"), bActorUpdateSurvived);

	// Only now, and only if it is alive, is the registry's own entry safe to read.
	if (bEventSurvived)
	{
		TestTrue(TEXT("and the event registry still resolves its id to that same struct"),
			Events->Resolve(EventProbeID) == WatchEvent.Get());
	}
	if (bActorUpdateSurvived)
	{
		TestTrue(TEXT("and the actor-update registry still resolves its id to that same struct"),
			ActorUpdates->Resolve(ActorUpdateProbeID) == WatchActorUpdate.Get());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
