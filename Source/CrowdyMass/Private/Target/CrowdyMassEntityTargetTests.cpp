#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Events/CrowdyMassEventLibrary.h"
#include "Fragments/CrowdyMassFragments.h"
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntityTypes.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/RPC/FCrowdyRpcTarget.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "Target/CrowdyMassEntityTargetResolver.h"
#include "Target/CrowdyMassEntityTargetTestTypes.h"
#include "UObject/UnrealType.h"
#include "Utils/UCrowdyClassRegistry.h"

// Covers the instance-less send surface (CrowdyMassTarget::ResolveRpcTarget / CheckEventClass and
// UCrowdyMassEventLibrary::SendEventToMassTarget) at every layer this codebase's headless harness can
// safely reach.
//
// Every resolve outcome is reached through ResolveRpcTargetIn, which takes the entity manager as a
// parameter. Going through Target's world instead would put all of them out of reach: that path reads the
// manager off UCrowdyMassEntitySubsystem, whose ShouldCreateSubsystem restricts creation to PIE and Game
// worlds whose map profile draws crowd rows, and standing one of those up here pulls in UMassEntitySubsystem's
// real initialization, which this codebase already documents as leaving Deinitialize asserting.
//
// ONE layer is still not reachable, and the tests below stop short of it rather than pretending otherwise:
// the wire dispatch itself (SingleActorToOwner/Host/Channel actually putting bytes on a transport). That
// needs a live UCrowdyEntitySubsystem, and no test in this codebase exercises it for the ACTOR path either,
// so it is a standing gap in the harness rather than one this surface introduced. What stands in for it
// here is the routing DECISION for each recipient, asserted against FCrowdyRPC::DecideRoute.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassEntityTargetTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Builds a parameter frame for Fn the same way UCrowdyMassEventLibrary::execCrowdyMassCallEventOnEntity
	// builds its own: one non-output CPF_Parm property at a time, in declaration order, each
	// InitializeValue'd before it is written. A real Blueprint call deposits each value by stepping the VM
	// frame (Stack.StepCompiledIn); a headless test has no VM frame to step, so this deposits the same
	// values by property name instead, which is the one substitution a test can make without reimplementing
	// the thunk's own bytecode-reading loop.
	void* BuildMassStyleFrame(UFunction* Fn, TArray<uint8>& FrameStorage,
		int32 Ammo, float Speed, const FString& Label, const FVector& Dir)
	{
		const int32 FrameSize = FMath::Max<int32>(Fn->ParmsSize, 1);
		FrameStorage.SetNumZeroed(FrameSize);
		void* Frame = FrameStorage.GetData();

		for (TFieldIterator<FProperty> It(Fn); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_Parm) || FCrowdyRPC::IsTrueOutputParam(Prop))
			{
				continue;
			}

			void* Dest = Prop->ContainerPtrToValuePtr<void>(Frame);
			Prop->InitializeValue(Dest);

			const FName PropName = Prop->GetFName();
			if (PropName == TEXT("InAmmo"))
			{
				*static_cast<int32*>(Dest) = Ammo;
			}
			else if (PropName == TEXT("InSpeed"))
			{
				*static_cast<float*>(Dest) = Speed;
			}
			else if (PropName == TEXT("InLabel"))
			{
				*static_cast<FString*>(Dest) = Label;
			}
			else if (PropName == TEXT("InDir"))
			{
				*static_cast<FVector*>(Dest) = Dir;
			}
		}

		return Frame;
	}

	// Mirrors the thunk's own FScopedEventParamFrame: destroys every non-output CPF_Parm property this
	// test constructed into Frame. Frame's storage itself is an owned TArray, so only the properties in it
	// need explicit cleanup.
	void DestroyMassStyleFrame(UFunction* Fn, void* Frame)
	{
		for (TFieldIterator<FProperty> It(Fn); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(CPF_Parm) && !FCrowdyRPC::IsTrueOutputParam(Prop))
			{
				Prop->DestroyValue_InContainer(Frame);
			}
		}
	}

	void AppendLittleEndian(TArray<uint8>& Out, const void* Value, int32 Size)
	{
		const uint8* Bytes = static_cast<const uint8*>(Value);
		for (int32 ByteIndex = 0; ByteIndex < Size; ++ByteIndex)
		{
			Out.Add(Bytes[ByteIndex]);
		}
	}
}

