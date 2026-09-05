#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/GameModel/CrowdyModelChangeActions.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h" // FCrowdyMutationApplied

namespace
{
	constexpr EAutomationTestFlags CrowdyModelChangeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Distinct helper names (not the CrowdyGameModelApplyTests ones) so the two test TUs never collide in a
	// unity build.
	UCrowdyGameModelSubsystem* MakeChangeModel()
	{
		return NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	}

	UCrowdyGameModelTestTarget* MakeChangeTarget()
	{
		return NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	}

	TSharedPtr<FJsonObject> OneNumber(const FString& Key, double Value)
	{
		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(Key, Value);
		return Object;
	}

	TSharedPtr<FJsonObject> OneString(const FString& Key, const FString& Value)
	{
		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(Key, Value);
		return Object;
	}
}

// The actor-bound re-pull path (ApplyStateToContainer) broadcasts OnModelAttributeChanged once per changed key,
// carrying the bound Target, the resolved ModelId, the server key, and the client's last-seen old value plus the
// new canonical value. An unchanged re-apply is silent, and an attribute with NO OnRep still broadcasts (the
// zero-setup payoff: a global observer needs no per-attribute notify).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelChangeFromStateTest,
	"CrowdySDK.GameModel.ChangeEventFromState", CrowdyModelChangeTestFlags)
bool FCrowdyModelChangeFromStateTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeChangeModel();
	UCrowdyGameModelTestTarget* Container = MakeChangeTarget();
	UCrowdyGameModelTestTarget* Observer = MakeChangeTarget();
	if (!TestNotNull(TEXT("subsystem"), Model) || !TestNotNull(TEXT("container"), Container)
		|| !TestNotNull(TEXT("observer"), Observer))
	{
		return false;
	}

	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("container-abc"));

	// First apply of hp: a new key -> one broadcast, empty old, canonical new, correct Target + ModelId.
	Model->ApplyStateToContainer(NetID, Container, OneNumber(TEXT("hp"), 87));
	TestEqual(TEXT("one broadcast on first change"), Observer->AttributeChangedCount, 1);
	TestTrue(TEXT("Target is the bound container"), Observer->LastAttrTarget.Get() == Container);
	TestEqual(TEXT("ModelId resolved from the binding"), Observer->LastAttrModelId, FString(TEXT("container-abc")));
	TestEqual(TEXT("attribute key"), Observer->LastAttrKey, FName(TEXT("hp")));
	TestEqual(TEXT("old is empty for a new key"), Observer->LastAttrOldJson, FString());
	TestEqual(TEXT("new is the canonical value"), Observer->LastAttrNewJson, FString(TEXT("87")));

	// Unchanged re-apply: nothing changed -> no broadcast.
	Model->ApplyStateToContainer(NetID, Container, OneNumber(TEXT("hp"), 87));
	TestEqual(TEXT("unchanged re-apply is silent"), Observer->AttributeChangedCount, 1);

	// Change hp: old is the client's last-seen value (not any server-claimed old), new is the changed value.
	Model->ApplyStateToContainer(NetID, Container, OneNumber(TEXT("hp"), 80));
	TestEqual(TEXT("second broadcast on change"), Observer->AttributeChangedCount, 2);
	TestEqual(TEXT("old is the previous cached value"), Observer->LastAttrOldJson, FString(TEXT("87")));
	TestEqual(TEXT("new is the changed value"), Observer->LastAttrNewJson, FString(TEXT("80")));

	// gold is a Model attribute with NO OnRep, yet the change delegate still fires - a global observer reacts to
	// it without a per-attribute notify.
	Model->ApplyStateToContainer(NetID, Container, OneNumber(TEXT("gold"), 5));
	TestEqual(TEXT("no-OnRep attribute still broadcasts"), Observer->AttributeChangedCount, 3);
	TestEqual(TEXT("no-OnRep attribute key"), Observer->LastAttrKey, FName(TEXT("gold")));
	TestEqual(TEXT("gold member written"), Container->Gold, 5);

	return true;
}

// The confirmed-invoke echo path (ApplyMutationsToContainer) broadcasts the same way, and the old value it
// carries is the client's cached value, not the mutation's server-supplied OldValueJson.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelChangeFromMutationsTest,
	"CrowdySDK.GameModel.ChangeEventFromMutations", CrowdyModelChangeTestFlags)
bool FCrowdyModelChangeFromMutationsTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeChangeModel();
	UCrowdyGameModelTestTarget* Container = MakeChangeTarget();
	UCrowdyGameModelTestTarget* Observer = MakeChangeTarget();
	if (!TestNotNull(TEXT("subsystem"), Model) || !TestNotNull(TEXT("container"), Container)
		|| !TestNotNull(TEXT("observer"), Observer))
	{
		return false;
	}

	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("c-2"));

	// The mutation claims a server old of "100", but the client's cache is empty, so the broadcast old is empty.
	TArray<FCrowdyMutationApplied> Mutations;
	FCrowdyMutationApplied Mutation;
	Mutation.Key = TEXT("hp");
	Mutation.OldValueJson = TEXT("100");
	Mutation.NewValueJson = TEXT("87");
	Mutations.Add(Mutation);

	Model->ApplyMutationsToContainer(NetID, Container, Mutations);
	TestEqual(TEXT("one broadcast from the mutation"), Observer->AttributeChangedCount, 1);
	TestEqual(TEXT("ModelId resolved"), Observer->LastAttrModelId, FString(TEXT("c-2")));
	TestEqual(TEXT("old is the client cache (empty), not the server-claimed old"), Observer->LastAttrOldJson, FString());
	TestEqual(TEXT("new is the confirmed value"), Observer->LastAttrNewJson, FString(TEXT("87")));

	// Replaying the same confirmed value is idempotent -> no broadcast.
	Model->ApplyMutationsToContainer(NetID, Container, Mutations);
	TestEqual(TEXT("identical mutation is silent"), Observer->AttributeChangedCount, 1);

	// A change now carries the cached "87" as old.
	FCrowdyMutationApplied Next;
	Next.Key = TEXT("hp");
	Next.NewValueJson = TEXT("70");
	Model->ApplyMutationsToContainer(NetID, Container, { Next });
	TestEqual(TEXT("second broadcast on change"), Observer->AttributeChangedCount, 2);
	TestEqual(TEXT("old is the cached value"), Observer->LastAttrOldJson, FString(TEXT("87")));
	TestEqual(TEXT("new is the changed value"), Observer->LastAttrNewJson, FString(TEXT("70")));

	return true;
}

// A free/data container (no actor) broadcasts with a null Target and the container id as ModelId; a removed key
// broadcasts an empty NewValueJson so a listener can distinguish a removal from a value change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelChangeFromFreeContainerTest,
	"CrowdySDK.GameModel.ChangeEventFromFreeContainer", CrowdyModelChangeTestFlags)
bool FCrowdyModelChangeFromFreeContainerTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeChangeModel();
	UCrowdyGameModelTestTarget* Observer = MakeChangeTarget();
	if (!TestNotNull(TEXT("subsystem"), Model) || !TestNotNull(TEXT("observer"), Observer))
	{
		return false;
	}

	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	const FString ContainerId = TEXT("free-1");

	// A new string key: null Target, container id as ModelId, canonical JSON string as the new value.
	Model->ApplyDataContainerState(ContainerId, OneString(TEXT("slot0"), TEXT("sword")));
	TestEqual(TEXT("one broadcast for a new key"), Observer->AttributeChangedCount, 1);
	TestNull(TEXT("free container has no Target"), Observer->LastAttrTarget.Get());
	TestEqual(TEXT("ModelId is the container id"), Observer->LastAttrModelId, ContainerId);
	TestEqual(TEXT("key"), Observer->LastAttrKey, FName(TEXT("slot0")));
	TestEqual(TEXT("new is the canonical JSON string"), Observer->LastAttrNewJson, FString(TEXT("\"sword\"")));

	// Change the value.
	Model->ApplyDataContainerState(ContainerId, OneString(TEXT("slot0"), TEXT("shield")));
	TestEqual(TEXT("second broadcast on value change"), Observer->AttributeChangedCount, 2);
	TestEqual(TEXT("old is the previous string"), Observer->LastAttrOldJson, FString(TEXT("\"sword\"")));
	TestEqual(TEXT("new is the changed string"), Observer->LastAttrNewJson, FString(TEXT("\"shield\"")));

	// Pull an empty state: slot0 is no longer present -> removed, broadcast with an empty new value.
	Model->ApplyDataContainerState(ContainerId, MakeShared<FJsonObject>());
	TestEqual(TEXT("third broadcast for a removal"), Observer->AttributeChangedCount, 3);
	TestEqual(TEXT("removed key"), Observer->LastAttrKey, FName(TEXT("slot0")));
	TestEqual(TEXT("removal old is the last value"), Observer->LastAttrOldJson, FString(TEXT("\"shield\"")));
	TestTrue(TEXT("removal new is empty"), Observer->LastAttrNewJson.IsEmpty());

	return true;
}

// The pure listener filter: an explicit Target wins over a ModelId, a destroyed Target (null) matches nothing
// (never silently widening to global), a ModelId matches by string, and no filter passes everything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelListenFilterTest,
	"CrowdySDK.GameModel.ListenFilterDecision", CrowdyModelChangeTestFlags)
