#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyAttributeChange.h"
#include "Replication/GameModel/CrowdyEntityClassContainerTestTypes.h"
#include "Replication/GameModel/CrowdyGameModelSubscriberTestTypes.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"

// Covers the Game Model plane's half of ICrowdyEntitySubscriber: an entity represented here by no object of its
// own still receives the server-owned values an apply settled for it, keyed on the cache advancing rather than on
// any particular attribute. CrowdyMass's half (landing a relayed value in a fragment) is covered by
// CrowdySDK.Mass.ModelField.* in that plugin's own test tree.
namespace
{
	constexpr EAutomationTestFlags CrowdyGameModelRelayTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Distinct helper names so this translation unit never collides with the other Game Model test helpers when a
	// unity build merges them.
	UCrowdyGameModelSubsystem* MakeRelayModel()
	{
		UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
		// The relay reads its subscriber out of the router's one slot, and a headless model subsystem has no world
		// to resolve a router from, so one is injected here. Registration then goes through the router exactly as it
		// does at runtime, which is what makes these cases exercise the real world-travel rule.
		Model->SetEventRouterForTest(NewObject<UCrowdyEventRouter>(GetTransientPackage()));
		return Model;
	}

	UCrowdyGameModelSpySubscriber* MakeRelaySpy(UCrowdyGameModelSubsystem* Model)
	{
		UCrowdyGameModelSpySubscriber* Spy = NewObject<UCrowdyGameModelSpySubscriber>(GetTransientPackage());
		Model->GetEventRouterForTest()->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Spy));
		return Spy;
	}

	// What represents an observed entity here when it is not drawn as an actor: it declares no Server Owned
	// attribute at all, so every pulled value for it resolves no property on this object.
	UCrowdyEntityClassStandIn* MakeRelayStandIn()
	{
		return NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	}

	TSharedPtr<FJsonObject> RelayStateOf(const double Health, const double Armour)
	{
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetNumberField(TEXT("health"), Health);
		State->SetNumberField(TEXT("armour"), Armour);
		return State;
	}

	TSharedPtr<FJsonValue> RelayNum(const double Value)
	{
		return MakeShared<FJsonValueNumber>(Value);
	}

	TSharedPtr<FJsonValue> RelayStr(const TCHAR* Value)
	{
		return MakeShared<FJsonValueString>(Value);
	}
}

// The whole point of the seam: a pulled value reaches an entity whose local representation declares no Server
// Owned attribute at all, so there is no member to write and no notify to fire. Both keys arrive, with the
// container they came from, on the entity's own id. Nothing here names a particular attribute; the two keys are
// arbitrary and the relay never looks at what they are called.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelRelayReachesObjectlessEntityTest,
	"CrowdySDK.GameModel.ModelRelayReachesEntityWithNoObject", CrowdyGameModelRelayTestFlags)
bool FCrowdyGameModelRelayReachesObjectlessEntityTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeRelayModel();
	UCrowdyEntityClassStandIn* StandIn = MakeRelayStandIn();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("stand-in created"), StandIn))
	{
		return false;
	}

	UCrowdyGameModelSpySubscriber* Spy = MakeRelaySpy(Model);
	const FGuid NetID = FGuid::NewGuid();
	Spy->SetOwnedClass(NetID, UCrowdyGameModelTestTarget::StaticClass());
	Model->BindEntityContainerForTest(NetID, TEXT("container-crowd-1"));

	Model->ApplyStateToContainer(NetID, StandIn, RelayStateOf(87, 5));

	// Delivered by the time the apply returned. A relay that answered by starting a pull of its own could not have
	// delivered anything synchronously, whatever it eventually did.
	TestEqual(TEXT("the subscriber was handed the values once"), Spy->ApplyModelChangesCallCount, 1);
	TestTrue(TEXT("on the entity's own id"), Spy->LastEntityUUID == NetID);
	TestEqual(TEXT("naming the container they came from"), Spy->LastContainerId, FString(TEXT("container-crowd-1")));
	TestEqual(TEXT("carrying both keys"), Spy->LastChanges.Num(), 2);
	TestEqual(TEXT("with the first value"), Spy->FindLastNewValue(FName(TEXT("health"))), FString(TEXT("87")));
	TestEqual(TEXT("with the second value"), Spy->FindLastNewValue(FName(TEXT("armour"))), FString(TEXT("5")));

	// And nothing asked for another round trip: no pending-entity sweep was armed.
	TestEqual(TEXT("no sweep was armed"), Model->GetPendingSweepCountForTest(), 0);

	return true;
}

