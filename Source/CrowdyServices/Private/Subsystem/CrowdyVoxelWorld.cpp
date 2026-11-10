#include "Subsystem/CrowdyVoxelWorld.h"

#include "Algo/BinarySearch.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "CrowdyCppReplication.h"
#include "CrowdyServicesLog.h"
#include "CrowdyVoxelDecode.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/Voxel/FVoxelStateUpdateRequest.h"
#include "Messages/Voxel/FVoxelUpdateNotificationMessage.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyVoxelLibrary.h"

namespace CrowdyVoxelWorldImpl
{
	constexpr int32 MaxStateBytes = 1024;
	constexpr int32 MaxHeldEdits = 4096;
	constexpr int32 MaxEditsPerChunk = 65536;
	constexpr int32 MaxSingleFetches = 32;
	constexpr double MaxSendBurst = 20.0;
	constexpr double RetrySeconds = 2.0;

	// An edit time further ahead of this machine's clock than this is applied but never used to order a cell.
	constexpr int64 MaxEpochAheadMs = 60 * 1000;

	bool FitsInt16(const int32 Value)
	{
		return Value >= MIN_int16 && Value <= MAX_int16;
	}

	bool SameVoxel(const FCrowdyVoxel& A, const FCrowdyVoxel& B)
	{
		return A.Type == B.Type && A.State == B.State;
	}

	// Exact for any two int64, which a plain subtraction is not: Abs of an overflowed difference can come out negative.
	int64 AxisDistance(const int64 A, const int64 B)
	{
		const uint64 Distance = A > B ? static_cast<uint64>(A) - static_cast<uint64>(B) : static_cast<uint64>(B) - static_cast<uint64>(A);
		return Distance > static_cast<uint64>(MAX_int64) ? MAX_int64 : static_cast<int64>(Distance);
	}

	int64 Chebyshev(const FInt64Vector& A, const FInt64Vector& B)
	{
		return FMath::Max3(AxisDistance(A.X, B.X), AxisDistance(A.Y, B.Y), AxisDistance(A.Z, B.Z));
	}

	FCrowdyVoxelChunk EmptyChunk(const FInt64Vector& Coord)
	{
		FCrowdyVoxelChunk Chunk;
		Chunk.X = Coord.X;
		Chunk.Y = Coord.Y;
		Chunk.Z = Coord.Z;
		Chunk.Grid.SetNumZeroed(CrowdyVoxelDecode::VoxelCount);
		return Chunk;
	}

	/** Sets one voxel as the fetch decode would have. False when the chunk already holds the most edits a chunk can. */
	bool Upsert(FCrowdyVoxelChunk& Chunk, const FCrowdyVoxel& Voxel)
	{
		const int32 Index = CrowdyVoxelDecode::GridIndex(Voxel.Position);
		if (Index != INDEX_NONE && Index < Chunk.Grid.Num())
		{
			Chunk.Grid[Index] = Voxel.Type >= 0 && Voxel.Type <= MAX_uint8 ? static_cast<uint8>(Voxel.Type) : 0;
		}
		const uint64 Key = CrowdyVoxelDecode::PositionKey(Voxel.Position);
		const int32 At = Algo::LowerBoundBy(Chunk.Edits, Key, [](const FCrowdyVoxel& Edit) { return CrowdyVoxelDecode::PositionKey(Edit.Position); });
		if (Chunk.Edits.IsValidIndex(At) && CrowdyVoxelDecode::PositionKey(Chunk.Edits[At].Position) == Key)
		{
			Chunk.Edits[At] = Voxel;
			return true;
		}
		if (Chunk.Edits.Num() >= MaxEditsPerChunk)
		{
			return false;
		}
		Chunk.Edits.Insert(Voxel, At);
		return true;
	}

	FInt64Vector ChunkOf(const FCrowdyVoxelWrite& Write)
	{
		return FInt64Vector(Write.ChunkX, Write.ChunkY, Write.ChunkZ);
	}

	FIntVector CellOf(const int32 Index)
	{
		constexpr int32 Axis = CrowdyVoxelDecode::CellsPerAxis;
		return FIntVector(Index % Axis, (Index / Axis) % Axis, Index / (Axis * Axis));
	}
}

using namespace CrowdyVoxelWorldImpl;

bool UCrowdyVoxelWorld::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UCrowdyVoxelWorld::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The game world exists before a game instance owns it at engine start.
	UGameInstance* Instance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	if (!Instance)
	{
		return;
	}

	Session = Instance->GetSubsystem<UCrowdyGameSession>();
	Udp = Instance->GetSubsystem<UCrowdyUDPSubsystem>();
	Voxels = Instance->GetSubsystem<UCrowdyVoxels>();
	Replication = Instance->GetSubsystem<UCrowdyCppReplicationSubsystem>();
	if (Replication.IsValid())
	{
		TrackedSendHandle = Replication->OnTrackedSend.AddUObject(this, &UCrowdyVoxelWorld::HandleTrackedSend);
	}

	const UCrowdySDKBridgeSubsystem* Bridge = Instance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	if (!Bridge || !Bridge->ServiceRegistry)
	{
		return;
	}

	FCrowdySubscriptionOptions Options;
	Options.Role = ECrowdySubscriptionRole::Observe;
	Options.SubscriberName = TEXT("CrowdyVoxelWorld");
	TWeakObjectPtr<UCrowdyVoxelWorld> WeakThis(this);
	NotificationSubscription = Bridge->ServiceRegistry->SubscribeToOpcode<FVoxelUpdateNotificationMessage>(
		ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION, Options,
		[WeakThis](const FVoxelUpdateNotificationMessage& Notification, const FCrowdyDelivery&)
		{
			UCrowdyVoxelWorld* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			// A state that did not arrive whole makes the edit unusable.
			const bool bStateWhole = Notification.StateSize == 0 || (Notification.bContainsState && Notification.StateBytes.Num() == Notification.StateSize);
			if (!bStateWhole)
			{
				return;
			}
			FLiveEdit Edit;
			Edit.Chunk = FInt64Vector(Notification.ChunkX, Notification.ChunkY, Notification.ChunkZ);
			Edit.Voxel.Position = FIntVector(Notification.Vx, Notification.Vy, Notification.Vz);
			Edit.Voxel.Type = Notification.VoxelType;
			Edit.Voxel.State = Notification.StateBytes;
			Edit.EpochMillis = Notification.Timestamp;
			Edit.Sequence = Notification.SequenceNumber;
			Edit.bOwnActor = Self->OwnActorId.IsSet() && Notification.UUID == Self->OwnActorId;
			Self->HandleLiveEdit(Edit);
		});
}

