#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/AllOf.h"
#include "CrowdyExecCodegen.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyExecDeveloperApi.h"
#include "CrowdyExecDeveloperCall.h"
#include "CrowdyExecRevisions.h"
#include "CrowdyGameKitStarters.h"
#include "CrowdyGameKitTypes.h"
#include "CrowdyServerObjectDefinition.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace CrowdyGameKitStartersTest
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeStarter(FAutomationTestBase& Test, const TCHAR* TypeName)
	{
		const CrowdyGameKitStarters::FStarter* Starter = CrowdyGameKitStarters::FindStarter(TypeName);
		UCrowdyServerObjectDefinition* Made = Starter ? CrowdyGameKitStarters::MakeDefinition(*Starter, GetTransientPackage(), NAME_None, RF_Transient) : nullptr;
		Test.TestNotNull(FString::Printf(TEXT("the %s starter's definition is made and bakes"), TypeName), Made);
		return TStrongObjectPtr<UCrowdyServerObjectDefinition>(Made);
	}

	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeOwnType(const TCHAR* TypeName)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TypeName;
		Definition->Functions.AddDefaulted();
		return Definition;
	}

	TSharedPtr<FJsonObject> ManifestType(const FString& Manifest, const TCHAR* TypeName)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedPtr<FJsonObject>* Types = nullptr;
		const TSharedPtr<FJsonObject>* Type = nullptr;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Manifest), Root) || !Root || !Root->TryGetObjectField(TEXT("types"), Types)
			|| !(*Types)->TryGetObjectField(TypeName, Type))
		{
			return nullptr;
		}
		return *Type;
	}

	FString FieldOf(const TSharedPtr<FJsonObject>& Type, const TCHAR* Field)
	{
		FString Value;
		return Type && Type->TryGetStringField(Field, Value) ? Value : FString(TEXT("<none>"));
	}

	FString CallsOf(const TSharedPtr<FJsonObject>& Type)
	{
		TArray<FString> Calls;
		if (Type)
		{
			Type->TryGetStringArrayField(TEXT("calls"), Calls);
		}
		return FString::Join(Calls, TEXT(","));
	}

	struct FStarterFolder
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("GameKitTests"), FGuid::NewGuid().ToString()));

		~FStarterFolder()
		{
			IFileManager::Get().DeleteDirectory(*Path, false, true);
		}

		bool CopyStarter(const TCHAR* TypeName) const
		{
			const FString Source = CrowdyGameKitStarters::GetCrateSource(*CrowdyGameKitStarters::FindStarter(TypeName));
			const FString Into = FPaths::Combine(Path, TypeName);
			return IFileManager::Get().Copy(*FPaths::Combine(Into, TEXT("Cargo.toml")), *FPaths::Combine(Source, TEXT("Cargo.toml"))) == COPY_OK
				&& IFileManager::Get().Copy(*FPaths::Combine(Into, TEXT("src/lib.rs")), *FPaths::Combine(Source, TEXT("src/lib.rs"))) == COPY_OK;
		}
	};

	const FCrowdyExecBakedStruct* FindBakedStruct(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct* Struct)
	{
		return Definition.BakedStructs.FindByPredicate([Struct](const FCrowdyExecBakedStruct& Baked) { return Baked.Struct == Struct; });
	}

	bool HasField(const FCrowdyExecBakedStruct* Baked, const TCHAR* ServerName)
	{
		return Baked && Baked->Fields.ContainsByPredicate([ServerName](const FCrowdyExecBakedField& Field) { return Field.ServerName.Equals(ServerName, ESearchCase::CaseSensitive); });
	}

	/** Whether the enum's baked server names are exactly Expected, in order and case-sensitively: TArray<FString> == compares without case. */
	bool EnumNamesAre(const UCrowdyServerObjectDefinition& Definition, const UEnum* Enum, const TArray<FString>& Expected)
	{
		const FCrowdyExecBakedEnum* Baked = Definition.BakedEnums.FindByPredicate([Enum](const FCrowdyExecBakedEnum& Each) { return Each.Enum == Enum; });
		if (!Baked || Baked->ServerNames.Num() != Expected.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			if (!Baked->ServerNames[Index].Equals(Expected[Index], ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	bool LoadLib(const TCHAR* TypeName, FString& OutLib)
	{
		const CrowdyGameKitStarters::FStarter* Starter = CrowdyGameKitStarters::FindStarter(TypeName);
		return Starter && FFileHelper::LoadFileToString(OutLib, *FPaths::Combine(CrowdyGameKitStarters::GetCrateSource(*Starter), TEXT("src/lib.rs")));
	}

	/** A starter sends all its events as one message on its State Topic, whose Type enum carries the event names its Rust sends. */
	void TestOneEventMessage(FAutomationTestBase& Test, const TCHAR* TypeName, const TCHAR* Topic, const UScriptStruct* Payload, const UEnum* EventType, const TArray<FString>& EventNames)
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(Test, TypeName);
		if (!Definition || !Test.TestEqual(FString::Printf(TEXT("%s sends its events as one message"), TypeName), Definition->Messages.Num(), 1))
		{
			return;
		}
		const FCrowdyServerMessage& Message = Definition->Messages[0];
		Test.TestEqualSensitive(FString::Printf(TEXT("%s's message is sent on its State Topic"), TypeName), Message.GetTopic(), FString(Topic));
		Test.TestEqualSensitive(FString::Printf(TEXT("%s's State Topic is that topic"), TypeName), Definition->KitStateTopic, FString(Topic));
		Test.TestTrue(FString::Printf(TEXT("%s's message is found by its topic and carries the event struct"), TypeName),
			Definition->FindMessageByTopic(Topic) == &Message && Message.GetPayloadStruct() == Payload);
		Test.TestTrue(FString::Printf(TEXT("%s's players can listen to it"), TypeName), Message.bPlayersCanListen);
		Test.TestTrue(FString::Printf(TEXT("%s's event Type values are the names its Rust sends"), TypeName), EnumNamesAre(*Definition, EventType, EventNames));

		const FCrowdyExecBakedStruct* Body = FindBakedStruct(*Definition, Payload);
		Test.TestTrue(FString::Printf(TEXT("%s's event body carries its Type as the body's type"), TypeName), HasField(Body, TEXT("type")));
		for (const TCHAR* Reserved : {TEXT("v"), TEXT("epoch"), TEXT("seq"), TEXT("kind")})
		{
			Test.TestFalse(FString::Printf(TEXT("%s's event body leaves the header's %s to the header"), TypeName, Reserved), HasField(Body, Reserved));
		}
		FString Lib;
		if (!Test.TestTrue(FString::Printf(TEXT("%s's Rust is readable"), TypeName), LoadLib(TypeName, Lib)))
		{
			return;
		}
		for (const FString& EventName : EventNames)
		{
			Test.TestTrue(FString::Printf(TEXT("%s's Rust sends the event %s"), TypeName, *EventName), Lib.Contains(FString::Printf(TEXT("\"%s\""), *EventName), ESearchCase::CaseSensitive));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStartersMatchTheirCratesTest, "CrowdySDK.CrowdyExecEditor.GameKit.StartersMatchTheirCrates", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitStartersMatchTheirCratesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	for (const CrowdyGameKitStarters::FStarter& Starter : CrowdyGameKitStarters::GetStarters())
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, Starter.TypeName);
		if (!Definition)
		{
			continue;
		}
		const FString Type = Starter.TypeName;
		TestTrue(Type + TEXT(" is a Game Kit starter named after its node type"), Definition->IsGameKitStarter() && Definition->TypeName == Type);
		const FString Source = CrowdyGameKitStarters::GetCrateSource(Starter);
		FString Cargo;
		FString Lib;
		if (!TestTrue(Type + TEXT("'s crate ships with the Crowdy SDK"), FFileHelper::LoadFileToString(Cargo, *FPaths::Combine(Source, TEXT("Cargo.toml")))
			&& FFileHelper::LoadFileToString(Lib, *FPaths::Combine(Source, TEXT("src/lib.rs")))))
		{
			continue;
		}
		TestTrue(Type + TEXT("'s package is named after its type, as the manifest names its crate"), Cargo.Contains(FString::Printf(TEXT("name = \"%s\""), *Type), ESearchCase::CaseSensitive));
		TestTrue(Type + TEXT("'s State Topic is one its hub publishes"), Lib.Contains(FString::Printf(TEXT("\"%s\""), *Definition->KitStateTopic), ESearchCase::CaseSensitive));
		TestTrue(Type + TEXT("'s Collection Topic, when it has one, is one its hub publishes"),
			Definition->KitCollectionTopic.IsEmpty() || Lib.Contains(FString::Printf(TEXT("\"%s\""), *Definition->KitCollectionTopic), ESearchCase::CaseSensitive));
		TestTrue(Type + TEXT("'s Collection Topic comes with a Record Struct"), Definition->KitCollectionTopic.IsEmpty() == !Definition->KitRecord);
		for (const FCrowdyServerFunction& Function : Definition->Functions)
		{
			TestTrue(FString::Printf(TEXT("%s's hub answers %s"), *Type, *Function.GetMethodName()),
				Lib.Contains(FString::Printf(TEXT("\"%s\""), *Function.GetMethodName()), ESearchCase::CaseSensitive));
		}
		for (const FCrowdyKitOwnerValue& Owned : Definition->KitOwnerValues)
		{
			TestTrue(FString::Printf(TEXT("%s's hub answers its Owner Value %s and names it in its changed messages"), *Type, *Owned.Method),
				Lib.Contains(FString::Printf(TEXT("\"%s\""), *Owned.Method), ESearchCase::CaseSensitive) && Lib.Contains(FString::Printf(TEXT("\"%s\""), *Owned.What), ESearchCase::CaseSensitive));
		}
		for (const FCrowdyExecBakedStruct& Struct : Definition->BakedStructs)
		{
			// A struct of one field is a bare answer, which travels without its field's name.
			if (Struct.Fields.Num() < 2)
			{
				continue;
			}
			for (const FCrowdyExecBakedField& Field : Struct.Fields)
			{
				TestTrue(FString::Printf(TEXT("%s's Rust has a field %s, as %s names it"), *Type, *Field.ServerName, *GetNameSafe(Struct.Struct)),
					Lib.Contains(Field.ServerName, ESearchCase::CaseSensitive));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitNpcsAreaMembersTest, "CrowdySDK.CrowdyExecEditor.GameKit.NpcsAreaMembersAreOneRecordStruct", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitNpcsAreaMembersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Npcs = MakeStarter(*this, TEXT("npcs"));
	if (!Npcs)
	{
		return false;
	}
	TestTrue(TEXT("npcs reads where players are"), Npcs->bReadsWorld);
	TestTrue(TEXT("and draws its mobs, NPCs and pets as Kit Actors"), Npcs->KitActors.Contains(TEXT("mob")) && Npcs->KitActors.Contains(TEXT("npc")) && Npcs->KitActors.Contains(TEXT("pet")));

	const FCrowdyExecBakedStruct* Member = Npcs->BakedStructs.FindByPredicate([](const FCrowdyExecBakedStruct& Struct) { return Struct.Struct == FCrowdyKitAreaMember::StaticStruct(); });
	TestTrue(TEXT("a member's Type is its hub's type_name"), Member && Member->Fields.ContainsByPredicate([](const FCrowdyExecBakedField& Field)
	{
		return Field.ServerName == TEXT("type_name");
	}));
	const FCrowdyExecBakedEnum* Types = Npcs->BakedEnums.FindByPredicate([](const FCrowdyExecBakedEnum& Enum) { return Enum.Enum == StaticEnum<ECrowdyKitAreaMemberType>(); });
	TestTrue(TEXT("whose values are its container types as they are spelled, Mob, Npc and Pet"), Types && Types->ServerNames.Num() == 3
		&& Types->ServerNames[0].Equals(TEXT("Mob"), ESearchCase::CaseSensitive) && Types->ServerNames[1].Equals(TEXT("Npc"), ESearchCase::CaseSensitive)
		&& Types->ServerNames[2].Equals(TEXT("Pet"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitWorldIsTheRootTest, "CrowdySDK.CrowdyExecEditor.GameKit.WorldIsTheRootOfTheManifest", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitWorldIsTheRootTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Matchmaker = MakeStarter(*this, TEXT("matchmaker"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Session = MakeStarter(*this, TEXT("session"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Arena = MakeOwnType(TEXT("arena"));
	if (!World || !Matchmaker || !Session)
	{
		return false;
	}
	World->KitConfig = TEXT(" {\"day_ms\": 600000} ");
	const FString Manifest = CrowdyExecDeploy::MakeManifest({Session.Get(), Arena.Get(), World.Get(), Matchmaker.Get()});
	TestTrue(FString::Printf(TEXT("world is the root and comes first (%s)"), *Manifest), Manifest.StartsWith(TEXT("{\"root\":\"world\",\"types\":{\"world\":{"), ESearchCase::CaseSensitive));
	TestNull(TEXT("the SDK's root is not sent"), ManifestType(Manifest, TEXT("root")).Get());
	TestTrue(TEXT("world's resource nodes are its records"), World->KitCollectionTopic == TEXT("nodes") && World->KitRecord == FCrowdyKitWorldNode::StaticStruct());
	const TSharedPtr<FJsonObject> WorldType = ManifestType(Manifest, TEXT("world"));
	TestEqualSensitive(TEXT("the root has no parent"), FieldOf(WorldType, TEXT("parent")), FString(TEXT("<none>")));
	TestEqualSensitive(TEXT("its crate is its type"), FieldOf(WorldType, TEXT("crate")), FString(TEXT("world")));
	TestTrue(TEXT("it saves every 10 seconds, as the starter pack has it, and is never idle"), WorldType && WorldType->GetNumberField(TEXT("persist_every_ms")) == 10000.0 && !WorldType->HasField(TEXT("evict_after_ms")));
	TestEqualSensitive(TEXT("its Config is its seed, trimmed"), FieldOf(WorldType, TEXT("seed_b64")), FBase64::Encode(TEXT("{\"day_ms\": 600000}")));
	const TSharedPtr<FJsonObject> MatchmakerType = ManifestType(Manifest, TEXT("matchmaker"));
	TestEqualSensitive(TEXT("a matchmaker runs under world"), FieldOf(MatchmakerType, TEXT("parent")), FString(TEXT("world")));
	TestEqualSensitive(TEXT("and calls session, which the project has"), CallsOf(MatchmakerType), FString(TEXT("session")));
	TestEqualSensitive(TEXT("a session runs under the matchmaker"), FieldOf(ManifestType(Manifest, TEXT("session")), TEXT("parent")), FString(TEXT("matchmaker")));
	TestEqualSensitive(TEXT("an own type runs under world"), FieldOf(ManifestType(Manifest, TEXT("arena")), TEXT("parent")), FString(TEXT("world")));
	TestEqualSensitive(TEXT("and has no seed"), FieldOf(ManifestType(Manifest, TEXT("arena")), TEXT("seed_b64")), FString(TEXT("<none>")));
	TestTrue(TEXT("the root is found among the definitions"), CrowdyExecDeploy::FindKitRoot({Arena.Get(), Matchmaker.Get(), World.Get()}) == World.Get());

	const FString Alone = CrowdyExecDeploy::MakeManifest({Session.Get(), Arena.Get()});
	TestTrue(TEXT("without world the SDK's root is the root"), Alone.StartsWith(TEXT("{\"root\":\"root\",\"types\":{\"root\":{\"kind\":\"hub\",\"crate\":\"root\",\"client\":true}"), ESearchCase::CaseSensitive));
	TestEqualSensitive(TEXT("a session without its matchmaker runs under the root"), FieldOf(ManifestType(Alone, TEXT("session")), TEXT("parent")), FString(TEXT("root")));
	TestEqualSensitive(TEXT("a matchmaker without session calls nothing"), CallsOf(ManifestType(CrowdyExecDeploy::MakeManifest({Matchmaker.Get()}), TEXT("matchmaker"))), FString());

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Leaderboard = MakeStarter(*this, TEXT("leaderboard"));
	if (!Leaderboard)
	{
		return false;
	}
	TestEqualSensitive(TEXT("a session without a leaderboard calls nothing"), CallsOf(ManifestType(Manifest, TEXT("session"))), FString());
	const FString WithBoard = CrowdyExecDeploy::MakeManifest({World.Get(), Matchmaker.Get(), Session.Get(), Leaderboard.Get()});
	TestEqualSensitive(TEXT("with one it calls the leaderboard, to post its scores"), CallsOf(ManifestType(WithBoard, TEXT("session"))), FString(TEXT("leaderboard")));
	const TSharedPtr<FJsonObject> BoardType = ManifestType(WithBoard, TEXT("leaderboard"));
	TestEqualSensitive(TEXT("a leaderboard runs under world"), FieldOf(BoardType, TEXT("parent")), FString(TEXT("world")));
	TestEqualSensitive(TEXT("and calls nothing"), CallsOf(BoardType), FString());
	TestTrue(TEXT("its board is its State"), Leaderboard->State == FCrowdyKitBoardView::StaticStruct() && Leaderboard->KitStateTopic == TEXT("board"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitAssembleTest, "CrowdySDK.CrowdyExecEditor.GameKit.AssembleSendsTheStartersOwnCrates", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitAssembleTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Matchmaker = MakeStarter(*this, TEXT("matchmaker"));
	if (!World || !Matchmaker)
	{
		return false;
	}
	FStarterFolder Folder;
	if (!TestTrue(TEXT("the world starter's crate is copied"), Folder.CopyStarter(TEXT("world"))))
	{
		return false;
	}
	auto DirectoryFor = [&Folder](const UCrowdyServerObjectDefinition& Definition) { return FPaths::Combine(Folder.Path, Definition.TypeName); };

	const CrowdyExecDeploy::FProjectDeploy Project = CrowdyExecDeploy::AssembleProject({World.Get()}, DirectoryFor);
	TestEqualSensitive(TEXT("a starter's own crate needs no Generate"), FString::Join(Project.Problems, TEXT(" | ")), FString());
	TestEqualSensitive(TEXT("the project's root is world"), Project.Root, FString(TEXT("world")));
	if (TestEqual(TEXT("world's crate is the only one: the SDK's root is not sent"), Project.Crates.Num(), 1))
	{
		TestEqualSensitive(TEXT("named after its type"), Project.Crates[0].Name, FString(TEXT("world")));
		TestEqual(TEXT("with its Cargo.toml and lib.rs"), Project.Crates[0].Files.Num(), 2);
	}

	const CrowdyExecDeploy::FProjectDeploy Missing = CrowdyExecDeploy::AssembleProject({World.Get(), Matchmaker.Get()}, DirectoryFor);
	TestTrue(FString::Printf(TEXT("a starter with no crate on disk is a problem naming it (%s)"), *FString::Join(Missing.Problems, TEXT(" | "))),
		Missing.Problems.Num() == 1 && Missing.Problems[0].StartsWith(TEXT("matchmaker: "), ESearchCase::CaseSensitive) && Missing.Problems[0].Contains(TEXT("add the starter again")));

	World->KitConfig = TEXT("{\"day_ms\": ");
	const CrowdyExecDeploy::FProjectDeploy BadConfig = CrowdyExecDeploy::AssembleProject({World.Get()}, DirectoryFor);
	TestTrue(TEXT("a Config that is not a JSON object is a problem"), BadConfig.Problems.Num() == 1 && BadConfig.Problems[0].Contains(TEXT("Config is not a JSON object")));
	World->KitConfig.Reset();

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Second(DuplicateObject(World.Get(), GetTransientPackage()));
	Second->TypeName = TEXT("world_two");
	const CrowdyExecDeploy::FProjectDeploy TwoRoots = CrowdyExecDeploy::AssembleProject({World.Get(), Second.Get()}, DirectoryFor);
	TestTrue(TEXT("two starters with no Parent are a problem"), TwoRoots.Problems.ContainsByPredicate([](const FString& Problem) { return Problem.Contains(TEXT("an app has one root")); }));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Session = MakeStarter(*this, TEXT("session"));
	if (!Session)
	{
		return false;
	}
	Matchmaker->KitParent = TEXT("session");
	const CrowdyExecDeploy::FProjectDeploy Circle = CrowdyExecDeploy::AssembleProject({World.Get(), Matchmaker.Get(), Session.Get()}, DirectoryFor);
	TestTrue(TEXT("Parents that run in a circle are a problem"), Circle.Problems.ContainsByPredicate([](const FString& Problem) { return Problem.Contains(TEXT("run in a circle")); }));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = MakeOwnType(TEXT("helpers"));
	Library->Kind = ECrowdyServerObjectKind::FunctionLibrary;
	Matchmaker->KitParent = TEXT("helpers");
	const CrowdyExecDeploy::FProjectDeploy UnderLibrary = CrowdyExecDeploy::AssembleProject({World.Get(), Matchmaker.Get(), Library.Get()}, DirectoryFor);
	TestTrue(TEXT("a Parent that is a Server Function Library is a problem"), UnderLibrary.Problems.ContainsByPredicate([](const FString& Problem) { return Problem.Contains(TEXT("is a Server Function Library")); }));

	UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
	const FSoftObjectPath Saved = Settings->GameKitWorld;
	Settings->GameKitWorld.Reset();
	const CrowdyExecDeploy::FProjectDeploy Unset = CrowdyExecDeploy::AssembleProject({World.Get()}, DirectoryFor);
	Settings->GameKitWorld = FSoftObjectPath(World.Get());
	const CrowdyExecDeploy::FProjectDeploy Set = CrowdyExecDeploy::AssembleProject({World.Get()}, DirectoryFor);
	Settings->GameKitWorld = Saved;
	auto NamesPlayersOnline = [](const FString& Warning) { return Warning.Contains(TEXT("Get Players Online reads")); };
	TestTrue(TEXT("a world root the Game Kit World setting does not name is warned of"), Unset.Warnings.ContainsByPredicate(NamesPlayersOnline));
	TestFalse(TEXT("and the setting naming it is not"), Set.Warnings.ContainsByPredicate(NamesPlayersOnline));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Leaderboard = MakeStarter(*this, TEXT("leaderboard"));
	if (!Leaderboard)
	{
		return false;
	}
	Session->KitConfig = TEXT("{\"board\": \"weekly\"}");
	auto NamesNoBoard = [](const FString& Warning) { return Warning.Contains(TEXT("posts scores to the board weekly, but the project has no leaderboard")); };
	TestTrue(TEXT("a session posting to a board in a project with no leaderboard is warned of"),
		CrowdyExecDeploy::AssembleProject({World.Get(), Session.Get()}, DirectoryFor).Warnings.ContainsByPredicate(NamesNoBoard));
	TestFalse(TEXT("and with the leaderboard it is not"),
		CrowdyExecDeploy::AssembleProject({World.Get(), Session.Get(), Leaderboard.Get()}, DirectoryFor).Warnings.ContainsByPredicate(NamesNoBoard));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitWorldConfigTest, "CrowdySDK.CrowdyExecEditor.GameKit.WorldConfigWithWrongTypesIsAProblem", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitWorldConfigTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	if (!World)
	{
		return false;
	}
	// world's folder is taken, so a Config AddStarter lets through stops at "already exists" before anything is written.
	const FStarterFolder Folder;
	if (!TestTrue(TEXT("world's crate is copied"), Folder.CopyStarter(TEXT("world"))))
	{
		return false;
	}
	auto DirectoryFor = [&Folder](const UCrowdyServerObjectDefinition& Definition) { return FPaths::Combine(Folder.Path, Definition.TypeName); };

	for (const TCHAR* Bad : {TEXT("{\"nodes\":\"many\"}"), TEXT("{\"day_ms\":-1}"), TEXT("{\"nodes\":1.5}"), TEXT("{\"nodes\":70000}"), TEXT("{\"node_amount\":null}"), TEXT("{\"regrow_ms\":true}"),
		TEXT("{\"nodes\":3.0}"), TEXT("{\"day_ms\":6e5}"), TEXT("{\"day_ms\":6E+5}"), TEXT("{\"regrow_ms\":-0}"), TEXT("{\"node_amount\":99999999999999999999}")})
	{
		World->KitConfig = Bad;
		const CrowdyExecDeploy::FProjectDeploy Project = CrowdyExecDeploy::AssembleProject({World.Get()}, DirectoryFor);
		TestTrue(FString::Printf(TEXT("%s is a deploy problem (%s)"), Bad, *FString::Join(Project.Problems, TEXT(" | "))),
			Project.Problems.Num() == 1 && Project.Problems[0].StartsWith(TEXT("world: its Game Kit Config has "), ESearchCase::CaseSensitive) && Project.Problems[0].Contains(TEXT("whole number")));

		TArray<FString> Added;
		FString Error;
		TestFalse(FString::Printf(TEXT("%s is refused by Add starter"), Bad), CrowdyGameKitStarters::AddStarter(TEXT("world"), {}, Folder.Path, Added, Error, Bad));
		TestTrue(FString::Printf(TEXT("saying which key (%s)"), *Error), Error.Contains(TEXT("whole number")) && Added.IsEmpty());
	}

	for (const TCHAR* Good : {TEXT("{\"nodes\":3}"), TEXT("{\"nodes\":0,\"day_ms\":600000,\"node_amount\":4294967295,\"regrow_ms\":1000,\"unknown\":\"x\"}"), TEXT("{\"Nodes\":\"x\",\"DAY_MS\":1.5}"), TEXT("{\"extra\":{\"nodes\":1.5,\"a\":[1.5]},\"day_ms\":600000}")})
	{
		World->KitConfig = Good;
		TestEqualSensitive(FString::Printf(TEXT("%s is no problem to deploy"), Good), FString::Join(CrowdyExecDeploy::AssembleProject({World.Get()}, DirectoryFor).Problems, TEXT(" | ")), FString());

		TArray<FString> Added;
		FString Error;
		TestFalse(FString::Printf(TEXT("%s passes the Config check, then meets world's taken folder"), Good), CrowdyGameKitStarters::AddStarter(TEXT("world"), {}, Folder.Path, Added, Error, Good));
		TestTrue(FString::Printf(TEXT("(%s)"), *Error), Error.Contains(TEXT("already exists")) && !Error.Contains(TEXT("whole number")));
	}

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Npcs = MakeStarter(*this, TEXT("npcs"));
	if (!Npcs || !TestTrue(TEXT("npcs' crate is copied"), Folder.CopyStarter(TEXT("npcs"))))
	{
		return false;
	}
	World->KitConfig.Reset();
	auto NpcsProblems = [&](const TCHAR* Config)
	{
		Npcs->KitConfig = Config;
		return FString::Join(CrowdyExecDeploy::AssembleProject({World.Get(), Npcs.Get()}, DirectoryFor).Problems, TEXT(" | "));
	};
	for (const TCHAR* Height : {TEXT("{\"z\":-30}"), TEXT("{\"z\":30}"), TEXT("{\"z\":0}"), TEXT("{\"z\":-4294967296}"), TEXT("{\"z\":4294967296}")})
	{
		TestEqualSensitive(FString::Printf(TEXT("%s is no problem to deploy"), Height), NpcsProblems(Height), FString());
	}
	for (const TCHAR* Bad : {TEXT("{\"z\":1.5}"), TEXT("{\"z\":-1.5}"), TEXT("{\"z\":-0}"), TEXT("{\"z\":1e2}"), TEXT("{\"z\":4294967297}"), TEXT("{\"z\":-4294967297}"), TEXT("{\"z\":-9223372036854775808}"), TEXT("{\"z\":\"5\"}")})
	{
		const FString Problems = NpcsProblems(Bad);
		TestTrue(FString::Printf(TEXT("%s is a deploy problem (%s)"), Bad, *Problems),
			Problems.StartsWith(TEXT("npcs: its Game Kit Config has "), ESearchCase::CaseSensitive) && Problems.Contains(TEXT("whole number from -4294967296 to 4294967296")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitConfigIdCaseTest, "CrowdySDK.CrowdyExecEditor.GameKit.ConfigIdsDifferingOnlyByCaseAreAProblem", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitConfigIdCaseTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Player = MakeStarter(*this, TEXT("player"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Market = MakeStarter(*this, TEXT("market"));
	const FStarterFolder Folder;
	if (!World || !Player || !Market || !TestTrue(TEXT("the crates are copied"), Folder.CopyStarter(TEXT("world")) && Folder.CopyStarter(TEXT("player")) && Folder.CopyStarter(TEXT("market"))))
	{
		return false;
	}
	auto DirectoryFor = [&Folder](const UCrowdyServerObjectDefinition& Definition) { return FPaths::Combine(Folder.Path, Definition.TypeName); };
	auto Problems = [&](UCrowdyServerObjectDefinition& Starter, const TCHAR* Config)
	{
		Starter.KitConfig = Config;
		return FString::Join(CrowdyExecDeploy::AssembleProject({World.Get(), Player.Get(), Market.Get()}, DirectoryFor).Problems, TEXT(" | "));
	};

	const TCHAR* const BadPlayer[] = {
		TEXT("{\"items\":[{\"id\":\"Ore\"},{\"id\":\"ore\"}]}"),
		TEXT("{\"currencies\":[\"Gold\",\"gold\"]}"),
		TEXT("{\"start\":{\"items\":{\"Ore\":1,\"ore\":2}}}"),
		TEXT("{\"production\":{\"capacity\":{\"Wood\":1,\"wood\":2}}}"),
	};
	for (const TCHAR* Bad : BadPlayer)
	{
		const FString Found = Problems(*Player, Bad);
		TestTrue(FString::Printf(TEXT("%s is a deploy problem (%s)"), Bad, *Found), Found.Contains(TEXT("player: its Game Kit Config has two ")) && Found.Contains(TEXT("differ only by case")));

		TArray<FString> Added;
		FString Error;
		TestFalse(FString::Printf(TEXT("%s is refused by Add starter"), Bad), CrowdyGameKitStarters::AddStarter(TEXT("player"), {}, Folder.Path, Added, Error, Bad));
		TestTrue(FString::Printf(TEXT("saying so (%s)"), *Error), Error.Contains(TEXT("differ only by case")) && Added.IsEmpty());
	}
	Player->KitConfig.Reset();

	const FString Found = Problems(*Market, TEXT("{\"book_items\":[\"Ore\",\"ore\"]}"));
	TestTrue(FString::Printf(TEXT("a market's book items differing by case are a deploy problem (%s)"), *Found), Found.Contains(TEXT("market: its Game Kit Config has two item ids")));
	Market->KitConfig.Reset();

	for (const TCHAR* Good : {TEXT("{\"items\":[{\"id\":\"ore\"},{\"id\":\"ore\"}]}"), TEXT("{\"currencies\":[\"gold\",\"gems\"]}"), TEXT("{\"currencies\":[\"Ore\"],\"items\":[{\"id\":\"ore\"}]}")})
	{
		TestFalse(FString::Printf(TEXT("%s is not a case problem"), Good), Problems(*Player, Good).Contains(TEXT("differ only by case")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitSecretWarningTest, "CrowdySDK.CrowdyExecEditor.GameKit.DeployWarnsOfAStarterWithNoSecret", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitSecretWarningTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Player = MakeStarter(*this, TEXT("player"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Deck = MakeStarter(*this, TEXT("deck"));
	if (!World || !Player || !Deck)
	{
		return false;
	}
	auto DirectoryFor = [](const UCrowdyServerObjectDefinition& Definition) { return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("GameKitTests"), Definition.TypeName); };
	auto Warns = [&](const TCHAR* Type)
	{
		const FString Start = FString(Type) + TEXT(": its Config has no secret");
		return CrowdyExecDeploy::AssembleProject({World.Get(), Player.Get(), Deck.Get()}, DirectoryFor).Warnings.ContainsByPredicate([&Start](const FString& Warning)
		{
			return Warning.StartsWith(Start, ESearchCase::CaseSensitive);
		});
	};

	TestTrue(TEXT("a player with no Config is warned of"), Warns(TEXT("player")));
	TestTrue(TEXT("and so is a deck"), Warns(TEXT("deck")));
	TestFalse(TEXT("a world, which has no secret to make, is not"), Warns(TEXT("world")));

	auto Refuses = [&](const TCHAR* Type)
	{
		const FString Start = FString(Type) + TEXT(": its Game Kit Config has \"secret\"");
		return CrowdyExecDeploy::AssembleProject({World.Get(), Player.Get(), Deck.Get()}, DirectoryFor).Problems.ContainsByPredicate([&Start](const FString& Problem)
		{
			return Problem.StartsWith(Start, ESearchCase::CaseSensitive);
		});
	};

	// The server code reads keys with case, so Secret is no secret.
	for (const TCHAR* NoSecret : {TEXT("{}"), TEXT("{\"secret\":\"\"}"), TEXT("{\"Secret\":\"x\"}"), TEXT("{\"Secret\":5}"), TEXT("{\"other\":\"x\"}")})
	{
		Player->KitConfig = NoSecret;
		Deck->KitConfig = NoSecret;
		TestTrue(FString::Printf(TEXT("a player with %s is warned of"), NoSecret), Warns(TEXT("player")));
		TestTrue(FString::Printf(TEXT("and so is a deck with %s"), NoSecret), Warns(TEXT("deck")));
		TestFalse(FString::Printf(TEXT("a player with %s is no problem"), NoSecret), Refuses(TEXT("player")));
		TestFalse(FString::Printf(TEXT("nor a deck with %s"), NoSecret), Refuses(TEXT("deck")));
	}

	for (const TCHAR* NotText : {TEXT("{\"secret\":5}"), TEXT("{\"secret\":null}"), TEXT("{\"secret\":{\"a\":1}}"), TEXT("{\"x\":[1],\"secret\":true}")})
	{
		Player->KitConfig = NotText;
		Deck->KitConfig = NotText;
		TestTrue(FString::Printf(TEXT("a player with %s is a deploy problem"), NotText), Refuses(TEXT("player")));
		TestTrue(FString::Printf(TEXT("and so is a deck with %s"), NotText), Refuses(TEXT("deck")));
		TestFalse(FString::Printf(TEXT("a player with %s is not also warned of"), NotText), Warns(TEXT("player")));
	}

	Player->KitConfig = TEXT(" {\"secret\":\"x\"} ");
	Deck->KitConfig = TEXT("{}");
	TestFalse(TEXT("a player with a secret is not"), Warns(TEXT("player")));
	TestTrue(TEXT("while a deck without one still is"), Warns(TEXT("deck")));
	Deck->KitConfig = TEXT("{\"secret\":\"x\",\"turn_ms\":1000}");
	TestFalse(TEXT("a deck with a secret among other keys is not"), Warns(TEXT("deck")));
	TestFalse(TEXT("a world is still not"), Warns(TEXT("world")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStartersPinSdkTest, "CrowdySDK.CrowdyExecEditor.GameKit.StartersPinTheTemplatesSdk", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitStartersPinSdkTest::RunTest(const FString& Parameters)
{
	const FString Pin = FString::Printf(TEXT("ckx-sdk = \"%s\""), CrowdyExecCodegen::CkxSdkVersion);
	for (const CrowdyGameKitStarters::FStarter& Starter : CrowdyGameKitStarters::GetStarters())
	{
		FString Cargo;
		if (!TestTrue(FString::Printf(TEXT("%s's Cargo.toml is read"), Starter.TypeName), FFileHelper::LoadFileToString(Cargo, *(CrowdyGameKitStarters::GetCrateSource(Starter) / TEXT("Cargo.toml")))))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s pins the same ckx-sdk as a new type's crate: %s"), Starter.TypeName, *Pin), Cargo.Contains(Pin, ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitPlanAddTest,"CrowdySDK.CrowdyExecEditor.GameKit.PlanAddBringsWhatAStarterNeeds", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitPlanAddTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	auto Names = [](const TArray<const CrowdyGameKitStarters::FStarter*>& Plan)
	{
		TArray<FString> Out;
		for (const CrowdyGameKitStarters::FStarter* Starter : Plan)
		{
			Out.Add(Starter->TypeName);
		}
		return FString::Join(Out, TEXT(","));
	};
	FString Problem;
	TestEqualSensitive(TEXT("a session brings its parents up to the root, then itself"), Names(CrowdyGameKitStarters::PlanAdd(TEXT("session"), {}, Problem)),
		FString(TEXT("world,matchmaker,session")));
	TestEqualSensitive(TEXT("a matchmaker brings world and the session it calls"), Names(CrowdyGameKitStarters::PlanAdd(TEXT("matchmaker"), {}, Problem)),
		FString(TEXT("world,matchmaker,session")));
	TestEqualSensitive(TEXT("world brings only itself"), Names(CrowdyGameKitStarters::PlanAdd(TEXT("world"), {}, Problem)), FString(TEXT("world")));
	TestEqualSensitive(TEXT("a leaderboard brings world"), Names(CrowdyGameKitStarters::PlanAdd(TEXT("leaderboard"), {}, Problem)), FString(TEXT("world,leaderboard")));
	TestEqualSensitive(TEXT("with nothing in the way"), Problem, FString());

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	TestEqualSensitive(TEXT("a starter the project has is left out"), Names(CrowdyGameKitStarters::PlanAdd(TEXT("session"), {World.Get()}, Problem)),
		FString(TEXT("matchmaker,session")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Own = MakeOwnType(TEXT("matchmaker"));
	TestTrue(TEXT("an own type with a starter's name stops it"), CrowdyGameKitStarters::PlanAdd(TEXT("session"), {Own.Get()}, Problem).IsEmpty() && Problem.Contains(TEXT("rename that type")));
	Problem.Reset();
	TestTrue(TEXT("a starter the SDK does not have is refused"), CrowdyGameKitStarters::PlanAdd(TEXT("racing"), {}, Problem).IsEmpty() && Problem.Contains(TEXT("no Game Kit starter named racing")));

	// world's folder is taken, so even a check that let the Config through would stop before writing anything.
	const FStarterFolder Folder;
	if (!TestTrue(TEXT("world's folder is taken"), Folder.CopyStarter(TEXT("world"))))
	{
		return false;
	}
	TArray<FString> Added;
	FString Error;
	TestFalse(TEXT("a Config that is not a JSON object is refused"), CrowdyGameKitStarters::AddStarter(TEXT("world"), {}, Folder.Path, Added, Error, TEXT("[1]")));
	TestTrue(FString::Printf(TEXT("before anything else, saying so (%s)"), *Error), Error.Contains(TEXT("not a JSON object")) && Added.IsEmpty());

	Error.Reset();
	TestFalse(TEXT("a Config for a starter the project has is refused, not dropped"), CrowdyGameKitStarters::AddStarter(TEXT("world"), {World.Get()}, Folder.Path, Added, Error, TEXT("{\"day_ms\": 600000}")));
	TestTrue(FString::Printf(TEXT("saying where to set it (%s)"), *Error), Error.Contains(TEXT("set Config in its definition")) && Added.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitEconomyStartersTest, "CrowdySDK.CrowdyExecEditor.GameKit.MarketCallsThePlayersHubs", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitEconomyStartersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	auto Names = [](const TArray<const CrowdyGameKitStarters::FStarter*>& Plan)
	{
		TArray<FString> Out;
		for (const CrowdyGameKitStarters::FStarter* Starter : Plan)
		{
			Out.Add(Starter->TypeName);
		}
		return FString::Join(Out, TEXT(","));
	};
	FString Problem;
	TestEqualSensitive(TEXT("a player brings world"), Names(CrowdyGameKitStarters::PlanAdd(TEXT("player"), {}, Problem)), FString(TEXT("world,player")));
	TestEqualSensitive(TEXT("a market brings world and the player it calls"), Names(CrowdyGameKitStarters::PlanAdd(TEXT("market"), {}, Problem)), FString(TEXT("world,market,player")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Player = MakeStarter(*this, TEXT("player"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Market = MakeStarter(*this, TEXT("market"));
	if (!World || !Player || !Market)
	{
		return false;
	}
	const FString Manifest = CrowdyExecDeploy::MakeManifest({World.Get(), Player.Get(), Market.Get()});
	const TSharedPtr<FJsonObject> PlayerType = ManifestType(Manifest, TEXT("player"));
	const TSharedPtr<FJsonObject> MarketType = ManifestType(Manifest, TEXT("market"));
	TestEqualSensitive(TEXT("a player runs under world"), FieldOf(PlayerType, TEXT("parent")), FString(TEXT("world")));
	TestEqualSensitive(TEXT("and calls nothing"), CallsOf(PlayerType), FString());
	TestEqualSensitive(TEXT("a market runs under world"), FieldOf(MarketType, TEXT("parent")), FString(TEXT("world")));
	TestEqualSensitive(TEXT("and calls the players' hubs"), CallsOf(MarketType), FString(TEXT("player")));
	TestEqualSensitive(TEXT("a market without player calls nothing"), CallsOf(ManifestType(CrowdyExecDeploy::MakeManifest({World.Get(), Market.Get()}), TEXT("market"))), FString());
	TestTrue(TEXT("a player's profile is its State"), Player->State == FCrowdyKitPlayerState::StaticStruct() && Player->KitStateTopic == TEXT("player"));
	TestTrue(TEXT("a market's view is its State"), Market->State == FCrowdyKitMarketState::StaticStruct() && Market->KitStateTopic == TEXT("market"));
	TestTrue(TEXT("a player is keyed by its player, and a market is not"), Player->IsKeyedByPlayer() && !Market->IsKeyedByPlayer());
	TestEqual(TEXT("a player's inventory, wallet and progress are its Owner Values"), Player->KitOwnerValues.Num(), 3);
	TestEqual(TEXT("a market's orders and trades are its"), Market->KitOwnerValues.Num(), 2);
	for (const TCHAR* HubOnly : {TEXT("hold"), TEXT("settle"), TEXT("release"), TEXT("exchange")})
	{
		TestFalse(FString::Printf(TEXT("the player's %s, which only a market's server code may call, is left out"), HubOnly),
			Player->Functions.ContainsByPredicate([HubOnly](const FCrowdyServerFunction& Function) { return Function.ServerName == HubOnly; }));
	}

	auto Removed = [](const TArray<UCrowdyServerObjectDefinition*>& Plan)
	{
		TArray<FString> Out;
		for (const UCrowdyServerObjectDefinition* Definition : Plan)
		{
			Out.Add(Definition->TypeName);
		}
		return FString::Join(Out, TEXT(","));
	};
	const TArray<UCrowdyServerObjectDefinition*> Project = {World.Get(), Player.Get(), Market.Get()};
	TestEqualSensitive(TEXT("removing player takes the market that calls it first"), Removed(CrowdyGameKitStarters::PlanRemove(TEXT("player"), Project, Problem)),
		FString(TEXT("market,player")));
	TestEqualSensitive(TEXT("removing the market leaves player"), Removed(CrowdyGameKitStarters::PlanRemove(TEXT("market"), Project, Problem)), FString(TEXT("market")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitFreshRefTest, "CrowdySDK.CrowdyExecEditor.GameKit.GrantAndDepositTakeAFreshRef", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitFreshRefTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Player = MakeStarter(*this, TEXT("player"));
	const FCrowdyServerFunction* Grant = Player ? Player->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("grant"); }) : nullptr;
	const FCrowdyServerFunction* Craft = Player ? Player->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("craft"); }) : nullptr;
	if (!TestNotNull(TEXT("the player starter has grant"), Grant) || !TestNotNull(TEXT("and craft"), Craft))
	{
		return false;
	}
	FInstancedStruct Inputs;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("a grant's inputs read from JSON (%s)"), *Error),
		CrowdyExecDeveloperCall::InputsFromJson(*Grant, TEXT("{\"Bundle\": {\"currency\": {\"gold\": 100}}, \"Xp\": 5}"), Inputs, Error)))
	{
		return false;
	}
	FString Ref;
	CrowdyExecDeveloperCall::FillFreshRef(*Player, *Grant, Inputs, Ref);
	const FCrowdyKitGrantInputs* Read = Inputs.GetPtr<FCrowdyKitGrantInputs>();
	TestTrue(FString::Printf(TEXT("an empty Ref is made fresh (%s)"), *Ref), Ref.StartsWith(TEXT("studio-grant-")) && Ref.Len() == 13 + 32 && Ref.Len() <= 128);
	TestTrue(TEXT("into the inputs sent"), Read && Read->Ref == Ref && Read->Bundle.Currency.FindRef(TEXT("gold")) == 100 && Read->Xp == 5);
	FInstancedStruct Again;
	CrowdyExecDeveloperCall::InputsFromJson(*Grant, TEXT("{\"Bundle\": {\"Currency\": {\"gold\": 100}}}"), Again, Error);
	FString Second;
	CrowdyExecDeveloperCall::FillFreshRef(*Player, *Grant, Again, Second);
	TestNotEqual(TEXT("each press makes another"), Second, Ref);

	FInstancedStruct Typed;
	CrowdyExecDeveloperCall::InputsFromJson(*Grant, TEXT("{\"Ref\": \"quest-reward:4211\"}"), Typed, Error);
	FString Kept;
	CrowdyExecDeveloperCall::FillFreshRef(*Player, *Grant, Typed, Kept);
	TestEqualSensitive(TEXT("a Ref given is sent as it is, to send the same grant again"), Kept, FString(TEXT("quest-reward:4211")));

	FInstancedStruct CraftInputs;
	CrowdyExecDeveloperCall::InputsFromJson(*Craft, TEXT("{\"Recipe\": \"plank\"}"), CraftInputs, Error);
	FString None;
	CrowdyExecDeveloperCall::FillFreshRef(*Player, *Craft, CraftInputs, None);
	TestTrue(TEXT("inputs without a Ref take none"), None.IsEmpty());

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Own(DuplicateObject(Player.Get(), GetTransientPackage()));
	Own->CodeSource = ECrowdyServerCodeSource::Generated;
	FInstancedStruct OwnInputs;
	CrowdyExecDeveloperCall::InputsFromJson(*Grant, TEXT("{}"), OwnInputs, Error);
	FString OwnRef;
	CrowdyExecDeveloperCall::FillFreshRef(*Own, *Grant, OwnInputs, OwnRef);
	TestTrue(TEXT("an own type's Ref is left as it is"), OwnRef.IsEmpty() && OwnInputs.GetPtr<FCrowdyKitGrantInputs>()->Ref.IsEmpty());

	TestTrue(TEXT("a grant of gold to a player's user id is sent"), CrowdyExecDeveloperCall::FindPointlessCall(*Player, TEXT("101910912345678"), Inputs).IsEmpty());
	for (const TCHAR* Key : {TEXT("007"), TEXT("+7"), TEXT("0"), TEXT("-7"), TEXT("alice"), TEXT("")})
	{
		TestFalse(FString::Printf(TEXT("a grant to \"%s\", a hub no game reads, is refused"), Key), CrowdyExecDeveloperCall::FindPointlessCall(*Player, Key, Inputs).IsEmpty());
	}
	FInstancedStruct Empty;
	CrowdyExecDeveloperCall::InputsFromJson(*Grant, TEXT("{\"Bundle\": {}}"), Empty, Error);
	TestTrue(TEXT("a grant that gives nothing is refused, saying so"), CrowdyExecDeveloperCall::FindPointlessCall(*Player, TEXT("7"), Empty).Contains(TEXT("gives the player nothing")));
	FInstancedStruct XpOnly;
	CrowdyExecDeveloperCall::InputsFromJson(*Grant, TEXT("{\"Xp\": 50}"), XpOnly, Error);
	TestTrue(TEXT("while one of experience alone is sent"), CrowdyExecDeveloperCall::FindPointlessCall(*Player, TEXT("7"), XpOnly).IsEmpty());
	TestTrue(TEXT("and a call to an own type keyed by anything is not judged"), CrowdyExecDeveloperCall::FindPointlessCall(*Own, TEXT("007"), Empty).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitDeveloperMethodsTest, "CrowdySDK.CrowdyExecEditor.GameKit.DeveloperMethodsAreDevelopersOnly", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitDeveloperMethodsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	int32 DeveloperOnly = 0;
	for (const CrowdyGameKitStarters::FStarter& Starter : CrowdyGameKitStarters::GetStarters())
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, Starter.TypeName);
		TArray<FString> Lines;
		if (!Definition || !TestTrue(FString::Printf(TEXT("%s's Rust is readable"), Starter.TypeName),
			FFileHelper::LoadFileToStringArray(Lines, *FPaths::Combine(CrowdyGameKitStarters::GetCrateSource(Starter), TEXT("src/lib.rs")))))
		{
			return false;
		}
		for (const FCrowdyServerFunction& Function : Definition->Functions)
		{
			// A method its Rust keeps from players checks so on the first line of its match arm, or is named where its handle refuses players.
			const FString Arm = FString::Printf(TEXT("\"%s\" => {"), *Function.ServerName);
			const FString Method = FString::Printf(TEXT("\"%s\""), *Function.ServerName);
			const int32 At = Lines.IndexOfByPredicate([&Arm](const FString& Line) { return Line.TrimStart().StartsWith(Arm, ESearchCase::CaseSensitive); });
			const bool bRefusedToPlayers = Lines.ContainsByPredicate([&Method](const FString& Line)
			{
				return Line.Contains(TEXT("Caller::Player(_) if matches!(call.method,"), ESearchCase::CaseSensitive) && Line.Contains(Method, ESearchCase::CaseSensitive);
			});
			const bool bRustDeveloper = bRefusedToPlayers || (Lines.IsValidIndex(At + 1)
				&& (Lines[At + 1].Contains(TEXT("developer("), ESearchCase::CaseSensitive) || Lines[At + 1].Contains(TEXT("Caller::Developer("), ESearchCase::CaseSensitive)));
			DeveloperOnly += bRustDeveloper ? 1 : 0;
			TestEqual(FString::Printf(TEXT("%s's %s is Developers Only exactly when its Rust says so"), Starter.TypeName, *Function.Name.ToString()),
				Function.WhoCanCall == ECrowdyServerFunctionCaller::DevelopersOnly, bRustDeveloper);
			TestEqual(FString::Printf(TEXT("and a developer call reaches %s's %s exactly then"), Starter.TypeName, *Function.Name.ToString()),
				CrowdyGameKitStarters::IsDeveloperOnly(*Definition, Function), bRustDeveloper);
		}
	}
	TestEqual(TEXT("world's schedule and cancel, the matchmaker's configure, the leaderboard's submit, configure and roll, the player's grant, record and deposit, and the territory's reset are the developers' only"),
		DeveloperOnly, 10);

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Saved = MakeStarter(*this, TEXT("matchmaker"));
	FCrowdyServerFunction* Configure = Saved ? Saved->Functions.FindByPredicate([](const FCrowdyServerFunction& Function) { return Function.ServerName == TEXT("configure"); }) : nullptr;
	if (!TestNotNull(TEXT("the matchmaker has configure"), Configure))
	{
		return false;
	}
	Configure->WhoCanCall = ECrowdyServerFunctionCaller::Players;
	TestTrue(TEXT("an asset saved before it said Developers Only is still called as the developer"), CrowdyExecDeveloperCall::IsForDevelopers(*Saved, *Configure));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitPlayerReadsTest, "CrowdySDK.CrowdyExecEditor.GameKit.PlayerReadsAreReadOnlyPlayersFunctions", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitPlayerReadsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Npcs = MakeStarter(*this, TEXT("npcs"));
	if (!Npcs)
	{
		return false;
	}
	auto Find = [&Npcs](const TCHAR* Method) { return Npcs->Functions.FindByPredicate([Method](const FCrowdyServerFunction& Function) { return Function.GetMethodName() == Method; }); };
	const FCrowdyServerFunction* State = Find(TEXT("state"));
	const FCrowdyServerFunction* Pets = Find(TEXT("pets"));
	if (!TestNotNull(TEXT("npcs has state"), State) || !TestNotNull(TEXT("npcs has pets"), Pets))
	{
		return false;
	}
	TestTrue(TEXT("an area's state is a player read"), CrowdyGameKitStarters::IsPlayerRead(*Npcs, *State));
	TestFalse(TEXT("and still Callable By players"), CrowdyExecDeveloperCall::IsForDevelopers(*Npcs, *State));
	TestFalse(TEXT("pets reads the caller's own, so it is not"), CrowdyGameKitStarters::IsPlayerRead(*Npcs, *Pets));

	Npcs->CodeSource = ECrowdyServerCodeSource::Generated;
	TestFalse(TEXT("a project's own type named npcs reads nothing for a developer"), CrowdyGameKitStarters::IsPlayerRead(*Npcs, *State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitRemoveStarterTest, "CrowdySDK.CrowdyExecEditor.GameKit.RemoveStarterTakesWhatNeedsIt", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitRemoveStarterTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	auto Names = [](const TArray<UCrowdyServerObjectDefinition*>& Plan)
	{
		TArray<FString> Out;
		for (const UCrowdyServerObjectDefinition* Definition : Plan)
		{
			Out.Add(Definition->TypeName);
		}
		return FString::Join(Out, TEXT(","));
	};
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Matchmaker = MakeStarter(*this, TEXT("matchmaker"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Session = MakeStarter(*this, TEXT("session"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Own = MakeOwnType(TEXT("sbx_own"));
	if (!World || !Matchmaker || !Session)
	{
		return false;
	}
	const TArray<UCrowdyServerObjectDefinition*> Project = {World.Get(), Matchmaker.Get(), Session.Get(), Own.Get()};
	FString Problem;
	TestEqualSensitive(TEXT("world takes what runs under it, deepest first"), Names(CrowdyGameKitStarters::PlanRemove(TEXT("world"), Project, Problem)),
		FString(TEXT("session,matchmaker,world")));
	TestEqualSensitive(TEXT("a session takes the matchmaker that calls it"), Names(CrowdyGameKitStarters::PlanRemove(TEXT("session"), Project, Problem)),
		FString(TEXT("matchmaker,session")));
	TestEqualSensitive(TEXT("a matchmaker takes the session under it, not world"), Names(CrowdyGameKitStarters::PlanRemove(TEXT("matchmaker"), Project, Problem)),
		FString(TEXT("session,matchmaker")));
	TestEqualSensitive(TEXT("with nothing in the way"), Problem, FString());
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Leaderboard = MakeStarter(*this, TEXT("leaderboard"));
	if (!Leaderboard)
	{
		return false;
	}
	const TArray<UCrowdyServerObjectDefinition*> WithBoard = {World.Get(), Matchmaker.Get(), Session.Get(), Leaderboard.Get()};
	TestEqualSensitive(TEXT("a leaderboard leaves the session that may post to it"), Names(CrowdyGameKitStarters::PlanRemove(TEXT("leaderboard"), WithBoard, Problem)),
		FString(TEXT("leaderboard")));
	TestEqualSensitive(TEXT("and a session leaves the leaderboard it may post to"), Names(CrowdyGameKitStarters::PlanRemove(TEXT("session"), WithBoard, Problem)),
		FString(TEXT("matchmaker,session")));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> OwnWorld = MakeOwnType(TEXT("world"));
	TestTrue(TEXT("an own type with a starter's name is not removed"), CrowdyGameKitStarters::PlanRemove(TEXT("world"), {OwnWorld.Get()}, Problem).IsEmpty()
		&& Problem.Contains(TEXT("no Game Kit starter named world")));

	const FStarterFolder Folder;
	const FString OwnFile = FPaths::Combine(Folder.Path, TEXT("sbx_own"), TEXT("Cargo.toml"));
	if (!TestTrue(TEXT("the starters' crates are in place"), Folder.CopyStarter(TEXT("world")) && Folder.CopyStarter(TEXT("matchmaker")) && Folder.CopyStarter(TEXT("session")))
		|| !TestTrue(TEXT("and an own type's"), FFileHelper::SaveStringToFile(TEXT("[package]"), *OwnFile)))
	{
		return false;
	}
	TArray<FString> Removed;
	FString Error;
	TestTrue(TEXT("removing world succeeds"), CrowdyGameKitStarters::RemoveStarter(TEXT("world"), Project, Folder.Path, Removed, Error));
	TestEqualSensitive(TEXT("with no error"), Error, FString());
	TestEqualSensitive(TEXT("and names what it removed"), FString::Join(Removed, TEXT(",")), FString(TEXT("session,matchmaker,world")));
	for (const TCHAR* Type : {TEXT("world"), TEXT("matchmaker"), TEXT("session")})
	{
		TestFalse(FString::Printf(TEXT("%s's folder is gone"), Type), IFileManager::Get().DirectoryExists(*FPaths::Combine(Folder.Path, Type)));
	}
	TestTrue(TEXT("an own type's folder stays"), FPaths::FileExists(OwnFile));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitRemoveSavedStartersTest, "CrowdySDK.CrowdyExecEditor.GameKit.RemoveStarterDeletesSavedDefinitions", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitRemoveSavedStartersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	// Clearing the Game Kit World setting writes the project's ini, so it is put back byte for byte.
	const FString IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini"));
	TArray<uint8> IniBytes;
	const bool bHadIni = FFileHelper::LoadFileToArray(IniBytes, *IniPath);
	UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
	const FSoftObjectPath SavedWorld = Settings->GameKitWorld;
	const FString Root = FString::Printf(TEXT("/Game/GameKitTests_%s"), *FGuid::NewGuid().ToString());
	ON_SCOPE_EXIT
	{
		Settings->GameKitWorld = SavedWorld;
		if (bHadIni)
		{
			FFileHelper::SaveArrayToFile(IniBytes, *IniPath);
		}
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root + TEXT("/")), false, true);
	};

	TArray<UCrowdyServerObjectDefinition*> Project;
	for (const TCHAR* TypeName : {TEXT("world"), TEXT("matchmaker"), TEXT("session")})
	{
		const FString PackageName = FString::Printf(TEXT("%s/CSO_Kit_%s"), *Root, TypeName);
		UPackage* Package = CreatePackage(*PackageName);
		UCrowdyServerObjectDefinition* Definition = CrowdyGameKitStarters::MakeDefinition(*CrowdyGameKitStarters::FindStarter(TypeName), Package,
			FName(FPackageName::GetShortName(PackageName)), RF_Public | RF_Standalone);
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		if (!TestNotNull(FString::Printf(TEXT("the %s definition is made"), TypeName), Definition)
			|| !TestTrue(FString::Printf(TEXT("the %s definition is saved"), TypeName), UPackage::SavePackage(Package, Definition,
				*FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), Args)))
		{
			return false;
		}
		Project.Add(Definition);
	}
	const FStarterFolder Folder;
	if (!TestTrue(TEXT("the starters' crates are in place"), Folder.CopyStarter(TEXT("world")) && Folder.CopyStarter(TEXT("matchmaker")) && Folder.CopyStarter(TEXT("session"))))
	{
		return false;
	}
	Settings->GameKitWorld = FSoftObjectPath(Project[0]);

	TArray<FString> Removed;
	FString Error;
	TestTrue(FString::Printf(TEXT("removing saved starters succeeds (%s)"), *Error), CrowdyGameKitStarters::RemoveStarter(TEXT("world"), Project, Folder.Path, Removed, Error));
	TestEqualSensitive(TEXT("naming what it removed"), FString::Join(Removed, TEXT(",")), FString(TEXT("session,matchmaker,world")));
	TestTrue(TEXT("the Game Kit World setting that named world is cleared"), Settings->GameKitWorld.IsNull());
	for (const TCHAR* TypeName : {TEXT("world"), TEXT("matchmaker"), TEXT("session")})
	{
		TestFalse(FString::Printf(TEXT("%s's definition file is deleted"), TypeName), FPackageName::DoesPackageExist(FString::Printf(TEXT("%s/CSO_Kit_%s"), *Root, TypeName)));
		TestFalse(FString::Printf(TEXT("%s's folder is deleted"), TypeName), IFileManager::Get().DirectoryExists(*FPaths::Combine(Folder.Path, TypeName)));
	}
	return true;
}

#if PLATFORM_WINDOWS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitRemoveStarterLinkTest, "CrowdySDK.CrowdyExecEditor.GameKit.RemoveStarterRefusesALinkInside", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitRemoveStarterLinkTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Matchmaker = MakeStarter(*this, TEXT("matchmaker"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Session = MakeStarter(*this, TEXT("session"));
	if (!World || !Matchmaker || !Session)
	{
		return false;
	}
	const FStarterFolder Folder;
	const FStarterFolder Outside;
	const FString Kept = FPaths::Combine(Outside.Path, TEXT("kept.txt"));
	const FString Link = FPaths::Combine(Folder.Path, TEXT("world"), TEXT("src"), TEXT("deep"), TEXT("linked"));
	if (!TestTrue(TEXT("the starters' crates are in place"), Folder.CopyStarter(TEXT("world")) && Folder.CopyStarter(TEXT("matchmaker")) && Folder.CopyStarter(TEXT("session")))
		|| !TestTrue(TEXT("with a file outside them"), FFileHelper::SaveStringToFile(TEXT("kept"), *Kept))
		|| !TestTrue(TEXT("and a folder deep in world's"), IFileManager::Get().MakeDirectory(*FPaths::GetPath(Link), true)))
	{
		return false;
	}
	// A junction needs no rights a symbolic link does; rmdir removes only the link, so the folders' cleanup cannot follow it.
	auto Shell = [](const FString& Command)
	{
		int32 Code = -1;
		FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *(TEXT("/c ") + Command), &Code, nullptr, nullptr);
		return Code == 0;
	};
	ON_SCOPE_EXIT { Shell(FString::Printf(TEXT("rmdir \"%s\""), *FPaths::ConvertRelativePathToFull(Link).Replace(TEXT("/"), TEXT("\\")))); };
	if (!TestTrue(TEXT("a junction to the outside folder is made"), Shell(FString::Printf(TEXT("mklink /J \"%s\" \"%s\""),
		*FPaths::ConvertRelativePathToFull(Link).Replace(TEXT("/"), TEXT("\\")), *FPaths::ConvertRelativePathToFull(Outside.Path).Replace(TEXT("/"), TEXT("\\"))))))
	{
		return false;
	}
	TArray<FString> Removed;
	FString Error;
	TestFalse(TEXT("removing world is refused"), CrowdyGameKitStarters::RemoveStarter(TEXT("world"), {World.Get(), Matchmaker.Get(), Session.Get()}, Folder.Path, Removed, Error));
	TestTrue(FString::Printf(TEXT("saying a link is in the way (%s)"), *Error), Error.Contains(TEXT("holds, a link")));
	TestTrue(TEXT("nothing is removed"), Removed.IsEmpty());
	for (const TCHAR* Type : {TEXT("world"), TEXT("matchmaker"), TEXT("session")})
	{
		TestTrue(FString::Printf(TEXT("%s's folder stays"), Type), IFileManager::Get().DirectoryExists(*FPaths::Combine(Folder.Path, Type)));
	}
	TestTrue(TEXT("and what the link points to is untouched"), FPaths::FileExists(Kept));
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitStarterPackCratesTest, "CrowdySDK.CrowdyExecEditor.GameKit.StarterPackSendsEachCrateOnce", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitStarterPackCratesTest::RunTest(const FString& Parameters)
{
	CrowdyExecDeveloper::FStarterPack Pack;
	for (const TCHAR* Pair : {TEXT("world:world-tick"), TEXT("race:racing"), TEXT("ball:racing"), TEXT("zone:territory"), TEXT("territory:territory")})
	{
		FString NodeType;
		FString Crate;
		FString(Pair).Split(TEXT(":"), &NodeType, &Crate);
		CrowdyExecDeveloper::FStarter& Starter = Pack.Starters.AddDefaulted_GetRef();
		Starter.NodeType = NodeType;
		Starter.Crate.Name = Crate;
	}
	const TArray<CrowdyExecDeveloper::FBuildCrate> Crates = Pack.GetCrates();
	TArray<FString> Names;
	for (const CrowdyExecDeveloper::FBuildCrate& Crate : Crates)
	{
		Names.Add(Crate.Name);
	}
	TestEqualSensitive(TEXT("a crate that serves two node types is built once, so 17 starters stay within a build's 16 crates"),
		FString::Join(Names, TEXT(",")), FString(TEXT("world-tick,racing,territory")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitGenerateLeavesStartersTest, "CrowdySDK.CrowdyExecEditor.GameKit.GenerateLeavesStarterCodeAlone", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitGenerateLeavesStartersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	if (!World)
	{
		return false;
	}
	CrowdyExecCodegen::FGeneratedCrate Crate;
	FString Error;
	TestFalse(TEXT("Generate writes nothing for a starter"), CrowdyExecCodegen::Generate(*World, Crate, Error));
	TestTrue(FString::Printf(TEXT("and says where its code is (%s)"), *Error), Error.Contains(TEXT("Game Kit starter")) && Error.Contains(TEXT("src/lib.rs")));
	TestTrue(TEXT("a starter's Server Functions are its lib.rs"), CrowdyExecCodegen::GetLogicFile(*World).EndsWith(TEXT("/world/src/lib.rs")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitRevisionsTest, "CrowdySDK.CrowdyExecEditor.GameKit.RevisionsSeeTheRootSwitch", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitRevisionsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	using CrowdyExecRevisions::EChange;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	if (!World)
	{
		return false;
	}
	CrowdyExecDeploy::FProjectDeploy Kit;
	Kit.Root = TEXT("world");
	CrowdyExecDeploy::FTypeState& State = Kit.Types.AddDefaulted_GetRef();
	State.TypeName = TEXT("world");
	State.State = CrowdyExecDeploy::ECrateState::UpToDate;
	CrowdyExecDeveloper::FBuildCrate& Crate = Kit.Crates.AddDefaulted_GetRef();
	Crate.Name = TEXT("world");
	Crate.Files.Add(CrowdyExecDeveloper::FBuildFile{TEXT("src/lib.rs"), TEXT("fn main() {}\n")});
	auto NoHistory = [](const FString&) { return TArray<CrowdyExecRevisions::FRevision>(); };
	auto Find = [](const TArray<CrowdyExecRevisions::FTypeChange>& Changes, const TCHAR* TypeName)
	{
		return Changes.FindByPredicate([TypeName](const CrowdyExecRevisions::FTypeChange& Change) { return Change.TypeName == TypeName; });
	};

	CrowdyExecDeveloper::FVersion SdkRooted;
	SdkRooted.Version = 4;
	SdkRooted.ManifestJson = TEXT("{\"root\":\"root\",\"types\":{\"root\":{\"kind\":\"hub\",\"crate\":\"root\",\"client\":true,\"digest\":\"aa\"}}}");
	const TArray<CrowdyExecRevisions::FTypeChange> ToKit = CrowdyExecRevisions::Compare(Kit, {World.Get()}, &SdkRooted, true, NoHistory);
	const CrowdyExecRevisions::FTypeChange* Root = Find(ToKit, TEXT("root"));
	TestTrue(TEXT("with world as the root, the live SDK root is removed"), Root && Root->Change == EChange::Removed);
	const CrowdyExecRevisions::FTypeChange* New = Find(ToKit, TEXT("world"));
	TestTrue(TEXT("and world is new"), New && New->Change == EChange::New);

	World->KitConfig = TEXT("{\"day_ms\": 1000}");
	CrowdyExecDeveloper::FVersion KitRooted;
	KitRooted.Version = 5;
	KitRooted.ManifestJson = FString::Printf(TEXT("{\"root\":\"world\",\"types\":{\"world\":{\"kind\":\"hub\",\"crate\":\"world\",\"client\":true,\"persist_every_ms\":10000,\"seed_b64\":\"%s\"}}}"),
		*FBase64::Encode(TEXT("{\"day_ms\": 5000}")));
	const TArray<CrowdyExecRevisions::FTypeChange> Reconfigured = CrowdyExecRevisions::Compare(Kit, {World.Get()}, &KitRooted, true, NoHistory);
	const CrowdyExecRevisions::FTypeChange* Seeded = Find(Reconfigured, TEXT("world"));
	TestTrue(TEXT("a Config that differs from the live seed is a change"), Seeded && Seeded->What.Contains(TEXT("config")));
	TestNull(TEXT("and no SDK root is asked for"), Find(Reconfigured, TEXT("root")));
	World->KitConfig = TEXT("{\"day_ms\": 5000}");
	const TArray<CrowdyExecRevisions::FTypeChange> Unchanged = CrowdyExecRevisions::Compare(Kit, {World.Get()}, &KitRooted, true, NoHistory);
	const CrowdyExecRevisions::FTypeChange* Same = Find(Unchanged, TEXT("world"));
	TestTrue(TEXT("the same Config is no change"), Same && !Same->What.Contains(TEXT("config")));
	TestTrue(TEXT("the root has no parent to change"), Same && !Same->What.Contains(TEXT("parent")));

	// The platform's versions list a seed only as its size.
	CrowdyExecDeveloper::FVersion Listed;
	Listed.Version = 6;
	Listed.ManifestJson = TEXT("{\"root\":\"world\",\"types\":{\"world\":{\"kind\":\"hub\",\"client\":true,\"persist_every_ms\":10000,\"digest\":\"bb\",\"seed_bytes\":16}}}");
	const TArray<CrowdyExecRevisions::FTypeChange> SameSizeChanges = CrowdyExecRevisions::Compare(Kit, {World.Get()}, &Listed, true, NoHistory);
	const CrowdyExecRevisions::FTypeChange* SameSize = Find(SameSizeChanges, TEXT("world"));
	TestTrue(TEXT("a Config of the live seed's size is no change"), SameSize && !SameSize->What.Contains(TEXT("config")));
	World->KitConfig = TEXT("{\"day_ms\": 50000}");
	const TArray<CrowdyExecRevisions::FTypeChange> OtherSizeChanges = CrowdyExecRevisions::Compare(Kit, {World.Get()}, &Listed, true, NoHistory);
	const CrowdyExecRevisions::FTypeChange* OtherSize = Find(OtherSizeChanges, TEXT("world"));
	TestTrue(TEXT("a Config of another size is a change"), OtherSize && OtherSize->What.Contains(TEXT("config")));
	World->KitConfig = TEXT("{\"day_ms\": 5000}");

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Arena = MakeOwnType(TEXT("arena"));
	CrowdyExecDeploy::FTypeState& ArenaState = Kit.Types.AddDefaulted_GetRef();
	ArenaState.TypeName = TEXT("arena");
	ArenaState.State = CrowdyExecDeploy::ECrateState::UpToDate;
	CrowdyExecDeveloper::FBuildCrate& ArenaCrate = Kit.Crates.AddDefaulted_GetRef();
	ArenaCrate.Name = TEXT("arena");
	CrowdyExecDeveloper::FVersion WithArena;
	WithArena.Version = 6;
	WithArena.ManifestJson = TEXT("{\"root\":\"root\",\"types\":{\"root\":{\"kind\":\"hub\",\"crate\":\"root\",\"client\":true},")
		TEXT("\"arena\":{\"kind\":\"hub\",\"parent\":\"root\",\"crate\":\"arena\",\"client\":true,\"persist_every_ms\":30000,\"evict_after_ms\":300000}}}");
	const TArray<CrowdyExecRevisions::FTypeChange> Switched = CrowdyExecRevisions::Compare(Kit, {World.Get(), Arena.Get()}, &WithArena, true, NoHistory);
	const CrowdyExecRevisions::FTypeChange* Moved = Find(Switched, TEXT("arena"));
	TestTrue(TEXT("an own type that moves from the SDK's root to world shows the move"), Moved && Moved->What.Contains(TEXT("parent")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitDirectorEventsTest, "CrowdySDK.CrowdyExecEditor.GameKit.DirectorEventsAreOneMessage", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitDirectorEventsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	TestOneEventMessage(*this, TEXT("director"), TEXT("encounter"), FCrowdyKitDirectorEvent::StaticStruct(), StaticEnum<ECrowdyKitDirectorEventType>(),
		{TEXT("wave_started"), TEXT("wave_cleared"), TEXT("boss_phase"), TEXT("won"), TEXT("lost")});
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Director = MakeStarter(*this, TEXT("director"));
	if (!Director)
	{
		return false;
	}
	const FCrowdyExecBakedStruct* Event = FindBakedStruct(*Director, FCrowdyKitDirectorEvent::StaticStruct());
	TestTrue(TEXT("an event carries the wave it started or cleared and the boss phase it entered"), HasField(Event, TEXT("wave")) && HasField(Event, TEXT("phase")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitTerritoryZoneEventsTest, "CrowdySDK.CrowdyExecEditor.GameKit.TerritoryAndZoneEventsAreOneMessage", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitTerritoryZoneEventsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	TestOneEventMessage(*this, TEXT("territory"), TEXT("territory"), FCrowdyKitTerritoryEvent::StaticStruct(), StaticEnum<ECrowdyKitTerritoryEventType>(),
		{TEXT("captured"), TEXT("income"), TEXT("won")});
	TestOneEventMessage(*this, TEXT("zone"), TEXT("zone"), FCrowdyKitZoneEvent::StaticStruct(), StaticEnum<ECrowdyKitZoneEventType>(),
		{TEXT("warning"), TEXT("shrinking"), TEXT("settled"), TEXT("final"), TEXT("eliminated"), TEXT("won")});
	TestOneEventMessage(*this, TEXT("arena"), TEXT("arena"), FCrowdyKitArenaEvent::StaticStruct(), StaticEnum<ECrowdyKitArenaEventType>(),
		{TEXT("cast"), TEXT("impact"), TEXT("kill"), TEXT("respawn"), TEXT("violation"), TEXT("removed"), TEXT("finished")});
	TestOneEventMessage(*this, TEXT("race"), TEXT("race"), FCrowdyKitRaceEvent::StaticStruct(), StaticEnum<ECrowdyKitRaceEventType>(),
		{TEXT("go"), TEXT("timing"), TEXT("violation"), TEXT("retired"), TEXT("finished")});
	TestOneEventMessage(*this, TEXT("ball"), TEXT("ball"), FCrowdyKitBallEvent::StaticStruct(), StaticEnum<ECrowdyKitBallEventType>(),
		{TEXT("claimed"), TEXT("passed"), TEXT("shot"), TEXT("goal"), TEXT("violation"), TEXT("left"), TEXT("removed"), TEXT("finished")});
	for (const TCHAR* Quiet : {TEXT("minigame"), TEXT("tactics"), TEXT("instance"), TEXT("deck")})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, Quiet);
		TestTrue(FString::Printf(TEXT("%s sends no event: its table is only its snapshots"), Quiet), Definition && Definition->Messages.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitReadTheWorldTest, "CrowdySDK.CrowdyExecEditor.GameKit.ZoneAndTerritoryReadTheWorld", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitReadTheWorldTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	for (const TCHAR* Type : {TEXT("territory"), TEXT("zone")})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, Type);
		FString Lib;
		if (!Definition || !TestTrue(FString::Printf(TEXT("%s's Rust is readable"), Type), LoadLib(Type, Lib)))
		{
			return false;
		}
		TestTrue(FString::Printf(TEXT("%s reads where players are"), Type), Definition->bReadsWorld);
		TestTrue(FString::Printf(TEXT("%s draws no Kit Actors: its realtime events are not mapped"), Type), Definition->KitActors.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s is z up: its Rust keeps height in z"), Type), Lib.Contains(TEXT("pub z:"), ESearchCase::CaseSensitive));
	}
	for (const TCHAR* Type : {TEXT("minigame"), TEXT("tactics"), TEXT("director"), TEXT("instance"), TEXT("deck"), TEXT("arena"), TEXT("race"), TEXT("ball")})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, Type);
		TestTrue(FString::Printf(TEXT("%s reads no world data"), Type), Definition && !Definition->bReadsWorld);
	}

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Zone = MakeStarter(*this, TEXT("zone"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Territory = MakeStarter(*this, TEXT("territory"));
	if (!Zone || !Territory)
	{
		return false;
	}
	const FCrowdyExecBakedStruct* Center = FindBakedStruct(*Zone, FCrowdyKitZoneCenter::StaticStruct());
	TestTrue(TEXT("a zone's centre is read from its [x, y] on the ground, in that order"), Center && Center->Fields.Num() == 2
		&& Center->Fields[0].ServerName == TEXT("x") && Center->Fields[1].ServerName == TEXT("y"));
	const FCrowdyExecBakedStruct* Zoned = FindBakedStruct(*Zone, FCrowdyKitZoneState::StaticStruct());
	TestTrue(TEXT("and its state names what its Rust sends"), HasField(Zoned, TEXT("center")) && HasField(Zoned, TEXT("radius")) && HasField(Zoned, TEXT("next_radius")) && HasField(Zoned, TEXT("alive")));
	const FCrowdyExecBakedStruct* Point = FindBakedStruct(*Territory, FCrowdyKitTerritoryPoint::StaticStruct());
	TestTrue(TEXT("a territory's points carry no place: that is in its Config"), HasField(Point, TEXT("owner")) && HasField(Point, TEXT("capturing")) && HasField(Point, TEXT("present"))
		&& !HasField(Point, TEXT("x")) && !HasField(Point, TEXT("z")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitInstanceBoardTest, "CrowdySDK.CrowdyExecEditor.GameKit.InstanceMayPostToALeaderboard", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitInstanceBoardTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const CrowdyGameKitStarters::FStarter* Starter = CrowdyGameKitStarters::FindStarter(TEXT("instance"));
	if (!TestNotNull(TEXT("the SDK has an instance starter"), Starter))
	{
		return false;
	}
	TestTrue(TEXT("an instance may call the leaderboard, and calls nothing else"), Starter->Calls.IsEmpty() && Starter->OptionalCalls.Num() == 1
		&& FString(Starter->OptionalCalls[0]).Equals(TEXT("leaderboard"), ESearchCase::CaseSensitive));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Instance = MakeStarter(*this, TEXT("instance"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Leaderboard = MakeStarter(*this, TEXT("leaderboard"));
	if (!World || !Instance || !Leaderboard)
	{
		return false;
	}
	TestEqualSensitive(TEXT("an instance without a leaderboard calls nothing"), CallsOf(ManifestType(CrowdyExecDeploy::MakeManifest({World.Get(), Instance.Get()}), TEXT("instance"))), FString());
	const FString WithBoard = CrowdyExecDeploy::MakeManifest({World.Get(), Instance.Get(), Leaderboard.Get()});
	TestEqualSensitive(TEXT("with one it calls the leaderboard, to post its depths"), CallsOf(ManifestType(WithBoard, TEXT("instance"))), FString(TEXT("leaderboard")));
	TestEqualSensitive(TEXT("and runs under world"), FieldOf(ManifestType(WithBoard, TEXT("instance")), TEXT("parent")), FString(TEXT("world")));

	auto Names = [](const TArray<const CrowdyGameKitStarters::FStarter*>& Plan)
	{
		TArray<FString> Out;
		for (const CrowdyGameKitStarters::FStarter* Each : Plan)
		{
			Out.Add(Each->TypeName);
		}
		return FString::Join(Out, TEXT(","));
	};
	auto Removed = [](const TArray<UCrowdyServerObjectDefinition*>& Plan)
	{
		TArray<FString> Out;
		for (const UCrowdyServerObjectDefinition* Each : Plan)
		{
			Out.Add(Each->TypeName);
		}
		return FString::Join(Out, TEXT(","));
	};
	FString Problem;
	TestEqualSensitive(TEXT("adding an instance does not bring a leaderboard"), Names(CrowdyGameKitStarters::PlanAdd(TEXT("instance"), {}, Problem)), FString(TEXT("world,instance")));
	const TArray<UCrowdyServerObjectDefinition*> Project = {World.Get(), Instance.Get(), Leaderboard.Get()};
	TestEqualSensitive(TEXT("removing the leaderboard leaves the instance that may post to it"), Removed(CrowdyGameKitStarters::PlanRemove(TEXT("leaderboard"), Project, Problem)), FString(TEXT("leaderboard")));
	TestEqualSensitive(TEXT("and removing the instance leaves the leaderboard"), Removed(CrowdyGameKitStarters::PlanRemove(TEXT("instance"), Project, Problem)), FString(TEXT("instance")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitTerritoryResetTest, "CrowdySDK.CrowdyExecEditor.GameKit.TerritoryResetIsForDevelopers", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitTerritoryResetTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Territory = MakeStarter(*this, TEXT("territory"));
	const FCrowdyServerFunction* Reset = Territory ? Territory->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("reset"); }) : nullptr;
	if (!TestNotNull(TEXT("the territory starter has reset"), Reset))
	{
		return false;
	}
	TestTrue(TEXT("a territory's reset, a new war, is the developers' only"), Reset->WhoCanCall == ECrowdyServerFunctionCaller::DevelopersOnly && CrowdyGameKitStarters::IsDeveloperOnly(*Territory, *Reset));
	// The first to join starts a zone or an encounter, and a player abandons their own run, so those are the players' too.
	for (const TCHAR* Type : {TEXT("minigame"), TEXT("tactics"), TEXT("director"), TEXT("instance"), TEXT("territory"), TEXT("zone"), TEXT("deck"), TEXT("arena"), TEXT("race"), TEXT("ball")})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, Type);
		if (!Definition)
		{
			return false;
		}
		for (const FCrowdyServerFunction& Function : Definition->Functions)
		{
			TestEqual(FString::Printf(TEXT("%s's %s is the developers' only exactly when it is the territory's reset"), Type, *Function.Name.ToString()),
				CrowdyGameKitStarters::IsDeveloperOnly(*Definition, Function), FString(Type) == TEXT("territory") && Function.GetMethodName() == TEXT("reset"));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitPresenceFlagTest, "CrowdySDK.CrowdyExecEditor.GameKit.OptionalIndexesHaveAPresenceFlag", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitPresenceFlagTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	struct FPair
	{
		const TCHAR* Type;
		UScriptStruct* State;
		const TCHAR* Value;
		const TCHAR* Flag;
		const TCHAR* ValueOnTheWire;
		const TCHAR* FlagOnTheWire;
	};
	const FPair Pairs[] = {
		{TEXT("director"), FCrowdyKitDirectorState::StaticStruct(), TEXT("Wave"), TEXT("bHasWave"), TEXT("wave"), TEXT("has_wave")},
		{TEXT("zone"), FCrowdyKitZoneState::StaticStruct(), TEXT("Stage"), TEXT("bHasStage"), TEXT("stage"), TEXT("has_stage")},
		{TEXT("deck"), FCrowdyKitDeckState::StaticStruct(), TEXT("Suit"), TEXT("bHasSuit"), TEXT("suit"), TEXT("has_suit")},
		{TEXT("world"), FCrowdyKitWorldConditions::StaticStruct(), TEXT("Online"), TEXT("bHasOnline"), TEXT("online"), TEXT("has_online")},
		{TEXT("director"), FCrowdyKitDirectorEvent::StaticStruct(), TEXT("Type"), TEXT("bHasType"), TEXT("type"), TEXT("has_type")},
		{TEXT("territory"), FCrowdyKitTerritoryEvent::StaticStruct(), TEXT("Type"), TEXT("bHasType"), TEXT("type"), TEXT("has_type")},
		{TEXT("zone"), FCrowdyKitZoneEvent::StaticStruct(), TEXT("Type"), TEXT("bHasType"), TEXT("type"), TEXT("has_type")},
		{TEXT("arena"), FCrowdyKitArenaEvent::StaticStruct(), TEXT("Type"), TEXT("bHasType"), TEXT("type"), TEXT("has_type")},
		{TEXT("race"), FCrowdyKitRaceEvent::StaticStruct(), TEXT("Type"), TEXT("bHasType"), TEXT("type"), TEXT("has_type")},
		{TEXT("race"), FCrowdyKitRaceEvent::StaticStruct(), TEXT("Stage"), TEXT("bHasStage"), TEXT("stage"), TEXT("has_stage")},
		{TEXT("ball"), FCrowdyKitBallEvent::StaticStruct(), TEXT("Type"), TEXT("bHasType"), TEXT("type"), TEXT("has_type")},
		{TEXT("ball"), FCrowdyKitBallEvent::StaticStruct(), TEXT("Team"), TEXT("bHasTeam"), TEXT("team"), TEXT("has_team")},
		{TEXT("ball"), FCrowdyKitBallEvent::StaticStruct(), TEXT("Winner"), TEXT("bHasWinner"), TEXT("winner"), TEXT("has_winner")},
		{TEXT("ball"), FCrowdyKitBallState::StaticStruct(), TEXT("Winner"), TEXT("bHasWinner"), TEXT("winner"), TEXT("has_winner")},
	};
	for (const FPair& Pair : Pairs)
	{
		const FProperty* Value = Pair.State->FindPropertyByName(Pair.Value);
		const FBoolProperty* Flag = CastField<FBoolProperty>(Pair.State->FindPropertyByName(Pair.Flag));
		TestTrue(FString::Printf(TEXT("%s's %s has a presence flag %s declared right after it"), Pair.Type, Pair.Value, Pair.Flag), Value && Flag && Value->Next == Flag);
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, Pair.Type);
		if (!Definition)
		{
			return false;
		}
		const FCrowdyExecBakedStruct* Baked = FindBakedStruct(*Definition, Pair.State);
		TestTrue(FString::Printf(TEXT("%s's %s is a wire field"), Pair.Type, Pair.Value), HasField(Baked, Pair.ValueOnTheWire));
		TestFalse(FString::Printf(TEXT("and %s's flag is not"), Pair.Type), HasField(Baked, Pair.FlagOnTheWire));
	}
	TestFalse(TEXT("a state starts with no wave"), FCrowdyKitDirectorState().bHasWave);
	TestFalse(TEXT("and no stage"), FCrowdyKitZoneState().bHasStage);
	TestFalse(TEXT("a world starts with no player count"), FCrowdyKitWorldConditions().bHasOnline);
	TestFalse(TEXT("an event starts with no known type"), FCrowdyKitDirectorEvent().bHasType || FCrowdyKitTerritoryEvent().bHasType || FCrowdyKitZoneEvent().bHasType || FCrowdyKitArenaEvent().bHasType
		|| FCrowdyKitRaceEvent().bHasType || FCrowdyKitBallEvent().bHasType);
	TestFalse(TEXT("a timing starts with no known stage, and a ball game with no winner"), FCrowdyKitRaceEvent().bHasStage || FCrowdyKitBallState().bHasWinner || FCrowdyKitBallEvent().bHasWinner);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitJsonInputsSetPresenceTest, "CrowdySDK.CrowdyExecEditor.GameKit.JsonInputsSetThePresenceFlagOfWhatTheyName", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitJsonInputsSetPresenceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Instance = MakeStarter(*this, TEXT("instance"));
	const FCrowdyServerFunction* Open = Instance ? Instance->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("open"); }) : nullptr;
	if (!TestNotNull(TEXT("the instance starter has open"), Open))
	{
		return false;
	}
	FString Error;
	FInstancedStruct Named;
	CrowdyExecDeveloperCall::InputsFromJson(*Open, TEXT("{\"Seed\":42}"), Named, Error, Instance.Get());
	TestTrue(TEXT("a seed the JSON names is sent"), Named.IsValid() && Named.Get<FCrowdyKitInstanceOpenInputs>().Seed == 42 && Named.Get<FCrowdyKitInstanceOpenInputs>().bHasSeed);
	FInstancedStruct Left;
	CrowdyExecDeveloperCall::InputsFromJson(*Open, TEXT("{}"), Left, Error, Instance.Get());
	TestTrue(TEXT("and one it leaves out is not"), Left.IsValid() && !Left.Get<FCrowdyKitInstanceOpenInputs>().bHasSeed);
	FInstancedStruct Flagged;
	CrowdyExecDeveloperCall::InputsFromJson(*Open, TEXT("{\"Seed\":42,\"bHasSeed\":false}"), Flagged, Error, Instance.Get());
	TestTrue(TEXT("and a flag the JSON names itself is left as written"), Flagged.IsValid() && !Flagged.Get<FCrowdyKitInstanceOpenInputs>().bHasSeed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitGenreStartersTest, "CrowdySDK.CrowdyExecEditor.GameKit.GenreStartersAreFreeKeyedUnderWorld", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitGenreStartersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	if (!World)
	{
		return false;
	}
	for (const TCHAR* Type : {TEXT("minigame"), TEXT("tactics"), TEXT("director"), TEXT("instance"), TEXT("territory"), TEXT("zone"), TEXT("deck"), TEXT("arena"), TEXT("race"), TEXT("ball")})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, Type);
		if (!Definition)
		{
			return false;
		}
		TestEqualSensitive(FString::Printf(TEXT("%s runs under world"), Type), Definition->KitParent, FString(TEXT("world")));
		TestTrue(FString::Printf(TEXT("%s is keyed by a free Instance Id, not by a player"), Type), !Definition->IsKeyedByPlayer() && !Definition->bOnlyOneInstance);
		TestTrue(FString::Printf(TEXT("%s keeps no records, no Kit Actors but a ball game's ball, and no Owner Values but a deck's hand: its table is its State"), Type),
			Definition->KitCollectionTopic.IsEmpty() && !Definition->KitRecord && Definition->KitOwnerValues.Num() == (FString(Type) == TEXT("deck") ? 1 : 0)
			&& Definition->KitActors.Num() == (FString(Type) == TEXT("ball") ? 1 : 0));
		TestTrue(FString::Printf(TEXT("%s has a State and a GetState that answers it"), Type), Definition->State
			&& Definition->Functions.ContainsByPredicate([&Definition](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("state") && Each.Reply == Definition->State; }));

		FString Problem;
		TArray<FString> Plan;
		for (const CrowdyGameKitStarters::FStarter* Each : CrowdyGameKitStarters::PlanAdd(Type, {}, Problem))
		{
			Plan.Add(Each->TypeName);
		}
		TestEqualSensitive(FString::Printf(TEXT("adding %s brings world and nothing else"), Type), FString::Join(Plan, TEXT(",")), FString(TEXT("world,")) + Type);

		const FString Manifest = CrowdyExecDeploy::MakeManifest({World.Get(), Definition.Get()});
		const TSharedPtr<FJsonObject> InManifest = ManifestType(Manifest, Type);
		TestEqualSensitive(FString::Printf(TEXT("%s runs under world in the manifest"), Type), FieldOf(InManifest, TEXT("parent")), FString(TEXT("world")));
		TestEqualSensitive(FString::Printf(TEXT("%s's crate is its type"), Type), FieldOf(InManifest, TEXT("crate")), FString(Type));
		TestEqualSensitive(FString::Printf(TEXT("%s calls nothing while the project has no leaderboard"), Type), CallsOf(InManifest), FString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitGenreRepliesTest, "CrowdySDK.CrowdyExecEditor.GameKit.GenreRepliesAreOneFieldStructsOrNone", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitGenreRepliesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	struct FReply
	{
		const TCHAR* Type;
		const TCHAR* Method;
		const TCHAR* Field;
	};
	// A bare scalar answer fills the one field of its reply struct, which no hub sends by name.
	const FReply Scalars[] = {
		{TEXT("minigame"), TEXT("join"), TEXT("started")}, {TEXT("minigame"), TEXT("throw"), TEXT("revealed")},
		{TEXT("tactics"), TEXT("attack"), TEXT("hp")},
		{TEXT("director"), TEXT("build"), TEXT("gold")}, {TEXT("director"), TEXT("hit"), TEXT("hp")},
		{TEXT("instance"), TEXT("open"), TEXT("seed")}, {TEXT("instance"), TEXT("enter"), TEXT("line")}, {TEXT("instance"), TEXT("descend"), TEXT("floor")},
		{TEXT("deck"), TEXT("play"), TEXT("playing")},
		{TEXT("arena"), TEXT("cast"), TEXT("hit")},
	};
	for (const FReply& Scalar : Scalars)
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, Scalar.Type);
		const FCrowdyServerFunction* Function = Definition ? Definition->Functions.FindByPredicate([&Scalar](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == Scalar.Method; }) : nullptr;
		if (!TestNotNull(FString::Printf(TEXT("%s has %s"), Scalar.Type, Scalar.Method), Function))
		{
			continue;
		}
		const FCrowdyExecBakedStruct* Reply = FindBakedStruct(*Definition, Function->GetReplyStruct());
		TestTrue(FString::Printf(TEXT("%s's %s answers one field, %s"), Scalar.Type, Scalar.Method, Scalar.Field),
			Reply && Reply->Fields.Num() == 1 && Reply->Fields[0].ServerName.Equals(Scalar.Field, ESearchCase::CaseSensitive));
	}

	// An answer that is always a bare true carries nothing.
	const TCHAR* const Nothing[] = {TEXT("minigame:leave"), TEXT("minigame:rematch"), TEXT("tactics:join"), TEXT("tactics:start"), TEXT("tactics:move"), TEXT("tactics:end_turn"),
		TEXT("director:join"), TEXT("director:start"), TEXT("instance:invite"), TEXT("instance:join"), TEXT("instance:abandon"),
		TEXT("territory:join"), TEXT("territory:reset"), TEXT("zone:join"), TEXT("zone:start"),
			TEXT("deck:join"), TEXT("deck:leave"), TEXT("deck:start"), TEXT("deck:pass"), TEXT("arena:join"), TEXT("arena:leave"), TEXT("arena:start"),
			TEXT("race:join"), TEXT("race:leave"), TEXT("race:start"), TEXT("ball:join"), TEXT("ball:leave"), TEXT("ball:start"), TEXT("ball:claim"), TEXT("ball:pass"), TEXT("ball:shoot")};
	for (const TCHAR* Pair : Nothing)
	{
		FString Type;
		FString Method;
		FString(Pair).Split(TEXT(":"), &Type, &Method);
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, *Type);
		const FCrowdyServerFunction* Function = Definition ? Definition->Functions.FindByPredicate([&Method](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == Method; }) : nullptr;
		if (TestNotNull(FString::Printf(TEXT("%s has %s"), *Type, *Method), Function))
		{
			TestNull(FString::Printf(TEXT("%s's %s answers nothing"), *Type, *Method), Function->GetReplyStruct());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitGenreTuplesTest, "CrowdySDK.CrowdyExecEditor.GameKit.BoardCellsAreFieldOrderTuples", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitGenreTuplesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	// A hub sends a cell as [x, z], which is read into the struct's fields in the order they are declared.
	for (const TCHAR* Type : {TEXT("tactics"), TEXT("director")})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeStarter(*this, Type);
		if (!Definition)
		{
			return false;
		}
		const FCrowdyExecBakedStruct* Cell = FindBakedStruct(*Definition, FCrowdyKitGridCell::StaticStruct());
		TestTrue(FString::Printf(TEXT("%s's board cell is read as x then z"), Type), Cell && Cell->Fields.Num() == 2
			&& Cell->Fields[0].ServerName == TEXT("x") && Cell->Fields[1].ServerName == TEXT("z"));
	}
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Tactics = MakeStarter(*this, TEXT("tactics"));
	const FCrowdyExecBakedStruct* Battle = Tactics ? FindBakedStruct(*Tactics, FCrowdyKitTacticsState::StaticStruct()) : nullptr;
	TestTrue(TEXT("a battle's obstacles are its cells"), HasField(Battle, TEXT("obstacles")) && HasField(Battle, TEXT("ended_turn")) && HasField(Battle, TEXT("phase_ends")));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Director = MakeStarter(*this, TEXT("director"));
	const FCrowdyExecBakedStruct* Encounter = Director ? FindBakedStruct(*Director, FCrowdyKitDirectorState::StaticStruct()) : nullptr;
	TestTrue(TEXT("and an encounter's spawn and base are cells"), HasField(Encounter, TEXT("spawn")) && HasField(Encounter, TEXT("base")) && HasField(Encounter, TEXT("next_wave_at")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitShuffleSecretTest, "CrowdySDK.CrowdyExecEditor.GameKit.AddStarterGivesShufflesASecret", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitShuffleSecretTest::RunTest(const FString& Parameters)
{
	const auto SecretOf = [](const FString& Config)
	{
		TSharedPtr<FJsonObject> Object;
		FString Secret;
		const bool bRead = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Config), Object) && Object.IsValid() && Object->TryGetStringField(TEXT("secret"), Secret);
		return bRead && Object->Values.Num() == 1 ? Secret : FString();
	};
	for (const TCHAR* Type : {TEXT("deck"), TEXT("player")})
	{
		const CrowdyGameKitStarters::FStarter* Starter = CrowdyGameKitStarters::FindStarter(Type);
		if (!TestNotNull(FString::Printf(TEXT("the SDK has %s"), Type), Starter))
		{
			return false;
		}
		const FString First = SecretOf(CrowdyGameKitStarters::ConfigToAdd(*Starter, TEXT("  ")));
		const FString Second = SecretOf(CrowdyGameKitStarters::ConfigToAdd(*Starter, FString()));
		TestEqual(FString::Printf(TEXT("%s given no Config gets a 32-digit secret"), Type), First.Len(), 32);
		TestNotEqual(FString::Printf(TEXT("%s gets a fresh one each time"), Type), First, Second);
		TestEqualSensitive(FString::Printf(TEXT("%s keeps the Config it is given"), Type), CrowdyGameKitStarters::ConfigToAdd(*Starter, TEXT(" {\"secret\": \"mine\"} ")), FString(TEXT("{\"secret\": \"mine\"}")));
	}
	const CrowdyGameKitStarters::FStarter* World = CrowdyGameKitStarters::FindStarter(TEXT("world"));
	TestTrue(TEXT("a starter that shuffles nothing keeps an empty Config"), World && CrowdyGameKitStarters::ConfigToAdd(*World, FString()).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitDeckCardsTest, "CrowdySDK.CrowdyExecEditor.GameKit.DeckCardsAreStructsOnTheHubsFaceKeys", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitDeckCardsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Deck = MakeStarter(*this, TEXT("deck"));
	FString Lib;
	if (!Deck || !TestTrue(TEXT("deck's Rust is readable"), LoadLib(TEXT("deck"), Lib)))
	{
		return false;
	}
	TArray<FString> Errors;
	const bool bBaked = Deck->Bake(Errors);
	TestTrue(FString::Printf(TEXT("deck bakes again with no errors (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked && Errors.IsEmpty());
	TestEqualSensitive(TEXT("deck publishes its table on the topic its Rust does"), Deck->KitStateTopic, FString(TEXT("table")));
	TestTrue(TEXT("deck's one Owner Value is the signed-in player's hand"), Deck->KitOwnerValues.Num() == 1 && Deck->KitOwnerValues[0].Method == TEXT("hand")
		&& Deck->KitOwnerValues[0].What == TEXT("hand") && Deck->KitOwnerValues[0].Variable == FName(TEXT("Hand")));

	const FCrowdyExecBakedStruct* Hand = FindBakedStruct(*Deck, FCrowdyKitDeckHand::StaticStruct());
	const FCrowdyExecBakedStruct* Table = FindBakedStruct(*Deck, FCrowdyKitDeckState::StaticStruct());
	const FCrowdyExecBakedStruct* Played = FindBakedStruct(*Deck, FCrowdyKitDeckPlayed::StaticStruct());
	TestTrue(TEXT("a hand reads its cards from faces and its playable cards from playable_faces"), HasField(Hand, TEXT("faces")) && HasField(Hand, TEXT("playable_faces"))
		&& HasField(Hand, TEXT("drawn")) && !HasField(Hand, TEXT("cards")) && !HasField(Hand, TEXT("playable")));
	TestTrue(TEXT("the table reads its top card from top_face, and its suit with a presence flag"), HasField(Table, TEXT("top_face")) && !HasField(Table, TEXT("top"))
		&& HasField(Table, TEXT("suit")) && !HasField(Table, TEXT("has_suit")) && HasField(Table, TEXT("hand_counts")) && HasField(Table, TEXT("hand")));
	TestTrue(TEXT("the last move reads its card from face"), HasField(Played, TEXT("face")) && !HasField(Played, TEXT("card")) && HasField(Played, TEXT("action")));
	TestTrue(TEXT("suits are the letters the hub sends"), EnumNamesAre(*Deck, StaticEnum<ECrowdyKitCardSuit>(), {TEXT("S"), TEXT("H"), TEXT("D"), TEXT("C")}));
	TestTrue(TEXT("phases are the names its Rust sends"), EnumNamesAre(*Deck, StaticEnum<ECrowdyKitDeckPhase>(), {TEXT("lobby"), TEXT("playing"), TEXT("finished")}));
	TestTrue(TEXT("actions are the names its Rust sends"), EnumNamesAre(*Deck, StaticEnum<ECrowdyKitDeckAction>(), {TEXT("play"), TEXT("draw"), TEXT("pass"), TEXT("timeout")}));

	for (const TCHAR* Name : {TEXT("faces"), TEXT("playable_faces"), TEXT("top_face"), TEXT("face")})
	{
		TestTrue(FString::Printf(TEXT("deck's Rust has the key %s"), Name), Lib.Contains(Name, ESearchCase::CaseSensitive));
	}
	for (const TCHAR* Method : {TEXT("join"), TEXT("leave"), TEXT("start"), TEXT("play"), TEXT("draw"), TEXT("pass"), TEXT("hand"), TEXT("state")})
	{
		TestTrue(FString::Printf(TEXT("deck's Rust answers %s"), Method), Lib.Contains(FString::Printf(TEXT("\"%s\""), Method), ESearchCase::CaseSensitive)
			&& Deck->Functions.ContainsByPredicate([Method](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == Method; }));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitArenaReportsPositionTest, "CrowdySDK.CrowdyExecEditor.GameKit.ArenaReportsPositionThroughMove", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitArenaReportsPositionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Arena = MakeStarter(*this, TEXT("arena"));
	FString Lib;
	if (!Arena || !TestTrue(TEXT("arena's Rust is readable"), LoadLib(TEXT("arena"), Lib)))
	{
		return false;
	}
	TArray<FString> Errors;
	const bool bBaked = Arena->Bake(Errors);
	TestTrue(FString::Printf(TEXT("arena bakes again with no errors (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked && Errors.IsEmpty());
	TestEqualSensitive(TEXT("arena publishes its fight on the topic its Rust does"), Arena->KitStateTopic, FString(TEXT("arena")));
	TestTrue(TEXT("arena sends its events as the Arena Event message"), Arena->Messages.Num() == 1 && Arena->Messages[0].Name == FName(TEXT("ArenaEvent")));

	const FCrowdyServerFunction* Move = Arena->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("move"); });
	if (!TestNotNull(TEXT("arena has move"), Move))
	{
		return false;
	}
	TestFalse(TEXT("move is not sent once: a report that is lost is followed by the next"), Move->bSendOnce);
	TestTrue(TEXT("and every other call is"), Algo::AllOf(Arena->Functions, [Move](const FCrowdyServerFunction& Each) { return &Each == Move || Each.GetMethodName() == TEXT("state") || Each.bSendOnce; }));
	TestTrue(TEXT("move answers the kept position as a ground position"), Move->GetReplyStruct() == FCrowdyKitGroundPosition::StaticStruct()
		&& Move->GetParamsStruct() == FCrowdyKitArenaMoveInputs::StaticStruct());
	TestTrue(TEXT("arena reports its player's place through Move"), Arena->KitPositionFunction == FName(TEXT("Move")));
	TestTrue(TEXT("five times a second"), Arena->KitPositionRate == 5.f);
	TestTrue(TEXT("so it reports a position"), Arena->ReportsPosition());
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Deck = MakeStarter(*this, TEXT("deck"));
	TestTrue(TEXT("and a starter that tracks no players does not"), Deck && Deck->KitPositionFunction.IsNone() && !Deck->ReportsPosition());

	const FCrowdyExecBakedStruct* Fighter = FindBakedStruct(*Arena, FCrowdyKitArenaFighter::StaticStruct());
	const FCrowdyExecBakedStruct* Join = FindBakedStruct(*Arena, FCrowdyKitArenaJoinInputs::StaticStruct());
	const FCrowdyExecBakedStruct* Ground = FindBakedStruct(*Arena, FCrowdyKitGroundPosition::StaticStruct());
	const FCrowdyExecBakedStruct* State = FindBakedStruct(*Arena, FCrowdyKitArenaState::StaticStruct());
	TestTrue(TEXT("a fighter's hero class is the hub's class"), HasField(Fighter, TEXT("class")) && !HasField(Fighter, TEXT("hero_class")) && HasField(Fighter, TEXT("max_hp")) && HasField(Fighter, TEXT("down")));
	TestTrue(TEXT("and so is the one a join sends"), HasField(Join, TEXT("class")) && !HasField(Join, TEXT("hero_class")));
	TestTrue(TEXT("a position is read from its [x, y] in that order"), Ground && Ground->Fields.Num() == 2 && Ground->Fields[0].ServerName == TEXT("x") && Ground->Fields[1].ServerName == TEXT("y"));
	TestTrue(TEXT("a fight names its corner, size, shots and areas"), HasField(State, TEXT("origin")) && HasField(State, TEXT("width")) && HasField(State, TEXT("depth"))
		&& HasField(State, TEXT("projectiles")) && HasField(State, TEXT("areas")) && HasField(State, TEXT("kill_target")) && HasField(State, TEXT("ends_at")) && HasField(State, TEXT("winner")));
	TestTrue(TEXT("phases are the names its Rust sends"), EnumNamesAre(*Arena, StaticEnum<ECrowdyKitArenaPhase>(), {TEXT("waiting"), TEXT("running"), TEXT("finished")}));

	for (const TCHAR* Method : {TEXT("move"), TEXT("cast")})
	{
		TestTrue(FString::Printf(TEXT("arena's Rust answers %s"), Method), Lib.Contains(FString::Printf(TEXT("\"%s\""), Method), ESearchCase::CaseSensitive));
	}
	for (const TCHAR* Key : {TEXT("class"), TEXT("origin"), TEXT("idle_ms")})
	{
		TestTrue(FString::Printf(TEXT("arena's Rust has the key %s"), Key), Lib.Contains(Key, ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitRaceReportsPositionTest, "CrowdySDK.CrowdyExecEditor.GameKit.RaceReportsPositionThroughMove", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitRaceReportsPositionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Race = MakeStarter(*this, TEXT("race"));
	FString Lib;
	if (!Race || !TestTrue(TEXT("race's Rust is readable"), LoadLib(TEXT("race"), Lib)))
	{
		return false;
	}
	TArray<FString> Errors;
	const bool bBaked = Race->Bake(Errors);
	TestTrue(FString::Printf(TEXT("race bakes again with no errors (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked && Errors.IsEmpty());
	TestEqualSensitive(TEXT("race publishes its course on the topic its Rust does"), Race->KitStateTopic, FString(TEXT("race")));
	TestTrue(TEXT("race sends its events as the Race Event message"), Race->Messages.Num() == 1 && Race->Messages[0].Name == FName(TEXT("RaceEvent")));

	const FCrowdyServerFunction* Move = Race->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("move"); });
	if (!TestNotNull(TEXT("race has move"), Move))
	{
		return false;
	}
	TestFalse(TEXT("move is not sent once: a report that is lost is followed by the next"), Move->bSendOnce);
	TestTrue(TEXT("and every other call is, but the two that read"), Algo::AllOf(Race->Functions, [Move](const FCrowdyServerFunction& Each)
	{
		return &Each == Move || Each.GetMethodName() == TEXT("state") || Each.GetMethodName() == TEXT("ghost") || Each.bSendOnce;
	}));
	TestTrue(TEXT("move takes and answers a ground position"), Move->GetReplyStruct() == FCrowdyKitGroundPosition::StaticStruct()
		&& Move->GetParamsStruct() == FCrowdyKitGroundPosition::StaticStruct());
	TestTrue(TEXT("race reports its player's place through Move, five times a second"), Race->KitPositionFunction == FName(TEXT("Move")) && Race->KitPositionRate == 5.f);
	TestTrue(TEXT("so it reports a position"), Race->ReportsPosition());
	TestTrue(TEXT("and draws no Kit Actors"), Race->KitActors.IsEmpty());
	const FCrowdyServerFunction* Ghost = Race->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("ghost"); });
	TestTrue(TEXT("the winner's run is a call that reads"), Ghost && !Ghost->bSendOnce && Ghost->GetReplyStruct() == FCrowdyKitRaceGhost::StaticStruct() && !Ghost->GetParamsStruct());

	const FCrowdyExecBakedStruct* State = FindBakedStruct(*Race, FCrowdyKitRaceState::StaticStruct());
	const FCrowdyExecBakedStruct* Racer = FindBakedStruct(*Race, FCrowdyKitRacer::StaticStruct());
	const FCrowdyExecBakedStruct* Gate = FindBakedStruct(*Race, FCrowdyKitRaceGate::StaticStruct());
	const FCrowdyExecBakedStruct* Event = FindBakedStruct(*Race, FCrowdyKitRaceEvent::StaticStruct());
	const FCrowdyExecBakedStruct* Frame = FindBakedStruct(*Race, FCrowdyKitRaceGhostFrame::StaticStruct());
	TestTrue(TEXT("a course names its gates, laps and times"), HasField(State, TEXT("course_id")) && HasField(State, TEXT("gates")) && HasField(State, TEXT("laps"))
		&& HasField(State, TEXT("go_at")) && HasField(State, TEXT("closes_at")) && HasField(State, TEXT("ends_at")) && HasField(State, TEXT("racers")));
	TestTrue(TEXT("a racer names its gate, laps and time as its Rust does"), HasField(Racer, TEXT("next_gate")) && HasField(Racer, TEXT("best_lap_ms")) && HasField(Racer, TEXT("total_ms"))
		&& HasField(Racer, TEXT("did_not_finish")) && !HasField(Racer, TEXT("b_did_not_finish")));
	TestTrue(TEXT("a gate is a place and a radius"), Gate && Gate->Fields.Num() == 3 && HasField(Gate, TEXT("x")) && HasField(Gate, TEXT("y")) && HasField(Gate, TEXT("radius")));
	TestTrue(TEXT("a timing names its stage and elapsed time"), HasField(Event, TEXT("stage")) && HasField(Event, TEXT("elapsed_ms")) && HasField(Event, TEXT("results")) && !HasField(Event, TEXT("has_stage")));
	TestTrue(TEXT("a ghost frame is read at at_ms"), HasField(Frame, TEXT("at_ms")) && HasField(Frame, TEXT("x")) && HasField(Frame, TEXT("y")));
	TestTrue(TEXT("phases are the names its Rust sends"), EnumNamesAre(*Race, StaticEnum<ECrowdyKitRacePhase>(), {TEXT("lobby"), TEXT("countdown"), TEXT("racing"), TEXT("finished")}));
	TestTrue(TEXT("and so are a timing's stages"), EnumNamesAre(*Race, StaticEnum<ECrowdyKitRaceStage>(), {TEXT("started"), TEXT("checkpoint"), TEXT("lap"), TEXT("finished")}));

	for (const TCHAR* Method : {TEXT("join"), TEXT("leave"), TEXT("start"), TEXT("move"), TEXT("ghost"), TEXT("state")})
	{
		TestTrue(FString::Printf(TEXT("race's Rust answers %s"), Method), Lib.Contains(FString::Printf(TEXT("\"%s\""), Method), ESearchCase::CaseSensitive));
	}
	for (const TCHAR* Key : {TEXT("gates"), TEXT("time_limit_ms"), TEXT("idle_ms"), TEXT("did_not_finish"), TEXT("at_ms")})
	{
		TestTrue(FString::Printf(TEXT("race's Rust has the key %s"), Key), Lib.Contains(Key, ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitRaceBoardTest, "CrowdySDK.CrowdyExecEditor.GameKit.RaceMayPostToALeaderboard", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitRaceBoardTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const CrowdyGameKitStarters::FStarter* Starter = CrowdyGameKitStarters::FindStarter(TEXT("race"));
	if (!TestNotNull(TEXT("the SDK has a race starter"), Starter))
	{
		return false;
	}
	TestTrue(TEXT("a race may call the leaderboard, and calls nothing else"), Starter->Calls.IsEmpty() && Starter->OptionalCalls.Num() == 1
		&& FString(Starter->OptionalCalls[0]).Equals(TEXT("leaderboard"), ESearchCase::CaseSensitive));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeStarter(*this, TEXT("world"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Race = MakeStarter(*this, TEXT("race"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Leaderboard = MakeStarter(*this, TEXT("leaderboard"));
	if (!World || !Race || !Leaderboard)
	{
		return false;
	}
	TestEqualSensitive(TEXT("a race without a leaderboard calls nothing"), CallsOf(ManifestType(CrowdyExecDeploy::MakeManifest({World.Get(), Race.Get()}), TEXT("race"))), FString());
	const FString WithBoard = CrowdyExecDeploy::MakeManifest({World.Get(), Race.Get(), Leaderboard.Get()});
	TestEqualSensitive(TEXT("with one it calls the leaderboard, to post its times"), CallsOf(ManifestType(WithBoard, TEXT("race"))), FString(TEXT("leaderboard")));
	TestEqualSensitive(TEXT("and runs under world"), FieldOf(ManifestType(WithBoard, TEXT("race")), TEXT("parent")), FString(TEXT("world")));

	FString Problem;
	const TArray<const CrowdyGameKitStarters::FStarter*> Added = CrowdyGameKitStarters::PlanAdd(TEXT("race"), {}, Problem);
	TestTrue(TEXT("adding a race brings world and no leaderboard"), Added.Num() == 2 && FString(Added[0]->TypeName) == TEXT("world") && FString(Added[1]->TypeName) == TEXT("race"));
	const TArray<UCrowdyServerObjectDefinition*> Project = {World.Get(), Race.Get(), Leaderboard.Get()};
	const TArray<UCrowdyServerObjectDefinition*> WithoutBoard = CrowdyGameKitStarters::PlanRemove(TEXT("leaderboard"), Project, Problem);
	TestTrue(TEXT("removing the leaderboard leaves the race that may post to it"), WithoutBoard.Num() == 1 && WithoutBoard[0]->TypeName == TEXT("leaderboard"));
	const TArray<UCrowdyServerObjectDefinition*> WithoutRace = CrowdyGameKitStarters::PlanRemove(TEXT("race"), Project, Problem);
	TestTrue(TEXT("and removing the race leaves the leaderboard"), WithoutRace.Num() == 1 && WithoutRace[0]->TypeName == TEXT("race"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameKitBallReportsPositionTest, "CrowdySDK.CrowdyExecEditor.GameKit.BallReportsPositionThroughMove", CrowdyGameKitStartersTest::TestFlags)
bool FCrowdyGameKitBallReportsPositionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameKitStartersTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Ball = MakeStarter(*this, TEXT("ball"));
	FString Lib;
	if (!Ball || !TestTrue(TEXT("ball's Rust is readable"), LoadLib(TEXT("ball"), Lib)))
	{
		return false;
	}
	TArray<FString> Errors;
	const bool bBaked = Ball->Bake(Errors);
	TestTrue(FString::Printf(TEXT("ball bakes again with no errors (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked && Errors.IsEmpty());
	TestEqualSensitive(TEXT("ball publishes its game on the topic its Rust does"), Ball->KitStateTopic, FString(TEXT("ball")));
	TestTrue(TEXT("ball sends its events as the Ball Event message"), Ball->Messages.Num() == 1 && Ball->Messages[0].Name == FName(TEXT("BallEvent")));
	TestTrue(TEXT("and draws its ball as a Kit Actor of kind ball, with no class chosen"), Ball->KitActors.Num() == 1 && Ball->KitActors.Contains(TEXT("ball"))
		&& Ball->KitActors.FindRef(TEXT("ball")).IsNull());

	const FCrowdyServerFunction* Move = Ball->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("move"); });
	const FCrowdyServerFunction* Shoot = Ball->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("shoot"); });
	const FCrowdyServerFunction* Pass = Ball->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("pass"); });
	const FCrowdyServerFunction* Join = Ball->Functions.FindByPredicate([](const FCrowdyServerFunction& Each) { return Each.GetMethodName() == TEXT("join"); });
	if (!TestTrue(TEXT("ball has move, shoot, pass and join"), Move && Shoot && Pass && Join))
	{
		return false;
	}
	TestFalse(TEXT("move is not sent once: a report that is lost is followed by the next"), Move->bSendOnce);
	TestTrue(TEXT("and every other call is"), Algo::AllOf(Ball->Functions, [Move](const FCrowdyServerFunction& Each) { return &Each == Move || Each.GetMethodName() == TEXT("state") || Each.bSendOnce; }));
	TestTrue(TEXT("move takes and answers a ground position"), Move->GetReplyStruct() == FCrowdyKitGroundPosition::StaticStruct()
		&& Move->GetParamsStruct() == FCrowdyKitGroundPosition::StaticStruct());
	TestTrue(TEXT("a shot takes a ground position and answers nothing"), Shoot->GetParamsStruct() == FCrowdyKitGroundPosition::StaticStruct() && !Shoot->GetReplyStruct());
	TestTrue(TEXT("a pass takes the teammate and a join the team"), Pass->GetParamsStruct() == FCrowdyKitBallPassInputs::StaticStruct()
		&& Join->GetParamsStruct() == FCrowdyKitBallJoinInputs::StaticStruct());
	TestTrue(TEXT("ball reports its player's place through Move, five times a second"), Ball->KitPositionFunction == FName(TEXT("Move")) && Ball->KitPositionRate == 5.f);
	TestTrue(TEXT("so it reports a position"), Ball->ReportsPosition());

	const FCrowdyExecBakedStruct* State = FindBakedStruct(*Ball, FCrowdyKitBallState::StaticStruct());
	const FCrowdyExecBakedStruct* TheBall = FindBakedStruct(*Ball, FCrowdyKitBall::StaticStruct());
	const FCrowdyExecBakedStruct* Player = FindBakedStruct(*Ball, FCrowdyKitBallPlayer::StaticStruct());
	const FCrowdyExecBakedStruct* Ground = FindBakedStruct(*Ball, FCrowdyKitGroundPosition::StaticStruct());
	TestTrue(TEXT("a pitch names its corner, size, goals and score"), HasField(State, TEXT("origin")) && HasField(State, TEXT("width")) && HasField(State, TEXT("depth"))
		&& HasField(State, TEXT("goal_width")) && HasField(State, TEXT("ball")) && HasField(State, TEXT("players")) && HasField(State, TEXT("score")) && HasField(State, TEXT("ends_at")));
	TestTrue(TEXT("and its winner with a presence flag"), HasField(State, TEXT("winner")) && !HasField(State, TEXT("has_winner")));
	TestTrue(TEXT("the ball names its speed, holder and touches as its Rust does"), HasField(TheBall, TEXT("vx")) && HasField(TheBall, TEXT("vy")) && HasField(TheBall, TEXT("holder"))
		&& HasField(TheBall, TEXT("held_since")) && HasField(TheBall, TEXT("last_touch")));
	TestTrue(TEXT("a player has its team and place"), HasField(Player, TEXT("team")) && HasField(Player, TEXT("x")) && HasField(Player, TEXT("y")));
	TestTrue(TEXT("a position is read from its [x, y] in that order"), Ground && Ground->Fields.Num() == 2 && Ground->Fields[0].ServerName == TEXT("x") && Ground->Fields[1].ServerName == TEXT("y"));
	TestTrue(TEXT("teams are the names its Rust sends"), EnumNamesAre(*Ball, StaticEnum<ECrowdyKitBallTeam>(), {TEXT("red"), TEXT("blue")}));
	TestTrue(TEXT("and so are the phases"), EnumNamesAre(*Ball, StaticEnum<ECrowdyKitBallPhase>(), {TEXT("lobby"), TEXT("playing"), TEXT("finished")}));

	for (const TCHAR* Method : {TEXT("join"), TEXT("leave"), TEXT("start"), TEXT("move"), TEXT("claim"), TEXT("pass"), TEXT("shoot"), TEXT("state")})
	{
		TestTrue(FString::Printf(TEXT("ball's Rust answers %s"), Method), Lib.Contains(FString::Printf(TEXT("\"%s\""), Method), ESearchCase::CaseSensitive));
	}
	for (const TCHAR* Key : {TEXT("origin"), TEXT("goal_width"), TEXT("held_since"), TEXT("last_touch"), TEXT("idle_ms"), TEXT("realtime"), TEXT("B{key}@1")})
	{
		TestTrue(FString::Printf(TEXT("ball's Rust has the key %s"), Key), Lib.Contains(Key, ESearchCase::CaseSensitive));
	}
	return true;
}

#endif
