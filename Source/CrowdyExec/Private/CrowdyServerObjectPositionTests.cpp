#include "CrowdyServerObjectPositionTestTypes.h"
#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SceneComponent.h"
#include "CrowdyExecCodec.h"
#include "CrowdyExecInternal.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectComponent.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectSubsystem.h"
#include "CrowdyServerObjectTestRig.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

struct FCrowdyServerObjectPositionTestAccess
{
	static void UseServerObjects(UCrowdyServerObjectComponent& Component, UCrowdyServerObjectSubsystem* Subsystem) { Component.ServerObjectsForTest = Subsystem; }
	static void Join(UCrowdyServerObjectComponent& Component) { Component.Join(); }
	static void Leave(UCrowdyServerObjectComponent& Component) { Component.Leave(); }
	static void Report(UCrowdyServerObjectComponent& Component, double Now)
	{
		Component.PositionTimeForTest = Now;
		Component.ReportPosition();
	}
	static void SetTime(UCrowdyServerObjectComponent& Component, double Now) { Component.PositionTimeForTest = Now; }
	static void HandleAnswer(UCrowdyServerObjectComponent& Component, const FVector& Sent, const FCrowdyServerCallResult& Result)
	{
		Component.HandlePositionAnswer(Component.PositionSerial, Sent, Result);
	}
	static bool InFlight(const UCrowdyServerObjectComponent& Component) { return Component.bPositionInFlight; }
	static bool Armed(const UCrowdyServerObjectComponent& Component) { return Component.PositionInX != nullptr; }
};

namespace CrowdyServerObjectPositionTests
{
	using namespace CrowdyServerObjectTests;
	using FAccess = FCrowdyServerObjectPositionTestAccess;

	inline constexpr const TCHAR* PositionTopic = TEXT("boss");
	inline constexpr const TCHAR* MoveFunction = TEXT("Move");
	inline constexpr const TCHAR* MoveMethod = TEXT("move");
	const FVector OwnerStart(1000.0, 2000.0, 300.0);

	/** Makes the rig's test_boss a Game Kit starter with a Move function of its own and the functions a bad Position Function would name. */
	void ConfigurePositionKit(UCrowdyServerObjectDefinition& Definition, FName PositionFunction)
	{
		Definition.CodeSource = ECrowdyServerCodeSource::GameKitStarter;
		Definition.KitStateTopic = PositionTopic;
		Definition.WatchedFields.Reset();
		Definition.KitPositionFunction = PositionFunction;
		if (Definition.Functions.Num() > 1)
		{
			return;
		}
		auto Add = [&Definition](const TCHAR* Name, UScriptStruct* Params, UScriptStruct* Reply)
		{
			FCrowdyServerFunction& Function = Definition.Functions.AddDefaulted_GetRef();
			Function.Name = Name;
			Function.Params = Params;
			Function.Reply = Reply;
		};
		Add(MoveFunction, FCrowdyServerObjectTestMoveInputs::StaticStruct(), FCrowdyServerObjectTestMoveReply::StaticStruct());
		Add(TEXT("Teleport"), FCrowdyServerObjectTestAttackParams::StaticStruct(), nullptr);
		Add(TEXT("Stumble"), FCrowdyServerObjectTestIntegerMoveInputs::StaticStruct(), nullptr);
		Add(TEXT("Slide"), FCrowdyServerObjectTestMoveInputs::StaticStruct(), FCrowdyServerObjectTestAttackReply::StaticStruct());
		Add(TEXT("Glide"), FCrowdyServerObjectTestMoveInputs::StaticStruct(), nullptr);
	}

	bool BakeWithPositionFunction(UCrowdyServerObjectDefinition& Definition, FName PositionFunction, FString& OutErrors)
	{
		Definition.KitPositionFunction = PositionFunction;
		TArray<FString> Errors;
		const bool bBaked = Definition.Bake(Errors);
		OutErrors = FString::Join(Errors, TEXT("; "));
		return bBaked;
	}

