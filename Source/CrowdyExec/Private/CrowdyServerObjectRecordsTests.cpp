#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SceneComponent.h"
#include "CrowdyExecInternal.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectComponent.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectSubsystem.h"
#include "CrowdyServerObjectTestRig.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

struct FCrowdyServerObjectRecordsTestAccess
{
	static void UseServerObjects(UCrowdyServerObjectComponent& Component, UCrowdyServerObjectSubsystem* Subsystem) { Component.ServerObjectsForTest = Subsystem; }
	static void Join(UCrowdyServerObjectComponent& Component) { Component.Join(); }
	static void TickFollow(UCrowdyServerObjectComponent& Component) { Component.TickFollow(); }
	static const FString& JoinError(const UCrowdyServerObjectComponent& Component) { return Component.JoinError; }
};

namespace CrowdyServerObjectRecordsTests
{
	using namespace CrowdyServerObjectTests;

	// What records_contract.rs asserts the hub sends: A (player 2) and a (player 1, Gold 5) pushed, player 1's read and changes, A removed.
	const TCHAR* const WirePush = TEXT("86a565706f6368cf000001998c91f600a373657101a47273657101a77265636f72647382a14183a66669656c647383a64865616c746814a54c6162656ca0a4526f777390a56f776e657202a47465616da4626c7565a16183a66669656c647383a64865616c74680aa54c6162656ca0a4526f777390a56f776e657201a47465616da3726564a772656d6f76656490aa72657374726963746564c3");
	const TCHAR* const WireRead = TEXT("86a8636f6e747261637402a565706f6368cf000001998c91f600a373657101a47273657101a77265636f72647382a14183a66669656c647383a64865616c746814a54c6162656ca0a4526f777390a56f776e657202a47465616da4626c7565a16183a66669656c647385a64865616c74680aa54c6162656ca0a4526f777390a4476f6c6405a4416d6d6f00a56f776e657201a47465616da3726564a46d6f7265c2");
	const TCHAR* const WireChanges = TEXT("83a47273657101a77265636f72647381a16181a66669656c647382a4476f6c6405a4416d6d6f00a46d6f7265c2");
	const TCHAR* const WireRemoval = TEXT("86a565706f6368cf000001998c91f600a373657102a47273657101a77265636f72647380a772656d6f76656491a141aa72657374726963746564c2");

	TArray<uint8> FromHex(const FString& Hex)
	{
		auto Digit = [](TCHAR Char) { return Char <= TEXT('9') ? Char - TEXT('0') : (Char | 0x20) - TEXT('a') + 10; };
		TArray<uint8> Bytes;
		for (int32 Index = 0; Index + 1 < Hex.Len(); Index += 2)
		{
			Bytes.Add(static_cast<uint8>(Digit(Hex[Index]) * 16 + Digit(Hex[Index + 1])));
		}
		return Bytes;
	}

	/** ck_exec_records' shape: Health, Label and Rows for everyone, Gold for the owner, Ammo for the team, Secret for the server. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeRecordsDefinition(FAutomationTestBase& Test)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Records(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Records->TypeName = TypeName;
		Records->Holds = ECrowdyServerObjectHolds::RecordPerEntity;
		Records->State = FCrowdyServerObjectTestRecord::StaticStruct();
		Records->WatchedFields = {TEXT("Health"), TEXT("Gold"), TEXT("Ammo"), TEXT("Label"), TEXT("Rows")};
		Records->OwnerFields = {TEXT("Gold")};
		Records->TeamFields = {TEXT("Ammo")};
		TArray<FString> Errors;
		const bool bBaked = Records->Bake(Errors);
		Test.TestTrue(FString::Printf(TEXT("the records definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
		return Records;
	}

	/** The rig with the records definition and player 1 signed in. */
	bool UseRecords(FRig& Rig)
	{
		Rig.Definition = MakeRecordsDefinition(Rig.Test);
		Rig.Session->SetUserID(1);
		return Rig.IsValid() && Rig.Definition->BakedStructs.Num() > 0;
	}

	void Text(FCrowdyExecWriter& Writer, const FString& Value)
	{
		Writer.String(Utf8Bytes(Value));
	}

	/** One record of a scripted message; an unset value is left out. */
	struct FRecordOut
	{
		FString Id;
		TOptional<int32> Health;
		TOptional<int32> Gold;
		TOptional<int32> Ammo;
		TOptional<int64> Owner;
		TOptional<FString> Team;
	};

	FRecordOut Full(const FString& Id, int32 Health, int64 Owner, const FString& Team)
	{
		return {Id, Health, {}, {}, Owner, Team};
	}

	FRecordOut HealthChange(const FString& Id, int32 Health)
	{
		return {Id, Health, {}, {}, {}, {}};
	}

	void WriteValue(FCrowdyExecWriter& Writer, const ANSICHAR* Name, const TOptional<int32>& Value)
	{
		if (Value.IsSet())
		{
			WriteKey(Writer, Name);
			Writer.Int(*Value);
		}
	}

