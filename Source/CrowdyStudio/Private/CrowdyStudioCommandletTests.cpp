#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "ConfigSync/FCrowdyConfigSync.h"
#include "CrowdyCppClient.h"
#include "CrowdyStudioCommandlet.h"
#include "Misc/AutomationTest.h"
#include "Model/CrowdyStudioControllerTestAccess.h"
#include "Settings/CrowdyStudioUserSettings.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStudioCommandletTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const CompleteCreate = TEXT("-run=CrowdyStudio -op=createapp -org=74637546537472 -name=\"My Game\" -slug=my-game -datacenter=or -description=\"A game, with a comma\"");

	bool Refuses(const FString& Params, const TCHAR* Switch)
	{
		FCrowdyStudioCommandRequest Request;
		FString Error;
		return !UCrowdyStudioCommandlet::ParseRequest(Params, Request, Error) && Error.Contains(Switch);
	}

	bool RefusesWith(const FString& Params, const TCHAR* Switch, const TCHAR* Fragment)
	{
		FCrowdyStudioCommandRequest Request;
		FString Error;
		return !UCrowdyStudioCommandlet::ParseRequest(Params, Request, Error) && Error.Contains(Switch) && Error.Contains(Fragment);
	}

	bool Accepts(const FString& Params, FCrowdyStudioCommandRequest& Request)
	{
		FString Error;
		return UCrowdyStudioCommandlet::ParseRequest(Params, Request, Error) && Error.IsEmpty();
	}

	TSharedPtr<FStudioAppFeature> Feature(const TCHAR* Key)
	{
		TSharedPtr<FStudioAppFeature> Result = MakeShared<FStudioAppFeature>();
		Result->FeatureKey = Key;
		return Result;
	}

	TSharedPtr<FStudioAccessTier> Tier(int64 TierId)
	{
		TSharedPtr<FStudioAccessTier> Result = MakeShared<FStudioAccessTier>();
		Result->TierId = TierId;
		return Result;
	}

	TSharedPtr<FStudioTierFeature> TierGrant(int64 TierId, const TCHAR* Key)
	{
		TSharedPtr<FStudioTierFeature> Result = MakeShared<FStudioTierFeature>();
		Result->TierId = TierId;
		Result->FeatureKey = Key;
		return Result;
	}

	FStudioDatacenter Datacenter(const TCHAR* Code, bool bPlaceable)
	{
		FStudioDatacenter Dc;
		Dc.Code = Code;
		Dc.bPlaceable = bPlaceable;
		return Dc;
	}

	TSharedPtr<FStudioOrg> Org(int64 OrgId)
	{
		TSharedPtr<FStudioOrg> Result = MakeShared<FStudioOrg>();
		Result->OrgId = OrgId;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesCreateAppTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesCreateApp", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesCreateAppTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;
	TestTrue(TEXT("a complete createapp parses"), UCrowdyStudioCommandlet::ParseRequest(CompleteCreate, Request, Error));
	TestTrue(TEXT("no error is reported"), Error.IsEmpty());
	TestEqual(TEXT("the operation is createapp"), Request.Op, FString(TEXT("createapp")));
	TestEqual(TEXT("an organization id wider than 32 bits is kept"), Request.OrgId, static_cast<int64>(74637546537472));
	TestEqual(TEXT("a quoted name keeps its space"), Request.Name, FString(TEXT("My Game")));
	TestEqual(TEXT("the slug is read"), Request.Slug, FString(TEXT("my-game")));
	TestEqual(TEXT("the datacenter is read"), Request.Datacenter, FString(TEXT("or")));
	TestEqual(TEXT("a quoted description keeps its comma"), Request.Description, FString(TEXT("A game, with a comma")));

	TestTrue(TEXT("the description is optional"), UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=createapp -org=5 -name=G -slug=g -datacenter=va"), Request, Error));
	TestTrue(TEXT("an absent description is empty"), Request.Description.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletNamesTheMissingSwitchTest, "CrowdySDK.CrowdyStudio.Commandlet.NamesTheMissingSwitch", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletNamesTheMissingSwitchTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(TEXT("createapp without -org names -org"), Refuses(TEXT("-op=createapp -name=G -slug=g -datacenter=or"), TEXT("-org")));
	TestTrue(TEXT("a non-numeric -org names -org"), Refuses(TEXT("-op=createapp -org=acme -name=G -slug=g -datacenter=or"), TEXT("-org")));
	TestTrue(TEXT("a zero -org names -org"), Refuses(TEXT("-op=createapp -org=0 -name=G -slug=g -datacenter=or"), TEXT("-org")));
	TestTrue(TEXT("createapp without -name names -name"), Refuses(TEXT("-op=createapp -org=5 -slug=g -datacenter=or"), TEXT("-name")));
	TestTrue(TEXT("createapp without -slug names -slug"), Refuses(TEXT("-op=createapp -org=5 -name=G -datacenter=or"), TEXT("-slug")));
	TestTrue(TEXT("a slug the Create app sheet would refuse names -slug"), Refuses(TEXT("-op=createapp -org=5 -name=G -slug=Bad_Slug -datacenter=or"), TEXT("-slug")));
	TestTrue(TEXT("createapp without -datacenter names -datacenter"), Refuses(TEXT("-op=createapp -org=5 -name=G -slug=g"), TEXT("-datacenter")));
	TestTrue(TEXT("sync without -app names -app"), Refuses(TEXT("-op=sync -yes"), TEXT("-app")));
	TestTrue(TEXT("a non-numeric -app names -app"), Refuses(TEXT("-op=sync -app=latest -yes"), TEXT("-app")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletRequiresYesTest, "CrowdySDK.CrowdyStudio.Commandlet.RequiresYesToChange", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletRequiresYesTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;

	UCrowdyStudioCommandlet::ParseRequest(CompleteCreate, Request, Error);
	TestTrue(TEXT("createapp without -yes needs confirmation"), UCrowdyStudioCommandlet::NeedsConfirmation(Request));
	UCrowdyStudioCommandlet::ParseRequest(FString(CompleteCreate) + TEXT(" -yes"), Request, Error);
	TestFalse(TEXT("createapp with -yes does not"), UCrowdyStudioCommandlet::NeedsConfirmation(Request));

	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=sync -app=77"), Request, Error);
	TestEqual(TEXT("the app id is read"), Request.AppId, static_cast<int64>(77));
	TestTrue(TEXT("sync without -yes needs confirmation"), UCrowdyStudioCommandlet::NeedsConfirmation(Request));
	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=sync -app=77 -yes"), Request, Error);
	TestFalse(TEXT("sync with -yes does not"), UCrowdyStudioCommandlet::NeedsConfirmation(Request));

	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=apps"), Request, Error);
	TestFalse(TEXT("apps changes nothing, so it never needs -yes"), UCrowdyStudioCommandlet::NeedsConfirmation(Request));

	const TCHAR* const Changing[] = {
		TEXT("-op=definefeature -key=vip"),
		TEXT("-op=grantfeature -tier=3 -key=vip"),
		TEXT("-op=revokefeature -tier=3 -key=vip"),
		TEXT("-op=createteam -name=Red"),
		TEXT("-op=createchannel -name=Chat"),
		TEXT("-op=creategrid -corner1=\"0,0,0\" -corner2=\"1,1,1\""),
		TEXT("-op=grantgrid -grid=9 -user=4 -keys=\"a\""),
		TEXT("-op=revokegrid -grid=9 -group=4 -all"),
		TEXT("-op=addmember -kind=team -group=7 -user=9"),
		TEXT("-op=removemember -kind=team -group=7 -user=9"),
		TEXT("-op=setroles -kind=team -group=7 -user=9 -noroles"),
		TEXT("-op=createrole -kind=team -group=7 -name=Scout"),
		TEXT("-op=updaterole -kind=team -group=7 -role=3 -rank=2"),
		TEXT("-op=deleterole -kind=team -group=7 -role=3"),
		TEXT("-op=updategroup -kind=team -group=7 -name=Blue"),
		TEXT("-op=deletegroup -kind=channel -group=7"),
		TEXT("-op=setteampolicy -creation=admin"),
		TEXT("-op=setchannelpolicy -maxmembers=0"),
		TEXT("-op=gridlimits -grid=9 -nolimit") };
	for (const TCHAR* Params : Changing)
	{
		FCrowdyStudioCommandRequest Without;
		FCrowdyStudioCommandRequest With;
		Accepts(Params, Without);
		Accepts(FString(Params) + TEXT(" -yes"), With);
		TestTrue(FString::Printf(TEXT("%s needs confirmation without -yes"), Params), UCrowdyStudioCommandlet::NeedsConfirmation(Without));
		TestFalse(FString::Printf(TEXT("%s does not with -yes"), Params), UCrowdyStudioCommandlet::NeedsConfirmation(With));
	}

	const TCHAR* const Reading[] = { TEXT("-op=features"), TEXT("-op=teams"), TEXT("-op=channels"), TEXT("-op=grids -low=\"0,0,0\" -high=\"1,1,1\""),
		TEXT("-op=groupdetail -kind=team -group=7"), TEXT("-op=gridgrants -grid=9 -group=4"), TEXT("-op=gridpermissions -grid=9") };
	for (const TCHAR* Params : Reading)
	{
		FCrowdyStudioCommandRequest Read;
		Accepts(Params, Read);
		TestFalse(FString::Printf(TEXT("%s only reads, so it never needs -yes"), Params), UCrowdyStudioCommandlet::NeedsConfirmation(Read));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesAccessTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesAccess", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesAccessTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	TestTrue(TEXT("features parses without -app"), Accepts(TEXT("-op=features"), Request));
	TestEqual(TEXT("an absent -app is 0, the project's app"), Request.AppId, static_cast<int64>(0));
	TestTrue(TEXT("features reads -app"), Accepts(TEXT("-op=features -app=74637611160064"), Request));
	TestEqual(TEXT("-app keeps a full-width id"), Request.AppId, static_cast<int64>(74637611160064));

	TestTrue(TEXT("definefeature parses"), Accepts(TEXT("-op=definefeature -key=vip.pass-2_b -description=\"A pass, for VIPs\""), Request));
	TestEqual(TEXT("the key is read"), Request.Key, FString(TEXT("vip.pass-2_b")));
	TestEqual(TEXT("a quoted description keeps its comma"), Request.Description, FString(TEXT("A pass, for VIPs")));

	TestTrue(TEXT("grantfeature parses"), Accepts(TEXT("-op=grantfeature -app=5 -tier=74637611160065 -key=vip"), Request));
	TestEqual(TEXT("the tier id is read"), Request.TierId, static_cast<int64>(74637611160065));

	TestTrue(TEXT("a non-numeric -app is refused naming -app"), Refuses(TEXT("-op=features -app=latest"), TEXT("-app")));
	TestTrue(TEXT("definefeature without -key names -key"), RefusesWith(TEXT("-op=definefeature"), TEXT("-key"), TEXT("required")));
	TestTrue(TEXT("a key with a space is refused with the key rule"), RefusesWith(TEXT("-op=definefeature -key=\"VIP Pass\""), TEXT("-key"), TEXT("letters, digits")));
	TestTrue(TEXT("a key over 64 characters is refused with the key rule"),
		RefusesWith(FString::Printf(TEXT("-op=definefeature -key=%s"), *FString::ChrN(65, TEXT('a'))), TEXT("-key"), TEXT("64")));
	TestTrue(TEXT("grantfeature without -tier names -tier"), RefusesWith(TEXT("-op=grantfeature -key=vip"), TEXT("-tier"), TEXT("required")));
	TestTrue(TEXT("a zero -tier names -tier"), RefusesWith(TEXT("-op=revokefeature -tier=0 -key=vip"), TEXT("-tier"), TEXT("not an id")));
	TestTrue(TEXT("revokefeature without -key names -key"), RefusesWith(TEXT("-op=revokefeature -tier=3"), TEXT("-key"), TEXT("required")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletResolvesTheProjectAppTest, "CrowdySDK.CrowdyStudio.Commandlet.ResolvesTheProjectApp", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletResolvesTheProjectAppTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;
	Accepts(TEXT("-op=features -app=11"), Request);
	TestEqual(TEXT("-app wins over the project's app"), UCrowdyStudioCommandlet::ResolveAppId(Request, 22, Error), static_cast<int64>(11));
	TestTrue(TEXT("no error when -app is given"), Error.IsEmpty());

	Accepts(TEXT("-op=features"), Request);
	TestEqual(TEXT("without -app the project's synced app is used"), UCrowdyStudioCommandlet::ResolveAppId(Request, 22, Error), static_cast<int64>(22));
	TestTrue(TEXT("no error when the project has an app"), Error.IsEmpty());

	TestEqual(TEXT("without -app and with no synced app nothing resolves"), UCrowdyStudioCommandlet::ResolveAppId(Request, 0, Error), static_cast<int64>(0));
	TestTrue(TEXT("the refusal names -app"), Error.Contains(TEXT("-app")));
	TestTrue(TEXT("the refusal points at sync"), Error.Contains(TEXT("-op=sync")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletRefusesLikeTheAccessPageTest, "CrowdySDK.CrowdyStudio.Commandlet.RefusesLikeTheAccessPage", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletRefusesLikeTheAccessPageTest::RunTest(const FString& /*Parameters*/)
{
	const TArray<TSharedPtr<FStudioAppFeature>> Features = { nullptr, Feature(TEXT("vip")) };
	const TArray<TSharedPtr<FStudioAccessTier>> Tiers = { nullptr, Tier(3) };
	FCrowdyStudioCommandRequest Request;

	Accepts(TEXT("-op=grantfeature -tier=3 -key=vip"), Request);
	TestTrue(TEXT("a defined feature on a listed tier is accepted"), UCrowdyStudioCommandlet::FindAccessProblem(Request, Features, Tiers).IsEmpty());

	Accepts(TEXT("-op=grantfeature -tier=4 -key=vip"), Request);
	const FString UnknownTier = UCrowdyStudioCommandlet::FindAccessProblem(Request, Features, Tiers);
	TestTrue(TEXT("a tier the app does not have is refused, naming -tier"), UnknownTier.Contains(TEXT("-tier=4")) && UnknownTier.Contains(TEXT("-op=features")));

	Accepts(TEXT("-op=grantfeature -tier=3 -key=other"), Request);
	const FString UnknownFeature = UCrowdyStudioCommandlet::FindAccessProblem(Request, Features, Tiers);
	TestTrue(TEXT("granting a feature that is not defined is refused, naming -key"), UnknownFeature.Contains(TEXT("-key=other")) && UnknownFeature.Contains(TEXT("definefeature")));

	Accepts(TEXT("-op=grantfeature -tier=3 -key=VIP"), Request);
	TestFalse(TEXT("a feature key in another case is another key"), UCrowdyStudioCommandlet::FindAccessProblem(Request, Features, Tiers).IsEmpty());

	Accepts(TEXT("-op=revokefeature -tier=3 -key=other"), Request);
	TestTrue(TEXT("revoking a key that is no longer defined is allowed"), UCrowdyStudioCommandlet::FindAccessProblem(Request, Features, Tiers).IsEmpty());
	Accepts(TEXT("-op=revokefeature -tier=4 -key=vip"), Request);
	TestFalse(TEXT("revoking from a tier the app does not have is refused"), UCrowdyStudioCommandlet::FindAccessProblem(Request, Features, Tiers).IsEmpty());

	Accepts(TEXT("-op=definefeature -key=other"), Request);
	TestTrue(TEXT("defining needs neither a tier nor an existing feature"), UCrowdyStudioCommandlet::FindAccessProblem(Request, Features, Tiers).IsEmpty());

	const TArray<TSharedPtr<FStudioTierFeature>> Grants = { nullptr, TierGrant(3, TEXT("vip")) };
	TestTrue(TEXT("a grant is found by tier and key"), UCrowdyStudioCommandlet::IsGranted(Grants, 3, TEXT("vip")));
	TestFalse(TEXT("another tier does not hold it"), UCrowdyStudioCommandlet::IsGranted(Grants, 4, TEXT("vip")));
	TestFalse(TEXT("another key is not held"), UCrowdyStudioCommandlet::IsGranted(Grants, 3, TEXT("other")));
	TestFalse(TEXT("the key match is case sensitive"), UCrowdyStudioCommandlet::IsGranted(Grants, 3, TEXT("VIP")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletFeatureKeyMatchesStudioTest, "CrowdySDK.CrowdyStudio.Commandlet.FeatureKeyMatchesStudio", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletFeatureKeyMatchesStudioTest::RunTest(const FString& /*Parameters*/)
{
	for (const TCHAR* Key : { TEXT("a"), TEXT("vip.pass-2_B"), TEXT("0") })
	{
		TestTrue(FString::Printf(TEXT("%s is a key Studio defines"), Key), FCrowdyStudioController::IsFeatureKey(Key));
	}
	TestTrue(TEXT("64 characters is the longest key"), FCrowdyStudioController::IsFeatureKey(FString::ChrN(64, TEXT('a'))));
	TestFalse(TEXT("65 characters is too long"), FCrowdyStudioController::IsFeatureKey(FString::ChrN(65, TEXT('a'))));
	TestFalse(TEXT("an empty key is refused"), FCrowdyStudioController::IsFeatureKey(FString()));
	for (const TCHAR* Key : { TEXT("VIP Pass"), TEXT("vip/pass"), TEXT("víp") })
	{
		TestFalse(FString::Printf(TEXT("%s is not a key"), Key), FCrowdyStudioController::IsFeatureKey(Key));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesGroupsTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesTeamsAndChannels", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesGroupsTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	TestTrue(TEXT("teams parses"), Accepts(TEXT("-op=teams -app=5"), Request));
	TestEqual(TEXT("teams reads -app"), Request.AppId, static_cast<int64>(5));
	TestTrue(TEXT("channels parses"), Accepts(TEXT("-op=channels"), Request));

	TestTrue(TEXT("createteam parses"), Accepts(TEXT("-op=createteam -name=\"Red Team\" -description=\"A team, of reds\" -policy=INVITE"), Request));
	TestEqual(TEXT("the name keeps its space"), Request.Name, FString(TEXT("Red Team")));
	TestEqual(TEXT("the description keeps its comma"), Request.Description, FString(TEXT("A team, of reds")));
	TestEqual(TEXT("the policy is lower-cased"), Request.Policy, FString(TEXT("invite")));

	TestTrue(TEXT("createteam without -policy parses"), Accepts(TEXT("-op=createteam -name=Red"), Request));
	TestEqual(TEXT("a team defaults to open membership, as the Teams page does"), Request.Policy, FString(TEXT("open")));

	TestTrue(TEXT("createchannel without -policy parses"), Accepts(TEXT("-op=createchannel -name=Chat"), Request));
	TestTrue(TEXT("a channel leaves the policy to the app, as the Channels page does"), Request.Policy.IsEmpty());
	TestTrue(TEXT("members can send by default"), Request.bMembersCanSend);
	TestTrue(TEXT("createchannel reads -memberscansend=false"), Accepts(TEXT("-op=createchannel -name=Chat -memberscansend=false"), Request));
	TestFalse(TEXT("members cannot send when told so"), Request.bMembersCanSend);
	TestTrue(TEXT("createchannel reads -memberscansend=1"), Accepts(TEXT("-op=createchannel -name=Chat -memberscansend=1"), Request));
	TestTrue(TEXT("1 means true"), Request.bMembersCanSend);

	TestTrue(TEXT("createteam without -name names -name"), RefusesWith(TEXT("-op=createteam -policy=open"), TEXT("-name"), TEXT("required")));
	TestTrue(TEXT("createchannel without -name names -name"), RefusesWith(TEXT("-op=createchannel"), TEXT("-name"), TEXT("required")));
	TestTrue(TEXT("an unknown policy names -policy and the choices"), RefusesWith(TEXT("-op=createteam -name=Red -policy=closed"), TEXT("-policy"), TEXT("open, request, invite or admin")));
	TestTrue(TEXT("a -memberscansend that is not a boolean names -memberscansend"), RefusesWith(TEXT("-op=createchannel -name=Chat -memberscansend=maybe"), TEXT("-memberscansend"), TEXT("true or false")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesGridsTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesGrids", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesGridsTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	TestTrue(TEXT("grids parses"), Accepts(TEXT("-op=grids -low=\"-4,0,12\" -high=\"3,7,74637611160064\" -user=9"), Request));
	TestEqual(TEXT("low x keeps its sign"), Request.Low.X, static_cast<int64>(-4));
	TestEqual(TEXT("low y"), Request.Low.Y, static_cast<int64>(0));
	TestEqual(TEXT("low z"), Request.Low.Z, static_cast<int64>(12));
	TestEqual(TEXT("high x"), Request.High.X, static_cast<int64>(3));
	TestEqual(TEXT("high z is wider than 32 bits"), Request.High.Z, static_cast<int64>(74637611160064));
	TestEqual(TEXT("-user is read"), Request.UserId, static_cast<int64>(9));

	TestTrue(TEXT("grids without -user parses"), Accepts(TEXT("-op=grids -low=\"0,0,0\" -high=\"1, 1, 1\""), Request));
	TestEqual(TEXT("an absent -user is 0, the signed-in user"), Request.UserId, static_cast<int64>(0));
	TestEqual(TEXT("spaces around an axis are ignored"), Request.High.Y, static_cast<int64>(1));

	TestTrue(TEXT("creategrid parses"), Accepts(TEXT("-op=creategrid -corner1=\"0,0,0\" -corner2=\"15,15,15\""), Request));
	TestEqual(TEXT("corner 1 is read into Low"), Request.Low.X, static_cast<int64>(0));
	TestEqual(TEXT("corner 2 is read into High"), Request.High.X, static_cast<int64>(15));

	TestTrue(TEXT("grids without -low names -low"), RefusesWith(TEXT("-op=grids -high=\"1,1,1\""), TEXT("-low"), TEXT("required")));
	TestTrue(TEXT("grids without -high names -high"), RefusesWith(TEXT("-op=grids -low=\"1,1,1\""), TEXT("-high"), TEXT("required")));
	TestTrue(TEXT("an unquoted triple is cut at its comma and refused"), RefusesWith(TEXT("-op=grids -low=0,0,0 -high=\"1,1,1\""), TEXT("-low"), TEXT("in quotes")));
	TestTrue(TEXT("two coordinates are refused"), RefusesWith(TEXT("-op=grids -low=\"1,1\" -high=\"1,1,1\""), TEXT("-low"), TEXT("x,y,z")));
	TestTrue(TEXT("a fractional coordinate is refused"), RefusesWith(TEXT("-op=grids -low=\"1,1.5,1\" -high=\"1,1,1\""), TEXT("-low"), TEXT("whole numbers")));
	TestTrue(TEXT("an empty coordinate is refused"), RefusesWith(TEXT("-op=creategrid -corner1=\"1,,1\" -corner2=\"1,1,1\""), TEXT("-corner1"), TEXT("whole numbers")));
	TestTrue(TEXT("creategrid without -corner2 names -corner2"), RefusesWith(TEXT("-op=creategrid -corner1=\"1,1,1\""), TEXT("-corner2"), TEXT("required")));
	TestTrue(TEXT("a non-numeric -user names -user"), Refuses(TEXT("-op=grids -low=\"0,0,0\" -high=\"1,1,1\" -user=me"), TEXT("-user")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesGridGrantsTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesGridGrants", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesGridGrantsTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	TestTrue(TEXT("a user grant parses"), Accepts(TEXT("-op=grantgrid -grid=74637611160064 -user=9 -keys=\"voxel.edit, voxel.place,voxel.edit\""), Request));
	TestEqual(TEXT("the grid id is full width"), Request.GridId, static_cast<int64>(74637611160064));
	TestEqual(TEXT("the user is read"), Request.UserId, static_cast<int64>(9));
	TestEqual(TEXT("repeated and padded keys are trimmed and listed once"), Request.Keys.Num(), 2);
	TestEqual(TEXT("the first key"), Request.Keys[0], FString(TEXT("voxel.edit")));
	TestEqual(TEXT("the second key"), Request.Keys[1], FString(TEXT("voxel.place")));

	TestTrue(TEXT("a group grant with a role parses"), Accepts(TEXT("-op=grantgrid -grid=4 -group=8 -role=2 -keys=\"a\""), Request));
	TestEqual(TEXT("the group is read"), Request.GroupId, static_cast<int64>(8));
	TestEqual(TEXT("the role is read"), Request.RoleId, static_cast<int64>(2));

	TestTrue(TEXT("a revoke of named keys parses"), Accepts(TEXT("-op=revokegrid -grid=4 -user=9 -keys=\"a\""), Request));
	TestFalse(TEXT("naming keys is not -all"), Request.bAllKeys);
	TestTrue(TEXT("a revoke of everything parses"), Accepts(TEXT("-op=revokegrid -grid=4 -user=9 -all"), Request));
	TestTrue(TEXT("-all is read"), Request.bAllKeys);
	TestTrue(TEXT("-all on a grant is not a key list"), RefusesWith(TEXT("-op=grantgrid -grid=4 -user=9 -all"), TEXT("-keys"), TEXT("required")));

	TestTrue(TEXT("a grant without -grid names -grid"), RefusesWith(TEXT("-op=grantgrid -user=9 -keys=\"a\""), TEXT("-grid"), TEXT("required")));
	TestTrue(TEXT("a grant with neither -user nor -group is refused"), RefusesWith(TEXT("-op=grantgrid -grid=4 -keys=\"a\""), TEXT("-user"), TEXT("exactly one")));
	TestTrue(TEXT("a grant with both -user and -group is refused"), RefusesWith(TEXT("-op=grantgrid -grid=4 -user=9 -group=8 -keys=\"a\""), TEXT("-group"), TEXT("exactly one")));
	TestTrue(TEXT("a role without a group is refused"), RefusesWith(TEXT("-op=grantgrid -grid=4 -user=9 -role=2 -keys=\"a\""), TEXT("-role"), TEXT("goes with -group")));
	TestTrue(TEXT("a grant without -keys names -keys"), RefusesWith(TEXT("-op=grantgrid -grid=4 -user=9"), TEXT("-keys"), TEXT("to grant")));
	TestTrue(TEXT("a revoke with neither -keys nor -all is refused"), RefusesWith(TEXT("-op=revokegrid -grid=4 -user=9"), TEXT("-keys"), TEXT("-all revokes every grant")));
	TestTrue(TEXT("a revoke with both -keys and -all is refused"), RefusesWith(TEXT("-op=revokegrid -grid=4 -user=9 -keys=\"a\" -all"), TEXT("-keys"), TEXT("not both")));
	TestTrue(TEXT("an unquoted key list is cut at its comma and refused"), RefusesWith(TEXT("-op=grantgrid -grid=4 -user=9 -keys=a,b"), TEXT("-keys"), TEXT("in quotes")));
	TestTrue(TEXT("a key over 64 characters is refused"), RefusesWith(FString::Printf(TEXT("-op=grantgrid -grid=4 -user=9 -keys=%s"), *FString::ChrN(65, TEXT('a'))), TEXT("-keys"), TEXT("64")));
	TestTrue(TEXT("a non-numeric -group names -group"), Refuses(TEXT("-op=grantgrid -grid=4 -group=red -keys=\"a\""), TEXT("-group")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletChecksPermissionKeysTest, "CrowdySDK.CrowdyStudio.Commandlet.ChecksPermissionKeys", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletChecksPermissionKeysTest::RunTest(const FString& /*Parameters*/)
{
	const TArray<FString> Catalog = { TEXT("voxel.edit"), TEXT("voxel.place") };
	TestTrue(TEXT("keys in the catalog are accepted"), UCrowdyStudioCommandlet::FindUnknownPermissionKey({ TEXT("voxel.edit"), TEXT("voxel.place") }, Catalog).IsEmpty());

	const FString Unknown = UCrowdyStudioCommandlet::FindUnknownPermissionKey({ TEXT("voxel.edit"), TEXT("voxel.break") }, Catalog);
	TestTrue(TEXT("a key outside the catalog is refused, naming -keys and the key"), Unknown.Contains(TEXT("-keys")) && Unknown.Contains(TEXT("voxel.break")));
	TestTrue(TEXT("the refusal lists the catalog"), Unknown.Contains(TEXT("voxel.edit, voxel.place")));

	TestFalse(TEXT("a key in another case is another key"), UCrowdyStudioCommandlet::FindUnknownPermissionKey({ TEXT("Voxel.Edit") }, Catalog).IsEmpty());
	TestTrue(TEXT("an empty catalog leaves the check to the server"), UCrowdyStudioCommandlet::FindUnknownPermissionKey({ TEXT("anything") }, TArray<FString>()).IsEmpty());
	TestTrue(TEXT("no keys, nothing to refuse"), UCrowdyStudioCommandlet::FindUnknownPermissionKey(TArray<FString>(), Catalog).IsEmpty());

	for (const TCHAR* Policy : { TEXT("open"), TEXT("request"), TEXT("invite"), TEXT("admin") })
	{
		TestTrue(FString::Printf(TEXT("%s is a membership policy"), Policy), UCrowdyStudioCommandlet::IsMembershipPolicy(Policy));
	}
	TestFalse(TEXT("closed is not"), UCrowdyStudioCommandlet::IsMembershipPolicy(TEXT("closed")));
	TestFalse(TEXT("an empty policy is not"), UCrowdyStudioCommandlet::IsMembershipPolicy(FString()));
	TestFalse(TEXT("the policy match is case sensitive, as it is sent verbatim"), UCrowdyStudioCommandlet::IsMembershipPolicy(TEXT("Open")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesMembersTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesMembers", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesMembersTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	TestTrue(TEXT("addmember parses"), Accepts(TEXT("-op=addmember -kind=team -group=74637611160064 -user=9"), Request));
	TestFalse(TEXT("-kind=team is a team"), Request.bChannel);
	TestEqual(TEXT("the group id is full width"), Request.GroupId, static_cast<int64>(74637611160064));
	TestEqual(TEXT("the user is read"), Request.UserId, static_cast<int64>(9));
	TestTrue(TEXT("removemember parses with -kind=CHANNEL"), Accepts(TEXT("-op=removemember -kind=CHANNEL -group=3 -user=9"), Request));
	TestTrue(TEXT("-kind=channel is a channel, in any case"), Request.bChannel);
	TestTrue(TEXT("groupdetail parses"), Accepts(TEXT("-op=groupdetail -kind=team -group=3"), Request));

	TestTrue(TEXT("setroles parses a role list"), Accepts(TEXT("-op=setroles -kind=team -group=3 -user=9 -roles=\"3, 4,3\""), Request));
	TestEqual(TEXT("repeated and padded role ids are listed once"), Request.RoleIds.Num(), 2);
	TestEqual(TEXT("the first role"), Request.RoleIds[0], static_cast<int64>(3));
	TestEqual(TEXT("the second role"), Request.RoleIds[1], static_cast<int64>(4));
	TestFalse(TEXT("naming roles is not -noroles"), Request.bNoRoles);
	TestTrue(TEXT("setroles parses -noroles"), Accepts(TEXT("-op=setroles -kind=team -group=3 -user=9 -noroles"), Request));
	TestTrue(TEXT("-noroles is read"), Request.bNoRoles && Request.RoleIds.IsEmpty());

	TestTrue(TEXT("a missing -kind names -kind"), RefusesWith(TEXT("-op=addmember -group=3 -user=9"), TEXT("-kind"), TEXT("required")));
	TestTrue(TEXT("an unknown -kind names -kind"), RefusesWith(TEXT("-op=addmember -kind=clan -group=3 -user=9"), TEXT("-kind"), TEXT("not team or channel")));
	TestTrue(TEXT("a missing -group names -group"), RefusesWith(TEXT("-op=addmember -kind=team -user=9"), TEXT("-group"), TEXT("required")));
	TestTrue(TEXT("a missing -user names -user"), RefusesWith(TEXT("-op=removemember -kind=team -group=3"), TEXT("-user"), TEXT("required")));
	TestTrue(TEXT("setroles without -roles or -noroles is refused"), RefusesWith(TEXT("-op=setroles -kind=team -group=3 -user=9"), TEXT("-roles"), TEXT("-noroles is required")));
	TestTrue(TEXT("setroles with both is refused"), RefusesWith(TEXT("-op=setroles -kind=team -group=3 -user=9 -roles=\"3\" -noroles"), TEXT("-roles"), TEXT("not both")));
	TestTrue(TEXT("a role entry that is not an id is refused"), RefusesWith(TEXT("-op=setroles -kind=team -group=3 -user=9 -roles=\"3,lead\""), TEXT("-roles"), TEXT("not a role id")));
	TestTrue(TEXT("a zero role id is refused"), RefusesWith(TEXT("-op=setroles -kind=team -group=3 -user=9 -roles=\"0\""), TEXT("-roles"), TEXT("not a role id")));
	TestTrue(TEXT("an unquoted role list is cut at its comma and refused"), RefusesWith(TEXT("-op=setroles -kind=team -group=3 -user=9 -roles=3,4"), TEXT("-roles"), TEXT("in quotes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesRolesTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesRoles", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesRolesTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	TestTrue(TEXT("createrole parses"), Accepts(TEXT("-op=createrole -kind=team -group=3 -name=\"Field Officer\" -perms=\"manage_members, send_messages\" -rank=5"), Request));
	TestEqual(TEXT("the name keeps its space"), Request.Name, FString(TEXT("Field Officer")));
	TestEqual(TEXT("both permissions are read"), Request.Perms.Num(), 2);
	TestEqual(TEXT("the rank is read"), Request.Rank, 5);
	TestTrue(TEXT("the rank is marked given"), Request.bHasRank);
	TestTrue(TEXT("createrole with only a name parses"), Accepts(TEXT("-op=createrole -kind=team -group=3 -name=Scout"), Request));
	TestTrue(TEXT("a role may have no permissions"), Request.Perms.IsEmpty());
	TestFalse(TEXT("an absent rank is not given"), Request.bHasRank);
	TestTrue(TEXT("a negative rank parses"), Accepts(TEXT("-op=createrole -kind=team -group=3 -name=Scout -rank=-2"), Request));
	TestEqual(TEXT("it keeps its sign"), Request.Rank, -2);

	TestTrue(TEXT("updaterole parses"), Accepts(TEXT("-op=updaterole -kind=channel -group=3 -role=11 -rank=3"), Request));
	TestEqual(TEXT("the role id is read"), Request.RoleId, static_cast<int64>(11));
	TestTrue(TEXT("updaterole takes -noperms alone"), Accepts(TEXT("-op=updaterole -kind=team -group=3 -role=11 -noperms"), Request));
	TestTrue(TEXT("-noperms is read"), Request.bNoPerms);
	TestTrue(TEXT("deleterole parses"), Accepts(TEXT("-op=deleterole -kind=team -group=3 -role=11"), Request));

	TestTrue(TEXT("createrole without -name names -name"), RefusesWith(TEXT("-op=createrole -kind=team -group=3"), TEXT("-name"), TEXT("required")));
	TestTrue(TEXT("a permission outside the group set is refused"), RefusesWith(TEXT("-op=createrole -kind=team -group=3 -name=Scout -perms=\"fly\""), TEXT("-perms"), TEXT("not a group permission")));
	TestTrue(TEXT("a rank that is not a number is refused"), RefusesWith(TEXT("-op=createrole -kind=team -group=3 -name=Scout -rank=high"), TEXT("-rank"), TEXT("whole number")));
	TestTrue(TEXT("-perms and -noperms together are refused"), RefusesWith(TEXT("-op=createrole -kind=team -group=3 -name=Scout -perms=\"send_messages\" -noperms"), TEXT("-perms"), TEXT("not both")));
	TestTrue(TEXT("an unquoted permission list is cut at its comma and refused"), RefusesWith(TEXT("-op=createrole -kind=team -group=3 -name=Scout -perms=send_messages,manage_group"), TEXT("-perms"), TEXT("in quotes")));
	TestTrue(TEXT("updaterole without -role names -role"), RefusesWith(TEXT("-op=updaterole -kind=team -group=3 -rank=1"), TEXT("-role"), TEXT("required")));
	TestTrue(TEXT("updaterole with nothing to change is refused"), RefusesWith(TEXT("-op=updaterole -kind=team -group=3 -role=11"), TEXT("-name"), TEXT("at least one")));
	TestTrue(TEXT("deleterole without -role names -role"), RefusesWith(TEXT("-op=deleterole -kind=team -group=3"), TEXT("-role"), TEXT("required")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesGroupEditsTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesGroupEdits", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesGroupEditsTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	TestTrue(TEXT("updategroup parses"), Accepts(TEXT("-op=updategroup -kind=team -group=3 -name=\"Blue Team\" -description=\"Blues, only\" -policy=INVITE"), Request));
	TestEqual(TEXT("the name keeps its space"), Request.Name, FString(TEXT("Blue Team")));
	TestEqual(TEXT("the description keeps its comma"), Request.Description, FString(TEXT("Blues, only")));
	TestEqual(TEXT("the policy is lower-cased"), Request.Policy, FString(TEXT("invite")));
	TestTrue(TEXT("updategroup takes one field"), Accepts(TEXT("-op=updategroup -kind=channel -group=3 -policy=admin"), Request));
	TestTrue(TEXT("deletegroup parses"), Accepts(TEXT("-op=deletegroup -kind=channel -group=3"), Request));
	TestTrue(TEXT("it is a channel"), Request.bChannel);

	TestTrue(TEXT("updategroup with nothing to change is refused"), RefusesWith(TEXT("-op=updategroup -kind=team -group=3"), TEXT("-name"), TEXT("at least one")));
	TestTrue(TEXT("updategroup with an unknown policy is refused"), RefusesWith(TEXT("-op=updategroup -kind=team -group=3 -policy=closed"), TEXT("-policy"), TEXT("not a membership policy")));
	TestTrue(TEXT("updategroup without -group names -group"), RefusesWith(TEXT("-op=updategroup -kind=team -name=Blue"), TEXT("-group"), TEXT("required")));
	TestTrue(TEXT("deletegroup without -kind names -kind"), RefusesWith(TEXT("-op=deletegroup -group=3"), TEXT("-kind"), TEXT("required")));
	TestTrue(TEXT("deletegroup with a non-numeric -group names -group"), Refuses(TEXT("-op=deletegroup -kind=team -group=red"), TEXT("-group")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesPoliciesTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesPolicies", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesPoliciesTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	TestTrue(TEXT("setteampolicy parses every field"), Accepts(TEXT("-op=setteampolicy -creation=Member -membership=invite -maxmembers=50 -maxgroupsperuser=0"), Request));
	TestFalse(TEXT("it is a team policy"), Request.bChannel);
	TestEqual(TEXT("the creation policy is lower-cased"), Request.Creation, FString(TEXT("member")));
	TestEqual(TEXT("the default membership is read"), Request.Policy, FString(TEXT("invite")));
	TestEqual(TEXT("the member cap is read"), Request.MaxMembers, 50);
	TestEqual(TEXT("0 is a cap given, meaning unlimited"), Request.MaxGroupsPerUser, 0);

	TestTrue(TEXT("setchannelpolicy parses one field"), Accepts(TEXT("-op=setchannelpolicy -maxmembers=0"), Request));
	TestTrue(TEXT("it is a channel policy"), Request.bChannel);
	TestEqual(TEXT("a cap left out is -1, keep the current one"), Request.MaxGroupsPerUser, -1);
	TestTrue(TEXT("the creation policy left out is empty"), Request.Creation.IsEmpty() && Request.Policy.IsEmpty());

	TestTrue(TEXT("a policy change with nothing in it is refused"), RefusesWith(TEXT("-op=setteampolicy"), TEXT("-creation"), TEXT("at least one")));
	TestTrue(TEXT("an unknown creation policy is refused"), RefusesWith(TEXT("-op=setteampolicy -creation=everyone"), TEXT("-creation"), TEXT("admin, member or anyone")));
	TestTrue(TEXT("an unknown membership policy is refused"), RefusesWith(TEXT("-op=setchannelpolicy -membership=closed"), TEXT("-membership"), TEXT("not a membership policy")));
	TestTrue(TEXT("a negative cap is refused"), RefusesWith(TEXT("-op=setteampolicy -maxmembers=-1"), TEXT("-maxmembers"), TEXT("0 or more")));
	TestTrue(TEXT("a cap that is not a number is refused"), RefusesWith(TEXT("-op=setteampolicy -maxgroupsperuser=lots"), TEXT("-maxgroupsperuser"), TEXT("whole number")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletParsesGridDetailTest, "CrowdySDK.CrowdyStudio.Commandlet.ParsesGridDetail", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletParsesGridDetailTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	TestTrue(TEXT("gridlimits parses a key list"), Accepts(TEXT("-op=gridlimits -grid=74637611160064 -keys=\"voxel.edit, voxel.place\""), Request));
	TestEqual(TEXT("the grid id is full width"), Request.GridId, static_cast<int64>(74637611160064));
	TestEqual(TEXT("both keys are read"), Request.Keys.Num(), 2);
	TestFalse(TEXT("naming keys is not -nolimit"), Request.bAllKeys);
	TestTrue(TEXT("gridlimits parses -nolimit"), Accepts(TEXT("-op=gridlimits -grid=5 -nolimit"), Request));
	TestTrue(TEXT("-nolimit is read"), Request.bAllKeys && Request.Keys.IsEmpty());

	TestTrue(TEXT("gridgrants parses"), Accepts(TEXT("-op=gridgrants -grid=5 -group=8"), Request));
	TestEqual(TEXT("the group is read"), Request.GroupId, static_cast<int64>(8));
	TestTrue(TEXT("gridpermissions parses without -user"), Accepts(TEXT("-op=gridpermissions -grid=5"), Request));
	TestEqual(TEXT("an absent -user is 0, the signed-in user"), Request.UserId, static_cast<int64>(0));
	TestTrue(TEXT("gridpermissions reads -user"), Accepts(TEXT("-op=gridpermissions -grid=5 -user=9"), Request));
	TestEqual(TEXT("the user is read"), Request.UserId, static_cast<int64>(9));

	TestTrue(TEXT("gridlimits without -grid names -grid"), RefusesWith(TEXT("-op=gridlimits -nolimit"), TEXT("-grid"), TEXT("required")));
	TestTrue(TEXT("gridlimits with neither -keys nor -nolimit is refused"), RefusesWith(TEXT("-op=gridlimits -grid=5"), TEXT("-keys"), TEXT("-nolimit is required")));
	TestTrue(TEXT("gridlimits with both is refused"), RefusesWith(TEXT("-op=gridlimits -grid=5 -keys=\"a\" -nolimit"), TEXT("-keys"), TEXT("not both")));
	TestTrue(TEXT("an unquoted key list is cut at its comma and refused"), RefusesWith(TEXT("-op=gridlimits -grid=5 -keys=a,b"), TEXT("-keys"), TEXT("in quotes")));
	TestTrue(TEXT("gridgrants without -group names -group"), RefusesWith(TEXT("-op=gridgrants -grid=5"), TEXT("-group"), TEXT("required")));
	TestTrue(TEXT("gridpermissions with a non-numeric -user names -user"), Refuses(TEXT("-op=gridpermissions -grid=5 -user=me"), TEXT("-user")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletUnknownOpPrintsUsageTest, "CrowdySDK.CrowdyStudio.Commandlet.UnknownOpPrintsUsage", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletUnknownOpPrintsUsageTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;
	TestFalse(TEXT("an unknown operation is refused"), UCrowdyStudioCommandlet::ParseRequest(TEXT("-run=CrowdyStudio -op=archive -app=5"), Request, Error));
	TestTrue(TEXT("the refusal names the operation"), Error.Contains(TEXT("archive")));
	TestTrue(TEXT("the refusal carries the usage"), Error.Contains(UCrowdyStudioCommandlet::GetUsage()));

	Error.Reset();
	TestFalse(TEXT("a missing operation is refused"), UCrowdyStudioCommandlet::ParseRequest(TEXT("-run=CrowdyStudio"), Request, Error));
	TestTrue(TEXT("the missing-operation refusal carries the usage"), Error.Contains(UCrowdyStudioCommandlet::GetUsage()));

	const FString Usage = UCrowdyStudioCommandlet::GetUsage();
	for (const TCHAR* Needle : { TEXT("-op=apps"), TEXT("createapp"), TEXT("-org="), TEXT("-name="), TEXT("-slug="), TEXT("-datacenter="), TEXT("-description="), TEXT("sync"), TEXT("-app="), TEXT("-yes"), TEXT("-editoropen"), TEXT("-out="),
		TEXT("features"), TEXT("definefeature"), TEXT("grantfeature"), TEXT("revokefeature"), TEXT("-tier="), TEXT("-key="), TEXT("teams"), TEXT("createteam"), TEXT("channels"), TEXT("createchannel"),
		TEXT("-policy="), TEXT("-memberscansend="), TEXT("grids"), TEXT("-low="), TEXT("-high="), TEXT("creategrid"), TEXT("-corner1="), TEXT("-corner2="), TEXT("grantgrid"), TEXT("revokegrid"),
		TEXT("-grid="), TEXT("-user="), TEXT("-group="), TEXT("-role="), TEXT("-keys="), TEXT("-all"),
		TEXT("groupdetail"), TEXT("addmember"), TEXT("removemember"), TEXT("setroles"), TEXT("-roles="), TEXT("-noroles"), TEXT("createrole"), TEXT("-perms="), TEXT("-noperms"), TEXT("-rank="),
		TEXT("updaterole"), TEXT("deleterole"), TEXT("-kind="), TEXT("updategroup"), TEXT("deletegroup"), TEXT("setteampolicy"), TEXT("setchannelpolicy"), TEXT("-creation="),
		TEXT("-membership="), TEXT("-maxmembers="), TEXT("-maxgroupsperuser="), TEXT("gridlimits"), TEXT("-nolimit"), TEXT("gridgrants"), TEXT("gridpermissions") })
	{
		TestTrue(FString::Printf(TEXT("the usage names %s"), Needle), Usage.Contains(Needle));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletRefusesLikeTheCreateSheetTest, "CrowdySDK.CrowdyStudio.Commandlet.RefusesLikeTheCreateSheet", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletRefusesLikeTheCreateSheetTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;
	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=createapp -org=5 -name=G -slug=g -datacenter=va"), Request, Error);
	const TArray<TSharedPtr<FStudioOrg>> Orgs = { Org(5) };
	const TArray<FStudioDatacenter> Datacenters = { Datacenter(TEXT("or"), true), Datacenter(TEXT("va"), true), Datacenter(TEXT("eu"), false) };

	TestTrue(TEXT("a placeable datacenter in the account's organization is accepted"), UCrowdyStudioCommandlet::FindCreateProblem(Request, Orgs, Datacenters).IsEmpty());

	TestTrue(TEXT("an organization the account is not in is refused, naming -org"),
		UCrowdyStudioCommandlet::FindCreateProblem(Request, { Org(6) }, Datacenters).Contains(TEXT("-org")));

	Request.Datacenter = TEXT("eu");
	TestTrue(TEXT("a datacenter that cannot take an app is refused, naming -datacenter and the choices"),
		UCrowdyStudioCommandlet::FindCreateProblem(Request, Orgs, Datacenters).Contains(TEXT("-datacenter=eu")));
	TestTrue(TEXT("the refusal lists the placeable codes"), UCrowdyStudioCommandlet::FindCreateProblem(Request, Orgs, Datacenters).Contains(TEXT("or, va")));

	Request.Datacenter = TEXT("VA");
	TestFalse(TEXT("a code in the wrong case is refused, as it is sent verbatim"), UCrowdyStudioCommandlet::FindCreateProblem(Request, Orgs, Datacenters).IsEmpty());

	TestFalse(TEXT("a deployment with nothing placeable refuses"), UCrowdyStudioCommandlet::FindCreateProblem(Request, Orgs, { Datacenter(TEXT("eu"), false) }).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletSyncMatchesProjectPageTest, "CrowdySDK.CrowdyStudio.Commandlet.SyncMatchesProjectPage", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletSyncMatchesProjectPageTest::RunTest(const FString& /*Parameters*/)
{
	FStudioApp WithEndpoint;
	WithEndpoint.AppId = 74637611160064;
	WithEndpoint.OrgId = 74637546537472;
	WithEndpoint.GameApiUrl = TEXT("https://game.example.test");

	FStudioApp WithoutEndpoint;
	WithoutEndpoint.AppId = 74637611160065;
	WithoutEndpoint.OrgId = 74637546537473;

	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::AddApp(*Controller, WithEndpoint);
	FCrowdyStudioControllerTestAccess::AddApp(*Controller, WithoutEndpoint);

	for (const FStudioApp* Case : { &WithEndpoint, &WithoutEndpoint })
	{
		// The Sync to project button writes the selected app onto the developer settings; a transient object takes the same
		// write here without saving anything.
		FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, Case->AppId);
		const TSharedPtr<FStudioApp> Selected = Controller->GetSelectedApp();
		if (!TestTrue(TEXT("the controller selects the app"), Selected.IsValid()))
		{
			continue;
		}
		const TStrongObjectPtr<UCrowdySDKDeveloperSettings> Written(NewObject<UCrowdySDKDeveloperSettings>(GetTransientPackage()));
		FCrowdyConfigSync::ApplyAppToObject(*Written, *Selected);
		const FStudioSettingsSnapshot ButtonWrites = FCrowdyConfigSync::ReadSettings(*Written);

		const TSharedPtr<FStudioApp> Found = UCrowdyStudioCommandlet::FindApp(Controller->GetApps(), Case->AppId);
		if (!TestTrue(TEXT("the commandlet finds the app"), Found.IsValid()))
		{
			continue;
		}
		const FStudioSettingsSnapshot Proposed = UCrowdyStudioCommandlet::ProposeSync(*Found);
		TestTrue(TEXT("the commandlet proposes what Sync to project writes"), Proposed == ButtonWrites);
		TestTrue(TEXT("and what the Project page shows"), Proposed == Controller->BuildProposedSettings());
		TestEqual(TEXT("the app id is the app's"), Proposed.AppId, Case->AppId);
		TestEqual(TEXT("the organization id is the app's"), Proposed.OrgId, Case->OrgId);
		TestEqual(TEXT("the shared origin is left to the backend selector"), Proposed.DiscoveryUrl, Controller->GetCurrentSettings().DiscoveryUrl);
	}

	const FStudioSettingsSnapshot Proposed = UCrowdyStudioCommandlet::ProposeSync(WithEndpoint);
	TestEqual(TEXT("the HTTP endpoint is the app's with the Game API path"), Proposed.GameApiHttpUrl, FString(TEXT("https://game.example.test/graphql")));
	TestEqual(TEXT("the WS endpoint is the same URL with a ws scheme"), Proposed.GameApiWsUrl, FString(TEXT("wss://game.example.test/graphql")));

	const FStudioSettingsSnapshot Kept = UCrowdyStudioCommandlet::ProposeSync(WithoutEndpoint);
	TestEqual(TEXT("an app with no game endpoint leaves the HTTP endpoint as it is"), Kept.GameApiHttpUrl, Controller->GetCurrentSettings().GameApiHttpUrl);
	TestEqual(TEXT("and the WS endpoint"), Kept.GameApiWsUrl, Controller->GetCurrentSettings().GameApiWsUrl);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletRefusesSyncWhileEditorRunsTest, "CrowdySDK.CrowdyStudio.Commandlet.RefusesSyncWhileEditorRuns", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletRefusesSyncWhileEditorRunsTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	FString Error;
	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=sync -app=77 -yes"), Request, Error);
	TestFalse(TEXT("an editor is not assumed open"), Request.bEditorOpen);

	const FString Refusal = UCrowdyStudioCommandlet::FindSyncWriteProblem(Request, /*bEditorRunning*/ true);
	TestFalse(TEXT("a running editor refuses the write"), Refusal.IsEmpty());
	TestTrue(TEXT("the refusal names the way past it"), Refusal.Contains(TEXT("-editoropen")));
	TestTrue(TEXT("no running editor, no refusal"), UCrowdyStudioCommandlet::FindSyncWriteProblem(Request, /*bEditorRunning*/ false).IsEmpty());

	UCrowdyStudioCommandlet::ParseRequest(TEXT("-op=sync -app=77 -yes -editoropen"), Request, Error);
	TestTrue(TEXT("-editoropen is read"), Request.bEditorOpen);
	TestTrue(TEXT("-editoropen writes anyway"), UCrowdyStudioCommandlet::FindSyncWriteProblem(Request, /*bEditorRunning*/ true).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioControllerClassifiesCredentialRejectionTest, "CrowdySDK.CrowdyStudio.Controller.ClassifiesCredentialRejection", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioControllerClassifiesCredentialRejectionTest::RunTest(const FString& /*Parameters*/)
{
	using Access = FCrowdyStudioControllerTestAccess;
	TestTrue(TEXT("HTTP 401 is a rejected token"), Access::IsCredentialRejection(TEXT("Server returned HTTP 401"), FString()));
	TestTrue(TEXT("HTTP 403 is a rejected token"), Access::IsCredentialRejection(TEXT("Server returned HTTP 403"), FString()));
	TestTrue(TEXT("the UNAUTHENTICATED code is a rejected token"), Access::IsCredentialRejection(TEXT("You must sign in"), TEXT("UNAUTHENTICATED")));

	TestFalse(TEXT("HTTP 500 says nothing about the token"), Access::IsCredentialRejection(TEXT("Server returned HTTP 500"), FString()));
	TestFalse(TEXT("HTTP 503 says nothing about the token"), Access::IsCredentialRejection(TEXT("Server returned HTTP 503"), FString()));
	TestFalse(TEXT("a network error says nothing about the token"), Access::IsCredentialRejection(TEXT("Could not connect to the server"), FString()));
	TestFalse(TEXT("a timeout says nothing about the token"), Access::IsCredentialRejection(TEXT("The request timed out"), FString()));
	TestFalse(TEXT("another server code says nothing about the token"), Access::IsCredentialRejection(TEXT("Something broke"), TEXT("INTERNAL_ERROR")));
	TestFalse(TEXT("a cancellation says nothing about the token"), Access::IsCredentialRejection(FCrowdyCppClient::CanceledErrorMessage(), FString()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletFindsAnAppTest, "CrowdySDK.CrowdyStudio.Commandlet.FindsAnApp", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletFindsAnAppTest::RunTest(const FString& /*Parameters*/)
{
	TArray<TSharedPtr<FStudioApp>> Apps;
	Apps.Add(nullptr);
	Apps.Add(MakeShared<FStudioApp>());
	Apps.Last()->AppId = 74637611160064;

	TestTrue(TEXT("an app is found by its full-width id"), UCrowdyStudioCommandlet::FindApp(Apps, 74637611160064) == Apps.Last());
	TestFalse(TEXT("another id is not found"), UCrowdyStudioCommandlet::FindApp(Apps, 74637611160065).IsValid());
	return true;
}

namespace
{
	FStudioGroup ExistingGroup(int64 GroupId, const TCHAR* Name, const TCHAR* Description, const TCHAR* Policy)
	{
		FStudioGroup Group;
		Group.GroupId = GroupId;
		Group.Name = Name;
		Group.Description = Description;
		Group.MembershipPolicy = Policy;
		return Group;
	}

	TSharedPtr<FStudioGroup> ListedGroup(int64 GroupId)
	{
		return MakeShared<FStudioGroup>(ExistingGroup(GroupId, TEXT("Listed"), TEXT(""), TEXT("open")));
	}

	FStudioGroupRole ExistingRole(int64 RoleId, const TCHAR* Name, int32 Rank, const TArray<FString>& Permissions)
	{
		FStudioGroupRole Role;
		Role.GroupRoleId = RoleId;
		Role.RoleName = Name;
		Role.Rank = Rank;
		Role.Permissions = Permissions;
		return Role;
	}

	TSharedPtr<FStudioGridGroupGrant> GroupGrantOf(const TCHAR* Key, int64 RoleId)
	{
		TSharedPtr<FStudioGridGroupGrant> Grant = MakeShared<FStudioGridGroupGrant>();
		Grant->PermissionKey = Key;
		Grant->bHasRole = RoleId != 0;
		Grant->GroupRoleId = RoleId;
		return Grant;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioControllerSelectsAnAppWithoutRememberingItTest, "CrowdySDK.CrowdyStudio.Controller.SelectsAnAppWithoutRememberingIt", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioControllerSelectsAnAppWithoutRememberingItTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	UCrowdyStudioUserSettings* User = GetMutableDefault<UCrowdyStudioUserSettings>();
	const int64 RememberedOrg = User->LastOrgId;
	const int64 RememberedApp = User->LastAppId;

	FStudioApp App;
	App.AppId = RememberedApp + 1;
	App.OrgId = RememberedOrg + 1;
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::AddApp(*Controller, App);
	FCrowdyStudioControllerTestAccess::InstallApiClient(*Controller, Client.ToSharedRef(), TEXT("session-token"), FString());

	Controller->SelectApp(App.AppId, false);
	Client->Poll();

	TestEqual(TEXT("the controller selects the app"), Controller->GetSelectedAppId(), App.AppId);
	TestEqual(TEXT("and the app's organization"), Controller->GetSelectedOrgId(), App.OrgId);
	TestEqual(TEXT("a selection made without remembering leaves the remembered app alone"), User->LastAppId, RememberedApp);
	TestEqual(TEXT("and the remembered organization"), User->LastOrgId, RememberedOrg);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioControllerStartsWithNoSelectionTest, "CrowdySDK.CrowdyStudio.Controller.StartsWithNoSelection", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioControllerStartsWithNoSelectionTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	TestEqual(TEXT("a controller that was not initialized has no app, so restoring a sign-in mints no token for a remembered one"), Controller->GetSelectedAppId(), static_cast<int64>(0));
	TestEqual(TEXT("and no organization"), Controller->GetSelectedOrgId(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletReadsBackADefinedFeatureTest, "CrowdySDK.CrowdyStudio.Commandlet.ReadsBackADefinedFeature", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletReadsBackADefinedFeatureTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	Accepts(TEXT("-op=definefeature -key=vip -description=\"Gold pass\""), Request);
	const TSharedPtr<FStudioAppFeature> Listed = Feature(TEXT("vip"));
	Listed->Description = TEXT("Gold pass");
	TestTrue(TEXT("a feature listed with the asked description reads back"), UCrowdyStudioCommandlet::FindDefineProblem(Request, { nullptr, Listed }).IsEmpty());

	Listed->Description = TEXT("Old text");
	const FString Stale = UCrowdyStudioCommandlet::FindDefineProblem(Request, { Listed });
	TestTrue(TEXT("a description that did not change is reported with both texts"), Stale.Contains(TEXT("description")) && Stale.Contains(TEXT("Old text")) && Stale.Contains(TEXT("Gold pass")));

	TestTrue(TEXT("a feature missing from the list is reported"), UCrowdyStudioCommandlet::FindDefineProblem(Request, { Feature(TEXT("other")) }).Contains(TEXT("not in the app's feature list")));

	Accepts(TEXT("-op=definefeature -key=vip"), Request);
	TestTrue(TEXT("without -description any description reads back"), UCrowdyStudioCommandlet::FindDefineProblem(Request, { Listed }).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletReportsADifferingGroupTest, "CrowdySDK.CrowdyStudio.Commandlet.ReportsADifferingGroup", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletReportsADifferingGroupTest::RunTest(const FString& /*Parameters*/)
{
	const FStudioGroup Existing = ExistingGroup(7, TEXT("Red"), TEXT("Reds"), TEXT("invite"));
	FCrowdyStudioCommandRequest Request;

	Accepts(TEXT("-op=createteam -name=Red -description=Reds -policy=invite"), Request);
	TestTrue(TEXT("a team with the same description and policy matches"), UCrowdyStudioCommandlet::FindGroupDifference(Request, Existing).IsEmpty());

	Accepts(TEXT("-op=createteam -name=Red -description=Reds -policy=open"), Request);
	const FString Policy = UCrowdyStudioCommandlet::FindGroupDifference(Request, Existing);
	TestTrue(TEXT("another policy is named with both values"), Policy.Contains(TEXT("membership policy is invite, not open")));
	TestFalse(TEXT("and the matching description is not"), Policy.Contains(TEXT("description")));
	TestTrue(TEXT("it points at updategroup for this team"), Policy.Contains(TEXT("-op=updategroup -kind=team -group=7")));

	Accepts(TEXT("-op=createteam -name=Red -description=Blues -policy=invite"), Request);
	TestTrue(TEXT("another description is named with both values"), UCrowdyStudioCommandlet::FindGroupDifference(Request, Existing).Contains(TEXT("description is \"Reds\", not \"Blues\"")));

	Accepts(TEXT("-op=createteam -name=Red -description=Blues -policy=open"), Request);
	const FString Both = UCrowdyStudioCommandlet::FindGroupDifference(Request, Existing);
	TestTrue(TEXT("both differences are named"), Both.Contains(TEXT("description is")) && Both.Contains(TEXT("membership policy is")));

	Accepts(TEXT("-op=createteam -name=Red"), Request);
	TestTrue(TEXT("a team left to the default policy is asking for open, which an invite team is not"), UCrowdyStudioCommandlet::FindGroupDifference(Request, Existing).Contains(TEXT("membership policy is invite, not open")));

	Accepts(TEXT("-op=createchannel -name=Red"), Request);
	TestTrue(TEXT("a channel that gave neither description nor policy asks for nothing to compare"), UCrowdyStudioCommandlet::FindGroupDifference(Request, Existing).IsEmpty());
	Accepts(TEXT("-op=createchannel -name=Red -policy=admin"), Request);
	TestTrue(TEXT("a channel names itself in the updategroup pointer"), UCrowdyStudioCommandlet::FindGroupDifference(Request, Existing).Contains(TEXT("-op=updategroup -kind=channel -group=7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletReportsADifferingRoleTest, "CrowdySDK.CrowdyStudio.Commandlet.ReportsADifferingRole", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletReportsADifferingRoleTest::RunTest(const FString& /*Parameters*/)
{
	const FStudioGroupRole Existing = ExistingRole(3, TEXT("Scout"), 2, { TEXT("send_messages"), TEXT("manage_members") });
	FCrowdyStudioCommandRequest Request;

	Accepts(TEXT("-op=createrole -kind=team -group=9 -name=Scout -perms=\"manage_members,send_messages\" -rank=2"), Request);
	TestTrue(TEXT("the same permissions in another order and the same rank match"), UCrowdyStudioCommandlet::FindRoleDifference(Request, Existing).IsEmpty());

	Accepts(TEXT("-op=createrole -kind=team -group=9 -name=Scout"), Request);
	TestTrue(TEXT("a role that gave no permissions and no rank asks for nothing to compare"), UCrowdyStudioCommandlet::FindRoleDifference(Request, Existing).IsEmpty());

	Accepts(TEXT("-op=createrole -kind=team -group=9 -name=Scout -perms=\"send_messages\""), Request);
	const FString Perms = UCrowdyStudioCommandlet::FindRoleDifference(Request, Existing);
	TestTrue(TEXT("other permissions are named with both sets"), Perms.Contains(TEXT("permissions are \"send_messages, manage_members\", not \"send_messages\"")));
	TestFalse(TEXT("and the matching rank is not"), Perms.Contains(TEXT("rank")));
	TestTrue(TEXT("it points at updaterole for this role"), Perms.Contains(TEXT("-op=updaterole -kind=team -group=9 -role=3")));

	Accepts(TEXT("-op=createrole -kind=channel -group=9 -name=Scout -rank=5"), Request);
	const FString Rank = UCrowdyStudioCommandlet::FindRoleDifference(Request, Existing);
	TestTrue(TEXT("another rank is named with both values"), Rank.Contains(TEXT("rank is 2, not 5")));
	TestTrue(TEXT("a channel role names its kind"), Rank.Contains(TEXT("-kind=channel")));

	Accepts(TEXT("-op=createrole -kind=team -group=9 -name=Scout -noperms"), Request);
	TestTrue(TEXT("-noperms against a role that holds permissions is a difference"), UCrowdyStudioCommandlet::FindRoleDifference(Request, Existing).Contains(TEXT("permissions are")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletRefusesAGridGroupOfNeitherKindTest, "CrowdySDK.CrowdyStudio.Commandlet.RefusesAGridGroupOfNeitherKind", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletRefusesAGridGroupOfNeitherKindTest::RunTest(const FString& /*Parameters*/)
{
	const TArray<TSharedPtr<FStudioGroup>> Teams = { nullptr, ListedGroup(5) };
	const TArray<TSharedPtr<FStudioGroup>> Channels = { ListedGroup(6) };
	TestTrue(TEXT("a team is accepted"), UCrowdyStudioCommandlet::FindGridGroupProblem(5, Teams, Channels).IsEmpty());
	TestTrue(TEXT("a channel is accepted"), UCrowdyStudioCommandlet::FindGridGroupProblem(6, Teams, Channels).IsEmpty());

	const FString Refusal = UCrowdyStudioCommandlet::FindGridGroupProblem(7, Teams, Channels);
	TestTrue(TEXT("a group in neither list is refused, naming -group"), Refusal.Contains(TEXT("-group=7")) && Refusal.Contains(TEXT("neither a team nor a channel")));
	TestFalse(TEXT("with nothing listed nothing is accepted"), UCrowdyStudioCommandlet::FindGridGroupProblem(5, TArray<TSharedPtr<FStudioGroup>>(), TArray<TSharedPtr<FStudioGroup>>()).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletFindsAnUnrevokedUserKeyTest, "CrowdySDK.CrowdyStudio.Commandlet.FindsAnUnrevokedUserKey", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletFindsAnUnrevokedUserKeyTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	Accepts(TEXT("-op=revokegrid -grid=4 -user=9 -keys=\"a,b\""), Request);

	TestTrue(TEXT("keys that are gone leave nothing to report"), UCrowdyStudioCommandlet::FindUnrevokedUserKey(Request, { TEXT("c") }).IsEmpty());
	const FString Held = UCrowdyStudioCommandlet::FindUnrevokedUserKey(Request, { TEXT("c"), TEXT("b") });
	TestTrue(TEXT("a revoked key the user still holds is named with the user and the grid"), Held.Contains(TEXT("-keys has b")) && Held.Contains(TEXT("user 9")) && Held.Contains(TEXT("grid 4")));
	TestTrue(TEXT("a key in another case is another key"), UCrowdyStudioCommandlet::FindUnrevokedUserKey(Request, { TEXT("B") }).IsEmpty());

	Accepts(TEXT("-op=revokegrid -grid=4 -user=9 -all"), Request);
	TestTrue(TEXT("-all names no key, so it finds none"), UCrowdyStudioCommandlet::FindUnrevokedUserKey(Request, { TEXT("a") }).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCommandletFindsAnUnrevokedGroupGrantTest, "CrowdySDK.CrowdyStudio.Commandlet.FindsAnUnrevokedGroupGrant", CrowdyStudioCommandletTestFlags)

bool FCrowdyStudioCommandletFindsAnUnrevokedGroupGrantTest::RunTest(const FString& /*Parameters*/)
{
	FCrowdyStudioCommandRequest Request;
	const TArray<TSharedPtr<FStudioGridGroupGrant>> Grants = { nullptr, GroupGrantOf(TEXT("a"), 2), GroupGrantOf(TEXT("b"), 0) };

	Accepts(TEXT("-op=revokegrid -grid=4 -group=8 -keys=\"a\""), Request);
	TestTrue(TEXT("a whole-group revoke is not failed by a role's grant or an unnamed key"), UCrowdyStudioCommandlet::FindUnrevokedGroupGrant(Request, Grants).IsEmpty());

	Accepts(TEXT("-op=revokegrid -grid=4 -group=8 -keys=\"b\""), Request);
	const FString Group = UCrowdyStudioCommandlet::FindUnrevokedGroupGrant(Request, Grants);
	TestTrue(TEXT("a named whole-group grant still there is reported with the group and the grid"), Group.Contains(TEXT("group 8 still holds b")) && Group.Contains(TEXT("grid 4")));

	Accepts(TEXT("-op=revokegrid -grid=4 -group=8 -role=2 -keys=\"a\""), Request);
	TestTrue(TEXT("a role revoke reports that role's grant still there"), UCrowdyStudioCommandlet::FindUnrevokedGroupGrant(Request, Grants).Contains(TEXT("still holds a")));
	Accepts(TEXT("-op=revokegrid -grid=4 -group=8 -role=3 -keys=\"a\""), Request);
	TestTrue(TEXT("another role's grant is not this revoke's"), UCrowdyStudioCommandlet::FindUnrevokedGroupGrant(Request, Grants).IsEmpty());

	Accepts(TEXT("-op=revokegrid -grid=4 -group=8 -all"), Request);
	TestTrue(TEXT("-all reports a whole-group grant still there"), UCrowdyStudioCommandlet::FindUnrevokedGroupGrant(Request, Grants).Contains(TEXT("still holds b")));
	TestTrue(TEXT("-all with nothing left in its scope reports nothing"), UCrowdyStudioCommandlet::FindUnrevokedGroupGrant(Request, { GroupGrantOf(TEXT("a"), 2) }).IsEmpty());
	return true;
}

#endif
