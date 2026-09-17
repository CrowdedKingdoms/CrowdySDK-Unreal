#include "GameModel/CrowdyPreSeedPlan.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
	constexpr EAutomationTestFlags PreSeedPlanTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyContainerManifestRow PreSeedTestManifestRow(const TCHAR* Type, const TCHAR* Key, const TCHAR* Name)
	{
		FCrowdyContainerManifestRow Row;
		Row.TypeName = Type;
		Row.BindingKey = Key;
		Row.DisplayName = Name;
		return Row;
	}

	FCrowdyPreSeedServerRow PreSeedTestServerRow(const TCHAR* Type, const TCHAR* Key, const TCHAR* Id, const TCHAR* Session = TEXT(""))
	{
		FCrowdyPreSeedServerRow Row;
		Row.TypeName = Type;
		Row.BindingKey = Key;
		Row.ContainerId = Id;
		Row.SessionId = Session;
		return Row;
	}

	int32 PreSeedTestCount(const FCrowdyPreSeedReport& Report, ECrowdyPreSeedRowKind Kind)
	{
		int32 Count = 0;
		for (const FCrowdyPreSeedRow& Row : Report.Rows)
		{
			Count += Row.Kind == Kind ? 1 : 0;
		}
		return Count;
	}

	FStudioContainerType PreSeedTestType(const TCHAR* Name, const TCHAR* InstantiableBy, const TCHAR* Scope = TEXT("session"),
		const TCHAR* BindPolicyJson = TEXT(""))
	{
		FStudioContainerType Type;
		Type.TypeName = Name;
		Type.InstantiableBy = InstantiableBy;
		Type.Scope = Scope;
		Type.BindPolicyJson = BindPolicyJson;
		return Type;
	}

	FCrowdyPreSeedRow PreSeedTestPlannedRow(const TCHAR* Type, const TCHAR* Key, bool bSeedEligible, bool bAppScoped = false,
		ECrowdyPreSeedRowKind Kind = ECrowdyPreSeedRowKind::Create)
	{
		FCrowdyPreSeedRow Row;
		Row.Kind = Kind;
		Row.TypeName = Type;
		Row.BindingKey = Key;
		Row.DisplayName = Key;
		Row.bSeedEligible = bSeedEligible;
		Row.bAppScoped = bAppScoped;
		return Row;
	}
}

// A manifest row the server holds is Existing with its id, one it lacks is Create, and a server row nothing claims
// is an Orphan. A row in another scope is neither matched nor reported.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPreSeedPlanDiffTest,
	"CrowdySDK.CrowdyStudio.PreSeedPlanDiff", PreSeedPlanTestFlags)
bool FCrowdyPreSeedPlanDiffTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyContainerManifestRow> Manifest;
	Manifest.Add(PreSeedTestManifestRow(TEXT("Camp"), TEXT("k1"), TEXT("Camp A")));
	Manifest.Add(PreSeedTestManifestRow(TEXT("Camp"), TEXT("k2"), TEXT("Camp B")));
	Manifest.Add(PreSeedTestManifestRow(TEXT("Turret"), TEXT("k1"), TEXT("Turret A")));

	TArray<FCrowdyPreSeedServerRow> Server;
	Server.Add(PreSeedTestServerRow(TEXT("Camp"), TEXT("k1"), TEXT("id-camp-1")));
	Server.Add(PreSeedTestServerRow(TEXT("Camp"), TEXT("k9"), TEXT("id-camp-orphan")));
	Server.Add(PreSeedTestServerRow(TEXT("Camp"), TEXT("k2"), TEXT("id-other-session"), TEXT("sess-7")));
	Server.Add(PreSeedTestServerRow(TEXT("Loot"), TEXT("k1"), TEXT("id-other-type")));

	FCrowdyPreSeedReport Report;
	FCrowdyPreSeedPlan::Diff(Manifest, Server, FString(), Report);

	TestTrue(TEXT("valid"), Report.bValid);
	TestEqual(TEXT("two to create"), Report.ToCreate, 2);
	TestEqual(TEXT("one existing"), Report.Existing, 1);
	TestEqual(TEXT("one orphan: the other session's row and the other type's row are not this plan's"), Report.Orphans, 1);
	TestEqual(TEXT("create rows"), PreSeedTestCount(Report, ECrowdyPreSeedRowKind::Create), 2);
	TestEqual(TEXT("existing rows"), PreSeedTestCount(Report, ECrowdyPreSeedRowKind::Existing), 1);
	TestEqual(TEXT("orphan rows"), PreSeedTestCount(Report, ECrowdyPreSeedRowKind::Orphan), 1);

	const FCrowdyPreSeedRow* Existing = Report.Rows.FindByPredicate([](const FCrowdyPreSeedRow& Row)
	{
		return Row.Kind == ECrowdyPreSeedRowKind::Existing;
	});
	if (TestNotNull(TEXT("existing row"), Existing))
	{
		TestEqual(TEXT("existing row carries the server id"), Existing->ContainerId, FString(TEXT("id-camp-1")));
	}
	TestEqual(TEXT("no warnings"), Report.Warnings.Num(), 0);
	return true;
}