void UCrowdyVoxelWorld::Deinitialize()
{
	// Waiting edits end with the world and report nothing: their listeners leave with the level.
	NotificationSubscription.Release();
	if (Replication.IsValid())
	{
		Replication->OnTrackedSend.Remove(TrackedSendHandle);
	}
	Chunks.Reset();
	Pending.Reset();
	EarlyDone.Reset();
	Followers.Reset();
	Super::Deinitialize();
}

TStatId UCrowdyVoxelWorld::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdyVoxelWorld, STATGROUP_Tickables);
}

void UCrowdyVoxelWorld::Tick(const float DeltaTime)
{
	Super::Tick(DeltaTime);
	Step();
}

void UCrowdyVoxelWorld::Step()
{
	UpdateConnection();
	UpdateActorId();
	UpdateFollowers();
	UpdateReach();
	SendDue();
	ResolveAnswers();
	IssueSettles();
	BroadcastChanges();
}

double UCrowdyVoxelWorld::Now() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (bUseTestSeams)
	{
		return NowForTest;
	}
#endif
	return FPlatformTime::Seconds();
}

bool UCrowdyVoxelWorld::IsConnected() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (bUseTestSeams)
	{
		return bConnectedForTest;
	}
#endif
	return Udp.IsValid() && Udp->GetConnectionState() == EUDPConnectionState::Connected
		&& Session.IsValid() && !Session->GetGameToken().IsEmpty();
}

bool UCrowdyVoxelWorld::HasActorId() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (bUseTestSeams)
	{
		return true;
	}
#endif
	return OwnActorId.IsSet();
}

bool UCrowdyVoxelWorld::HasLocalChunk(FInt64Vector& OutChunk) const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (bUseTestSeams)
	{
		OutChunk = LocalChunkForTest;
		return bHasLocalChunkForTest;
	}
#endif
	return Session.IsValid() && Session->TryGetLocalPlayerChunk(GetWorld(), OutChunk);
}

uint64 UCrowdyVoxelWorld::Send(const FCrowdyVoxelWrite& Write, FString& OutError)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (bUseTestSeams)
	{
		return SendForTest ? SendForTest(Write, OutError) : 0;
	}
#endif
	if (!Replication.IsValid() || !Session.IsValid())
	{
		OutError = TEXT("there is no realtime connection");
		return 0;
	}

	FVoxelStateUpdateRequest Message;
	Message.UUID = OwnActorId;
	Message.AppID = Session->GetAppID();
	Message.ChunkX = Write.ChunkX;
	Message.ChunkY = Write.ChunkY;
	Message.ChunkZ = Write.ChunkZ;
	Message.Vx = static_cast<int16>(Write.Voxel.Position.X);
	Message.Vy = static_cast<int16>(Write.Voxel.Position.Y);
	Message.Vz = static_cast<int16>(Write.Voxel.Position.Z);
	Message.VoxelType = static_cast<int16>(Write.Voxel.Type);
	Message.StateBytes = Write.Voxel.State;
	Message.StateSize = static_cast<uint16>(Write.Voxel.State.Num());
	Message.bContainsState = Write.Voxel.State.Num() > 0;
	Message.bContainsAuth = true;
	Message.DecayRate = ECrowdyDecayRate::No_Decay;
	Message.ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks;

	const uint64 Ticket = Replication->NewSendTicket();
	switch (Replication->TrySendMessage(Message, Ticket))
	{
	case ECrowdyCppSendOutcome::Sent:
		return Ticket;
	case ECrowdyCppSendOutcome::NotRouted:
		OutError = TEXT("there is no realtime connection");
		return 0;
	default:
		OutError = TEXT("the realtime connection did not take the edit");
		return 0;
	}
}

void UCrowdyVoxelWorld::Fetch(const int64 X, const int64 Y, const int64 Z, const int32 Range, TFunction<void(const FCrowdyChunkFetch&)> OnDone)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (bUseTestSeams)
	{
		if (FetchForTest)
		{
			FetchForTest(X, Y, Z, Range, MoveTemp(OnDone));
		}
		return;
	}
#endif
	if (!Voxels.IsValid())
	{
		FCrowdyChunkFetch Failed;
		Failed.Message = TEXT("the voxel fetch is not available");
		OnDone(Failed);
		return;
	}
	if (Range == 0)
	{
		Voxels->FetchChunk(X, Y, Z, MoveTemp(OnDone));
		return;
	}
	Voxels->FetchChunksAround(X, Y, Z, Range, MoveTemp(OnDone));
}

void UCrowdyVoxelWorld::Follow(AActor* Actor, const int32 Range)
{
	if (!IsValid(Actor))
	{
		return;
	}
	const int32 Clamped = FMath::Clamp(Range, 1, LiveReach - 1);
	FFollower* Existing = Followers.FindByPredicate([Actor](const FFollower& Follower) { return Follower.Actor.Get() == Actor; });
	if (Existing)
	{
		Existing->Range = Clamped;
	}
	else
	{
		FFollower& Added = Followers.AddDefaulted_GetRef();
		Added.Actor = Actor;
		Added.Range = Clamped;
	}
	bFollowDirty = true;
}

void UCrowdyVoxelWorld::StopFollowing(AActor* Actor)
{
	Followers.RemoveAll([Actor](const FFollower& Follower) { return Follower.Actor.Get() == Actor; });
	bFollowDirty = true;
}