// The load-bearing case: a call built through the instance-less Mass surface must equal, field for field,
// a call built through the ordinary actor/instance path for the SAME function and the SAME argument
// values, across four different parameter kinds so a single-int comparison cannot pass by accident.
//
// Both paths call FCrowdyRPC::BuildCall, so this cannot catch a defect BuildCall/SerializeParams carries
// into both sides equally; the independent literal check below covers the fixed-width primitive prefix for
// exactly that reason. What this test is actually a gate on is FRAME CONSTRUCTION: does depositing the
// same argument values through a generic per-property reflection walk (what the Mass CustomThunk does)
// produce the identical bytes that depositing them through MarshalCall's typed PlaceAll (what a live
// actor's own CROWDY_EVENT thunk does) produces. Mutating either path's declaration-order iteration, or
// dropping/reordering a parameter in one but not the other, turns this red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEventParityMatchesActorInstancePathTest,
	"CrowdySDK.Mass.EventParityMatchesActorInstancePath", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassEventParityMatchesActorInstancePathTest::RunTest(const FString& Parameters)
{
	UFunction* Fn = ACrowdyMassEventParityTestTarget::StaticClass()->FindFunctionByName(TEXT("ParityEvent_Implementation"));
	if (!TestNotNull(TEXT("ParityEvent resolved"), Fn))
	{
		return false;
	}

	const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);

	const int32 Ammo = 7;
	const float Speed = 12.5f;
	const FString Label = TEXT("crowdy-mass-parity-label");
	const FVector Dir(1.0, -2.5, 3.25);

	// Side A: the actor/instance path. MarshalCall is exactly what a live actor's own CROWDY_EVENT thunk
	// calls (SendChecked -> MarshalCall -> BuildCall) before routing; it needs no spawned instance because
	// this signature carries no object-reference parameter for MarshalCall to resolve against one.
	const FCrowdyRpcCall ActorPathCall = FCrowdyRPC::MarshalCall(Fn, Info,
		&ACrowdyMassEventParityTestTarget::ParityEvent_Implementation, Ammo, Speed, Label, Dir);

	// Side B: the instance-less Mass path. SendEventToMassTarget and FCrowdyRPC::SendToTarget both call
	// BuildCall directly over an already-built frame; this builds that frame the way the CustomThunk builds
	// its own and calls the identical BuildCall entry point they call.
	TArray<uint8> FrameStorage;
	void* MassFrame = BuildMassStyleFrame(Fn, FrameStorage, Ammo, Speed, Label, Dir);
	const FCrowdyRpcCall MassPathCall = FCrowdyRPC::BuildCall(Fn, Info, MassFrame);
	DestroyMassStyleFrame(Fn, MassFrame);

	const int64 DeclaringClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(Fn->GetOwnerClass()));
	TestEqual(TEXT("actor-path ClassID is the declaring class"), ActorPathCall.ClassID, DeclaringClassID);
	TestEqual(TEXT("mass-path ClassID is the declaring class"), MassPathCall.ClassID, DeclaringClassID);
	TestEqual(TEXT("both paths agree on FunctionID"), ActorPathCall.FunctionID, MassPathCall.FunctionID);
	TestEqual(TEXT("both paths produce the same blob length"), ActorPathCall.ParamBlob.Num(), MassPathCall.ParamBlob.Num());
	TestTrue(TEXT("both paths produce byte-identical parameter blobs, field for field"),
		ActorPathCall.ParamBlob == MassPathCall.ParamBlob);

	// Independent control over the fixed-width primitive prefix (version byte + int32 + float), authored
	// by hand rather than derived from a second call to the same serializer: this is the half of the claim
	// the A/B comparison above cannot cover, because a defect shared by both paths would pass it exactly as
	// invisibly as it deceives a round-trip. It stops at the primitive prefix and does not hand-author the
	// FString/FVector tail: that wire format is already independently covered by CrowdySDK's own RPC
	// serializer round-trip tests, and getting its exact byte layout right by hand here would risk
	// asserting a wrong expectation rather than testing one.
	TArray<uint8> ExpectedPrefix;
	ExpectedPrefix.Add(CrowdyRpcParamBlobVersion);
	AppendLittleEndian(ExpectedPrefix, &Ammo, sizeof(Ammo));
	AppendLittleEndian(ExpectedPrefix, &Speed, sizeof(Speed));

	if (TestTrue(TEXT("the blob is at least as long as the primitive prefix"),
		ActorPathCall.ParamBlob.Num() >= ExpectedPrefix.Num()))
	{
		const TArray<uint8> ActorPrefix(ActorPathCall.ParamBlob.GetData(), ExpectedPrefix.Num());
		TestTrue(TEXT("the primitive prefix matches the independently authored bytes"),
			ActorPrefix == ExpectedPrefix);
	}

	return true;
}