// Scoped to a session, only that session's rows count; a duplicate manifest row is one Create and a warning.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPreSeedPlanScopeAndDuplicatesTest,
	"CrowdySDK.CrowdyStudio.PreSeedPlanScopeAndDuplicates", PreSeedPlanTestFlags)
bool FCrowdyPreSeedPlanScopeAndDuplicatesTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyContainerManifestRow> Manifest;
	Manifest.Add(PreSeedTestManifestRow(TEXT("Camp"), TEXT("k1"), TEXT("Camp A")));
	Manifest.Add(PreSeedTestManifestRow(TEXT("Camp"), TEXT("k1"), TEXT("Camp A copy")));

	TArray<FCrowdyPreSeedServerRow> Server;
	Server.Add(PreSeedTestServerRow(TEXT("Camp"), TEXT("k1"), TEXT("id-app-global")));
	Server.Add(PreSeedTestServerRow(TEXT("Camp"), TEXT("k1"), TEXT("id-in-session"), TEXT("sess-7")));

	FCrowdyPreSeedReport Report;
	FCrowdyPreSeedPlan::Diff(Manifest, Server, TEXT("sess-7"), Report);
	TestEqual(TEXT("the session's row matches"), Report.Existing, 1);
	TestEqual(TEXT("nothing to create"), Report.ToCreate, 0);
	TestEqual(TEXT("the app-global row is not an orphan of this scope"), Report.Orphans, 0);
	TestEqual(TEXT("one duplicate warning"), Report.Warnings.Num(), 1);
	TestEqual(TEXT("one planned row"), Report.Rows.Num(), 1);

	const TArray<FString> Types = FCrowdyPreSeedPlan::DistinctTypes(Manifest);
	TestEqual(TEXT("one distinct type"), Types.Num(), 1);
	return true;
}

// Types come out in first-seen order and exact case; the report text marks each row kind and names orphans once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPreSeedPlanTextTest,
	"CrowdySDK.CrowdyStudio.PreSeedPlanText", PreSeedPlanTestFlags)
bool FCrowdyPreSeedPlanTextTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyContainerManifestRow> Manifest;
	Manifest.Add(PreSeedTestManifestRow(TEXT("Turret"), TEXT("k1"), TEXT("T")));
	Manifest.Add(PreSeedTestManifestRow(TEXT("camp"), TEXT("k1"), TEXT("c")));
	Manifest.Add(PreSeedTestManifestRow(TEXT("Camp"), TEXT("k2"), TEXT("C")));
	Manifest.Add(PreSeedTestManifestRow(TEXT("Turret"), TEXT("k3"), TEXT("T3")));

	const TArray<FString> Types = FCrowdyPreSeedPlan::DistinctTypes(Manifest);
	TestEqual(TEXT("three distinct types, case exact"), Types.Num(), 3);
	if (Types.Num() == 3)
	{
		TestEqual(TEXT("first seen first"), Types[0], FString(TEXT("Turret")));
		TestEqual(TEXT("then camp"), Types[1], FString(TEXT("camp")));
		TestEqual(TEXT("then Camp"), Types[2], FString(TEXT("Camp")));
	}

	TArray<FCrowdyPreSeedServerRow> Server;
	Server.Add(PreSeedTestServerRow(TEXT("Turret"), TEXT("k1"), TEXT("id-t1")));
	Server.Add(PreSeedTestServerRow(TEXT("Turret"), TEXT("k7"), TEXT("id-orphan")));
	FCrowdyPreSeedReport Report;
	FCrowdyPreSeedPlan::Diff(Manifest, Server, FString(), Report);
	Report.MapPackage = TEXT("/Game/Maps/L_Test");

	const FString Text = FCrowdyPreSeedPlan::BuildReportText(Report);
	TestTrue(TEXT("an existing row is marked = with its id"), Text.Contains(TEXT("= Turret k1 T [id-t1]")));
	TestTrue(TEXT("a create row is marked +"), Text.Contains(TEXT("+ Camp k2 C")));
	TestTrue(TEXT("an orphan is marked ?"), Text.Contains(TEXT("? Turret k7")));
	TestTrue(TEXT("the orphan footer appears"), Text.Contains(TEXT("no placement in the map")));
	TestEqual(TEXT("the count line"), FCrowdyPreSeedPlan::BuildCountLine(Report), FString(TEXT("Plan: 3 to create, 1 existing, 1 orphan(s).")));
	return true;
}

// The server seeds keyed rows only on an admin-instantiable type or one carrying a bind policy; a plain member type
// is ensured one row at a time. Words compare without case, as the server's enum words arrive lowercase.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPreSeedPlanSeedEligibilityTest,
	"CrowdySDK.CrowdyStudio.PreSeedPlanSeedEligibility", PreSeedPlanTestFlags)
bool FCrowdyPreSeedPlanSeedEligibilityTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("admin type"), FCrowdyPreSeedPlan::IsSeedEligible(PreSeedTestType(TEXT("A"), TEXT("admin"))));
	TestTrue(TEXT("admin type, any case"), FCrowdyPreSeedPlan::IsSeedEligible(PreSeedTestType(TEXT("A"), TEXT("Admin"))));
	TestTrue(TEXT("member type with a bind policy"),
		FCrowdyPreSeedPlan::IsSeedEligible(PreSeedTestType(TEXT("B"), TEXT("member"), TEXT("session"), TEXT("{\"allow\":\"admin\"}"))));
	TestFalse(TEXT("member type without a policy"), FCrowdyPreSeedPlan::IsSeedEligible(PreSeedTestType(TEXT("C"), TEXT("member"))));
	TestFalse(TEXT("owner type without a policy"), FCrowdyPreSeedPlan::IsSeedEligible(PreSeedTestType(TEXT("D"), TEXT("owner"))));
	TestFalse(TEXT("an older server reporting no instantiableBy"), FCrowdyPreSeedPlan::IsSeedEligible(PreSeedTestType(TEXT("E"), TEXT(""))));

	TestTrue(TEXT("app scope"), FCrowdyPreSeedPlan::IsAppScoped(PreSeedTestType(TEXT("A"), TEXT("admin"), TEXT("app"))));
	TestFalse(TEXT("session scope"), FCrowdyPreSeedPlan::IsAppScoped(PreSeedTestType(TEXT("A"), TEXT("admin"), TEXT("session"))));
	TestFalse(TEXT("a default-constructed type is session-scoped"), FCrowdyPreSeedPlan::IsAppScoped(FStudioContainerType()));
	return true;
}

// With the server's types, an app-scoped type's rows match on an empty session whatever scope was picked, and every
// planned row records its type's scope and seed path; the report text names the app-scoped group once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPreSeedPlanAppScopedTypeTest,
	"CrowdySDK.CrowdyStudio.PreSeedPlanAppScopedType", PreSeedPlanTestFlags)
