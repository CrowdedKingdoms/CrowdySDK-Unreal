#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyServiceApiTestSupport.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Queries/Data/Avatar/Types/FCrowdyAppAvatarState.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatar.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatarError.h"
#include "Subsystem/CrowdyPersistenceSubsystem.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"

// Avatars and the persistence pull no longer carry their own GraphQL text: they name an operation and let
// FCrowdyCppClient::RunOp resolve it against the vendored CrowdyCPP generated tables. This file pins what that swap
// made load-bearing: that every operation name still resolves, and that the answers are read the way the generated
// documents actually shape them.
namespace
{
	constexpr EAutomationTestFlags ServiceTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	using CrowdyServiceApiTest::MakeCanceledResult;
	using CrowdyServiceApiTest::MakeResult;
	using CrowdyServiceApiTest::OperationResolves;

	struct FRoutedOperation
	{
		ECrowdyCppApiDomain Domain;
		const TCHAR* OperationName;
	};

	// Every operation UCrowdyAvatars and the persistence pull issue.
	const FRoutedOperation kRoutedOperations[] =
	{
		{ ECrowdyCppApiDomain::Avatars, TEXT("MyAvatars") },
		{ ECrowdyCppApiDomain::Avatars, TEXT("AvatarById") },
		{ ECrowdyCppApiDomain::Avatars, TEXT("UserAvatars") },
		{ ECrowdyCppApiDomain::Avatars, TEXT("AvatarAppState") },
		{ ECrowdyCppApiDomain::Avatars, TEXT("AvatarAppStates") },
		{ ECrowdyCppApiDomain::Avatars, TEXT("CreateAvatar") },
		{ ECrowdyCppApiDomain::Avatars, TEXT("UpdateAvatar") },
		{ ECrowdyCppApiDomain::Avatars, TEXT("DeleteAvatar") },
		{ ECrowdyCppApiDomain::Avatars, TEXT("UpdateAvatarState") },
		{ ECrowdyCppApiDomain::Avatars, TEXT("UpdateAvatarAppState") },

		{ ECrowdyCppApiDomain::Chunks,  TEXT("GetVoxelList") },
	};

	FString EncodeState(const TArray<uint8>& Bytes)
	{
		return FBase64::Encode(Bytes);
	}
}

// A vendored bump that renames or drops one of these must fail here rather than the first time a player presses the
// button. A canned 200 stands in for the server, so this is pure name resolution with no network.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAvatarHostPersistenceOperationsResolveTest,
	"CrowdySDK.CrowdyServices.AvatarHostPersistenceOperationsResolve", ServiceTestFlags)
bool FCrowdyAvatarHostPersistenceOperationsResolveTest::RunTest(const FString& Parameters)
{
	for (const FRoutedOperation& Op : kRoutedOperations)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
		if (!TestTrue(FString::Printf(TEXT("test client constructs for %s"), Op.OperationName), Client.IsValid()))
		{
			continue;
		}

		FString Error;
		TestTrue(FString::Printf(TEXT("%s resolves to a document"), Op.OperationName),
			OperationResolves(Client, Op.Domain, Op.OperationName, Error));
	}
	return true;
}

// The proof the resolves-check above can fail, in each domain it covers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAvatarHostPersistenceUnknownOperationFailsTest,
	"CrowdySDK.CrowdyServices.AvatarHostPersistenceUnknownOperationFails", ServiceTestFlags)
bool FCrowdyAvatarHostPersistenceUnknownOperationFailsTest::RunTest(const FString& Parameters)
{
	const FRoutedOperation Bogus[] =
	{
		{ ECrowdyCppApiDomain::Avatars, TEXT("ThisAvatarOperationDoesNotExist") },
		{ ECrowdyCppApiDomain::Chunks,  TEXT("ThisChunkOperationDoesNotExist") },
	};

	for (const FRoutedOperation& Op : Bogus)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
		if (!TestTrue(TEXT("test client constructs"), Client.IsValid()))
		{
			continue;
		}

		FString Error;
		TestFalse(FString::Printf(TEXT("%s does not resolve"), Op.OperationName),
			OperationResolves(Client, Op.Domain, Op.OperationName, Error));
		TestTrue(TEXT("the failure names the operation"), Error.Contains(Op.OperationName));
	}
	return true;
}

// The avatar answers are read straight out of the `data` object rather than through a response struct. A list keeps
// only the rows that parse, which is a deliberate change from the old parser: it added an unparsed row as a zeroed
// avatar, so a malformed row used to reach Blueprint as avatar id 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAvatarResponseReadersTest,
	"CrowdySDK.CrowdyServices.AvatarResponseReaders", ServiceTestFlags)