// Wrong-class refusal, as the three-way distinction CheckEventClass exists to make rather than as a bare
// pass/fail: a class not yet recorded (null) and a class that is recorded but wrong must not collapse into
// one refusal, and a class that DERIVES the declaring class (the Blueprint-derived case) must pass exactly
// like an exact match.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassCheckEventClassThreeWayTest,
	"CrowdySDK.Mass.CheckEventClassThreeWay", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassCheckEventClassThreeWayTest::RunTest(const FString& Parameters)
{
	UFunction* Fn = UCrowdyMassEventClassCheckBaseTestTarget::StaticClass()->FindFunctionByName(TEXT("CheckEvent_Implementation"));
	if (!TestNotNull(TEXT("CheckEvent resolved"), Fn))
	{
		return false;
	}

	using namespace CrowdyMassTarget;

	TestTrue(TEXT("a class not yet recorded (null) reads as ClassUnknown, not a mismatch"),
		CheckEventClass(Fn, nullptr) == EClassCheckResult::ClassUnknown);

	TestTrue(TEXT("the exact declaring class passes"),
		CheckEventClass(Fn, UCrowdyMassEventClassCheckBaseTestTarget::StaticClass()) == EClassCheckResult::Ok);

	TestTrue(TEXT("a class that DERIVES the declaring class also passes (the Blueprint-derived case)"),
		CheckEventClass(Fn, UCrowdyMassEventClassCheckDerivedTestTarget::StaticClass()) == EClassCheckResult::Ok);

	TestTrue(TEXT("a class unrelated to the declaring class is a genuine mismatch, not folded into ClassUnknown"),
		CheckEventClass(Fn, UCrowdyMassEventClassCheckUnrelatedTestTarget::StaticClass()) == EClassCheckResult::ClassMismatch);

	TestNotEqual(TEXT("ClassUnknown and ClassMismatch are reported as different results, not the same refusal reached two ways"),
		static_cast<uint8>(CheckEventClass(Fn, nullptr)),
		static_cast<uint8>(CheckEventClass(Fn, UCrowdyMassEventClassCheckUnrelatedTestTarget::StaticClass())));

	// An event declared on a component: the entity class never mentions it, and the receiving side would
	// still resolve the body onto the component.
	UFunction* ComponentFn = UCrowdyMassEventClassCheckTestComponent::StaticClass()
		->FindFunctionByName(TEXT("ComponentEvent_Implementation"));
	if (!TestNotNull(TEXT("ComponentEvent resolved"), ComponentFn))
	{
		return false;
	}

	TestTrue(TEXT("a component-declared event passes against an entity class that does not mention it"),
		CheckEventClass(ComponentFn, ACrowdyMassEventParityTestTarget::StaticClass()) == EClassCheckResult::Ok);

	TestTrue(TEXT("a component-declared event still needs a recorded class"),
		CheckEventClass(ComponentFn, nullptr) == EClassCheckResult::ClassUnknown);

	return true;
}

