#include "Subsystem/CrowdyVoxels.h"
#include "Algo/Reverse.h"
#include "Algo/Sort.h"
#include "CrowdyCppClient.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyServicesLog.h"
#include "CrowdyVoxelDecode.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Misc/Base64.h"
#include "Subsystem/CrowdyGameSession.h"

using namespace CrowdyServiceApi;
using namespace CrowdyVoxelDecode;

namespace
{
	constexpr const TCHAR* VoxelsLogName = TEXT("CrowdyVoxels");
	constexpr int32 GridChars = 5464;
	constexpr int32 MaxStateChars = 1368;
	constexpr int32 MaxSkipDepth = 64;
	constexpr uint32 MostEditsReserved = 4096;

	// Reads MessagePack in place. Every read is bounds-checked, and a failed one leaves the reader failed for good.
	class FMsgpackReader
	{
	public:
		explicit FMsgpackReader(TConstArrayView<uint8> Bytes) : At(Bytes.GetData()), End(Bytes.GetData() + Bytes.Num()) {}

		bool Ok() const { return bOk; }
		bool Invalidate() { return Fail(); }
		const uint8* Position() const { return At; }
		void Seek(const uint8* Position) { At = Position; }

		bool IsNil() const { return bOk && At < End && *At == 0xc0; }
		bool IsStr() const { return bOk && At < End && ((*At >= 0xa0 && *At <= 0xbf) || (*At >= 0xd9 && *At <= 0xdb)); }

		bool Map(uint32& OutCount) { return Container(0x80, 0xde, OutCount); }
		bool Array(uint32& OutCount) { return Container(0x90, 0xdc, OutCount); }

		bool Str(FAnsiStringView& Out)
		{
			const uint8 Tag = Peek();
			uint64 Length = 0;
			if (Tag >= 0xa0 && Tag <= 0xbf)
			{
				++At;
				Length = Tag & 0x1f;
			}
			else if (Tag >= 0xd9 && Tag <= 0xdb)
			{
				++At;
				Length = BigEndian(1 << (Tag - 0xd9));
			}
			else
			{
				return Fail();
			}
			const uint8* Bytes = Take(Length);
			Out = Bytes ? FAnsiStringView(reinterpret_cast<const ANSICHAR*>(Bytes), static_cast<int32>(Length)) : FAnsiStringView();
			return bOk;
		}

		bool Int(int64& Out)
		{
			const uint8 Tag = Peek();
			if (Tag <= 0x7f || Tag >= 0xe0)
			{
				++At;
				Out = static_cast<int8>(Tag);
				return bOk;
			}
			if (Tag >= 0xcc && Tag <= 0xcf)
			{
				++At;
				const uint64 Value = BigEndian(1 << (Tag - 0xcc));
				Out = static_cast<int64>(Value);
				return bOk && Value <= static_cast<uint64>(MAX_int64) ? true : Fail();
			}
			if (Tag >= 0xd0 && Tag <= 0xd3)
			{
				++At;
				const int32 Bytes = 1 << (Tag - 0xd0);
				const int32 Shift = 64 - 8 * Bytes;
				Out = static_cast<int64>(BigEndian(Bytes) << Shift) >> Shift;
				return bOk;
			}
			return Fail();
		}

		bool Bool(bool& Out)
		{
			const uint8 Tag = Peek();
			if (Tag != 0xc2 && Tag != 0xc3)
			{
				return Fail();
			}
			++At;
			Out = Tag == 0xc3;
			return bOk;
		}

		bool Skip(int32 Depth = 0)
		{
			const uint8 Tag = Peek();
			if (!bOk || Depth > MaxSkipDepth)
			{
				return Fail();
			}
			uint32 Count = 0;
			if ((Tag >= 0x80 && Tag <= 0x8f) || Tag == 0xde || Tag == 0xdf)
			{
				return Map(Count) && SkipItems(static_cast<uint64>(Count) * 2, Depth);
			}
			if ((Tag >= 0x90 && Tag <= 0x9f) || Tag == 0xdc || Tag == 0xdd)
			{
				return Array(Count) && SkipItems(Count, Depth);
			}
			++At;
			if (Tag <= 0x7f || Tag >= 0xe0 || Tag == 0xc0 || Tag == 0xc2 || Tag == 0xc3)
			{
				return true;
			}
			if (Tag >= 0xa0 && Tag <= 0xbf)
			{
				return Take(Tag & 0x1f) != nullptr;
			}
			switch (Tag)
			{
			case 0xcc: case 0xd0: return Take(1) != nullptr;
			case 0xcd: case 0xd1: case 0xd4: return Take(2) != nullptr;
			case 0xd5: return Take(3) != nullptr;
			case 0xce: case 0xd2: case 0xca: return Take(4) != nullptr;
			case 0xd6: return Take(5) != nullptr;
			case 0xcf: case 0xd3: case 0xcb: return Take(8) != nullptr;
			case 0xd7: return Take(9) != nullptr;
			case 0xd8: return Take(17) != nullptr;
			case 0xc4: case 0xd9: return Take(BigEndian(1)) != nullptr;
			case 0xc5: case 0xda: return Take(BigEndian(2)) != nullptr;
			case 0xc6: case 0xdb: return Take(BigEndian(4)) != nullptr;
			case 0xc7: return Take(BigEndian(1) + 1) != nullptr;
			case 0xc8: return Take(BigEndian(2) + 1) != nullptr;
			case 0xc9: return Take(BigEndian(4) + 1) != nullptr;
			default: return Fail();
			}
		}