bool FCrowdyModelListenFilterTest::RunTest(const FString& Parameters)
{
	UObject* A = MakeChangeTarget();
	UObject* B = MakeChangeTarget();
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B))
	{
		return false;
	}

	using CrowdyModelListen::PassesFilter;

	// No filter -> global: everything passes, including a null Target (a free container).
	TestTrue(TEXT("global passes a value"), PassesFilter(false, nullptr, FString(), A, TEXT("cid")));
	TestTrue(TEXT("global passes a null-Target free container"), PassesFilter(false, nullptr, FString(), nullptr, TEXT("cid")));

	// Target filter: matches only that exact object.
	TestTrue(TEXT("target matches self"), PassesFilter(true, A, FString(), A, TEXT("cid")));
	TestFalse(TEXT("target rejects another object"), PassesFilter(true, A, FString(), B, TEXT("cid")));
	// A destroyed Target (now null) matches nothing rather than becoming a global listener.
	TestFalse(TEXT("stale target matches nothing"), PassesFilter(true, nullptr, FString(), A, TEXT("cid")));

	// ModelId filter: matches by string.
	TestTrue(TEXT("modelId matches"), PassesFilter(false, nullptr, TEXT("cid-1"), A, TEXT("cid-1")));
	TestFalse(TEXT("modelId rejects a different id"), PassesFilter(false, nullptr, TEXT("cid-1"), A, TEXT("cid-2")));

	// Target takes precedence over a ModelId: the ModelId is ignored when a Target filter is set.
	TestTrue(TEXT("target wins over a mismatched modelId"), PassesFilter(true, A, TEXT("cid-1"), A, TEXT("cid-2")));
	TestFalse(TEXT("target mismatch rejects even when modelId matches"), PassesFilter(true, A, TEXT("cid-1"), B, TEXT("cid-1")));

	return true;
}

// A confirmed free/data invoke applies its changed-subset as a MERGE (ApplyDataContainerMutations): an untouched
// cached key survives and fires no change event, and only the actually-changed key broadcasts. Guards against the
// full-replace regression where a mutations-only state evicted untouched keys and reported them as removed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelInvokeMergePreservesKeysTest,
	"CrowdySDK.GameModel.InvokeMergePreservesUntouchedKeys", CrowdyModelChangeTestFlags)
bool FCrowdyModelInvokeMergePreservesKeysTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeChangeModel();
	UCrowdyGameModelTestTarget* Observer = MakeChangeTarget();
	if (!TestNotNull(TEXT("subsystem"), Model) || !TestNotNull(TEXT("observer"), Observer))
	{
		return false;
	}

	const FString ContainerId = TEXT("inv-1");

	// Seed a two-key container BEFORE listening, so the seed's changes do not count against the observer.
	const TSharedPtr<FJsonObject> Seed = MakeShared<FJsonObject>();
	Seed->SetNumberField(TEXT("gold"), 10);
	Seed->SetNumberField(TEXT("potions"), 3);
	Model->ApplyDataContainerState(ContainerId, Seed);

	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	// A confirmed invoke that changed only gold.
	TArray<FCrowdyMutationApplied> Mutations;
	FCrowdyMutationApplied Mutation;
	Mutation.Key = TEXT("gold");
	Mutation.NewValueJson = TEXT("7");
	Mutations.Add(Mutation);
	Model->ApplyDataContainerMutations(ContainerId, Mutations);

	// Exactly one change (gold); potions is neither broadcast nor evicted.
	TestEqual(TEXT("only the changed key broadcasts"), Observer->AttributeChangedCount, 1);
	TestEqual(TEXT("changed key is gold"), Observer->LastAttrKey, FName(TEXT("gold")));
	TestEqual(TEXT("gold old"), Observer->LastAttrOldJson, FString(TEXT("10")));
	TestEqual(TEXT("gold new"), Observer->LastAttrNewJson, FString(TEXT("7")));

	FString PotionsJson;
	TestTrue(TEXT("untouched potions key survives the merge"),
		Model->TryGetContainerValueJson(ContainerId, FName(TEXT("potions")), PotionsJson));
	TestEqual(TEXT("potions value intact"), PotionsJson, FString(TEXT("3")));

	return true;
}

// The actor-bound change delegate fires only for real Server Owned attributes: a non-attribute server key present
// in the pulled state updates the diff cache but does not broadcast (it has no client property to read back), so
// the delegate matches the OnRep basis.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelChangeSkipsNonAttributeTest,
	"CrowdySDK.GameModel.ChangeEventSkipsNonAttributeKey", CrowdyModelChangeTestFlags)
bool FCrowdyModelChangeSkipsNonAttributeTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeChangeModel();
	UCrowdyGameModelTestTarget* Container = MakeChangeTarget();
	UCrowdyGameModelTestTarget* Observer = MakeChangeTarget();
	if (!TestNotNull(TEXT("subsystem"), Model) || !TestNotNull(TEXT("container"), Container)
		|| !TestNotNull(TEXT("observer"), Observer))
	{
		return false;
	}

	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("hero-1"));

	// A pulled state carrying a real attribute (hp) and a non-attribute server key (notanattribute).
	const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetNumberField(TEXT("hp"), 5);
	State->SetNumberField(TEXT("notanattribute"), 9);
	Model->ApplyStateToContainer(NetID, Container, State);

	// Only hp broadcasts; the non-attribute key does not.
	TestEqual(TEXT("only the real attribute broadcasts"), Observer->AttributeChangedCount, 1);
	TestEqual(TEXT("broadcast key is hp"), Observer->LastAttrKey, FName(TEXT("hp")));
	TestEqual(TEXT("hp OnRep still fired"), Container->HpOnRepCount, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