bool UCrowdyVoxelWorld::IsChunkLoaded(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ) const
{
	const FChunkState* State = Chunks.Find(FInt64Vector(ChunkX, ChunkY, ChunkZ));
	return State && State->bLoaded;
}

bool UCrowdyVoxelWorld::IsChunkLive(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ) const
{
	const FChunkState* State = Chunks.Find(FInt64Vector(ChunkX, ChunkY, ChunkZ));
	return State && State->bLoaded && State->bInReach && bSettleDone && State->FetchedAt >= SettleAt;
}

bool UCrowdyVoxelWorld::IsChunkInReach(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ) const
{
	return bLocalKnown && Chebyshev(FInt64Vector(ChunkX, ChunkY, ChunkZ), LocalChunk) <= LiveReach;
}

const UCrowdyVoxelWorld::FPendingEdit* UCrowdyVoxelWorld::NewestPending(const FInt64Vector& Coord, const uint64 Key) const
{
	for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
	{
		const FPendingEdit& Edit = Pending[Index];
		if (Edit.Key == Key && Edit.Chunk == Coord)
		{
			return &Edit;
		}
	}
	return nullptr;
}

FCrowdyVoxel UCrowdyVoxelWorld::SeenVoxel(const FInt64Vector& Coord, const FChunkState& State, const FIntVector& Position) const
{
	if (PendingPerChunk.FindRef(Coord) > 0)
	{
		if (const FPendingEdit* Edit = NewestPending(Coord, CrowdyVoxelDecode::PositionKey(Position)))
		{
			return Edit->Write.Voxel;
		}
	}
	return UCrowdyVoxelLibrary::GetVoxel(State.Confirmed, Position);
}

FCrowdyVoxel UCrowdyVoxelWorld::GetVoxel(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ, const FIntVector Position) const
{
	const FInt64Vector Coord(ChunkX, ChunkY, ChunkZ);
	const FChunkState* State = Chunks.Find(Coord);
	if (!State || !State->bLoaded)
	{
		FCrowdyVoxel Empty;
		Empty.Position = Position;
		return Empty;
	}
	return SeenVoxel(Coord, *State, Position);
}

bool UCrowdyVoxelWorld::FindChunk(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ, FCrowdyVoxelChunk& Chunk) const
{
	const FInt64Vector Coord(ChunkX, ChunkY, ChunkZ);
	const FChunkState* State = Chunks.Find(Coord);
	if (!State || !State->bLoaded)
	{
		return false;
	}
	Chunk = State->Confirmed;
	if (PendingPerChunk.FindRef(Coord) == 0)
	{
		return true;
	}
	// Oldest first, so the newest edit per cell is the one left standing.
	for (const FPendingEdit& Edit : Pending)
	{
		if (Edit.Chunk == Coord)
		{
			Upsert(Chunk, Edit.Write.Voxel);
		}
	}
	return true;
}

void UCrowdyVoxelWorld::ForEachLoadedChunk(const TFunctionRef<void(int64 ChunkX, int64 ChunkY, int64 ChunkZ)> Visit) const
{
	for (const TPair<FInt64Vector, FChunkState>& Entry : Chunks)
	{
		if (Entry.Value.bLoaded)
		{
			Visit(Entry.Key.X, Entry.Key.Y, Entry.Key.Z);
		}
	}
}

void UCrowdyVoxelWorld::ForEachFilledVoxel(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
	const TFunctionRef<void(FIntVector Position, int32 Type)> Visit) const
{
	const FInt64Vector Coord(ChunkX, ChunkY, ChunkZ);
	const FChunkState* State = Chunks.Find(Coord);
	if (!State || !State->bLoaded || State->Confirmed.Grid.Num() != CrowdyVoxelDecode::VoxelCount)
	{
		return;
	}

	// The few cells this player's waiting edits cover, resolved once instead of per cell.
	TMap<int32, int32, TInlineSetAllocator<16>> Overlay;
	if (PendingPerChunk.FindRef(Coord) > 0)
	{
		for (const FPendingEdit& Edit : Pending)
		{
			const int32 Index = Edit.Chunk == Coord ? CrowdyVoxelDecode::GridIndex(Edit.Write.Voxel.Position) : INDEX_NONE;
			if (Index != INDEX_NONE)
			{
				const int32 Type = Edit.Write.Voxel.Type;
				Overlay.Add(Index, Type >= 0 && Type <= MAX_uint8 ? Type : 0);
			}
		}
	}

	const uint8* Grid = State->Confirmed.Grid.GetData();
	for (int32 Index = 0; Index < CrowdyVoxelDecode::VoxelCount; ++Index)
	{
		const int32* Over = Overlay.Num() > 0 ? Overlay.Find(Index) : nullptr;
		const int32 Type = Over ? *Over : Grid[Index];
		if (Type != 0)
		{
			Visit(CellOf(Index), Type);
		}
	}
}

int64 UCrowdyVoxelWorld::SetVoxel(const FCrowdyVoxelWrite& Write)
{
	const int64 EditId = NextEditId++;
	const FInt64Vector Coord = ChunkOf(Write);
	const FIntVector& Position = Write.Voxel.Position;
	FChunkState* State = Chunks.Find(Coord);

	FPendingEdit Edit;
	Edit.EditId = EditId;
	Edit.Write = Write;
	Edit.Chunk = Coord;
	Edit.Key = CrowdyVoxelDecode::PositionKey(Position);

	const bool bFits = FitsInt16(Position.X) && FitsInt16(Position.Y) && FitsInt16(Position.Z) && FitsInt16(Write.Voxel.Type);
	if (!State || !State->bLoaded || !bFits || Write.Voxel.State.Num() > MaxStateBytes)
	{
		Edit.Result = ECrowdyVoxelResult::Failed;
		Edit.Message = !State || !State->bLoaded ? TEXT("the chunk is not loaded")
			: !bFits ? TEXT("a position or type is wider than 16 bits")
			: TEXT("the state is over 1,024 bytes");
		EarlyDone.Add(MoveTemp(Edit));
		return EditId;
	}
	if (!IsConnected() || !HasActorId())
	{
		Edit.Result = ECrowdyVoxelResult::NotSignedIn;
		Edit.Message = IsConnected() ? TEXT("this player has no realtime actor yet") : TEXT("there is no realtime connection");
		EarlyDone.Add(MoveTemp(Edit));
		return EditId;
	}

	Pending.Add(MoveTemp(Edit));
	++PendingPerChunk.FindOrAdd(Coord);
	// Named even when the value does not change, so the broadcast that shows the edit always comes before its answer.
	MarkChanged(Coord, *State, Position);
	return EditId;
}