bool FCrowdyPreSeedPlanAppScopedTypeTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyContainerManifestRow> Manifest;
	Manifest.Add(PreSeedTestManifestRow(TEXT("Health"), TEXT("k1"), TEXT("H1")));
	Manifest.Add(PreSeedTestManifestRow(TEXT("Landmark"), TEXT("k1"), TEXT("L1")));
	Manifest.Add(PreSeedTestManifestRow(TEXT("Landmark"), TEXT("k2"), TEXT("L2")));

	TArray<FCrowdyPreSeedServerRow> Server;
	Server.Add(PreSeedTestServerRow(TEXT("Health"), TEXT("k1"), TEXT("id-h1"), TEXT("sess-7")));
	Server.Add(PreSeedTestServerRow(TEXT("Landmark"), TEXT("k1"), TEXT("id-l1")));
	Server.Add(PreSeedTestServerRow(TEXT("Landmark"), TEXT("k9"), TEXT("id-l-orphan")));

	const TArray<FStudioContainerType> Types = {
		PreSeedTestType(TEXT("Health"), TEXT("member")),
		PreSeedTestType(TEXT("Landmark"), TEXT("admin"), TEXT("app")),
	};
	FCrowdyPreSeedReport Report;
	FCrowdyPreSeedPlan::Diff(Manifest, Server, TEXT("sess-7"), Report, Types);

	TestEqual(TEXT("the session row and the app-global row both match"), Report.Existing, 2);
	TestEqual(TEXT("one to create"), Report.ToCreate, 1);
	TestEqual(TEXT("the app-global row nobody claims is an orphan of this plan"), Report.Orphans, 1);
	for (const FCrowdyPreSeedRow& Row : Report.Rows)
	{
		const bool bLandmark = Row.TypeName == TEXT("Landmark");
		TestEqual(*FString::Printf(TEXT("%s %s app-scoped"), *Row.TypeName, *Row.BindingKey), Row.bAppScoped, bLandmark);
		if (Row.Kind != ECrowdyPreSeedRowKind::Orphan)
		{
			TestEqual(*FString::Printf(TEXT("%s %s seed-eligible"), *Row.TypeName, *Row.BindingKey), Row.bSeedEligible, bLandmark);
		}
	}
	const FString Text = FCrowdyPreSeedPlan::BuildReportText(Report);
	TestTrue(TEXT("the app-scoped group is named"), Text.Contains(TEXT("Landmark (app-scoped, applied app-wide)")));
	TestFalse(TEXT("the session type is not"), Text.Contains(TEXT("Health (app-scoped")));

	// Without the types every row is a plain session row, as before.
	FCrowdyPreSeedReport Untyped;
	FCrowdyPreSeedPlan::Diff(Manifest, Server, TEXT("sess-7"), Untyped);
	TestEqual(TEXT("untyped: only the session row matches"), Untyped.Existing, 1);
	TestEqual(TEXT("untyped: no row is app-scoped or seed-eligible"), Untyped.Rows.ContainsByPredicate(
		[](const FCrowdyPreSeedRow& Row) { return Row.bAppScoped || Row.bSeedEligible; }), false);
	return true;
}

// Seed-eligible Create rows are batched in row order, at most MaxPerBatch each, session and app rows never sharing a
// batch; the rest go to the ensure list. Existing and Orphan rows are left alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPreSeedPlanSplitCreateRowsTest,
	"CrowdySDK.CrowdyStudio.PreSeedPlanSplitCreateRows", PreSeedPlanTestFlags)
