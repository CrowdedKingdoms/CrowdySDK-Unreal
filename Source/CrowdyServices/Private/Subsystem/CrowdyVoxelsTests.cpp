#include "Subsystem/CrowdyVoxels.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "CrowdyVoxelDecode.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Subsystem/CrowdyVoxelLibrary.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags VoxelTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
	constexpr int32 Cells = 4096;

	int32 Index(int32 X, int32 Y, int32 Z)
	{
		return X + 16 * Y + 256 * Z;
	}

	FString GridJson(const TArray<uint8>& Grid)
	{
		return FString::Printf(TEXT("\"%s\""), *FBase64::Encode(Grid));
	}

	FString EditJson(int32 X, int32 Y, int32 Z, int32 Type, const FString& State = TEXT("null"))
	{
		return FString::Printf(TEXT("{\"voxelCoord\":{\"x\":%d,\"y\":%d,\"z\":%d},\"voxelType\":%d,\"state\":%s}"), X, Y, Z, Type, *State);
	}

	FString ChunkJson(int64 X, int64 Y, int64 Z, const FString& Grid, const FString& Edits = FString(), bool bTruncated = false)
	{
		return FString::Printf(
			TEXT("{\"chunkId\":\"1\",\"appId\":\"31337\",\"coordinates\":{\"x\":\"%lld\",\"y\":\"%lld\",\"z\":\"%lld\"},\"voxels\":%s,")
			TEXT("\"voxelStates\":[%s],\"voxelStatesTruncated\":%s,\"owner\":null,\"chunkState\":null,\"lods\":null}"),
			X, Y, Z, *Grid, *Edits, bTruncated ? TEXT("true") : TEXT("false"));
	}

	FString PageJson(const TArray<FString>& Chunks, int32 Limit = 32)
	{
		return FString::Printf(TEXT("{\"data\":{\"getChunksByDistance\":{\"limit\":%d,\"skip\":0,\"chunks\":[%s]}}}"), Limit, *FString::Join(Chunks, TEXT(",")));
	}

	FString OneJson(const FString& Chunk)
	{
		return FString::Printf(TEXT("{\"data\":{\"getChunk\":%s}}"), *Chunk);
	}

	/** One row of the edit log, with every member the vendored document selects. */
	FString RowJson(int32 X, int32 Y, int32 Z, int32 Type, const FString& State = TEXT("null"))
	{
		return FString::Printf(
			TEXT("{\"voxelUpdateId\":\"1\",\"appId\":\"31337\",\"location\":{\"x\":%d,\"y\":%d,\"z\":%d},\"voxelType\":%d,\"state\":%s,")
			TEXT("\"createdBy\":\"5\",\"createdAt\":\"2026-11-12T00:00:00.000Z\"}"),
			X, Y, Z, Type, *State);
	}

	FString GroupJson(int64 X, int64 Y, int64 Z, const TArray<FString>& Rows)
	{
		return FString::Printf(TEXT("{\"coordinates\":{\"x\":\"%lld\",\"y\":\"%lld\",\"z\":\"%lld\"},\"voxels\":[%s]}"), X, Y, Z, *FString::Join(Rows, TEXT(",")));
	}

	FString EditsPageJson(const TArray<FString>& Groups, int32 Limit = 32)
	{
		return FString::Printf(TEXT("{\"data\":{\"listVoxelUpdatesByDistance\":{\"centerCoordinate\":{\"x\":\"0\",\"y\":\"0\",\"z\":\"0\"},\"limit\":%d,\"skip\":0,\"chunks\":[%s]}}}"),
			Limit, *FString::Join(Groups, TEXT(",")));
	}

	TArray<uint8> EmptyGrid()
	{
		TArray<uint8> Grid;
		Grid.SetNumZeroed(Cells);
		return Grid;
	}

	// A MessagePack writer for hand-built pages: each call appends one value or header, big-endian.
	struct FPack
	{
		TArray<uint8> Bytes;

		FPack& Raw(std::initializer_list<uint8> Values)
		{
			Bytes.Append(Values.begin(), static_cast<int32>(Values.size()));
			return *this;
		}

		FPack& Str(const ANSICHAR* Text)
		{
			const int32 Length = FCStringAnsi::Strlen(Text);
			Bytes.Add(static_cast<uint8>(0xa0 | Length));
			Bytes.Append(reinterpret_cast<const uint8*>(Text), Length);
			return *this;
		}

		FPack& Map(uint8 Count) { return Raw({static_cast<uint8>(0x80 | Count)}); }
		FPack& Array(uint8 Count) { return Raw({static_cast<uint8>(0x90 | Count)}); }
	};

	TArray<FString> EmptyChunks(int32 Count, int64 FirstX)
	{
		const FString Encoded = GridJson(EmptyGrid());
		TArray<FString> Out;
		for (int32 I = 0; I < Count; ++I)
		{
			Out.Add(ChunkJson(FirstX + I, 0, 0, Encoded));
		}
		return Out;
	}

	struct FVoxelsRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyVoxels> Voxels;
		TSharedPtr<FCrowdyCppClient> Client;
		FCrowdyChunkFetch Answer;
		int32 Answers = 0;

		explicit FVoxelsRig(const FString& Body, int32 Status = 200)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Voxels(NewObject<UCrowdyVoxels>(Instance.Get()))
			, Client(FCrowdyCppClient::MakeForTest(Body, Status))
		{
			if (Client.IsValid())
			{
				Client->SetGameToken(TEXT("app-token"));
				Voxels->InitializeForTest(Client, 31337);
			}
		}

		void Fetch(int64 X, int64 Y, int64 Z, int32 Range)
		{
			Voxels->FetchChunksAround(X, Y, Z, Range, [this](const FCrowdyChunkFetch& Fetched) { Take(Fetched); });
			Pump();
		}

		void FetchOne(int64 X, int64 Y, int64 Z)
		{
			Voxels->FetchChunk(X, Y, Z, [this](const FCrowdyChunkFetch& Fetched) { Take(Fetched); });
			Pump();
		}

		void FetchBox(const FInt64Vector& Corner, const FInt64Vector& Opposite)
		{
			Voxels->FetchChunksInBox(Corner, Opposite, [this](const FCrowdyChunkFetch& Fetched) { Take(Fetched); });
			Pump();
		}

		/** Starts a box read, sets its cancel flag while the first request is out, then lets the answers arrive. */
		void FetchBoxCancelled(const FInt64Vector& Corner, const FInt64Vector& Opposite)
		{
			const TSharedRef<bool> Cancelled = MakeShared<bool>(false);
			Voxels->FetchChunksInBox(Corner, Opposite, [this](const FCrowdyChunkFetch& Fetched) { Take(Fetched); }, Cancelled);
			*Cancelled = true;
			Pump();
		}

		const FCrowdyVoxelChunk* Find(int64 X, int64 Y, int64 Z) const
		{
			return Answer.Chunks.FindByPredicate([X, Y, Z](const FCrowdyVoxelChunk& Chunk) { return Chunk.X == X && Chunk.Y == Y && Chunk.Z == Z; });
		}

		FString SentBody() const
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);
			return Body;
		}

	private:
		void Take(const FCrowdyChunkFetch& Fetched)
		{
			Answer = Fetched;
			++Answers;
		}

		void Pump()
		{
			for (int32 Poll = 0; Poll < 400 && Answers == 0; ++Poll)
			{
				Client->Poll();
			}
		}
	};

	TArray<uint8> Packed(const FString& Body)
	{
		const TSharedPtr<FCrowdyCppClient> Bridge = FCrowdyCppClient::MakeForTest(Body, 200);
		TArray<uint8> Page;
		Bridge->RunOpMsgpack(ECrowdyCppApiDomain::Chunks, TEXT("GetChunksByDistance"), MakeShared<FJsonObject>(), [&Page](FCrowdyCppJsonResult Result) { Page = MoveTemp(Result.Msgpack); });
		for (int32 Poll = 0; Poll < 100 && Page.IsEmpty(); ++Poll)
		{
			Bridge->Poll();
		}
		return Page;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsDecodesGridAndEditsTest, "CrowdySDK.Voxels.DecodesGridAndEdits", VoxelTestFlags)