TArray<int64> UCrowdyVoxelWorld::SetVoxels(const TArray<FCrowdyVoxelWrite>& Writes)
{
	TArray<int64> Ids;
	Ids.Reserve(Writes.Num());
	for (const FCrowdyVoxelWrite& Write : Writes)
	{
		Ids.Add(SetVoxel(Write));
	}
	return Ids;
}

void UCrowdyVoxelWorld::MarkChanged(const FInt64Vector& Coord, FChunkState& State, const FIntVector& Position)
{
	bool bAlready = false;
	State.ChangedKeys.Add(CrowdyVoxelDecode::PositionKey(Position), &bAlready);
	if (bAlready)
	{
		return;
	}
	State.Changed.Add(Position);
	if (State.Changed.Num() == 1)
	{
		DirtyChunks.Add(Coord);
	}
}

void UCrowdyVoxelWorld::HandleLiveEdit(const FLiveEdit& Edit)
{
	if (Edit.Voxel.State.Num() > MaxStateBytes)
	{
		return;
	}

	bool bConfirmedOwn = false;
	if (Edit.bOwnActor)
	{
		FPendingEdit* Answered = Pending.FindByPredicate([&Edit](const FPendingEdit& Waiting)
		{
			return !Waiting.bAnswered && Waiting.bSequenceKnown && Waiting.Sequence == Edit.Sequence && Waiting.Chunk == Edit.Chunk
				&& Waiting.Write.Voxel.Position == Edit.Voxel.Position && SameVoxel(Waiting.Write.Voxel, Edit.Voxel);
		});
		if (Answered)
		{
			Answered->bAnswered = true;
			Answered->Result = ECrowdyVoxelResult::Ok;
			bConfirmedOwn = true;
		}
	}

	FChunkState* State = Chunks.Find(Edit.Chunk);
	if (!State)
	{
		return;
	}
	if (State->FetchRequest != 0)
	{
		if (State->HeldDuringFetch.Num() < MaxHeldEdits)
		{
			State->HeldDuringFetch.Add(Edit);
		}
		else
		{
			State->bHeldOverflow = true;
		}
	}
	// An own edit the platform confirmed but a newer-looking time outranks is settled by asking the platform.
	if (State->bLoaded && !ApplyLive(Edit.Chunk, *State, Edit) && bConfirmedOwn)
	{
		RequestSettle(Edit.Chunk, Edit.Voxel.Position);
	}
}

bool UCrowdyVoxelWorld::ApplyLive(const FInt64Vector& Coord, FChunkState& State, const FLiveEdit& Edit)
{
	const uint64 Key = CrowdyVoxelDecode::PositionKey(Edit.Voxel.Position);
	if (const int64* Time = State.CellTimes.Find(Key); Time && *Time > Edit.EpochMillis)
	{
		return false;
	}
	const FCrowdyVoxel Before = SeenVoxel(Coord, State, Edit.Voxel.Position);
	if (!Upsert(State.Confirmed, Edit.Voxel))
	{
		return false;
	}
	// A time far ahead of this machine's clock is not trusted to order the cell, so it cannot pin it.
	const int64 NowMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000;
	if (Edit.EpochMillis <= NowMs + MaxEpochAheadMs)
	{
		State.CellTimes.Add(Key, Edit.EpochMillis);
	}
	else
	{
		State.CellTimes.Remove(Key);
	}
	if (!SameVoxel(Before, SeenVoxel(Coord, State, Edit.Voxel.Position)))
	{
		MarkChanged(Coord, State, Edit.Voxel.Position);
	}
	return true;
}

void UCrowdyVoxelWorld::HandleTrackedSend(const FCrowdyTrackedSend& Event)
{
	FPendingEdit* Edit = Pending.FindByPredicate([&Event](const FPendingEdit& Waiting) { return Waiting.Ticket == Event.Ticket; });
	if (!Edit || Edit->bAnswered)
	{
		return;
	}
	switch (Event.Event)
	{
	case ECrowdyTrackedSendEvent::Sent:
		Edit->Sequence = Event.Sequence;
		Edit->bSequenceKnown = true;
		return;
	case ECrowdyTrackedSendEvent::Refused:
		Edit->bAnswered = true;
		Edit->Result = ECrowdyVoxelResult::Refused;
		Edit->Message = FString::Printf(TEXT("the platform refused the edit (%s)"), *FCrowdyCppReplication::DescribeErrorCode(Event.ErrorCode));
		return;
	case ECrowdyTrackedSendEvent::Lapped:
		Edit->bAnswered = true;
		Edit->Result = ECrowdyVoxelResult::NoAnswer;
		Edit->Message = TEXT("the sequence number came round before an answer");
		return;
	case ECrowdyTrackedSendEvent::Lost:
		Edit->bAnswered = true;
		Edit->Result = IsConnected() ? ECrowdyVoxelResult::NoAnswer : ECrowdyVoxelResult::NotSignedIn;
		Edit->Message = TEXT("the edit never reached the network");
		return;
	}
}