// FCrowdyMassEntityTarget::IsSet() is a structural check only (entity handle bits set AND the weak world
// still resolving), not a live-entity check; this pins both halves independently and confirms the world
// going away flips it even though the entity handle field is untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityTargetIsSetStructuralCheckTest,
	"CrowdySDK.Mass.EntityTargetIsSetStructuralCheck", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassEntityTargetIsSetStructuralCheckTest::RunTest(const FString& Parameters)
{
	FCrowdyMassEntityTarget EmptyTarget;
	TestFalse(TEXT("a default-constructed target is not set"), EmptyTarget.IsSet());

	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
	Context.SetCurrentWorld(World);

	FCrowdyMassEntityTarget WorldOnlyTarget;
	WorldOnlyTarget.World = World;
	TestFalse(TEXT("a world with no entity handle is not set"), WorldOnlyTarget.IsSet());

	const FMassEntityHandle Handle(1, 1);
	FCrowdyMassEntityTarget HandleOnlyTarget;
	HandleOnlyTarget.Entity = Handle;
	TestFalse(TEXT("an entity handle with no world is not set"), HandleOnlyTarget.IsSet());

	FCrowdyMassEntityTarget FullTarget;
	FullTarget.Entity = Handle;
	FullTarget.World = World;
	TestTrue(TEXT("both halves present reads as set"), FullTarget.IsSet());

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld=*/false);

	// DestroyWorld tears the world down and takes it out of the root set, but it does not mark the UWorld
	// itself as garbage, so a weak pointer to it stays valid until a collection actually runs. Without
	// this the assertion below would be testing that a live pointer is live.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge=*/true);

	TestFalse(TEXT("a collected world reads as not set even though the entity handle field is untouched"),
		FullTarget.IsSet());

	return true;
}

// A target whose weak world has gone: ResolveRpcTarget must answer NoWorld (not silently address the
// origin, and not crash reading through a stale weak pointer), and SendEventToMassTarget must return false
// for exactly this reason - the one case its contract carves out from "every refusal returns true".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTargetWhoseWorldHasGoneRefusesTest,
	"CrowdySDK.Mass.TargetWhoseWorldHasGoneRefuses", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassTargetWhoseWorldHasGoneRefusesTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
	Context.SetCurrentWorld(World);

	FCrowdyMassEntityTarget Target;
	Target.Entity = FMassEntityHandle(1, 1);
	Target.World = World;

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld=*/false);

	// Collect, then assert the pointer really did go stale before resolving through it. NoWorld has two
	// causes (the world pointer is gone, or the world holds no Mass entity manager) and a torn-down world
	// satisfies the second one whether or not it satisfies the first, so without this the test would pass
	// while never once exercising the gone-world path it is named for.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge=*/true);
	if (!TestFalse(TEXT("the weak world is stale before resolving, so this exercises the gone-world path"),
		Target.World.IsValid()))
	{
		return false;
	}

	FCrowdyRpcTarget Resolved;
	const CrowdyMassTarget::EResolveResult ResolveResult = CrowdyMassTarget::ResolveRpcTarget(Target, Resolved);
	TestTrue(TEXT("a target whose weak world has gone resolves as NoWorld"),
		ResolveResult == CrowdyMassTarget::EResolveResult::NoWorld);

	UFunction* Fn = ACrowdyMassEventParityTestTarget::StaticClass()->FindFunctionByName(TEXT("ParityEvent_Implementation"));
	if (!TestNotNull(TEXT("ParityEvent resolved"), Fn))
	{
		return false;
	}

	TArray<uint8> FrameStorage;
	void* Frame = BuildMassStyleFrame(Fn, FrameStorage, 1, 1.0f, TEXT("x"), FVector::ZeroVector);
	const bool bSendResult = UCrowdyMassEventLibrary::SendEventToMassTarget(Target, Fn, Frame);
	DestroyMassStyleFrame(Fn, Frame);

	TestFalse(TEXT("SendEventToMassTarget returns false only for a gone world, matching SendToTarget's own contract"),
		bSendResult);

	return true;
}