	private:
		const uint8* At;
		const uint8* End;
		bool bOk = true;

		bool Fail()
		{
			bOk = false;
			At = End;
			return false;
		}

		uint8 Peek()
		{
			if (!bOk || At >= End)
			{
				Fail();
				return 0xc1;
			}
			return *At;
		}

		const uint8* Take(uint64 Length)
		{
			if (!bOk || Length > static_cast<uint64>(End - At))
			{
				Fail();
				return nullptr;
			}
			const uint8* Taken = At;
			At += Length;
			return Taken;
		}

		uint64 BigEndian(int32 Bytes)
		{
			const uint8* Data = Take(Bytes);
			uint64 Value = 0;
			for (int32 Index = 0; Data && Index < Bytes; ++Index)
			{
				Value = (Value << 8) | Data[Index];
			}
			return Value;
		}

		bool Container(uint8 FixBase, uint8 Wide16, uint32& OutCount)
		{
			const uint8 Tag = Peek();
			if (Tag >= FixBase && Tag <= FixBase + 0x0f)
			{
				++At;
				OutCount = Tag & 0x0f;
				return bOk;
			}
			if (Tag != Wide16 && Tag != Wide16 + 1)
			{
				return Fail();
			}
			++At;
			OutCount = static_cast<uint32>(BigEndian(Tag == Wide16 ? 2 : 4));
			// Every item takes at least a byte, so a count past what is left cannot be true.
			return bOk && OutCount <= static_cast<uint64>(End - At) ? true : Fail();
		}

		bool SkipItems(uint64 Count, int32 Depth)
		{
			for (uint64 Item = 0; Item < Count && bOk; ++Item)
			{
				Skip(Depth + 1);
			}
			return bOk;
		}
	};

	// Digits with an optional sign and nothing else.
	bool ParseInt64(FAnsiStringView Text, int64& Out)
	{
		const bool bNegative = Text.StartsWith('-');
		const FAnsiStringView Digits = bNegative ? Text.RightChop(1) : Text;
		if (Digits.IsEmpty() || Digits.Len() > 19)
		{
			return false;
		}
		uint64 Value = 0;
		for (const ANSICHAR Digit : Digits)
		{
			if (Digit < '0' || Digit > '9')
			{
				return false;
			}
			Value = Value * 10 + static_cast<uint64>(Digit - '0');
		}
		if (Value > static_cast<uint64>(MAX_int64) + (bNegative ? 1 : 0))
		{
			return false;
		}
		Out = bNegative ? static_cast<int64>(0 - Value) : static_cast<int64>(Value);
		return true;
	}

	// The Game API sends BigInt as a decimal string and Int as a number; either is accepted for both. A string that is
	// not an integer fails the reader, so nothing after it is read out of place.
	bool ReadInteger(FMsgpackReader& Reader, int64& Out)
	{
		if (!Reader.IsStr())
		{
			return Reader.Int(Out);
		}
		FAnsiStringView Text;
		return Reader.Str(Text) && (ParseInt64(Text, Out) || Reader.Invalidate());
	}

	bool ReadPoint(FMsgpackReader& Reader, int64 (&Out)[3])
	{
		uint32 Fields = 0;
		if (!Reader.Map(Fields))
		{
			return false;
		}
		uint32 Found = 0;
		for (uint32 Field = 0; Field < Fields; ++Field)
		{
			FAnsiStringView Key;
			if (!Reader.Str(Key))
			{
				return false;
			}
			const int32 Axis = Key == "x" ? 0 : Key == "y" ? 1 : Key == "z" ? 2 : INDEX_NONE;
			if (Axis == INDEX_NONE)
			{
				Reader.Skip();
				continue;
			}
			if (!ReadInteger(Reader, Out[Axis]))
			{
				return false;
			}
			Found |= 1u << Axis;
		}
		return Reader.Ok() && Found == 7;
	}

	bool FitsInt16(int64 Value)
	{
		return Value >= MIN_int16 && Value <= MAX_int16;
	}

	// Base64 of at most 1,024 bytes; an empty string is no state.
	bool ReadState(FMsgpackReader& Reader, TArray<uint8>& Out)
	{
		if (Reader.IsNil())
		{
			return Reader.Skip();
		}
		FAnsiStringView Text;
		if (!Reader.Str(Text) || Text.Len() > MaxStateChars)
		{
			return Reader.Invalidate();
		}
		if (Text.IsEmpty())
		{
			return true;
		}
		const uint32 Size = FBase64::GetDecodedDataSize(Text.GetData(), Text.Len());
		if (Size == 0 || Size > MaxStateBytes)
		{
			return Reader.Invalidate();
		}
		Out.SetNumUninitialized(Size);
		return FBase64::Decode(Text.GetData(), Text.Len(), Out.GetData()) || Reader.Invalidate();
	}

	// Reused across one answer's chunks: the newest edit per grid cell, and the edits outside the grid by position.
	struct FEditScratch
	{
		TArray<int32> NewestAtCell;
		TArray<TPair<uint64, int32>> Outside;
	};