bool FCrowdyAvatarResponseReadersTest::RunTest(const FString& Parameters)
{
	FCrowdyAvatarError Error;

	const FCrowdyCppJsonResult One = MakeResult(TEXT(
		R"({ "avatar": { "avatarId": "42", "userId": "7", "name": "Rook", "publicState": "AA==", "privateState": "BB==", "createdAt": "t" } })"));

	FCrowdyAvatar Avatar;
	if (TestTrue(TEXT("an avatar object reads"), CrowdyServiceApi::ReadObject(One, TEXT("avatar"), Avatar, Error)))
	{
		TestEqual(TEXT("the BigInt id is parsed from its string"), Avatar.AvatarId, static_cast<int64>(42));
		TestEqual(TEXT("the owning user is parsed"), Avatar.UserId, static_cast<int64>(7));
		TestEqual(TEXT("the name survives"), Avatar.Name, FString(TEXT("Rook")));
		TestEqual(TEXT("private state survives"), Avatar.PrivateState, FString(TEXT("BB==")));
	}

	// The field this operation answers with is not the field another operation answers with, so asking for the
	// wrong one must fail rather than quietly read an empty avatar.
	FCrowdyAvatar Wrong;
	TestFalse(TEXT("a different field is not this operation's answer"),
		CrowdyServiceApi::ReadObject(One, TEXT("createAvatar"), Wrong, Error));
	TestEqual(TEXT("an unreadable answer is a server error"), Error.Code, ECrowdyAvatarErrorCode::ServerError);

	const FCrowdyCppJsonResult List = MakeResult(TEXT(
		R"({ "myAvatars": [ { "avatarId": "1", "name": "A" }, { "name": "no id" }, { "avatarId": "3", "name": "C" } ] })"));

	TArray<FCrowdyAvatar> Avatars;
	if (TestTrue(TEXT("a list reads"), CrowdyServiceApi::ReadArray(List, TEXT("myAvatars"), Avatars, Error)))
	{
		TestEqual(TEXT("the row without an id is dropped rather than zeroed"), Avatars.Num(), 2);
		if (Avatars.Num() == 2)
		{
			TestEqual(TEXT("the good rows survive in order"), Avatars[0].AvatarId, static_cast<int64>(1));
			TestEqual(TEXT("the good rows survive in order"), Avatars[1].AvatarId, static_cast<int64>(3));
		}
	}

	TArray<FCrowdyAvatar> None;
	TestTrue(TEXT("an empty list is a successful read of nothing"),
		CrowdyServiceApi::ReadArray(MakeResult(TEXT(R"({ "myAvatars": [] })")), TEXT("myAvatars"), None, Error));

	const FCrowdyCppJsonResult AppState = MakeResult(TEXT(
		R"({ "avatarAppState": { "appId": "9", "avatarId": "42", "state": "QUJD", "createdAt": "c", "updatedAt": "u" } })"));

	FCrowdyAppAvatarState State;
	if (TestTrue(TEXT("an app state reads"), CrowdyServiceApi::ReadObject(AppState, TEXT("avatarAppState"), State, Error)))
	{
		TestEqual(TEXT("the app is parsed"), State.AppId, static_cast<int64>(9));
		TestEqual(TEXT("the raw state is handed back untouched"), State.RawState, FString(TEXT("QUJD")));
	}

	// A cancellation is not a server verdict, so it must not be classified by keyword-matching its wording.
	FCrowdyAvatar Unreached;
	TestFalse(TEXT("a cancelled call does not read"),
		CrowdyServiceApi::ReadObject(MakeCanceledResult(), TEXT("avatar"), Unreached, Error));
	TestEqual(TEXT("a cancellation is a network failure"), Error.Code, ECrowdyAvatarErrorCode::NetworkError);

	return true;
}

// The persistence pull reads one chunk per struct type and one voxel per instance. Telling "read and empty" apart
// from "not read at all" is what decides whether a waiting Pull State node reports an empty slot or a failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPersistenceVoxelChunkReadTest,
	"CrowdySDK.CrowdyServices.PersistenceVoxelChunkRead", ServiceTestFlags)