// A null function has nothing to resolve a class or a receiver against, so this refuses before target
// resolution is even attempted, and (like every other refusal SendEventToMassTarget makes on its own
// account) returns true rather than false: there IS a target's world to route through in principle, there
// is simply nothing to send.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSendRefusesNullFunctionTest,
	"CrowdySDK.Mass.SendRefusesNullFunction", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassSendRefusesNullFunctionTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("no function to call."), EAutomationExpectedErrorFlags::Contains, 1);

	FCrowdyMassEntityTarget Target;
	const bool bResult = UCrowdyMassEventLibrary::SendEventToMassTarget(Target, nullptr, nullptr);

	TestTrue(TEXT("a null function refuses (true); it is not treated as a missing transport"), bResult);

	return true;
}

// A function with no CrowdyEvent metadata refuses before target resolution ever runs, so an entirely
// empty/unset target proves this is genuinely the "not a CrowdyEvent" branch rather than a resolve failure
// wearing the same return value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSendRefusesNonCrowdyEventFunctionTest,
	"CrowdySDK.Mass.SendRefusesNonCrowdyEventFunction", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassSendRefusesNonCrowdyEventFunctionTest::RunTest(const FString& Parameters)
{
	UFunction* Fn = ACrowdyMassEventParityTestTarget::StaticClass()->FindFunctionByName(TEXT("NotAnEvent_Implementation"));
	if (!TestNotNull(TEXT("NotAnEvent resolved"), Fn))
	{
		return false;
	}

	// The refusal log is throttled by a process-wide, time-based gate, so whether this warning appears at
	// all depends on what ran before this test and how recently. Clearing it first is what makes the exact
	// count below mean something.
	UCrowdyMassEventLibrary::ResetRefusalLogThrottleForTest();

	AddExpectedMessagePlain(TEXT("it is not a CrowdyEvent"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	FCrowdyMassEntityTarget EmptyTarget;
	const bool bResult = UCrowdyMassEventLibrary::SendEventToMassTarget(EmptyTarget, Fn, nullptr);

	TestTrue(TEXT("refused (true): there is a function, just no receiver willing to run it"), bResult);

	return true;
}

// Ties this module's own CrowdyEvent declarations to the real ownership-model routing table: each
// recipient this file declares resolves (via BuildFnInfo, off the UFUNCTION's own metadata) to the
// ECrowdyEventRecipient its meta names, and DecideRoute maps that recipient to the transport a Mass
// entity (spatial, so bNonSpatial=false) actually uses. OwningClient's row is what carries opcode 142
// (ECrowdyMessageType::SINGLE_ACTOR_MESSAGE) to the entity's owner; Host's carries the same opcode to the
// host. This covers the routing DECISION the send path makes; it does not exercise the wire dispatch
// itself, which needs a live World-resolved entity subsystem this harness cannot supply (see the file
// header).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEventRecipientsRouteAsDeclaredTest,
	"CrowdySDK.Mass.EventRecipientsRouteAsDeclared", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassEventRecipientsRouteAsDeclaredTest::RunTest(const FString& Parameters)
{
	auto CheckRoute = [this](const TCHAR* FuncName, ECrowdyEventRecipient ExpectedRecipient,
		ECrowdyRpcRoute ExpectedRoute, bool bExpectedRunLocally)
	{
		UFunction* Fn = ACrowdyMassEventParityTestTarget::StaticClass()->FindFunctionByName(FuncName);
		if (!TestNotNull(FString::Printf(TEXT("%s resolved"), FuncName), Fn))
		{
			return;
		}

		const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Fn);
#if WITH_METADATA
		TestTrue(FString::Printf(TEXT("%s declares the expected recipient"), FuncName),
			Info.Recipient == ExpectedRecipient);
#endif

		// A target this client does not own and is not the host for: the case every one of these events
		// hits when it addresses a Mass-rendered remote player belonging to somebody else.
		const FCrowdyRpcRouteDecision Decision = FCrowdyRPC::DecideRoute(Info.Recipient, /*bNonSpatial=*/false,
			/*bEntityValid=*/true, /*bWeOwnEntity=*/false, /*bWeAreHost=*/false);

		TestTrue(FString::Printf(TEXT("%s routes as declared"), FuncName), Decision.Route == ExpectedRoute);
		TestEqual(FString::Printf(TEXT("%s local-run flag matches"), FuncName),
			Decision.bRunLocally, bExpectedRunLocally);
	};

	CheckRoute(TEXT("ParityEvent_Implementation"), ECrowdyEventRecipient::OwningClient,
		ECrowdyRpcRoute::SingleActorToOwner, /*bExpectedRunLocally=*/false);
	CheckRoute(TEXT("HostRecipientEvent_Implementation"), ECrowdyEventRecipient::Host,
		ECrowdyRpcRoute::SingleActorToHost, /*bExpectedRunLocally=*/false);
	CheckRoute(TEXT("MulticastRecipientEvent_Implementation"), ECrowdyEventRecipient::Multicast,
		ECrowdyRpcRoute::Channel, /*bExpectedRunLocally=*/true);
	CheckRoute(TEXT("SpatialRecipientEvent_Implementation"), ECrowdyEventRecipient::SpatialMulticast,
		ECrowdyRpcRoute::SpatialBroadcast, /*bExpectedRunLocally=*/true);

	return true;
}

namespace
{
	// A standalone Mass entity manager with no World, the same fixture shape
	// Subsystems/CrowdyMassStateFragmentProviderTests.cpp uses. The manager needs a real owner: its
	// PostInitialize constructs relation observers outered to whatever owner it was given, and an
	// owner-less manager takes the process down.
	struct FCrowdyMassTargetResolveFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;

		FCrowdyMassTargetResolveFixture()
		{
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();
		}

		~FCrowdyMassTargetResolveFixture()
		{
			EntityManager->Deinitialize();
		}

		FMassEntityHandle MakeEntity(bool bWithIdentity, bool bWithTransform)
		{
			TArray<const UScriptStruct*> Fragments;
			if (bWithIdentity)
			{
				Fragments.Add(FCrowdyRemoteIdentityFragment::StaticStruct());
			}
			if (bWithTransform)
			{
				Fragments.Add(FTransformFragment::StaticStruct());
			}

			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(Fragments);
			return EntityManager->CreateEntity(Archetype);
		}

		// A target naming Entity. World is deliberately left unset: ResolveRpcTargetIn never reads it,
		// which is the whole reason these outcomes are reachable at all.
		FCrowdyMassEntityTarget MakeTarget(FMassEntityHandle Entity) const
		{
			FCrowdyMassEntityTarget Target;
			Target.Entity = Entity;
			return Target;
		}
	};
}

