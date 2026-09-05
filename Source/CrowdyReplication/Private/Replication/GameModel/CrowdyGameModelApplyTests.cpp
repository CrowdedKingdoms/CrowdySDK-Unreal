#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyGameModelApplyTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	UCrowdyGameModelSubsystem* MakeSubsystem()
	{
		return NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	}

	UCrowdyGameModelTestTarget* MakeTarget()
	{
		return NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	}

	TSharedPtr<FJsonObject> StateOf(int32 Hp, int32 Mana, int32 Gold)
	{
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetNumberField(TEXT("hp"), Hp);
		State->SetNumberField(TEXT("mana"), Mana);
		State->SetNumberField(TEXT("gold"), Gold);
		return State;
	}
}

// ApplyState fires a parameterless CrowdyOnRep for exactly the keys whose value changed vs the cache the
// "feels like a replicated property" payoff and only those: a re-apply of unchanged values is silent, a
// Model key with no OnRep updates the cache but fires nothing, and a non-Model server key never fires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelApplyStateFiresOnRepTest,
	"CrowdySDK.GameModel.ApplyStateFiresOnRep", CrowdyGameModelApplyTestFlags)
bool FCrowdyGameModelApplyStateFiresOnRepTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeSubsystem();
	UCrowdyGameModelTestTarget* Target = MakeTarget();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();

	// First apply from an empty cache: every key is new -> hp & mana OnRep fire once; gold (no OnRep) is silent.
	Model->ApplyStateToContainer(NetID, Target, StateOf(87, 50, 5));
	TestEqual(TEXT("hp OnRep fired once"), Target->HpOnRepCount, 1);
	TestEqual(TEXT("mana OnRep fired once"), Target->ManaOnRepCount, 1);
	// The pulled values are written onto the live members (feels like a replicated property), even for the
	// no-OnRep attribute.
	TestEqual(TEXT("hp member written"), Target->Hp, 87);
	TestEqual(TEXT("mana member written"), Target->Mana, 50);
	TestEqual(TEXT("gold member written even without OnRep"), Target->Gold, 5);

	// Re-apply identical values: nothing changed -> no further OnRep.
	Model->ApplyStateToContainer(NetID, Target, StateOf(87, 50, 5));
	TestEqual(TEXT("hp OnRep not re-fired on unchanged"), Target->HpOnRepCount, 1);
	TestEqual(TEXT("mana OnRep not re-fired on unchanged"), Target->ManaOnRepCount, 1);

	// Change only hp: only hp OnRep fires again, and the member reflects the new value.
	Model->ApplyStateToContainer(NetID, Target, StateOf(80, 50, 5));
	TestEqual(TEXT("hp OnRep fires on change"), Target->HpOnRepCount, 2);
	TestEqual(TEXT("mana OnRep still not re-fired"), Target->ManaOnRepCount, 1);
	TestEqual(TEXT("hp member updated"), Target->Hp, 80);

	// A non-Model server key with a matching name never fires the map is CrowdyModel-only.
	const TSharedPtr<FJsonObject> Rogue = MakeShared<FJsonObject>();
	Rogue->SetNumberField(TEXT("notanattribute"), 999);
	Model->ApplyStateToContainer(NetID, Target, Rogue);
	TestEqual(TEXT("non-Model key fires no OnRep"), Target->HpOnRepCount, 2);

	return true;
}

// ApplyMutations echoes a confirmed invoke's writes into the cache and fires OnRep for changed keys, and is
// idempotent. The final assertion proves the two apply paths share ONE canonical cache: a pulled state equal
// to a previously-applied mutation fires nothing (guards the number/string canonicalization contract).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelApplyMutationsFiresOnRepTest,
	"CrowdySDK.GameModel.ApplyMutationsFiresOnRep", CrowdyGameModelApplyTestFlags)