	// Sorts edits by position, keeping the newest at each. Cells inside the grid go through a table indexed by cell, which
	// is already position order, so only edits outside the grid are sorted.
	void KeepNewestInOrder(TArray<FCrowdyVoxel>& Edits, FEditScratch& Scratch)
	{
		Scratch.NewestAtCell.SetNumUninitialized(VoxelCount);
		FMemory::Memset(Scratch.NewestAtCell.GetData(), 0xff, VoxelCount * sizeof(int32));
		Scratch.Outside.Reset();
		for (int32 Index = 0; Index < Edits.Num(); ++Index)
		{
			const int32 Cell = GridIndex(Edits[Index].Position);
			if (Cell != INDEX_NONE)
			{
				Scratch.NewestAtCell[Cell] = Index;
				continue;
			}
			Scratch.Outside.Emplace(PositionKey(Edits[Index].Position), Index);
		}
		Algo::Sort(Scratch.Outside);

		TArray<FCrowdyVoxel> Kept;
		Kept.Reserve(FMath::Min(Edits.Num(), VoxelCount + Scratch.Outside.Num()));
		int32 Wide = 0;
		const auto TakeOutsideBelow = [&](uint64 Below)
		{
			for (; Wide < Scratch.Outside.Num() && Scratch.Outside[Wide].Key < Below; ++Wide)
			{
				const bool bNewest = Wide + 1 == Scratch.Outside.Num() || Scratch.Outside[Wide + 1].Key != Scratch.Outside[Wide].Key;
				if (bNewest)
				{
					Kept.Add(MoveTemp(Edits[Scratch.Outside[Wide].Value]));
				}
			}
		};
		for (int32 Cell = 0; Cell < VoxelCount; ++Cell)
		{
			const int32 Newest = Scratch.NewestAtCell[Cell];
			if (Newest == INDEX_NONE)
			{
				continue;
			}
			TakeOutsideBelow(PositionKey(Edits[Newest].Position));
			Kept.Add(MoveTemp(Edits[Newest]));
		}
		TakeOutsideBelow(MAX_uint64);
		Edits = MoveTemp(Kept);
	}

	// How one answer lists a chunk's edits: a chunk's voxelStates oldest first, an edit-log group newest first.
	struct FEditRows
	{
		FAnsiStringView PositionKey;
		bool bNewestFirst = false;
	};

	// Keeps every edit, applies those that fit a cell and a byte to the grid oldest first, then orders the edits by
	// position keeping the newest of each, since the platform may list a position more than once.
	bool ReadEdits(FMsgpackReader& Reader, FCrowdyVoxelChunk& Out, FEditScratch& Scratch, const FEditRows& Rows)
	{
		uint32 Count = 0;
		if (!Reader.Array(Count))
		{
			return false;
		}
		// The count is the answer's own claim, so only a grid's worth is reserved ahead of the edits that back it.
		Out.Edits.Reserve(FMath::Min(Count, MostEditsReserved));
		for (uint32 Item = 0; Item < Count; ++Item)
		{
			// Newest first, so past the cap only the oldest are dropped.
			if (Rows.bNewestFirst && Out.Edits.Num() >= MaxEditsPerChunk)
			{
				Out.bEditsTruncated = true;
				if (!Reader.Skip())
				{
					return false;
				}
				continue;
			}
			uint32 Fields = 0;
			if (!Reader.Map(Fields))
			{
				return false;
			}
			int64 Cell[3] = {0, 0, 0};
			int64 Type = 0;
			bool bCell = false;
			bool bType = false;
			TArray<uint8> State;
			for (uint32 Field = 0; Field < Fields && Reader.Ok(); ++Field)
			{
				FAnsiStringView Key;
				if (!Reader.Str(Key))
				{
					return false;
				}
				if (Key == Rows.PositionKey)
				{
					bCell = ReadPoint(Reader, Cell);
				}
				else if (Key == "voxelType")
				{
					bType = ReadInteger(Reader, Type);
				}
				else if (Key == "state")
				{
					ReadState(Reader, State);
				}
				else
				{
					Reader.Skip();
				}
			}
			// Both are required by the platform's schema, and positions and types are 16-bit on every platform path, so an
			// edit without them, or with a wider value, is not an answer from it.
			if (!Reader.Ok() || !bCell || !bType || !FitsInt16(Cell[0]) || !FitsInt16(Cell[1]) || !FitsInt16(Cell[2]) || !FitsInt16(Type))
			{
				return Reader.Invalidate();
			}
			FCrowdyVoxel& Edit = Out.Edits.AddDefaulted_GetRef();
			Edit.Position = FIntVector(static_cast<int32>(Cell[0]), static_cast<int32>(Cell[1]), static_cast<int32>(Cell[2]));
			Edit.Type = static_cast<int32>(Type);
			Edit.State = MoveTemp(State);
		}

		if (Rows.bNewestFirst)
		{
			Algo::Reverse(Out.Edits);
		}
		for (const FCrowdyVoxel& Edit : Out.Edits)
		{
			const int32 Index = GridIndex(Edit.Position);
			if (Index != INDEX_NONE)
			{
				Out.Grid[Index] = Edit.Type >= 0 && Edit.Type <= MAX_uint8 ? static_cast<uint8>(Edit.Type) : 0;
			}
		}

		bool bInOrder = true;
		for (int32 Index = 1; Index < Out.Edits.Num() && bInOrder; ++Index)
		{
			bInOrder = PositionKey(Out.Edits[Index - 1].Position) < PositionKey(Out.Edits[Index].Position);
		}
		if (!bInOrder)
		{
			KeepNewestInOrder(Out.Edits, Scratch);
		}
		return true;
	}

