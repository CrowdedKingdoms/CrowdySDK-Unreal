// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "GameModel/CrowdyApplySelection.h"
#include "GameModel/CrowdyModelLoadState.h"
#include "GameModel/CrowdySchemaSync.h"
#include "Gql/CrowdyStudioQueries.h"
#include "Model/CrowdyStudioControllerTestAccess.h"
#include "Model/CrowdyStudioTypes.h"
#include "Model/FCrowdyStudioController.h"
#include "Serialization/JsonSerializer.h"
#include "UI/GameModel/CrowdyReconcileSummary.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStudioAppScopedStateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Every fixture below carries the AppState prefix. Adaptive unity merges this module's .cpp files into shared
	// translation units, so two anonymous-namespace helpers of one name in two files redefine each other.

	FCrowdySchemaTypeUpsert AppStateType(const FString& TypeName)
	{
		FCrowdySchemaTypeUpsert Upsert;
		Upsert.Type.TypeName = TypeName;
		Upsert.Type.DisplayName = TypeName;
		Upsert.bIsNew = true;
		return Upsert;
	}

	FCrowdySchemaPropUpsert AppStateProp(const FString& TypeName, const FString& Key)
	{
		FCrowdySchemaPropUpsert Upsert;
		Upsert.ContainerTypeName = TypeName;
		Upsert.Prop.Key = Key;
		Upsert.Prop.ValueType = TEXT("int");
		Upsert.bIsNew = true;
		return Upsert;
	}

	FCrowdySchemaFunctionUpsert AppStateFunction(const FString& TypeName, const FString& Name)
	{
		FCrowdySchemaFunctionUpsert Upsert;
		Upsert.Function.ContainerTypeName = TypeName;
		Upsert.Function.Name = Name;
		Upsert.bIsNew = true;
		return Upsert;
	}

	// One unit of the pending plan, addressed the way the walk addresses it. Returns a unit with an invalid plan
	// index when the plan holds no such entity, which the caller reports rather than silently sending nothing.
	bool AppStateUnitFor(const TArray<FCrowdyApplyUnit>& Units, ECrowdyApplyKind Kind, const FString& OwningType,
		const FString& Name, FCrowdyApplyUnit& OutUnit)
	{
		for (const FCrowdyApplyUnit& Unit : Units)
		{
			if (Unit.Kind == Kind
				&& Unit.OwningType.Equals(OwningType, ESearchCase::CaseSensitive)
				&& Unit.Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				OutUnit = Unit;
				return true;
			}
		}
		return false;
	}

	// A finished plan carrying some upserts and some entities the server has that this project does not. Built
	// through BuildReport so the counts are the ones a real plan would print, including the server-only half,
	// which no seeding of the pending arrays can produce.
	FCrowdySchemaSyncReport AppStateReport(int32 ServerOnlyTypes, int32 Upserts, bool bApplied)
	{
		FCrowdySchemaDelta Delta;
		for (int32 Index = 0; Index < ServerOnlyTypes; ++Index)
		{
			Delta.ServerOnlyTypes.Add(FString::Printf(TEXT("HandAuthored%d"), Index));
		}
		for (int32 Index = 0; Index < Upserts; ++Index)
		{
			Delta.TypeUpserts.Add(AppStateType(FString::Printf(TEXT("Knight%d"), Index)));
		}
		return FCrowdySchemaSync::BuildReport(Delta, {}, bApplied);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAppSwitchClearsGameModelStateTest,
	"CrowdySDK.CrowdyStudio.AppSwitchClearsGameModelState", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioAppSwitchClearsGameModelStateTest::RunTest(const FString& /*Parameters*/)
{
	// A default-constructed controller touches no network, settings or token vault; Initialize does, and is not called.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();

	FCrowdyStudioControllerTestAccess::PopulateAppScopedState(*Controller);
	TestTrue(TEXT("Game-model state is populated before the reset"), Controller->GetContainerTypes().Num() > 0);
	TestTrue(TEXT("Team state is populated before the reset"), Controller->GetTeams().Num() > 0);
	TestTrue(TEXT("Grid state is populated before the reset"), Controller->GetNearbyGrids().Num() > 0);
	TestTrue(TEXT("Automation state is populated before the reset"), Controller->GetAutomations().Num() > 0);
	TestTrue(TEXT("The lists are marked read before the reset"), Controller->HasReadFunctions());

	// The reset is the one signal that always arrives on an app switch: minting the new app's token can fail, and then
	// nothing else announces the change. A view holding automations only rebuilds if it is told to.
	int32 AutomationsAnnounced = 0;
	Controller->OnAutomationsChanged.AddLambda([&AutomationsAnnounced]() { ++AutomationsAnnounced; });

	FCrowdyStudioControllerTestAccess::ClearAppScopedState(*Controller);

	TestEqual(TEXT("Teams cleared"), Controller->GetTeams().Num(), 0);
	TestEqual(TEXT("Channels cleared"), Controller->GetChannels().Num(), 0);
	TestEqual(TEXT("Group members cleared"), Controller->GetGroupMembers().Num(), 0);
	TestEqual(TEXT("Group roles cleared"), Controller->GetGroupRoles().Num(), 0);
	TestEqual(TEXT("Selected group cleared"), Controller->GetSelectedGroupId(), static_cast<int64>(0));
	TestTrue(TEXT("Team policy cleared"), Controller->GetTeamPolicy().CreationPolicy.IsEmpty());
	TestEqual(TEXT("Team policy app cleared"), Controller->GetTeamPolicy().AppId, static_cast<int64>(0));
	TestTrue(TEXT("Channel policy cleared"), Controller->GetChannelPolicy().CreationPolicy.IsEmpty());
	TestEqual(TEXT("Channel policy app cleared"), Controller->GetChannelPolicy().AppId, static_cast<int64>(0));

	TestEqual(TEXT("Nearby grids cleared"), Controller->GetNearbyGrids().Num(), 0);
	TestEqual(TEXT("Grid whitelist keys cleared"), Controller->GetGridWhitelistKeys().Num(), 0);
	TestEqual(TEXT("Grid user effective keys cleared"), Controller->GetGridUserEffectiveKeys().Num(), 0);
	TestEqual(TEXT("Grid group grants cleared"), Controller->GetGridGroupGrants().Num(), 0);

	TestEqual(TEXT("Container types cleared"), Controller->GetContainerTypes().Num(), 0);
	TestEqual(TEXT("Attribute mirror cleared"), Controller->GetPropertyDefs().Num(), 0);
	TestTrue(TEXT("Attribute mirror type cleared"),
		FCrowdyStudioControllerTestAccess::GetMirrorType(*Controller).IsEmpty());
	TestEqual(TEXT("Per-type attribute cache cleared"),
		FCrowdyStudioControllerTestAccess::GetDefsByType(*Controller).Num(), 0);
	TestEqual(TEXT("In-flight attribute reads cleared"),
		FCrowdyStudioControllerTestAccess::GetFetchesInFlight(*Controller).Num(), 0);
	TestEqual(TEXT("Attribute read serials cleared"),
		FCrowdyStudioControllerTestAccess::GetLatestRequests(*Controller).Num(), 0);
	TestEqual(TEXT("Functions cleared"), Controller->GetFunctions().Num(), 0);
	TestEqual(TEXT("App-wide functions cleared"), Controller->GetUnfilteredFunctions().Num(), 0);
	TestEqual(TEXT("Automations cleared"), Controller->GetAutomations().Num(), 0);
	TestEqual(TEXT("Automation triggers cleared"), Controller->GetAutomationTriggers().Num(), 0);
	TestEqual(TEXT("Automation views are told to rebuild"), AutomationsAnnounced, 1);
	// The three lists above are empty, so the mark saying which app they hold has to go with them, or the Game Model
	// page reads "already loaded" for the new app and shows an empty browser forever.
	TestEqual(TEXT("Game-model list load mark cleared"),
		FCrowdyStudioControllerTestAccess::GetGameModelListsAppId(*Controller), static_cast<int64>(0));
	// And the marks saying those lists were READ. Left set, the new app's empty function list would read as "this
	// app has no functions" rather than "nobody has asked", which is what clears a delete pre-flight's bound-function
	// refusal for every model in the app at once.
	TestFalse(TEXT("The container-type read mark cleared"), Controller->HasReadContainerTypes());
	TestFalse(TEXT("The function read mark cleared"), Controller->HasReadFunctions());
	TestFalse(TEXT("The automation read mark cleared"), Controller->HasReadAutomations());
	// Every family, not only the three with an accessor of their own: a slot left saying Loaded or Loading answers
	// for a list the switch just emptied.
	for (int32 Slot = 0; Slot < static_cast<int32>(ECrowdyModelFamily::Count); ++Slot)
	{
		TestTrue(FString::Printf(TEXT("Family slot %d is back to never requested"), Slot),
			Controller->GetFamilyLoadState(static_cast<ECrowdyModelFamily>(Slot))
				== ECrowdyModelLoadState::NeverRequested);
	}
	// A recorded attribute failure names a model of the app being left, so a model of the same name under the new
	// app would otherwise open onto the previous app's error.
	TestEqual(TEXT("Recorded attribute read failures cleared"),
		FCrowdyStudioControllerTestAccess::GetFailedAttributeTypes(*Controller).Num(), 0);
	TestEqual(TEXT("Features cleared"), Controller->GetFeatures().Num(), 0);
	TestEqual(TEXT("Tier features cleared"), Controller->GetTierFeatures().Num(), 0);
	TestEqual(TEXT("Access tiers cleared"), Controller->GetAccessTiers().Num(), 0);
	TestEqual(TEXT("Live containers cleared"), Controller->GetContainers().Num(), 0);
	TestFalse(TEXT("Container state cleared"), Controller->GetContainerState().bValid);
	TestTrue(TEXT("Container state id cleared"), Controller->GetContainerState().ContainerId.IsEmpty());
	TestTrue(TEXT("Selected container cleared"), Controller->GetSelectedContainerId().IsEmpty());
	TestTrue(TEXT("Live container type filter cleared"),
		FCrowdyStudioControllerTestAccess::GetLastContainerTypeFilter(*Controller).IsEmpty());
	TestTrue(TEXT("Live container session filter cleared"),
		FCrowdyStudioControllerTestAccess::GetLastContainerSessionFilter(*Controller).IsEmpty());
	TestEqual(TEXT("Live container page limit cleared"),
		FCrowdyStudioControllerTestAccess::GetLastContainerLimit(*Controller), 0);
	TestEqual(TEXT("Live container page offset cleared"),
		FCrowdyStudioControllerTestAccess::GetLastContainerOffset(*Controller), 0);
	TestEqual(TEXT("Live container page size cleared"),
		FCrowdyStudioControllerTestAccess::GetLastContainerPageSize(*Controller), 0);
	// The page state is what "load more" reads, so a stale full page from the previous app would offer another
	// page of a list the new app has not read at all.
	TestFalse(TEXT("No further page is offered after an app switch"), Controller->ContainersMayHaveMore());
	TestFalse(TEXT("Game model policy cleared"), Controller->GetGameModelPolicy().bValid);
	TestTrue(TEXT("Game model policy body cleared"),
		Controller->GetGameModelPolicy().SessionCreationPolicy.IsEmpty());

	// A purge left pinned to the app being left never ends: its replies are dropped the moment the selection moves,
	// so nothing decrements it and the Live tab keeps every delete control disabled behind a purge that is over.
	TestEqual(TEXT("Live-model purge ended"),
		FCrowdyStudioControllerTestAccess::GetContainerPurgeAppId(*Controller), static_cast<int64>(0));
	TestTrue(TEXT("And it ended as a cancellation, not a server refusal"),
		Controller->GetLastContainerPurgeOutcome().bStoppedByCancel);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPropertyDefsAreScopedPerTypeTest,
	"CrowdySDK.CrowdyStudio.PropertyDefsAreScopedPerType", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioPropertyDefsAreScopedPerTypeTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();

	TestNull(TEXT("A never-loaded type reads as null, not as an empty list"),
		Controller->GetPropertyDefsForType(TEXT("Health")));

	FCrowdyStudioControllerTestAccess::Ingest(*Controller, TEXT("Health"),
		{ FCrowdyStudioControllerTestAccess::MakeDef(TEXT("Health"), TEXT("Current")),
		  FCrowdyStudioControllerTestAccess::MakeDef(TEXT("Health"), TEXT("Max")) });

	FCrowdyStudioControllerTestAccess::Ingest(*Controller, TEXT("Inventory"),
		{ FCrowdyStudioControllerTestAccess::MakeDef(TEXT("Inventory"), TEXT("Slots")) });

	const TArray<TSharedPtr<FStudioPropertyDef>>* HealthDefs = Controller->GetPropertyDefsForType(TEXT("Health"));
	const TArray<TSharedPtr<FStudioPropertyDef>>* InventoryDefs = Controller->GetPropertyDefsForType(TEXT("Inventory"));

	if (!TestNotNull(TEXT("Health attributes survived the second type's load"), HealthDefs)
		|| !TestNotNull(TEXT("Inventory attributes were stored"), InventoryDefs))
	{
		return false;
	}

	TestEqual(TEXT("Health kept both of its attributes"), HealthDefs->Num(), 2);
	TestEqual(TEXT("Inventory kept its own attribute"), InventoryDefs->Num(), 1);
	TestEqual(TEXT("Health's first attribute is its own"), (*HealthDefs)[0]->Key, FString(TEXT("Current")));
	TestEqual(TEXT("Inventory's attribute is its own"), (*InventoryDefs)[0]->Key, FString(TEXT("Slots")));

	// A type genuinely holding no attributes is distinguishable from one that was never loaded.
	FCrowdyStudioControllerTestAccess::Ingest(*Controller, TEXT("Empty"), {});
	const TArray<TSharedPtr<FStudioPropertyDef>>* EmptyDefs = Controller->GetPropertyDefsForType(TEXT("Empty"));
	if (TestNotNull(TEXT("A loaded type with no attributes is present in the cache"), EmptyDefs))
	{
		TestEqual(TEXT("A loaded type with no attributes reads as empty"), EmptyDefs->Num(), 0);
	}

	// With no type claimed by the mirror, the mirror follows the most recently loaded type without disturbing the
	// per-type cache.
	TestEqual(TEXT("Mirror follows the last load"), Controller->GetPropertyDefs().Num(), 0);
	TestEqual(TEXT("Health is still intact behind the mirror"),
		Controller->GetPropertyDefsForType(TEXT("Health"))->Num(), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPropertyDefMirrorFollowsSelectedTypeTest,
	"CrowdySDK.CrowdyStudio.PropertyDefMirrorFollowsSelectedType", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioPropertyDefMirrorFollowsSelectedTypeTest::RunTest(const FString& /*Parameters*/)
{
	// Clicking Health, then Inventory, then Health again while the first read is still open coalesces the third
	// click onto the first read, so no query is left to correct the mirror afterwards. Health's reply lands first
	// and is correct; Inventory's lands second and must NOT repaint a mirror that is showing Health, or the page
	// renders Inventory's attributes under Health's name and the mutation editor offers Health the wrong keys.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetMirrorType(*Controller, TEXT("Health"));

	FCrowdyStudioControllerTestAccess::Ingest(*Controller, TEXT("Health"),
		{ FCrowdyStudioControllerTestAccess::MakeDef(TEXT("Health"), TEXT("Current")),
		  FCrowdyStudioControllerTestAccess::MakeDef(TEXT("Health"), TEXT("Max")) });
	TestEqual(TEXT("The mirror takes the type it is showing"), Controller->GetPropertyDefs().Num(), 2);

	FCrowdyStudioControllerTestAccess::Ingest(*Controller, TEXT("Inventory"),
		{ FCrowdyStudioControllerTestAccess::MakeDef(TEXT("Inventory"), TEXT("Slots")) });

	TestEqual(TEXT("The mirror ignores a reply for a type it is not showing"),
		Controller->GetPropertyDefs().Num(), 2);
	if (Controller->GetPropertyDefs().Num() == 2)
	{
		TestEqual(TEXT("The mirror still holds the shown type's attributes"),
			Controller->GetPropertyDefs()[0]->ContainerTypeName, FString(TEXT("Health")));
	}

	// The cache is not what the guard protects: every reply still lands there.
	const TArray<TSharedPtr<FStudioPropertyDef>>* InventoryDefs = Controller->GetPropertyDefsForType(TEXT("Inventory"));
	if (TestNotNull(TEXT("The ignored reply still filled the per-type cache"), InventoryDefs))
	{
		TestEqual(TEXT("The cached attributes are the ones that arrived"), InventoryDefs->Num(), 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioCacheFillIsAnnouncedWhateverTheMirrorShowsTest,
	"CrowdySDK.CrowdyStudio.CacheFillIsAnnouncedWhateverTheMirrorShows", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioCacheFillIsAnnouncedWhateverTheMirrorShowsTest::RunTest(const FString& /*Parameters*/)
{
	// Two views can want two types' attributes at once, and only one of them owns the flat mirror. The one that does
	// not still asked for its type and is showing a loading state until it hears back, so the arrival has to be
	// announced even where the mirror deliberately ignores the reply. Without that, the reply lands in the cache and
	// the view waiting on it never repaints.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetMirrorType(*Controller, TEXT("Health"));

	int32 CacheFills = 0;
	int32 MirrorRepaints = 0;
	Controller->OnPropertyDefsCached.AddLambda([&CacheFills]() { ++CacheFills; });
	Controller->OnPropertyDefsChanged.AddLambda([&MirrorRepaints]() { ++MirrorRepaints; });

	FCrowdyStudioControllerTestAccess::Ingest(*Controller, TEXT("Inventory"),
		{ FCrowdyStudioControllerTestAccess::MakeDef(TEXT("Inventory"), TEXT("Slots")) });

	TestEqual(TEXT("A reply the mirror ignores is still announced as having reached the cache"), CacheFills, 1);
	TestEqual(TEXT("The mirror itself is not announced, because it did not change"), MirrorRepaints, 0);

	FCrowdyStudioControllerTestAccess::Ingest(*Controller, TEXT("Health"),
		{ FCrowdyStudioControllerTestAccess::MakeDef(TEXT("Health"), TEXT("Current")) });

	TestEqual(TEXT("The shown type's reply is announced as a cache fill too"), CacheFills, 2);
	TestEqual(TEXT("The shown type's reply repaints the mirror"), MirrorRepaints, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioPostWriteAttributeReadIsNotCoalescedTest,
	"CrowdySDK.CrowdyStudio.PostWriteAttributeReadIsNotCoalesced", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioPostWriteAttributeReadIsNotCoalescedTest::RunTest(const FString& /*Parameters*/)
{
	// Saving a property re-reads the type. If a read of that type was already open, coalescing onto it would answer
	// with the list as it stood BEFORE the save, and nothing further would be issued to correct it: the saved
	// attribute would simply be missing until the type was selected again. A forced read must therefore go out even
	// with one in flight, while an ordinary re-selection must still coalesce.
	//
	// Neither call reaches a server here: with no session sign-in the game plane refuses before any transport, which
	// is enough to observe which of the two issued a read of its own.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 1);
	FCrowdyStudioControllerTestAccess::MarkFetchInFlight(*Controller, TEXT("Health"));

	Controller->FetchPropertyDefs(TEXT("Health"));
	TestEqual(TEXT("An ordinary re-selection coalesces onto the open read"),
		FCrowdyStudioControllerTestAccess::GetLatestRequests(*Controller).Num(), 0);

	Controller->FetchPropertyDefs(TEXT("Health"), /*bForceRefresh*/ true);
	TestTrue(TEXT("A post-write read is issued even with one already in flight"),
		FCrowdyStudioControllerTestAccess::GetLatestRequests(*Controller).Contains(TEXT("Health")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioBackgroundRefreshDoesNotClaimMirrorTest,
	"CrowdySDK.CrowdyStudio.BackgroundRefreshDoesNotClaimMirror", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioBackgroundRefreshDoesNotClaimMirrorTest::RunTest(const FString& /*Parameters*/)
{
	// A re-read that follows a save runs in the background, and by the time it goes out the user may already be
	// looking at a different type. If it took the mirror, the type actually on screen would have its own reply thrown
	// away as "not the shown type" and the written type's attributes painted under the shown type's name instead.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 1);
	FCrowdyStudioControllerTestAccess::SetMirrorType(*Controller, TEXT("Inventory"));

	Controller->FetchPropertyDefs(TEXT("Health"), /*bForceRefresh*/ true);
	TestEqual(TEXT("A background refresh leaves the mirror on the type being shown"),
		FCrowdyStudioControllerTestAccess::GetMirrorType(*Controller), FString(TEXT("Inventory")));

	// A selection is the only thing that moves the mirror.
	Controller->FetchPropertyDefs(TEXT("Health"));
	TestEqual(TEXT("A selection moves the mirror to the type it asked for"),
		FCrowdyStudioControllerTestAccess::GetMirrorType(*Controller), FString(TEXT("Health")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioSupersededAttributeReplyIsDiscardedTest,
	"CrowdySDK.CrowdyStudio.SupersededAttributeReplyIsDiscarded", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioSupersededAttributeReplyIsDiscardedTest::RunTest(const FString& /*Parameters*/)
{
	// Replies can arrive in any order. A reply is acted on only while it is still the newest read issued for its type
	// and its app is still selected; anything else can only put an older picture back.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 1);
	FCrowdyStudioControllerTestAccess::SetLatestRequest(*Controller, TEXT("Health"), 5);

	TestTrue(TEXT("The newest read of a type is acted on"),
		FCrowdyStudioControllerTestAccess::IsLatestRequest(*Controller, TEXT("Health"), 1, 5));
	TestFalse(TEXT("A read superseded by a later one is discarded"),
		FCrowdyStudioControllerTestAccess::IsLatestRequest(*Controller, TEXT("Health"), 1, 4));
	TestFalse(TEXT("A reply for an app that is no longer selected is discarded"),
		FCrowdyStudioControllerTestAccess::IsLatestRequest(*Controller, TEXT("Health"), 2, 5));
	TestFalse(TEXT("A reply for a type with no read on record is discarded"),
		FCrowdyStudioControllerTestAccess::IsLatestRequest(*Controller, TEXT("Inventory"), 1, 5));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioDeleteFunctionRefusesAcrossAppsTest,
	"CrowdySDK.CrowdyStudio.DeleteFunctionRefusesAcrossApps", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioDeleteFunctionRefusesAcrossAppsTest::RunTest(const FString& /*Parameters*/)
{
	// A function usually carries the same name in a development and a production app, since both are deployed from the
	// same assets, so a delete aimed by name alone can destroy the wrong app's function. There is no undo. The caller
	// says which app it read the name from, and a delete goes out only if that app is still the selected one.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedApp(*Controller, 200);

	Controller->DeleteFunction(TEXT("Knight"), TEXT("regen"), /*ExpectedAppId*/ 100);
	TestTrue(TEXT("A delete naming another app's function is refused"),
		Controller->GetStatusMessage().Contains(TEXT("different app")));

	// The matching case gets past the app check and is stopped only by the missing sign-in, which is a different
	// message: that difference is what shows the refusal above was the app check and not a shared early return.
	Controller->DeleteFunction(TEXT("Knight"), TEXT("regen"), /*ExpectedAppId*/ 200);
	TestFalse(TEXT("A delete for the selected app is not refused by the app check"),
		Controller->GetStatusMessage().Contains(TEXT("different app")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioContainerPageAppendSkipsHeldTest,
	"CrowdySDK.CrowdyStudio.ContainerPageAppendSkipsHeld", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioContainerPageAppendSkipsHeldTest::RunTest(const FString& /*Parameters*/)
{
	// Pages are read by offset over an ordering that keeps moving: delete a container between two reads and the
	// window shifts, so the next page hands back a container the list already holds. Appended blindly, the same
	// instance appears twice and the delete button names one of two identical-looking rows.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetContainers(*Controller,
		{ FCrowdyStudioControllerTestAccess::MakeContainer(TEXT("aaa")),
		  FCrowdyStudioControllerTestAccess::MakeContainer(TEXT("bbb")) });

	FCrowdyStudioControllerTestAccess::AppendContainers(*Controller,
		{ FCrowdyStudioControllerTestAccess::MakeContainer(TEXT("bbb")),
		  FCrowdyStudioControllerTestAccess::MakeContainer(TEXT("ccc")) });

	TestEqual(TEXT("An overlapping page adds only what is new"), Controller->GetContainers().Num(), 3);
	if (Controller->GetContainers().Num() == 3)
	{
		TestEqual(TEXT("The held containers keep their order"),
			Controller->GetContainers()[0]->ContainerId, FString(TEXT("aaa")));
		TestEqual(TEXT("The new container lands at the end"),
			Controller->GetContainers()[2]->ContainerId, FString(TEXT("ccc")));
	}

	// Container ids are opaque, so two ids differing only in case are two different containers. Matching them with
	// FString::operator== or through a TSet<FString>, both of which ignore case, would silently drop this one.
	FCrowdyStudioControllerTestAccess::AppendContainers(*Controller,
		{ FCrowdyStudioControllerTestAccess::MakeContainer(TEXT("BBB")) });

	TestEqual(TEXT("An id differing only in case is a different container"), Controller->GetContainers().Num(), 4);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioContainerPageMayHaveMoreTest,
	"CrowdySDK.CrowdyStudio.ContainerPageMayHaveMore", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioContainerPageMayHaveMoreTest::RunTest(const FString& /*Parameters*/)
{
	// The query reports no count of any kind, so the only evidence another page exists is a page that came back
	// full. A short page is the end of the list, and an unpaged read already returned everything.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();

	TestFalse(TEXT("Nothing read yet offers no further page"), Controller->ContainersMayHaveMore());

	FCrowdyStudioControllerTestAccess::SetContainerPageResult(*Controller, /*Limit*/ 50, /*PageSize*/ 50);
	TestTrue(TEXT("A full page means there may be more"), Controller->ContainersMayHaveMore());

	FCrowdyStudioControllerTestAccess::SetContainerPageResult(*Controller, /*Limit*/ 50, /*PageSize*/ 49);
	TestFalse(TEXT("A short page is the end of the list"), Controller->ContainersMayHaveMore());

	FCrowdyStudioControllerTestAccess::SetContainerPageResult(*Controller, /*Limit*/ 0, /*PageSize*/ 120);
	TestFalse(TEXT("An unpaged read already returned everything"), Controller->ContainersMayHaveMore());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioContainerRelistKeepsPagingEvidenceTest,
	"CrowdySDK.CrowdyStudio.ContainerRelistKeepsPagingEvidence", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioContainerRelistKeepsPagingEvidenceTest::RunTest(const FString& /*Parameters*/)
{
	// After a delete the whole window on screen is re-read as one request. It comes back one container short of
	// what it asked for, because one was just deleted. Judged as a page, that reads as the end of the list and
	// retires "load more" for good, with hundreds of containers still unread and Refresh the only way back.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();

	FCrowdyStudioControllerTestAccess::SetContainerPageResult(*Controller, /*Limit*/ 50, /*PageSize*/ 50);
	FCrowdyStudioControllerTestAccess::SetContainerPageResult(*Controller, /*Limit*/ 50, /*PageSize*/ 50);
	TestTrue(TEXT("Two full pages mean there may be more"), Controller->ContainersMayHaveMore());

	FCrowdyStudioControllerTestAccess::RelistContainerWindow(*Controller, /*Limit*/ 100, /*PageSize*/ 99);
	TestTrue(TEXT("A re-read of the window does not answer for the end of the list"),
		Controller->ContainersMayHaveMore());

	// A real page still has to be able to say the list ended, or nothing would ever retire the button.
	FCrowdyStudioControllerTestAccess::SetContainerPageResult(*Controller, /*Limit*/ 50, /*PageSize*/ 12);
	TestFalse(TEXT("A short page still ends the list"), Controller->ContainersMayHaveMore());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioContainerReadSerialDropsStaleReplyTest,
	"CrowdySDK.CrowdyStudio.ContainerReadSerialDropsStaleReply", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioContainerReadSerialDropsStaleReplyTest::RunTest(const FString& /*Parameters*/)
{
	// The app a read was issued for is not enough to decide whether its reply is still wanted: a user who switches
	// away and back selects the same app again, so the ids compare equal while the list the reply would fill has
	// been emptied in between. A page landing then would leave the list holding page two and nothing else.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();

	const uint64 First = FCrowdyStudioControllerTestAccess::BeginContainerRead(*Controller);
	TestTrue(TEXT("The only read in flight is the newest"),
		FCrowdyStudioControllerTestAccess::IsLatestContainerRead(*Controller, First));

	const uint64 Second = FCrowdyStudioControllerTestAccess::BeginContainerRead(*Controller);
	TestFalse(TEXT("A read overtaken by a newer one is dropped"),
		FCrowdyStudioControllerTestAccess::IsLatestContainerRead(*Controller, First));
	TestTrue(TEXT("The newest read is still wanted"),
		FCrowdyStudioControllerTestAccess::IsLatestContainerRead(*Controller, Second));

	FCrowdyStudioControllerTestAccess::ClearAppScopedState(*Controller);
	TestFalse(TEXT("An app switch drops the read that was in flight across it"),
		FCrowdyStudioControllerTestAccess::IsLatestContainerRead(*Controller, Second));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioContainerBindingKeyIsReadTest,
	"CrowdySDK.CrowdyStudio.ContainerBindingKeyIsRead", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioContainerBindingKeyIsReadTest::RunTest(const FString& /*Parameters*/)
{
	// A container ensured under a binding key is recreated by the runtime the next time that key is ensured, so
	// the key has to reach the list for the page to be able to say so.
	const FString Json = TEXT(R"({"data":{"gameModelContainers":[)")
		TEXT(R"({"containerId":"c-1","typeName":"Health","displayName":"Knight","bindingKey":"player:42"},)")
		TEXT(R"({"containerId":"c-2","typeName":"Health","displayName":"Wolf"}]}})");

	TSharedPtr<FJsonObject> Envelope;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!TestTrue(TEXT("The envelope parses"), FJsonSerializer::Deserialize(Reader, Envelope) && Envelope.IsValid()))
	{
		return false;
	}

	TArray<TSharedPtr<FStudioContainer>> Containers;
	CrowdyStudioGql::ParseContainers(Envelope, TEXT("gameModelContainers"), Containers);

	if (!TestEqual(TEXT("Both containers are read"), Containers.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("A bound container carries its key"), Containers[0]->BindingKey, FString(TEXT("player:42")));
	TestTrue(TEXT("A container created outright carries no key"), Containers[1]->BindingKey.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioGameModelListsLoadOncePerAppTest,
	"CrowdySDK.CrowdyStudio.GameModelListsLoadOncePerApp", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioGameModelListsLoadOncePerAppTest::RunTest(const FString& /*Parameters*/)
{
	// The Game Model page asks this every time it is painted, so the answer has to be no as soon as the lists are the
	// selected app's. Saying yes twice for one app turns a page that is merely on screen into a read loop.
	TestTrue(TEXT("Nothing loaded yet, so an app's lists are owed"),
		FCrowdyStudioController::ShouldLoadGameModelLists(/*SelectedAppId*/ 7, /*LoadedAppId*/ 0));

	TestFalse(TEXT("The lists are already this app's, so nothing is owed"),
		FCrowdyStudioController::ShouldLoadGameModelLists(7, 7));

	TestTrue(TEXT("The lists belong to another app, so this app's are owed"),
		FCrowdyStudioController::ShouldLoadGameModelLists(7, 8));

	// With no app selected there is nothing to read and no id to scope a read to, whatever was loaded before.
	TestFalse(TEXT("No app selected, nothing is owed"),
		FCrowdyStudioController::ShouldLoadGameModelLists(0, 0));
	TestFalse(TEXT("No app selected still owes nothing when a previous app's lists are held"),
		FCrowdyStudioController::ShouldLoadGameModelLists(0, 8));

	// App ids are BigInt on the wire, so the comparison has to hold past the 32-bit range rather than truncating two
	// distinct apps into one.
	constexpr int64 BeyondInt32 = 4300000000LL;
	TestFalse(TEXT("A BigInt app id matches itself"),
		FCrowdyStudioController::ShouldLoadGameModelLists(BeyondInt32, BeyondInt32));
	TestTrue(TEXT("Two app ids differing only above the 32-bit range are different apps"),
		FCrowdyStudioController::ShouldLoadGameModelLists(BeyondInt32, BeyondInt32 + 1));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioSchemaPlanBusyIsScopedToTheSelectionTest,
	"CrowdySDK.CrowdyStudio.SchemaPlanBusyIsScopedToTheSelection", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioSchemaPlanBusyIsScopedToTheSelectionTest::RunTest(const FString& /*Parameters*/)
{
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);

	int32 Announced = 0;
	Controller->OnSchemaPlanProgress.AddLambda([&Announced]() { ++Announced; });

	TestFalse(TEXT("Nothing is running before a plan starts"), Controller->IsSchemaPlanInFlight());

	FCrowdyStudioControllerTestAccess::SetSchemaPlanPhase(*Controller, 7, TEXT("Reading the server's schema"));
	TestTrue(TEXT("A plan for the selected app reads as running"), Controller->IsSchemaPlanInFlight());
	TestEqual(TEXT("The phase is what the indicator will show"),
		Controller->GetSchemaPlanPhase(), FString(TEXT("Reading the server's schema")));
	TestEqual(TEXT("Starting a phase is announced"), Announced, 1);

	// The user switches app while that plan is still reading. It is superseded and will never repaint anything, so
	// reporting it as running under the new app would promise an answer that is never coming.
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 8);
	TestFalse(TEXT("A plan pinned to another app is not this app's business"), Controller->IsSchemaPlanInFlight());

	// Switching back finds it still running, because the plan really is: nothing cancelled it.
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);
	TestTrue(TEXT("Switching back finds the same plan still running"), Controller->IsSchemaPlanInFlight());

	// An empty phase is how every abandon path says the plan stopped, whichever app it was pinned to.
	FCrowdyStudioControllerTestAccess::SetSchemaPlanPhase(*Controller, 7, FString());
	TestFalse(TEXT("An empty phase ends the plan"), Controller->IsSchemaPlanInFlight());
	TestEqual(TEXT("Ending the plan is announced too"), Announced, 2);

	// A stopped plan stays stopped under every app, so no selection can resurrect a spinner.
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 0);
	TestFalse(TEXT("No app selected reads as idle"), Controller->IsSchemaPlanInFlight());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioApplyPartialRemovesOnlyWhatItSentTest,
	"CrowdySDK.CrowdyStudio.ApplyPartialRemovesOnlyWhatItSent", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioApplyPartialRemovesOnlyWhatItSentTest::RunTest(const FString& /*Parameters*/)
{
	// A walk may carry a subset of the plan. What it did not send is the exact remainder a second press has to
	// finish, and there is nothing anywhere else that records it, so clearing all five arrays destroys it.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);
	FCrowdyStudioControllerTestAccess::SeedPendingSchemaSync(*Controller, /*PlannedAppId*/ 7,
		{ AppStateType(TEXT("Knight")), AppStateType(TEXT("Goblin")) },
		{ AppStateProp(TEXT("Knight"), TEXT("hp")), AppStateProp(TEXT("Knight"), TEXT("mana")),
		  AppStateProp(TEXT("Goblin"), TEXT("hp")) },
		{ AppStateFunction(TEXT("Knight"), TEXT("take_damage")) },
		{}, {});

	const TArray<FCrowdyApplyUnit> Units =
		CrowdyApplySelection::BuildUnits(Controller->MakeSchemaApplyPlanInput());

	// Two attributes chosen so their plan indices are NOT adjacent: removing the lower one first shifts the higher
	// one, and an ascending walk would then delete the entry the send deliberately left behind.
	FCrowdyApplyUnit KnightType;
	FCrowdyApplyUnit KnightHp;
	FCrowdyApplyUnit GoblinHp;
	if (!TestTrue(TEXT("the plan holds the Knight model"),
			AppStateUnitFor(Units, ECrowdyApplyKind::Type, FString(), TEXT("Knight"), KnightType))
		|| !TestTrue(TEXT("the plan holds hp on Knight"),
			AppStateUnitFor(Units, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("hp"), KnightHp))
		|| !TestTrue(TEXT("the plan holds hp on Goblin"),
			AppStateUnitFor(Units, ECrowdyApplyKind::Attribute, TEXT("Goblin"), TEXT("hp"), GoblinHp)))
	{
		return false;
	}

	FCrowdyStudioControllerTestAccess::FinishSchemaUpsertWalk(*Controller, { KnightType, KnightHp, GoblinHp });

	TestEqual(TEXT("only the sent model went"),
		FCrowdyStudioControllerTestAccess::GetPendingTypes(*Controller).Num(), 1);
	if (FCrowdyStudioControllerTestAccess::GetPendingTypes(*Controller).Num() == 1)
	{
		TestEqual(TEXT("the unsent model is the one still pending"),
			FCrowdyStudioControllerTestAccess::GetPendingTypes(*Controller)[0].Type.TypeName, FString(TEXT("Goblin")));
	}

	TestEqual(TEXT("only the two sent attributes went"),
		FCrowdyStudioControllerTestAccess::GetPendingProps(*Controller).Num(), 1);
	if (FCrowdyStudioControllerTestAccess::GetPendingProps(*Controller).Num() == 1)
	{
		// mana sat between the two entries that were removed. Removing the lower index first would have shifted it
		// into the higher index's place and deleted it instead.
		TestEqual(TEXT("the attribute between the two sent ones survives"),
			FCrowdyStudioControllerTestAccess::GetPendingProps(*Controller)[0].Prop.Key, FString(TEXT("mana")));
		TestEqual(TEXT("and it survives on its own model"),
			FCrowdyStudioControllerTestAccess::GetPendingProps(*Controller)[0].ContainerTypeName, FString(TEXT("Knight")));
	}

	TestEqual(TEXT("a kind the walk never touched is untouched"),
		FCrowdyStudioControllerTestAccess::GetPendingFunctions(*Controller).Num(), 1);
	TestTrue(TEXT("something is still pending, so the app still has drift"), Controller->HasPendingSchemaSync());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioApplyPartialDoesNotReportInSyncTest,
	"CrowdySDK.CrowdyStudio.ApplyPartialDoesNotReportInSync", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioApplyPartialDoesNotReportInSyncTest::RunTest(const FString& /*Parameters*/)
{
	// The readiness pill reads bApplied as "no drift, whatever the counts say". Set after a partial send it paints
	// an app whose schema really has drifted as in sync, and nothing on the page contradicts it.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);
	FCrowdyStudioControllerTestAccess::SeedPendingSchemaSync(*Controller, /*PlannedAppId*/ 7,
		{ AppStateType(TEXT("Knight")), AppStateType(TEXT("Goblin")) }, {}, {}, {}, {});

	const TArray<FCrowdyApplyUnit> Units =
		CrowdyApplySelection::BuildUnits(Controller->MakeSchemaApplyPlanInput());

	FCrowdyApplyUnit KnightType;
	if (!TestTrue(TEXT("the plan holds the Knight model"),
		AppStateUnitFor(Units, ECrowdyApplyKind::Type, FString(), TEXT("Knight"), KnightType)))
	{
		return false;
	}

	FCrowdyStudioControllerTestAccess::FinishSchemaUpsertWalk(*Controller, { KnightType });

	TestFalse(TEXT("a partial send does not claim the schema was applied"),
		Controller->GetSchemaSyncReport().bApplied);
	TestTrue(TEXT("the setup strip still says the schema needs syncing"),
		Controller->GetSchemaReadiness() == ECrowdyStudioReadiness::NotReady);
	TestTrue(TEXT("and the banner says how many were left"),
		Controller->GetSchemaSyncReport().StatusNote.Contains(TEXT("still planned")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioServerOnlyIsNotNeedsSyncTest,
	"CrowdySDK.CrowdyStudio.ServerOnlyIsNotNeedsSync", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioServerOnlyIsNotNeedsSyncTest::RunTest(const FString& /*Parameters*/)
{
	// "Needs sync" is a call to action for the button on the same card, and a sync never deletes. Counted as
	// drift, an app holding one hand-authored or console-seeded model read as needing a sync forever, beside a
	// report on the same card saying there was nothing to sync.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);

	FCrowdyStudioControllerTestAccess::SetSchemaSyncReport(*Controller,
		AppStateReport(/*ServerOnlyTypes*/ 3, /*Upserts*/ 0, /*bApplied*/ false));

	TestTrue(TEXT("an empty plan with prune candidates does not ask for a sync"),
		Controller->GetSchemaReadiness() != ECrowdyStudioReadiness::NotReady);
	TestTrue(TEXT("it reads as something to review instead"),
		Controller->GetSchemaReadiness() == ECrowdyStudioReadiness::Advisory);
	TestFalse(TEXT("and the line beside the indicator does not claim a clean match"),
		CrowdyReconcileSummary::BuildCountLine(Controller->GetSchemaSyncReport()).Contains(TEXT("Everything matches")));

	// Control: an outstanding upsert IS drift, so the amber state has not been argued away wholesale.
	FCrowdyStudioControllerTestAccess::SetSchemaSyncReport(*Controller,
		AppStateReport(/*ServerOnlyTypes*/ 3, /*Upserts*/ 1, /*bApplied*/ false));
	TestTrue(TEXT("an outstanding upsert still needs a sync"),
		Controller->GetSchemaReadiness() == ECrowdyStudioReadiness::NotReady);

	// Control: a plan that found nothing at all is plainly ready, so the review state is not the answer to
	// everything either.
	FCrowdyStudioControllerTestAccess::SetSchemaSyncReport(*Controller,
		AppStateReport(/*ServerOnlyTypes*/ 0, /*Upserts*/ 0, /*bApplied*/ false));
	TestTrue(TEXT("a plan that found nothing is ready"),
		Controller->GetSchemaReadiness() == ECrowdyStudioReadiness::Ready);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioUncomparedSchemaIsNotReadyGreenTest,
	"CrowdySDK.CrowdyStudio.UncomparedSchemaIsNotReadyGreen", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioUncomparedSchemaIsNotReadyGreenTest::RunTest(const FString& /*Parameters*/)
{
	// A project that declares no containers and no effects has nothing to diff, so the plan short-circuits without
	// ever reading the server. Its zero counts mean nothing was compared, and reading that as Ready tells an author
	// the schema matches an app nobody asked, which can be carrying a kit deploy or a console-seeded schema.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);

	FCrowdySchemaSyncReport Uncompared = AppStateReport(/*ServerOnlyTypes*/ 0, /*Upserts*/ 0, /*bApplied*/ false);
	Uncompared.bServerNotCompared = true;
	FCrowdyStudioControllerTestAccess::SetSchemaSyncReport(*Controller, Uncompared);

	TestTrue(TEXT("a plan that never read the server does not read as ready"),
		Controller->GetSchemaReadiness() != ECrowdyStudioReadiness::Ready);
	TestTrue(TEXT("it reads as something to look at instead"),
		Controller->GetSchemaReadiness() == ECrowdyStudioReadiness::Advisory);
	TestFalse(TEXT("and the line beside the indicator does not claim a clean match"),
		CrowdyReconcileSummary::BuildCountLine(Controller->GetSchemaSyncReport()).Contains(TEXT("Everything matches")));

	// Control: the identical zero-count report that DID compare is plainly ready, so the flag is what decides it
	// and the ready state has not been argued away for every empty plan.
	FCrowdyStudioControllerTestAccess::SetSchemaSyncReport(*Controller,
		AppStateReport(/*ServerOnlyTypes*/ 0, /*Upserts*/ 0, /*bApplied*/ false));
	TestTrue(TEXT("a compared, empty plan is ready"),
		Controller->GetSchemaReadiness() == ECrowdyStudioReadiness::Ready);
	TestTrue(TEXT("and does say everything matches"),
		CrowdyReconcileSummary::BuildCountLine(Controller->GetSchemaSyncReport()).Contains(TEXT("Everything matches")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAppliedKeepsServerOnlyReviewTest,
	"CrowdySDK.CrowdyStudio.AppliedKeepsServerOnlyReview", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioAppliedKeepsServerOnlyReviewTest::RunTest(const FString& /*Parameters*/)
{
	// An apply writes the upserts and touches nothing server-only, so the review outlives it. The indicator and
	// the line on the same card have to agree about that: one of them saying the app is settled while the other
	// asks for a review is the contradiction this card shipped with.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);

	FCrowdyStudioControllerTestAccess::SetSchemaSyncReport(*Controller,
		AppStateReport(/*ServerOnlyTypes*/ 2, /*Upserts*/ 1, /*bApplied*/ true));

	TestTrue(TEXT("what the apply wrote is not still outstanding"),
		Controller->GetSchemaReadiness() != ECrowdyStudioReadiness::NotReady);
	TestTrue(TEXT("but the untouched server-only entities are still a review"),
		Controller->GetSchemaReadiness() == ECrowdyStudioReadiness::Advisory);
	TestTrue(TEXT("and the line says how many, rather than calling it settled"),
		CrowdyReconcileSummary::BuildCountLine(Controller->GetSchemaSyncReport()).Contains(TEXT("2 only on server")));

	// Control: an apply with nothing left on the server is ready, and says exactly the one settled sentence.
	FCrowdyStudioControllerTestAccess::SetSchemaSyncReport(*Controller,
		AppStateReport(/*ServerOnlyTypes*/ 0, /*Upserts*/ 1, /*bApplied*/ true));
	TestTrue(TEXT("a clean apply is ready"),
		Controller->GetSchemaReadiness() == ECrowdyStudioReadiness::Ready);
	TestEqual(TEXT("and says so once"),
		CrowdyReconcileSummary::BuildCountLine(Controller->GetSchemaSyncReport()),
		FString(TEXT("Synced. Check again to confirm.")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioApplyFullStillClearsEverythingTest,
	"CrowdySDK.CrowdyStudio.ApplyFullStillClearsEverything", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioApplyFullStillClearsEverythingTest::RunTest(const FString& /*Parameters*/)
{
	// The path nobody narrowed has to end exactly where it always did: every array empty, the app in sync and no
	// leftover banner. A removal that misses one entry leaves the app permanently one change short of clean.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);
	FCrowdyStudioControllerTestAccess::SeedPendingSchemaSync(*Controller, /*PlannedAppId*/ 7,
		{ AppStateType(TEXT("Knight")), AppStateType(TEXT("Goblin")) },
		{ AppStateProp(TEXT("Knight"), TEXT("hp")), AppStateProp(TEXT("Goblin"), TEXT("hp")) },
		{ AppStateFunction(TEXT("Knight"), TEXT("take_damage")) },
		{}, {});

	const TArray<FCrowdyApplyUnit> Units =
		CrowdyApplySelection::BuildUnits(Controller->MakeSchemaApplyPlanInput());
	TestEqual(TEXT("the plan holds every seeded entity"), Units.Num(), 5);

	FCrowdyStudioControllerTestAccess::FinishSchemaUpsertWalk(*Controller, Units);

	TestEqual(TEXT("models cleared"), FCrowdyStudioControllerTestAccess::GetPendingTypes(*Controller).Num(), 0);
	TestEqual(TEXT("attributes cleared"), FCrowdyStudioControllerTestAccess::GetPendingProps(*Controller).Num(), 0);
	TestEqual(TEXT("functions cleared"), FCrowdyStudioControllerTestAccess::GetPendingFunctions(*Controller).Num(), 0);
	TestEqual(TEXT("automations cleared"),
		FCrowdyStudioControllerTestAccess::GetPendingAutomations(*Controller).Num(), 0);
	TestEqual(TEXT("triggers cleared"), FCrowdyStudioControllerTestAccess::GetPendingTriggers(*Controller).Num(), 0);

	TestTrue(TEXT("a whole send reports the schema as applied"), Controller->GetSchemaSyncReport().bApplied);
	TestTrue(TEXT("and leaves no partial-send banner behind"),
		Controller->GetSchemaSyncReport().StatusNote.IsEmpty());
	// The next press starts from everything again, so a selection made once is never silently repeated.
	TestTrue(TEXT("the selection is back to everything"),
		FCrowdyStudioControllerTestAccess::IsSchemaApplySelectionAll(*Controller));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioApplySelectionNeedsTheSessionChannelOnlyForItsOwnFunctionsTest,
	"CrowdySDK.CrowdyStudio.ApplySelectionNeedsTheSessionChannelOnlyForItsOwnFunctions",
	CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioApplySelectionNeedsTheSessionChannelOnlyForItsOwnFunctionsTest::RunTest(const FString& /*Parameters*/)
{
	// Whether the PLAN needs the app's session channel is not the same question as whether THIS selection does.
	// A selection with no channel-notifying function in it has no use for a channel, and creating one anyway spends
	// a write and a whole re-plan on nothing.
	//
	// The upserts are seeded WITHOUT any channel notification on purpose, because that is the only shape the diff
	// can produce here: with no channel to resolve an id from, it cannot author the notification and substitutes the
	// server's own, so the upserts of an app whose channel is missing carry no trace of one. Which functions asked
	// for the channel is therefore recorded off the desired schema by the plan, and that record is what the
	// selection is judged against. Asking the upserts instead answers "none" for every possible selection, exactly
	// when the channel is absent, which is the only state the auto-create exists for.
	//
	// What separates the two outcomes here is whether the apply reached its walk at all. Neither call can talk to a
	// server (there is no sign-in), so the walk's first operation fails immediately and leaves its own "applied 0 of
	// N" banner; an apply that stopped to create a channel never gets that far and leaves the banner empty.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);

	auto Seed = [&Controller]()
	{
		FCrowdyStudioControllerTestAccess::SeedPendingSchemaSync(*Controller, /*PlannedAppId*/ 7, {}, {},
			{ AppStateFunction(TEXT("Knight"), TEXT("take_damage")),
			  AppStateFunction(TEXT("Knight"), TEXT("regen")) },
			{}, {});
		FCrowdyStudioControllerTestAccess::SetPlannedSyncNeedsSessionChannel(*Controller, true);
		FCrowdyStudioControllerTestAccess::SetPlannedSessionChannelFunctions(*Controller,
			{ TPair<FString, FString>(TEXT("Knight"), TEXT("regen")) });
	};

	Seed();
	const TArray<FCrowdyApplyUnit> Units =
		CrowdyApplySelection::BuildUnits(Controller->MakeSchemaApplyPlanInput());

	FCrowdyApplyUnit Plain;
	FCrowdyApplyUnit Notifying;
	if (!TestTrue(TEXT("the plan holds the function that notifies nothing"),
			AppStateUnitFor(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("take_damage"), Plain))
		|| !TestTrue(TEXT("the plan holds the function that notifies the session channel"),
			AppStateUnitFor(Units, ECrowdyApplyKind::Function, TEXT("Knight"), TEXT("regen"), Notifying)))
	{
		return false;
	}

	Controller->SetSchemaApplySelection({ Plain.IdentityKey() });
	Controller->ApplySchemaSync();
	TestTrue(TEXT("a selection with no channel-notifying function goes straight to its walk"),
		Controller->GetSchemaSyncReport().StatusNote.Contains(TEXT("Applied 0 of 1")));
	TestFalse(TEXT("and never enters the channel auto-create"),
		FCrowdyStudioControllerTestAccess::IsAutoCreatingSessionChannel(*Controller));

	// The other half of the rule, and the one the whole auto-create exists for: a selection that DOES carry a
	// channel-notifying function stops and provisions the channel before it sends anything.
	Seed();
	Controller->SetSchemaApplySelection({ Notifying.IdentityKey() });
	Controller->ApplySchemaSync();
	TestTrue(TEXT("a selection carrying a channel-notifying function stops to create the channel first"),
		Controller->GetSchemaSyncReport().StatusNote.IsEmpty());

	// And the default selection, which carries both, reaches it as well: an apply nobody narrowed is the path that
	// used to provision the channel for everybody, and it must not have quietly stopped doing so.
	Seed();
	Controller->ClearSchemaApplySelection();
	Controller->ApplySchemaSync();
	TestTrue(TEXT("an apply nobody narrowed still creates the channel first"),
		Controller->GetSchemaSyncReport().StatusNote.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioApplyEverythingSkipsWhatCannotBePickedTest,
	"CrowdySDK.CrowdyStudio.ApplyEverythingSkipsWhatCannotBePicked", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioApplyEverythingSkipsWhatCannotBePickedTest::RunTest(const FString& /*Parameters*/)
{
	// One effect whose target model does not resolve leaves a function in the plan that nothing can identify. It
	// cannot be ticked, and picking it anyway makes the WHOLE plan unsendable, so an apply nobody narrowed - which
	// picks everything - would refuse every unrelated change in the app because of it, while unticking any single
	// row would release them all. Everything means everything that can be picked.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);

	auto Seed = [&Controller]()
	{
		FCrowdyStudioControllerTestAccess::SeedPendingSchemaSync(*Controller, /*PlannedAppId*/ 7,
			{ AppStateType(TEXT("Knight")) },
			{ AppStateProp(TEXT("Knight"), TEXT("hp")) },
			{ AppStateFunction(FString(), TEXT("orphan")),
			  AppStateFunction(TEXT("Knight"), TEXT("take_damage")) },
			{}, {});
	};

	Seed();
	Controller->ClearSchemaApplySelection();
	Controller->ApplySchemaSync();

	// Reaching the walk at all is the point, and the count says exactly which changes went: the model, its attribute
	// and the function that has one. There is no sign-in, so the first operation fails and leaves that count behind.
	TestTrue(TEXT("the ordinary changes are sent, and all of them"),
		Controller->GetSchemaSyncReport().StatusNote.Contains(TEXT("Applied 0 of 3")));

	// The refusal itself is intact: it belongs to a selection that actually names the unidentifiable function, which
	// is the case nothing can answer safely.
	Seed();
	const TArray<FCrowdyApplyUnit> Units =
		CrowdyApplySelection::BuildUnits(Controller->MakeSchemaApplyPlanInput());

	FCrowdyApplyUnit Orphan;
	if (!TestTrue(TEXT("the plan holds the function with no model"),
		AppStateUnitFor(Units, ECrowdyApplyKind::Function, FString(), TEXT("orphan"), Orphan)))
	{
		return false;
	}

	Controller->SetSchemaApplySelection({ Orphan.IdentityKey() });
	Controller->ApplySchemaSync();
	TestTrue(TEXT("picking the unidentifiable function is still refused, by name"),
		Controller->GetStatusMessage().Contains(TEXT("have no model")));
	TestTrue(TEXT("and that refusal never reached a walk"),
		Controller->GetSchemaSyncReport().StatusNote.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioApplyWalkIgnoresARePlanUnderItTest,
	"CrowdySDK.CrowdyStudio.ApplyWalkIgnoresARePlanUnderIt", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioApplyWalkIgnoresARePlanUnderItTest::RunTest(const FString& /*Parameters*/)
{
	// Nothing stops a fresh plan from starting while a walk is still sending: pressing Preview changes mid-send
	// refills all five pending arrays from a new diff. The walk addresses those arrays by plan INDEX, so a reply
	// landing afterwards would strike entries of the NEW plan out at the OLD plan's positions - entries nobody sent -
	// and if the count reached zero the readiness pill would read Ready over a model that was never created.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);

	FCrowdyStudioControllerTestAccess::SeedPendingSchemaSync(*Controller, /*PlannedAppId*/ 7,
		{ AppStateType(TEXT("Knight")), AppStateType(TEXT("Goblin")) }, {}, {}, {}, {});
	const uint64 FirstPlan = FCrowdyStudioControllerTestAccess::GetSchemaPlanGeneration(*Controller);

	const TArray<FCrowdyApplyUnit> Units =
		CrowdyApplySelection::BuildUnits(Controller->MakeSchemaApplyPlanInput());
	FCrowdyApplyUnit KnightType;
	if (!TestTrue(TEXT("the first plan holds the Knight model"),
		AppStateUnitFor(Units, ECrowdyApplyKind::Type, FString(), TEXT("Knight"), KnightType)))
	{
		return false;
	}

	// Preview changes, pressed while the walk is sending. A different diff entirely, and its only entry sits at the
	// index the walk is about to strike out.
	FCrowdyStudioControllerTestAccess::SeedPendingSchemaSync(*Controller, /*PlannedAppId*/ 7,
		{ AppStateType(TEXT("Wizard")) }, {}, {}, {}, {});
	TestTrue(TEXT("a re-plan is a different plan"),
		FCrowdyStudioControllerTestAccess::GetSchemaPlanGeneration(*Controller) != FirstPlan);

	FCrowdyStudioControllerTestAccess::FinishSchemaUpsertWalkForGeneration(*Controller, { KnightType }, FirstPlan);

	if (TestEqual(TEXT("the new plan keeps its entry"),
		FCrowdyStudioControllerTestAccess::GetPendingTypes(*Controller).Num(), 1))
	{
		TestEqual(TEXT("and it is the model the new plan actually holds"),
			FCrowdyStudioControllerTestAccess::GetPendingTypes(*Controller)[0].Type.TypeName, FString(TEXT("Wizard")));
	}
	TestFalse(TEXT("a walk that outlived its plan cannot report the app as applied"),
		Controller->GetSchemaSyncReport().bApplied);
	TestTrue(TEXT("and the app still reads as drifted"),
		Controller->GetSchemaReadiness() != ECrowdyStudioReadiness::Ready);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioApplyPartialRestatesTheCountsTest,
	"CrowdySDK.CrowdyStudio.ApplyPartialRestatesTheCounts", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioApplyPartialRestatesTheCountsTest::RunTest(const FString& /*Parameters*/)
{
	// The panel prints the report's per-kind counts beside the banner saying how many changes were not sent. Left
	// describing the whole plan after a partial send, those two numbers contradict each other on the same screen.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);
	FCrowdyStudioControllerTestAccess::SeedPendingSchemaSync(*Controller, /*PlannedAppId*/ 7,
		{ AppStateType(TEXT("Knight")), AppStateType(TEXT("Goblin")) },
		{ AppStateProp(TEXT("Knight"), TEXT("hp")) },
		{}, {}, {});

	TestEqual(TEXT("the plan counts both models"), Controller->GetSchemaSyncReport().TypesToCreate, 2);
	TestEqual(TEXT("and its attribute"), Controller->GetSchemaSyncReport().PropsToCreate, 1);

	const TArray<FCrowdyApplyUnit> Units =
		CrowdyApplySelection::BuildUnits(Controller->MakeSchemaApplyPlanInput());
	FCrowdyApplyUnit KnightType;
	FCrowdyApplyUnit KnightHp;
	if (!TestTrue(TEXT("the plan holds the Knight model"),
			AppStateUnitFor(Units, ECrowdyApplyKind::Type, FString(), TEXT("Knight"), KnightType))
		|| !TestTrue(TEXT("the plan holds hp on Knight"),
			AppStateUnitFor(Units, ECrowdyApplyKind::Attribute, TEXT("Knight"), TEXT("hp"), KnightHp)))
	{
		return false;
	}

	FCrowdyStudioControllerTestAccess::FinishSchemaUpsertWalk(*Controller, { KnightType, KnightHp });

	TestEqual(TEXT("only the unsent model is still counted"),
		Controller->GetSchemaSyncReport().TypesToCreate, 1);
	TestEqual(TEXT("the sent attribute is no longer counted"),
		Controller->GetSchemaSyncReport().PropsToCreate, 0);
	TestEqual(TEXT("and the total agrees with what is still pending"),
		Controller->GetSchemaSyncReport().UpsertCount(), 1);

	// The per-entity lines are the same statement in words, so they follow the counts down or the panel lists
	// entities it has just said are gone.
	if (TestEqual(TEXT("one line per remaining change"), Controller->GetSchemaSyncReport().Lines.Num(), 1))
	{
		TestTrue(TEXT("and it names the change that stayed behind"),
			Controller->GetSchemaSyncReport().Lines[0].Contains(TEXT("Goblin")));
	}
	TestTrue(TEXT("the banner still says how many were left"),
		Controller->GetSchemaSyncReport().StatusNote.Contains(TEXT("1 more")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioApplyStoppedPartwayRereadsWhatItWroteTest,
	"CrowdySDK.CrowdyStudio.ApplyStoppedPartwayRereadsWhatItWrote", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioApplyStoppedPartwayRereadsWhatItWroteTest::RunTest(const FString& /*Parameters*/)
{
	// A walk that stopped on operation thirteen still WROTE the twelve before it. Those entities exist on the server
	// now, so the retained plan verdict and the three browsed lists are judgements about a server that has moved, and
	// every row would keep rendering an answer computed before the write.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);
	FCrowdyStudioControllerTestAccess::SeedPendingSchemaSync(*Controller, /*PlannedAppId*/ 7,
		{ AppStateType(TEXT("Knight")) }, {}, {}, {}, {});
	FCrowdyStudioControllerTestAccess::SeedModelSnapshot(*Controller, 7);

	// The three lists stand as read for this app, so a re-read is visible as their leaving that state.
	for (const ECrowdyModelFamily Family :
		{ ECrowdyModelFamily::Models, ECrowdyModelFamily::Functions, ECrowdyModelFamily::Automations })
	{
		FCrowdyStudioControllerTestAccess::SetFamilyLoad(*Controller, Family, ECrowdyModelLoadState::Loaded,
			/*AppId*/ 7, /*Serial*/ 0);
	}

	// A walk that got nowhere has invalidated nothing, so it must leave all of that alone.
	FCrowdyStudioControllerTestAccess::StopSchemaUpsertWalkAfter(*Controller, /*Total*/ 30, /*Completed*/ 0);
	TestNotNull(TEXT("a walk that wrote nothing leaves the plan verdict standing"),
		Controller->GetModelSnapshot().Get());
	TestTrue(TEXT("and does not re-read the models"),
		Controller->GetFamilyLoadState(ECrowdyModelFamily::Models) == ECrowdyModelLoadState::Loaded);

	FCrowdyStudioControllerTestAccess::StopSchemaUpsertWalkAfter(*Controller, /*Total*/ 30, /*Completed*/ 12);

	TestNull(TEXT("a walk that wrote twelve drops the verdict computed before them"),
		Controller->GetModelSnapshot().Get());
	for (const ECrowdyModelFamily Family :
		{ ECrowdyModelFamily::Models, ECrowdyModelFamily::Functions, ECrowdyModelFamily::Automations })
	{
		TestFalse(FString::Printf(TEXT("family %d is re-read after a partial write"), static_cast<int32>(Family)),
			Controller->GetFamilyLoadState(Family) == ECrowdyModelLoadState::Loaded);
	}
	// The banner the walk already wrote is untouched by any of that.
	TestTrue(TEXT("and the banner still says how far it got"),
		Controller->GetSchemaSyncReport().StatusNote.Contains(TEXT("Applied 12 of 30")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioFilteredFunctionReplyCannotReplaceTheMirrorTest,
	"CrowdySDK.CrowdyStudio.FilteredFunctionReplyCannotReplaceTheMirror", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioFilteredFunctionReplyCannotReplaceTheMirrorTest::RunTest(const FString& /*Parameters*/)
{
	// Replies can arrive in any order. A read narrowed to one container type claims no family, so nothing but a
	// number of its own can stop it landing after a whole-app read issued later and leaving the list holding a
	// handful of one model's functions, with the count printed and the views told to repaint from it.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);

	int32 Announced = 0;
	Controller->OnFunctionsChanged.AddLambda([&Announced]() { ++Announced; });

	// One model's functions asked for, then the whole app's, which is what pressing Refresh behind an open drill-in
	// does.
	const uint64 NarrowRead = FCrowdyStudioControllerTestAccess::BeginFunctionRead(*Controller);
	const uint64 WideFamily =
		FCrowdyStudioControllerTestAccess::IssueFamilyRead(*Controller, ECrowdyModelFamily::Functions);
	const uint64 WideRead = FCrowdyStudioControllerTestAccess::BeginFunctionRead(*Controller);

	FCrowdyStudioControllerTestAccess::LandFunctionsReply(*Controller,
		{ TEXT("take_damage"), TEXT("heal"), TEXT("regen") }, /*bUnfiltered*/ true, /*AppId*/ 7, WideFamily, WideRead);
	TestEqual(TEXT("the whole-app read fills the app-wide list"), Controller->GetUnfilteredFunctions().Num(), 3);
	TestEqual(TEXT("and the mirror, being the newest read"), Controller->GetFunctions().Num(), 3);
	TestEqual(TEXT("and is announced once"), Announced, 1);

	// The narrowed read, issued first, answers last.
	FCrowdyStudioControllerTestAccess::LandFunctionsReply(*Controller,
		{ TEXT("take_damage") }, /*bUnfiltered*/ false, /*AppId*/ 7, /*FamilySerial*/ 0, NarrowRead);

	TestEqual(TEXT("a superseded narrowed reply does not narrow the mirror"), Controller->GetFunctions().Num(), 3);
	TestEqual(TEXT("and never reaches the app-wide list"), Controller->GetUnfilteredFunctions().Num(), 3);
	TestEqual(TEXT("and announces nothing, because it changed nothing"), Announced, 1);
	TestTrue(TEXT("nor does it print its own count"),
		Controller->GetStatusMessage().Contains(TEXT("3 function(s).")));

	// A narrowed read that IS the newest still owns the mirror, or a drill-in could never show one model's list.
	const uint64 NewestNarrow = FCrowdyStudioControllerTestAccess::BeginFunctionRead(*Controller);
	FCrowdyStudioControllerTestAccess::LandFunctionsReply(*Controller,
		{ TEXT("take_damage") }, /*bUnfiltered*/ false, /*AppId*/ 7, /*FamilySerial*/ 0, NewestNarrow);
	TestEqual(TEXT("the newest narrowed read takes the mirror"), Controller->GetFunctions().Num(), 1);
	TestEqual(TEXT("without touching the app-wide list"), Controller->GetUnfilteredFunctions().Num(), 3);
	TestEqual(TEXT("and is announced"), Announced, 2);

	// An app switch moves the numbering on, so a read in flight across a switch away and back can never match again
	// however the ids line up.
	const uint64 AcrossTheSwitch = FCrowdyStudioControllerTestAccess::BeginFunctionRead(*Controller);
	FCrowdyStudioControllerTestAccess::ClearAppScopedState(*Controller);
	FCrowdyStudioControllerTestAccess::LandFunctionsReply(*Controller,
		{ TEXT("stale") }, /*bUnfiltered*/ false, /*AppId*/ 7, /*FamilySerial*/ 0, AcrossTheSwitch);
	TestEqual(TEXT("a read in flight across an app switch cannot refill the mirror"),
		Controller->GetFunctions().Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioFamilyReplyForAnOldSerialIsIgnoredTest,
	"CrowdySDK.CrowdyStudio.FamilyReplyForAnOldSerialIsIgnored", CrowdyStudioAppScopedStateTestFlags)

bool FCrowdyStudioFamilyReplyForAnOldSerialIsIgnoredTest::RunTest(const FString& /*Parameters*/)
{
	// The app a read was issued for cannot decide on its own whether its reply is still wanted: a user who switches
	// away and back selects the SAME app, so the ids compare equal again while the list that reply was issued
	// against was emptied in between. Landed anyway, an older read overwrites a newer list and marks it as read.
	const TSharedRef<FCrowdyStudioController> Controller = MakeShared<FCrowdyStudioController>();
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);

	const uint64 First = FCrowdyStudioControllerTestAccess::IssueFamilyRead(*Controller, ECrowdyModelFamily::Models);
	const uint64 Second = FCrowdyStudioControllerTestAccess::IssueFamilyRead(*Controller, ECrowdyModelFamily::Models);
	TestTrue(TEXT("an issued read reads as loading, not as an app with no models"),
		Controller->GetFamilyLoadState(ECrowdyModelFamily::Models) == ECrowdyModelLoadState::Loading);

	// The first, superseded read answers last, which is the ordering nothing on the wire forbids.
	FCrowdyStudioControllerTestAccess::LandContainerTypesReply(*Controller,
		{ TEXT("Stale"), TEXT("AlsoStale") }, /*AppId*/ 7, First);
	TestTrue(TEXT("a superseded reply does not mark the family read"),
		Controller->GetFamilyLoadState(ECrowdyModelFamily::Models) == ECrowdyModelLoadState::Loading);
	TestEqual(TEXT("and does not fill the list"), Controller->GetContainerTypes().Num(), 0);

	// The newest read still has to be able to answer, or nothing would ever leave the loading state.
	FCrowdyStudioControllerTestAccess::LandContainerTypesReply(*Controller, { TEXT("Knight") }, /*AppId*/ 7, Second);
	TestTrue(TEXT("the newest reply marks the family read"),
		Controller->GetFamilyLoadState(ECrowdyModelFamily::Models) == ECrowdyModelLoadState::Loaded);
	if (TestEqual(TEXT("and fills the list"), Controller->GetContainerTypes().Num(), 1))
	{
		TestEqual(TEXT("with its own models"),
			Controller->GetContainerTypes()[0]->TypeName, FString(TEXT("Knight")));
	}

	// A reply for an app that is no longer selected is dropped even though the family still remembers that app.
	const uint64 Third = FCrowdyStudioControllerTestAccess::IssueFamilyRead(*Controller, ECrowdyModelFamily::Models);
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 8);
	TestFalse(TEXT("a reply whose app is no longer selected is dropped"),
		FCrowdyStudioControllerTestAccess::LandFamilyReply(*Controller, ECrowdyModelFamily::Models, 7, Third,
			/*bSuccess*/ true));

	// And the app-switch reset moves the numbering on, so a read in flight across a switch away and back can never
	// match again however the ids line up.
	FCrowdyStudioControllerTestAccess::SetSelectedAppId(*Controller, 7);
	const uint64 Fourth = FCrowdyStudioControllerTestAccess::IssueFamilyRead(*Controller, ECrowdyModelFamily::Models);
	FCrowdyStudioControllerTestAccess::ClearAppScopedState(*Controller);
	TestFalse(TEXT("an app switch drops the family read that was in flight across it"),
		FCrowdyStudioControllerTestAccess::LandFamilyReply(*Controller, ECrowdyModelFamily::Models, 7, Fourth,
			/*bSuccess*/ true));
	TestTrue(TEXT("and leaves the family saying nobody has asked"),
		Controller->GetFamilyLoadState(ECrowdyModelFamily::Models) == ECrowdyModelLoadState::NeverRequested);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