// The resolve every send depends on, exercised against an injected entity manager: the identity fragment
// supplies the id, the owner and the role together, and the location comes off the entity's own transform
// fragment rather than from anything a caller could hand in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTargetResolvesFromFragmentsTest,
	"CrowdySDK.Mass.EntityTargetResolvesFromFragments", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassTargetResolvesFromFragmentsTest::RunTest(const FString& Parameters)
{
	FCrowdyMassTargetResolveFixture Fixture;

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithIdentity=*/true, /*bWithTransform=*/true);

	const FGuid ExpectedUUID = FGuid::NewGuid();
	const FGuid ExpectedOwner = FGuid::NewGuid();
	const FVector ExpectedLocation(1200.0, -340.0, 88.0);

	FCrowdyRemoteIdentityFragment* Identity =
		Fixture.EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Entity);
	if (!TestNotNull(TEXT("the fixture entity carries an identity fragment"), Identity))
	{
		return false;
	}
	Identity->UUID = ExpectedUUID;
	Identity->OwnerID = ExpectedOwner;
	Identity->Role = ECrowdyRole::RemoteProxy;

	FTransformFragment* Transform = Fixture.EntityManager->GetFragmentDataPtr<FTransformFragment>(Entity);
	if (!TestNotNull(TEXT("the fixture entity carries a transform fragment"), Transform))
	{
		return false;
	}
	Transform->GetMutableTransform().SetLocation(ExpectedLocation);

	FCrowdyRpcTarget Resolved;
	const CrowdyMassTarget::EResolveResult Result =
		CrowdyMassTarget::ResolveRpcTargetIn(*Fixture.EntityManager, Fixture.MakeTarget(Entity), Resolved);

	TestTrue(TEXT("an entity with an identity and a valid id resolves"),
		Result == CrowdyMassTarget::EResolveResult::Resolved);
	TestEqual(TEXT("NetID comes from the identity fragment"), Resolved.NetID, ExpectedUUID);
	TestEqual(TEXT("OwnerID comes from the identity fragment"), Resolved.OwnerID, ExpectedOwner);
	TestTrue(TEXT("Role comes from the identity fragment"), Resolved.Role == ECrowdyRole::RemoteProxy);
	TestTrue(TEXT("a routable target reports itself routable"), Resolved.IsRoutable());

	if (TestTrue(TEXT("the location is set from the transform fragment"), Resolved.HasLocation()))
	{
		TestEqual(TEXT("the location is the entity's own, not a defaulted one"),
			Resolved.Location.GetValue(), ExpectedLocation);
	}

	return true;
}