	TArray<uint8> PositionKitMessage(uint64 MessageEpoch, uint64 Seq, const TArray<uint8>& Body)
	{
		const int32 Count = Body.IsEmpty() ? 0 : Body[0] & 0x0f;
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		Writer.MapHeader(4 + Count);
		WriteKey(Writer, "v");
		Writer.UInt(CrowdyExec::KitProtocolVersion);
		WriteKey(Writer, "epoch");
		Writer.UInt(MessageEpoch);
		WriteKey(Writer, "seq");
		Writer.UInt(Seq);
		WriteKey(Writer, "kind");
		WriteKey(Writer, "snapshot");
		if (Count > 0)
		{
			Bytes.Append(Body.GetData() + 1, Body.Num() - 1);
		}
		return Bytes;
	}

	bool PayloadHasBytes(const TArray<uint8>& Payload, const TArray<uint8>& Needle)
	{
		for (int32 At = 0; At + Needle.Num() <= Payload.Num(); ++At)
		{
			if (FMemory::Memcmp(Payload.GetData() + At, Needle.GetData(), Needle.Num()) == 0)
			{
				return true;
			}
		}
		return false;
	}

	bool ReadPayloadNumber(const TArray<uint8>& Payload, int32& At, double& Out)
	{
		if (At >= Payload.Num())
		{
			return false;
		}
		const uint8 Tag = Payload[At++];
		if (Tag <= 0x7f || Tag >= 0xe0)
		{
			Out = Tag <= 0x7f ? Tag : static_cast<int8>(Tag);
			return true;
		}
		if (Tag == 0xcc && At + 1 <= Payload.Num())
		{
			Out = Payload[At++];
			return true;
		}
		if (Tag == 0xcd && At + 2 <= Payload.Num())
		{
			Out = (Payload[At] << 8) | Payload[At + 1];
			At += 2;
			return true;
		}
		if (Tag == 0xcb && At + 8 <= Payload.Num())
		{
			uint64 Bits = 0;
			for (int32 Byte = 0; Byte < 8; ++Byte)
			{
				Bits = (Bits << 8) | Payload[At++];
			}
			FMemory::Memcpy(&Out, &Bits, sizeof(Out));
			return true;
		}
		return false;
	}

	/** Reads the call's inputs, a map of exactly x and y, by key. */
	bool ReadMovePayload(const TArray<uint8>& Payload, double& OutX, double& OutY)
	{
		if (Payload.IsEmpty() || (Payload[0] & 0xf0) != 0x80 || (Payload[0] & 0x0f) != 2)
		{
			return false;
		}
		int32 At = 1;
		bool bHasX = false;
		bool bHasY = false;
		for (int32 Pair = 0; Pair < 2; ++Pair)
		{
			if (At + 2 > Payload.Num() || Payload[At] != 0xa1)
			{
				return false;
			}
			const uint8 Key = Payload[At + 1];
			At += 2;
			double& Out = Key == 'x' ? OutX : OutY;
			if ((Key != 'x' && Key != 'y') || !ReadPayloadNumber(Payload, At, Out))
			{
				return false;
			}
			(Key == 'x' ? bHasX : bHasY) = true;
		}
		return bHasX && bHasY;
	}

	bool PayloadIsPosition(const TArray<uint8>& Payload, double X, double Y)
	{
		double ReadX = 0.0;
		double ReadY = 0.0;
		return ReadMovePayload(Payload, ReadX, ReadY) && FMath::IsNearlyEqual(ReadX, X, 1e-9) && FMath::IsNearlyEqual(ReadY, Y, 1e-9);
	}

	bool PayloadHasText(const TArray<uint8>& Payload, const FString& Text)
	{
		return PayloadHasBytes(Payload, Utf8Bytes(Text));
	}

	/** A Ready Game Kit starter held by a component on an actor, over the scripted gateway. */
	struct FPositionFixture
	{
		FAutomationTestBase& Test;
		FRig Rig;
		TStrongObjectPtr<AActor> Actor;
		USceneComponent* Root = nullptr;
		TStrongObjectPtr<UCrowdyServerObjectComponent> Component;
		TStrongObjectPtr<UCrowdyServerObjectComponent> Second;
		TStrongObjectPtr<UCrowdyServerObjectPositionTestListener> Listener;