bool FCrowdyPreSeedPlanSplitCreateRowsTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyPreSeedRow> Rows;
	for (int32 Index = 0; Index < 1003; ++Index)
	{
		Rows.Add(PreSeedTestPlannedRow(TEXT("Seedable"), *LexToString(Index), /*bSeedEligible*/ true));
	}
	Rows.Add(PreSeedTestPlannedRow(TEXT("Plain"), TEXT("p1"), /*bSeedEligible*/ false));          // 1003
	Rows.Add(PreSeedTestPlannedRow(TEXT("Landmark"), TEXT("l1"), true, /*bAppScoped*/ true));   // 1004
	Rows.Add(PreSeedTestPlannedRow(TEXT("Seedable"), TEXT("x"), true, false, ECrowdyPreSeedRowKind::Existing)); // 1005
	Rows.Add(PreSeedTestPlannedRow(TEXT("Seedable"), TEXT("y"), true));                          // 1006
	Rows.Add(PreSeedTestPlannedRow(TEXT("Plain"), TEXT("p2"), false));                          // 1007

	TArray<FCrowdyPreSeedBatch> Batches;
	TArray<int32> Ensure;
	FCrowdyPreSeedPlan::SplitCreateRows(Rows, 1000, Batches, Ensure);

	TestEqual(TEXT("three batches: two session, one app"), Batches.Num(), 3);
	if (Batches.Num() == 3)
	{
		TestEqual(TEXT("the first session batch is full"), Batches[0].RowIndices.Num(), 1000);
		TestFalse(TEXT("session batch"), Batches[0].bAppScoped);
		TestEqual(TEXT("first index"), Batches[0].RowIndices[0], 0);
		TestEqual(TEXT("last index of the full batch"), Batches[0].RowIndices[999], 999);
		TestEqual(TEXT("the second session batch holds the overflow in order"), Batches[1].RowIndices, TArray<int32>({ 1000, 1001, 1002, 1006 }));
		TestFalse(TEXT("still session"), Batches[1].bAppScoped);
		TestEqual(TEXT("the app batch holds the app row alone"), Batches[2].RowIndices, TArray<int32>({ 1004 }));
		TestTrue(TEXT("app batch"), Batches[2].bAppScoped);
	}
	TestEqual(TEXT("plain rows are ensured, in order"), Ensure, TArray<int32>({ 1003, 1007 }));
	return true;
}

// The seed variables: appId a JSON string, no sessionId for an app-scoped batch, containers an array carrying the row
// index as a string tempId with the type, name and key, and nothing else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPreSeedPlanSeedVariablesTest,
	"CrowdySDK.CrowdyStudio.PreSeedPlanSeedVariables", PreSeedPlanTestFlags)