bool FCrowdyGameModelApplyMutationsFiresOnRepTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeSubsystem();
	UCrowdyGameModelTestTarget* Target = MakeTarget();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();

	TArray<FCrowdyMutationApplied> Mutations;
	FCrowdyMutationApplied Mutation;
	Mutation.Key = TEXT("hp");
	Mutation.OldValueJson = TEXT("100");
	Mutation.NewValueJson = TEXT("87");
	Mutations.Add(Mutation);

	Model->ApplyMutationsToContainer(NetID, Target, Mutations);
	TestEqual(TEXT("hp OnRep fired from mutation"), Target->HpOnRepCount, 1);
	TestEqual(TEXT("hp member written from mutation"), Target->Hp, 87);

	// Replaying the same confirmed value is idempotent (canonical "87" already in cache).
	Model->ApplyMutationsToContainer(NetID, Target, Mutations);
	TestEqual(TEXT("hp OnRep not re-fired on identical mutation"), Target->HpOnRepCount, 1);

	// A pulled state that agrees with the cache (still 87) fires nothing the two apply paths share one cache,
	// so the mutation's "87" and the pulled number 87 canonicalize to the same string.
	const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetNumberField(TEXT("hp"), 87);
	Model->ApplyStateToContainer(NetID, Target, State);
	TestEqual(TEXT("shared cache: pulled-equal value fires no OnRep"), Target->HpOnRepCount, 1);

	// A pulled state that DIFFERS fires OnRep.
	const TSharedPtr<FJsonObject> Changed = MakeShared<FJsonObject>();
	Changed->SetNumberField(TEXT("hp"), 60);
	Model->ApplyStateToContainer(NetID, Target, Changed);
	TestEqual(TEXT("shared cache: differing pulled value fires OnRep"), Target->HpOnRepCount, 2);
	TestEqual(TEXT("hp member updated from pull"), Target->Hp, 60);

	return true;
}

// Non-integer types round-trip through both apply paths: a float and a string are written onto their live
// members, and the mutation path canonicalizes IDENTICALLY to the pull path so a re-pull of an unchanged
// float fires no OnRep. An int-only fixture masked a bare-scalar JSON-parse bug (mutation values never
// parsed -> member never written, cache stored the raw string -> spurious OnRep for floats/strings); this
// locks that fix in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelApplyFloatAndStringTest,
	"CrowdySDK.GameModel.ApplyHandlesFloatAndString", CrowdyGameModelApplyTestFlags)
bool FCrowdyGameModelApplyFloatAndStringTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeSubsystem();
	UCrowdyGameModelTestTarget* Target = MakeTarget();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();

	// Pulled state writes the float + string members and fires the float's OnRep.
	const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetNumberField(TEXT("speed"), 2.5);
	State->SetStringField(TEXT("title"), TEXT("Aria"));
	Model->ApplyStateToContainer(NetID, Target, State);
	TestEqual(TEXT("speed member written"), Target->Speed, 2.5f);
	TestEqual(TEXT("title member written"), Target->Title, FString(TEXT("Aria")));
	TestEqual(TEXT("speed OnRep fired"), Target->SpeedOnRepCount, 1);

	// Re-pull the same float: canonicalization is stable, so no spurious OnRep.
	Model->ApplyStateToContainer(NetID, Target, State);
	TestEqual(TEXT("unchanged float fires no OnRep"), Target->SpeedOnRepCount, 1);

	// The mutation path parses + canonicalizes a float string IDENTICALLY to the pull path: applying a float
	// mutation writes the member, and a follow-up equal pull is then silent.
	const FGuid NetID2 = FGuid::NewGuid();
	UCrowdyGameModelTestTarget* Target2 = MakeTarget();
	TArray<FCrowdyMutationApplied> Muts;
	FCrowdyMutationApplied M;
	M.Key = TEXT("speed");
	M.OldValueJson = TEXT("1.0");
	M.NewValueJson = TEXT("3.5");
	Muts.Add(M);
	Model->ApplyMutationsToContainer(NetID2, Target2, Muts);
	TestEqual(TEXT("mutation wrote float member"), Target2->Speed, 3.5f);
	TestEqual(TEXT("mutation fired float OnRep"), Target2->SpeedOnRepCount, 1);

	const TSharedPtr<FJsonObject> State2 = MakeShared<FJsonObject>();
	State2->SetNumberField(TEXT("speed"), 3.5);
	Model->ApplyStateToContainer(NetID2, Target2, State2);
	TestEqual(TEXT("cross-path float canonicalization: equal pull fires no OnRep"), Target2->SpeedOnRepCount, 1);

	return true;
}