bool FCrowdyVoxelsDecodesGridAndEditsTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Grid = EmptyGrid();
	Grid[Index(1, 2, 3)] = 7;
	Grid[Index(15, 12, 12)] = 2;
	Grid[Index(4, 4, 4)] = 3;
	Grid[Index(5, 5, 5)] = 3;
	const FString Edits = FString::Join(TArray<FString>{
		EditJson(15, 15, 15, 12),
		EditJson(15, 12, 12, 0),
		EditJson(16, 0, 0, 9),
		EditJson(0, -1, 0, 9),
		EditJson(4, 4, 4, 300),
		EditJson(5, 5, 5, -5)}, TEXT(","));

	FVoxelsRig Rig(PageJson({ChunkJson(37, -52, 1, GridJson(Grid), Edits, true)}));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Fetch(37, -52, 1, 2);

	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestEqual(TEXT("ok"), Rig.Answer.Result, ECrowdyVoxelResult::Ok);
	if (!TestEqual(TEXT("one chunk"), Rig.Answer.Chunks.Num(), 1))
	{
		return false;
	}
	const FCrowdyVoxelChunk& Chunk = Rig.Answer.Chunks[0];
	TestEqual(TEXT("x"), Chunk.X, 37ll);
	TestEqual(TEXT("y, negative, from a BigInt string"), Chunk.Y, -52ll);
	TestEqual(TEXT("z"), Chunk.Z, 1ll);
	TestTrue(TEXT("a fetched chunk is stored"), Chunk.bStored);
	TestEqual(TEXT("4,096 cells"), Chunk.Grid.Num(), Cells);
	TestEqual(TEXT("grid byte kept"), Chunk.Grid[Index(1, 2, 3)], static_cast<uint8>(7));
	TestEqual(TEXT("edit sets an empty cell"), Chunk.Grid[Index(15, 15, 15)], static_cast<uint8>(12));
	TestEqual(TEXT("edit to 0 removes a grid cube"), Chunk.Grid[Index(15, 12, 12)], static_cast<uint8>(0));
	TestEqual(TEXT("a type wider than a byte empties its grid cell"), Chunk.Grid[Index(4, 4, 4)], static_cast<uint8>(0));
	TestEqual(TEXT("a negative type empties its grid cell, not 251"), Chunk.Grid[Index(5, 5, 5)], static_cast<uint8>(0));
	TestEqual(TEXT("an edit outside the grid changes no cell"), Chunk.Grid[Index(0, 0, 0)], static_cast<uint8>(0));
	TestTrue(TEXT("truncation reported"), Chunk.bEditsTruncated);

	if (!TestEqual(TEXT("every edit with a position and a type is kept"), Chunk.Edits.Num(), 6))
	{
		return false;
	}
	TestEqual(TEXT("sorted z, then y, then x: the negative y first"), Chunk.Edits[0].Position, FIntVector(0, -1, 0));
	TestEqual(TEXT("then the one past the grid on x"), Chunk.Edits[1].Position, FIntVector(16, 0, 0));
	TestEqual(TEXT("its type"), Chunk.Edits[1].Type, 9);
	TestEqual(TEXT("the wide type kept whole"), Chunk.Edits[2].Type, 300);
	TestEqual(TEXT("the negative type kept whole"), Chunk.Edits[3].Type, -5);
	TestEqual(TEXT("the last edit is the highest position"), Chunk.Edits[5].Position, FIntVector(15, 15, 15));

	TestEqual(TEXT("filled: two grid cells and the four edits the grid cannot show"), UCrowdyVoxelLibrary::GetFilledVoxels(Chunk).Num(), 6);

	const FString Body = Rig.SentBody();
	TestTrue(TEXT("app id as a BigInt string"), Body.Contains(TEXT("\"appId\":\"31337\"")));
	TestTrue(TEXT("centre x as a BigInt string"), Body.Contains(TEXT("\"x\":\"37\"")));
	TestTrue(TEXT("centre y as a BigInt string"), Body.Contains(TEXT("\"y\":\"-52\"")));
	TestTrue(TEXT("range sent as the distance"), Body.Contains(TEXT("\"maxDistance\":2")));
	TestTrue(TEXT("page size sent"), Body.Contains(TEXT("\"limit\":32")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsStatesTest, "CrowdySDK.Voxels.StatesAreKept", VoxelTestFlags)

bool FCrowdyVoxelsStatesTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Grid = EmptyGrid();
	Grid[Index(1, 1, 1)] = 4;
	const FString Edits = FString::Join(TArray<FString>{
		EditJson(1, 1, 1, 4, TEXT("\"AAEC\"")),
		EditJson(2, 2, 2, 5, TEXT("\"\"")),
		EditJson(3, 3, 3, 6),
		EditJson(-4, 0, 0, 700, TEXT("\"/w==\""))}, TEXT(","));
	FVoxelsRig Rig(PageJson({ChunkJson(0, 0, 0, GridJson(Grid), Edits)}));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Fetch(0, 0, 0, 1);
	if (!TestEqual(TEXT("one chunk"), Rig.Answer.Chunks.Num(), 1))
	{
		return false;
	}
	const FCrowdyVoxelChunk& Chunk = Rig.Answer.Chunks[0];

	const FCrowdyVoxel WithState = UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(1, 1, 1));
	TestEqual(TEXT("type"), WithState.Type, 4);
	TestTrue(TEXT("state decoded from base64"), WithState.State == TArray<uint8>({0, 1, 2}));
	TestTrue(TEXT("an empty string is no state"), UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(2, 2, 2)).State.IsEmpty());
	TestTrue(TEXT("null is no state"), UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(3, 3, 3)).State.IsEmpty());
	TestTrue(TEXT("a wide voxel keeps its state"), UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(-4, 0, 0)).State == TArray<uint8>({0xff}));

	const TArray<FCrowdyVoxel> Filled = UCrowdyVoxelLibrary::GetFilledVoxels(Chunk);
	const FCrowdyVoxel* Cell = Filled.FindByPredicate([](const FCrowdyVoxel& Voxel) { return Voxel.Position == FIntVector(1, 1, 1); });
	TestTrue(TEXT("a grid cell carries its edit's state"), Cell && Cell->State == TArray<uint8>({0, 1, 2}));
	const FCrowdyVoxel* Wide = Filled.FindByPredicate([](const FCrowdyVoxel& Voxel) { return Voxel.Position == FIntVector(-4, 0, 0); });
	TestTrue(TEXT("a wide voxel is filled, with its type"), Wide && Wide->Type == 700);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsNewestWinsTest, "CrowdySDK.Voxels.NewestEditPerPositionWins", VoxelTestFlags)