void UCrowdyVoxelWorld::UpdateConnection()
{
	const bool bConnected = IsConnected();
	if (bConnected && !bSettleDone && SettleAt >= 0.0 && Now() >= SettleAt)
	{
		bSettleDone = true;
		Reload(true);
	}
	if (bConnected == bWasConnected)
	{
		return;
	}
	bWasConnected = bConnected;
	if (bConnected)
	{
		SettleAt = Now() + SettleSeconds;
		bSettleDone = false;
		bFollowDirty = true;
		return;
	}
	for (FPendingEdit& Edit : Pending)
	{
		if (Edit.bAnswered)
		{
			continue;
		}
		// Without a receipt the datagram never reached the network, so the edit cannot have been applied.
		Edit.bAnswered = true;
		Edit.Result = Edit.bSequenceKnown ? ECrowdyVoxelResult::NoAnswer : ECrowdyVoxelResult::NotSignedIn;
		Edit.Message = TEXT("the realtime connection dropped");
	}
	SettleAt = -1.0;
	bSettleDone = false;
}

void UCrowdyVoxelWorld::UpdateActorId()
{
#if WITH_DEV_AUTOMATION_TESTS
	if (bUseTestSeams)
	{
		return;
	}
#endif
	if (!Session.IsValid())
	{
		return;
	}
	const FString& Uuid = Session->GetUUID();
	if (Uuid == OwnActorUuid)
	{
		return;
	}
	OwnActorUuid = Uuid;
	OwnActorId = FCrowdyActorId::FromStringOrUnset(Uuid);
	// Other servers learn of this player's actor from its first updates, so the settling refetch counts from here.
	if (OwnActorId.IsSet() && bWasConnected)
	{
		SettleAt = Now() + SettleSeconds;
		bSettleDone = false;
	}
}

void UCrowdyVoxelWorld::UpdateFollowers()
{
	if (!bWasConnected || Now() < RetryAt)
	{
		return;
	}

	Followers.RemoveAll([this](const FFollower& Follower)
	{
		const bool bGone = !Follower.Actor.IsValid();
		bFollowDirty |= bGone;
		return bGone;
	});
	for (FFollower& Follower : Followers)
	{
		int64 X = 0;
		int64 Y = 0;
		int64 Z = 0;
		FIntVector Cell;
		UCrowdyVoxelLibrary::WorldToVoxel(this, Follower.Actor->GetActorLocation(), X, Y, Z, Cell);
		const FInt64Vector Chunk(X, Y, Z);
		if (!Follower.bHasChunk || Follower.Chunk != Chunk)
		{
			Follower.Chunk = Chunk;
			Follower.bHasChunk = true;
			bFollowDirty = true;
		}
	}
	if (!bFollowDirty)
	{
		return;
	}
	bFollowDirty = false;

	TArray<FInt64Vector> Unload;
	for (const TPair<FInt64Vector, FChunkState>& Entry : Chunks)
	{
		const bool bKept = Followers.ContainsByPredicate([&Entry](const FFollower& Follower)
		{
			return Chebyshev(Entry.Key, Follower.Chunk) <= Follower.Range + 1;
		});
		if (!bKept)
		{
			Unload.Add(Entry.Key);
		}
	}
	for (const FInt64Vector& Coord : Unload)
	{
		if (Chunks.FindChecked(Coord).bLoaded)
		{
			UnloadedEvents.Add(Coord);
		}
		Chunks.Remove(Coord);
	}

	TArray<FInt64Vector> Missing;
	TSet<FInt64Vector> Seen;
	for (const FFollower& Follower : Followers)
	{
		const int32 R = Follower.Range;
		int32 MissingHere = 0;
		for (int32 DZ = -R; DZ <= R; ++DZ)
		{
			for (int32 DY = -R; DY <= R; ++DY)
			{
				for (int32 DX = -R; DX <= R; ++DX)
				{
					const FInt64Vector Coord = Follower.Chunk + FInt64Vector(DX, DY, DZ);
					if (Chunks.Contains(Coord))
					{
						continue;
					}
					++MissingHere;
					bool bAlready = false;
					Seen.Add(Coord, &bAlready);
					if (!bAlready)
					{
						Missing.Add(Coord);
					}
				}
			}
		}
		// A follower with nothing loaded yet reads its whole cube in one request chain.
		const int32 Side = 2 * R + 1;
		if (MissingHere == Side * Side * Side)
		{
			RequestCube(Follower.Chunk, R, false);
		}
	}
	Missing.RemoveAll([this](const FInt64Vector& Coord) { return Chunks.Contains(Coord); });
	if (Missing.Num() <= MaxSingleFetches)
	{
		RequestChunks(Missing, true);
		return;
	}
	// A slab of new chunks is covered by range-1 cubes on a fixed grid of 3, each one page, never by re-reading a
	// follower's whole cube; only the missing chunks in each are loaded.
	const TSet<FInt64Vector> Wanted(Missing);
	TSet<FInt64Vector> Centres;
	const auto Snap = [](const int64 Axis) { return 3 * FMath::FloorToInt64(Axis / 3.0) + 1; };
	for (const FInt64Vector& Coord : Missing)
	{
		Centres.Add(FInt64Vector(Snap(Coord.X), Snap(Coord.Y), Snap(Coord.Z)));
	}
	for (const FInt64Vector& Centre : Centres)
	{
		RequestCube(Centre, 1, false, &Wanted);
	}
}

void UCrowdyVoxelWorld::UpdateReach()
{
	FInt64Vector Local;
	// Without a pawn the last known chunk stands, so a death camera does not drop every chunk out of reach.
	if (!HasLocalChunk(Local) || (bLocalKnown && Local == LocalChunk))
	{
		return;
	}
	const bool bWasKnown = bLocalKnown;
	bLocalKnown = true;
	LocalChunk = Local;

	TArray<FInt64Vector> Returned;
	for (TPair<FInt64Vector, FChunkState>& Entry : Chunks)
	{
		const bool bInReach = Chebyshev(Entry.Key, Local) <= LiveReach;
		// Chunks loaded before this player had an actor are settled by the settling refetch instead.
		if (bWasKnown && bInReach && !Entry.Value.bInReach && Entry.Value.bLoaded)
		{
			Returned.Add(Entry.Key);
		}
		Entry.Value.bInReach = bInReach;
	}
	if (Returned.Num() == 0)
	{
		return;
	}
	if (Returned.Num() <= MaxSingleFetches)
	{
		RequestChunks(Returned, true);
		return;
	}
	Reload(false);
}

