#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodegen.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyServerObjectDefinition.h"
#include "Misc/AutomationTest.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace CrowdyExecRequiredFeatureTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** A type whose State is one Int32 variable, so it bakes and generates with nothing else set. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDefinition(const TCHAR* TypeName)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TypeName;
		Definition->StateForm = ECrowdyServerValuesForm::List;
		FPropertyBagPropertyDesc Gold(TEXT("Gold"), EPropertyBagPropertyType::Int32);
		Gold.ID = FGuid::NewDeterministicGuid(TEXT("Gold"));
		Definition->StateList.AddProperties({Gold});
		return Definition;
	}

	FCrowdyServerFunction& AddFunction(UCrowdyServerObjectDefinition& Definition, const TCHAR* Name, ECrowdyServerFunctionCaller Caller, const TCHAR* Feature)
	{
		FCrowdyServerFunction& Function = Definition.Functions.AddDefaulted_GetRef();
		Function.Name = Name;
		Function.WhoCanCall = Caller;
		Function.RequiredFeature = Feature;
		return Function;
	}

	TArray<FString> BakeErrors(UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Errors;
		Definition.Bake(Errors);
		return Errors;
	}

	bool HasError(const TArray<FString>& Errors, const TCHAR* Part)
	{
		return Errors.ContainsByPredicate([Part](const FString& Error) { return Error.Contains(Part, ESearchCase::CaseSensitive); });
	}

	bool GenerateLib(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, FString& OutLib)
	{
		const TArray<FString> Errors = BakeErrors(Definition);
		if (!Test.TestTrue(FString::Printf(TEXT("%s bakes (%s)"), *Definition.TypeName, *FString::Join(Errors, TEXT("; "))), Errors.IsEmpty()))
		{
			return false;
		}
		CrowdyExecCodegen::FGeneratedCrate Crate;
		FString Error;
		const bool bGenerated = CrowdyExecCodegen::Generate(Definition, Crate, Error);
		if (!Test.TestTrue(FString::Printf(TEXT("%s generates (%s)"), *Definition.TypeName, *Error), bGenerated))
		{
			return false;
		}
		const CrowdyExecCodegen::FGeneratedFile* Lib = Crate.Files.FindByPredicate([](const CrowdyExecCodegen::FGeneratedFile& File)
		{
			return File.Path.Equals(TEXT("src/lib.rs"), ESearchCase::CaseSensitive);
		});
		if (!Test.TestNotNull(TEXT("the crate has a lib.rs"), Lib))
		{
			return false;
		}
		OutLib = Lib->Text;
		return true;
	}

	int32 CountText(const FString& Text, const TCHAR* Part)
	{
		int32 Count = 0;
		for (int32 Found = Text.Find(Part, ESearchCase::CaseSensitive); Found != INDEX_NONE; Found = Text.Find(Part, ESearchCase::CaseSensitive, ESearchDir::FromStart, Found + 1))
		{
			++Count;
		}
		return Count;
	}

	int32 FindFrom(const FString& Text, const TCHAR* Part, int32 From)
	{
		return Text.Find(Part, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
	}

	/** The glue's arm for Method must run Authorize, then the feature check, then the cooldown check, in that order. */
	void CheckArmOrder(FAutomationTestBase& Test, const FString& Lib, const TCHAR* Method, const TCHAR* Authorize, const TCHAR* Cooldown)
	{
		const int32 Arm = FindFrom(Lib, *FString::Printf(TEXT("            \"%s\" => {\n"), Method), 0);
		const int32 Check = FindFrom(Lib, TEXT("require_feature(ctx, &call, \"vip\")?;\n"), Arm);
		if (!Test.TestTrue(FString::Printf(TEXT("%s has its dispatch arm and a feature check after it"), Method), Arm != INDEX_NONE && Check != INDEX_NONE))
		{
			return;
		}
		const int32 Callers = FindFrom(Lib, Authorize, Arm);
		const int32 Wait = FindFrom(Lib, Cooldown, Arm);
		Test.TestTrue(FString::Printf(TEXT("%s checks Callable By before the feature"), Method), Callers != INDEX_NONE && Callers < Check);
		Test.TestTrue(FString::Printf(TEXT("%s checks the feature before the cooldown"), Method), Wait != INDEX_NONE && Check < Wait);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRequiredFeatureGlueTest, "CrowdySDK.CrowdyExecEditor.RequiredFeatureIsCheckedInTheGlue", CrowdyExecRequiredFeatureTests::TestFlags)
bool FCrowdyExecRequiredFeatureGlueTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRequiredFeatureTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Rooms = MakeDefinition(TEXT("feature_rooms"));
	Rooms->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	AddFunction(*Rooms, TEXT("Claim"), ECrowdyServerFunctionCaller::Members, TEXT("vip")).CooldownSeconds = 3.f;
	AddFunction(*Rooms, TEXT("Free"), ECrowdyServerFunctionCaller::Players, TEXT(""));
	FString Lib;
	if (!GenerateLib(*this, *Rooms, Lib))
	{
		return false;
	}
	CheckArmOrder(*this, Lib, TEXT("claim"), TEXT("authorize(ctx, &call, &self.roster, Rule::Members)?;\n"), TEXT("check_cooldown(&self.cooldowns, ctx, &call, \"Claim\", 3000)?;\n"));
	TestEqual(TEXT("only the function with a key checks one"), CountText(Lib, TEXT("require_feature(ctx, &call,")), 1);
	TestEqual(TEXT("the check is written once"), CountText(Lib, TEXT("fn require_feature(")), 1);
	TestTrue(TEXT("a missing feature is a denied refusal naming the key"), Lib.Contains(TEXT("Err(Error::new(&format!(\"denied: needs the {feature} feature\")))"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("a features read that fails is a denied refusal"), Lib.Contains(TEXT("\"denied: features could not be checked ({error})\""), ESearchCase::CaseSensitive));
	TestTrue(TEXT("only a player's call is checked"), Lib.Contains(TEXT("fn require_feature(ctx: &Ctx, call: &Call<'_>, feature: &str) -> Result<()> {\n    let Ok(player) = call.player() else {\n        return Ok(());\n"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the features are the caller's own"), Lib.Contains(TEXT(".features(player)"), ESearchCase::CaseSensitive));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Plain = MakeDefinition(TEXT("feature_plain"));
	AddFunction(*Plain, TEXT("Claim"), ECrowdyServerFunctionCaller::Players, TEXT("vip"));
	FString PlainLib;
	if (!GenerateLib(*this, *Plain, PlainLib))
	{
		return false;
	}
	const int32 Arm = FindFrom(PlainLib, TEXT("            \"claim\" => {\n"), 0);
	const int32 Callers = FindFrom(PlainLib, TEXT("authorize(ctx, &call, true)?;\n"), Arm);
	const int32 Check = FindFrom(PlainLib, TEXT("require_feature(ctx, &call, \"vip\")?;\n"), Arm);
	TestTrue(TEXT("without members the feature is checked after Callable By too"), Arm != INDEX_NONE && Callers != INDEX_NONE && Callers < Check);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRequiredFeatureAbsentTest, "CrowdySDK.CrowdyExecEditor.NoRequiredFeatureWritesNoCheck", CrowdyExecRequiredFeatureTests::TestFlags)
bool FCrowdyExecRequiredFeatureAbsentTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRequiredFeatureTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(TEXT("feature_none"));
	AddFunction(*Definition, TEXT("Claim"), ECrowdyServerFunctionCaller::Players, TEXT("")).CooldownSeconds = 3.f;
	FString Lib;
	if (!GenerateLib(*this, *Definition, Lib))
	{
		return false;
	}
	TestFalse(TEXT("no feature check is called"), Lib.Contains(TEXT("require_feature"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("the platform's features are not read"), Lib.Contains(TEXT("features("), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRequiredFeatureNamedNoneTest, "CrowdySDK.CrowdyExecEditor.RequiredFeatureNamedNoneIsChecked", CrowdyExecRequiredFeatureTests::TestFlags)
bool FCrowdyExecRequiredFeatureNamedNoneTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRequiredFeatureTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(TEXT("feature_named_none"));
	AddFunction(*Definition, TEXT("Claim"), ECrowdyServerFunctionCaller::Players, TEXT("None"));
	TestEqual(TEXT("a key that reads like an empty name still bakes"), BakeErrors(*Definition).Num(), 0);
	FString Lib;
	if (!GenerateLib(*this, *Definition, Lib))
	{
		return false;
	}
	TestTrue(TEXT("a key named None is checked as written"), Lib.Contains(TEXT("require_feature(ctx, &call, \"None\")?;"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("and asks for players.read"), CountText(CrowdyExecDeploy::MakeManifest({Definition.Get()}), TEXT("\"players.read\"")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRequiredFeatureManifestTest, "CrowdySDK.CrowdyExecEditor.RequiredFeatureAsksForPlayersRead", CrowdyExecRequiredFeatureTests::TestFlags)
bool FCrowdyExecRequiredFeatureManifestTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRequiredFeatureTests;
	const TCHAR* const Scope = TEXT("\"scopes\":[\"players.read\"]");
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Bare = MakeDefinition(TEXT("bare"));
	AddFunction(*Bare, TEXT("Claim"), ECrowdyServerFunctionCaller::Players, TEXT(""));
	TestEqual(TEXT("no feature and no team asks for no scope"), CountText(CrowdyExecDeploy::MakeManifest({Bare.Get()}), TEXT("scopes")), 0);

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Gated = MakeDefinition(TEXT("gated"));
	AddFunction(*Gated, TEXT("Free"), ECrowdyServerFunctionCaller::Players, TEXT(""));
	AddFunction(*Gated, TEXT("Claim"), ECrowdyServerFunctionCaller::Players, TEXT("vip"));
	AddFunction(*Gated, TEXT("Raid"), ECrowdyServerFunctionCaller::Players, TEXT("raider"));
	const FString GatedManifest = CrowdyExecDeploy::MakeManifest({Gated.Get()});
	TestTrue(TEXT("a function with a feature asks for players.read, once however many functions have one"), GatedManifest.Contains(
		TEXT("\"gated\":{\"kind\":\"hub\",\"parent\":\"root\",\"crate\":\"gated\",\"client\":true,\"persist_every_ms\":30000,\"evict_after_ms\":300000,\"scopes\":[\"players.read\"]}}}"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("the scope is listed once"), CountText(GatedManifest, Scope), 1);

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Team = MakeDefinition(TEXT("team_gated"));
	Team->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	AddFunction(*Team, TEXT("Claim"), ECrowdyServerFunctionCaller::Players, TEXT("vip"));
	const FString TeamManifest = CrowdyExecDeploy::MakeManifest({Team.Get()});
	TestEqual(TEXT("a Crowdy Team type with a feature still lists the scope once"), CountText(TeamManifest, Scope), 1);
	TestEqual(TEXT("and players.read is not repeated anywhere"), CountText(TeamManifest, TEXT("players.read")), 1);

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> TeamOnly = MakeDefinition(TEXT("team_only"));
	TeamOnly->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	TestEqual(TEXT("a Crowdy Team type without a feature keeps its scope"), CountText(CrowdyExecDeploy::MakeManifest({TeamOnly.Get()}), Scope), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecReadsWorldManifestTest, "CrowdySDK.CrowdyExecEditor.ReadsWorldAsksForWorldRead", CrowdyExecRequiredFeatureTests::TestFlags)
bool FCrowdyExecReadsWorldManifestTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRequiredFeatureTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Neither = MakeDefinition(TEXT("neither"));
	TestEqual(TEXT("a type that reads no world data asks for no world.read"), CountText(CrowdyExecDeploy::MakeManifest({Neither.Get()}), TEXT("world.read")), 0);

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeDefinition(TEXT("world_only"));
	World->bReadsWorld = true;
	TestTrue(TEXT("Reads World Data asks for world.read alone"), CrowdyExecDeploy::MakeManifest({World.Get()}).Contains(
		TEXT("\"world_only\":{\"kind\":\"hub\",\"parent\":\"root\",\"crate\":\"world_only\",\"client\":true,\"persist_every_ms\":30000,\"evict_after_ms\":300000,\"scopes\":[\"world.read\"]}}}"), ESearchCase::CaseSensitive));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Both = MakeDefinition(TEXT("both"));
	Both->bReadsWorld = true;
	AddFunction(*Both, TEXT("Claim"), ECrowdyServerFunctionCaller::Players, TEXT("vip"));
	TestTrue(TEXT("with a feature it lists players.read first, then world.read"), CrowdyExecDeploy::MakeManifest({Both.Get()}).Contains(
		TEXT("\"scopes\":[\"players.read\",\"world.read\"]"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCrowdyTeamRecordsManifestTest, "CrowdySDK.CrowdyExecEditor.CrowdyTeamRecordsAskForPlayersRead", CrowdyExecRequiredFeatureTests::TestFlags)
bool FCrowdyExecCrowdyTeamRecordsManifestTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRequiredFeatureTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Own = MakeDefinition(TEXT("own_teams"));
	Own->Holds = ECrowdyServerObjectHolds::RecordPerEntity;
	TestEqual(TEXT("records whose teams are their own ask for no scope"), CountText(CrowdyExecDeploy::MakeManifest({Own.Get()}), TEXT("scopes")), 0);

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> OneSet = MakeDefinition(TEXT("one_set"));
	OneSet->TeamFrom = ECrowdyServerTeamSource::CrowdyTeam;
	TestEqual(TEXT("Team From is read only for records"), CountText(CrowdyExecDeploy::MakeManifest({OneSet.Get()}), TEXT("scopes")), 0);

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Teams = MakeDefinition(TEXT("crowdy_teams"));
	Teams->Holds = ECrowdyServerObjectHolds::RecordPerEntity;
	Teams->TeamFrom = ECrowdyServerTeamSource::CrowdyTeam;
	TestTrue(TEXT("records on Crowdy Teams ask for players.read"), CrowdyExecDeploy::MakeManifest({Teams.Get()}).Contains(
		TEXT("\"crate\":\"crowdy_teams\",\"client\":true,\"persist_every_ms\":30000,\"evict_after_ms\":300000,\"scopes\":[\"players.read\"]}"), ESearchCase::CaseSensitive));
	Teams->bReadsWorld = true;
	TestTrue(TEXT("then world.read, when it reads world data too"), CrowdyExecDeploy::MakeManifest({Teams.Get()}).Contains(
		TEXT("\"scopes\":[\"players.read\",\"world.read\"]"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecRequiredFeatureValidationTest,"CrowdySDK.CrowdyExecEditor.RequiredFeatureIsValidated", CrowdyExecRequiredFeatureTests::TestFlags)
bool FCrowdyExecRequiredFeatureValidationTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecRequiredFeatureTests;
	const FString Longest = FString::ChrN(64, TEXT('a'));
	for (const FString& Good : {FString(TEXT("vip")), FString(TEXT("Tier-2.beta_x")), Longest})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(TEXT("keys"));
		AddFunction(*Definition, TEXT("Claim"), ECrowdyServerFunctionCaller::Players, *Good);
		const TArray<FString> Errors = BakeErrors(*Definition);
		TestFalse(FString::Printf(TEXT("the key '%s' is accepted (%s)"), *Good, *FString::Join(Errors, TEXT("; "))), HasError(Errors, TEXT("Required Feature")));
	}
	for (const FString& Bad : {FString(TEXT("has space")), FString(TEXT("a/b")), FString(TEXT("café")), FString::ChrN(65, TEXT('a'))})
	{
		const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(TEXT("keys"));
		AddFunction(*Definition, TEXT("Claim"), ECrowdyServerFunctionCaller::Players, *Bad);
		TestTrue(FString::Printf(TEXT("the key '%s' is refused"), *Bad), HasError(BakeErrors(*Definition), TEXT("Function Claim: Required Feature")));
	}

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Server = MakeDefinition(TEXT("keys"));
	AddFunction(*Server, TEXT("Claim"), ECrowdyServerFunctionCaller::ServerOnly, TEXT("vip"));
	TestTrue(TEXT("a Server Only function with a key is refused, since it would never be checked"), HasError(BakeErrors(*Server), TEXT("Required Feature is only checked for players")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> ServerPlain = MakeDefinition(TEXT("keys"));
	AddFunction(*ServerPlain, TEXT("Claim"), ECrowdyServerFunctionCaller::ServerOnly, TEXT(""));
	TestTrue(TEXT("a Server Only function without one bakes"), BakeErrors(*ServerPlain).IsEmpty());
	return true;
}

#endif