	// Edits apply over the grid whichever order the two arrive in, so their place is kept and read last.
	bool DecodeChunk(FMsgpackReader& Reader, FCrowdyVoxelChunk& Out, FEditScratch& Scratch)
	{
		uint32 Fields = 0;
		if (!Reader.Map(Fields))
		{
			return false;
		}
		Out.Grid.SetNumUninitialized(VoxelCount);
		Out.Edits.Reset();
		Out.bStored = true;
		Out.bEditsTruncated = false;
		int64 Point[3] = {0, 0, 0};
		bool bPoint = false;
		bool bGrid = false;
		const uint8* Edits = nullptr;
		for (uint32 Field = 0; Field < Fields && Reader.Ok(); ++Field)
		{
			FAnsiStringView Key;
			if (!Reader.Str(Key))
			{
				return false;
			}
			if (Key == "coordinates")
			{
				bPoint = ReadPoint(Reader, Point);
				continue;
			}
			if (Key == "voxelStatesTruncated")
			{
				Reader.IsNil() ? Reader.Skip() : Reader.Bool(Out.bEditsTruncated);
				continue;
			}
			if (Key == "voxelStates" && !Reader.IsNil())
			{
				Edits = Reader.Position();
				Reader.Skip();
				continue;
			}
			if (Key != "voxels" || Reader.IsNil())
			{
				Reader.Skip();
				continue;
			}
			FAnsiStringView Grid;
			if (!Reader.Str(Grid) || Grid.Len() > GridChars || FBase64::GetDecodedDataSize(Grid.GetData(), Grid.Len()) != static_cast<uint32>(VoxelCount)
				|| !FBase64::Decode(Grid.GetData(), Grid.Len(), Out.Grid.GetData()))
			{
				return false;
			}
			bGrid = true;
		}
		if (!Reader.Ok() || !bPoint)
		{
			return false;
		}
		Out.X = Point[0];
		Out.Y = Point[1];
		Out.Z = Point[2];
		if (!bGrid)
		{
			FMemory::Memzero(Out.Grid.GetData(), VoxelCount);
		}
		if (!Edits)
		{
			return true;
		}
		const uint8* Resume = Reader.Position();
		Reader.Seek(Edits);
		const bool bRead = ReadEdits(Reader, Out, Scratch, FEditRows{"voxelCoord", false});
		Reader.Seek(Resume);
		return bRead;
	}

	// One listVoxelUpdatesByDistance chunk group, kept unless its coordinates are already in Seen. Its edits are read in
	// place when the coordinates come first, else found again once they are known. bOutKept says whether Out holds it.
	bool DecodeEditGroup(FMsgpackReader& Reader, FCrowdyVoxelChunk& Out, FEditScratch& Scratch, TSet<FInt64Vector>& Seen, bool& bOutKept)
	{
		bOutKept = false;
		uint32 Fields = 0;
		if (!Reader.Map(Fields))
		{
			return false;
		}
		const FEditRows Rows{"location", true};
		bool bPoint = false;
		bool bKeep = false;
		bool bEditsRead = false;
		const uint8* Edits = nullptr;
		for (uint32 Field = 0; Field < Fields && Reader.Ok(); ++Field)
		{
			FAnsiStringView Key;
			if (!Reader.Str(Key))
			{
				return false;
			}
			if (Key == "coordinates")
			{
				int64 Point[3] = {0, 0, 0};
				if (bPoint || !ReadPoint(Reader, Point))
				{
					return Reader.Invalidate();
				}
				bPoint = true;
				bool bSeen = false;
				Seen.Add(FInt64Vector(Point[0], Point[1], Point[2]), &bSeen);
				bKeep = !bSeen;
				Out.X = Point[0];
				Out.Y = Point[1];
				Out.Z = Point[2];
				Out.Grid.SetNumZeroed(VoxelCount);
				continue;
			}
			if (Key != "voxels" || Reader.IsNil() || bEditsRead)
			{
				Reader.Skip();
				continue;
			}
			if (bKeep)
			{
				bEditsRead = ReadEdits(Reader, Out, Scratch, Rows);
				continue;
			}
			Edits = bPoint ? nullptr : Reader.Position();
			Reader.Skip();
		}
		if (!Reader.Ok() || !bPoint)
		{
			return false;
		}
		if (bKeep && Edits && !bEditsRead)
		{
			const uint8* Resume = Reader.Position();
			Reader.Seek(Edits);
			bEditsRead = ReadEdits(Reader, Out, Scratch, Rows);
			Reader.Seek(Resume);
		}
		bOutKept = bKeep && bEditsRead && Out.Edits.Num() > 0;
		return Reader.Ok();
	}

	// Steps into the value of Key in the map the reader is on, skipping every other member.
	bool EnterMember(FMsgpackReader& Reader, FAnsiStringView Key)
	{
		uint32 Fields = 0;
		if (!Reader.Map(Fields))
		{
			return false;
		}
		for (uint32 Field = 0; Field < Fields; ++Field)
		{
			FAnsiStringView Name;
			if (!Reader.Str(Name))
			{
				return false;
			}
			if (Name == Key)
			{
				return true;
			}
			Reader.Skip();
		}
		return false;
	}