// These are CHANGES, not deliveries: the diff basis is a cache held per entity, so a re-pull of the same values
// hands the subscriber nothing at all, and a pull that moves one key hands it exactly that key. An implementation
// that relayed everything a pull returned would re-run every entity's apply on every heartbeat.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelRelayCarriesOnlyWhatMovedTest,
	"CrowdySDK.GameModel.ModelRelayCarriesOnlyWhatMoved", CrowdyGameModelRelayTestFlags)
bool FCrowdyGameModelRelayCarriesOnlyWhatMovedTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeRelayModel();
	UCrowdyEntityClassStandIn* StandIn = MakeRelayStandIn();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("stand-in created"), StandIn))
	{
		return false;
	}

	UCrowdyGameModelSpySubscriber* Spy = MakeRelaySpy(Model);
	const FGuid NetID = FGuid::NewGuid();
	Spy->SetOwnedClass(NetID, UCrowdyGameModelTestTarget::StaticClass());

	Model->ApplyStateToContainer(NetID, StandIn, RelayStateOf(87, 5));
	TestEqual(TEXT("the first pull is all new"), Spy->LastChanges.Num(), 2);

	Model->ApplyStateToContainer(NetID, StandIn, RelayStateOf(87, 5));
	TestEqual(TEXT("an identical re-pull hands over nothing"), Spy->ApplyModelChangesCallCount, 1);

	Model->ApplyStateToContainer(NetID, StandIn, RelayStateOf(80, 5));
	TestEqual(TEXT("a pull that moved one key is handed over"), Spy->ApplyModelChangesCallCount, 2);

	// A gate rather than a bare comparison, because the two reads below index this array. A delivery
	// regression that leaves it empty would otherwise be an engine bounds assert that takes the whole run
	// down, instead of this case failing by name and every other case still reporting.
	if (!TestEqual(TEXT("carrying that key alone"), Spy->LastChanges.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("which is the one that moved"), Spy->LastChangesContain(FName(TEXT("health"))));
	TestEqual(TEXT("with its previous value"), Spy->LastChanges[0].OldValueJson, FString(TEXT("87")));
	TestEqual(TEXT("and its new one"), Spy->LastChanges[0].NewValueJson, FString(TEXT("80")));

	return true;
}

// An entity the subscriber does not hold is not its business, however little the local object declares. This is
// what leaves the ordinary object-backed path exactly as it was: such a value is dropped as before rather than
// handed to whoever happens to be registered.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelRelaySkipsEntityNobodyHoldsTest,
	"CrowdySDK.GameModel.ModelRelaySkipsEntityNobodyHolds", CrowdyGameModelRelayTestFlags)
