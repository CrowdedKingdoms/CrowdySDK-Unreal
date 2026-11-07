#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Components/SceneComponent.h"
#include "Data/CrowdyActorPoolPolicy.h"
#include "Data/CrowdyTransformRepPolicy.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "UObject/Package.h"

#include <limits>

namespace CrowdyProxyMotionTest
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	struct FWorld
	{
		UWorld* World = nullptr;

		FWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, false);
			GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
		}

		~FWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	};

	void Push(UCrowdyTransformRepPolicy* Policy, const FVector& Location, const int64 TimestampMs, const int32 SlotId, const float AimPitch = 0.f)
	{
		FCrowdyActorState State;
		State.Location = Location;
		State.AimPitch = AimPitch;
		Policy->ExtractFields(FInstancedStruct::Make(State), TimestampMs, SlotId);
	}
}

/**
 * A pooled character's movement component is stopped, so its animation blueprint reads whatever velocity and
 * acceleration the policy gives it. Drawn moving 500 cm/s, it must read 500 cm/s and accelerating that way; a
 * teleport and a released slot read still; a position no actor can stand at is refused.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProxyMotionVelocityTest,
	"CrowdySDK.ActorPool.ProxyCharacterReadsItsDrawnVelocity", CrowdyProxyMotionTest::Flags)
bool FCrowdyProxyMotionVelocityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyProxyMotionTest;

	FWorld Test;
	ACharacter* const Character = Test.World->SpawnActor<ACharacter>();
	if (!TestNotNull(TEXT("a character spawns in the test world"), Character)) return false;
	CrowdyActorPool::DisableProxyMovement(Character);
	UCharacterMovementComponent* const Movement = Character->GetCharacterMovement();

	UCrowdyTransformRepPolicy* const Policy = NewObject<UCrowdyTransformRepPolicy>(GetTransientPackage());
	Push(Policy, FVector(0.0, 0.0, 0.0), 1000, 0);
	Push(Policy, FVector(50.0, 0.0, 0.0), 1100, 0);

	Policy->ApplyToActor(Character, 0, 1020);
	TestTrue(TEXT("on the first drawn frame there is no previous one, so it stands still"),
		Movement->Velocity.IsNearlyZero());

	Policy->ApplyToActor(Character, 0, 1036);
	TestEqual(TEXT("drawn 8 cm further 16 ms later, it moves at 500 cm/s"), Movement->Velocity.X, 500.0, 0.5);
	TestEqual(TEXT("and GetVelocity reads the same, though the component is deactivated"),
		Character->GetVelocity().X, 500.0, 0.5);
	TestTrue(TEXT("and it accelerates the way it moves, which starts the walk"),
		Movement->GetCurrentAcceleration().Equals(FVector(1.0, 0.0, 0.0), 0.001));

	Policy->ApplyToActor(Character, 0, 1036);
	TestEqual(TEXT("drawn twice at one render time, it keeps its velocity"), Movement->Velocity.X, 500.0, 0.5);

	Policy->OnInstanceDeactivated(0);
	TestTrue(TEXT("a released slot leaves the pooled character still"), Movement->Velocity.IsNearlyZero());
	TestTrue(TEXT("and not accelerating"), Movement->GetCurrentAcceleration().IsNearlyZero());

	Push(Policy, FVector(0.0, 0.0, 0.0), 1000, 2);
	Push(Policy, FVector(50.0, 0.0, 0.0), 1100, 2);
	Push(Policy, FVector(100000.0, 0.0, 0.0), 1200, 2);
	Policy->ApplyToActor(Character, 2, 1020);
	Policy->ApplyToActor(Character, 2, 1036);
	TestEqual(TEXT("reused in another slot it moves again"), Movement->Velocity.X, 500.0, 0.5);
	Policy->ApplyToActor(Character, 2, 1150);
	TestTrue(TEXT("crossing half a kilometre in a frame is a teleport, so it stands still"), Movement->Velocity.IsNearlyZero());

	FCrowdyActorState NotFinite;
	NotFinite.Location = FVector(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0);
	TestFalse(TEXT("a peer's non-finite position is refused, so it never reaches the actor or its velocity"),
		Policy->ExtractFields(FInstancedStruct::Make(NotFinite), 1300, 4));
	FCrowdyActorState OutOfWorld;
	OutOfWorld.Location = FVector(0.0, 0.0, 1.0e13);
	TestFalse(TEXT("and so is one past the addressable world"), Policy->ExtractFields(FInstancedStruct::Make(OutOfWorld), 1300, 4));

	Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Movement->Velocity = FVector(120.0, 0.0, 0.0);
	Policy->ApplyToActor(Character, 2, 1160);
	Policy->OnInstanceDeactivated(2);
	TestEqual(TEXT("a character whose movement runs again is simulated here, so the policy never writes its velocity"),
		Movement->Velocity.X, 120.0, 0.01);

	AActor* const Prop = Test.World->SpawnActor<AActor>();
	USceneComponent* const Root = NewObject<USceneComponent>(Prop);
	Prop->SetRootComponent(Root);
	Root->RegisterComponent();
	Push(Policy, FVector(0.0, 0.0, 0.0), 1000, 1);
	Push(Policy, FVector(50.0, 0.0, 0.0), 1100, 1);
	Policy->ApplyToActor(Prop, 1, 1020);
	Policy->ApplyToActor(Prop, 1, 1036);
	TestEqual(TEXT("an actor with no character movement is still placed as before"), Prop->GetActorLocation().X, 18.0, 0.01);

	return true;
}

/**
 * Each guard on its own: no previous frame, a stall, a render time that went backwards, another actor arriving in the
 * slot, and the teleport speed for a character walking at the default 600 cm/s (3000 cm/s).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProxyMotionGuardsTest,
	"CrowdySDK.ActorPool.ProxyCharacterMotionGuards", CrowdyProxyMotionTest::Flags)
bool FCrowdyProxyMotionGuardsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyProxyMotionTest;

	FWorld Test;
	ACharacter* const Character = Test.World->SpawnActor<ACharacter>();
	ACharacter* const Other = Test.World->SpawnActor<ACharacter>();
	if (!TestNotNull(TEXT("two characters spawn in the test world"), Other)) return false;
	CrowdyActorPool::DisableProxyMovement(Character);
	CrowdyActorPool::DisableProxyMovement(Other);
	UCharacterMovementComponent* const Movement = Character->GetCharacterMovement();
	UCrowdyTransformRepPolicy* const Policy = NewObject<UCrowdyTransformRepPolicy>(GetTransientPackage());

	Push(Policy, FVector(0.0, 0.0, 0.0), 100, 10);
	Push(Policy, FVector(10.0, 0.0, 0.0), 200, 10);
	Policy->ApplyToActor(Character, 10, 150);
	TestTrue(TEXT("a first frame soon after the clock's start still has nothing to measure from"), Movement->Velocity.IsNearlyZero());

	Push(Policy, FVector(0.0, 0.0, 0.0), 1000, 11);
	Push(Policy, FVector(50.0, 0.0, 0.0), 1100, 11);
	Push(Policy, FVector(200.0, 0.0, 0.0), 1400, 11);
	Policy->ApplyToActor(Character, 11, 1020);
	Policy->ApplyToActor(Character, 11, 1036);
	Policy->ApplyToActor(Character, 11, 1300);
	TestTrue(TEXT("after a stall of more than a quarter second it stands still"), Movement->Velocity.IsNearlyZero());

	Push(Policy, FVector(0.0, 0.0, 0.0), 1000, 12);
	Push(Policy, FVector(50.0, 0.0, 0.0), 1100, 12);
	Policy->ApplyToActor(Character, 12, 1020);
	Policy->ApplyToActor(Character, 12, 1036);
	Policy->ApplyToActor(Character, 12, 1030);
	TestTrue(TEXT("a render time that went backwards starts over"), Movement->Velocity.IsNearlyZero());
	Policy->ApplyToActor(Character, 12, 1046);
	TestEqual(TEXT("and measures again from there"), Movement->Velocity.X, 500.0, 0.5);

	Policy->ApplyToActor(Other, 12, 1062);
	TestEqual(TEXT("another actor in the slot leaves the first one alone, which may be another slot's now"),
		Movement->Velocity.X, 500.0, 0.5);
	TestTrue(TEXT("and starts still itself"), Other->GetCharacterMovement()->Velocity.IsNearlyZero());

	Push(Policy, FVector(0.0, 0.0, 0.0), 1000, 13);
	Push(Policy, FVector(290.0, 0.0, 0.0), 1100, 13);
	Policy->ApplyToActor(Character, 13, 1020);
	Policy->ApplyToActor(Character, 13, 1036);
	TestEqual(TEXT("2900 cm/s is under the teleport speed"), Movement->Velocity.X, 2900.0, 1.0);

	Push(Policy, FVector(0.0, 0.0, 0.0), 1000, 14);
	Push(Policy, FVector(310.0, 0.0, 0.0), 1100, 14);
	Policy->ApplyToActor(Character, 14, 1020);
	Policy->ApplyToActor(Character, 14, 1036);
	TestTrue(TEXT("3100 cm/s is over it, so it reads as a teleport"), Movement->Velocity.IsNearlyZero());

	return true;
}

/**
 * A remote character has no controller, so an aim offset reads its pitch through GetBaseAimRotation, which takes
 * the remote view pitch only while the actor itself is level.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProxyAimPitchTest,
	"CrowdySDK.ActorPool.ProxyCharacterAimsWithRemoteViewPitch", CrowdyProxyMotionTest::Flags)
bool FCrowdyProxyAimPitchTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyProxyMotionTest;

	FWorld Test;
	ACharacter* const Character = Test.World->SpawnActor<ACharacter>();
	if (!TestNotNull(TEXT("a character spawns in the test world"), Character)) return false;
	TestNull(TEXT("it has no controller, like a pooled copy"), Character->GetController());

	Character->SetRemoteViewPitch(25.0);
	TestEqual(TEXT("a level character aims at its remote view pitch"), Character->GetBaseAimRotation().Pitch, 25.0, 0.01);

	Character->SetRemoteViewPitch(-40.0);
	TestEqual(TEXT("looking down reads back in 0 to 360, so an aim offset must normalize it"),
		Character->GetBaseAimRotation().Pitch, 320.0, 0.01);
	TestEqual(TEXT("and normalized it is the pitch that was set"),
		FRotator::NormalizeAxis(Character->GetBaseAimRotation().Pitch), -40.0, 0.01);

	Character->SetActorRotation(FRotator(10.0, 0.0, 0.0));
	TestEqual(TEXT("a tilted character aims along its own pitch instead"), Character->GetBaseAimRotation().Pitch, 10.0, 0.01);

	return true;
}

/** The policy gives a drawn pawn the sender's aim, blended between updates like its position. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProxyAimFollowsUpdatesTest,
	"CrowdySDK.ActorPool.ProxyAimFollowsTheSendersUpdates", CrowdyProxyMotionTest::Flags)
bool FCrowdyProxyAimFollowsUpdatesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyProxyMotionTest;

	FWorld Test;
	ACharacter* const Character = Test.World->SpawnActor<ACharacter>();
	if (!TestNotNull(TEXT("a character spawns in the test world"), Character)) return false;
	CrowdyActorPool::DisableProxyMovement(Character);

	UCrowdyTransformRepPolicy* const Policy = NewObject<UCrowdyTransformRepPolicy>(GetTransientPackage());
	Push(Policy, FVector::ZeroVector, 1000, 0, 10.f);
	Push(Policy, FVector::ZeroVector, 1100, 0, 30.f);

	Policy->ApplyToActor(Character, 0, 1050);
	TestEqual(TEXT("halfway between two updates it aims halfway"), Character->GetBaseAimRotation().Pitch, 20.0, 0.02);

	Policy->ApplyToActor(Character, 0, 1160);
	TestEqual(TEXT("past the last update it holds the aim rather than extrapolating it"), Character->GetBaseAimRotation().Pitch, 30.0, 0.02);

	Policy->OnInstanceDeactivated(0);
	Push(Policy, FVector::ZeroVector, 2000, 0, 5.f);
	Policy->ApplyToActor(Character, 0, 1500);
	TestEqual(TEXT("a reused slot aims from its own updates, not the last entity's"), Character->GetBaseAimRotation().Pitch, 5.0, 0.02);

	FCrowdyActorState NotFinite;
	NotFinite.AimPitch = std::numeric_limits<float>::quiet_NaN();
	TestTrue(TEXT("a peer's non-finite pitch does not cost it its position"), Policy->ExtractFields(FInstancedStruct::Make(NotFinite), 1200, 1));
	Policy->ApplyToActor(Character, 1, 1200);
	TestEqual(TEXT("and reads as level"), Character->GetBaseAimRotation().Pitch, 0.0, 0.02);

	AActor* const Prop = Test.World->SpawnActor<AActor>();
	USceneComponent* const Root = NewObject<USceneComponent>(Prop);
	Prop->SetRootComponent(Root);
	Root->RegisterComponent();
	Push(Policy, FVector(40.0, 0.0, 0.0), 1000, 2, 45.f);
	Policy->ApplyToActor(Prop, 2, 1000);
	TestEqual(TEXT("an actor that is not a pawn is placed all the same"), Prop->GetActorLocation().X, 40.0, 0.01);
	return true;
}

#endif