bool FCrowdyVoxelsNewestWinsTest::RunTest(const FString& Parameters)
{
	const FString Edits = FString::Join(TArray<FString>{
		EditJson(9, 9, 9, 1),
		EditJson(2, 2, 2, 5, TEXT("\"AQ==\"")),
		EditJson(-1, 0, 0, 400),
		EditJson(2, 2, 2, 6, TEXT("\"Ag==\"")),
		EditJson(-1, 0, 0, 0)}, TEXT(","));
	FVoxelsRig Rig(PageJson({ChunkJson(0, 0, 0, TEXT("null"), Edits)}));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Fetch(0, 0, 0, 1);
	if (!TestEqual(TEXT("one chunk"), Rig.Answer.Chunks.Num(), 1))
	{
		return false;
	}
	const FCrowdyVoxelChunk& Chunk = Rig.Answer.Chunks[0];
	if (!TestEqual(TEXT("one edit per position"), Chunk.Edits.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("sorted: the negative x first"), Chunk.Edits[0].Position, FIntVector(-1, 0, 0));
	TestEqual(TEXT("its later removal wins"), Chunk.Edits[0].Type, 0);
	TestEqual(TEXT("then (2, 2, 2)"), Chunk.Edits[1].Position, FIntVector(2, 2, 2));
	TestEqual(TEXT("its later type wins"), Chunk.Edits[1].Type, 6);
	TestTrue(TEXT("with its later state"), Chunk.Edits[1].State == TArray<uint8>({2}));
	TestEqual(TEXT("the grid shows the later type"), Chunk.Grid[Index(2, 2, 2)], static_cast<uint8>(6));
	TestEqual(TEXT("the removed wide voxel is not filled"), UCrowdyVoxelLibrary::GetFilledVoxels(Chunk).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsNullGridTest, "CrowdySDK.Voxels.NullGridTakesEdits", VoxelTestFlags)

bool FCrowdyVoxelsNullGridTest::RunTest(const FString& Parameters)
{
	FVoxelsRig Rig(PageJson({ChunkJson(0, 0, 0, TEXT("null"), EditJson(3, 4, 5, 6))}));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Fetch(0, 0, 0, 1);

	if (!TestEqual(TEXT("one chunk"), Rig.Answer.Chunks.Num(), 1))
	{
		return false;
	}
	const FCrowdyVoxelChunk& Chunk = Rig.Answer.Chunks[0];
	TestEqual(TEXT("a chunk with no grid is 4,096 cells"), Chunk.Grid.Num(), Cells);
	TestEqual(TEXT("its edit applied"), Chunk.Grid[Index(3, 4, 5)], static_cast<uint8>(6));
	TestEqual(TEXT("the rest empty"), Chunk.Grid[Index(0, 0, 0)], static_cast<uint8>(0));
	TestFalse(TEXT("not truncated"), Chunk.bEditsTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsEditsFirstTest, "CrowdySDK.Voxels.EditsBeforeTheGridStillWin", VoxelTestFlags)

bool FCrowdyVoxelsEditsFirstTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Grid = EmptyGrid();
	Grid[Index(2, 2, 2)] = 4;
	const FString Chunk = FString::Printf(TEXT("{\"voxelStates\":[%s,%s],\"coordinates\":{\"z\":\"0\",\"y\":\"0\",\"x\":\"5\"},\"voxels\":%s}"),
		*EditJson(2, 2, 2, 0), *EditJson(3, 3, 3, 9), *GridJson(Grid));
	FVoxelsRig Rig(PageJson({Chunk}));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Fetch(5, 0, 0, 1);

	if (!TestEqual(TEXT("one chunk"), Rig.Answer.Chunks.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("coordinates in any order"), Rig.Answer.Chunks[0].X, 5ll);
	TestEqual(TEXT("an edit listed before the grid still removes its cube"), Rig.Answer.Chunks[0].Grid[Index(2, 2, 2)], static_cast<uint8>(0));
	TestEqual(TEXT("and still sets its cell"), Rig.Answer.Chunks[0].Grid[Index(3, 3, 3)], static_cast<uint8>(9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsHostileBytesTest, "CrowdySDK.Voxels.HostileBytesNeverOverread", VoxelTestFlags)

bool FCrowdyVoxelsHostileBytesTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Grid = EmptyGrid();
	Grid[Index(1, 2, 3)] = 7;
	const TArray<uint8> Page = Packed(PageJson({ChunkJson(1, 2, 3, GridJson(Grid), EditJson(4, 5, 6, 7) + TEXT(",") + EditJson(1, 1, 1, 2, TEXT("\"AAAA\"")))}));

	TArray<FCrowdyVoxelChunk> Out;
	TSet<FInt64Vector> Seen;
	int32 Limit = 0;
	if (!TestEqual(TEXT("the whole page decodes"), CrowdyVoxelDecode::DecodePage(Page, 32, Out, Seen, Limit), 1))
	{
		return false;
	}
	TestEqual(TEXT("as the chunk that was sent"), Out[0].Grid[Index(1, 2, 3)] + Out[0].Grid[Index(4, 5, 6)] + Out[0].Grid[Index(1, 1, 1)], 7 + 7 + 2);
	TestEqual(TEXT("with its state"), UCrowdyVoxelLibrary::GetVoxel(Out[0], FIntVector(1, 1, 1)).State.Num(), 3);

	// Each cut gets its own buffer of exactly its length, so a read past the end leaves the allocation.
	int32 Accepted = 0;
	for (int32 Length = 0; Length < Page.Num(); ++Length)
	{
		const TArray<uint8> Cut(Page.GetData(), Length);
		Out.Reset();
		Seen.Reset();
		Accepted += CrowdyVoxelDecode::DecodePage(Cut, 32, Out, Seen, Limit) != INDEX_NONE;
	}
	TestEqual(TEXT("no cut of the page is taken as whole"), Accepted, 0);

	// Corrupted pages only have to come back without a crash: a flipped byte may still be a valid page.
	FRandomStream Random(77);
	for (int32 Trial = 0; Trial < 2000; ++Trial)
	{
		TArray<uint8> Bent = Page;
		for (int32 Flip = 0; Flip < 4; ++Flip)
		{
			Bent[Random.RandHelper(Bent.Num())] = static_cast<uint8>(Random.RandHelper(256));
		}
		Out.Reset();
		Seen.Reset();
		CrowdyVoxelDecode::DecodePage(Bent, 32, Out, Seen, Limit);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsEveryFormatTest, "CrowdySDK.Voxels.SkipsEveryMessagePackFormat", VoxelTestFlags)

bool FCrowdyVoxelsEveryFormatTest::RunTest(const FString& Parameters)
{
	// Unknown members of every format family, before and inside the one chunk, must be stepped over exactly.
	auto Build = [](int32 Nesting)
	{
		FPack P;
		P.Map(1).Str("getChunksByDistance").Map(3).Str("limit").Raw({0x20});
		P.Str("extra").Array(15)
			.Raw({0xca, 0, 0, 0, 0}).Raw({0xcb, 0, 0, 0, 0, 0, 0, 0, 0})
			.Raw({0xc4, 2, 9, 9}).Raw({0xc5, 0, 1, 9}).Raw({0xc6, 0, 0, 0, 1, 9})
			.Raw({0xd4, 1, 9}).Raw({0xd8, 1}).Raw({9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9})
			.Raw({0xc7, 1, 1, 9}).Raw({0xda, 0, 2, 'h', 'i'})
			.Raw({0xde, 0, 1}).Str("k").Raw({0xc0})
			.Raw({0xdc, 0, 1, 0xc3}).Raw({0xcf, 0, 0, 0, 0, 0, 0, 0, 1}).Raw({0xd3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})
			.Raw({0xc2});
		for (int32 Level = 0; Level < Nesting; ++Level)
		{
			P.Array(1);
		}
		P.Raw({0xc0});
		P.Str("chunks").Array(1).Map(3);
		P.Str("unknown").Raw({0xdd, 0, 0, 0, 0});
		P.Str("coordinates").Map(3).Str("x").Raw({0xfd}).Str("y").Raw({0xcd, 0x01, 0x2c}).Str("z").Raw({0xd2, 0xff, 0xfe, 0xee, 0x90});
		P.Str("voxels").Raw({0xc0});
		return P.Bytes;
	};

	TArray<FCrowdyVoxelChunk> Out;
	TSet<FInt64Vector> Seen;
	int32 Limit = 0;
	if (!TestEqual(TEXT("a page with every format decodes"), CrowdyVoxelDecode::DecodePage(Build(10), 32, Out, Seen, Limit), 1))
	{
		return false;
	}
	TestEqual(TEXT("negative fixint x"), Out[0].X, -3ll);
	TestEqual(TEXT("uint16 y"), Out[0].Y, 300ll);
	TestEqual(TEXT("int32 z"), Out[0].Z, -70000ll);
	TestEqual(TEXT("the echoed page size"), Limit, 32);

	Out.Reset();
	Seen.Reset();
	TestEqual(TEXT("an unknown member nested past 64 is refused"), CrowdyVoxelDecode::DecodePage(Build(70), 32, Out, Seen, Limit), static_cast<int32>(INDEX_NONE));

	// A second chunk list in one page would get past the per-page bound, so the page is refused before decoding it.
	FPack Twice;
	Twice.Map(1).Str("getChunksByDistance").Map(2);
	for (const ANSICHAR* X : {"1", "2"})
	{
		Twice.Str("chunks").Array(1).Map(1).Str("coordinates").Map(3).Str("x").Str(X).Str("y").Raw({0}).Str("z").Raw({0});
	}
	Out.Reset();
	Seen.Reset();
	TestEqual(TEXT("a page with two chunk lists is refused"), CrowdyVoxelDecode::DecodePage(Twice.Bytes, 32, Out, Seen, Limit), static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsInt64Test, "CrowdySDK.Voxels.CoordinatesSpanInt64", VoxelTestFlags)

bool FCrowdyVoxelsInt64Test::RunTest(const FString& Parameters)
{
	FVoxelsRig Rig(PageJson({TEXT("{\"coordinates\":{\"x\":\"-9223372036854775808\",\"y\":\"9223372036854775807\",\"z\":-3},\"voxels\":null,\"voxelStates\":[]}")}));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Fetch(0, 0, 0, 1);

	if (!TestEqual(TEXT("one chunk"), Rig.Answer.Chunks.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("the lowest int64"), Rig.Answer.Chunks[0].X, MIN_int64);
	TestEqual(TEXT("the highest int64"), Rig.Answer.Chunks[0].Y, MAX_int64);
	TestEqual(TEXT("a plain number"), Rig.Answer.Chunks[0].Z, -3ll);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsPagesTest, "CrowdySDK.Voxels.PagesUntilShort", VoxelTestFlags)

bool FCrowdyVoxelsPagesTest::RunTest(const FString& Parameters)
{
	FVoxelsRig Rig(PageJson({}));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Client->SetTestResponseScript({TPair<int32, FString>(200, PageJson(EmptyChunks(32, 0))), TPair<int32, FString>(200, PageJson(EmptyChunks(3, 100)))});
	Rig.Fetch(0, 0, 0, 2);

	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestEqual(TEXT("ok"), Rig.Answer.Result, ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("both pages"), Rig.Answer.Chunks.Num(), 35);
	TestTrue(TEXT("the second page skips the first"), Rig.SentBody().Contains(TEXT("\"skip\":32")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsRepeatTest, "CrowdySDK.Voxels.AChunkOnTwoPagesIsKeptOnce", VoxelTestFlags)

bool FCrowdyVoxelsRepeatTest::RunTest(const FString& Parameters)
{
	// Pages are stable, so a chunk that comes back on a second page is the same chunk and is kept once.
	TArray<FString> Second = EmptyChunks(3, 100);
	Second.Add(EmptyChunks(1, 31)[0]);

	FVoxelsRig Rig(PageJson(EmptyChunks(1, 900)));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Client->SetTestResponseScript({TPair<int32, FString>(200, PageJson(EmptyChunks(32, 0))), TPair<int32, FString>(200, PageJson(Second))});
	Rig.Fetch(0, 0, 0, 2);
	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestEqual(TEXT("ok"), Rig.Answer.Result, ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("the repeat kept once, with no read again"), Rig.Answer.Chunks.Num(), 35);
	TestFalse(TEXT("nothing past the short page was asked for"), Rig.Answer.Chunks.ContainsByPredicate(
		[](const FCrowdyVoxelChunk& Chunk) { return Chunk.X == 900; }));

	// A platform that repeats the same full page for ever is cut off once the pages span the region.
	FVoxelsRig Endless(PageJson(EmptyChunks(32, 0)));
	Endless.Fetch(0, 0, 0, 2);
	TestEqual(TEXT("an endless repeat still answers once"), Endless.Answers, 1);
	TestEqual(TEXT("with the chunks it did send"), Endless.Answer.Chunks.Num(), 32);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsEchoedLimitTest, "CrowdySDK.Voxels.FollowsTheEchoedPageSize", VoxelTestFlags)

bool FCrowdyVoxelsEchoedLimitTest::RunTest(const FString& Parameters)
{
	FVoxelsRig Rig(PageJson({}));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Client->SetTestResponseScript({TPair<int32, FString>(200, PageJson(EmptyChunks(10, 0), 10)), TPair<int32, FString>(200, PageJson(EmptyChunks(3, 100), 10))});
	Rig.Fetch(0, 0, 0, 2);

	TestEqual(TEXT("ok"), Rig.Answer.Result, ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("a page of 10 under a limit of 10 is full, so the next is fetched"), Rig.Answer.Chunks.Num(), 13);
	TestTrue(TEXT("and it skips 10"), Rig.SentBody().Contains(TEXT("\"skip\":10")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsOversizedPageTest, "CrowdySDK.Voxels.MoreChunksThanAskedIsABadReply", VoxelTestFlags)

bool FCrowdyVoxelsOversizedPageTest::RunTest(const FString& Parameters)
{
	FVoxelsRig Rig(PageJson(EmptyChunks(33, 0)));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("The chunk fetch ended BadReply"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Fetch(0, 0, 0, 8);
	TestEqual(TEXT("33 chunks to a request for 32"), Rig.Answer.Result, ECrowdyVoxelResult::BadReply);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsDeepAnswerTest, "CrowdySDK.Voxels.AnOverDeepAnswerIsRefused", VoxelTestFlags)

bool FCrowdyVoxelsDeepAnswerTest::RunTest(const FString& Parameters)
{
	const FString Deep = FString::Printf(TEXT("{\"data\":{\"getChunksByDistance\":{\"lods\":%s1%s,\"chunks\":[]}}}"), *FString::ChrN(5000, TEXT('[')), *FString::ChrN(5000, TEXT(']')));
	FVoxelsRig Rig(Deep);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	// The bridge packs any depth, so the decoder's own nesting bound is what refuses it.
	AddExpectedMessagePlain(TEXT("The chunk fetch ended BadReply"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Fetch(0, 0, 0, 2);
	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestEqual(TEXT("refused by the decoder"), Rig.Answer.Result, ECrowdyVoxelResult::BadReply);
	TestEqual(TEXT("with nothing half-decoded returned"), Rig.Answer.Chunks.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsPageBoundTest, "CrowdySDK.Voxels.EndlessPagesAreABadReply", VoxelTestFlags)

bool FCrowdyVoxelsPageBoundTest::RunTest(const FString& Parameters)
{
	FVoxelsRig Rig(PageJson(EmptyChunks(32, 0)));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("The chunk fetch ended BadReply"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Fetch(0, 0, 0, 1);

	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestEqual(TEXT("a range of 27 chunks answered with 32 is a bad reply"), Rig.Answer.Result, ECrowdyVoxelResult::BadReply);
	TestEqual(TEXT("nothing returned"), Rig.Answer.Chunks.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsMalformedTest, "CrowdySDK.Voxels.MalformedChunkFailsTheFetch", VoxelTestFlags)

bool FCrowdyVoxelsMalformedTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Short;
	Short.SetNumZeroed(Cells - 1);
	TArray<uint8> Long;
	Long.SetNumZeroed(Cells + 1);
	TArray<uint8> BigState;
	BigState.SetNumZeroed(1025);
	const FString NoGrid = TEXT("null");
	const TArray<TPair<FString, FString>> Cases = {
		{TEXT("a grid one byte short"), ChunkJson(0, 0, 0, GridJson(Short))},
		{TEXT("a grid one byte long"), ChunkJson(0, 0, 0, GridJson(Long))},
		{TEXT("a grid that is not base64"), ChunkJson(0, 0, 0, FString::Printf(TEXT("\"%s\""), *(FString::ChrN(5462, TEXT('!')) + TEXT("=="))))},
		{TEXT("a coordinate that is not an integer"), TEXT("{\"coordinates\":{\"x\":\"1.5\",\"y\":\"0\",\"z\":\"0\"},\"voxels\":null,\"voxelStates\":[]}")},
		{TEXT("no coordinates"), TEXT("{\"voxels\":null,\"voxelStates\":[]}")},
		{TEXT("a coordinate past int64"), TEXT("{\"coordinates\":{\"x\":\"9223372036854775808\",\"y\":\"0\",\"z\":\"0\"},\"voxels\":null,\"voxelStates\":[]}")},
		{TEXT("an edit position that is not an integer"), TEXT("{\"coordinates\":{\"x\":\"0\",\"y\":\"0\",\"z\":\"0\"},\"voxels\":null,\"voxelStates\":[{\"voxelCoord\":{\"x\":\"abc\",\"y\":0,\"z\":0},\"voxelType\":1}]}")},
		{TEXT("an edit position past 16 bits"), ChunkJson(0, 0, 0, NoGrid, EditJson(32768, 0, 0, 1))},
		{TEXT("an edit position missing an axis"), ChunkJson(0, 0, 0, NoGrid, TEXT("{\"voxelCoord\":{\"x\":1,\"y\":2},\"voxelType\":7}"))},
		{TEXT("an edit with no type"), ChunkJson(0, 0, 0, NoGrid, TEXT("{\"voxelCoord\":{\"x\":1,\"y\":2,\"z\":3}}"))},
		{TEXT("an edit with no position"), ChunkJson(0, 0, 0, NoGrid, TEXT("{\"voxelType\":5}"))},
		{TEXT("an edit type past 16 bits"), ChunkJson(0, 0, 0, NoGrid, EditJson(0, 0, 0, -32769))},
		{TEXT("a state that is not base64"), ChunkJson(0, 0, 0, NoGrid, EditJson(0, 0, 0, 1, TEXT("\"!!!!\"")))},
		{TEXT("a state over 1,024 bytes"), ChunkJson(0, 0, 0, NoGrid, EditJson(0, 0, 0, 1, GridJson(BigState)))},
	};

	AddExpectedMessagePlain(TEXT("The chunk fetch ended BadReply"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, Cases.Num());
	for (const TPair<FString, FString>& Case : Cases)
	{
		FVoxelsRig Rig(PageJson({EmptyChunks(1, 9)[0], Case.Value}));
		if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
		{
			return false;
		}
		Rig.Fetch(0, 0, 0, 1);
		TestEqual(Case.Key, Rig.Answer.Result, ECrowdyVoxelResult::BadReply);
		TestEqual(Case.Key + TEXT(": nothing half-decoded is returned"), Rig.Answer.Chunks.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsWidestTest, "CrowdySDK.Voxels.SixteenBitBoundsAreKept", VoxelTestFlags)

bool FCrowdyVoxelsWidestTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Full;
	Full.SetNumZeroed(1024);
	const FString Edits = EditJson(-32768, 32767, 0, -32768) + TEXT(",") + EditJson(0, 0, 1, 32767, GridJson(Full));
	FVoxelsRig Rig(PageJson({ChunkJson(0, 0, 0, TEXT("null"), Edits)}));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Fetch(0, 0, 0, 1);
	if (!TestEqual(TEXT("ok"), Rig.Answer.Result, ECrowdyVoxelResult::Ok) || !TestEqual(TEXT("one chunk"), Rig.Answer.Chunks.Num(), 1))
	{
		return false;
	}
	const FCrowdyVoxelChunk& Chunk = Rig.Answer.Chunks[0];
	TestEqual(TEXT("the lowest 16-bit type at the widest position"), UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(-32768, 32767, 0)).Type, -32768);
	TestEqual(TEXT("a 1,024-byte state is kept whole"), UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(0, 0, 1)).State.Num(), 1024);
	TestEqual(TEXT("the highest 16-bit type"), UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(0, 0, 1)).Type, 32767);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsRefusedTest, "CrowdySDK.Voxels.RefusalCarriesTheCode", VoxelTestFlags)

bool FCrowdyVoxelsRefusedTest::RunTest(const FString& Parameters)
{
	const FString Forbidden = TEXT("{\"errors\":[{\"message\":\"Not your app.\",\"extensions\":{\"code\":\"FORBIDDEN\"}}],\"data\":null}");
	FVoxelsRig Rig(Forbidden, 403);
	FVoxelsRig One(Forbidden, 403);
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid() && One.Client.IsValid()))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("The chunk fetch ended Refused"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	Rig.Fetch(0, 0, 0, 2);
	One.FetchOne(0, 0, 0);

	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestEqual(TEXT("refused"), Rig.Answer.Result, ECrowdyVoxelResult::Refused);
	TestTrue(TEXT("the code is in the message"), Rig.Answer.Message.Contains(TEXT("FORBIDDEN")));
	TestEqual(TEXT("Fetch Chunk is refused the same way"), One.Answer.Result, ECrowdyVoxelResult::Refused);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsRangeTest, "CrowdySDK.Voxels.RangeIsClamped", VoxelTestFlags)

bool FCrowdyVoxelsRangeTest::RunTest(const FString& Parameters)
{
	const TArray<TPair<int32, FString>> Cases = {{0, TEXT("\"maxDistance\":1")}, {-5, TEXT("\"maxDistance\":1")}, {20, TEXT("\"maxDistance\":8")}};
	for (const TPair<int32, FString>& Case : Cases)
	{
		FVoxelsRig Rig(PageJson({}));
		if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
		{
			return false;
		}
		Rig.Fetch(0, 0, 0, Case.Key);
		TestEqual(FString::Printf(TEXT("range %d fetches"), Case.Key), Rig.Answer.Result, ECrowdyVoxelResult::Ok);
		TestTrue(FString::Printf(TEXT("range %d is sent as %s"), Case.Key, *Case.Value), Rig.SentBody().Contains(Case.Value));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsNotSignedInTest, "CrowdySDK.Voxels.NoTokenIsNotSignedIn", VoxelTestFlags)

bool FCrowdyVoxelsNotSignedInTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UCrowdyVoxels> Voxels(NewObject<UCrowdyVoxels>(Instance.Get()));
	TArray<ECrowdyVoxelResult> Answers;
	AddExpectedMessagePlain(TEXT("The chunk fetch ended NotSignedIn"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	Voxels->FetchChunksAround(0, 0, 0, 2, [&Answers](const FCrowdyChunkFetch& Fetch) { Answers.Add(Fetch.Result); });
	Voxels->FetchChunk(0, 0, 0, [&Answers](const FCrowdyChunkFetch& Fetch) { Answers.Add(Fetch.Result); });

	if (!TestEqual(TEXT("both answered at once"), Answers.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("not signed in"), Answers[0], ECrowdyVoxelResult::NotSignedIn);
	TestEqual(TEXT("Fetch Chunk, not signed in"), Answers[1], ECrowdyVoxelResult::NotSignedIn);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsFetchChunkTest, "CrowdySDK.Voxels.FetchChunkAnswersOneChunk", VoxelTestFlags)

bool FCrowdyVoxelsFetchChunkTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Grid = EmptyGrid();
	Grid[Index(1, 2, 3)] = 9;
	FVoxelsRig Stored(OneJson(ChunkJson(37, -52, 1, GridJson(Grid), EditJson(20, 0, 0, 3, TEXT("\"AQ==\"")))));
	FVoxelsRig Absent(OneJson(TEXT("null")));
	FVoxelsRig Other(OneJson(ChunkJson(37, -52, 2, GridJson(Grid))));
	if (!TestTrue(TEXT("clients built"), Stored.Client.IsValid() && Absent.Client.IsValid() && Other.Client.IsValid()))
	{
		return false;
	}

	Stored.FetchOne(37, -52, 1);
	TestEqual(TEXT("ok"), Stored.Answer.Result, ECrowdyVoxelResult::Ok);
	if (!TestEqual(TEXT("exactly one chunk"), Stored.Answer.Chunks.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("stored"), Stored.Answer.Chunks[0].bStored);
	TestEqual(TEXT("its grid"), Stored.Answer.Chunks[0].Grid[Index(1, 2, 3)], static_cast<uint8>(9));
	TestTrue(TEXT("its wide edit and state"), UCrowdyVoxelLibrary::GetVoxel(Stored.Answer.Chunks[0], FIntVector(20, 0, 0)).State == TArray<uint8>({1}));
	const FString Body = Stored.SentBody();
	TestTrue(TEXT("app id as a BigInt string"), Body.Contains(TEXT("\"appId\":\"31337\"")));
	TestTrue(TEXT("the chunk under coordinates"), Body.Contains(TEXT("\"coordinates\"")) && Body.Contains(TEXT("\"y\":\"-52\"")));

	Absent.FetchOne(-4, 5, -6);
	TestEqual(TEXT("a chunk the platform does not store is ok"), Absent.Answer.Result, ECrowdyVoxelResult::Ok);
	if (!TestEqual(TEXT("as one chunk"), Absent.Answer.Chunks.Num(), 1))
	{
		return false;
	}
	const FCrowdyVoxelChunk& Empty = Absent.Answer.Chunks[0];
	TestFalse(TEXT("not stored"), Empty.bStored);
	TestTrue(TEXT("at the coordinates asked for"), FInt64Vector(Empty.X, Empty.Y, Empty.Z) == FInt64Vector(-4, 5, -6));
	TestEqual(TEXT("with an empty grid"), Empty.Grid.Num(), Cells);
	TestEqual(TEXT("and nothing filled"), UCrowdyVoxelLibrary::GetFilledVoxels(Empty).Num(), 0);

	AddExpectedMessagePlain(TEXT("The chunk fetch ended BadReply"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Other.FetchOne(37, -52, 1);
	TestEqual(TEXT("a different chunk than the one asked for is a bad reply"), Other.Answer.Result, ECrowdyVoxelResult::BadReply);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsLeanDocumentTest, "CrowdySDK.Voxels.ReadsSelectOnlyWhatTheDecodeReads", VoxelTestFlags)

bool FCrowdyVoxelsLeanDocumentTest::RunTest(const FString& Parameters)
{
	FVoxelsRig Around(PageJson({}));
	FVoxelsRig Box(PageJson({ChunkJson(4, 0, 0, GridJson(EmptyGrid()))}));
	if (!TestTrue(TEXT("clients built"), Around.Client.IsValid() && Box.Client.IsValid()))
	{
		return false;
	}
	Around.Fetch(0, 0, 0, 1);
	const FString AroundBody = Around.SentBody();
	TestTrue(TEXT("Fetch Chunks Around sends the lean document"), AroundBody.Contains(TEXT("GetChunksByDistanceVoxels")));
	TestFalse(TEXT("which selects no chunk state"), AroundBody.Contains(TEXT("chunkState")));
	TestFalse(TEXT("no LODs"), AroundBody.Contains(TEXT("lods")));
	TestFalse(TEXT("and no upload time"), AroundBody.Contains(TEXT("cdnUploadedAt")));

	Box.FetchBox(FInt64Vector(4, 0, 0), FInt64Vector(4, 0, 0));
	TestEqual(TEXT("a box read is ok"), Box.Answer.Result, ECrowdyVoxelResult::Ok);
	const FString BoxBody = Box.SentBody();
	TestTrue(TEXT("a box read sends the lean document too"), BoxBody.Contains(TEXT("GetChunksByDistanceVoxels")) && !BoxBody.Contains(TEXT("chunkState")));
	TestTrue(TEXT("with both corners"), BoxBody.Contains(TEXT("\"lowChunk\"")) && BoxBody.Contains(TEXT("\"highChunk\"")));
	TestFalse(TEXT("and no cube"), BoxBody.Contains(TEXT("maxDistance")));
	TestTrue(TEXT("corners as BigInt strings"), BoxBody.Contains(TEXT("\"x\":\"4\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsBoxPagesTest, "CrowdySDK.Voxels.BoxPagesByTheEchoedLimit", VoxelTestFlags)

bool FCrowdyVoxelsBoxPagesTest::RunTest(const FString& Parameters)
{
	// Anything asked for past the script gets an answer no read accepts, so an extra request shows as a failure.
	FVoxelsRig Rig(TEXT("{\"data\":null}"));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	const FString Grid = GridJson(EmptyGrid());
	Rig.Client->SetTestResponseScript({
		TPair<int32, FString>(200, PageJson({ChunkJson(0, 0, 0, Grid), ChunkJson(1, 0, 0, Grid)}, 2)),
		TPair<int32, FString>(200, PageJson({ChunkJson(1, 0, 0, Grid), ChunkJson(2, 0, 0, Grid)}, 2)),
		TPair<int32, FString>(200, EditsPageJson({}))});
	Rig.FetchBox(FInt64Vector(0, 0, 0), FInt64Vector(3, 0, 0));

	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestEqual(TEXT("ok"), Rig.Answer.Result, ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("a chunk on two pages is kept once"), Rig.Answer.Chunks.Num(), 3);
	TestTrue(TEXT("each stored chunk once"), Rig.Find(0, 0, 0) && Rig.Find(1, 0, 0) && Rig.Find(2, 0, 0));
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("two full pages of 2 span the box of 4, so the edits are read next"), Body.Contains(TEXT("ListVoxelUpdatesByDistance")));
	TestTrue(TEXT("for the one chunk the read did not return"), Body.Contains(TEXT("\"x\":\"3\"")) && !Body.Contains(TEXT("\"x\":\"0\"")));
	TestTrue(TEXT("through the lean document, which selects only what rebuilds a cell"),
		Body.Contains(TEXT("ListVoxelUpdatesByDistanceCells")) && !Body.Contains(TEXT("voxelUpdateId")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsNeverStoredEditsTest, "CrowdySDK.Voxels.BoxReadsEditsOfChunksNeverStored", VoxelTestFlags)

bool FCrowdyVoxelsNeverStoredEditsTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Grid = EmptyGrid();
	Grid[Index(1, 1, 1)] = 4;
	FVoxelsRig Rig(TEXT("{\"data\":null}"));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	// The edit log lists each chunk's rows newest first.
	Rig.Client->SetTestResponseScript({
		TPair<int32, FString>(200, PageJson({ChunkJson(0, 0, 0, GridJson(Grid))})),
		TPair<int32, FString>(200, EditsPageJson({
			GroupJson(0, 0, 0, {RowJson(1, 1, 1, 9)}),
			GroupJson(1, 0, 0, {RowJson(1, 1, 1, 5, TEXT("\"Ag==\"")), RowJson(2, 2, 2, 9), RowJson(1, 1, 1, 3, TEXT("\"AQ==\"")), RowJson(20, 0, 0, 300)}),
			GroupJson(2, 0, 0, {})}))});
	Rig.FetchBox(FInt64Vector(0, 0, 0), FInt64Vector(2, 0, 0));

	TestEqual(TEXT("ok"), Rig.Answer.Result, ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("the stored chunk and the one with edits"), Rig.Answer.Chunks.Num(), 2);
	const FCrowdyVoxelChunk* Stored = Rig.Find(0, 0, 0);
	const FCrowdyVoxelChunk* Edited = Rig.Find(1, 0, 0);
	if (!TestTrue(TEXT("both found"), Stored && Edited))
	{
		return false;
	}
	TestTrue(TEXT("the stored chunk is stored"), Stored->bStored);
	TestEqual(TEXT("and keeps its own read, not the edit log's second copy"), Stored->Grid[Index(1, 1, 1)], static_cast<uint8>(4));
	TestFalse(TEXT("a chunk never stored says so"), Edited->bStored);
	TestEqual(TEXT("its newest edit wins over an older one at the same cell"), Edited->Grid[Index(1, 1, 1)], static_cast<uint8>(5));
	TestTrue(TEXT("with the newest edit's state"), UCrowdyVoxelLibrary::GetVoxel(*Edited, FIntVector(1, 1, 1)).State == TArray<uint8>({2}));
	TestEqual(TEXT("its other edit applies over an empty grid"), Edited->Grid[Index(2, 2, 2)], static_cast<uint8>(9));
	TestEqual(TEXT("a wide edit is kept"), UCrowdyVoxelLibrary::GetVoxel(*Edited, FIntVector(20, 0, 0)).Type, 300);
	TestEqual(TEXT("one edit per position"), Edited->Edits.Num(), 3);
	TestFalse(TEXT("not truncated"), Edited->bEditsTruncated);
	TestFalse(TEXT("a group with no edits is left out"), Rig.Find(2, 0, 0) != nullptr);
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("the edits are read only over the chunks the read did not return"), Body.Contains(TEXT("ListVoxelUpdatesByDistance"))
		&& Body.Contains(TEXT("\"x\":\"1\"")) && Body.Contains(TEXT("\"x\":\"2\"")) && !Body.Contains(TEXT("\"x\":\"0\"")));

	FVoxelsRig Whole(TEXT("{\"data\":null}"));
	Whole.Client->SetTestResponseScript({TPair<int32, FString>(200, PageJson({ChunkJson(0, 0, 0, GridJson(Grid))}))});
	Whole.FetchBox(FInt64Vector(0, 0, 0), FInt64Vector(0, 0, 0));
	TestEqual(TEXT("a box the read returned whole is ok"), Whole.Answer.Result, ECrowdyVoxelResult::Ok);
	TestTrue(TEXT("without reading its edits again"), Whole.SentBody().Contains(TEXT("GetChunksByDistanceVoxels")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsAbsentBoxesTest, "CrowdySDK.Voxels.EditsAreReadOnlyOverAbsentChunks", VoxelTestFlags)

bool FCrowdyVoxelsAbsentBoxesTest::RunTest(const FString& Parameters)
{
	// Two absent chunks at opposite ends of a box, the stored one between them: two small edit reads, side by side.
	FVoxelsRig Rig(TEXT("{\"data\":null}"));
	if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Client->SetTestResponseScript({
		TPair<int32, FString>(200, PageJson({ChunkJson(1, 0, 0, GridJson(EmptyGrid()))})),
		TPair<int32, FString>(200, EditsPageJson({GroupJson(0, 0, 0, {RowJson(1, 1, 1, 3)})})),
		TPair<int32, FString>(200, EditsPageJson({GroupJson(2, 0, 0, {RowJson(2, 2, 2, 4)})}))});
	Rig.FetchBox(FInt64Vector(0, 0, 0), FInt64Vector(2, 0, 0));

	TestEqual(TEXT("ok"), Rig.Answer.Result, ECrowdyVoxelResult::Ok);
	TestEqual(TEXT("the stored chunk and both edited ones"), Rig.Answer.Chunks.Num(), 3);
	const FCrowdyVoxelChunk* First = Rig.Find(0, 0, 0);
	const FCrowdyVoxelChunk* Last = Rig.Find(2, 0, 0);
	TestTrue(TEXT("each edited chunk from its own read"), First && Last && First->Grid[Index(1, 1, 1)] == 3 && Last->Grid[Index(2, 2, 2)] == 4);
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("the last edit read covers only the far chunk"), Body.Contains(TEXT("ListVoxelUpdatesByDistance"))
		&& Body.Contains(TEXT("\"x\":\"2\"")) && !Body.Contains(TEXT("\"x\":\"1\"")) && !Body.Contains(TEXT("\"x\":\"0\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsCancelTest, "CrowdySDK.Voxels.ACancelledBoxReadAsksForNothingMore", VoxelTestFlags)

bool FCrowdyVoxelsCancelTest::RunTest(const FString& Parameters)
{
	// A full first page of a box of 64 would ask for a second; cancelled while the first is out, it asks for nothing.
	FVoxelsRig Paging(PageJson(EmptyChunks(32, 0)));
	if (!TestTrue(TEXT("client built"), Paging.Client.IsValid()))
	{
		return false;
	}
	Paging.FetchBoxCancelled(FInt64Vector(0, 0, 0), FInt64Vector(63, 0, 0));
	TestEqual(TEXT("a cancelled read never answers"), Paging.Answers, 0);
	TestFalse(TEXT("and asks for no second page"), Paging.SentBody().Contains(TEXT("\"skip\":32")));

	// Nor does it go on to read the edits of the chunks its page left out.
	FVoxelsRig Edits(PageJson({ChunkJson(0, 0, 0, GridJson(EmptyGrid()))}));
	Edits.FetchBoxCancelled(FInt64Vector(0, 0, 0), FInt64Vector(1, 0, 0));
	TestEqual(TEXT("no answer"), Edits.Answers, 0);
	TestFalse(TEXT("and no edit read"), Edits.SentBody().Contains(TEXT("ListVoxelUpdatesByDistance")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsEditsBoundsTest, "CrowdySDK.Voxels.ForgedEditsAreABadReply", VoxelTestFlags)

bool FCrowdyVoxelsEditsBoundsTest::RunTest(const FString& Parameters)
{
	TArray<FString> TooMany;
	for (int32 Group = 0; Group < 33; ++Group)
	{
		TooMany.Add(GroupJson(Group % 2, 0, 0, {RowJson(0, 0, 0, 1)}));
	}
	const TArray<TPair<FString, FString>> Cases = {
		{TEXT("edits for a chunk outside the box"), EditsPageJson({GroupJson(5, 0, 0, {RowJson(0, 0, 0, 1)})})},
		{TEXT("a row with no position"), EditsPageJson({GroupJson(0, 0, 0, {TEXT("{\"voxelType\":1}")})})},
		{TEXT("a row with no type"), EditsPageJson({GroupJson(0, 0, 0, {TEXT("{\"location\":{\"x\":1,\"y\":1,\"z\":1}}")})})},
		{TEXT("a group with no coordinates"), EditsPageJson({TEXT("{\"voxels\":[]}")})},
		{TEXT("more groups than were asked for"), EditsPageJson(TooMany)},
		{TEXT("an answer that is not an edit list"), PageJson({})}};

	AddExpectedMessagePlain(TEXT("The chunk fetch ended BadReply"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, Cases.Num());
	for (const TPair<FString, FString>& Case : Cases)
	{
		FVoxelsRig Rig(TEXT("{\"data\":null}"));
		if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
		{
			return false;
		}
		Rig.Client->SetTestResponseScript({TPair<int32, FString>(200, PageJson({})), TPair<int32, FString>(200, Case.Value)});
		Rig.FetchBox(FInt64Vector(0, 0, 0), FInt64Vector(1, 0, 0));
		TestEqual(Case.Key, Rig.Answer.Result, ECrowdyVoxelResult::BadReply);
		TestEqual(Case.Key + TEXT(": nothing half-decoded is returned"), Rig.Answer.Chunks.Num(), 0);
	}

	FVoxelsRig Large(PageJson({}));
	AddExpectedMessagePlain(TEXT("The chunk fetch ended Failed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Large.FetchBox(FInt64Vector(0, 0, 0), FInt64Vector(17, 16, 16));
	TestEqual(TEXT("a box past 4,913 chunks fails before asking"), Large.Answer.Result, ECrowdyVoxelResult::Failed);
	FString Unsent;
	TestFalse(TEXT("so nothing was sent"), Large.Client->GetLastTestRequestBody(Unsent));

	FVoxelsRig Reversed(PageJson(EmptyChunks(4, 0)));
	Reversed.FetchBox(FInt64Vector(3, 0, 0), FInt64Vector(0, 0, 0));
	TestEqual(TEXT("corners in either order read the same box"), Reversed.Answer.Chunks.Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsEditsPageTest, "CrowdySDK.Voxels.EditsPageDecodesWithinItsBounds", VoxelTestFlags)

bool FCrowdyVoxelsEditsPageTest::RunTest(const FString& Parameters)
{
	// One group of 65,537 rows, newest first: the oldest, at a position no other row uses, is the one past the cap.
	constexpr uint32 Rows = 65537;
	FPack P;
	P.Map(1).Str("listVoxelUpdatesByDistance").Map(2).Str("limit").Raw({0x20});
	P.Str("chunks").Array(1).Map(2).Str("coordinates").Map(3).Str("x").Raw({7}).Str("y").Raw({0}).Str("z").Raw({0});
	P.Str("voxels").Raw({0xdd, static_cast<uint8>(Rows >> 24), static_cast<uint8>(Rows >> 16), static_cast<uint8>(Rows >> 8), static_cast<uint8>(Rows)});
	for (uint32 Row = 0; Row + 1 < Rows; ++Row)
	{
		P.Map(2).Str("location").Map(3).Str("x").Raw({static_cast<uint8>(Row % 16)}).Str("y").Raw({static_cast<uint8>((Row / 16) % 16)});
		P.Str("z").Raw({0xcc, static_cast<uint8>(Row / 256)}).Str("voxelType").Raw({1});
	}
	P.Map(2).Str("location").Map(3).Str("x").Raw({0}).Str("y").Raw({0}).Str("z").Raw({0xcd, 0x01, 0x2c}).Str("voxelType").Raw({7});

	TArray<FCrowdyVoxelChunk> Out;
	TSet<FInt64Vector> Seen;
	int32 Limit = 0;
	if (!TestEqual(TEXT("the page decodes"), CrowdyVoxelDecode::DecodeEditsPage(P.Bytes, 32, Out, Seen, Limit), 1) || !TestEqual(TEXT("one chunk"), Out.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("the newest 65,536 edits are kept"), Out[0].Edits.Num(), 65536);
	TestTrue(TEXT("and the chunk is marked truncated"), Out[0].bEditsTruncated);
	TestEqual(TEXT("the oldest is the one dropped"), UCrowdyVoxelLibrary::GetVoxel(Out[0], FIntVector(0, 0, 300)).Type, 0);
	TestFalse(TEXT("a chunk read from the edit log alone is not stored"), Out[0].bStored);

	Out.Reset();
	Seen.Reset();
	Seen.Add(FInt64Vector(7, 0, 0));
	TestEqual(TEXT("a group for a chunk already read is counted"), CrowdyVoxelDecode::DecodeEditsPage(P.Bytes, 32, Out, Seen, Limit), 1);
	TestEqual(TEXT("but not kept"), Out.Num(), 0);

	const TArray<uint8> Page = Packed(EditsPageJson({GroupJson(1, 2, 3, {RowJson(4, 5, 6, 7), RowJson(1, 1, 1, 2, TEXT("\"AAAA\""))})}));
	Out.Reset();
	Seen.Reset();
	if (!TestEqual(TEXT("a packed page decodes"), CrowdyVoxelDecode::DecodeEditsPage(Page, 32, Out, Seen, Limit), 1))
	{
		return false;
	}
	int32 Accepted = 0;
	for (int32 Length = 0; Length < Page.Num(); ++Length)
	{
		const TArray<uint8> Cut(Page.GetData(), Length);
		Out.Reset();
		Seen.Reset();
		Accepted += CrowdyVoxelDecode::DecodeEditsPage(Cut, 32, Out, Seen, Limit) != INDEX_NONE;
	}
	TestEqual(TEXT("no cut of the page is taken as whole"), Accepted, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsHelpersTest, "CrowdySDK.Voxels.PlacementHelpers", VoxelTestFlags)

bool FCrowdyVoxelsHelpersTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("voxel size without a session"), UCrowdyVoxelLibrary::GetVoxelSize(nullptr), 100.0);
	TestEqual(TEXT("origin of chunk (-1, 2, 3)"), UCrowdyVoxelLibrary::GetChunkOrigin(nullptr, -1, 2, 3), FVector(-1600.0, 3200.0, 4800.0));
	TestEqual(TEXT("origin of the seed chunk"), UCrowdyVoxelLibrary::GetChunkOrigin(nullptr, 37, -52, 1), FVector(59200.0, -83200.0, 1600.0));
	TestEqual(TEXT("centre of the lone cube"), UCrowdyVoxelLibrary::VoxelToWorld(nullptr, 37, -52, 1, FIntVector(1, 2, 3)), FVector(59350.0, -82950.0, 1950.0));

	struct FCase
	{
		FVector Location;
		FInt64Vector Chunk;
		FIntVector Cell;
	};
	const FCase Cases[] = {
		{FVector(59350.0, -82950.0, 1950.0), FInt64Vector(37, -52, 1), FIntVector(1, 2, 3)},
		{FVector(0.0, 0.0, 0.0), FInt64Vector(0, 0, 0), FIntVector(0, 0, 0)},
		{FVector(-1.0, -1.0, -1.0), FInt64Vector(-1, -1, -1), FIntVector(15, 15, 15)},
		{FVector(1599.999, -1600.0, 3200.0), FInt64Vector(0, -1, 2), FIntVector(15, 0, 0)},
		{FVector(1.0e30, -1.0e30, 0.0), FInt64Vector(9200000000000000000ll, -9200000000000000000ll, 0), FIntVector(15, 0, 0)}};
	for (const FCase& Case : Cases)
	{
		int64 X = 0;
		int64 Y = 0;
		int64 Z = 0;
		FIntVector Cell;
		UCrowdyVoxelLibrary::WorldToVoxel(nullptr, Case.Location, X, Y, Z, Cell);
		TestTrue(FString::Printf(TEXT("chunk of %s is %s"), *Case.Location.ToString(), *Case.Chunk.ToString()), FInt64Vector(X, Y, Z) == Case.Chunk);
		TestEqual(FString::Printf(TEXT("cell of %s"), *Case.Location.ToString()), Cell, Case.Cell);
	}

	FCrowdyVoxelChunk Chunk;
	Chunk.Grid.SetNumZeroed(Cells);
	Chunk.Grid[Index(15, 15, 15)] = 12;
	Chunk.Grid[Index(1, 2, 3)] = 7;
	const TArray<FCrowdyVoxel> Filled = UCrowdyVoxelLibrary::GetFilledVoxels(Chunk);
	if (!TestEqual(TEXT("two filled"), Filled.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("index order, first"), Filled[0].Position, FIntVector(1, 2, 3));
	TestEqual(TEXT("its type"), Filled[0].Type, 7);
	TestEqual(TEXT("index order, last"), Filled[1].Position, FIntVector(15, 15, 15));
	TestEqual(TEXT("an empty cell reads as type 0"), UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(0, 0, 0)).Type, 0);
	TestEqual(TEXT("a position past the grid with no edit reads as type 0"), UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(99, 0, 0)).Type, 0);
	FCrowdyVoxel& Edit = Chunk.Edits.AddDefaulted_GetRef();
	Edit.Type = 5;
	TestEqual(TEXT("the edit at (0, 0, 0) is found"), UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(0, 0, 0)).Type, 5);
	TestEqual(TEXT("a position past 16 bits never matches (0, 0, 0)'s edit"), UCrowdyVoxelLibrary::GetVoxel(Chunk, FIntVector(65536, 0, 0)).Type, 0);

	FCrowdyVoxelChunk Short;
	Short.Grid.Init(1, 3);
	TestEqual(TEXT("a short hand-made chunk reads only what it has"), UCrowdyVoxelLibrary::GetFilledVoxels(Short).Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsDecodeScalesTest, "CrowdySDK.Voxels.DecodeScalesLinearly", VoxelTestFlags)

bool FCrowdyVoxelsDecodeScalesTest::RunTest(const FString& Parameters)
{
	// Unsorted edits with repeated positions: the shape that makes ordering them costly. MostX past 15 puts edits outside
	// the grid, which are the ones sorted.
	const auto PageOf = [](int32 Edits, int32 MostX)
	{
		FRandomStream Random(99);
		TArray<FString> Items;
		for (int32 E = 0; E < Edits; ++E)
		{
			Items.Add(EditJson(Random.RandRange(MostX > 15 ? -20 : 0, MostX), Random.RandRange(0, 15), Random.RandRange(0, 15), Random.RandRange(1, 255)));
		}
		return Packed(PageJson({ChunkJson(0, 0, 0, TEXT("null"), FString::Join(Items, TEXT(",")))}));
	};
	const auto Fastest = [](const TArray<uint8>& Page)
	{
		double Best = MAX_dbl;
		for (int32 Run = 0; Run < 5; ++Run)
		{
			TArray<FCrowdyVoxelChunk> Out;
			TSet<FInt64Vector> Seen;
			int32 Limit = 0;
			const double Start = FPlatformTime::Seconds();
			CrowdyVoxelDecode::DecodePage(Page, 32, Out, Seen, Limit);
			Best = FMath::Min(Best, FPlatformTime::Seconds() - Start);
		}
		return Best;
	};
	for (const int32 MostX : {15, 35})
	{
		// Both sizes are past a desktop CPU's cache, so the ratio measures the algorithm, not the cache it falls out of;
		// the larger is past the platform's per-chunk cap for that reason. Linear is about 4x, quadratic 16x.
		const double Small = Fastest(PageOf(65536, MostX));
		const double Large = Fastest(PageOf(262144, MostX));
		const TCHAR* Shape = MostX > 15 ? TEXT("most outside the grid") : TEXT("all inside the grid");
		AddInfo(FString::Printf(TEXT("decode of one chunk, %s: 65,536 edits %.2f ms, 262,144 edits %.2f ms (%.1fx)"), Shape, Small * 1000.0, Large * 1000.0, Large / Small));
		TestTrue(FString::Printf(TEXT("%s: four times the edits takes less than ten times as long"), Shape), Large < Small * 10.0);
		TestTrue(FString::Printf(TEXT("%s: the platform's per-chunk cap of 65,536 edits decodes in under 100 ms"), Shape), Small < 0.1);
	}
	return true;
}

namespace
{
	TArray<FString> DenseChunks(int32 Count, int32 EditsPerChunk, int32 StateBytes)
	{
		FRandomStream Random(1234);
		TArray<uint8> Grid;
		Grid.SetNumUninitialized(Cells);
		TArray<uint8> StateBlob;
		StateBlob.SetNumZeroed(StateBytes);
		const FString State = StateBytes > 0 ? GridJson(StateBlob) : FString(TEXT("null"));

		TArray<FString> Out;
		for (int32 C = 0; C < Count; ++C)
		{
			for (uint8& Type : Grid)
			{
				Type = static_cast<uint8>(Random.RandRange(1, 255));
			}
			TArray<FString> Edits;
			for (int32 E = 0; E < EditsPerChunk; ++E)
			{
				Edits.Add(EditJson(Random.RandRange(0, 15), Random.RandRange(0, 15), Random.RandRange(0, 15), Random.RandRange(0, 255), State));
			}
			Out.Add(ChunkJson(C % 5 - 2, C / 5 % 5 - 2, C / 25 - 2, GridJson(Grid), FString::Join(Edits, TEXT(","))));
		}
		return Out;
	}

	// Milliseconds the bridge alone takes over every page, as a JSON tree or as MessagePack, whose bytes it keeps.
	double TimeBridge(const TArray<TPair<int32, FString>>& Script, bool bMsgpack, int32 Runs, TArray<TArray<uint8>>& OutPacked)
	{
		double Seconds = 0.0;
		for (int32 Run = 0; Run < Runs; ++Run)
		{
			const TSharedPtr<FCrowdyCppClient> Bridge = FCrowdyCppClient::MakeForTest(PageJson({}), 200);
			Bridge->SetTestResponseScript(Script);
			OutPacked.Reset();
			const double Start = FPlatformTime::Seconds();
			for (int32 Page = 0; Page < Script.Num(); ++Page)
			{
				bool bDone = false;
				auto OnDone = [&bDone, &OutPacked](FCrowdyCppJsonResult Result)
				{
					OutPacked.Add(MoveTemp(Result.Msgpack));
					bDone = true;
				};
				bMsgpack
					? Bridge->RunOpMsgpack(ECrowdyCppApiDomain::Chunks, TEXT("GetChunksByDistance"), MakeShared<FJsonObject>(), OnDone)
					: Bridge->RunOp(ECrowdyCppApiDomain::Chunks, TEXT("GetChunksByDistance"), MakeShared<FJsonObject>(), OnDone);
				for (int32 Poll = 0; Poll < 100 && !bDone; ++Poll)
				{
					Bridge->Poll();
				}
			}
			Seconds += FPlatformTime::Seconds() - Start;
		}
		return Seconds * 1000.0 / Runs;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyVoxelsPerfTest, "CrowdySDK.Perf.ChunkFetch", VoxelTestFlags)

bool FCrowdyVoxelsPerfTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Name;
		int32 Chunks;
		int32 Edits;
		int32 StateBytes;
	};
	const FCase Cases[] = {
		{TEXT("125 dense chunks, no edits"), 125, 0, 0},
		{TEXT("125 dense chunks, 256 edits each"), 125, 256, 0},
		{TEXT("125 dense chunks, 256 edits each with 64-byte states"), 125, 256, 64},
		{TEXT("1 chunk, 65,536 edits"), 1, 65536, 0},
		{TEXT("32 chunks, 8,192 edits each"), 32, 8192, 0}};

	for (const FCase& Case : Cases)
	{
		const int32 Range = Case.Chunks;
		const TArray<FString> Texts = DenseChunks(Range, Case.Edits, Case.StateBytes);

		TArray<TPair<int32, FString>> Script;
		for (int32 First = 0; First < Range; First += UCrowdyVoxels::PageSize)
		{
			const int32 Count = FMath::Min(UCrowdyVoxels::PageSize, Range - First);
			Script.Add(TPair<int32, FString>(200, PageJson(TArray<FString>(Texts.GetData() + First, Count))));
		}
		int64 Bytes = 0;
		for (const TPair<int32, FString>& Page : Script)
		{
			Bytes += Page.Value.Len();
		}

		constexpr int32 FetchRuns = 5;
		TArray<TArray<uint8>> PackedPages;
		const double TreeMs = TimeBridge(Script, false, FetchRuns, PackedPages);
		const double PackMs = TimeBridge(Script, true, FetchRuns, PackedPages);

		int64 PackedBytes = 0;
		for (const TArray<uint8>& Page : PackedPages)
		{
			PackedBytes += Page.Num();
		}
		TArray<FCrowdyVoxelChunk> Decoded;
		double DecodeSeconds = 0.0;
		for (int32 Run = 0; Run < FetchRuns; ++Run)
		{
			Decoded.Reset();
			TSet<FInt64Vector> Seen;
			const double Start = FPlatformTime::Seconds();
			for (const TArray<uint8>& Page : PackedPages)
			{
				int32 Limit = 0;
				CrowdyVoxelDecode::DecodePage(Page, UCrowdyVoxels::PageSize, Decoded, Seen, Limit);
			}
			DecodeSeconds += FPlatformTime::Seconds() - Start;
		}
		const double DecodeUs = DecodeSeconds * 1.0e6 / (FetchRuns * Range);

		double FetchSeconds = 0.0;
		int32 Fetched = 0;
		for (int32 Run = 0; Run < FetchRuns; ++Run)
		{
			FVoxelsRig Rig(PageJson({}));
			if (!TestTrue(TEXT("client built"), Rig.Client.IsValid()))
			{
				return false;
			}
			Rig.Client->SetTestResponseScript(Script);
			const double Start = FPlatformTime::Seconds();
			Rig.Fetch(0, 0, 0, 2);
			FetchSeconds += FPlatformTime::Seconds() - Start;
			Fetched = Rig.Answer.Chunks.Num();
		}
		TestEqual(FString::Printf(TEXT("%s: every chunk fetched"), Case.Name), Fetched, Range);
		TestEqual(FString::Printf(TEXT("%s: the timed decode read every chunk"), Case.Name), Decoded.Num(), Range);

		int32 EditCount = 0;
		for (const FCrowdyVoxelChunk& Chunk : Decoded)
		{
			EditCount += Chunk.Edits.Num();
		}
		const double FilledStart = FPlatformTime::Seconds();
		int32 FilledCount = 0;
		for (const FCrowdyVoxelChunk& Chunk : Decoded)
		{
			FilledCount += UCrowdyVoxelLibrary::GetFilledVoxels(Chunk).Num();
		}
		const double FilledUs = (FPlatformTime::Seconds() - FilledStart) * 1.0e6 / Range;

		AddInfo(FString::Printf(TEXT("chunk fetch perf, %s: %lld KB JSON over %d pages; bridge as a JSON tree %.2f ms, as MessagePack %.2f ms (%lld KB); decode %.1f us/chunk (%d edits kept); full fetch %.2f ms; filled voxels %.1f us/chunk (%d voxels)"),
			Case.Name, Bytes / 1024, Script.Num(), TreeMs, PackMs, PackedBytes / 1024, DecodeUs, EditCount, FetchSeconds * 1000.0 / FetchRuns, FilledUs, FilledCount));
	}
	return true;
}

#endif