bool FCrowdyGameModelRelaySkipsEntityNobodyHoldsTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeRelayModel();
	UCrowdyEntityClassStandIn* StandIn = MakeRelayStandIn();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("stand-in created"), StandIn))
	{
		return false;
	}

	UCrowdyGameModelSpySubscriber* Spy = MakeRelaySpy(Model);
	const FGuid HeldNetID = FGuid::NewGuid();
	const FGuid OtherNetID = FGuid::NewGuid();
	Spy->SetOwnedClass(HeldNetID, UCrowdyGameModelTestTarget::StaticClass());

	Model->ApplyStateToContainer(OtherNetID, StandIn, RelayStateOf(87, 5));
	TestEqual(TEXT("an id the subscriber does not hold is never handed over"), Spy->ApplyModelChangesCallCount, 0);
	TestTrue(TEXT("but it was asked"), Spy->IsEntityKnownCallCount > 0);

	// The same pull for the id it does hold still lands, so the refusal above was about the id rather than about
	// the relay being off altogether.
	Model->ApplyStateToContainer(HeldNetID, StandIn, RelayStateOf(87, 5));
	TestEqual(TEXT("the held id is handed over"), Spy->ApplyModelChangesCallCount, 1);

	return true;
}

// Whether the subscriber HOLDS the id is the only question this path asks. An entity held with no class at all is
// a legitimate entity, and answering from the class instead would silently drop every server-owned value it ever
// receives while looking like a routing decision.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelRelayAsksOnlyWhetherHeldTest,
	"CrowdySDK.GameModel.ModelRelayAsksOnlyWhetherEntityIsHeld", CrowdyGameModelRelayTestFlags)
bool FCrowdyGameModelRelayAsksOnlyWhetherHeldTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeRelayModel();
	UCrowdyEntityClassStandIn* StandIn = MakeRelayStandIn();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("stand-in created"), StandIn))
	{
		return false;
	}

	UCrowdyGameModelSpySubscriber* Spy = MakeRelaySpy(Model);
	const FGuid NetID = FGuid::NewGuid();
	Spy->SetHeldWithNoClass(NetID);

	Model->ApplyStateToContainer(NetID, StandIn, RelayStateOf(87, 5));

	TestEqual(TEXT("an entity held with no class still receives its values"), Spy->ApplyModelChangesCallCount, 1);
	TestEqual(TEXT("carrying both keys"), Spy->LastChanges.Num(), 2);
	TestEqual(TEXT("and the class was never consulted"), Spy->GetEntityClassCallCount, 0);

	return true;
}

// A value the codec REJECTED never reaches the subscriber. What is handed over is exactly the set of keys whose
// cached value advanced, so a rejected write is in neither, and a later pull of the original value is still
// treated as unchanged. Relaying it would have written a forged aggregate into the entity's own storage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelRelayDropsRejectedValueTest,
	"CrowdySDK.GameModel.ModelRelayDropsRejectedValue", CrowdyGameModelRelayTestFlags)
bool FCrowdyGameModelRelayDropsRejectedValueTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeRelayModel();
	UCrowdyGameModelRichTarget* Target = NewObject<UCrowdyGameModelRichTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target))
	{
		return false;
	}

	UCrowdyGameModelSpySubscriber* Spy = MakeRelaySpy(Model);
	const FGuid NetID = FGuid::NewGuid();
	Spy->SetOwnedClass(NetID, UCrowdyGameModelRichTarget::StaticClass());

	const TSharedPtr<FJsonObject> Good = MakeShared<FJsonObject>();
	Good->SetArrayField(TEXT("scores"), {RelayNum(1), RelayNum(2)});
	Model->ApplyStateToContainer(NetID, Target, Good);
	TestEqual(TEXT("a valid value is handed over"), Spy->ApplyModelChangesCallCount, 1);

	// Wrong element type for the declared array: the codec refuses it and leaves the member alone. It reports the
	// refusal at Warning level, which does not fail an automation run.
	const TSharedPtr<FJsonObject> Forged = MakeShared<FJsonObject>();
	Forged->SetArrayField(TEXT("scores"), {RelayStr(TEXT("x")), RelayStr(TEXT("y"))});
	Model->ApplyStateToContainer(NetID, Target, Forged);
	TestEqual(TEXT("a rejected value is not handed over"), Spy->ApplyModelChangesCallCount, 1);

	// And it did not poison the cache: the original value is still what this client holds.
	Model->ApplyStateToContainer(NetID, Target, Good);
	TestEqual(TEXT("re-pulling the original is still a no-op"), Spy->ApplyModelChangesCallCount, 1);

	return true;
}