	const TCHAR* ResultName(ECrowdyVoxelResult Result)
	{
		switch (Result)
		{
		case ECrowdyVoxelResult::Ok: return TEXT("Ok");
		case ECrowdyVoxelResult::NotSignedIn: return TEXT("NotSignedIn");
		case ECrowdyVoxelResult::Refused: return TEXT("Refused");
		case ECrowdyVoxelResult::BadReply: return TEXT("BadReply");
		default: return TEXT("Failed");
		}
	}

	void FinishFetch(const TFunction<void(const FCrowdyChunkFetch&)>& OnDone, ECrowdyVoxelResult Result, const FString& Message, TArray<FCrowdyVoxelChunk>&& Chunks)
	{
		FCrowdyChunkFetch Fetch;
		Fetch.Result = Result;
		Fetch.Message = Message;
		if (Result == ECrowdyVoxelResult::Ok)
		{
			Fetch.Chunks = MoveTemp(Chunks);
		}
		else
		{
			UE_LOG(LogCrowdyServices, Warning, TEXT("[%s] The chunk fetch ended %s: %s"), VoxelsLogName, ResultName(Result), *Message);
		}
		if (OnDone)
		{
			OnDone(Fetch);
		}
	}

	void FinishFetchFailure(const TFunction<void(const FCrowdyChunkFetch&)>& OnDone, const FCrowdyCppJsonResult& Result)
	{
		const bool bRefused = !Result.ErrorCode.IsEmpty();
		FinishFetch(OnDone, bRefused ? ECrowdyVoxelResult::Refused : ECrowdyVoxelResult::Failed,
			bRefused ? FString::Printf(TEXT("%s: %s"), *Result.ErrorCode, *Result.ErrorMessage) : Result.ErrorMessage, {});
	}

	TSharedPtr<FJsonObject> ChunkInput(int64 AppId, int64 X, int64 Y, int64 Z, const TCHAR* CoordinatesKey)
	{
		TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
		Point->SetStringField(TEXT("x"), BigInt(X));
		Point->SetStringField(TEXT("y"), BigInt(Y));
		Point->SetStringField(TEXT("z"), BigInt(Z));
		TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("appId"), BigInt(AppId));
		Input->SetObjectField(CoordinatesKey, Point);
		return Input;
	}

	TSharedPtr<FJsonObject> PointInput(const FInt64Vector& Coord)
	{
		TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
		Point->SetStringField(TEXT("x"), BigInt(Coord.X));
		Point->SetStringField(TEXT("y"), BigInt(Coord.Y));
		Point->SetStringField(TEXT("z"), BigInt(Coord.Z));
		return Point;
	}

	TSharedPtr<FJsonObject> BoxInput(int64 AppId, const FInt64Vector& Low, const FInt64Vector& High, int32 Skip)
	{
		TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("appId"), BigInt(AppId));
		Input->SetObjectField(TEXT("lowChunk"), PointInput(Low));
		Input->SetObjectField(TEXT("highChunk"), PointInput(High));
		Input->SetNumberField(TEXT("limit"), UCrowdyVoxels::PageSize);
		Input->SetNumberField(TEXT("skip"), Skip);
		return Input;
	}

	// One edit-log read over a box of chunks the chunk read did not return.
	struct FEditsBox
	{
		FInt64Vector Low;
		FInt64Vector High;
		int32 MostChunks = 0;
		int32 Skip = 0;
	};

	// The chunks of Low to High not in Seen, split into boxes of one page that hold only those chunks.
	void FindAbsentBoxes(const FInt64Vector& Low, const FInt64Vector& High, const TSet<FInt64Vector>& Seen, TArray<FEditsBox>& Out)
	{
		const FIntVector Size = BoxSize(Low, High);
		const int32 Count = BoxCount(Size);
		TSet<FInt64Vector> Absent;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FInt64Vector Coord = BoxCell(Low, Size, Index);
			if (!Seen.Contains(Coord))
			{
				Absent.Add(Coord);
			}
		}
		SplitIntoBoxes(Absent, UCrowdyVoxels::PageSize, [&Out](const FInt64Vector& BoxLow, const FInt64Vector& BoxHigh)
		{
			Out.Add({BoxLow, BoxHigh, BoxCount(BoxSize(BoxLow, BoxHigh)), 0});
		});
	}

	// Walks one paged region answer under Root: its echoed limit and its single chunk list, each item read by Item.
	int32 DecodeRegionPage(TConstArrayView<uint8> Msgpack, FAnsiStringView Root, int32 MaxChunks, int32& OutLimit,
		TFunctionRef<bool(FMsgpackReader&, FEditScratch&)> Item)
	{
		FMsgpackReader Reader(Msgpack);
		uint32 Fields = 0;
		if (!EnterMember(Reader, Root) || !Reader.Map(Fields))
		{
			return INDEX_NONE;
		}
		OutLimit = MaxChunks;
		int32 Count = INDEX_NONE;
		for (uint32 Field = 0; Field < Fields && Reader.Ok(); ++Field)
		{
			FAnsiStringView Key;
			if (!Reader.Str(Key))
			{
				return INDEX_NONE;
			}
			if (Key == "limit" && !Reader.IsNil())
			{
				int64 Limit = 0;
				OutLimit = ReadInteger(Reader, Limit) && Limit > 0 && Limit < MaxChunks ? static_cast<int32>(Limit) : MaxChunks;
				continue;
			}
			if (Key != "chunks")
			{
				Reader.Skip();
				continue;
			}
			uint32 Items = 0;
			// More chunks than were asked for is not an answer to this request, and each one costs 4 KiB; a second chunk
			// list would be a way past that bound.
			if (Count != INDEX_NONE || !Reader.Array(Items) || Items > static_cast<uint32>(MaxChunks))
			{
				return INDEX_NONE;
			}
			FEditScratch Scratch;
			for (uint32 Index = 0; Index < Items; ++Index)
			{
				if (!Item(Reader, Scratch))
				{
					return INDEX_NONE;
				}
			}
			Count = static_cast<int32>(Items);
		}
		return Reader.Ok() ? Count : INDEX_NONE;
	}
}

