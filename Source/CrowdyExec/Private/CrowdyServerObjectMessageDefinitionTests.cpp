#include "CrowdyExecTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodec.h"
#include "CrowdyServerObjectDefinition.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyServerObjectMessageTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** A boss that sends BossDefeated (a struct), LootDropped on the topic loot (a List holding Gold, 7) and Wake (nothing). */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> NewBoss()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TEXT("test_message_boss");
		Definition->State = FCrowdyExecTestBossState::StaticStruct();
		FCrowdyServerMessage& Defeated = Definition->Messages.AddDefaulted_GetRef();
		Defeated.Name = TEXT("BossDefeated");
		Defeated.Payload = FCrowdyExecTestHit::StaticStruct();
		FCrowdyServerMessage& Loot = Definition->Messages.AddDefaulted_GetRef();
		Loot.Name = TEXT("LootDropped");
		Loot.ServerName = TEXT("loot");
		Loot.PayloadForm = ECrowdyServerValuesForm::List;
		Loot.PayloadList.AddProperty(TEXT("Gold"), EPropertyBagPropertyType::Int32);
		Loot.PayloadList.SetValueInt32(TEXT("Gold"), 7);
		Definition->Messages.AddDefaulted_GetRef().Name = TEXT("Wake");
		return Definition;
	}

	/** A definition with a State and nothing else a player could reach. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> NewQuiet()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TEXT("test_message_quiet");
		Definition->State = FCrowdyExecTestBossState::StaticStruct();
		return Definition;
	}

	TArray<FString> BakeErrors(UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Errors;
		Definition.Bake(Errors);
		return Errors;
	}

	/** Compared case-sensitively, since FString's == ignores case. */
	void TestRefuses(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const FString& Expected)
	{
		const TArray<FString> Errors = BakeErrors(Definition);
		const bool bFound = Errors.ContainsByPredicate([&Expected](const FString& Error) { return Error.Equals(Expected, ESearchCase::CaseSensitive); });
		Test.TestTrue(FString::Printf(TEXT("refused with \"%s\" (got: %s)"), *Expected, *FString::Join(Errors, TEXT("; "))), bFound);
	}

	void TestBakes(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const TCHAR* What)
	{
		const TArray<FString> Errors = BakeErrors(Definition);
		Test.TestTrue(FString::Printf(TEXT("%s bakes (%s)"), What, *FString::Join(Errors, TEXT("; "))), Errors.IsEmpty());
	}

	int32 GoldOf(const FInstancedStruct& Values)
	{
		const TValueOrError<int32, EPropertyBagResult> Value = CrowdyExec::ToList(Values).GetValueInt32(TEXT("Gold"));
		return Value.HasValue() ? Value.GetValue() : -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMessageResolveTest, "CrowdySDK.CrowdyExec.Messages.BakeAndResolve", CrowdyServerObjectMessageTests::TestFlags)
bool FCrowdyServerObjectMessageResolveTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectMessageTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Boss = NewBoss();
	const FCrowdyServerMessage& Defeated = Boss->Messages[0];
	const FCrowdyServerMessage& Loot = Boss->Messages[1];
	const FCrowdyServerMessage& Wake = Boss->Messages[2];
	TestTrue(TEXT("an unbaked message has no baked topic"), Defeated.BakedTopic.IsEmpty());
	TestEqualSensitive(TEXT("and works its topic out from its name in snake case"), *Defeated.GetTopic(), TEXT("boss_defeated"));
	TestEqualSensitive(TEXT("or from its server name"), *Loot.GetTopic(), TEXT("loot"));
	TestTrue(TEXT("the boss bakes"), BakeErrors(*Boss).IsEmpty());
	TestEqualSensitive(TEXT("a bake writes the topic down"), *Defeated.BakedTopic, TEXT("boss_defeated"));
	TestEqualSensitive(TEXT("the server name when there is one"), *Loot.BakedTopic, TEXT("loot"));

	TestTrue(TEXT("a message is found by name"), Boss->FindMessage(TEXT("LootDropped")) == &Boss->Messages[1]);
	TestNull(TEXT("an unknown name finds nothing"), Boss->FindMessage(TEXT("Nope")));
	TestNull(TEXT("a topic is not a name"), Boss->FindMessage(TEXT("loot")));
	TestTrue(TEXT("a message is found by topic"), Boss->FindMessageByTopic(TEXT("boss_defeated")) == &Boss->Messages[0]);
	TestTrue(TEXT("also when the topic is a server name"), Boss->FindMessageByTopic(TEXT("loot")) == &Boss->Messages[1]);
	TestTrue(TEXT("also when it has no payload"), Boss->FindMessageByTopic(TEXT("wake")) == &Boss->Messages[2]);
	TestNull(TEXT("a name is not a topic"), Boss->FindMessageByTopic(TEXT("BossDefeated")));
	TestNull(TEXT("the topic a server name replaced finds nothing"), Boss->FindMessageByTopic(TEXT("loot_dropped")));
	TestNull(TEXT("the watched values' topic is no message"), Boss->FindMessageByTopic(TEXT("state")));
	TestNull(TEXT("a topic differing in case finds nothing"), Boss->FindMessageByTopic(TEXT("LOOT")));
	Boss->Messages[2].BakedTopic.Reset();
	TestTrue(TEXT("a message with no baked topic is found by the topic its name works out"), Boss->FindMessageByTopic(TEXT("wake")) == &Boss->Messages[2]);
	Boss->Messages[2].BakedTopic = TEXT("wake");

	Boss->Messages[0].Name = TEXT("BossSlain");
	TestEqualSensitive(TEXT("the baked topic is kept, not worked out from the name as it reads now"), *Boss->Messages[0].GetTopic(), TEXT("boss_defeated"));
	TestTrue(TEXT("and still finds the message"), Boss->FindMessageByTopic(TEXT("boss_defeated")) == &Boss->Messages[0]);
	TestTrue(TEXT("baking again writes the new one"), BakeErrors(*Boss).IsEmpty());
	TestEqualSensitive(TEXT("down"), *Boss->Messages[0].GetTopic(), TEXT("boss_slain"));
	Boss->Messages[0].Name = TEXT("BossDefeated");
	BakeErrors(*Boss);

	TestTrue(TEXT("a struct payload is the struct"), Defeated.GetPayloadStruct() == FCrowdyExecTestHit::StaticStruct());
	const UScriptStruct* LootStruct = Loot.GetPayloadStruct();
	TestTrue(TEXT("a List payload is its own struct"), LootStruct && LootStruct != FCrowdyExecTestHit::StaticStruct());
	TestNull(TEXT("no payload is null"), Wake.GetPayloadStruct());
	TestEqualSensitive(TEXT("a struct payload has no List name"), *Defeated.GetPayloadListName(), TEXT(""));
	TestEqualSensitive(TEXT("a List payload is named after its message"), *Loot.GetPayloadListName(), TEXT("LootDroppedMessage"));
	TestEqualSensitive(TEXT("the definition names the List"), *Boss->FindListName(LootStruct), TEXT("LootDroppedMessage"));
	TestTrue(TEXT("and finds it by name"), Boss->FindListStruct(TEXT("LootDroppedMessage")) == LootStruct);
	TestEqualSensitive(TEXT("a struct payload is no List"), *Boss->FindListName(FCrowdyExecTestHit::StaticStruct()), TEXT(""));

	TArray<FCrowdyServerNamedList> Lists;
	Boss->GetLists(Lists);
	TestTrue(TEXT("GetLists holds the payload List"), Lists.ContainsByPredicate([&Loot](const FCrowdyServerNamedList& List)
	{
		return List.Name == TEXT("LootDroppedMessage") && List.Values == &Loot.PayloadList;
	}));

	FInstancedStruct Payload;
	Loot.InitializePayload(Payload);
	TestEqual(TEXT("a List payload starts from its own values"), GoldOf(Payload), 7);
	FInstancedStruct FromDefinition;
	Boss->InitializeValues(LootStruct, FromDefinition);
	TestEqual(TEXT("and so does the definition's InitializeValues"), GoldOf(FromDefinition), 7);
	FInstancedStruct Hit;
	Defeated.InitializePayload(Hit);
	TestTrue(TEXT("a struct payload starts as the struct"), Hit.GetScriptStruct() == FCrowdyExecTestHit::StaticStruct());
	FInstancedStruct Nothing;
	Nothing.InitializeAs(FCrowdyExecTestHit::StaticStruct());
	Wake.InitializePayload(Nothing);
	TestFalse(TEXT("no payload starts empty"), Nothing.IsValid());

	const bool bHitBaked = Boss->BakedStructs.ContainsByPredicate([](const FCrowdyExecBakedStruct& Baked) { return Baked.Struct.Get() == FCrowdyExecTestHit::StaticStruct(); });
	TestTrue(TEXT("a struct payload no function sends is in the baked tables"), bHitBaked);
	const bool bLootBaked = Boss->BakedStructs.ContainsByPredicate([](const FCrowdyExecBakedStruct& Baked) { return Baked.List == TEXT("LootDroppedMessage"); });
	TestTrue(TEXT("and so is a List payload"), bLootBaked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMessageCodecTest, "CrowdySDK.CrowdyExec.Messages.PayloadsEncodeAndDecode", CrowdyServerObjectMessageTests::TestFlags)
bool FCrowdyServerObjectMessageCodecTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectMessageTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Boss = NewBoss();
	if (!TestTrue(TEXT("the boss bakes"), BakeErrors(*Boss).IsEmpty()))
	{
		return false;
	}
	const FCrowdyServerMessage& Defeated = Boss->Messages[0];
	const FCrowdyServerMessage& Loot = Boss->Messages[1];

	FCrowdyExecTestHit Hit;
	Hit.Damage = 12;
	TArray<uint8> Bytes;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("a struct payload encodes (%s)"), *Error), CrowdyExec::Encode(*Boss, Defeated.GetPayloadStruct(), &Hit, Bytes, Error)))
	{
		return false;
	}
	FInstancedStruct Decoded;
	Defeated.InitializePayload(Decoded);
	TestTrue(FString::Printf(TEXT("and decodes (%s)"), *Error), CrowdyExec::Decode(*Boss, *Defeated.GetPayloadStruct(), Bytes, Decoded.GetMutableMemory(), Error));
	TestEqual(TEXT("with its value"), Decoded.Get<FCrowdyExecTestHit>().Damage, 12);

	FInstancedPropertyBag Sent = Loot.PayloadList;
	Sent.SetValueInt32(TEXT("Gold"), 9);
	Bytes.Reset();
	if (!TestTrue(FString::Printf(TEXT("a List payload encodes (%s)"), *Error), CrowdyExec::Encode(*Boss, Loot.GetPayloadStruct(), Sent.GetValue().GetMemory(), Bytes, Error)))
	{
		return false;
	}
	FInstancedStruct Defaults;
	Loot.InitializePayload(Defaults);
	FInstancedStruct Received;
	Loot.InitializePayload(Received);
	TestTrue(FString::Printf(TEXT("and decodes (%s)"), *Error), CrowdyExec::Decode(*Boss, *Loot.GetPayloadStruct(), Bytes, Received.GetMutableMemory(), Error, Defaults.GetMemory()));
	TestEqual(TEXT("with its value"), GoldOf(Received), 9);

	const uint8 EmptyMap[] = {0x80};
	FInstancedStruct Missing;
	Loot.InitializePayload(Missing);
	TestTrue(FString::Printf(TEXT("an empty payload decodes (%s)"), *Error), CrowdyExec::Decode(*Boss, *Loot.GetPayloadStruct(), EmptyMap, Missing.GetMutableMemory(), Error, Defaults.GetMemory()));
	TestEqual(TEXT("and a value it leaves out takes the message's own value"), GoldOf(Missing), 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMessageRefusalsTest, "CrowdySDK.CrowdyExec.Messages.Refusals", CrowdyServerObjectMessageTests::TestFlags)
bool FCrowdyServerObjectMessageRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectMessageTests;
	const FString Rule = TEXT("which must be lowercase letters, digits and underscores, start with a letter, and be at most 64 characters");

	TStrongObjectPtr<UCrowdyServerObjectDefinition> BadTopic = NewBoss();
	BadTopic->Messages[0].ServerName = TEXT("Boss Defeated");
	TestRefuses(*this, *BadTopic, FString::Printf(TEXT("Message %s: 'Boss Defeated' cannot be its topic, %s"), *BadTopic->Messages[0].Name.ToString(), *Rule));
	BadTopic->Messages[0].ServerName = FString::ChrN(65, 'a');
	TestRefuses(*this, *BadTopic, FString::Printf(TEXT("Message %s: '%s' cannot be its topic, %s"), *BadTopic->Messages[0].Name.ToString(), *BadTopic->Messages[0].ServerName, *Rule));
	BadTopic->Messages[0].ServerName = FString::ChrN(64, 'a');
	TestBakes(*this, *BadTopic, TEXT("a topic of 64 characters"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Unnamed = NewBoss();
	Unnamed->Messages[0].Name = NAME_None;
	TestRefuses(*this, *Unnamed, TEXT("Messages: message 1 has no Name; type one"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> State = NewBoss();
	State->Messages[0].Name = TEXT("State");
	TestRefuses(*this, *State, FString::Printf(TEXT("Message %s: 'state' is reserved for the variables players watch; rename the message"), *State->Messages[0].Name.ToString()));
	State->Messages[0].Name = TEXT("BossDefeated");
	State->Messages[0].ServerName = TEXT("state");
	TestRefuses(*this, *State, FString::Printf(TEXT("Message %s: 'state' is reserved for the variables players watch; rename the message"), *State->Messages[0].Name.ToString()));
	State->Messages[0].ServerName = TEXT("state_changed");
	TestBakes(*this, *State, TEXT("a topic that only starts with state"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Twins = NewBoss();
	Twins->Messages[2].ServerName = TEXT("loot");
	TestRefuses(*this, *Twins, FString::Printf(TEXT("Message %s: another message is also sent on the topic 'loot'"), *Twins->Messages[2].Name.ToString()));
	Twins->Messages[2].ServerName = TEXT("wake_up");
	TestBakes(*this, *Twins, TEXT("topics that differ"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> SameName = NewBoss();
	SameName->Messages[2].Name = SameName->Messages[0].Name;
	SameName->Messages[2].ServerName = TEXT("wake_up");
	TestRefuses(*this, *SameName, FString::Printf(TEXT("Message %s: another message has this name"), *SameName->Messages[2].Name.ToString()));
	SameName->Messages[2].Name = TEXT("Wake");
	TestBakes(*this, *SameName, TEXT("messages with different names"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Failing = NewBoss();
	Failing->Messages[2].ServerName = TEXT("loot");
	TArray<FString> FailingErrors;
	TestFalse(TEXT("a refused bake fails"), Failing->Bake(FailingErrors));
	TestTrue(TEXT("and leaves no field tables"), Failing->BakedStructs.IsEmpty());

#if WITH_EDITORONLY_DATA
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Empty = NewBoss();
	Empty->ListenTo.Add(nullptr);
	TestRefuses(*this, *Empty, TEXT("Listen To: an entry is empty; choose a Server Object definition or remove the entry"));

	TStrongObjectPtr<UCrowdyServerObjectDefinition> Itself = NewBoss();
	Itself->ListenTo.Add(Itself.Get());
	TestRefuses(*this, *Itself, TEXT("Listen To: a type cannot listen to itself"));

	const FName SenderName = MakeUniqueObjectName(GetTransientPackage(), UCrowdyServerObjectDefinition::StaticClass(), TEXT("MessageNoTypeName"));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Nameless(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), SenderName, RF_Transient));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Listener = NewQuiet();
	Listener->ListenTo.Add(Nameless.Get());
	TestRefuses(*this, *Listener, FString::Printf(TEXT("Listen To: %s has no Type Name"), *Nameless->GetName()));

	Nameless->TypeName = TEXT("Message Sender");
	TestRefuses(*this, *Listener, FString::Printf(TEXT("Listen To: %s has the Type Name 'Message Sender', %s"), *Nameless->GetName(),
		TEXT("which must be lowercase letters, digits and underscores, start with a letter, and be at most 48 characters")));
	TestFalse(TEXT("a Listen To entry is not read as a Can Call one"), BakeErrors(*Listener).ContainsByPredicate([](const FString& Error) { return Error.StartsWith(TEXT("Can Call")); }));

	Nameless->TypeName = TEXT("test_message_sender");
	TestBakes(*this, *Listener, TEXT("a Listen To entry with a Type Name"));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMessageReadersTest, "CrowdySDK.CrowdyExec.Messages.RestrictedReaders", CrowdyServerObjectMessageTests::TestFlags)
bool FCrowdyServerObjectMessageReadersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectMessageTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewQuiet();
	TestFalse(TEXT("every player reading everything is not restricted"), Definition->HasRestrictedReaders());
	Definition->Visibility = ECrowdyServerObjectVisibility::OwnerOnly;
	TestTrue(TEXT("Owner Only is restricted"), Definition->HasRestrictedReaders());
	Definition->Visibility = ECrowdyServerObjectVisibility::Members;
	TestTrue(TEXT("Members is restricted"), Definition->HasRestrictedReaders());
	Definition->Visibility = ECrowdyServerObjectVisibility::Public;
	Definition->OwnerFields = {TEXT("Health")};
	TestTrue(TEXT("an Owner Only variable is restricted"), Definition->HasRestrictedReaders());
	Definition->OwnerFields.Reset();
	Definition->TeamFields = {TEXT("Health")};
	TestTrue(TEXT("a Team Only variable is restricted"), Definition->HasRestrictedReaders());
	Definition->TeamFields.Reset();
	TestFalse(TEXT("and clearing them makes it public again"), Definition->HasRestrictedReaders());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectMessageReachTest, "CrowdySDK.CrowdyExec.Messages.ReachableByPlayers", CrowdyServerObjectMessageTests::TestFlags)
bool FCrowdyServerObjectMessageReachTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectMessageTests;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = NewQuiet();
	TestFalse(TEXT("a type with nothing players can use is closed to them"), Definition->IsReachableByPlayers());
	TestFalse(TEXT("and has no player messages"), Definition->HasPlayerMessages());
	FCrowdyServerMessage& HubOnly = Definition->Messages.AddDefaulted_GetRef();
	HubOnly.Name = TEXT("Whisper");
	HubOnly.bPlayersCanListen = false;
	TestFalse(TEXT("a message only other types hear does not open it"), Definition->IsReachableByPlayers());
	TestFalse(TEXT("nor is it a player message"), Definition->HasPlayerMessages());
	Definition->Messages.AddDefaulted_GetRef().Name = TEXT("Wake");
	TestTrue(TEXT("a message players may listen to opens it"), Definition->IsReachableByPlayers());
	TestTrue(TEXT("and is a player message"), Definition->HasPlayerMessages());
	TestTrue(TEXT("a message lets players listen unless turned off"), Definition->Messages[1].bPlayersCanListen);
	Definition->Messages.Reset();
	TestFalse(TEXT("and removing them closes it again"), Definition->IsReachableByPlayers());
	return true;
}

#endif