// An entity with no transform fragment leaves the location UNSET rather than defaulting it. The origin is
// a real, addressable place, so "where this entity is" and "nobody knows where this entity is" must not
// arrive at the send path as the same value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTargetWithoutTransformLeavesLocationUnsetTest,
	"CrowdySDK.Mass.EntityTargetWithoutTransformLeavesLocationUnset", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassTargetWithoutTransformLeavesLocationUnsetTest::RunTest(const FString& Parameters)
{
	FCrowdyMassTargetResolveFixture Fixture;

	const FMassEntityHandle Entity = Fixture.MakeEntity(/*bWithIdentity=*/true, /*bWithTransform=*/false);
	Fixture.EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Entity)->UUID = FGuid::NewGuid();

	FCrowdyRpcTarget Resolved;
	const CrowdyMassTarget::EResolveResult Result =
		CrowdyMassTarget::ResolveRpcTargetIn(*Fixture.EntityManager, Fixture.MakeTarget(Entity), Resolved);

	TestTrue(TEXT("an entity with no transform fragment still resolves"),
		Result == CrowdyMassTarget::EResolveResult::Resolved);
	TestFalse(TEXT("its location is unset, not the origin"), Resolved.HasLocation());

	return true;
}

// The two ways a live entity can still fail to produce a routable target, kept apart from each other and
// from InvalidEntity: it carries no identity fragment at all, or it carries one that names nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTargetMissingIdentityAndUnroutableTest,
	"CrowdySDK.Mass.EntityTargetMissingIdentityAndUnroutable", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassTargetMissingIdentityAndUnroutableTest::RunTest(const FString& Parameters)
{
	FCrowdyMassTargetResolveFixture Fixture;

	const FMassEntityHandle NoIdentity = Fixture.MakeEntity(/*bWithIdentity=*/false, /*bWithTransform=*/true);
	FCrowdyRpcTarget ResolvedNoIdentity;
	TestTrue(TEXT("a live entity carrying no identity fragment answers MissingIdentity"),
		CrowdyMassTarget::ResolveRpcTargetIn(*Fixture.EntityManager, Fixture.MakeTarget(NoIdentity),
			ResolvedNoIdentity) == CrowdyMassTarget::EResolveResult::MissingIdentity);

	// Default-constructed, so its UUID is the invalid guid: an entity whose identity has not been filled
	// in yet, which is a different answer from having no identity fragment at all.
	const FMassEntityHandle BlankIdentity = Fixture.MakeEntity(/*bWithIdentity=*/true, /*bWithTransform=*/true);
	FCrowdyRpcTarget ResolvedBlank;
	TestTrue(TEXT("an identity fragment carrying no valid id answers Unroutable"),
		CrowdyMassTarget::ResolveRpcTargetIn(*Fixture.EntityManager, Fixture.MakeTarget(BlankIdentity),
			ResolvedBlank) == CrowdyMassTarget::EResolveResult::Unroutable);

	return true;
}