int32 CrowdyVoxelDecode::DecodePage(TConstArrayView<uint8> Msgpack, int32 MaxChunks, TArray<FCrowdyVoxelChunk>& Out, TSet<FInt64Vector>& Seen, int32& OutLimit)
{
	return DecodeRegionPage(Msgpack, "getChunksByDistance", MaxChunks, OutLimit, [&Out, &Seen](FMsgpackReader& Reader, FEditScratch& Scratch)
	{
		FCrowdyVoxelChunk& Chunk = Out.AddDefaulted_GetRef();
		if (!DecodeChunk(Reader, Chunk, Scratch))
		{
			return false;
		}
		bool bSeen = false;
		Seen.Add(FInt64Vector(Chunk.X, Chunk.Y, Chunk.Z), &bSeen);
		if (bSeen)
		{
			Out.Pop(EAllowShrinking::No);
		}
		return true;
	});
}

int32 CrowdyVoxelDecode::DecodeEditsPage(TConstArrayView<uint8> Msgpack, int32 MaxChunks, TArray<FCrowdyVoxelChunk>& Out, TSet<FInt64Vector>& Seen, int32& OutLimit)
{
	return DecodeRegionPage(Msgpack, "listVoxelUpdatesByDistance", MaxChunks, OutLimit, [&Out, &Seen](FMsgpackReader& Reader, FEditScratch& Scratch)
	{
		FCrowdyVoxelChunk& Chunk = Out.AddDefaulted_GetRef();
		bool bKept = false;
		if (!DecodeEditGroup(Reader, Chunk, Scratch, Seen, bKept))
		{
			return false;
		}
		if (!bKept)
		{
			Out.Pop(EAllowShrinking::No);
		}
		return true;
	});
}

int32 CrowdyVoxelDecode::DecodeOne(TConstArrayView<uint8> Msgpack, FCrowdyVoxelChunk& Out)
{
	FMsgpackReader Reader(Msgpack);
	if (!EnterMember(Reader, "getChunk"))
	{
		return INDEX_NONE;
	}
	if (Reader.IsNil())
	{
		return 0;
	}
	FEditScratch Scratch;
	return DecodeChunk(Reader, Out, Scratch) ? 1 : INDEX_NONE;
}

struct UCrowdyVoxels::FRead
{
	int64 AppId = 0;
	FInt64Vector Centre;
	FInt64Vector Low;
	FInt64Vector High;
	int32 Range = 1;
	int32 MostChunks = 0;
	int32 Skip = 0;
	int32 EditsLeft = 0;
	bool bBox = false;
	bool bDone = false;
	TArray<FEditsBox> EditsBoxes;
	TArray<FCrowdyVoxelChunk> Chunks;
	TSet<FInt64Vector> Seen;
	TSharedPtr<const bool> Cancelled;
	TFunction<void(const FCrowdyChunkFetch&)> OnDone;

	/** Done or cancelled: an answer still arriving changes nothing and asks for nothing more. */
	bool IsOver() const
	{
		return bDone || (Cancelled.IsValid() && *Cancelled);
	}

	void Finish(ECrowdyVoxelResult Result, const FString& Message)
	{
		if (bDone)
		{
			return;
		}
		bDone = true;
		FinishFetch(OnDone, Result, Message, MoveTemp(Chunks));
	}

	void FinishFailure(const FCrowdyCppJsonResult& Result)
	{
		if (bDone)
		{
			return;
		}
		bDone = true;
		FinishFetchFailure(OnDone, Result);
	}
};

void UCrowdyVoxels::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LiveSessionToken = MakeShared<uint8>(0);
}

void UCrowdyVoxels::Deinitialize()
{
	LiveSessionToken.Reset();
	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyVoxels::InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId)
{
	LiveSessionToken = MakeShared<uint8>(0);
	ClientForTest = Client;
	AppIdForTest = AppId;
}
#endif

int64 UCrowdyVoxels::GetAppId() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (AppIdForTest != 0)
	{
		return AppIdForTest;
	}
#endif
	return ResolveAppId(GetGameInstance());
}

bool UCrowdyVoxels::HasAppToken() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ClientForTest.IsValid())
	{
		return true;
	}
#endif
	const UGameInstance* GameInstance = GetGameInstance();
	const UCrowdyGameSession* Session = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	return Session && !Session->GetGameToken().IsEmpty();
}

FCrowdyCppClient* UCrowdyVoxels::ResolveClient() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (const TSharedPtr<FCrowdyCppClient> Override = ClientForTest.Pin())
	{
		return Override.Get();
	}
#endif
	return ResolveApiClient(GetGameInstance(), VoxelsLogName);
}