	TArray<uint8> RecordsBytes(TConstArrayView<FRecordOut> Records)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(Records.Num());
		for (const FRecordOut& Record : Records)
		{
			Text(Writer, Record.Id);
			Writer.MapHeader(1 + (Record.Owner.IsSet() ? 1 : 0) + (Record.Team.IsSet() ? 1 : 0));
			WriteKey(Writer, "fields");
			Writer.MapHeader((Record.Health.IsSet() ? 1 : 0) + (Record.Gold.IsSet() ? 1 : 0) + (Record.Ammo.IsSet() ? 1 : 0));
			WriteValue(Writer, "Health", Record.Health);
			WriteValue(Writer, "Gold", Record.Gold);
			WriteValue(Writer, "Ammo", Record.Ammo);
			if (Record.Owner.IsSet())
			{
				WriteKey(Writer, "owner");
				Writer.UInt(static_cast<uint64>(*Record.Owner));
			}
			if (Record.Team.IsSet())
			{
				WriteKey(Writer, "team");
				Text(Writer, *Record.Team);
			}
		}
		return Bytes;
	}

	TArray<uint8> UIntBytes(uint64 Value)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter(Bytes).UInt(Value);
		return Bytes;
	}

	TArray<uint8> BoolBytes(bool bValue)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter(Bytes).Bool(bValue);
		return Bytes;
	}

	TArray<uint8> IdsBytes(TConstArrayView<FString> Ids)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.ArrayHeader(Ids.Num());
		for (const FString& Id : Ids)
		{
			Text(Writer, Id);
		}
		return Bytes;
	}

	using FEntry = TPair<const ANSICHAR*, TArray<uint8>>;

	/** A map of the given keys, each followed by its value's bytes as given. */
	TArray<uint8> Envelope(TConstArrayView<FEntry> Entries)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(Entries.Num());
		for (const FEntry& Entry : Entries)
		{
			WriteKey(Writer, Entry.Key);
			Bytes.Append(Entry.Value);
		}
		return Bytes;
	}

	TArray<uint8> ReadPage(uint64 Seq, uint64 RSeq, TConstArrayView<FRecordOut> Records, bool bMore, uint64 PageEpoch = Epoch)
	{
		return Envelope({FEntry("contract", UIntBytes(CrowdyExec::RecordsContractVersion)), FEntry("epoch", UIntBytes(PageEpoch)), FEntry("seq", UIntBytes(Seq)),
			FEntry("rseq", UIntBytes(RSeq)), FEntry("records", RecordsBytes(Records)), FEntry("more", BoolBytes(bMore))});
	}

	TArray<uint8> RecordsPush(uint64 Seq, TConstArrayView<FRecordOut> Records, TConstArrayView<FString> Removed = {}, bool bRestricted = false, uint64 PushEpoch = Epoch,
		uint64 RSeq = 0)
	{
		return Envelope({FEntry("epoch", UIntBytes(PushEpoch)), FEntry("seq", UIntBytes(Seq)), FEntry("rseq", UIntBytes(RSeq)), FEntry("records", RecordsBytes(Records)),
			FEntry("removed", IdsBytes(Removed)), FEntry("restricted", BoolBytes(bRestricted))});
	}

	/** A push at seq 1 whose records and removed values are the bytes given. */
	TArray<uint8> RawPush(const TArray<uint8>& Records, const TArray<uint8>& Removed = TArray<uint8>{0x90})
	{
		return Envelope({FEntry("epoch", UIntBytes(Epoch)), FEntry("seq", UIntBytes(1)), FEntry("rseq", UIntBytes(0)), FEntry("records", Records), FEntry("removed", Removed),
			FEntry("restricted", BoolBytes(false))});
	}

	TArray<uint8> ChangesPage(uint64 RSeq, TConstArrayView<FRecordOut> Records, bool bMore)
	{
		return Envelope({FEntry("rseq", UIntBytes(RSeq)), FEntry("records", RecordsBytes(Records)), FEntry("more", BoolBytes(bMore))});
	}

	FString TokenText(const FCrowdyExecToken& Token)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Token.Data), Token.Count);
	}

	/** The after and since a read or changes call asked with; Since stays 0 when the call does not carry it. */
	bool ParsePageRequest(const TArray<uint8>& Payload, FString& OutAfter, uint64& OutSince)
	{
		FCrowdyExecReader Reader(Payload);
		FCrowdyExecToken Map;
		FString Error;
		if (!Reader.Next(Map, Error) || Map.Type != ECrowdyExecToken::Map)
		{
			return false;
		}
		OutSince = 0;
		for (int32 Index = 0; Index < Map.Count; ++Index)
		{
			FCrowdyExecToken Key;
			FCrowdyExecToken Value;
			if (!Reader.Next(Key, Error) || !Reader.Next(Value, Error))
			{
				return false;
			}
			const FString Name = TokenText(Key);
			OutSince = Name == TEXT("since") ? Value.UInt : OutSince;
			OutAfter = Name == TEXT("after") ? TokenText(Value) : OutAfter;
		}
		return true;
	}

	/** Takes the next call of Method and checks the page it asks for. */
	bool ExpectPageCall(FRig& Rig, const TCHAR* Method, const FString& After, uint64 Since, FSentFrame& Out)
	{
		FString Asked;
		uint64 AskedSince = 0;
		if (!Rig.Test.TestTrue(FString::Printf(TEXT("a %s call is sent"), Method), Rig.WaitForFrame(KindCall, Method, Out))
			|| !Rig.Test.TestTrue(TEXT("its input is a page request"), ParsePageRequest(Out.Payload, Asked, AskedSince)))
		{
			return false;
		}
		Rig.Test.TestEqualSensitive(FString::Printf(TEXT("%s asks for the page after the last id"), Method), Asked, After);
		return Rig.Test.TestEqual(FString::Printf(TEXT("%s asks since the kept restricted seq"), Method), AskedSince, Since);
	}

	const FCrowdyServerObjectTestRecord* RecordOf(const UCrowdyServerObject* Object, const FString& Id)
	{
		const FCrowdyServerRecord* Record = Object ? Object->GetRecord(Id) : nullptr;
		return Record ? Record->Values.GetPtr<FCrowdyServerObjectTestRecord>() : nullptr;
	}

	int32 HealthIn(const UCrowdyServerObject* Object, const FString& Id)
	{
		const FCrowdyServerObjectTestRecord* Record = RecordOf(Object, Id);
		return Record ? Record->Health : -1;
	}

	/** Acknowledges the subscribe and answers the first read with one page. */
	bool AnswerRecordsRead(FRig& Rig, UCrowdyServerObject* Object, const TArray<uint8>& Page)
	{
		FSentFrame Subscribe;
		FSentFrame Read;
		if (!Rig.Test.TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
			|| !ExpectPageCall(Rig, TEXT("read"), FString(), 0, Read))
		{
			return false;
		}
		Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
		Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Page);
		return Rig.Test.TestTrue(TEXT("the read makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }));
	}

	UCrowdyServerObject* AcquireRecords(FRig& Rig, const UObject* Owner, const TArray<uint8>& Page)
	{
		UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner);
		return Object && Rig.Open() && AnswerRecordsRead(Rig, Object, Page) ? Object : nullptr;
	}

	struct FRecordsSpy
	{
		TSharedRef<TArray<TPair<TArray<FString>, TArray<FString>>>> Events = MakeShared<TArray<TPair<TArray<FString>, TArray<FString>>>>();

		void Listen(UCrowdyServerObject* Object) const
		{
			TSharedRef<TArray<TPair<TArray<FString>, TArray<FString>>>> Out = Events;
			Object->OnRecordsChanged.AddLambda([Out](UCrowdyServerObject*, TConstArrayView<FString> Changed, TConstArrayView<FString> Removed, TConstArrayView<TBitArray<>>)
			{
				Out->Emplace(TArray<FString>(Changed), TArray<FString>(Removed));
			});
		}
	};

	bool Decodes(const UCrowdyServerObjectDefinition& Definition, ECrowdyExecRecordsMessage Message, const TArray<uint8>& Bytes, FCrowdyExecRecordsMessage& Out, FString& Error)
	{
		return CrowdyExec::DecodeRecords(Definition, Message, Bytes, [](const FString&) -> const void* { return nullptr; }, Out, Error);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsWireFixturesTest,
	"CrowdySDK.CrowdyExec.RecordsWireFixtures", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsWireFixturesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeRecordsDefinition(*this);
	FString Error;

	FCrowdyExecRecordsMessage Push;
	if (!TestTrue(FString::Printf(TEXT("the hub's push decodes (%s)"), *Error), Decodes(*Definition, ECrowdyExecRecordsMessage::Push, FromHex(WirePush), Push, Error))
		|| !TestEqual(TEXT("with both records"), Push.Records.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("at the epoch"), Push.Epoch, Epoch);
	TestEqual(TEXT("and seq 1"), Push.Seq, 1ull);
	TestEqual(TEXT("carrying the restricted seq its send raised"), Push.RSeq, 1ull);
	TestTrue(TEXT("saying restricted values changed"), Push.bRestricted);
	TestTrue(TEXT("and removing nothing"), Push.Removed.IsEmpty());
	const FCrowdyExecRecordUpdate& Upper = Push.Records[0];
	const FCrowdyExecRecordUpdate& Lower = Push.Records[1];
	TestEqualSensitive(TEXT("A and a are two ids"), Upper.Id + Lower.Id, FString(TEXT("Aa")));
	TestEqual(TEXT("A's Health"), Upper.Values.Get<FCrowdyServerObjectTestRecord>().Health, 20);
	TestEqual(TEXT("a's Health"), Lower.Values.Get<FCrowdyServerObjectTestRecord>().Health, 10);
	TestTrue(TEXT("a new record carries its owner and team"), Upper.bHasOwner && Upper.bHasTeam && Upper.Owner == 2 && Upper.Team == TEXT("blue"));
	TestTrue(TEXT("each one its own"), Lower.Owner == 1 && Lower.Team.Equals(TEXT("red"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("a push carries the three Everyone values"), Lower.Fields.CountSetBits(), 3);

	FCrowdyExecRecordsMessage Read;
	if (!TestTrue(FString::Printf(TEXT("the hub's read page decodes (%s)"), *Error), Decodes(*Definition, ECrowdyExecRecordsMessage::Read, FromHex(WireRead), Read, Error))
		|| !TestEqual(TEXT("with both records"), Read.Records.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("its seq, restricted seq and last page"), Read.Seq == 1 && Read.RSeq == 1 && !Read.bMore);
	TestEqual(TEXT("the owner reads a's Gold"), Read.Records[1].Values.Get<FCrowdyServerObjectTestRecord>().Gold, 5);
	TestEqual(TEXT("and not A's"), Read.Records[0].Fields.CountSetBits(), 3);

	FCrowdyServerObjectTestRecord Stored;
	Stored.Health = 77;
	auto FindStored = [&Stored](const FString& Id) -> const void* { return Id.Equals(TEXT("a"), ESearchCase::CaseSensitive) ? &Stored : nullptr; };
	FCrowdyExecRecordsMessage Changes;
	if (!TestTrue(FString::Printf(TEXT("the hub's changes page decodes (%s)"), *Error),
		CrowdyExec::DecodeRecords(*Definition, ECrowdyExecRecordsMessage::Changes, FromHex(WireChanges), FindStored, Changes, Error))
		|| !TestEqual(TEXT("with a alone"), Changes.Records.Num(), 1))
	{
		return false;
	}
	const FCrowdyServerObjectTestRecord& Restricted = Changes.Records[0].Values.Get<FCrowdyServerObjectTestRecord>();
	TestTrue(TEXT("its Gold and Ammo, decoded over the stored record"), Restricted.Gold == 5 && Restricted.Ammo == 0 && Restricted.Health == 77);
	TestEqual(TEXT("carrying those two values only"), Changes.Records[0].Fields.CountSetBits(), 2);
	TestTrue(TEXT("at restricted seq 1, the last page"), Changes.RSeq == 1 && !Changes.bMore);

	FCrowdyExecRecordsMessage Removal;
	if (!TestTrue(FString::Printf(TEXT("the hub's removal push decodes (%s)"), *Error), Decodes(*Definition, ECrowdyExecRecordsMessage::Push, FromHex(WireRemoval), Removal, Error)))
	{
		return false;
	}
	TestTrue(TEXT("at seq 2, with no record and nothing restricted"), Removal.Seq == 2 && Removal.Records.IsEmpty() && !Removal.bRestricted);
	TestEqualSensitive(TEXT("removing A"), FString::Join(Removal.Removed, TEXT(",")), FString(TEXT("A")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsMalformedTest,
	"CrowdySDK.CrowdyExec.RecordsMalformedMessagesRefused", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsMalformedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeRecordsDefinition(*this);
	const TArray<FRecordOut> One = {Full(TEXT("a"), 10, 1, TEXT("red"))};
	auto Refused = [this, &Definition](const TCHAR* What, ECrowdyExecRecordsMessage Message, const TArray<uint8>& Bytes, const TCHAR* Reason)
	{
		FCrowdyExecRecordsMessage Out;
		Out.Seq = 77;
		FString Error;
		const bool bDecoded = Decodes(*Definition, Message, Bytes, Out, Error);
		TestFalse(FString::Printf(TEXT("%s is refused"), What), bDecoded);
		TestEqual(FString::Printf(TEXT("%s leaves the output as it was"), What), Out.Seq, 77ull);
		TestTrue(FString::Printf(TEXT("%s says why (%s)"), What, *Error), Error.Contains(Reason));
	};

	FCrowdyExecRecordsMessage Control;
	FString ControlError;
	TestTrue(FString::Printf(TEXT("the unmutated push decodes (%s)"), *ControlError), Decodes(*Definition, ECrowdyExecRecordsMessage::Push, RecordsPush(1, One), Control, ControlError));

	TArray<uint8> Huge = Envelope({FEntry("epoch", UIntBytes(Epoch)), FEntry("seq", UIntBytes(1)), FEntry("records", TArray<uint8>{0xde, 0x10, 0x01})});
	Huge.AddZeroed(5000);
	Refused(TEXT("a records map of 4097 entries"), ECrowdyExecRecordsMessage::Push, Huge, TEXT("over the limit of 4096"));
	Refused(TEXT("records as an array"), ECrowdyExecRecordsMessage::Push, RawPush(TArray<uint8>{0x90}), TEXT("expected a map of records"));
	Refused(TEXT("a push without restricted"), ECrowdyExecRecordsMessage::Push,
		Envelope({FEntry("epoch", UIntBytes(Epoch)), FEntry("seq", UIntBytes(1)), FEntry("rseq", UIntBytes(1)), FEntry("records", RecordsBytes(One)), FEntry("removed", TArray<uint8>{0x90})}),
		TEXT("carries no restricted"));
	Refused(TEXT("a push without rseq"), ECrowdyExecRecordsMessage::Push,
		Envelope({FEntry("epoch", UIntBytes(Epoch)), FEntry("seq", UIntBytes(1)), FEntry("records", RecordsBytes(One)), FEntry("removed", TArray<uint8>{0x90}),
			FEntry("restricted", BoolBytes(false))}),
		TEXT("carries no rseq"));
	Refused(TEXT("a read without its contract"), ECrowdyExecRecordsMessage::Read,
		Envelope({FEntry("epoch", UIntBytes(Epoch)), FEntry("seq", UIntBytes(1)), FEntry("rseq", UIntBytes(1)), FEntry("records", RecordsBytes(One)), FEntry("more", BoolBytes(false))}),
		TEXT("message format none"));
	Refused(TEXT("a read of One Set's format"), ECrowdyExecRecordsMessage::Read,
		Envelope({FEntry("contract", UIntBytes(1)), FEntry("epoch", UIntBytes(Epoch)), FEntry("seq", UIntBytes(1)), FEntry("rseq", UIntBytes(1)),
			FEntry("records", RecordsBytes(One)), FEntry("more", BoolBytes(false))}),
		TEXT("message format 1"));
	Refused(TEXT("a read record without owner and team"), ECrowdyExecRecordsMessage::Read, ReadPage(1, 1, {HealthChange(TEXT("a"), 3)}, false), TEXT("no owner or team"));
	Refused(TEXT("a doubled records key"), ECrowdyExecRecordsMessage::Changes,
		Envelope({FEntry("rseq", UIntBytes(1)), FEntry("records", RecordsBytes(One)), FEntry("records", RecordsBytes(One)), FEntry("more", BoolBytes(false))}),
		TEXT("appears twice"));
	Refused(TEXT("an id of 65 bytes"), ECrowdyExecRecordsMessage::Push, RecordsPush(1, {HealthChange(FString::ChrN(65, TEXT('k')), 1)}), TEXT("65 bytes"));
	Refused(TEXT("an id holding a control character"), ECrowdyExecRecordsMessage::Push, RecordsPush(1, {HealthChange(TEXT("a\nb"), 1)}), TEXT("control character"));
	Refused(TEXT("a removed id that is a number"), ECrowdyExecRecordsMessage::Push, RawPush(TArray<uint8>{0x80}, TArray<uint8>{0x91, 0x05}), TEXT("expected a record id"));

	TArray<uint8> OwnerText;
	FCrowdyExecWriter OwnerWriter(OwnerText);
	OwnerWriter.MapHeader(1);
	Text(OwnerWriter, TEXT("a"));
	OwnerWriter.MapHeader(2);
	WriteKey(OwnerWriter, "fields");
	OwnerWriter.MapHeader(0);
	WriteKey(OwnerWriter, "owner");
	Text(OwnerWriter, TEXT("one"));
	Refused(TEXT("an owner that is text"), ECrowdyExecRecordsMessage::Push, RawPush(OwnerText), TEXT("expected a user id"));

	TArray<uint8> HealthText;
	FCrowdyExecWriter HealthWriter(HealthText);
	HealthWriter.MapHeader(1);
	Text(HealthWriter, TEXT("a"));
	HealthWriter.MapHeader(1);
	WriteKey(HealthWriter, "fields");
	HealthWriter.MapHeader(1);
	WriteKey(HealthWriter, "Health");
	Text(HealthWriter, TEXT("lots"));
	Refused(TEXT("a Health that is text"), ECrowdyExecRecordsMessage::Push, RawPush(HealthText), TEXT("expected an integer"));

	// 17 rows of 4096 cells pass the 65,536 elements one message may hold.
	TArray<uint8> Grid;
	FCrowdyExecWriter GridWriter(Grid);
	GridWriter.MapHeader(1);
	Text(GridWriter, TEXT("a"));
	GridWriter.MapHeader(1);
	WriteKey(GridWriter, "fields");
	GridWriter.MapHeader(1);
	WriteKey(GridWriter, "Rows");
	GridWriter.ArrayHeader(17);
	for (int32 Row = 0; Row < 17; ++Row)
	{
		GridWriter.MapHeader(1);
		WriteKey(GridWriter, "Cells");
		GridWriter.ArrayHeader(CrowdyExec::MaxElements);
		Grid.AddZeroed(CrowdyExec::MaxElements);
	}
	Refused(TEXT("a record past the element budget"), ECrowdyExecRecordsMessage::Push, RawPush(Grid), TEXT("elements in one message"));

	TArray<uint8> Cut = FromHex(WirePush);
	Cut.Pop();
	Refused(TEXT("a push cut short"), ECrowdyExecRecordsMessage::Push, Cut, TEXT("ends early"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsPagedReadTest,
	"CrowdySDK.CrowdyExec.RecordsPagedReadReplaysPushesAboveTheFirstPage", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsPagedReadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame First;
	if (!Object || !Rig.Open() || !TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !ExpectPageCall(Rig, TEXT("read"), FString(), 0, First))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(5, 2, {Full(TEXT("a"), 10, 1, TEXT("red"))}, true));
	FSentFrame Second;
	if (!ExpectPageCall(Rig, TEXT("read"), TEXT("a"), 0, Second))
	{
		return false;
	}
	TestEqual(TEXT("an object still reading is not Ready"), Object->GetStatus(), ECrowdyServerObjectStatus::Connecting);

	// Seq 5 is the first page's own; 6 is below the last page's 9 but above the first's; 7 to 40 are more pushes than a read of one set buffers.
	Rig.Push(RecordsPush(5, {HealthChange(TEXT("z"), 99)}));
	Rig.Push(RecordsPush(6, {HealthChange(TEXT("a"), 77)}));
	for (uint64 Seq = 7; Seq <= 40; ++Seq)
	{
		Rig.Push(RecordsPush(Seq, {HealthChange(TEXT("b"), static_cast<int32>(Seq))}));
	}
	Rig.PumpFor(0.2);
	Rig.Reply(Second.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(9, 3, {Full(TEXT("b"), 7, 2, TEXT("red"))}, false));
	if (!TestTrue(TEXT("the last page makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	TestEqual(TEXT("both pages' records are held, and a push at the first page's seq is not replayed"), Object->NumRecords(), 2);
	TestEqual(TEXT("a push above the first page's seq is replayed over an earlier page, though below the last page's"), HealthIn(Object, TEXT("a")), 77);
	TestEqual(TEXT("every push above it is, in order, over the later page"), HealthIn(Object, TEXT("b")), 40);
	TestFalse(TEXT("35 buffered pushes do not overflow into another read"), Rig.SentAny(KindCall, TEXT("read")));
	Rig.Push(RecordsPush(41, {HealthChange(TEXT("b"), 41)}));
	TestTrue(TEXT("the next push follows the last replayed one"), Rig.PumpUntil([Object]() { return HealthIn(Object, TEXT("b")) == 41; }));
	TestFalse(TEXT("without a read"), Rig.SentAny(KindCall, TEXT("read")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsEpochBetweenPagesTest,
	"CrowdySDK.CrowdyExec.RecordsEpochChangeBetweenPagesStartsOver", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsEpochBetweenPagesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame First;
	if (!Object || !Rig.Open() || !TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !ExpectPageCall(Rig, TEXT("read"), FString(), 0, First))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(5, 2, {Full(TEXT("a"), 10, 1, TEXT("red"))}, true));
	FSentFrame Second;
	if (!ExpectPageCall(Rig, TEXT("read"), TEXT("a"), 0, Second))
	{
		return false;
	}
	// The server restarted between the pages: its record a is gone, so a page of the new epoch cannot be joined to the old.
	Rig.Reply(Second.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(1, 0, {Full(TEXT("b"), 7, 2, TEXT("red"))}, false, Epoch + 1));
	FSentFrame Again;
	if (!TestTrue(TEXT("the read starts over from the first record"), ExpectPageCall(Rig, TEXT("read"), FString(), 0, Again)))
	{
		return false;
	}
	Rig.Reply(Again.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(1, 0, {Full(TEXT("b"), 7, 2, TEXT("red"))}, false, Epoch + 1));
	if (!TestTrue(TEXT("the restarted read makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	TestEqual(TEXT("only the new epoch's records are held"), Object->NumRecords(), 1);
	TestNull(TEXT("the old epoch's record is not"), Object->GetRecord(TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsGapAndEpochTest,
	"CrowdySDK.CrowdyExec.RecordsSeqGapAndEpochChangeReread", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsGapAndEpochTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(3, 1, {Full(TEXT("a"), 10, 1, TEXT("red"))}, false));
	if (!Object)
	{
		return false;
	}
	Rig.Push(RecordsPush(5, {HealthChange(TEXT("a"), 50)}));
	FSentFrame Reread;
	if (!ExpectPageCall(Rig, TEXT("read"), FString(), 0, Reread))
	{
		return false;
	}
	TestEqual(TEXT("a push past a gap is not applied"), HealthIn(Object, TEXT("a")), 10);
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(5, 1, {Full(TEXT("a"), 50, 1, TEXT("red"))}, false));
	Rig.PumpUntil([Object]() { return HealthIn(Object, TEXT("a")) == 50; });

	Rig.Push(RecordsPush(6, {HealthChange(TEXT("a"), 60)}, {}, false, Epoch + 1));
	if (!ExpectPageCall(Rig, TEXT("read"), FString(), 0, Reread))
	{
		return false;
	}
	TestEqual(TEXT("nor one of a new epoch"), HealthIn(Object, TEXT("a")), 50);
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(1, 1, {Full(TEXT("a"), 60, 1, TEXT("red"))}, false, Epoch + 1));
	TestTrue(TEXT("the new epoch's read is applied"), Rig.PumpUntil([Object]() { return HealthIn(Object, TEXT("a")) == 60; }));
	Rig.Push(RecordsPush(2, {HealthChange(TEXT("a"), 61)}, {}, false, Epoch + 1));
	TestTrue(TEXT("and its pushes follow"), Rig.PumpUntil([Object]() { return HealthIn(Object, TEXT("a")) == 61; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsChangesLimiterTest,
	"CrowdySDK.CrowdyExec.RecordsRestrictedChangesAreLimited", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsChangesLimiterTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(1, 4, {Full(TEXT("a"), 10, 1, TEXT("red")), Full(TEXT("b"), 20, 2, TEXT("red"))}, false));
	if (!Object)
	{
		return false;
	}
	Rig.Push(RecordsPush(2, {}, {}, true));
	FSentFrame First;
	if (!ExpectPageCall(Rig, TEXT("changes"), FString(), 4, First))
	{
		return false;
	}
	Rig.Push(RecordsPush(3, {}, {}, true));
	TestFalse(TEXT("a second restricted push while one is in flight sends nothing"), Rig.SentAny(KindCall, TEXT("changes")));

	FRecordOut Gold = {TEXT("a"), {}, 9, {}, {}, {}};
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Ok, ChangesPage(6, {Gold}, true));
	FSentFrame Next;
	if (!ExpectPageCall(Rig, TEXT("changes"), TEXT("a"), 4, Next))
	{
		return false;
	}
	FRecordOut Ammo = {TEXT("b"), {}, {}, 3, {}, {}};
	Rig.Reply(Next.Rid, ECrowdyNativeExecStatus::Ok, ChangesPage(7, {Ammo}, false));
	Rig.PumpUntil([Object]() { const FCrowdyServerObjectTestRecord* B = RecordOf(Object, TEXT("b")); return B && B->Ammo == 3; });
	const FCrowdyServerObjectTestRecord* A = RecordOf(Object, TEXT("a"));
	TestTrue(TEXT("the pages' values land over the stored records"), A && A->Gold == 9 && A->Health == 10);

	Rig.Tick(0.5f);
	TestFalse(TEXT("the waiting request is not sent within a second of the last"), Rig.SentAny(KindCall, TEXT("changes")));
	Rig.Tick(0.6f);
	FSentFrame Coalesced;
	ExpectPageCall(Rig, TEXT("changes"), FString(), 6, Coalesced);
	TestFalse(TEXT("and both restricted pushes made one call"), Rig.SentAny(KindCall, TEXT("changes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsOwnTeamTest,
	"CrowdySDK.CrowdyExec.RecordsOwnTeamChangeRereads", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsOwnTeamTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(1, 1, {Full(TEXT("a"), 10, 1, TEXT("red")), Full(TEXT("b"), 20, 2, TEXT("red"))}, false));
	if (!Object)
	{
		return false;
	}
	FRecordOut Other = {TEXT("b"), {}, {}, {}, {}, FString(TEXT("blue"))};
	Rig.Push(RecordsPush(2, {Other}));
	Rig.PumpUntil([Object]() { return Object->GetRecord(TEXT("b")) && Object->GetRecord(TEXT("b"))->Team == TEXT("blue"); });
	TestFalse(TEXT("another player's team change reads nothing again"), Rig.SentAny(KindCall, TEXT("read")));

	FRecordOut Own = {TEXT("a"), {}, {}, {}, {}, FString(TEXT("blue"))};
	Rig.Push(RecordsPush(3, {Own}));
	FSentFrame Reread;
	ExpectPageCall(Rig, TEXT("read"), FString(), 0, Reread);
	TestTrue(TEXT("the player's own new team is kept"), Object->GetRecord(TEXT("a")) && Object->GetRecord(TEXT("a"))->Team.Equals(TEXT("blue"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsRemovalAndCaseTest,
	"CrowdySDK.CrowdyExec.RecordsRemovalKeepsIdsDifferingInCase", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsRemovalAndCaseTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), FromHex(WireRead));
	if (!Object || !TestEqual(TEXT("the hub's page holds A and a apart"), Object->NumRecords(), 2))
	{
		return false;
	}
	TestEqual(TEXT("A is player 2's"), HealthIn(Object, TEXT("A")), 20);
	TestEqual(TEXT("a is player 1's"), HealthIn(Object, TEXT("a")), 10);
	const FCrowdyServerRecord* Lower = Object->GetRecord(TEXT("a"));
	TestTrue(TEXT("with its owner, team and Owner Only Gold"), Lower && Lower->Owner == 1 && Lower->Team == TEXT("red") && RecordOf(Object, TEXT("a"))->Gold == 5);
	FInstancedStruct Values;
	int64 RecordOwner = 0;
	FString Team;
	TestTrue(TEXT("Find Record finds A"), Object->K2_FindRecord(TEXT("A"), Values, RecordOwner, Team) && RecordOwner == 2 && Team == TEXT("blue"));

	FRecordsSpy Spy;
	Spy.Listen(Object);
	Rig.Push(FromHex(WireRemoval));
	if (!TestTrue(TEXT("the hub's removal removes A"), Rig.PumpUntil([Object]() { return Object->GetRecord(TEXT("A")) == nullptr; })))
	{
		return false;
	}
	TestEqual(TEXT("and not a"), HealthIn(Object, TEXT("a")), 10);
	TestEqualSensitive(TEXT("Get Record Ids lists a alone"), FString::Join(Object->GetRecordIds(), TEXT(",")), FString(TEXT("a")));
	if (!TestEqual(TEXT("one change is announced"), Spy.Events->Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("changing no record"), (*Spy.Events)[0].Key.IsEmpty());
	TestEqualSensitive(TEXT("and removing A"), FString::Join((*Spy.Events)[0].Value, TEXT(",")), FString(TEXT("A")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsComponentTest,
	"CrowdySDK.CrowdyExec.RecordsComponentReadsItsEntitysRecord", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsComponentTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	AddExpectedMessagePlain(TEXT("could not join"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<AActor> Actor(NewObject<AActor>(GetTransientPackage()));
	TStrongObjectPtr<UCrowdyEntityComponent> Entity(NewObject<UCrowdyEntityComponent>(Actor.Get()));
	const FGuid NetId(0x0A0B0C0D, 0x11223344, 0x55667788, 0x99AABBCC);
	Entity->AssignPooledIdentity(NetId, FGuid(1, 2, 3, 4), ECrowdyRole::Owner, 0);
	const FString Id = NetId.ToString(EGuidFormats::Digits);
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component(NewObject<UCrowdyServerObjectComponent>(Actor.Get()));
	Component->Definition = Rig.Definition.Get();
	Component->InstanceId = Rig.InstanceId;
	FCrowdyServerObjectRecordsTestAccess::UseServerObjects(*Component, Rig.Subsystem.Get());
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener(NewObject<UCrowdyServerObjectTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	Component->OnVariablesChanged.AddDynamic(Listener.Get(), &UCrowdyServerObjectTestListener::HandleVariables);

	FCrowdyServerObjectRecordsTestAccess::Join(*Component);
	UCrowdyServerObject* Object = Component->GetServerObject();
	if (!TestNotNull(TEXT("the component holds the records object"), Object) || !TestEqualSensitive(TEXT("its record is its entity's 32-digit id"), Component->GetRecordId(), Id)
		|| !Rig.Open() || !AnswerRecordsRead(Rig, Object, ReadPage(1, 1, {Full(Id, 10, 1, TEXT("red")), Full(TEXT("other"), 5, 2, TEXT("red"))}, false)))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("the first read names every variable once (got %d events)"), Listener->VariableEvents.Num()),
		Listener->VariableEvents.Num() == 1 && NamesAre(Listener->VariableEvents[0], {TEXT("Health"), TEXT("Gold"), TEXT("Ammo"), TEXT("Label"), TEXT("Rows")}));
	Rig.Push(RecordsPush(2, {HealthChange(TEXT("other"), 6)}));
	Rig.PumpUntil([Object]() { return HealthIn(Object, TEXT("other")) == 6; });
	TestEqual(TEXT("another entity's change is not this actor's"), Listener->VariableEvents.Num(), 1);
	Rig.Push(RecordsPush(3, {HealthChange(Id, 7)}));
	Rig.PumpUntil([&Listener]() { return Listener->VariableEvents.Num() == 2; });
	TestTrue(TEXT("its own change names only Health"), Listener->VariableEvents.Num() == 2 && NamesAre(Listener->VariableEvents[1], {TEXT("Health")}));

	const FProperty* Count = FindFProperty<FProperty>(FCrowdyServerObjectTestPins::StaticStruct(), TEXT("Count"));
	int32 Health = 0;
	TestTrue(TEXT("a per-variable read gets the record's Health"), UCrowdyServerObjectLibrary::ReadServerVariableInto(Component.Get(), Rig.Definition.Get(), TEXT("Health"), Count, &Health) && Health == 7);
	const FInstancedStruct Variables = Component->GetVariables();
	const FCrowdyServerObjectTestRecord* Read = Variables.GetPtr<FCrowdyServerObjectTestRecord>();
	TestTrue(TEXT("and Get Variables the record"), Read && Read->Health == 7);

	Rig.Push(RecordsPush(4, {}, {Id}));
	Rig.PumpUntil([&Listener]() { return Listener->VariableEvents.Num() == 3; });
	TestEqual(TEXT("its removal is a change too"), Listener->VariableEvents.Num(), 3);
	TestFalse(TEXT("after which there is nothing to read"), UCrowdyServerObjectLibrary::ReadServerVariableInto(Component.Get(), Rig.Definition.Get(), TEXT("Health"), Count, &Health));

	TStrongObjectPtr<AActor> Bare(NewObject<AActor>(GetTransientPackage()));
	TStrongObjectPtr<UCrowdyServerObjectComponent> Lone(NewObject<UCrowdyServerObjectComponent>(Bare.Get()));
	Lone->Definition = Rig.Definition.Get();
	Lone->InstanceId = Rig.InstanceId;
	FCrowdyServerObjectRecordsTestAccess::UseServerObjects(*Lone, Rig.Subsystem.Get());
	FCrowdyServerObjectRecordsTestAccess::Join(*Lone);
	TestNull(TEXT("an actor with no entity holds nothing"), Lone->GetServerObject());
	TestTrue(TEXT("and says it needs a Crowdy Entity component"), FCrowdyServerObjectRecordsTestAccess::JoinError(*Lone).Contains(TEXT("Crowdy Entity component")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectAreaFollowsTheActorTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.ThisActorsAreaFollowsTheActor", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectAreaFollowsTheActorTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	Rig.Session->SetChunkSize(1600.0);
	TStrongObjectPtr<AActor> Actor(NewObject<AActor>(GetTransientPackage()));
	USceneComponent* Root = NewObject<USceneComponent>(Actor.Get());
	Actor->SetRootComponent(Root);
	auto MoveTo = [Root](const FVector& Location)
	{
		Root->SetRelativeLocation_Direct(Location);
		Root->UpdateComponentToWorld();
	};
	MoveTo(FVector(7000.0, -100.0, 99999.0));
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component(NewObject<UCrowdyServerObjectComponent>(Actor.Get()));
	Component->Definition = Rig.Definition.Get();
	Component->InstanceMode = ECrowdyServerObjectInstanceMode::ThisActorsArea;
	FCrowdyServerObjectRecordsTestAccess::UseServerObjects(*Component, Rig.Subsystem.Get());

	FString Error;
	TestEqualSensitive(TEXT("chunk 4,-1 lies in area 1,-1 of 4 chunks, whatever the height"), Component->ResolveInstanceId(Error), FString(TEXT("1,-1")));
	TestTrue(FString::Printf(TEXT("with no error (%s)"), *Error), Error.IsEmpty());
	FCrowdyServerObjectRecordsTestAccess::Join(*Component);
	TWeakObjectPtr<UCrowdyServerObject> First = Component->GetServerObject();
	if (!TestTrue(TEXT("the component holds that area's object"), First.IsValid() && First->GetInstanceId() == TEXT("1,-1")) || !Rig.Open())
	{
		return false;
	}
	FCrowdyServerObjectRecordsTestAccess::TickFollow(*Component);
	TestTrue(TEXT("an actor that stays keeps its object"), Component->GetServerObject() == First.Get());

	MoveTo(FVector(-1.0, -100.0, 0.0));
	FCrowdyServerObjectRecordsTestAccess::TickFollow(*Component);
	UCrowdyServerObject* Second = Component->GetServerObject();
	TestTrue(TEXT("a move to chunk -1,-1 follows into area -1,-1"), Second && Second != First.Get() && Second->GetInstanceId() == TEXT("-1,-1"));
	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestTrue(TEXT("the area left behind is given back after the grace period"), !First.IsValid() || First->GetStatus() == ECrowdyServerObjectStatus::Released);
	TestTrue(TEXT("and the new one kept"), Second && Second->GetStatus() != ECrowdyServerObjectStatus::Released);
	return true;
}

namespace CrowdyServerObjectRecordsTests
{
	/** Records holding State, watching Watched, with no Owner Only or Team Only values. */
	bool RebakeRecords(FRig& Rig, UScriptStruct* NewState, const TArray<FName>& Watched)
	{
		Rig.Definition->State = NewState;
		Rig.Definition->WatchedFields = Watched;
		Rig.Definition->OwnerFields.Reset();
		Rig.Definition->TeamFields.Reset();
		TArray<FString> Errors;
		const bool bBaked = Rig.Definition->Bake(Errors);
		return Rig.Test.TestTrue(FString::Printf(TEXT("the reshaped definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	/** Collects the names each change of one record announces to its watchers. */
	struct FRecordWatch
	{
		TSharedRef<TArray<TArray<FString>>> Events = MakeShared<TArray<TArray<FString>>>();

		void Watch(UCrowdyServerObject* Object, const FString& Id) const
		{
			TSharedRef<TArray<TArray<FString>>> Out = Events;
			Object->WatchRecord(Id, FOnCrowdyServerValuesChanged::FDelegate::CreateLambda([Out](UCrowdyServerObject*, TConstArrayView<FString> Fields)
			{
				Out->Emplace(Fields);
			}));
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsChangesAheadOfPushTest,
	"CrowdySDK.CrowdyExec.RecordsChangesForARecordNotYetPushedAskAgain", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsChangesAheadOfPushTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(1, 4, {Full(TEXT("a"), 10, 1, TEXT("red"))}, false));
	FSentFrame First;
	if (!Object)
	{
		return false;
	}
	Rig.Push(RecordsPush(2, {}, {}, true));
	if (!ExpectPageCall(Rig, TEXT("changes"), FString(), 4, First))
	{
		return false;
	}
	FRecordOut GoldA = {TEXT("a"), {}, 9, {}, {}, {}};
	FRecordOut GoldN = {TEXT("n"), {}, 5, {}, {}, {}};
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Ok, ChangesPage(6, {GoldA, GoldN}, false));
	Rig.PumpUntil([Object]() { const FCrowdyServerObjectTestRecord* A = RecordOf(Object, TEXT("a")); return A && A->Gold == 9; });
	TestNull(TEXT("a record not pushed yet is not made from changes"), Object->GetRecord(TEXT("n")));

	// The push that makes n carries the restricted seq n's values changed at, as the server's does.
	Rig.Push(RecordsPush(3, {Full(TEXT("n"), 30, 1, TEXT("red"))}, {}, false, Epoch, 6));
	Rig.PumpUntil([Object]() { return Object->GetRecord(TEXT("n")) != nullptr; });
	Rig.Tick(1.1f);
	FSentFrame Again;
	if (!TestTrue(TEXT("changes are asked for again from the same restricted seq"), ExpectPageCall(Rig, TEXT("changes"), FString(), 4, Again)))
	{
		return false;
	}
	Rig.Reply(Again.Rid, ECrowdyNativeExecStatus::Ok, ChangesPage(6, {GoldN}, false));
	TestTrue(TEXT("and the late record gets its Owner Only value"), Rig.PumpUntil([Object]() { const FCrowdyServerObjectTestRecord* N = RecordOf(Object, TEXT("n")); return N && N->Gold == 5; }));
	Rig.Tick(1.1f);
	TestFalse(TEXT("after which nothing more is asked"), Rig.SentAny(KindCall, TEXT("changes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsOwnerChangeClearsRestrictedTest,
	"CrowdySDK.CrowdyExec.RecordsOwnerChangeClearsRestrictedValues", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsOwnerChangeClearsRestrictedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	FRecordOut Mine = {TEXT("a"), 10, 9, 4, 1, FString(TEXT("red"))};
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(1, 1, {Mine}, false));
	if (!Object || !TestTrue(TEXT("the owner reads its Gold and Ammo"), RecordOf(Object, TEXT("a")) && RecordOf(Object, TEXT("a"))->Gold == 9))
	{
		return false;
	}
	FRecordWatch Watch;
	Watch.Watch(Object, TEXT("a"));
	FRecordOut Given = {TEXT("a"), {}, {}, {}, 2, {}};
	Rig.Push(RecordsPush(2, {Given}, {}, true));
	if (!TestTrue(TEXT("the record changes hands"), Rig.PumpUntil([Object]() { return Object->GetRecord(TEXT("a")) && Object->GetRecord(TEXT("a"))->Owner == 2; })))
	{
		return false;
	}
	const FCrowdyServerObjectTestRecord* A = RecordOf(Object, TEXT("a"));
	TestTrue(TEXT("its Owner Only and Team Only values go back to their defaults"), A && A->Gold == 0 && A->Ammo == 0);
	TestTrue(TEXT("leaving its Everyone values"), A && A->Health == 10);
	TestTrue(TEXT("and its watchers hear of both, after the first call that named every value"), Watch.Events->Num() == 2 && NamesAre((*Watch.Events)[1], {TEXT("Gold"), TEXT("Ammo")}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsDeepPushTest,
	"CrowdySDK.CrowdyExec.RecordsPushOfADeepStateApplies", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsDeepPushTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig) || !RebakeRecords(Rig, FCrowdyServerObjectTestDeepRecord::StaticStruct(), {TEXT("Health"), TEXT("Deep")}))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(1, 0, {Full(TEXT("a"), 10, 1, TEXT("red"))}, false));
	if (!Object)
	{
		return false;
	}
	FCrowdyServerObjectTestDeepRecord Deepest;
	Deepest.Health = 11;
	FCrowdyExecTestDepth6& L6 = Deepest.Deep.Items.AddDefaulted_GetRef();
	FCrowdyExecTestDepth5& L5 = L6.Items.AddDefaulted_GetRef();
	FCrowdyExecTestDepth4& L4 = L5.Items.AddDefaulted_GetRef();
	FCrowdyExecTestDepth3& L3 = L4.Items.AddDefaulted_GetRef();
	FCrowdyExecTestDepth2& L2 = L3.Items.AddDefaulted_GetRef();
	L2.Items.AddDefaulted_GetRef().Items.Add(FVector(1.0, 2.0, 3.0));
	const TArray<uint8> Fields = Rig.Encode(Deepest);

	TArray<uint8> Records;
	FCrowdyExecWriter Writer(Records);
	Writer.MapHeader(1);
	Text(Writer, TEXT("a"));
	Writer.MapHeader(1);
	WriteKey(Writer, "fields");
	Records.Append(Fields);
	Rig.Push(Envelope({FEntry("epoch", UIntBytes(Epoch)), FEntry("seq", UIntBytes(2)), FEntry("rseq", UIntBytes(0)), FEntry("records", Records), FEntry("removed", TArray<uint8>{0x90}),
		FEntry("restricted", BoolBytes(false))}));
	auto Applied = [Object]()
	{
		const FCrowdyServerRecord* Record = Object->GetRecord(TEXT("a"));
		const FCrowdyServerObjectTestDeepRecord* A = Record ? Record->Values.GetPtr<FCrowdyServerObjectTestDeepRecord>() : nullptr;
		return A && A->Health == 11 && A->Deep.Items.Num() == 1;
	};
	TestTrue(TEXT("a push 16 levels deep in the State applies"), Rig.PumpUntil(Applied));
	TestFalse(TEXT("without a read"), Rig.SentAny(KindCall, TEXT("read")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsChangesBackoffTest,
	"CrowdySDK.CrowdyExec.RecordsFailedChangesBackOffOrFail", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsChangesBackoffTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(1, 4, {Full(TEXT("a"), 10, 1, TEXT("red"))}, false));
	FSentFrame Call;
	if (!Object)
	{
		return false;
	}
	Rig.Push(RecordsPush(2, {}, {}, true));
	if (!ExpectPageCall(Rig, TEXT("changes"), FString(), 4, Call))
	{
		return false;
	}
	Rig.Reply(Call.Rid, ECrowdyNativeExecStatus::Internal, Utf8Bytes(TEXT("internal")));
	Rig.PumpFor(0.1);
	Rig.Tick(1.1f);
	if (!ExpectPageCall(Rig, TEXT("changes"), FString(), 4, Call))
	{
		return false;
	}
	Rig.Reply(Call.Rid, ECrowdyNativeExecStatus::Internal, Utf8Bytes(TEXT("internal")));
	Rig.PumpFor(0.1);
	Rig.Tick(1.1f);
	TestFalse(TEXT("a second failure waits two seconds, not one"), Rig.SentAny(KindCall, TEXT("changes")));
	Rig.Tick(1.f);
	if (!ExpectPageCall(Rig, TEXT("changes"), FString(), 4, Call))
	{
		return false;
	}
	Rig.Reply(Call.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("denied: not yours")));
	TestTrue(TEXT("a refusal that retrying cannot change fails the object"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Failed; }));
	Rig.Tick(5.f);
	TestFalse(TEXT("and asks nothing more"), Rig.SentAny(KindCall, TEXT("changes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsEpochRestartDropsPushesTest,
	"CrowdySDK.CrowdyExec.RecordsEpochRestartDropsTheOldEpochsPushes", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsEpochRestartDropsPushesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	FSentFrame Subscribe;
	FSentFrame First;
	if (!Object || !Rig.Open() || !TestTrue(TEXT("the object subscribes"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
		|| !ExpectPageCall(Rig, TEXT("read"), FString(), 0, First))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(5, 2, {Full(TEXT("a"), 10, 1, TEXT("red"))}, true));
	FSentFrame Second;
	if (!ExpectPageCall(Rig, TEXT("read"), TEXT("a"), 0, Second))
	{
		return false;
	}
	Rig.Push(RecordsPush(6, {HealthChange(TEXT("a"), 60)}));
	Rig.PumpFor(0.2);
	Rig.Reply(Second.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(1, 0, {Full(TEXT("b"), 7, 2, TEXT("red"))}, false, Epoch + 1));
	FSentFrame Again;
	if (!ExpectPageCall(Rig, TEXT("read"), FString(), 0, Again))
	{
		return false;
	}
	Rig.Reply(Again.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(1, 0, {Full(TEXT("b"), 7, 2, TEXT("red"))}, false, Epoch + 1));
	if (!TestTrue(TEXT("the restarted read makes the object Ready"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; })))
	{
		return false;
	}
	TestFalse(TEXT("a push of the old epoch is not replayed into another read"), Rig.SentAny(KindCall, TEXT("read")));
	TestNull(TEXT("nor applied"), Object->GetRecord(TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsFollowIsGuardedTest,
	"CrowdySDK.CrowdyExec.RecordsComponentFollowDoesNotJoinInside", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsFollowIsGuardedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<AActor> Actor(NewObject<AActor>(GetTransientPackage()));
	TStrongObjectPtr<UCrowdyEntityComponent> Entity(NewObject<UCrowdyEntityComponent>(Actor.Get()));
	const FGuid FirstId(1, 1, 1, 1);
	const FGuid SecondId(2, 2, 2, 2);
	Entity->AssignPooledIdentity(FirstId, FGuid(9, 9, 9, 9), ECrowdyRole::Owner, 0);
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component(NewObject<UCrowdyServerObjectComponent>(Actor.Get()));
	Component->Definition = Rig.Definition.Get();
	Component->InstanceId = Rig.InstanceId;
	FCrowdyServerObjectRecordsTestAccess::UseServerObjects(*Component, Rig.Subsystem.Get());
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Listener(NewObject<UCrowdyServerObjectTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	Component->OnVariablesChanged.AddDynamic(Listener.Get(), &UCrowdyServerObjectTestListener::HandleVariables);
	FCrowdyServerObjectRecordsTestAccess::Join(*Component);
	UCrowdyServerObject* Object = Component->GetServerObject();
	const FString Second = SecondId.ToString(EGuidFormats::Digits);
	if (!Object || !Rig.Open()
		|| !AnswerRecordsRead(Rig, Object, ReadPage(1, 1, {Full(FirstId.ToString(EGuidFormats::Digits), 10, 1, TEXT("red")), Full(Second, 20, 1, TEXT("red"))}, false)))
	{
		return false;
	}

	// A handler of the new record's first values joins again, as a Rejoin from Blueprint would.
	bool bJoinInside = true;
	UCrowdyServerObjectComponent* Held = Component.Get();
	Listener->OnVariables = [&bJoinInside, Held]()
	{
		if (bJoinInside)
		{
			bJoinInside = false;
			FCrowdyServerObjectRecordsTestAccess::Join(*Held);
		}
	};
	Entity->AssignPooledIdentity(SecondId, FGuid(9, 9, 9, 9), ECrowdyRole::Owner, 0);
	FCrowdyServerObjectRecordsTestAccess::TickFollow(*Component);
	TestEqualSensitive(TEXT("the component follows its entity's new id"), Component->GetRecordId(), Second);
	const int32 Before = Listener->VariableEvents.Num();
	Rig.Push(RecordsPush(2, {HealthChange(Second, 21)}));
	Rig.PumpUntil([Object, &Second]() { return HealthIn(Object, Second) == 21; });
	Rig.PumpFor(0.1);
	TestEqual(TEXT("a change of the record is announced once, so no watch was left behind"), Listener->VariableEvents.Num() - Before, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsChangesAfterReshapeTest,
	"CrowdySDK.CrowdyExec.RecordsChangesAfterAReshapedStateReread", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsChangesAfterReshapeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(1, 4, {Full(TEXT("a"), 10, 1, TEXT("red"))}, false));
	FSentFrame Call;
	if (!Object)
	{
		return false;
	}
	Rig.Push(RecordsPush(2, {}, {}, true));
	if (!ExpectPageCall(Rig, TEXT("changes"), FString(), 4, Call) || !RebakeRecords(Rig, FCrowdyExecTestBossState::StaticStruct(), {TEXT("Health"), TEXT("bDefeated")}))
	{
		return false;
	}
	FRecordOut Gold = {TEXT("a"), {}, 9, {}, {}, {}};
	Rig.Reply(Call.Rid, ECrowdyNativeExecStatus::Ok, ChangesPage(6, {Gold}, false));
	FSentFrame Reread;
	TestTrue(TEXT("changes for a State edited since the read read again"), ExpectPageCall(Rig, TEXT("read"), FString(), 0, Reread));
	const FCrowdyServerRecord* A = Object->GetRecord(TEXT("a"));
	TestTrue(TEXT("leaving the held record in its own shape"), A && A->Values.GetScriptStruct() == FCrowdyServerObjectTestRecord::StaticStruct() && RecordOf(Object, TEXT("a"))->Gold == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsChangesOvertakeOwnerChangeTest,
	"CrowdySDK.CrowdyExec.RecordsChangesOvertakingAnOwnerChangeStillRestoreItsValue", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsChangesOvertakeOwnerChangeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	// Player 1 already owns b on red, so taking a on red later leaves its own team as it was.
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(1, 1, {Full(TEXT("a"), 10, 2, TEXT("red")), Full(TEXT("b"), 20, 1, TEXT("red"))}, false));
	FSentFrame First;
	if (!Object)
	{
		return false;
	}
	Rig.Push(RecordsPush(2, {}, {}, true, Epoch, 1));
	if (!ExpectPageCall(Rig, TEXT("changes"), FString(), 1, First))
	{
		return false;
	}
	// The server gives a to player 1 at restricted seq 2 and answers the changes after that, before the push saying so arrives.
	FRecordOut Gold = {TEXT("a"), {}, 9, {}, {}, {}};
	Rig.Reply(First.Rid, ECrowdyNativeExecStatus::Ok, ChangesPage(2, {Gold}, false));
	Rig.PumpUntil([Object]() { const FCrowdyServerObjectTestRecord* A = RecordOf(Object, TEXT("a")); return A && A->Gold == 9; });
	FRecordOut Given = {TEXT("a"), {}, {}, {}, 1, {}};
	Rig.Push(RecordsPush(3, {Given}, {}, true, Epoch, 2));
	if (!TestTrue(TEXT("the push gives a to player 1"), Rig.PumpUntil([Object]() { return Object->GetRecord(TEXT("a")) && Object->GetRecord(TEXT("a"))->Owner == 1; })))
	{
		return false;
	}
	TestEqual(TEXT("and clears the Gold the earlier reply brought"), RecordOf(Object, TEXT("a"))->Gold, 0);
	Rig.Tick(1.1f);
	FSentFrame Again;
	if (!TestTrue(TEXT("changes are asked from before the push's restricted seq"), ExpectPageCall(Rig, TEXT("changes"), FString(), 1, Again)))
	{
		return false;
	}
	Rig.Reply(Again.Rid, ECrowdyNativeExecStatus::Ok, ChangesPage(2, {Gold}, false));
	TestTrue(TEXT("so the owner's Gold comes back"), Rig.PumpUntil([Object]() { const FCrowdyServerObjectTestRecord* A = RecordOf(Object, TEXT("a")); return A && A->Gold == 9; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsSignOutClearsTest,
	"CrowdySDK.CrowdyExec.RecordsSignOutLeavesNoRecords", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsSignOutClearsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(1, 1, {Full(TEXT("a"), 10, 1, TEXT("red"))}, false));
	if (!Object)
	{
		return false;
	}
	FInstancedStruct Values;
	int64 RecordOwner = 0;
	FString Team;
	TestTrue(TEXT("a Ready object finds its record"), Object->K2_FindRecord(TEXT("a"), Values, RecordOwner, Team));
	Rig.Subsystem->SignOutForTest();
	TestEqual(TEXT("a sign-out fails the object"), Object->GetStatus(), ECrowdyServerObjectStatus::Failed);
	TestEqual(TEXT("and leaves none of the account's records"), Object->NumRecords(), 0);
	TestFalse(TEXT("so Find Record finds nothing"), Object->K2_FindRecord(TEXT("a"), Values, RecordOwner, Team));

	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Other = MakeOwner();
	UCrowdyServerObject* Given = Rig.Acquire(TEXT("given-back"), Other.Get());
	if (!TestNotNull(TEXT("another object is held"), Given))
	{
		return false;
	}
	Given->ApplyRecordsForTest({TPair<FString, FInstancedStruct>(TEXT("b"), FInstancedStruct::Make(FCrowdyServerObjectTestRecord()))}, {});
	Rig.Subsystem->Release(Given, Other.Get());
	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestEqual(TEXT("an object given back is Released"), Given->GetStatus(), ECrowdyServerObjectStatus::Released);
	TestTrue(TEXT("still holding the record it had"), Given->GetRecord(TEXT("b")) != nullptr);
	TestFalse(TEXT("which Find Record does not give, as the object is not Ready"), Given->K2_FindRecord(TEXT("b"), Values, RecordOwner, Team));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsCrowdyTeamRereadTest,
	"CrowdySDK.CrowdyExec.RecordsOnCrowdyTeamsRereadWhenThePlayersTeamsChange", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsCrowdyTeamRereadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	Rig.Definition->TeamFrom = ECrowdyServerTeamSource::CrowdyTeam;
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = AcquireRecords(Rig, Owner.Get(), ReadPage(1, 1, {Full(TEXT("a"), 10, 1, TEXT("5"))}, false));
	if (!Object)
	{
		return false;
	}
	FRecordOut Own = {TEXT("a"), {}, {}, {}, {}, FString(TEXT("6"))};
	Rig.Push(RecordsPush(2, {Own}));
	Rig.PumpUntil([Object]() { return Object->GetRecord(TEXT("a")) && Object->GetRecord(TEXT("a"))->Team == TEXT("6"); });
	TestFalse(TEXT("the team of a record the player owns is not their Crowdy Team, so it reads nothing again"), Rig.SentAny(KindCall, TEXT("read")));

	FCrowdyTeamMembership Membership;
	Membership.Team.TeamId = 6;
	Rig.Subsystem->MyTeamsChangedForTest({Membership});
	FSentFrame Reread;
	if (!TestTrue(TEXT("joining a Crowdy Team reads again"), ExpectPageCall(Rig, TEXT("read"), FString(), 0, Reread)))
	{
		return false;
	}
	Rig.Reply(Reread.Rid, ECrowdyNativeExecStatus::Ok, ReadPage(2, 1, {Full(TEXT("a"), 10, 1, TEXT("6"))}, false));
	Rig.PumpFor(0.1);
	Rig.Subsystem->MyTeamsChangedForTest({Membership});
	TestFalse(TEXT("the same teams heard again read nothing"), Rig.SentAny(KindCall, TEXT("read")));

	Rig.Definition->TeamFrom = ECrowdyServerTeamSource::ThisObjectsRecords;
	Rig.Subsystem->MyTeamsChangedForTest({});
	TestFalse(TEXT("records whose teams are their own do not follow Crowdy Teams"), Rig.SentAny(KindCall, TEXT("read")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectRecordsAreaHandoverTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.ThisActorsAreaReadsTheAreaLeftUntilTheNewOneHasTheRecord", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectRecordsAreaHandoverTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectRecordsTests;
	FRig Rig(*this);
	if (!UseRecords(Rig))
	{
		return false;
	}
	Rig.Session->SetChunkSize(1600.0);
	TStrongObjectPtr<AActor> Actor(NewObject<AActor>(GetTransientPackage()));
	USceneComponent* Root = NewObject<USceneComponent>(Actor.Get());
	Actor->SetRootComponent(Root);
	auto MoveTo = [Root](const FVector& Location)
	{
		Root->SetRelativeLocation_Direct(Location);
		Root->UpdateComponentToWorld();
	};
	MoveTo(FVector(100.0, 100.0, 0.0));
	TStrongObjectPtr<UCrowdyEntityComponent> Entity(NewObject<UCrowdyEntityComponent>(Actor.Get()));
	const FGuid NetId(0x0A0B0C0D, 0x11223344, 0x55667788, 0x99AABBCC);
	Entity->AssignPooledIdentity(NetId, FGuid(1, 2, 3, 4), ECrowdyRole::Owner, 0);
	const FString Id = NetId.ToString(EGuidFormats::Digits);
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component(NewObject<UCrowdyServerObjectComponent>(Actor.Get()));
	Component->Definition = Rig.Definition.Get();
	Component->InstanceMode = ECrowdyServerObjectInstanceMode::ThisActorsArea;
	FCrowdyServerObjectRecordsTestAccess::UseServerObjects(*Component, Rig.Subsystem.Get());
	FCrowdyServerObjectRecordsTestAccess::Join(*Component);
	UCrowdyServerObject* First = Component->GetServerObject();
	if (!TestTrue(TEXT("the component holds area 0,0"), First && First->GetInstanceId() == TEXT("0,0")) || !Rig.Open()
		|| !AnswerRecordsRead(Rig, First, ReadPage(1, 1, {Full(Id, 10, 1, TEXT("red"))}, false)))
	{
		return false;
	}
	FCrowdyServerObjectRecordsTestAccess::TickFollow(*Component);

	MoveTo(FVector(7000.0, 100.0, 0.0));
	FCrowdyServerObjectRecordsTestAccess::TickFollow(*Component);
	UCrowdyServerObject* Second = Component->GetServerObject();
	if (!TestTrue(TEXT("a move to chunk 4,0 joins area 1,0"), Second && Second != First && Second->GetInstanceId() == TEXT("1,0")))
	{
		return false;
	}
	const FProperty* Count = FindFProperty<FProperty>(FCrowdyServerObjectTestPins::StaticStruct(), TEXT("Count"));
	int32 Health = 0;
	const FInstancedStruct Before = Component->GetVariables();
	const FCrowdyServerObjectTestRecord* Kept = Before.GetPtr<FCrowdyServerObjectTestRecord>();
	TestTrue(TEXT("while the new area has not read, Get Variables gives the record in the area left"), Kept && Kept->Health == 10);
	TestTrue(TEXT("and so does a per-variable read"),
		UCrowdyServerObjectLibrary::ReadServerVariableInto(Component.Get(), Rig.Definition.Get(), TEXT("Health"), Count, &Health) && Health == 10);

	if (!AnswerRecordsRead(Rig, Second, ReadPage(1, 1, {Full(Id, 11, 1, TEXT("red"))}, false)))
	{
		return false;
	}
	const FInstancedStruct After = Component->GetVariables();
	const FCrowdyServerObjectTestRecord* Moved = After.GetPtr<FCrowdyServerObjectTestRecord>();
	TestTrue(TEXT("once the new area holds the record it is read from there"), Moved && Moved->Health == 11);
	TestTrue(TEXT("and the area left is no longer read"), Component->GetReadObject() == Second);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectComponentMissingDefinitionWarnsOnceTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.MissingDefinitionWarnsOncePerActorClass", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectComponentMissingDefinitionWarnsOnceTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(TEXT("could not join: Pick a Server Object definition"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TArray<TStrongObjectPtr<UCrowdyServerObjectComponent>> Crowd;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AActor* Actor = NewObject<ACrowdyServerObjectTestCrowdActor>(GetTransientPackage());
		TStrongObjectPtr<UCrowdyServerObjectComponent>& Component = Crowd.Emplace_GetRef(NewObject<UCrowdyServerObjectComponent>(Actor));
		FCrowdyServerObjectRecordsTestAccess::Join(*Component);
		TestEqualSensitive(TEXT("each component still says why it holds nothing"), FCrowdyServerObjectRecordsTestAccess::JoinError(*Component),
			FString(TEXT("Pick a Server Object definition on the Crowdy Server Object component")));
	}
	return true;
}

#endif
