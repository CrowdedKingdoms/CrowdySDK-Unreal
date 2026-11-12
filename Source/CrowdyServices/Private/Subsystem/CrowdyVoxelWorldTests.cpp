#include "Subsystem/CrowdyVoxelWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SceneComponent.h"
#include "CrowdyVoxelWorldTestTypes.h"
#include "GameFramework/Actor.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/Voxel/FChunkChangedNotification.h"
#include "Misc/AutomationTest.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyVoxelWorldTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
	constexpr double Frame = 1.0 / 60.0;

	struct FFetchCall
	{
		FInt64Vector Low;
		FInt64Vector High;
		TFunction<void(const FCrowdyChunkFetch&)> Done;

		FInt64Vector Size() const { return High - Low + FInt64Vector(1, 1, 1); }
	};

	struct FSent
	{
		uint64 Ticket = 0;
		FCrowdyVoxelWrite Write;
	};

	/** A voxel world with the platform replaced: fetches wait until answered, sends are recorded with tickets. */
	struct FRig
	{
		TStrongObjectPtr<UCrowdyVoxelWorld> World;
		TStrongObjectPtr<UCrowdyVoxelWorldRecorder> Recorder;
		TStrongObjectPtr<AActor> Actor;
		TMap<FInt64Vector, FCrowdyVoxelChunk> Platform;
		TArray<FFetchCall> Fetches;
		TArray<FSent> Sent;
		TSharedPtr<const bool> LastCancelled;
		uint64 NextTicket = 1;
		bool bTakeSends = true;
		double Time = 100.0;

		FRig()
		{
			World.Reset(NewObject<UCrowdyVoxelWorld>(GetTransientPackage()));
			Recorder.Reset(NewObject<UCrowdyVoxelWorldRecorder>());
			World->InitializeForTest(
				[this](const FCrowdyVoxelWrite& Write, FString& OutError) -> uint64
				{
					if (!bTakeSends)
					{
						OutError = TEXT("refused");
						return 0;
					}
					Sent.Add({NextTicket, Write});
					return NextTicket++;
				},
				[this](const FInt64Vector& Low, const FInt64Vector& High, const TSharedRef<const bool>& Cancelled, TFunction<void(const FCrowdyChunkFetch&)> Done)
				{
					LastCancelled = Cancelled;
					Fetches.Add({Low, High, MoveTemp(Done)});
				});
			World->OnChunkLoaded.AddDynamic(Recorder.Get(), &UCrowdyVoxelWorldRecorder::HandleLoaded);
			World->OnChunkUnloaded.AddDynamic(Recorder.Get(), &UCrowdyVoxelWorldRecorder::HandleUnloaded);
			World->OnChunkChanged.AddDynamic(Recorder.Get(), &UCrowdyVoxelWorldRecorder::HandleChanged);
			World->OnEditDone.AddDynamic(Recorder.Get(), &UCrowdyVoxelWorldRecorder::HandleDone);
			World->SetConnectedForTest(true);
			World->SetLocalChunkForTest(FInt64Vector::ZeroValue);
			World->SetNowForTest(Time);

			Actor.Reset(NewObject<AActor>(GetTransientPackage()));
			USceneComponent* Root = NewObject<USceneComponent>(Actor.Get());
			Actor->SetRootComponent(Root);
		}

		void MoveActor(const FVector& Location)
		{
			Actor->GetRootComponent()->SetWorldLocation(Location);
		}

		void Step(const double Seconds = Frame)
		{
			Time += Seconds;
			World->SetNowForTest(Time);
			World->TickForTest();
		}

		static FCrowdyVoxelChunk Empty(const FInt64Vector& Coord)
		{
			FCrowdyVoxelChunk Chunk;
			Chunk.X = Coord.X;
			Chunk.Y = Coord.Y;
			Chunk.Z = Coord.Z;
			Chunk.Grid.SetNumZeroed(4096);
			return Chunk;
		}

		FCrowdyVoxelChunk& Store(const FInt64Vector& Coord)
		{
			FCrowdyVoxelChunk& Chunk = Platform.Add(Coord, Empty(Coord));
			Chunk.bStored = true;
			return Chunk;
		}

		/** A chunk the platform never stored but holds edits for, as a box fetch answers it. */
		FCrowdyVoxelChunk& EditOnly(const FInt64Vector& Coord, const FIntVector& Position, int32 Type)
		{
			FCrowdyVoxelChunk& Chunk = Platform.Add(Coord, Empty(Coord));
			Chunk.Grid[Position.X + 16 * Position.Y + 256 * Position.Z] = static_cast<uint8>(Type);
			FCrowdyVoxel& Edit = Chunk.Edits.AddDefaulted_GetRef();
			Edit.Position = Position;
			Edit.Type = Type;
			return Chunk;
		}

		/** Answers every fetch asked so far from Platform, as a box fetch would: a chunk it holds nothing for is left out. */
		void AnswerFetches()
		{
			TArray<FFetchCall> Calls = MoveTemp(Fetches);
			for (FFetchCall& Call : Calls)
			{
				FCrowdyChunkFetch Answer;
				Answer.Result = ECrowdyVoxelResult::Ok;
				for (const TPair<FInt64Vector, FCrowdyVoxelChunk>& Entry : Platform)
				{
					const FInt64Vector& C = Entry.Key;
					const bool bInside = C.X >= Call.Low.X && C.X <= Call.High.X && C.Y >= Call.Low.Y && C.Y <= Call.High.Y
						&& C.Z >= Call.Low.Z && C.Z <= Call.High.Z;
					if (bInside)
					{
						Answer.Chunks.Add(Entry.Value);
					}
				}
				Call.Done(Answer);
			}
		}

		/** Follow the actor at range 1 around chunk 0 and load the 27 chunks. */
		void Load()
		{
			World->Follow(Actor.Get(), 1);
			Step();
			AnswerFetches();
			Step();
		}

		static FCrowdyVoxelWrite Write(int32 X, int32 Y, int32 Z, int32 Type, const FInt64Vector& Chunk = FInt64Vector::ZeroValue)
		{
			FCrowdyVoxelWrite Out;
			Out.ChunkX = Chunk.X;
			Out.ChunkY = Chunk.Y;
			Out.ChunkZ = Chunk.Z;
			Out.Voxel.Position = FIntVector(X, Y, Z);
			Out.Voxel.Type = Type;
			return Out;
		}

		void Receipt(uint64 Ticket, ECrowdyTrackedSendEvent Event, uint8 Sequence, uint8 Code = 0)
		{
			World->HandleTrackedSend({Ticket, Event, Sequence, Code});
		}

		void Live(const FCrowdyVoxelWrite& Write, int64 Epoch, bool bOwn = false, uint8 Sequence = 0)
		{
			UCrowdyVoxelWorld::FLiveEdit Edit;
			Edit.Chunk = FInt64Vector(Write.ChunkX, Write.ChunkY, Write.ChunkZ);
			Edit.Voxel = Write.Voxel;
			Edit.EpochMillis = Epoch;
			Edit.bOwnActor = bOwn;
			Edit.Sequence = Sequence;
			World->HandleLiveEdit(Edit);
		}

		int32 TypeAt(int32 X, int32 Y, int32 Z, const FInt64Vector& Chunk = FInt64Vector::ZeroValue) const
		{
			return World->GetVoxel(Chunk.X, Chunk.Y, Chunk.Z, FIntVector(X, Y, Z)).Type;
		}

		/** Fetches of a cube of that range on every axis; range 0 is one chunk. */
		int32 CountFetches(int32 Range) const
		{
			const int64 Side = 2 * Range + 1;
			return CountBoxes(FInt64Vector(Side, Side, Side));
		}

		int32 CountBoxes(const FInt64Vector& Size) const
		{
			return Fetches.FilterByPredicate([&Size](const FFetchCall& Call) { return Call.Size() == Size; }).Num();
		}

		/** Chunks the fetches asked so far span together, and the most any one spans. */
		int64 CoveredChunks(int64& OutLargest) const
		{
			int64 Covered = 0;
			OutLargest = 0;
			for (const FFetchCall& Call : Fetches)
			{
				const FInt64Vector Size = Call.Size();
				Covered += Size.X * Size.Y * Size.Z;
				OutLargest = FMath::Max(OutLargest, Size.X * Size.Y * Size.Z);
			}
			return Covered;
		}

		bool AnyFetchHolds(const FInt64Vector& Coord) const
		{
			return Fetches.ContainsByPredicate([&Coord](const FFetchCall& Call)
			{
				return Coord.X >= Call.Low.X && Coord.X <= Call.High.X && Coord.Y >= Call.Low.Y && Coord.Y <= Call.High.Y
					&& Coord.Z >= Call.Low.Z && Coord.Z <= Call.High.Z;
			});
		}
	};

	struct FReloadAnswer
	{
		int32 Count = 0;
		ECrowdyVoxelResult Result = ECrowdyVoxelResult::Ok;
		FString Message;
	};

	/** Asks for a reload whose answer is kept and also logged after the broadcasts, as "reload <result>". */
	TSharedRef<FReloadAnswer> Reload(FRig& Rig, const FInt64Vector& Coord = FInt64Vector::ZeroValue)
	{
		TSharedRef<FReloadAnswer> Answer = MakeShared<FReloadAnswer>();
		UCrowdyVoxelWorldRecorder* Recorder = Rig.Recorder.Get();
		Rig.World->ReloadChunk(Coord.X, Coord.Y, Coord.Z, [Answer, Recorder](ECrowdyVoxelResult Result, const FString& Message)
		{
			++Answer->Count;
			Answer->Result = Result;
			Answer->Message = Message;
			Recorder->Log.Add(FString::Printf(TEXT("reload %s"), *UEnum::GetValueAsString(Result)));
		});
		return Answer;
	}

	/** Answers every fetch asked so far with Result and no chunks. */
	void FailFetches(FRig& Rig, ECrowdyVoxelResult Result)
	{
		TArray<FFetchCall> Calls = MoveTemp(Rig.Fetches);
		for (FFetchCall& Call : Calls)
		{
			FCrowdyChunkFetch Answer;
			Answer.Result = Result;
			Answer.Message = TEXT("FORBIDDEN: no");
			Call.Done(Answer);
		}
	}
}

using namespace CrowdyVoxelWorldTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldLoadsAroundFollowerTest, "CrowdySDK.Voxels.World.LoadsAroundFollower", Flags)
bool FCrowdyVoxelWorldLoadsAroundFollowerTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Store(FInt64Vector::ZeroValue).Grid[1 + 16 * 2 + 256 * 3] = 9;

	Rig.World->Follow(Rig.Actor.Get(), 1);
	Rig.Step();
	TestEqual(TEXT("a follower with nothing loaded reads its whole cube once"), Rig.CountFetches(1), 1);
	TestEqual(TEXT("and nothing one by one"), Rig.CountFetches(0), 0);
	TestFalse(TEXT("nothing is loaded before the answers"), Rig.World->IsChunkLoaded(0, 0, 0));

	Rig.AnswerFetches();
	Rig.Step();
	TestEqual(TEXT("27 chunks load"), Rig.Recorder->Loaded.Num(), 27);
	TestEqual(TEXT("the stored cube reads back"), Rig.TypeAt(1, 2, 3), 9);
	TestTrue(TEXT("a chunk the platform does not store loads as empty"), Rig.World->IsChunkLoaded(1, 1, 1) && Rig.TypeAt(0, 0, 0, FInt64Vector(1, 1, 1)) == 0);
	TestFalse(TEXT("nothing past range is loaded"), Rig.World->IsChunkLoaded(2, 0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldFetchesOnlyMissingTest, "CrowdySDK.Voxels.World.FetchesOnlyMissingOnCrossing", Flags)
bool FCrowdyVoxelWorldFetchesOnlyMissingTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	Rig.MoveActor(FVector(1600.0 + 50.0, 50.0, 50.0));
	Rig.Step();
	TestEqual(TEXT("crossing into chunk 1 fetches one box"), Rig.Fetches.Num(), 1);
	TestTrue(TEXT("holding only the new slab of 9"), Rig.Fetches.Num() == 1 && Rig.Fetches[0].Low == FInt64Vector(2, -1, -1)
		&& Rig.Fetches[0].High == FInt64Vector(2, 1, 1));
	TestTrue(TEXT("the slab two behind stays within the unload band"), Rig.World->IsChunkLoaded(-1, 0, 0));

	Rig.AnswerFetches();
	Rig.MoveActor(FVector(3 * 1600.0 + 50.0, 50.0, 50.0));
	Rig.Step();
	TestFalse(TEXT("a chunk past range plus one unloads"), Rig.World->IsChunkLoaded(-1, 0, 0) || Rig.World->IsChunkLoaded(0, 0, 0));
	TestTrue(TEXT("unloading is reported"), Rig.Recorder->Unloaded.Contains(FInt64Vector(0, 0, 0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldListsLoadedChunksTest, "CrowdySDK.Voxels.World.ListsLoadedChunks", Flags)
bool FCrowdyVoxelWorldListsLoadedChunksTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.World->Follow(Rig.Actor.Get(), 1);
	Rig.Step();
	int32 WhileLoading = 0;
	Rig.World->ForEachLoadedChunk([&WhileLoading](int64, int64, int64) { ++WhileLoading; });
	TestEqual(TEXT("a chunk still loading is not listed"), WhileLoading, 0);

	Rig.AnswerFetches();
	TSet<FInt64Vector> Listed;
	Rig.World->ForEachLoadedChunk([&Listed](int64 X, int64 Y, int64 Z) { Listed.Add(FInt64Vector(X, Y, Z)); });
	TestEqual(TEXT("every loaded chunk is listed once"), Listed.Num(), 27);
	TestTrue(TEXT("with its coordinates"), Listed.Contains(FInt64Vector(1, -1, 0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldSlabAsBoxesTest, "CrowdySDK.Voxels.World.CrossingReadsSlabsInOnePageBoxes", Flags)
bool FCrowdyVoxelWorldSlabAsBoxesTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.World->Follow(Rig.Actor.Get(), 3);
	Rig.Step();
	int64 Largest = 0;
	TestEqual(TEXT("a follower with nothing loaded asks for its whole cube of 343"), Rig.CoveredChunks(Largest), 343ll);
	TestTrue(TEXT("in boxes of one page each"), Largest <= UCrowdyVoxels::PageSize);
	TestTrue(TEXT("about one box a page"), Rig.Fetches.Num() >= 11 && Rig.Fetches.Num() <= 14);
	Rig.AnswerFetches();
	Rig.MoveActor(FVector(1600.0 + 50.0, 50.0, 50.0));
	Rig.Step();
	TestEqual(TEXT("a step along x reads its slab of 49"), Rig.CoveredChunks(Largest), 49ll);
	TestEqual(TEXT("as two one-page boxes"), Rig.Fetches.Num(), 2);
	TestTrue(TEXT("one chunk deep"), Rig.Fetches.Num() == 2 && Rig.Fetches[0].Low.X == 4 && Rig.Fetches[0].High.X == 4 && Rig.Fetches[1].Low.X == 4);
	Rig.AnswerFetches();
	TestTrue(TEXT("the slab loads"), Rig.World->IsChunkLoaded(4, 3, -3));
	TestFalse(TEXT("nothing past the follower's range is loaded"), Rig.World->IsChunkLoaded(5, 0, 0));

	// A diagonal step uncovers an L of two slabs: boxes holding exactly the 91 chunks not loaded.
	Rig.MoveActor(FVector(2 * 1600.0 + 50.0, 1600.0 + 50.0, 50.0));
	Rig.Step();
	bool bAnyLoaded = false;
	for (const FFetchCall& Call : Rig.Fetches)
	{
		const FInt64Vector Size = Call.Size();
		for (int64 Cell = 0; Cell < Size.X * Size.Y * Size.Z; ++Cell)
		{
			const FInt64Vector At = Call.Low + FInt64Vector(Cell % Size.X, (Cell / Size.X) % Size.Y, Cell / (Size.X * Size.Y));
			bAnyLoaded |= Rig.World->IsChunkLoaded(At.X, At.Y, At.Z);
		}
	}
	TestEqual(TEXT("the boxes together hold the 91 new chunks"), Rig.CoveredChunks(Largest), 91ll);
	TestTrue(TEXT("each one page"), Largest <= UCrowdyVoxels::PageSize);
	TestTrue(TEXT("in few boxes"), Rig.Fetches.Num() <= 5);
	TestFalse(TEXT("and no chunk already loaded"), bAnyLoaded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldWideRangeBoxesTest, "CrowdySDK.Voxels.World.RangeSevenReadsInOnePageBoxes", Flags)
bool FCrowdyVoxelWorldWideRangeBoxesTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.World->Follow(Rig.Actor.Get(), 7);
	Rig.Step();
	int64 Largest = 0;
	TestEqual(TEXT("the first load at range 7 asks for 3,375 chunks"), Rig.CoveredChunks(Largest), 3375ll);
	TestTrue(TEXT("in one-page boxes"), Largest <= UCrowdyVoxels::PageSize);
	AddInfo(FString::Printf(TEXT("range 7 first load: %d boxes"), Rig.Fetches.Num()));
	TestTrue(TEXT("close to one box per 32 chunks"), Rig.Fetches.Num() <= 120);
	Rig.AnswerFetches();
	Rig.MoveActor(FVector(1600.0 + 50.0, 50.0, 50.0));
	Rig.Step();
	TestEqual(TEXT("a step at range 7 reads its slab of 225"), Rig.CoveredChunks(Largest), 225ll);
	TestTrue(TEXT("in one-page boxes"), Largest <= UCrowdyVoxels::PageSize);
	TestEqual(TEXT("eight of them, read side by side"), Rig.Fetches.Num(), 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldUnionOfFollowersTest, "CrowdySDK.Voxels.World.UnionOfFollowers", Flags)
bool FCrowdyVoxelWorldUnionOfFollowersTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	TStrongObjectPtr<AActor> Other(NewObject<AActor>(GetTransientPackage()));
	USceneComponent* Root = NewObject<USceneComponent>(Other.Get());
	Other->SetRootComponent(Root);
	Root->SetWorldLocation(FVector(10 * 1600.0 + 50.0, 50.0, 50.0));

	Rig.World->Follow(Rig.Actor.Get(), 1);
	Rig.World->Follow(Other.Get(), 1);
	Rig.Step();
	TestEqual(TEXT("54 missing chunks are read as two cubes"), Rig.CountFetches(1), 2);
	Rig.AnswerFetches();
	Rig.Step();
	TestTrue(TEXT("both followers' chunks load"), Rig.World->IsChunkLoaded(0, 0, 0) && Rig.World->IsChunkLoaded(10, 0, 0));
	TestEqual(TEXT("the union loads once each"), Rig.Recorder->Loaded.Num(), 54);

	Rig.World->StopFollowing(Other.Get());
	Rig.Step();
	TestFalse(TEXT("a follower that stops takes its chunks with it"), Rig.World->IsChunkLoaded(10, 0, 0));
	TestTrue(TEXT("the other follower keeps its own"), Rig.World->IsChunkLoaded(0, 0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldEditConfirmedTest, "CrowdySDK.Voxels.World.EditShownSentConfirmed", Flags)
bool FCrowdyVoxelWorldEditConfirmedTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();

	const int64 EditId = Rig.World->SetVoxel(FRig::Write(4, 5, 0, 7));
	TestTrue(TEXT("an edit id is never 0"), EditId != 0);
	TestEqual(TEXT("the edit shows at once"), Rig.TypeAt(4, 5, 0), 7);
	TestEqual(TEXT("nothing is reported inside the call"), Rig.Recorder->Results.Num(), 0);

	Rig.Step();
	TestEqual(TEXT("the edit is sent once"), Rig.Sent.Num(), 1);
	TestTrue(TEXT("the change names the cell"), Rig.Recorder->ChangedPositions.Contains(FIntVector(4, 5, 0)));

	Rig.Receipt(Rig.Sent[0].Ticket, ECrowdyTrackedSendEvent::Sent, 33);
	Rig.Live(FRig::Write(4, 5, 0, 7), 1000, true, 33);
	Rig.Step();
	TestTrue(TEXT("the matching echo confirms it"), Rig.Recorder->Results.FindRef(EditId) == ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("the confirmed value stays"), Rig.TypeAt(4, 5, 0), 7);
	TestEqual(TEXT("a confirmed edit fetches nothing"), Rig.Fetches.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldEchoMustMatchTest, "CrowdySDK.Voxels.World.EchoMustMatchEveryField", Flags)
bool FCrowdyVoxelWorldEchoMustMatchTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();

	const int64 EditId = Rig.World->SetVoxel(FRig::Write(4, 5, 0, 7));
	Rig.Step();
	Rig.Receipt(Rig.Sent[0].Ticket, ECrowdyTrackedSendEvent::Sent, 33);
	Rig.Live(FRig::Write(4, 5, 0, 8), 1000, true, 33);
	Rig.Live(FRig::Write(4, 6, 0, 7), 1001, true, 33);
	Rig.Live(FRig::Write(4, 5, 0, 7), 1002, false, 33);
	Rig.Live(FRig::Write(4, 5, 0, 7), 1003, true, 34);
	Rig.Live(FRig::Write(4, 5, 0, 7, FInt64Vector(1, 0, 0)), 1004, true, 33);
	FCrowdyVoxelWrite WithState = FRig::Write(4, 5, 0, 7);
	WithState.Voxel.State = {1};
	Rig.Live(WithState, 1005, true, 33);
	Rig.Step();
	TestFalse(TEXT("an echo differing in type, cell, actor, sequence, chunk or state confirms nothing"), Rig.Recorder->Results.Contains(EditId));

	Rig.Step(UCrowdyVoxelWorld::AnswerTimeoutSeconds + 0.1);
	TestTrue(TEXT("with no matching echo it ends with no answer"), Rig.Recorder->Results.FindRef(EditId) == ECrowdyVoxelResult::NoAnswer);
	TestEqual(TEXT("and the chunk is fetched again to settle it"), Rig.CountFetches(0), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldRefusalPutsBackTest, "CrowdySDK.Voxels.World.RefusalPutsBackNewestConfirmed", Flags)
bool FCrowdyVoxelWorldRefusalPutsBackTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Store(FInt64Vector::ZeroValue).Grid[4 + 16 * 5] = 2;
	Rig.Load();

	const int64 EditId = Rig.World->SetVoxel(FRig::Write(4, 5, 0, 7));
	Rig.Step();
	Rig.Receipt(Rig.Sent[0].Ticket, ECrowdyTrackedSendEvent::Sent, 33);
	// Another player's committed change arrives while this player's edit waits.
	Rig.Live(FRig::Write(4, 5, 0, 5), 2000);
	TestEqual(TEXT("this player's waiting edit stays on top"), Rig.TypeAt(4, 5, 0), 7);

	Rig.Receipt(Rig.Sent[0].Ticket, ECrowdyTrackedSendEvent::Refused, 33, 7);
	Rig.Recorder->Log.Reset();
	Rig.Step();
	TestTrue(TEXT("the refusal is reported"), Rig.Recorder->Results.FindRef(EditId) == ECrowdyVoxelResult::Refused);
	TestEqual(TEXT("the cell shows the other player's change, not the value before the edit"), Rig.TypeAt(4, 5, 0), 5);
	Rig.Step();
	const int32 Done = Rig.Recorder->Log.IndexOfByKey(FString::Printf(TEXT("done %lld ECrowdyVoxelResult::Refused"), EditId));
	const int32 Back = Rig.Recorder->Log.IndexOfByKey(TEXT("changed 4 5 0"));
	TestTrue(TEXT("the cell is put back after the refusal is reported"), Done != INDEX_NONE && Back > Done);
	TestEqual(TEXT("the refusal is settled by one fetch"), Rig.CountFetches(0), 1);

	Rig.Platform[FInt64Vector::ZeroValue].Grid[4 + 16 * 5] = 5;
	Rig.AnswerFetches();
	TestEqual(TEXT("the settling fetch agrees"), Rig.TypeAt(4, 5, 0), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldShownBeforeAnswerTest, "CrowdySDK.Voxels.World.ShownBeforeAnswer", Flags)
bool FCrowdyVoxelWorldShownBeforeAnswerTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	Rig.Recorder->Log.Reset();

	// Answered in the same frame it was set, and with the value the cell already holds: it is still named first.
	const int64 EditId = Rig.World->SetVoxel(FRig::Write(4, 5, 0, 0));
	Rig.World->SetConnectedForTest(false);
	Rig.Step();
	TestFalse(TEXT("an answer is held until the edit has been shown"), Rig.Recorder->Results.Contains(EditId));
	Rig.Step();
	const int32 Shown = Rig.Recorder->Log.IndexOfByKey(TEXT("changed 4 5 0"));
	const int32 Done = Rig.Recorder->Log.IndexOfByKey(FString::Printf(TEXT("done %lld ECrowdyVoxelResult::NotSignedIn"), EditId));
	TestTrue(TEXT("the broadcast that shows the edit comes before its answer"), Shown != INDEX_NONE && Done > Shown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldOrderedByTimeTest, "CrowdySDK.Voxels.World.LiveEditsOrderedByTime", Flags)
bool FCrowdyVoxelWorldOrderedByTimeTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	Rig.Live(FRig::Write(1, 1, 1, 6), 5000);
	Rig.Live(FRig::Write(1, 1, 1, 3), 4000);
	TestEqual(TEXT("an older edit arriving late does not replace a newer one"), Rig.TypeAt(1, 1, 1), 6);
	Rig.Live(FRig::Write(1, 1, 1, 4), 6000);
	TestEqual(TEXT("a newer edit does"), Rig.TypeAt(1, 1, 1), 4);
	Rig.Live(FRig::Write(1, 1, 1, 9, FInt64Vector(5, 5, 5)), 1, false);
	TestEqual(TEXT("an edit for a chunk not loaded is ignored"), Rig.World->GetVoxel(5, 5, 5, FIntVector(1, 1, 1)).Type, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldLiveBoundsTest, "CrowdySDK.Voxels.World.LiveEditsBounded", Flags)
bool FCrowdyVoxelWorldLiveBoundsTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();

	Rig.Live(FRig::Write(16, 0, 0, 5), 10);
	TestEqual(TEXT("a position past the grid lands in the edits"), Rig.TypeAt(16, 0, 0), 5);
	TestEqual(TEXT("and leaves the grid alone"), Rig.TypeAt(0, 1, 0), 0);

	Rig.Live(FRig::Write(2, 2, 2, 300), 11);
	FCrowdyVoxelChunk Chunk;
	Rig.World->FindChunk(0, 0, 0, Chunk);
	TestEqual(TEXT("a type past a byte reads 0 in the grid"), static_cast<int32>(Chunk.Grid[2 + 16 * 2 + 256 * 2]), 0);
	TestEqual(TEXT("and keeps its type through Get Voxel"), Rig.TypeAt(2, 2, 2), 300);

	FCrowdyVoxelWrite Big = FRig::Write(3, 3, 3, 4);
	Big.Voxel.State.SetNumZeroed(1025);
	Rig.Live(Big, 12);
	TestEqual(TEXT("an edit with a state over 1,024 bytes is dropped"), Rig.TypeAt(3, 3, 3), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldHeldDuringLoadTest, "CrowdySDK.Voxels.World.EditsHeldWhileLoading", Flags)
bool FCrowdyVoxelWorldHeldDuringLoadTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.World->Follow(Rig.Actor.Get(), 1);
	Rig.Step();
	Rig.Live(FRig::Write(7, 7, 7, 8), 100);
	Rig.AnswerFetches();
	TestEqual(TEXT("an edit that arrived while the chunk loaded is applied over the fetch"), Rig.TypeAt(7, 7, 7), 8);

	FRig Overflow;
	Overflow.World->Follow(Overflow.Actor.Get(), 1);
	Overflow.Step();
	for (int32 Index = 0; Index <= 4096; ++Index)
	{
		Overflow.Live(FRig::Write(Index % 16, (Index / 16) % 16, 20 + Index / 256, 1), Index);
	}
	Overflow.AnswerFetches();
	TestEqual(TEXT("past 4,096 held edits the chunk is fetched again"), Overflow.CountFetches(0), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldMergedRefetchTest, "CrowdySDK.Voxels.World.NoAnswerRefetchesOncePerChunk", Flags)
bool FCrowdyVoxelWorldMergedRefetchTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	const TArray<int64> Ids = Rig.World->SetVoxels({FRig::Write(1, 0, 0, 1), FRig::Write(2, 0, 0, 2), FRig::Write(3, 0, 0, 3)});
	Rig.Step(0.1);
	TestEqual(TEXT("three edits are sent"), Rig.Sent.Num(), 3);
	Rig.Receipt(Rig.Sent[1].Ticket, ECrowdyTrackedSendEvent::Lapped, 0);
	Rig.Step(0.1);
	TestTrue(TEXT("a lapped edit ends with no answer before its timeout"), Rig.Recorder->Results.FindRef(Ids[1]) == ECrowdyVoxelResult::NoAnswer);
	TestFalse(TEXT("the others are still waiting"), Rig.Recorder->Results.Contains(Ids[0]) || Rig.Recorder->Results.Contains(Ids[2]));
	Rig.AnswerFetches();
	Rig.Step(UCrowdyVoxelWorld::AnswerTimeoutSeconds);
	for (const int64 EditId : Ids)
	{
		TestTrue(TEXT("each unanswered edit ends with no answer"), Rig.Recorder->Results.FindRef(EditId) == ECrowdyVoxelResult::NoAnswer);
	}
	TestEqual(TEXT("one fetch settles both that timed out in the same frame"), Rig.CountFetches(0), 1);

	Rig.Platform.Add(FInt64Vector::ZeroValue, FRig::Empty(FInt64Vector::ZeroValue)).Grid[1] = 1;
	Rig.AnswerFetches();
	Rig.Step();
	TestEqual(TEXT("an edit the platform applied stays"), Rig.TypeAt(1, 0, 0), 1);
	TestEqual(TEXT("an edit it did not is taken back"), Rig.TypeAt(3, 0, 0), 0);
	TestEqual(TEXT("a settling fetch asks for nothing more"), Rig.Fetches.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldSettleWhileFetchingTest, "CrowdySDK.Voxels.World.SettleWaitsForFetchInFlight", Flags)
bool FCrowdyVoxelWorldSettleWhileFetchingTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	Rig.Step(UCrowdyVoxelWorld::SettleSeconds);
	TestEqual(TEXT("the settling read of the 27 held chunks is out"), Rig.CountFetches(1), 1);

	const int64 EditId = Rig.World->SetVoxel(FRig::Write(9, 9, 9, 4));
	Rig.Step();
	Rig.Step(UCrowdyVoxelWorld::AnswerTimeoutSeconds + 0.1);
	TestTrue(TEXT("the edit ends with no answer"), Rig.Recorder->Results.FindRef(EditId) == ECrowdyVoxelResult::NoAnswer);
	TestEqual(TEXT("no second fetch while one is out"), Rig.CountFetches(0), 0);

	// The read that was already out was asked for before the edit, so it cannot settle it.
	Rig.Store(FInt64Vector::ZeroValue).Grid[9 + 16 * 9 + 256 * 9] = 4;
	TArray<FFetchCall> Older = MoveTemp(Rig.Fetches);
	FCrowdyChunkFetch Stale;
	Stale.Result = ECrowdyVoxelResult::Ok;
	Older[0].Done(Stale);
	Rig.Step();
	TestEqual(TEXT("the cell is asked for again once that read is in"), Rig.CountFetches(0), 1);
	Rig.AnswerFetches();
	TestEqual(TEXT("and the platform's value stands"), Rig.TypeAt(9, 9, 9), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldReconnectTest, "CrowdySDK.Voxels.World.ReconnectSettlesAgain", Flags)
bool FCrowdyVoxelWorldReconnectTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	Rig.Step(UCrowdyVoxelWorld::SettleSeconds);
	Rig.AnswerFetches();
	TestTrue(TEXT("live after the first settle"), Rig.World->IsChunkLive(0, 0, 0));

	Rig.World->SetConnectedForTest(false);
	Rig.Step();
	TestFalse(TEXT("not live while disconnected"), Rig.World->IsChunkLive(0, 0, 0));
	Rig.World->SetConnectedForTest(true);
	Rig.Step();
	TestFalse(TEXT("not live again until the new connection settles"), Rig.World->IsChunkLive(0, 0, 0));
	Rig.Step(UCrowdyVoxelWorld::SettleSeconds);
	TestEqual(TEXT("the new connection settles with one more read"), Rig.CountFetches(1), 1);
	Rig.AnswerFetches();
	TestTrue(TEXT("live again after it"), Rig.World->IsChunkLive(0, 0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldFutureTimeTest, "CrowdySDK.Voxels.World.FutureTimeDoesNotPin", Flags)
bool FCrowdyVoxelWorldFutureTimeTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	const int64 NowMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000;
	Rig.Live(FRig::Write(2, 2, 2, 5), NowMs + 24 * 3600 * 1000);
	TestEqual(TEXT("an edit stamped a day ahead still applies"), Rig.TypeAt(2, 2, 2), 5);
	Rig.Live(FRig::Write(2, 2, 2, 6), NowMs);
	TestEqual(TEXT("but cannot hold the cell against the next real edit"), Rig.TypeAt(2, 2, 2), 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldPawnlessFrameTest, "CrowdySDK.Voxels.World.PawnlessFrameKeepsReach", Flags)
bool FCrowdyVoxelWorldPawnlessFrameTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	Rig.World->ClearLocalChunkForTest();
	Rig.Step();
	Rig.World->SetLocalChunkForTest(FInt64Vector::ZeroValue);
	Rig.Step();
	TestEqual(TEXT("a frame without a pawn fetches nothing again"), Rig.Fetches.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldInReachTest, "CrowdySDK.Voxels.World.InReachIsEightChunksFromThePlayer", Flags)
bool FCrowdyVoxelWorldInReachTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	TestFalse(TEXT("before the player's chunk is known nothing is in reach"), Rig.World->IsChunkInReach(0, 0, 0));
	Rig.Step();
	TestTrue(TEXT("the player's own chunk is in reach"), Rig.World->IsChunkInReach(0, 0, 0));
	TestTrue(TEXT("8 chunks away on every axis is in reach"), Rig.World->IsChunkInReach(8, -8, 8));
	TestFalse(TEXT("9 chunks away on one axis is not"), Rig.World->IsChunkInReach(0, -9, 0));
	TestFalse(TEXT("a chunk nobody loaded"), Rig.World->IsChunkLoaded(5, 0, 0));
	TestTrue(TEXT("is in reach all the same"), Rig.World->IsChunkInReach(5, 0, 0));
	TestFalse(TEXT("the farthest int64 chunk is not in reach"), Rig.World->IsChunkInReach(MIN_int64, 0, 0));

	Rig.World->SetLocalChunkForTest(FInt64Vector(20, 0, 0));
	Rig.Step();
	TestFalse(TEXT("reach moves with the player"), Rig.World->IsChunkInReach(0, 0, 0));
	TestTrue(TEXT("to its new chunk"), Rig.World->IsChunkInReach(28, 0, 0));
	Rig.World->ClearLocalChunkForTest();
	Rig.Step();
	TestTrue(TEXT("a frame without a pawn keeps the last chunk"), Rig.World->IsChunkInReach(28, 0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldPaceTest, "CrowdySDK.Voxels.World.PacedAt100ASecond", Flags)
bool FCrowdyVoxelWorldPaceTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	TArray<FCrowdyVoxelWrite> Writes;
	for (int32 Index = 0; Index < 400; ++Index)
	{
		Writes.Add(FRig::Write(Index % 16, (Index / 16) % 16, Index / 256, 1));
	}
	Rig.World->SetVoxels(Writes);
	for (int32 Tick = 0; Tick < 100; ++Tick)
	{
		Rig.Step(0.01);
	}
	TestTrue(TEXT("about 100 edits go out in a second"), Rig.Sent.Num() >= 95 && Rig.Sent.Num() <= 125);
	TestEqual(TEXT("the last write, still queued, is shown"), Rig.TypeAt(15, 8, 1), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldEarlyResultsTest, "CrowdySDK.Voxels.World.ImmediateFailuresNextFrame", Flags)
bool FCrowdyVoxelWorldEarlyResultsTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();

	const int64 Unloaded = Rig.World->SetVoxel(FRig::Write(0, 0, 0, 1, FInt64Vector(9, 9, 9)));
	FCrowdyVoxelWrite Wide = FRig::Write(0, 0, 0, 40000);
	const int64 TooWide = Rig.World->SetVoxel(Wide);
	TestEqual(TEXT("nothing is reported inside the call"), Rig.Recorder->Results.Num(), 0);
	TestEqual(TEXT("a refused write shows nothing"), Rig.TypeAt(0, 0, 0), 0);
	Rig.Step();
	TestTrue(TEXT("a chunk not loaded fails"), Rig.Recorder->Results.FindRef(Unloaded) == ECrowdyVoxelResult::Failed);
	TestTrue(TEXT("a type past 16 bits fails"), Rig.Recorder->Results.FindRef(TooWide) == ECrowdyVoxelResult::Failed);
	TestTrue(TEXT("edit ids are distinct"), Unloaded != TooWide);
	TestEqual(TEXT("nothing is sent"), Rig.Sent.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldDisconnectTest, "CrowdySDK.Voxels.World.DisconnectEndsWaitingEdits", Flags)
bool FCrowdyVoxelWorldDisconnectTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	TArray<FCrowdyVoxelWrite> Writes;
	for (int32 Index = 0; Index < 40; ++Index)
	{
		Writes.Add(FRig::Write(Index % 16, Index / 16, 0, 1));
	}
	const TArray<int64> Ids = Rig.World->SetVoxels(Writes);
	Rig.Step(0.05);
	Rig.Step(0.05);
	const int32 SentCount = Rig.Sent.Num();
	TestTrue(TEXT("some edits are out and some still queued"), SentCount > 1 && SentCount < Ids.Num());
	Rig.Receipt(Rig.Sent[0].Ticket, ECrowdyTrackedSendEvent::Sent, 5);

	Rig.World->SetConnectedForTest(false);
	Rig.Step();
	TestTrue(TEXT("an edit on the wire ends with no answer"), Rig.Recorder->Results.FindRef(Ids[0]) == ECrowdyVoxelResult::NoAnswer);
	TestTrue(TEXT("an edit queued but never on the wire ends not signed in"), Rig.Recorder->Results.FindRef(Ids[1]) == ECrowdyVoxelResult::NotSignedIn);
	TestTrue(TEXT("a queued edit ends not signed in"), Rig.Recorder->Results.FindRef(Ids.Last()) == ECrowdyVoxelResult::NotSignedIn);
	TestEqual(TEXT("nothing more is sent"), Rig.Sent.Num(), SentCount);
	TestEqual(TEXT("a queued edit is taken back"), Rig.TypeAt(Writes.Last().Voxel.Position.X, Writes.Last().Voxel.Position.Y, 0), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldLiveAfterSettleTest, "CrowdySDK.Voxels.World.LiveAfterSettle", Flags)
bool FCrowdyVoxelWorldLiveAfterSettleTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	TestFalse(TEXT("a chunk is not live before the connection settles"), Rig.World->IsChunkLive(0, 0, 0));

	Rig.Step(UCrowdyVoxelWorld::SettleSeconds);
	TestEqual(TEXT("settling reads the chunks held in each follower's cube and unload band once more"), Rig.CountFetches(1), 1);
	Rig.Live(FRig::Write(6, 6, 6, 3), 50);
	Rig.AnswerFetches();
	TestTrue(TEXT("after the settling read the chunk is live"), Rig.World->IsChunkLive(0, 0, 0));
	TestEqual(TEXT("an edit that arrived during the settling read survives it"), Rig.TypeAt(6, 6, 6), 3);

	Rig.World->SetLocalChunkForTest(FInt64Vector(20, 0, 0));
	Rig.Step();
	TestFalse(TEXT("a chunk past 8 of this player's actor is not live"), Rig.World->IsChunkLive(0, 0, 0));
	Rig.World->SetLocalChunkForTest(FInt64Vector::ZeroValue);
	Rig.Step();
	TestEqual(TEXT("coming back within reach fetches the 27 chunks again, in one box"), Rig.CountFetches(1), 1);
	TestEqual(TEXT("and nothing else"), Rig.Fetches.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldOverlayReadsTest, "CrowdySDK.Voxels.World.ReadsIncludeWaitingEdits", Flags)
bool FCrowdyVoxelWorldOverlayReadsTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Store(FInt64Vector::ZeroValue).Grid[0] = 2;
	Rig.Load();
	Rig.World->SetVoxel(FRig::Write(0, 0, 0, 0));
	Rig.World->SetVoxel(FRig::Write(1, 0, 0, 4));
	Rig.World->SetVoxel(FRig::Write(1, 0, 0, 6));

	FCrowdyVoxelChunk Chunk;
	TestTrue(TEXT("a loaded chunk is found"), Rig.World->FindChunk(0, 0, 0, Chunk));
	TestEqual(TEXT("the copy shows an erase"), static_cast<int32>(Chunk.Grid[0]), 0);
	TestEqual(TEXT("the copy shows the newest edit per cell"), static_cast<int32>(Chunk.Grid[1]), 6);

	TArray<TPair<FIntVector, int32>> Filled;
	Rig.World->ForEachFilledVoxel(0, 0, 0, [&Filled](FIntVector Position, int32 Type) { Filled.Add({Position, Type}); });
	TestEqual(TEXT("the visit sees one filled cell"), Filled.Num(), 1);
	TestTrue(TEXT("with the newest waiting type"), Filled.Num() == 1 && Filled[0].Key == FIntVector(1, 0, 0) && Filled[0].Value == 6);
	TestFalse(TEXT("a chunk not loaded is not found"), Rig.World->FindChunk(9, 9, 9, Chunk));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldReloadMergesTest, "CrowdySDK.Voxels.World.ReloadMergesAFreshFetch", Flags)
bool FCrowdyVoxelWorldReloadMergesTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Store(FInt64Vector::ZeroValue).Grid[1 + 16 * 2 + 256 * 3] = 9;
	Rig.Load();
	Rig.Platform[FInt64Vector::ZeroValue].Grid[1 + 16 * 2 + 256 * 3] = 0;
	Rig.Platform[FInt64Vector::ZeroValue].Grid[4 + 16 * 4 + 256 * 4] = 5;
	Rig.Recorder->Log.Reset();
	Rig.Recorder->Loaded.Reset();
	Rig.Recorder->ChangedPositions.Reset();

	const TSharedRef<FReloadAnswer> Answer = Reload(Rig);
	TestEqual(TEXT("nothing answers inside the call"), Answer->Count, 0);
	TestEqual(TEXT("the chunk is fetched once"), Rig.CountFetches(0), 1);
	Rig.Step();
	TestEqual(TEXT("nothing answers before the fetch does"), Answer->Count, 0);

	Rig.AnswerFetches();
	TestEqual(TEXT("nothing answers inside the fetch's answer"), Answer->Count, 0);
	Rig.Step();
	TestEqual(TEXT("one answer"), Answer->Count, 1);
	TestTrue(TEXT("the reload succeeded"), Answer->Result == ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("a removed cell reads empty"), Rig.TypeAt(1, 2, 3), 0);
	TestEqual(TEXT("a new cell reads its type"), Rig.TypeAt(4, 4, 4), 5);
	const int32 Removed = Rig.Recorder->Log.IndexOfByKey(TEXT("changed 1 2 3"));
	const int32 Added = Rig.Recorder->Log.IndexOfByKey(TEXT("changed 4 4 4"));
	const int32 Ok = Rig.Recorder->Log.IndexOfByKey(TEXT("reload ECrowdyVoxelResult::Ok"));
	TestTrue(TEXT("the removal and the new cell are named before the Ok"), Removed != INDEX_NONE && Added != INDEX_NONE && Ok > Removed && Ok > Added);
	TestEqual(TEXT("only the changed cells are named"), Rig.Recorder->ChangedPositions.Num(), 2);
	TestFalse(TEXT("an already loaded chunk is not reported loaded again"), Rig.Recorder->Loaded.Contains(FInt64Vector::ZeroValue));
	Rig.Step();
	TestEqual(TEXT("still one answer"), Answer->Count, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldReloadUnloadedTest, "CrowdySDK.Voxels.World.ReloadOfAnUnloadedChunkFails", Flags)
bool FCrowdyVoxelWorldReloadUnloadedTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	const TSharedRef<FReloadAnswer> Answer = Reload(Rig, FInt64Vector(5, 5, 5));
	TestEqual(TEXT("nothing answers inside the call"), Answer->Count, 0);
	Rig.Step();
	TestEqual(TEXT("one answer on the next frame"), Answer->Count, 1);
	TestTrue(TEXT("a chunk not loaded fails"), Answer->Result == ECrowdyVoxelResult::Failed);
	TestEqual(TEXT("nothing is fetched"), Rig.Fetches.Num(), 0);
	TestFalse(TEXT("and the chunk is not loaded by it"), Rig.World->IsChunkLoaded(5, 5, 5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldReloadIgnoresEarlierFetchTest, "CrowdySDK.Voxels.World.ReloadIgnoresAFetchAlreadyOut", Flags)
bool FCrowdyVoxelWorldReloadIgnoresEarlierFetchTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	Rig.Step(UCrowdyVoxelWorld::SettleSeconds);
	TestEqual(TEXT("the settling read is out"), Rig.CountFetches(1), 1);

	const TSharedRef<FReloadAnswer> Answer = Reload(Rig);
	TestEqual(TEXT("no second fetch while one is out"), Rig.CountFetches(0), 0);
	Rig.AnswerFetches();
	Rig.Step();
	TestEqual(TEXT("the read asked for before the reload does not answer it"), Answer->Count, 0);
	TestEqual(TEXT("the chunk is asked for again once that read is in"), Rig.CountFetches(0), 1);

	Rig.Store(FInt64Vector::ZeroValue).Grid[2 + 16 * 2 + 256 * 2] = 7;
	Rig.AnswerFetches();
	Rig.Step();
	TestTrue(TEXT("the later fetch answers it"), Answer->Count == 1 && Answer->Result == ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("with the platform's value"), Rig.TypeAt(2, 2, 2), 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldReloadOverlapTest, "CrowdySDK.Voxels.World.OverlappingReloadsShareALaterFetch", Flags)
bool FCrowdyVoxelWorldReloadOverlapTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	const TSharedRef<FReloadAnswer> First = Reload(Rig);
	TestEqual(TEXT("the first reload fetches the chunk"), Rig.CountFetches(0), 1);
	const TSharedRef<FReloadAnswer> Second = Reload(Rig);
	const TSharedRef<FReloadAnswer> Third = Reload(Rig);
	TestEqual(TEXT("reloads asked for while that fetch is out ask for nothing yet"), Rig.CountFetches(0), 1);

	Rig.AnswerFetches();
	Rig.Step();
	TestTrue(TEXT("the first fetch answers the first reload"), First->Count == 1 && First->Result == ECrowdyVoxelResult::Ok);
	TestTrue(TEXT("but not those asked for after it began"), Second->Count == 0 && Third->Count == 0);
	TestEqual(TEXT("which share one later fetch"), Rig.CountFetches(0), 1);

	Rig.AnswerFetches();
	Rig.Step();
	TestTrue(TEXT("that fetch answers both"), Second->Count == 1 && Third->Count == 1
		&& Second->Result == ECrowdyVoxelResult::Ok && Third->Result == ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("the first answers only once"), First->Count, 1);
	TestEqual(TEXT("and nothing more is fetched"), Rig.Fetches.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldReloadFailureTest, "CrowdySDK.Voxels.World.ReloadFailsWithItsFetchOrTimesOut", Flags)
bool FCrowdyVoxelWorldReloadFailureTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	const TSharedRef<FReloadAnswer> Refused = Reload(Rig);
	FailFetches(Rig, ECrowdyVoxelResult::Refused);
	Rig.Step();
	TestTrue(TEXT("a refused fetch ends the reload refused"), Refused->Count == 1 && Refused->Result == ECrowdyVoxelResult::Refused);
	TestTrue(TEXT("with the platform's words"), Refused->Message.Contains(TEXT("FORBIDDEN")));
	TestTrue(TEXT("and the chunk stays loaded"), Rig.World->IsChunkLoaded(0, 0, 0));

	const TSharedRef<FReloadAnswer> Late = Reload(Rig);
	TestEqual(TEXT("the reload's fetch is out"), Rig.CountFetches(0), 1);
	Rig.Step(UCrowdyVoxelWorld::ReloadTimeoutSeconds - 1.0);
	TestEqual(TEXT("nothing answers before 30 seconds"), Late->Count, 0);
	Rig.Step(1.1);
	TestTrue(TEXT("30 seconds without a fetch fails"), Late->Count == 1 && Late->Result == ECrowdyVoxelResult::Failed);
	Rig.AnswerFetches();
	Rig.Step();
	TestEqual(TEXT("a fetch that comes in later answers nothing more"), Late->Count, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldReloadSessionTest, "CrowdySDK.Voxels.World.ReloadEndsWhenTheSessionDrops", Flags)
bool FCrowdyVoxelWorldReloadSessionTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	const TSharedRef<FReloadAnswer> Waiting = Reload(Rig);
	Rig.World->SetConnectedForTest(false);
	Rig.Step();
	TestTrue(TEXT("a reload waiting when the connection drops ends not signed in"), Waiting->Count == 1 && Waiting->Result == ECrowdyVoxelResult::NotSignedIn);

	Rig.Fetches.Reset();
	const TSharedRef<FReloadAnswer> Offline = Reload(Rig);
	Rig.Step();
	TestTrue(TEXT("a reload asked for without a connection ends not signed in"), Offline->Count == 1 && Offline->Result == ECrowdyVoxelResult::NotSignedIn);
	TestEqual(TEXT("without a fetch"), Rig.Fetches.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldReloadChunkGoneTest, "CrowdySDK.Voxels.World.ReloadEndsWhenItsChunkUnloads", Flags)
bool FCrowdyVoxelWorldReloadChunkGoneTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	const TSharedRef<FReloadAnswer> Waiting = Reload(Rig);
	Rig.World->StopFollowing(Rig.Actor.Get());
	Rig.Step();
	TestEqual(TEXT("a reload whose chunk unloads answers once"), Waiting->Count, 1);
	TestTrue(TEXT("not Ok"), Waiting->Result != ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("saying the chunk was unloaded"), Waiting->Message, FString(TEXT("the chunk was unloaded")));
	Rig.AnswerFetches();
	Rig.Step();
	TestEqual(TEXT("and a fetch landing after does not answer it again"), Waiting->Count, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldReloadOverlayTest, "CrowdySDK.Voxels.World.ReloadKeepsWaitingEditsOnTop", Flags)
bool FCrowdyVoxelWorldReloadOverlayTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Store(FInt64Vector::ZeroValue).Grid[3 + 16 * 3 + 256 * 3] = 2;
	Rig.Load();
	Rig.World->SetVoxel(FRig::Write(3, 3, 3, 6));
	Rig.Step();
	Rig.Platform[FInt64Vector::ZeroValue].Grid[3 + 16 * 3 + 256 * 3] = 4;
	Rig.Platform[FInt64Vector::ZeroValue].Grid[5 + 16 * 5 + 256 * 5] = 1;
	Rig.Recorder->ChangedPositions.Reset();

	const TSharedRef<FReloadAnswer> Answer = Reload(Rig);
	Rig.AnswerFetches();
	Rig.Step();
	TestTrue(TEXT("the reload succeeded"), Answer->Count == 1 && Answer->Result == ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("this player's waiting edit still shows over the fetched value"), Rig.TypeAt(3, 3, 3), 6);
	TestFalse(TEXT("so its cell is not named as changed"), Rig.Recorder->ChangedPositions.Contains(FIntVector(3, 3, 3)));
	TestTrue(TEXT("while another changed cell is"), Rig.Recorder->ChangedPositions.Contains(FIntVector(5, 5, 5)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldReloadWorldEndTest, "CrowdySDK.Voxels.World.ReloadDroppedWhenTheWorldEnds", Flags)
bool FCrowdyVoxelWorldReloadWorldEndTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	const TSharedRef<FReloadAnswer> Answer = Reload(Rig);
	const TSharedRef<FReloadAnswer> Unloaded = Reload(Rig, FInt64Vector(9, 9, 9));
	Rig.World->EndWorldForTest();
	Rig.AnswerFetches();
	Rig.Step();
	TestEqual(TEXT("a reload waiting on its fetch runs nothing once the world ends"), Answer->Count, 0);
	TestEqual(TEXT("nor does one that had already failed"), Unloaded->Count, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldNeverStoredEditsTest, "CrowdySDK.Voxels.World.NeverStoredChunkLoadsWithItsEdits", Flags)
bool FCrowdyVoxelWorldNeverStoredEditsTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.EditOnly(FInt64Vector(1, 0, 0), FIntVector(2, 3, 4), 6);
	Rig.Load();
	TestEqual(TEXT("a first edit in a chunk the platform never stored shows on load"), Rig.TypeAt(2, 3, 4, FInt64Vector(1, 0, 0)), 6);
	FCrowdyVoxelChunk Chunk;
	TestTrue(TEXT("the chunk is found"), Rig.World->FindChunk(1, 0, 0, Chunk));
	TestFalse(TEXT("and says it is not stored"), Chunk.bStored);

	const TSharedRef<FReloadAnswer> Answer = Reload(Rig, FInt64Vector(1, 0, 0));
	TestEqual(TEXT("a reload reads that one chunk as a box"), Rig.CountFetches(0), 1);
	Rig.AnswerFetches();
	Rig.Step();
	TestTrue(TEXT("the reload succeeds"), Answer->Count == 1 && Answer->Result == ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("and the edit survives it"), Rig.TypeAt(2, 3, 4, FInt64Vector(1, 0, 0)), 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldChunkChangedReloadsTest, "CrowdySDK.Voxels.World.ChunkChangedReloadsALoadedChunk", Flags)
bool FCrowdyVoxelWorldChunkChangedReloadsTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Store(FInt64Vector::ZeroValue).Grid[1 + 16 * 2 + 256 * 3] = 9;
	Rig.Load();
	Rig.World->SetVoxel(FRig::Write(3, 3, 3, 6));
	Rig.Step();
	FCrowdyVoxelChunk& Replaced = Rig.Store(FInt64Vector::ZeroValue);
	Replaced.Grid[4 + 16 * 4 + 256 * 4] = 5;
	Replaced.Grid[3 + 16 * 3 + 256 * 3] = 2;
	Rig.Recorder->Log.Reset();
	Rig.Recorder->Loaded.Reset();
	Rig.Recorder->ChangedPositions.Reset();

	Rig.World->HandleChunkChanged(FInt64Vector::ZeroValue);
	TestEqual(TEXT("nothing is fetched inside the call"), Rig.Fetches.Num(), 0);
	Rig.Step();
	TestEqual(TEXT("the next frame fetches the chunk once"), Rig.CountFetches(0), 1);
	TestTrue(TEXT("that chunk"), Rig.Fetches.Num() == 1 && Rig.Fetches[0].Low == FInt64Vector::ZeroValue);
	Rig.AnswerFetches();
	Rig.Step();
	TestEqual(TEXT("a removed cell reads empty"), Rig.TypeAt(1, 2, 3), 0);
	TestEqual(TEXT("a new cell reads its type"), Rig.TypeAt(4, 4, 4), 5);
	TestEqual(TEXT("this player's waiting edit stays on top"), Rig.TypeAt(3, 3, 3), 6);
	TestTrue(TEXT("On Chunk Changed names the changed cells"), Rig.Recorder->ChangedPositions.Contains(FIntVector(1, 2, 3))
		&& Rig.Recorder->ChangedPositions.Contains(FIntVector(4, 4, 4)));
	TestFalse(TEXT("but not the cell a waiting edit covers"), Rig.Recorder->ChangedPositions.Contains(FIntVector(3, 3, 3)));
	TestEqual(TEXT("no second On Chunk Loaded"), Rig.Recorder->Loaded.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldChunkChangedUnloadedTest, "CrowdySDK.Voxels.World.ChunkChangedIgnoresAnUnloadedChunk", Flags)
bool FCrowdyVoxelWorldChunkChangedUnloadedTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	Rig.World->HandleChunkChanged(FInt64Vector(9, 9, 9));
	Rig.World->HandleChunkChanged(FInt64Vector(MIN_int64, 0, MAX_int64));
	Rig.Step();
	Rig.Step();
	TestEqual(TEXT("a change to a chunk not loaded fetches nothing"), Rig.Fetches.Num(), 0);
	TestFalse(TEXT("and does not load it"), Rig.World->IsChunkLoaded(9, 9, 9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldChunkChangedBurstTest, "CrowdySDK.Voxels.World.ChunkChangedBurstCoalesces", Flags)
bool FCrowdyVoxelWorldChunkChangedBurstTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		Rig.World->HandleChunkChanged(FInt64Vector::ZeroValue);
	}
	Rig.Step();
	TestEqual(TEXT("a burst in one frame costs one fetch"), Rig.Fetches.Num(), 1);

	// Frames that take no time, so the settling read stays out of this count.
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		Rig.World->HandleChunkChanged(FInt64Vector::ZeroValue);
		Rig.Step(0.0);
	}
	TestEqual(TEXT("a burst while it is out asks for nothing yet"), Rig.Fetches.Num(), 1);
	Rig.AnswerFetches();
	Rig.Step();
	TestEqual(TEXT("and costs one follow-up once it is in"), Rig.Fetches.Num(), 1);
	Rig.AnswerFetches();
	Rig.Step();
	Rig.Step();
	TestEqual(TEXT("then nothing more"), Rig.Fetches.Num(), 0);
	TestEqual(TEXT("no second On Chunk Loaded for it"), Rig.Recorder->Loaded.FilterByPredicate(
		[](const FInt64Vector& Coord) { return Coord == FInt64Vector::ZeroValue; }).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldChunkChangedRetryTest, "CrowdySDK.Voxels.World.ChunkChangedRetriesAFailedFetch", Flags)
bool FCrowdyVoxelWorldChunkChangedRetryTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	Rig.World->HandleChunkChanged(FInt64Vector::ZeroValue);
	Rig.Step();
	TestEqual(TEXT("the change fetches the chunk"), Rig.CountFetches(0), 1);
	FailFetches(Rig, ECrowdyVoxelResult::Failed);
	Rig.Step();
	TestEqual(TEXT("a failed fetch is not retried before the back-off"), Rig.Fetches.Num(), 0);
	Rig.Step(2.1);
	TestEqual(TEXT("after it the chunk is fetched exactly once more"), Rig.Fetches.Num(), 1);
	TestTrue(TEXT("that chunk"), Rig.CountFetches(0) == 1 && Rig.Fetches[0].Low == FInt64Vector::ZeroValue);
	Rig.Step();
	TestEqual(TEXT("and once only"), Rig.Fetches.Num(), 1);

	// A change folded into a fetch already out is not lost when that fetch fails.
	FRig Folded;
	Folded.Load();
	const TSharedRef<FReloadAnswer> Answer = Reload(Folded);
	Folded.World->HandleChunkChanged(FInt64Vector::ZeroValue);
	Folded.Step();
	TestEqual(TEXT("the change waits behind the fetch already out"), Folded.Fetches.Num(), 1);
	FailFetches(Folded, ECrowdyVoxelResult::Failed);
	Folded.Step();
	TestTrue(TEXT("the reload that fetch served fails with it"), Answer->Count == 1 && Answer->Result == ECrowdyVoxelResult::Failed);
	Folded.Step(2.1);
	TestEqual(TEXT("and is fetched once the back-off has passed"), Folded.CountFetches(0), 1);
	Folded.Fetches.Reset();
	Folded.Step();
	TestEqual(TEXT("with nothing more asked for"), Folded.Fetches.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldChunkChangedManyTest, "CrowdySDK.Voxels.World.ChunkChangedOverManyChunksIsReadInBoxes", Flags)
bool FCrowdyVoxelWorldChunkChangedManyTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.World->Follow(Rig.Actor.Get(), 3);
	Rig.Step();
	Rig.AnswerFetches();
	Rig.Step();
	int32 Changed = 0;
	for (int64 Z = -3; Z <= 3 && Changed < 300; ++Z)
	{
		for (int64 Y = -3; Y <= 3 && Changed < 300; ++Y)
		{
			for (int64 X = -3; X <= 3 && Changed < 300; ++X, ++Changed)
			{
				Rig.World->HandleChunkChanged(FInt64Vector(X, Y, Z));
			}
		}
	}
	Rig.Step();
	int64 Largest = 0;
	TestEqual(TEXT("300 changed chunks are all asked for"), Rig.CoveredChunks(Largest), 300ll);
	TestTrue(TEXT("in one-page boxes"), Largest <= UCrowdyVoxels::PageSize);
	AddInfo(FString::Printf(TEXT("300 changed chunks: %d box reads"), Rig.Fetches.Num()));
	TestTrue(TEXT("about one read per 32 chunks, not one per chunk"), Rig.Fetches.Num() >= 10 && Rig.Fetches.Num() <= 13);

	// The same burst again while those reads are out costs one box read each once they are in, not one per chunk.
	for (int64 X = -3; X <= 3; ++X)
	{
		Rig.World->HandleChunkChanged(FInt64Vector(X, -3, -3));
	}
	Rig.Step();
	const int32 Out = Rig.Fetches.Num();
	Rig.AnswerFetches();
	TestEqual(TEXT("the 7 asked for again ride one follow-up box"), Rig.Fetches.Num(), 1);
	TestTrue(TEXT("nothing was asked while the reads were out"), Out >= 10 && Out <= 13);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldSettleRetryTest, "CrowdySDK.Voxels.World.FailedSettleIsRetried", Flags)
bool FCrowdyVoxelWorldSettleRetryTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	Rig.MoveActor(FVector(1600.0 + 50.0, 50.0, 50.0));
	Rig.Step();
	Rig.AnswerFetches();
	TestTrue(TEXT("the chunks one behind are held in the unload band"), Rig.World->IsChunkLoaded(-1, 0, 0));

	Rig.Step(UCrowdyVoxelWorld::SettleSeconds);
	int64 Largest = 0;
	TestEqual(TEXT("the settle reads the 36 chunks held in the cube and its band"), Rig.CoveredChunks(Largest), 36ll);
	TestTrue(TEXT("in one-page boxes"), Largest <= UCrowdyVoxels::PageSize);
	TestTrue(TEXT("the band is among them"), Rig.AnyFetchHolds(FInt64Vector(-1, 0, 0)));
	FailFetches(Rig, ECrowdyVoxelResult::Failed);
	Rig.Step();
	TestFalse(TEXT("a failed settle leaves the chunks not live"), Rig.World->IsChunkLive(0, 0, 0));

	Rig.Step(2.1);
	TestEqual(TEXT("after the back-off the settle is read again"), Rig.CoveredChunks(Largest), 36ll);
	Rig.AnswerFetches();
	Rig.Step();
	TestTrue(TEXT("and the chunks are live"), Rig.World->IsChunkLive(0, 0, 0) && Rig.World->IsChunkLive(-1, 0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldEndCancelsReadsTest, "CrowdySDK.Voxels.World.WorldEndCancelsItsReads", Flags)
bool FCrowdyVoxelWorldEndCancelsReadsTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.World->Follow(Rig.Actor.Get(), 1);
	Rig.Step();
	const TSharedPtr<const bool> Cancelled = Rig.LastCancelled;
	if (!TestTrue(TEXT("the read was handed a cancel flag"), Cancelled.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("which is clear while the world lives"), *Cancelled);
	Rig.World->EndWorldForTest();
	TestTrue(TEXT("and set when it ends, so the read asks for no further page"), *Cancelled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelWorldChunkChangedRoutedTest, "CrowdySDK.Voxels.World.ChunkChangedArrivesThroughTheRegistry", Flags)
bool FCrowdyVoxelWorldChunkChangedRoutedTest::RunTest(const FString& Parameters)
{
	FRig Rig;
	Rig.Load();
	FCrowdyServiceRegistry Registry;
	Rig.World->SubscribeForTest(Registry);

	// The frame a 146 decodes from: the envelope names the chunk, the body is the update time.
	const TArray<uint8> Uuid = TArray<uint8>(reinterpret_cast<const uint8*>("00000000000000000000000000000000"), 32);
	const TArray<uint8> Body = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
	FCrowdyFrame ChangedFrame;
	ChangedFrame.Opcode = static_cast<uint8>(ECrowdyMessageType::CHUNK_CHANGED_NOTIFICATION);
	ChangedFrame.Body = Body;
	ChangedFrame.bHasEnvelope = true;
	ChangedFrame.Envelope.Uuid = Uuid;
	ChangedFrame.Envelope.ChunkX = 1;
	TSharedRef<FChunkChangedNotification, ESPMode::ThreadSafe> Message = MakeShared<FChunkChangedNotification, ESPMode::ThreadSafe>();
	const bool bDecoded = Message->DecodePayload(ChangedFrame);
	TestTrue(TEXT("the frame decodes"), bDecoded);
	if (bDecoded)
	{
		Registry.DispatchMessage(Message);
	}
	Rig.Step();
	TestEqual(TEXT("a dispatched 146 for a held chunk fetches it"), Rig.CountFetches(0), 1);
	TestTrue(TEXT("that chunk"), Rig.Fetches.Num() == 1 && Rig.Fetches[0].Low == FInt64Vector(1, 0, 0));

	// Released before the registry it subscribed to goes out of scope.
	Rig.World->EndWorldForTest();
	Rig.Fetches.Reset();
	Registry.DispatchMessage(Message);
	Rig.Step();
	TestEqual(TEXT("and after the world ends nothing is subscribed"), Rig.Fetches.Num(), 0);
	return true;
}

#endif