void UCrowdyVoxels::FetchChunksAround(int64 ChunkX, int64 ChunkY, int64 ChunkZ, int32 Range, TFunction<void(const FCrowdyChunkFetch&)> OnDone)
{
	const TSharedRef<FRead> Read = MakeShared<FRead>();
	Read->AppId = GetAppId();
	Read->Centre = FInt64Vector(ChunkX, ChunkY, ChunkZ);
	Read->Range = FMath::Clamp(Range, 1, 8);
	Read->OnDone = MoveTemp(OnDone);

	const int32 Side = 2 * Read->Range + 1;
	Read->MostChunks = Side * Side * Side;

	if (!HasAppToken())
	{
		Read->Finish(ECrowdyVoxelResult::NotSignedIn, TEXT("The player has no app token yet."));
		return;
	}
	RequestPage(Read);
}

void UCrowdyVoxels::RequestPage(const TSharedRef<FRead>& Read)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		Read->Finish(ECrowdyVoxelResult::Failed, TEXT("The Crowdy API client is unavailable, so the request was not sent."));
		return;
	}
	TSharedPtr<FJsonObject> Input;
	if (Read->bBox)
	{
		Input = BoxInput(Read->AppId, Read->Low, Read->High, Read->Skip);
	}
	else
	{
		Input = ChunkInput(Read->AppId, Read->Centre.X, Read->Centre.Y, Read->Centre.Z, TEXT("centerCoordinate"));
		Input->SetNumberField(TEXT("maxDistance"), Read->Range);
		Input->SetNumberField(TEXT("limit"), PageSize);
		Input->SetNumberField(TEXT("skip"), Read->Skip);
	}

	TWeakObjectPtr<UCrowdyVoxels> WeakThis(this);
	Client->RunOpMsgpack(ECrowdyCppApiDomain::Chunks, TEXT("GetChunksByDistanceVoxels"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, Read](FCrowdyCppJsonResult Result)
		{
			UCrowdyVoxels* Self = WeakThis.Get();
			if (Read->IsOver())
			{
				return;
			}
			if (!Self || Self->GetAppId() != Read->AppId)
			{
				Read->Finish(ECrowdyVoxelResult::Failed, TEXT("The app changed while the fetch was in flight."));
				return;
			}
			if (!Result.bTransportOk)
			{
				Read->FinishFailure(Result);
				return;
			}

			const int32 Before = Read->Chunks.Num();
			int32 Limit = PageSize;
			const int32 PageChunks = DecodePage(Result.Msgpack, PageSize, Read->Chunks, Read->Seen, Limit);
			if (PageChunks == INDEX_NONE)
			{
				Read->Finish(ECrowdyVoxelResult::BadReply, TEXT("The answer holds no well-formed chunk list."));
				return;
			}
			if (Read->Chunks.Num() > Read->MostChunks)
			{
				Read->Finish(ECrowdyVoxelResult::BadReply, TEXT("The platform answered more chunks than the range holds."));
				return;
			}
			for (int32 Index = Before; Read->bBox && Index < Read->Chunks.Num(); ++Index)
			{
				const FCrowdyVoxelChunk& Chunk = Read->Chunks[Index];
				if (!InBox(FInt64Vector(Chunk.X, Chunk.Y, Chunk.Z), Read->Low, Read->High))
				{
					Read->Finish(ECrowdyVoxelResult::BadReply, TEXT("The platform answered a chunk outside the box."));
					return;
				}
			}
			// Pages are stable, so a chunk on two pages is kept once; the region's size bounds how many pages are read.
			Read->Skip += Limit;
			if (PageChunks == Limit && Read->Skip < Read->MostChunks)
			{
				Self->RequestPage(Read);
				return;
			}
			if (Read->bBox)
			{
				FindAbsentBoxes(Read->Low, Read->High, Read->Seen, Read->EditsBoxes);
			}
			// Only the chunks a box read left out have their edits read, in boxes of one page read side by side.
			Read->EditsLeft = Read->EditsBoxes.Num();
			if (Read->EditsLeft == 0)
			{
				Read->Finish(ECrowdyVoxelResult::Ok, FString());
				return;
			}
			for (int32 Box = 0; Box < Read->EditsBoxes.Num() && !Read->IsOver(); ++Box)
			{
				Self->RequestEditsPage(Read, Box);
			}
		}));
}

void UCrowdyVoxels::FetchChunksInBox(const FInt64Vector& Corner, const FInt64Vector& OppositeCorner, TFunction<void(const FCrowdyChunkFetch&)> OnDone,
	TSharedPtr<const bool> Cancelled)
{
	const TSharedRef<FRead> Read = MakeShared<FRead>();
	Read->AppId = GetAppId();
	Read->Cancelled = MoveTemp(Cancelled);
	Read->bBox = true;
	Read->Low = FInt64Vector(FMath::Min(Corner.X, OppositeCorner.X), FMath::Min(Corner.Y, OppositeCorner.Y), FMath::Min(Corner.Z, OppositeCorner.Z));
	Read->High = FInt64Vector(FMath::Max(Corner.X, OppositeCorner.X), FMath::Max(Corner.Y, OppositeCorner.Y), FMath::Max(Corner.Z, OppositeCorner.Z));
	Read->MostChunks = BoxCount(BoxSize(Read->Low, Read->High));
	Read->OnDone = MoveTemp(OnDone);

	if (Read->MostChunks == 0)
	{
		Read->Finish(ECrowdyVoxelResult::Failed, TEXT("The box holds more than 4,913 chunks."));
		return;
	}
	if (!HasAppToken())
	{
		Read->Finish(ECrowdyVoxelResult::NotSignedIn, TEXT("The player has no app token yet."));
		return;
	}
	RequestPage(Read);
}