void UCrowdyVoxelWorld::Reload(const bool bWholeBand)
{
	// The band past each follower's range holds loaded chunks too, so a settle reaches one chunk further.
	for (const FFollower& Follower : Followers)
	{
		if (Follower.bHasChunk)
		{
			RequestCube(Follower.Chunk, bWholeBand ? Follower.Range + 1 : Follower.Range, true);
		}
	}
}

void UCrowdyVoxelWorld::RequestChunks(const TArray<FInt64Vector>& Coords, const bool bFull)
{
	for (const FInt64Vector& Coord : Coords)
	{
		FChunkState& State = Chunks.FindOrAdd(Coord);
		if (State.FetchRequest != 0)
		{
			State.bFetchAgain = true;
			State.bFetchAgainFull |= bFull;
			continue;
		}
		const int32 Request = NextFetchRequest++;
		State.FetchRequest = Request;
		State.FetchIssuedAt = Now();
		State.bFetchFull = bFull || !State.bLoaded;
		State.SettleInFlight = MoveTemp(State.SettlePositions);
		State.HeldDuringFetch.Reset();
		State.bHeldOverflow = false;
		// The answer can come back before Fetch returns, so nothing touches State after this call.
		TWeakObjectPtr<UCrowdyVoxelWorld> WeakThis(this);
		Fetch(Coord.X, Coord.Y, Coord.Z, 0, [WeakThis, Coord, Request](const FCrowdyChunkFetch& Result)
		{
			if (UCrowdyVoxelWorld* Self = WeakThis.Get())
			{
				Self->OnFetched(Coord, Request, Result);
			}
		});
	}
}

void UCrowdyVoxelWorld::RequestCube(const FInt64Vector& Centre, const int32 Range, const bool bReload, const TSet<FInt64Vector>* Only)
{
	const int32 Request = NextFetchRequest++;
	int32 Marked = 0;
	for (int32 DZ = -Range; DZ <= Range; ++DZ)
	{
		for (int32 DY = -Range; DY <= Range; ++DY)
		{
			for (int32 DX = -Range; DX <= Range; ++DX)
			{
				const FInt64Vector Coord = Centre + FInt64Vector(DX, DY, DZ);
				if (Only && !Only->Contains(Coord))
				{
					continue;
				}
				FChunkState* State = Chunks.Find(Coord);
				if (!State && bReload)
				{
					continue;
				}
				if (State && State->bLoaded && !bReload)
				{
					continue;
				}
				if (!State)
				{
					State = &Chunks.Add(Coord);
				}
				if (State->FetchRequest != 0)
				{
					State->bFetchAgain |= bReload;
					State->bFetchAgainFull |= bReload;
					continue;
				}
				State->FetchRequest = Request;
				State->FetchIssuedAt = Now();
				State->bFetchFull = true;
				State->SettleInFlight = MoveTemp(State->SettlePositions);
				State->HeldDuringFetch.Reset();
				State->bHeldOverflow = false;
				++Marked;
			}
		}
	}
	if (Marked == 0)
	{
		return;
	}
	TWeakObjectPtr<UCrowdyVoxelWorld> WeakThis(this);
	Fetch(Centre.X, Centre.Y, Centre.Z, Range, [WeakThis, Centre, Range, Request](const FCrowdyChunkFetch& Result)
	{
		if (UCrowdyVoxelWorld* Self = WeakThis.Get())
		{
			Self->OnCubeFetched(Centre, Range, Request, Result);
		}
	});
}

void UCrowdyVoxelWorld::OnFetched(const FInt64Vector& Coord, const int32 Request, const FCrowdyChunkFetch& Fetch)
{
	FChunkState* State = Chunks.Find(Coord);
	if (!State || State->FetchRequest != Request)
	{
		return;
	}
	if (Fetch.Result == ECrowdyVoxelResult::Ok && Fetch.Chunks.Num() == 1)
	{
		Adopt(Coord, &Fetch.Chunks[0]);
		return;
	}
	UE_LOG(LogCrowdyServices, Warning, TEXT("Voxel world: fetching chunk %lld %lld %lld ended %s."), Coord.X, Coord.Y, Coord.Z,
		*UEnum::GetValueAsString(Fetch.Result));
	State->FetchRequest = 0;
	RetryAt = Now() + RetrySeconds;
	if (!State->bLoaded)
	{
		Chunks.Remove(Coord);
		bFollowDirty = true;
		return;
	}
	// The cells it was to settle wait for the next settle round.
	State->SettlePositions.Append(MoveTemp(State->SettleInFlight));
	if (State->SettlePositions.Num() > 0)
	{
		SettleQueue.AddUnique(Coord);
	}
}

void UCrowdyVoxelWorld::OnCubeFetched(const FInt64Vector& Centre, const int32 Range, const int32 Request, const FCrowdyChunkFetch& Fetch)
{
	const bool bOk = Fetch.Result == ECrowdyVoxelResult::Ok;
	UE_CLOG(!bOk, LogCrowdyServices, Warning, TEXT("Voxel world: fetching the chunks around %lld %lld %lld ended %s."),
		Centre.X, Centre.Y, Centre.Z, *UEnum::GetValueAsString(Fetch.Result));

	TMap<FInt64Vector, const FCrowdyVoxelChunk*> ByCoord;
	for (const FCrowdyVoxelChunk& Chunk : Fetch.Chunks)
	{
		ByCoord.Add(FInt64Vector(Chunk.X, Chunk.Y, Chunk.Z), &Chunk);
	}
	for (int32 DZ = -Range; DZ <= Range; ++DZ)
	{
		for (int32 DY = -Range; DY <= Range; ++DY)
		{
			for (int32 DX = -Range; DX <= Range; ++DX)
			{
				const FInt64Vector Coord = Centre + FInt64Vector(DX, DY, DZ);
				FChunkState* State = Chunks.Find(Coord);
				if (!State || State->FetchRequest != Request)
				{
					continue;
				}
				if (bOk)
				{
					// A chunk the platform does not store is absent from the answer and loads as empty.
					Adopt(Coord, ByCoord.FindRef(Coord));
					continue;
				}
				State->FetchRequest = 0;
				RetryAt = Now() + RetrySeconds;
				if (!State->bLoaded)
				{
					Chunks.Remove(Coord);
					bFollowDirty = true;
					continue;
				}
				State->SettlePositions.Append(MoveTemp(State->SettleInFlight));
				if (State->SettlePositions.Num() > 0)
				{
					SettleQueue.AddUnique(Coord);
				}
			}
		}
	}
}