// A uint8/enum-backed attribute's live member is actually written (a naive int32-only write branch silently
// no-ops a byte member while its OnRep still fires, reading stale), and a string containing a double-quote
// round-trips through the CANONICAL CACHE (the cache must be valid escaped JSON so the BP getter reads it).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelApplyByteAndEscapedStringTest,
	"CrowdySDK.GameModel.ApplyByteAndEscapedString", CrowdyGameModelApplyTestFlags)
bool FCrowdyGameModelApplyByteAndEscapedStringTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeSubsystem();
	UCrowdyGameModelTestTarget* Target = MakeTarget();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();

	// Byte member written (the generic FNumericProperty write branch), OnRep fires.
	const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetNumberField(TEXT("level"), 7);
	Model->ApplyStateToContainer(NetID, Target, State);
	TestEqual(TEXT("uint8 level member written"), static_cast<int32>(Target->Level), 7);
	TestEqual(TEXT("level OnRep fired"), Target->LevelOnRepCount, 1);

	// A string with an embedded double-quote: the member is written verbatim, and the canonical cache is valid
	// (escaped) JSON that parses back to the original a hand-wrap without escaping would be malformed JSON.
	const FString Tricky = TEXT("Sir \"Ironhand\" \\ the Bold");
	const TSharedPtr<FJsonObject> State2 = MakeShared<FJsonObject>();
	State2->SetStringField(TEXT("title"), Tricky);
	Model->ApplyStateToContainer(NetID, Target, State2);
	TestEqual(TEXT("string member written verbatim"), Target->Title, Tricky);

	FString CachedJson;
	if (TestTrue(TEXT("title cached"), Model->TryGetCachedValueJson(NetID, FName(TEXT("title")), CachedJson)))
	{
		// Parse the cached canonical back (wrap it, since a bare top-level scalar is rejected): it must be valid
		// JSON and equal the original, proving the escape round-trip the BP getter depends on.
		const FString Wrapped = FString::Printf(TEXT("{\"v\":%s}"), *CachedJson);
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Wrapped);
		TSharedPtr<FJsonObject> Obj;
		const bool bParsed = FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid();
		if (TestTrue(TEXT("cached string is valid escaped JSON"), bParsed))
		{
			FString RoundTripped;
			TestTrue(TEXT("cached value present"), Obj->TryGetStringField(TEXT("v"), RoundTripped));
			TestEqual(TEXT("string round-trips through the cache"), RoundTripped, Tricky);
		}
	}

	return true;
}

// A confirmed invoke's mutations go to the container each one NAMES, not to the invoke's own container. This is
// the damage-credit shape: an effect whose self is the victim writes source.damagedealt on the ATTACKER, so one
// result carries writes to two containers. Applying the whole set to the invoke target looked up "speed" on a
// class that does not declare it, dropped it silently, and left the attacker's member at its default forever.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelRoutesMutationsByContainerTest,
	"CrowdySDK.GameModel.InvokeMutationsRouteByContainer", CrowdyGameModelApplyTestFlags)