void UCrowdyVoxels::RequestEditsPage(const TSharedRef<FRead>& Read, const int32 Box)
{
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		Read->Finish(ECrowdyVoxelResult::Failed, TEXT("The Crowdy API client is unavailable, so the request was not sent."));
		return;
	}

	const FEditsBox& Edits = Read->EditsBoxes[Box];
	TWeakObjectPtr<UCrowdyVoxels> WeakThis(this);
	Client->RunOpMsgpack(ECrowdyCppApiDomain::Voxels, TEXT("ListVoxelUpdatesByDistanceCells"), WrapInput(BoxInput(Read->AppId, Edits.Low, Edits.High, Edits.Skip)),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, Read, Box](FCrowdyCppJsonResult Result)
		{
			UCrowdyVoxels* Self = WeakThis.Get();
			if (Read->IsOver())
			{
				return;
			}
			if (!Self || Self->GetAppId() != Read->AppId)
			{
				Read->Finish(ECrowdyVoxelResult::Failed, TEXT("The app changed while the fetch was in flight."));
				return;
			}
			if (!Result.bTransportOk)
			{
				Read->FinishFailure(Result);
				return;
			}

			FEditsBox& Asked = Read->EditsBoxes[Box];
			const int32 Before = Read->Chunks.Num();
			int32 Limit = PageSize;
			const int32 PageChunks = DecodeEditsPage(Result.Msgpack, PageSize, Read->Chunks, Read->Seen, Limit);
			if (PageChunks == INDEX_NONE)
			{
				Read->Finish(ECrowdyVoxelResult::BadReply, TEXT("The answer holds no well-formed list of edits."));
				return;
			}
			for (int32 Index = Before; Index < Read->Chunks.Num(); ++Index)
			{
				const FCrowdyVoxelChunk& Chunk = Read->Chunks[Index];
				if (!InBox(FInt64Vector(Chunk.X, Chunk.Y, Chunk.Z), Asked.Low, Asked.High))
				{
					Read->Finish(ECrowdyVoxelResult::BadReply, TEXT("The platform answered edits outside the box."));
					return;
				}
			}
			Asked.Skip += Limit;
			if (PageChunks == Limit && Asked.Skip < Asked.MostChunks)
			{
				Self->RequestEditsPage(Read, Box);
				return;
			}
			if (--Read->EditsLeft == 0)
			{
				Read->Finish(ECrowdyVoxelResult::Ok, FString());
			}
		}));
}

void UCrowdyVoxels::FetchChunk(int64 ChunkX, int64 ChunkY, int64 ChunkZ, TFunction<void(const FCrowdyChunkFetch&)> OnDone)
{
	if (!HasAppToken())
	{
		FinishFetch(OnDone, ECrowdyVoxelResult::NotSignedIn, TEXT("The player has no app token yet."), {});
		return;
	}
	FCrowdyCppClient* Client = ResolveClient();
	if (!Client)
	{
		FinishFetch(OnDone, ECrowdyVoxelResult::Failed, TEXT("The Crowdy API client is unavailable, so the request was not sent."), {});
		return;
	}
	const int64 AppId = GetAppId();
	const FInt64Vector Asked(ChunkX, ChunkY, ChunkZ);
	TWeakObjectPtr<UCrowdyVoxels> WeakThis(this);
	Client->RunOpMsgpack(ECrowdyCppApiDomain::Chunks, TEXT("GetChunk"), WrapInput(ChunkInput(AppId, ChunkX, ChunkY, ChunkZ, TEXT("coordinates"))),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, AppId, Asked, OnDone = MoveTemp(OnDone)](FCrowdyCppJsonResult Result)
		{
			const UCrowdyVoxels* Self = WeakThis.Get();
			if (!Self || Self->GetAppId() != AppId)
			{
				FinishFetch(OnDone, ECrowdyVoxelResult::Failed, TEXT("The app changed while the fetch was in flight."), {});
				return;
			}
			if (!Result.bTransportOk)
			{
				FinishFetchFailure(OnDone, Result);
				return;
			}
			TArray<FCrowdyVoxelChunk> Chunks;
			FCrowdyVoxelChunk& Chunk = Chunks.AddDefaulted_GetRef();
			const int32 Found = DecodeOne(Result.Msgpack, Chunk);
			if (Found == INDEX_NONE)
			{
				FinishFetch(OnDone, ECrowdyVoxelResult::BadReply, TEXT("The answer holds no well-formed chunk."), {});
				return;
			}
			if (Found == 0)
			{
				Chunk.X = Asked.X;
				Chunk.Y = Asked.Y;
				Chunk.Z = Asked.Z;
				Chunk.Grid.SetNumZeroed(VoxelCount);
			}
			if (FInt64Vector(Chunk.X, Chunk.Y, Chunk.Z) != Asked)
			{
				FinishFetch(OnDone, ECrowdyVoxelResult::BadReply, TEXT("The platform answered a different chunk than the one asked for."), {});
				return;
			}
			FinishFetch(OnDone, ECrowdyVoxelResult::Ok, FString(), MoveTemp(Chunks));
		}));
}