void UCrowdyVoxelWorld::Adopt(const FInt64Vector& Coord, const FCrowdyVoxelChunk* Fetched)
{
	FChunkState* State = Chunks.Find(Coord);
	if (!State)
	{
		return;
	}

	const FCrowdyVoxelChunk Empty = Fetched ? FCrowdyVoxelChunk() : EmptyChunk(Coord);
	const FCrowdyVoxelChunk& Source = Fetched ? *Fetched : Empty;

	if (!State->bLoaded || State->bFetchFull)
	{
		const bool bWasLoaded = State->bLoaded;
		FCrowdyVoxelChunk Old = MoveTemp(State->Confirmed);
		State->Confirmed = Source;
		if (State->Confirmed.Grid.Num() != CrowdyVoxelDecode::VoxelCount)
		{
			State->Confirmed.Grid.SetNumZeroed(CrowdyVoxelDecode::VoxelCount);
		}
		State->CellTimes.Reset();
		State->bLoaded = true;
		if (!bWasLoaded)
		{
			LoadedEvents.Add(Coord);
		}
		else
		{
			const bool bHasPending = PendingPerChunk.FindRef(Coord) > 0;
			const auto Compare = [this, &Coord, State, &Old, bHasPending](const FIntVector& Position)
			{
				if (bHasPending && NewestPending(Coord, CrowdyVoxelDecode::PositionKey(Position)))
				{
					return;
				}
				if (!SameVoxel(UCrowdyVoxelLibrary::GetVoxel(Old, Position), UCrowdyVoxelLibrary::GetVoxel(State->Confirmed, Position)))
				{
					MarkChanged(Coord, *State, Position);
				}
			};
			for (int32 Index = 0; Index < CrowdyVoxelDecode::VoxelCount; ++Index)
			{
				if (Old.Grid[Index] != State->Confirmed.Grid[Index])
				{
					Compare(CellOf(Index));
				}
			}
			for (const FCrowdyVoxel& Edit : Old.Edits)
			{
				Compare(Edit.Position);
			}
			for (const FCrowdyVoxel& Edit : State->Confirmed.Edits)
			{
				Compare(Edit.Position);
			}
		}
	}
	else
	{
		for (const FIntVector& Position : State->SettleInFlight)
		{
			const FCrowdyVoxel Before = SeenVoxel(Coord, *State, Position);
			Upsert(State->Confirmed, UCrowdyVoxelLibrary::GetVoxel(Source, Position));
			State->CellTimes.Remove(CrowdyVoxelDecode::PositionKey(Position));
			if (!SameVoxel(Before, SeenVoxel(Coord, *State, Position)))
			{
				MarkChanged(Coord, *State, Position);
			}
		}
	}

	State->SettleInFlight.Reset();
	// When the fetch was asked for, not answered: an answer older than the settle does not make a chunk live.
	State->FetchedAt = State->FetchIssuedAt;
	State->FetchRequest = 0;
	State->bFetchFull = false;
	State->bInReach = bLocalKnown && Chebyshev(Coord, LocalChunk) <= LiveReach;

	// Edits that arrived while the fetch was out may be newer than its answer.
	const TArray<FLiveEdit> Held = MoveTemp(State->HeldDuringFetch);
	for (const FLiveEdit& Edit : Held)
	{
		ApplyLive(Coord, *State, Edit);
	}

	const bool bAgain = State->bFetchAgain || State->bHeldOverflow;
	const bool bAgainFull = State->bFetchAgainFull || State->bHeldOverflow;
	State->bFetchAgain = false;
	State->bFetchAgainFull = false;
	State->bHeldOverflow = false;
	if (bAgain)
	{
		RequestChunks({Coord}, bAgainFull);
	}
}

void UCrowdyVoxelWorld::RequestSettle(const FInt64Vector& Coord, const FIntVector& Position)
{
	FChunkState* State = Chunks.Find(Coord);
	if (!State || !State->bLoaded)
	{
		return;
	}
	State->SettlePositions.AddUnique(Position);
	SettleQueue.AddUnique(Coord);
}

void UCrowdyVoxelWorld::IssueSettles()
{
	// Once a frame, so every cell a frame asks to settle in one chunk rides one fetch.
	const TArray<FInt64Vector> Queue = MoveTemp(SettleQueue);
	TArray<FInt64Vector> Ready;
	for (const FInt64Vector& Coord : Queue)
	{
		FChunkState* State = Chunks.Find(Coord);
		// A chunk with a fetch out is asked for again when that answer is adopted (RequestChunks).
		if (!State || !State->bLoaded || State->SettlePositions.Num() == 0)
		{
			continue;
		}
		Ready.Add(Coord);
	}
	if (Ready.Num() > 0 && Now() >= RetryAt)
	{
		RequestChunks(Ready, false);
		return;
	}
	SettleQueue.Append(Ready);
}

void UCrowdyVoxelWorld::SendDue()
{
	const double Time = Now();
	if (LastTickAt >= 0.0)
	{
		SendAllowance = FMath::Min(SendAllowance + (Time - LastTickAt) * EditsPerSecond, MaxSendBurst);
	}
	LastTickAt = Time;

	for (FPendingEdit& Edit : Pending)
	{
		if (SendAllowance < 1.0)
		{
			return;
		}
		if (Edit.bSent || Edit.bAnswered)
		{
			continue;
		}
		SendAllowance -= 1.0;
		FString Error;
		const uint64 Ticket = Send(Edit.Write, Error);
		if (Ticket == 0)
		{
			Edit.bAnswered = true;
			Edit.Result = IsConnected() ? ECrowdyVoxelResult::Failed : ECrowdyVoxelResult::NotSignedIn;
			Edit.Message = Error;
			continue;
		}
		Edit.Ticket = Ticket;
		Edit.bSent = true;
		Edit.SentAt = Time;
	}
}