bool FCrowdyPreSeedPlanSeedVariablesTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyPreSeedRow> Rows;
	Rows.Add(PreSeedTestPlannedRow(TEXT("Plain"), TEXT("p1"), false));
	Rows.Add(PreSeedTestPlannedRow(TEXT("Landmark"), TEXT("l1"), true, true));
	Rows.Add(PreSeedTestPlannedRow(TEXT("Landmark"), TEXT("l2"), true, true));
	Rows[2].DisplayName.Reset();

	const TSharedPtr<FJsonObject> AppVars = FCrowdyPreSeedPlan::BuildSeedVariables(9007199254740993, FString(), Rows, { 1, 2 });
	const TSharedPtr<FJsonObject>* Input = nullptr;
	if (!TestTrue(TEXT("input object"), AppVars->TryGetObjectField(TEXT("input"), Input)))
	{
		return true;
	}
	FString AppId;
	TestTrue(TEXT("appId is a string"), (*Input)->TryGetStringField(TEXT("appId"), AppId));
	TestEqual(TEXT("appId keeps every digit"), AppId, FString(TEXT("9007199254740993")));
	TestFalse(TEXT("an app-scoped batch carries no sessionId"), (*Input)->HasField(TEXT("sessionId")));
	TestFalse(TEXT("no containerTypes"), (*Input)->HasField(TEXT("containerTypes")));
	const TArray<TSharedPtr<FJsonValue>>* Containers = nullptr;
	if (!TestTrue(TEXT("containers is an array"), (*Input)->TryGetArrayField(TEXT("containers"), Containers)))
	{
		return true;
	}
	TestEqual(TEXT("one entry per row index"), Containers->Num(), 2);
	const TSharedPtr<FJsonObject>* First = nullptr;
	if ((*Containers)[0]->TryGetObject(First))
	{
		FString TempId;
		TestTrue(TEXT("tempId is a string"), (*First)->TryGetStringField(TEXT("tempId"), TempId));
		TestEqual(TEXT("tempId is the row index"), TempId, FString(TEXT("1")));
		TestEqual(TEXT("typeName"), (*First)->GetStringField(TEXT("typeName")), FString(TEXT("Landmark")));
		TestEqual(TEXT("displayName"), (*First)->GetStringField(TEXT("displayName")), FString(TEXT("l1")));
		TestEqual(TEXT("bindingKey"), (*First)->GetStringField(TEXT("bindingKey")), FString(TEXT("l1")));
		TestFalse(TEXT("no properties"), (*First)->HasField(TEXT("properties")));
	}
	const TSharedPtr<FJsonObject>* Second = nullptr;
	if ((*Containers)[1]->TryGetObject(Second))
	{
		TestEqual(TEXT("an empty display name falls back to the type"), (*Second)->GetStringField(TEXT("displayName")), FString(TEXT("Landmark")));
	}

	const TSharedPtr<FJsonObject> SessionVars = FCrowdyPreSeedPlan::BuildSeedVariables(7, TEXT("sess-7"), Rows, { 0 });
	const TSharedPtr<FJsonObject>* SessionInput = nullptr;
	if (SessionVars->TryGetObjectField(TEXT("input"), SessionInput))
	{
		TestEqual(TEXT("a session batch names the session"), (*SessionInput)->GetStringField(TEXT("sessionId")), FString(TEXT("sess-7")));
	}
	return true;
}

// idMapJson is a JSON string holding an object tempId -> containerId: a mapped row becomes Existing with its id, an
// unmapped one stays Create, and rows outside the batch are untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPreSeedPlanApplySeedIdMapTest,
	"CrowdySDK.CrowdyStudio.PreSeedPlanApplySeedIdMap", PreSeedPlanTestFlags)
bool FCrowdyPreSeedPlanApplySeedIdMapTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyPreSeedRow> Rows;
	Rows.Add(PreSeedTestPlannedRow(TEXT("Landmark"), TEXT("l0"), true));
	Rows.Add(PreSeedTestPlannedRow(TEXT("Landmark"), TEXT("l1"), true));
	Rows.Add(PreSeedTestPlannedRow(TEXT("Landmark"), TEXT("l2"), true));

	const int32 Mapped = FCrowdyPreSeedPlan::ApplySeedIdMap(TEXT("{\"0\":\"id-0\",\"2\":\"id-2\",\"7\":\"id-stray\"}"), { 0, 1 }, Rows);
	TestEqual(TEXT("one row of the batch was mapped"), Mapped, 1);
	TestEqual(TEXT("the mapped row is Existing"), Rows[0].Kind, ECrowdyPreSeedRowKind::Existing);
	TestEqual(TEXT("with its id"), Rows[0].ContainerId, FString(TEXT("id-0")));
	TestEqual(TEXT("a missing tempId leaves the row Create"), Rows[1].Kind, ECrowdyPreSeedRowKind::Create);
	TestTrue(TEXT("and without an id"), Rows[1].ContainerId.IsEmpty());
	TestEqual(TEXT("a row outside the batch is untouched even when the map names it"), Rows[2].Kind, ECrowdyPreSeedRowKind::Create);

	TestEqual(TEXT("an unparseable map maps nothing"), FCrowdyPreSeedPlan::ApplySeedIdMap(TEXT("not json"), { 1 }, Rows), 0);
	TestEqual(TEXT("an empty map maps nothing"), FCrowdyPreSeedPlan::ApplySeedIdMap(FString(), { 1 }, Rows), 0);
	TestEqual(TEXT("still Create"), Rows[1].Kind, ECrowdyPreSeedRowKind::Create);
	return true;
}

#endif