bool FCrowdyGameModelRoutesMutationsByContainerTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeSubsystem();
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	// Two DIFFERENT classes on purpose: RegisterParticipant seeds a Host NetID from the class path alone, so two
	// instances of one class collapse onto a single entity and the routing under test would have nothing to route
	// between. The derived fixture declares no attributes of its own, so it carries the same hp/speed as the base
	// and a misroute is still detectable as a write landing on the wrong object.
	UCrowdyGameModelTestTargetDerived* Victim = NewObject<UCrowdyGameModelTestTargetDerived>(GetTransientPackage());
	UCrowdyGameModelTestTarget* Attacker = MakeTarget();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("entities created"), Entities)
		|| !TestNotNull(TEXT("victim created"), Victim) || !TestNotNull(TEXT("attacker created"), Attacker))
	{
		return false;
	}

	Entities->SetLocalPlayerID(FGuid::NewGuid());
	Model->SetEntitySubsystemForTest(Entities);

	const FString VictimContainer = TEXT("c-victim");
	const FString AttackerContainer = TEXT("c-attacker");
	const FGuid VictimNetID = Entities->RegisterParticipant(Victim, ECrowdyOwnership::Host);
	const FGuid AttackerNetID = Entities->RegisterParticipant(Attacker, ECrowdyOwnership::Host);
	// Asserted, never assumed: if these two ever collapsed onto one NetID the rest of this test would pass for
	// the wrong reason, since every routed write would land on the single surviving participant.
	if (!TestTrue(TEXT("victim registered"), VictimNetID.IsValid())
		|| !TestTrue(TEXT("attacker registered"), AttackerNetID.IsValid())
		|| !TestTrue(TEXT("the two participants are distinct entities"), VictimNetID != AttackerNetID))
	{
		return false;
	}
	Model->BindEntityContainer(VictimNetID, VictimContainer);
	Model->BindEntityContainer(AttackerNetID, AttackerContainer);

	// The invoke ran against the victim; the result credits the attacker in the same transaction.
	TArray<FCrowdyMutationApplied> Mutations;
	FCrowdyMutationApplied& SelfWrite = Mutations.AddDefaulted_GetRef();
	SelfWrite.ContainerId = VictimContainer;
	SelfWrite.Key = TEXT("hp");
	SelfWrite.NewValueJson = TEXT("87");
	FCrowdyMutationApplied& SourceWrite = Mutations.AddDefaulted_GetRef();
	SourceWrite.ContainerId = AttackerContainer;
	SourceWrite.Key = TEXT("speed");
	SourceWrite.NewValueJson = TEXT("12.5");

	Model->ApplyInvokeMutations(VictimNetID, VictimContainer, Mutations);

	TestEqual(TEXT("self write landed on the invoke target"), Victim->Hp, 87);
	TestEqual(TEXT("self write fired the target's OnRep"), Victim->HpOnRepCount, 1);
	// The whole point: the source's write reaches the SOURCE's live member and fires its notify there.
	TestEqual(TEXT("source write landed on the source participant"), Attacker->Speed, 12.5f);
	TestEqual(TEXT("source write fired the source's OnRep"), Attacker->SpeedOnRepCount, 1);
	// Each container keeps its own cache, so the routed key never pollutes the invoke target's.
	TestEqual(TEXT("source write did not touch the target's member"), Victim->Speed, 1.0f);
	TestEqual(TEXT("target fired no OnRep for the routed key"), Victim->SpeedOnRepCount, 0);
	FString Cached;
	TestFalse(TEXT("routed key is absent from the target's cache"),
		Model->TryGetCachedValueJson(VictimNetID, FName(TEXT("speed")), Cached));
	TestTrue(TEXT("routed key is cached against the source"),
		Model->TryGetCachedValueJson(AttackerNetID, FName(TEXT("speed")), Cached));

	// A mutation carrying no container id is what a server predating the field sends: it must still land on the
	// invoke's own container, so an older server keeps working exactly as before routing existed.
	TArray<FCrowdyMutationApplied> Legacy;
	FCrowdyMutationApplied& NoId = Legacy.AddDefaulted_GetRef();
	NoId.Key = TEXT("hp");
	NoId.NewValueJson = TEXT("42");
	Model->ApplyInvokeMutations(VictimNetID, VictimContainer, Legacy);
	TestEqual(TEXT("an id-less mutation falls back to the invoke's own container"), Victim->Hp, 42);

	// A write naming a container this client neither binds nor watches is skipped, not misapplied onto self.
	TArray<FCrowdyMutationApplied> Foreign;
	FCrowdyMutationApplied& Unknown = Foreign.AddDefaulted_GetRef();
	Unknown.ContainerId = TEXT("c-someone-else");
	Unknown.Key = TEXT("hp");
	Unknown.NewValueJson = TEXT("1");
	Model->ApplyInvokeMutations(VictimNetID, VictimContainer, Foreign);
	TestEqual(TEXT("an unbound container's write is skipped, not applied to self"), Victim->Hp, 42);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