void UCrowdyVoxelWorld::ResolveAnswers()
{
	const double Time = Now();
	TArray<int64> Ready;
	for (FPendingEdit& Edit : Pending)
	{
		if (!Edit.bAnswered && Edit.bSent && Time - Edit.SentAt > AnswerTimeoutSeconds)
		{
			Edit.bAnswered = true;
			Edit.Result = ECrowdyVoxelResult::NoAnswer;
			Edit.Message = TEXT("no answer within a second");
		}
		if (Edit.bAnswered && Edit.bShown)
		{
			Ready.Add(Edit.EditId);
		}
	}

	const TArray<FPendingEdit> Early = MoveTemp(EarlyDone);
	for (const FPendingEdit& Edit : Early)
	{
		OnEditDone.Broadcast(Edit.EditId, Edit.Write, Edit.Result, Edit.Message);
	}
	if (Ready.Num() == 0)
	{
		return;
	}

	// Copies, because a listener may set voxels and grow the array under them. Every answer is reported before any
	// cell is put back.
	TArray<FPendingEdit> Done;
	Done.Reserve(Ready.Num());
	for (const FPendingEdit& Edit : Pending)
	{
		if (Algo::BinarySearch(Ready, Edit.EditId) != INDEX_NONE)
		{
			Done.Add(Edit);
		}
	}
	for (const FPendingEdit& Edit : Done)
	{
		OnEditDone.Broadcast(Edit.EditId, Edit.Write, Edit.Result, Edit.Message);
	}
	TakeOff(Ready);
	for (const FPendingEdit& Edit : Done)
	{
		if (Edit.Result == ECrowdyVoxelResult::Refused || Edit.Result == ECrowdyVoxelResult::NoAnswer)
		{
			RequestSettle(Edit.Chunk, Edit.Write.Voxel.Position);
		}
	}
}

void UCrowdyVoxelWorld::TakeOff(const TArray<int64>& EditIds)
{
	// What each cell showed with the edits still on, then one pass to remove them all, then what it shows without.
	TArray<TPair<int32, FCrowdyVoxel>> Before;
	Before.Reserve(EditIds.Num());
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		const FPendingEdit& Edit = Pending[Index];
		if (Algo::BinarySearch(EditIds, Edit.EditId) == INDEX_NONE)
		{
			continue;
		}
		const FChunkState* State = Chunks.Find(Edit.Chunk);
		Before.Add({Index, State && State->bLoaded ? SeenVoxel(Edit.Chunk, *State, Edit.Write.Voxel.Position) : FCrowdyVoxel()});
	}
	TArray<TPair<FInt64Vector, FIntVector>> Cells;
	Cells.Reserve(Before.Num());
	for (const TPair<int32, FCrowdyVoxel>& Entry : Before)
	{
		const FPendingEdit& Edit = Pending[Entry.Key];
		Cells.Add({Edit.Chunk, Edit.Write.Voxel.Position});
		int32& Count = PendingPerChunk.FindOrAdd(Edit.Chunk);
		if (--Count <= 0)
		{
			PendingPerChunk.Remove(Edit.Chunk);
		}
	}
	Pending.RemoveAll([&EditIds](const FPendingEdit& Edit) { return Algo::BinarySearch(EditIds, Edit.EditId) != INDEX_NONE; });

	for (int32 Index = 0; Index < Cells.Num(); ++Index)
	{
		FChunkState* State = Chunks.Find(Cells[Index].Key);
		if (State && State->bLoaded && !SameVoxel(Before[Index].Value, SeenVoxel(Cells[Index].Key, *State, Cells[Index].Value)))
		{
			MarkChanged(Cells[Index].Key, *State, Cells[Index].Value);
		}
	}
}

void UCrowdyVoxelWorld::BroadcastChanges()
{
	// Edits a listener sets during these broadcasts are named by the next frame's, not this one.
	const int64 ShownUpTo = NextEditId - 1;

	const TArray<FInt64Vector> Unloaded = MoveTemp(UnloadedEvents);
	for (const FInt64Vector& Coord : Unloaded)
	{
		OnChunkUnloaded.Broadcast(Coord.X, Coord.Y, Coord.Z);
	}
	const TArray<FInt64Vector> Loaded = MoveTemp(LoadedEvents);
	for (const FInt64Vector& Coord : Loaded)
	{
		if (IsChunkLoaded(Coord.X, Coord.Y, Coord.Z))
		{
			OnChunkLoaded.Broadcast(Coord.X, Coord.Y, Coord.Z);
		}
	}

	const TArray<FInt64Vector> Dirty = MoveTemp(DirtyChunks);
	for (const FInt64Vector& Coord : Dirty)
	{
		FChunkState* State = Chunks.Find(Coord);
		if (!State)
		{
			continue;
		}
		const TArray<FIntVector> Positions = MoveTemp(State->Changed);
		State->ChangedKeys.Reset();
		if (State->bLoaded && Positions.Num() > 0)
		{
			OnChunkChanged.Broadcast(Coord.X, Coord.Y, Coord.Z, Positions);
		}
	}

	// These edits have now been named by a broadcast, so their answers may be reported from the next frame on.
	for (FPendingEdit& Edit : Pending)
	{
		Edit.bShown |= Edit.EditId <= ShownUpTo;
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyVoxelWorld::InitializeForTest(FSendFn InSend, FFetchFn InFetch)
{
	bUseTestSeams = true;
	SendForTest = MoveTemp(InSend);
	FetchForTest = MoveTemp(InFetch);
}
#endif