bool FCrowdyPersistenceVoxelChunkReadTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> SlotSeven = { 1, 2, 3, 4 };
	const TArray<uint8> Singleton = { 9 };

	// Every row carrying this is one whose slot cannot be read. None of them may reach a slot, and slot 0 in
	// particular: that is the singleton row, so a row that fell through to it would answer every singleton pull
	// with an unrelated instance's bytes.
	const TArray<uint8> Poison = { 0xDE, 0xAD };

	const FString Payload = FString::Printf(TEXT(
		R"({ "getVoxelList": { "coordinates": { "x": "12", "y": "0", "z": "0" }, "voxels": [
			{ "location": { "x": "7", "y": "0", "z": "0" }, "voxelType": "12", "state": "%s" },
			{ "location": { "x": "0", "y": "0", "z": "0" }, "voxelType": "12", "state": "%s" },
			{ "location": { "x": "5", "y": "0", "z": "0" }, "voxelType": "12" },
			{ "location": { "x": "6", "y": "0", "z": "0" }, "voxelType": "12", "state": "!!not base64!!" },
			{ "location": { "y": "0", "z": "0" }, "voxelType": "12", "state": "%s" },
			{ "location": { "x": null, "y": "0", "z": "0" }, "voxelType": "12", "state": "%s" },
			{ "location": { "x": "40000", "y": "0", "z": "0" }, "voxelType": "12", "state": "%s" },
			{ "voxelType": "12", "state": "%s" }
		] } })"),
		*EncodeState(SlotSeven), *EncodeState(Singleton),
		*EncodeState(Poison), *EncodeState(Poison), *EncodeState(Poison), *EncodeState(Poison));

	const CrowdyServiceApi::FVoxelChunkRead Read = CrowdyServiceApi::ReadVoxelChunk(MakeResult(Payload));

	TestTrue(TEXT("the chunk was read"), Read.bRead);
	TestEqual(TEXT("only the rows with a readable slot and a decodable state are kept"), Read.StateBySlot.Num(), 2);

	if (const TArray<uint8>* Bytes = Read.StateBySlot.Find(7))
	{
		TestEqual(TEXT("an instance slot keeps its own bytes"), *Bytes, SlotSeven);
	}
	else
	{
		AddError(TEXT("slot 7 is missing from the read"));
	}

	// Slot 0 is how a singleton struct is stored, so it must survive alongside the hashed instance slots, and it
	// must hold the row that actually claimed it rather than one whose slot could not be read.
	if (const TArray<uint8>* Bytes = Read.StateBySlot.Find(0))
	{
		TestEqual(TEXT("the singleton slot keeps its own bytes"), *Bytes, Singleton);
	}
	else
	{
		AddError(TEXT("the singleton slot is missing from the read"));
	}

	// An out-of-range slot must be refused rather than truncated into a real one: 40000 wraps to a valid int16
	// that names a live instance.
	TestFalse(TEXT("an out-of-range slot does not wrap onto a real one"),
		Read.StateBySlot.Contains(static_cast<int16>(40000)));

	const CrowdyServiceApi::FVoxelChunkRead Empty =
		CrowdyServiceApi::ReadVoxelChunk(MakeResult(TEXT(R"({ "getVoxelList": { "voxels": [] } })")));
	TestTrue(TEXT("a chunk with nothing stored still counts as read"), Empty.bRead);
	TestEqual(TEXT("and holds no state"), Empty.StateBySlot.Num(), 0);

	const CrowdyServiceApi::FVoxelChunkRead Unread = CrowdyServiceApi::ReadVoxelChunk(MakeCanceledResult());
	TestFalse(TEXT("a call that never landed is not a read"), Unread.bRead);

	const CrowdyServiceApi::FVoxelChunkRead NullList =
		CrowdyServiceApi::ReadVoxelChunk(MakeResult(TEXT(R"({ "getVoxelList": null })")));
	TestFalse(TEXT("a null chunk is not a read"), NullList.bRead);

	// An answer whose voxels field is missing has not said the chunk is empty. Reporting it as read would tell a
	// caller its slot is empty, and the normal response to that is to write defaults over state it never saw.
	const CrowdyServiceApi::FVoxelChunkRead NoList =
		CrowdyServiceApi::ReadVoxelChunk(MakeResult(TEXT(R"({ "getVoxelList": { "coordinates": { "x": "12" } } })")));
	TestFalse(TEXT("a chunk with no voxels field is not a read"), NoList.bRead);

	// A blob larger than any push could have produced is refused rather than handed to a struct deserializer.
	TArray<uint8> Oversized;
	Oversized.SetNumZeroed(CrowdyServiceApi::MaxVoxelStateBytes + 1);
	const CrowdyServiceApi::FVoxelChunkRead TooBig = CrowdyServiceApi::ReadVoxelChunk(MakeResult(FString::Printf(
		TEXT(R"({ "getVoxelList": { "voxels": [ { "location": { "x": "3" }, "state": "%s" } ] } })"),
		*EncodeState(Oversized))));
	TestTrue(TEXT("the oversized answer still counts as read"), TooBig.bRead);
	TestEqual(TEXT("but the oversized blob is refused"), TooBig.StateBySlot.Num(), 0);

	return true;
}