// The property the target struct exists for: a handle held across a despawn does not follow the player
// onto a fresh entity respawned under the SAME UUID. The handle's serial is what makes that true, so this
// is the test that would fail if the target ever started carrying a UUID instead of a handle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTargetDoesNotFollowARespawnTest,
	"CrowdySDK.Mass.EntityTargetDoesNotFollowARespawn", CrowdyMassEntityTargetTestFlags)
bool FCrowdyMassTargetDoesNotFollowARespawnTest::RunTest(const FString& Parameters)
{
	FCrowdyMassTargetResolveFixture Fixture;

	const FGuid SharedUUID = FGuid::NewGuid();

	const FMassEntityHandle Original = Fixture.MakeEntity(/*bWithIdentity=*/true, /*bWithTransform=*/true);
	Fixture.EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Original)->UUID = SharedUUID;

	const FCrowdyMassEntityTarget HeldTarget = Fixture.MakeTarget(Original);

	FCrowdyRpcTarget BeforeDespawn;
	if (!TestTrue(TEXT("the held target resolves while its entity is alive"),
		CrowdyMassTarget::ResolveRpcTargetIn(*Fixture.EntityManager, HeldTarget, BeforeDespawn)
			== CrowdyMassTarget::EResolveResult::Resolved))
	{
		return false;
	}

	Fixture.EntityManager->DestroyEntity(Original);

	// The same UUID comes back on a brand new entity, which is exactly what a respawn looks like here.
	const FMassEntityHandle Respawned = Fixture.MakeEntity(/*bWithIdentity=*/true, /*bWithTransform=*/true);
	Fixture.EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Respawned)->UUID = SharedUUID;

	// Assert the reuse actually happened, because without it this test would be satisfied by the entity
	// merely having been destroyed, and the serial-number property the whole no-carried-id design rests on
	// would never be exercised while the test kept its name and stayed green.
	if (!TestEqual(TEXT("the respawn reused the despawned entity's slot"), Respawned.Index, Original.Index))
	{
		AddError(TEXT("this run never exercised slot reuse, so it cannot speak to the serial-number property"));
		return false;
	}
	TestNotEqual(TEXT("and the reused slot got a fresh serial"), Respawned.SerialNumber, Original.SerialNumber);

	FCrowdyRpcTarget AfterRespawn;
	TestTrue(TEXT("the target held across the despawn refuses instead of naming the new entity"),
		CrowdyMassTarget::ResolveRpcTargetIn(*Fixture.EntityManager, HeldTarget, AfterRespawn)
			== CrowdyMassTarget::EResolveResult::InvalidEntity);

	// The respawned entity is addressable in its own right, so the refusal above is about the stale handle
	// and not about the UUID having become unusable.
	FCrowdyRpcTarget ResolvedFresh;
	if (TestTrue(TEXT("a target naming the respawned entity resolves"),
		CrowdyMassTarget::ResolveRpcTargetIn(*Fixture.EntityManager, Fixture.MakeTarget(Respawned),
			ResolvedFresh) == CrowdyMassTarget::EResolveResult::Resolved))
	{
		TestEqual(TEXT("and resolves to the same UUID the despawned one carried"),
			ResolvedFresh.NetID, SharedUUID);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