// A confirmed invoke's writes are as authoritative as a pull's and arrive without one, so they reach the entity's
// storage the same way. Without this a crowd entity's storage would sit on the value the last pull left until
// something happened to pull again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelRelayFollowsConfirmedInvokeTest,
	"CrowdySDK.GameModel.ModelRelayFollowsConfirmedInvoke", CrowdyGameModelRelayTestFlags)
bool FCrowdyGameModelRelayFollowsConfirmedInvokeTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeRelayModel();
	UCrowdyEntityClassStandIn* StandIn = MakeRelayStandIn();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("stand-in created"), StandIn))
	{
		return false;
	}

	UCrowdyGameModelSpySubscriber* Spy = MakeRelaySpy(Model);
	const FGuid NetID = FGuid::NewGuid();
	Spy->SetOwnedClass(NetID, UCrowdyGameModelTestTarget::StaticClass());

	FCrowdyMutationApplied Mutation;
	Mutation.Key = TEXT("health");
	Mutation.NewValueJson = TEXT("42");
	TArray<FCrowdyMutationApplied> Mutations;
	Mutations.Add(Mutation);
	Model->ApplyMutationsToContainer(NetID, StandIn, Mutations);

	TestEqual(TEXT("a confirmed invoke's write is handed over"), Spy->ApplyModelChangesCallCount, 1);
	TestEqual(TEXT("carrying that key"), Spy->FindLastNewValue(FName(TEXT("health"))), FString(TEXT("42")));

	// The invoke and the pull share one cache, so a pull returning the value the invoke already applied hands the
	// subscriber nothing: it has genuinely not moved since.
	const TSharedPtr<FJsonObject> Echo = MakeShared<FJsonObject>();
	Echo->SetNumberField(TEXT("health"), 42);
	Model->ApplyStateToContainer(NetID, StandIn, Echo);
	TestEqual(TEXT("a pull echoing it hands over nothing"), Spy->ApplyModelChangesCallCount, 1);

	return true;
}

// Two worlds are alive at once while travelling between levels, and the world being left tears down after the
// world being entered has registered. The newest registration holds the slot and a release by anyone else is
// ignored, so a departing world cannot silence the live one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelRelaySubscriberSlotSupersedeTest,
	"CrowdySDK.GameModel.ModelRelaySubscriberSlotSupersede", CrowdyGameModelRelayTestFlags)
bool FCrowdyGameModelRelaySubscriberSlotSupersedeTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeRelayModel();
	UCrowdyEntityClassStandIn* StandIn = MakeRelayStandIn();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("stand-in created"), StandIn))
	{
		return false;
	}

	UCrowdyGameModelSpySubscriber* Departing = MakeRelaySpy(Model);
	UCrowdyGameModelSpySubscriber* Arriving = MakeRelaySpy(Model);
	const FGuid NetID = FGuid::NewGuid();
	Departing->SetHeldWithNoClass(NetID);
	Arriving->SetHeldWithNoClass(NetID);

	// The departing world tears down last and releases; the slot must stay with the arriving one.
	Model->GetEventRouterForTest()->UnregisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Departing));

	Model->ApplyStateToContainer(NetID, StandIn, RelayStateOf(87, 5));
	TestEqual(TEXT("the arriving subscriber still receives"), Arriving->ApplyModelChangesCallCount, 1);
	TestEqual(TEXT("and the departing one receives nothing"), Departing->ApplyModelChangesCallCount, 0);

	// The holder's own release does empty the slot.
	Model->GetEventRouterForTest()->UnregisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Arriving));
	Model->ApplyStateToContainer(NetID, StandIn, RelayStateOf(70, 5));
	TestEqual(TEXT("nothing is delivered once the holder releases"), Arriving->ApplyModelChangesCallCount, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