// The generated documents take a single input wrapper where the hand-written queries passed flat scalars, and every
// id is a BigInt the API wants as a string. Getting either wrong is rejected by the server rather than by the
// compiler, so the composition both mistakes would break is pinned here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAvatarVariableShapeTest,
	"CrowdySDK.CrowdyServices.AvatarVariableShape", ServiceTestFlags)
bool FCrowdyAvatarVariableShapeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), CrowdyServiceApi::BigInt(18750));
	Input->SetStringField(TEXT("avatarId"), CrowdyServiceApi::BigInt(42));
	Input->SetStringField(TEXT("state"), TEXT("QUJD"));

	const TSharedPtr<FJsonObject> Variables = CrowdyServiceApi::WrapInput(Input);

	const TSharedPtr<FJsonObject>* Wrapped = nullptr;
	if (!TestTrue(TEXT("the variables carry a single input object"),
		Variables->TryGetObjectField(TEXT("input"), Wrapped)))
	{
		return false;
	}

	FString AppId;
	TestTrue(TEXT("appId is inside the input"), (*Wrapped)->TryGetStringField(TEXT("appId"), AppId));
	TestEqual(TEXT("and is a BigInt string, never a number"), AppId, FString(TEXT("18750")));

	TestFalse(TEXT("nothing is left at the top level beside the input"),
		Variables->HasField(TEXT("appId")));

	// UpdateAvatar and UpdateAvatarState take the row id beside the input rather than inside it.
	TSharedPtr<FJsonObject> StateInput = MakeShared<FJsonObject>();
	StateInput->SetStringField(TEXT("publicState"), TEXT("AA=="));

	const TSharedPtr<FJsonObject> StateVariables = CrowdyServiceApi::WrapInput(StateInput);
	StateVariables->SetStringField(TEXT("id"), CrowdyServiceApi::BigInt(42));

	FString Id;
	TestTrue(TEXT("the row id sits beside the input"), StateVariables->TryGetStringField(TEXT("id"), Id));
	TestEqual(TEXT("and is a BigInt string"), Id, FString(TEXT("42")));

	const TSharedPtr<FJsonObject>* StateWrapped = nullptr;
	StateVariables->TryGetObjectField(TEXT("input"), StateWrapped);

	// A public-only update must not name privateState at all: naming it with an empty value would clear it.
	TestTrue(TEXT("a public-only update carries publicState"), (*StateWrapped)->HasField(TEXT("publicState")));
	TestFalse(TEXT("and leaves privateState unnamed"), (*StateWrapped)->HasField(TEXT("privateState")));

	return true;
}

// A level-placed actor is stored under its path, and a PIE world prefixes that path where a packaged build does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPersistenceInstanceKeyStripsPIEPrefixTest,
	"CrowdySDK.CrowdyServices.PersistenceInstanceKeyStripsPIEPrefix", ServiceTestFlags)
bool FCrowdyPersistenceInstanceKeyStripsPIEPrefixTest::RunTest(const FString& Parameters)
{
	UPackage* PlainPackage = CreatePackage(TEXT("/Temp/PersistKeyMap"));
	UPackage* PIEPackage = CreatePackage(TEXT("/Temp/UEDPIE_0_PersistKeyMap"));
	if (!TestNotNull(TEXT("both packages exist"), PlainPackage) || !TestNotNull(TEXT("both packages exist"), PIEPackage))
	{
		return false;
	}

	const FName SubjectName = MakeUniqueObjectName(PlainPackage, UObjectRedirector::StaticClass(), TEXT("Subject"));
	const UObject* Plain = NewObject<UObjectRedirector>(PlainPackage, SubjectName, RF_Transient);
	const UObject* InPIE = NewObject<UObjectRedirector>(PIEPackage, SubjectName, RF_Transient);

	const FString PlainKey = UCrowdyPersistenceSubsystem::DeriveInstanceKey(Plain);
	const FString PIEKey = UCrowdyPersistenceSubsystem::DeriveInstanceKey(InPIE);

	TestTrue(TEXT("the PIE object really has a PIE path"), InPIE->GetPathName().Contains(TEXT("UEDPIE_0_")));
	TestEqual(TEXT("the plain path is the key"), PlainKey, Plain->GetPathName());
	TestEqual(TEXT("a PIE path and the plain path derive the same key"), PIEKey, PlainKey);

	return true;
}

#endif