		explicit FPositionFixture(FAutomationTestBase& InTest)
			: Test(InTest)
			, Rig(InTest)
		{
		}

		bool Begin(FName PositionFunction, UClass* OwnerClass = AActor::StaticClass(), ECrowdyPositionReport Report = ECrowdyPositionReport::ThisActor)
		{
			ConfigurePositionKit(*Rig.Definition, PositionFunction);
			FString Errors;
			const bool bBaked = Rig.IsValid() && BakeWithPositionFunction(*Rig.Definition, PositionFunction, Errors);
			if (!Test.TestTrue(FString::Printf(TEXT("the position starter bakes (%s)"), *Errors), bBaked))
			{
				return false;
			}
			Actor.Reset(NewObject<AActor>(GetTransientPackage(), OwnerClass));
			Root = NewObject<USceneComponent>(Actor.Get());
			Actor->SetRootComponent(Root);
			MoveTo(OwnerStart);
			Component.Reset(NewObject<UCrowdyServerObjectComponent>(Actor.Get()));
			Component->Definition = Rig.Definition.Get();
			Component->InstanceId = Rig.InstanceId;
			Component->PositionReport = Report;
			Listener.Reset(NewObject<UCrowdyServerObjectPositionTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
			Component->OnPositionKept.AddDynamic(Listener.Get(), &UCrowdyServerObjectPositionTestListener::HandleKept);
			FAccess::UseServerObjects(*Component, Rig.Subsystem.Get());
			FAccess::Join(*Component);
			UCrowdyServerObject* Object = Component->GetServerObject();
			FSentFrame Subscribe;
			FSentFrame Read;
			if (!Test.TestNotNull(TEXT("the component joins the starter"), Object) || !Rig.Open()
				|| !Test.TestTrue(TEXT("the starter subscribes to its State Topic"), Rig.WaitForFrame(KindSubscribe, PositionTopic, Subscribe))
				|| !Test.TestTrue(TEXT("the starter reads with snapshot"), Rig.WaitForFrame(KindCall, TEXT("snapshot"), Read)))
			{
				return false;
			}
			Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
			Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, PositionKitMessage(Epoch, 1, Rig.Encode(FCrowdyExecTestBossState())));
			return Test.TestTrue(TEXT("the first snapshot makes the starter Ready"),
				Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }));
		}

		/** A second component on the same actor and Server Object, reporting this actor too. */
		bool AddSecond()
		{
			Second.Reset(NewObject<UCrowdyServerObjectComponent>(Actor.Get()));
			Second->Definition = Rig.Definition.Get();
			Second->InstanceId = Rig.InstanceId;
			Second->PositionReport = ECrowdyPositionReport::ThisActor;
			FAccess::UseServerObjects(*Second, Rig.Subsystem.Get());
			FAccess::Join(*Second);
			return Test.TestTrue(TEXT("the second component holds the same Server Object and arms its reporter"),
				Second->GetServerObject() == Component->GetServerObject() && FAccess::Armed(*Second));
		}

		void ReportSecond(double Now)
		{
			FAccess::Report(*Second, Now);
		}

		/** Pushes the next snapshot, which changes the State. */
		bool ChangeState(uint64 Seq, int32 Health)
		{
			UCrowdyServerObject* Object = Component->GetServerObject();
			Rig.PushTopic(PositionTopic, PositionKitMessage(Epoch, Seq, Rig.Encode(Boss(Health, false, ECrowdyExecTestPhase::Calm))));
			return Test.TestTrue(TEXT("the State change arrives"), Rig.PumpUntil([Object, Health]() { return HealthOf(Object) == Health; }));
		}

		void MoveTo(const FVector& Location)
		{
			Root->SetRelativeLocation_Direct(Location);
			Root->UpdateComponentToWorld();
		}

		FVector Location() const
		{
			return Actor->GetActorLocation();
		}

		void Report(double Now)
		{
			FAccess::Report(*Component, Now);
		}

		/** Calls that went out and were not taken yet. */
		int32 MovesSent()
		{
			Rig.PumpFor(0.15);
			return Rig.CountFrames(KindCall, MoveMethod);
		}

		bool TakeMove(FSentFrame& Frame)
		{
			return Test.TestTrue(TEXT("a move is sent"), Rig.WaitForFrame(KindCall, MoveMethod, Frame));
		}

		bool WaitAnswered()
		{
			return Test.TestTrue(TEXT("the report completes"), Rig.PumpUntil([this]() { return !FAccess::InFlight(*Component); }));
		}

		bool Answer(const FSentFrame& Frame, double X, double Y)
		{
			FCrowdyServerObjectTestMoveReply Reply;
			Reply.X = X;
			Reply.Y = Y;
			Rig.Reply(Frame.Rid, ECrowdyNativeExecStatus::Ok, Rig.Encode(Reply));
			return WaitAnswered();
		}

		/** The starter says no with a code, as it does to a player who is not fighting. */
		bool Refuse(const FSentFrame& Frame)
		{
			Rig.Reply(Frame.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("kit:not_member: not in the fight")));
			return WaitAnswered();
		}

		/** The call fails with no code. */
		bool Fail(const FSentFrame& Frame)
		{
			Rig.Reply(Frame.Rid, ECrowdyNativeExecStatus::AppError, Utf8Bytes(TEXT("something broke")));
			return WaitAnswered();
		}
	};
}

using namespace CrowdyServerObjectPositionTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionFunctionBakeTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionFunctionBake", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionFunctionBakeTest::RunTest(const FString& Parameters)
{
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	UCrowdyServerObjectDefinition& Definition = *Rig.Definition;
	ConfigurePositionKit(Definition, NAME_None);
	FString Errors;
	TestTrue(FString::Printf(TEXT("a starter with no Position Function bakes (%s)"), *Errors), BakeWithPositionFunction(Definition, NAME_None, Errors));
	TestFalse(TEXT("and tracks no player"), Definition.ReportsPosition());

	TestTrue(FString::Printf(TEXT("Move, with double X and Y in and out, bakes (%s)"), *Errors), BakeWithPositionFunction(Definition, MoveFunction, Errors));
	TestTrue(TEXT("and the starter reports position"), Definition.ReportsPosition());
	TestTrue(TEXT("a function with the inputs and no reply bakes"), BakeWithPositionFunction(Definition, TEXT("Glide"), Errors));

	TestFalse(TEXT("a Position Function the starter does not have is refused"), BakeWithPositionFunction(Definition, TEXT("Nope"), Errors));
	TestTrue(FString::Printf(TEXT("saying so (%s)"), *Errors), Errors.Contains(TEXT("is not a function of this starter")));

	FCrowdyServerFunction* Move = Definition.Functions.FindByPredicate([](const FCrowdyServerFunction& Function) { return Function.Name == MoveFunction; });
	if (!TestNotNull(TEXT("the Move function"), Move))
	{
		return false;
	}
	Move->bSendOnce = true;
	TestFalse(TEXT("a Send Once Position Function is refused"), BakeWithPositionFunction(Definition, MoveFunction, Errors));
	TestTrue(FString::Printf(TEXT("saying it is Send Once (%s)"), *Errors), Errors.Contains(TEXT("is Send Once")));
	Move->bSendOnce = false;

	TestFalse(TEXT("inputs with no X and Y are refused"), BakeWithPositionFunction(Definition, TEXT("Teleport"), Errors));
	TestTrue(FString::Printf(TEXT("naming the inputs (%s)"), *Errors), Errors.Contains(TEXT("needs inputs with double variables X and Y")));
	TestFalse(TEXT("an X and Y that are not doubles are refused"), BakeWithPositionFunction(Definition, TEXT("Stumble"), Errors));
	TestTrue(FString::Printf(TEXT("the same way (%s)"), *Errors), Errors.Contains(TEXT("needs inputs with double variables X and Y")));

	TestFalse(TEXT("a reply with no X and Y is refused"), BakeWithPositionFunction(Definition, TEXT("Slide"), Errors));
	TestTrue(FString::Printf(TEXT("naming the reply (%s)"), *Errors), Errors.Contains(TEXT("must reply with nothing or with double variables X and Y")));

	Definition.CodeSource = ECrowdyServerCodeSource::Generated;
	Definition.KitPositionFunction = MoveFunction;
	TestFalse(TEXT("a type that is no Game Kit starter reports no position"), Definition.ReportsPosition());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionReportsOwnerLocationTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionReportsOwnerLocationInMetres", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionReportsOwnerLocationTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	TestTrue(TEXT("a Ready starter with a Position Function arms the reporter"), FAccess::Armed(*Fixture.Component));
	const FVector Location(1234.5, -678.9, 50.0);
	Fixture.MoveTo(Location);
	Fixture.Report(0.0);
	FSentFrame Frame;
	if (!Fixture.TakeMove(Frame))
	{
		return false;
	}
	TestTrue(TEXT("the call carries the owner's X over 100 as x and its Y over 100 as y, and nothing else"), PayloadIsPosition(Frame.Payload, Location.X / 100.0, Location.Y / 100.0));
	TestFalse(TEXT("with no action id, as a call that may repeat"), PayloadHasText(Frame.Payload, TEXT("action_id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionOneReportInFlightTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionKeepsOneReportInFlight", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionOneReportInFlightTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	Fixture.Report(0.0);
	FSentFrame First;
	if (!Fixture.TakeMove(First))
	{
		return false;
	}
	// Not a whole number of metres: the kit writes those as integers.
	Fixture.MoveTo(OwnerStart +FVector(512.5, 0.0, 0.0));
	Fixture.Report(0.2);
	Fixture.Report(0.4);
	TestEqual(TEXT("two timer fires while a report is unanswered send nothing"), Fixture.MovesSent(), 0);

	if (!Fixture.Answer(First, OwnerStart.X / 100.0, OwnerStart.Y / 100.0))
	{
		return false;
	}
	Fixture.Report(0.6);
	FSentFrame Second;
	if (!Fixture.TakeMove(Second))
	{
		return false;
	}
	TestTrue(TEXT("the next report is the newest location, not the one skipped"), PayloadIsPosition(Second.Payload, (OwnerStart.X + 512.5) / 100.0, OwnerStart.Y / 100.0));
	TestEqual(TEXT("and one report at a time"), Fixture.MovesSent(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionSnapsToKeptPositionTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionSnapsToKeptPosition", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionSnapsToKeptPositionTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	FSentFrame Frame;
	Fixture.Report(0.0);
	if (!Fixture.TakeMove(Frame) || !Fixture.Answer(Frame, 12.0, 20.0))
	{
		return false;
	}
	const FVector Kept(1200.0, 2000.0, OwnerStart.Z);
	TestTrue(TEXT("a kept position 2 m away moves the owner there, keeping its height"), Fixture.Location().Equals(Kept, 0.01));
	if (!TestEqual(TEXT("and tells the listeners once"), Fixture.Listener->Kept.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("with the position, in Unreal units, at the owner's height"), Fixture.Listener->Kept[0].Equals(Kept, 0.01));

	Fixture.Report(1.0);
	if (!Fixture.TakeMove(Frame) || !Fixture.Answer(Frame, 12.04, 20.0))
	{
		return false;
	}
	TestEqual(TEXT("a kept position 4 cm from the reported one is not a correction"), Fixture.Listener->Kept.Num(), 1);
	TestTrue(TEXT("and does not move the owner"), Fixture.Location().Equals(Kept, 0.01));

	Fixture.Report(2.0);
	if (!Fixture.TakeMove(Frame) || !Fixture.Answer(Frame, 12.06, 20.0))
	{
		return false;
	}
	TestEqual(TEXT("one 6 cm away is"), Fixture.Listener->Kept.Num(), 2);
	TestTrue(TEXT("and moves the owner"), Fixture.Location().Equals(FVector(1206.0, 2000.0, OwnerStart.Z), 0.01));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionSnapOffTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionKeptWithoutSnap", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionSnapOffTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	Fixture.Component->bSnapToKeptPosition = false;
	FSentFrame Frame;
	Fixture.Report(0.0);
	if (!Fixture.TakeMove(Frame) || !Fixture.Answer(Frame, 12.0, 20.0))
	{
		return false;
	}
	TestTrue(TEXT("with Snap To Kept Position off the owner stays where it is"), Fixture.Location().Equals(OwnerStart, 0.01));
	if (TestEqual(TEXT("and the listeners still hear the kept position"), Fixture.Listener->Kept.Num(), 1))
	{
		TestTrue(TEXT("at the owner's height"), Fixture.Listener->Kept[0].Equals(FVector(1200.0, 2000.0, OwnerStart.Z), 0.01));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionRefusalHoldsReportsTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionRefusalHoldsReportsForASecond", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionRefusalHoldsReportsTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	FSentFrame Frame;
	Fixture.Report(0.0);
	if (!Fixture.TakeMove(Frame) || !Fixture.Fail(Frame))
	{
		return false;
	}
	TestEqual(TEXT("a failure moves nobody"), Fixture.Listener->Kept.Num(), 0);
	Fixture.MoveTo(OwnerStart +FVector(300.0, 0.0, 0.0));
	Fixture.Report(0.9);
	TestEqual(TEXT("no report goes out within a second of a failure with no code"), Fixture.MovesSent(), 0);
	Fixture.Report(1.1);
	TestTrue(TEXT("and reports resume after it"), Fixture.TakeMove(Frame));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionCodedRefusalHoldsUntilStateChangesTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionCodedRefusalHoldsUntilTheStateChanges", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionCodedRefusalHoldsUntilStateChangesTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	FSentFrame Frame;
	Fixture.Report(0.0);
	if (!Fixture.TakeMove(Frame) || !Fixture.Refuse(Frame))
	{
		return false;
	}
	Fixture.MoveTo(OwnerStart + FVector(300.0, 0.0, 0.0));
	Fixture.Report(1.5);
	TestEqual(TEXT("a coded refusal holds reports longer than a second"), Fixture.MovesSent(), 0);

	FAccess::SetTime(*Fixture.Component, 0.3);
	if (!Fixture.ChangeState(2, 2500))
	{
		return false;
	}
	Fixture.Report(0.6);
	TestEqual(TEXT("a State change 0.3 s after the refusal does not release it before a second"), Fixture.MovesSent(), 0);
	Fixture.Report(1.1);
	if (!Fixture.TakeMove(Frame) || !Fixture.Refuse(Frame))
	{
		return false;
	}

	FAccess::SetTime(*Fixture.Component, 3.1);
	if (!Fixture.ChangeState(3, 2600))
	{
		return false;
	}
	Fixture.MoveTo(OwnerStart + FVector(600.0, 0.0, 0.0));
	Fixture.Report(3.1);
	if (!TestTrue(TEXT("one 2 s after the refusal releases it at once"), Fixture.TakeMove(Frame)) || !Fixture.Refuse(Frame))
	{
		return false;
	}

	Fixture.MoveTo(OwnerStart + FVector(900.0, 0.0, 0.0));
	Fixture.Report(7.0);
	TestEqual(TEXT("with no State change the hold lasts 5 seconds"), Fixture.MovesSent(), 0);
	Fixture.Report(8.2);
	TestTrue(TEXT("and reports resume after them"), Fixture.TakeMove(Frame));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionReportTimesOutTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionUnansweredReportTimesOut", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionReportTimesOutTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	FSentFrame Frame;
	Fixture.Report(0.0);
	if (!Fixture.TakeMove(Frame))
	{
		return false;
	}
	Fixture.Rig.Tick(1.5f);
	if (!Fixture.WaitAnswered())
	{
		return false;
	}
	Fixture.MoveTo(OwnerStart + FVector(300.0, 0.0, 0.0));
	Fixture.Report(0.5);
	TestEqual(TEXT("an unanswered report is given up on after a second and holds reports for a second"), Fixture.MovesSent(), 0);
	Fixture.Report(1.1);
	TestTrue(TEXT("then reports resume"), Fixture.TakeMove(Frame));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionStandingStillTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionStandingStillReportsOncePerSecond", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionStandingStillTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	FSentFrame Frame;
	Fixture.Report(0.0);
	if (!Fixture.TakeMove(Frame) || !Fixture.Answer(Frame, OwnerStart.X / 100.0, OwnerStart.Y / 100.0))
	{
		return false;
	}
	Fixture.Report(0.5);
	TestEqual(TEXT("an owner that stands still sends nothing within a second"), Fixture.MovesSent(), 0);
	Fixture.MoveTo(OwnerStart +FVector(0.5, 0.0, 0.0));
	Fixture.Report(0.6);
	TestEqual(TEXT("half a centimetre is still standing"), Fixture.MovesSent(), 0);

	Fixture.Report(1.0);
	if (!Fixture.TakeMove(Frame) || !Fixture.Answer(Frame, OwnerStart.X / 100.0, OwnerStart.Y / 100.0))
	{
		return false;
	}
	Fixture.MoveTo(OwnerStart +FVector(500.0, 0.0, 0.0));
	Fixture.Report(1.2);
	TestTrue(TEXT("a second later it reports again, and an owner that moved is reported at once"), Fixture.TakeMove(Frame));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionNoFunctionNeverReportsTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionNeverReportsWithoutAFunction", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionNoFunctionNeverReportsTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(NAME_None))
	{
		return false;
	}
	TestFalse(TEXT("a starter with no Position Function arms nothing"), FAccess::Armed(*Fixture.Component));
	Fixture.Report(0.0);
	Fixture.MoveTo(OwnerStart +FVector(500.0, 0.0, 0.0));
	Fixture.Report(2.0);
	TestEqual(TEXT("and never reports"), Fixture.MovesSent(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionReportSwitchesTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionReportsOnlyWhenWanted", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionReportSwitchesTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	Fixture.Component->PositionReport = ECrowdyPositionReport::Off;
	Fixture.Report(0.0);
	TestEqual(TEXT("Position Report off never reports"), Fixture.MovesSent(), 0);

	Fixture.Component->PositionReport = ECrowdyPositionReport::ThisActor;
	FSentFrame Frame;
	Fixture.Report(0.0);
	if (!Fixture.TakeMove(Frame) || !Fixture.Answer(Frame, OwnerStart.X / 100.0, OwnerStart.Y / 100.0))
	{
		return false;
	}

	FAccess::Leave(*Fixture.Component);
	TestFalse(TEXT("leaving the Server Object stops the reporter"), FAccess::Armed(*Fixture.Component));
	Fixture.MoveTo(OwnerStart +FVector(500.0, 0.0, 0.0));
	Fixture.Report(5.0);
	TestEqual(TEXT("and nothing is reported"), Fixture.MovesSent(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionOtherPlayersPawnTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionIgnoresAPawnThisGameDoesNotControl", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionOtherPlayersPawnTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction, APawn::StaticClass(), ECrowdyPositionReport::PlayerPawn))
	{
		return false;
	}
	TestTrue(TEXT("the reporter is armed on a pawn too"), FAccess::Armed(*Fixture.Component));
	Fixture.Report(0.0);
	Fixture.Report(2.0);
	TestEqual(TEXT("a pawn no player controller drives is never reported"), Fixture.MovesSent(), 0);

	// A game without a server runs every controller locally, an AI's too.
	APawn* Pawn = Cast<APawn>(Fixture.Actor.Get());
	Pawn->Controller = NewObject<ACrowdyServerObjectPositionTestController>(GetTransientPackage());
	if (!TestTrue(TEXT("a controlled pawn counts as locally controlled here"), Pawn->IsLocallyControlled()))
	{
		return false;
	}
	Fixture.Report(4.0);
	Fixture.Report(6.0);
	TestEqual(TEXT("nor is a pawn a controller other than a player's drives"), Fixture.MovesSent(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionNonPawnOwnerTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionReportsANonPawnOnlyAsThisActor", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionNonPawnOwnerTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction, AActor::StaticClass(), ECrowdyPositionReport::PlayerPawn))
	{
		return false;
	}
	Fixture.Report(0.0);
	TestEqual(TEXT("an owner that is no pawn is not reported by default"), Fixture.MovesSent(), 0);

	Fixture.Component->PositionReport = ECrowdyPositionReport::ThisActor;
	Fixture.Report(0.0);
	FSentFrame Frame;
	TestTrue(TEXT("This Actor reports it wherever it is"), Fixture.TakeMove(Frame));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionOneReporterPerObjectTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionOneReporterPerServerObject", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionOneReporterPerObjectTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction) || !Fixture.AddSecond())
	{
		return false;
	}
	FSentFrame Frame;
	Fixture.Report(0.0);
	Fixture.ReportSecond(0.0);
	if (!Fixture.TakeMove(Frame))
	{
		return false;
	}
	TestEqual(TEXT("two components on one Server Object send one report per fire"), Fixture.MovesSent(), 0);
	if (!Fixture.Answer(Frame, OwnerStart.X / 100.0, OwnerStart.Y / 100.0))
	{
		return false;
	}

	Fixture.MoveTo(OwnerStart + FVector(512.5, 0.0, 0.0));
	Fixture.ReportSecond(0.6);
	TestEqual(TEXT("and the second never reports while the first does"), Fixture.MovesSent(), 0);

	FAccess::Leave(*Fixture.Component);
	Fixture.ReportSecond(1.2);
	TestTrue(TEXT("when the reporter leaves, the other takes over"), Fixture.TakeMove(Frame));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionSnapKeepsMovementTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionSnapKeepsMovementMadeMeanwhile", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionSnapKeepsMovementTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	FSentFrame Frame;
	Fixture.Report(0.0);
	if (!Fixture.TakeMove(Frame))
	{
		return false;
	}
	Fixture.MoveTo(FVector(1100.0, 2050.0, 350.0));
	if (!Fixture.Answer(Frame, 10.06, 20.0))
	{
		return false;
	}
	TestTrue(TEXT("the correction of 6 cm is applied to where the owner is now, not to where it was"),
		Fixture.Location().Equals(FVector(1106.0, 2050.0, 350.0), 0.01));
	if (TestEqual(TEXT("and heard"), Fixture.Listener->Kept.Num(), 1))
	{
		TestTrue(TEXT("with the location it gives the owner"), Fixture.Listener->Kept[0].Equals(FVector(1106.0, 2050.0, 350.0), 0.01));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectPositionIgnoresANonFiniteReplyTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PositionIgnoresANonFiniteReply", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerObjectPositionIgnoresANonFiniteReplyTest::RunTest(const FString& Parameters)
{
	FPositionFixture Fixture(*this);
	if (!Fixture.Begin(MoveFunction))
	{
		return false;
	}
	FSentFrame Frame;
	Fixture.Report(0.0);
	if (!Fixture.TakeMove(Frame))
	{
		return false;
	}
	FCrowdyServerObjectTestMoveReply Reply;
	Reply.X = std::numeric_limits<double>::infinity();
	Reply.Y = 20.0;
	FCrowdyServerCallResult Result;
	Result.Outcome = ECrowdyServerCallOutcome::Success;
	Result.Reply = FInstancedStruct::Make(Reply);
	FAccess::HandleAnswer(*Fixture.Component, OwnerStart, Result);
	TestTrue(TEXT("a reply with an infinite X moves nothing"), Fixture.Location().Equals(OwnerStart, 0.01));
	TestEqual(TEXT("and tells nobody"), Fixture.Listener->Kept.Num(), 0);
	return true;
}

#endif
