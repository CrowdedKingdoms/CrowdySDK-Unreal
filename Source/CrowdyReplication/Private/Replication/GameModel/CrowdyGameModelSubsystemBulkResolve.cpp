#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#include "CrowdyCppClient.h"
#include "CrowdyGameModelLog.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

namespace
{
	TAutoConsoleVariable<int32> CVarCrowdyGameModelBulkResolve(
		TEXT("crowdy.gamemodel.bulkresolve"), 1,
		TEXT("1: shared (Host-owned) entities and remote copies of other players' entities bind their Game Model containers from one paged list per type; 0: one call per entity."),
		ECVF_Default);

	// The client hands back the raw `data` object; the codec parsers read Envelope->data.
	TSharedPtr<FJsonObject> BulkWrapDataEnvelope(const TSharedPtr<FJsonObject>& DataObject)
	{
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		if (DataObject.IsValid())
		{
			Envelope->SetObjectField(TEXT("data"), DataObject);
		}
		return Envelope;
	}

	// After a failed list a group waits 2, 4, 8 ... 30 s before its next list, so a refusing server is asked at the
	// rate the per-entity backoff would ask, not every sweep.
	double BulkResolveBackoffSeconds(int32 Failures)
	{
		return FMath::Min(2.0 * static_cast<double>(1 << FMath::Clamp(Failures, 0, 4)), 30.0);
	}
}

FString UCrowdyGameModelSubsystem::BulkResolveGroupKey(const FString& TypeName, const FString& Session)
{
	return TypeName + TEXT("|") + Session;
}

struct UCrowdyGameModelSubsystem::FCrowdyBulkResolveRun
{
	FString TypeName;
	FString ResolvedSession;
	FString GroupKey;
	int64 AppId = 0;
	TArray<FGuid> NetIDs;
	TMap<FString, FGuid> NetIDByKey;
	TArray<FCrowdyBulkResolveHit> Hits;
	int32 PagesRead = 0;
	int32 MaxPages = BulkResolveMaxPagesPerType;
	bool bHasAuthoritative = false;
	bool bReachedEnd = false;
};

bool UCrowdyGameModelSubsystem::IsBulkResolveEnabled()
{
	return CVarCrowdyGameModelBulkResolve.GetValueOnGameThread() != 0;
}

bool UCrowdyGameModelSubsystem::IsBulkResolveEligible(const FGuid& NetID) const
{
	// A shared entity's row, a remote copy of another player's entity and a class-derived read are all found by the
	// list; only a locally owned entity ensures its own row one by one.
	const UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	const FCrowdyEntityRecord* Record = Entities ? Entities->FindRecord(NetID) : nullptr;
	if (!Record)
	{
		return false;
	}
	return ClassDerivedBindings.Contains(NetID) || Record->Role == ECrowdyRole::HostOwned
		|| Record->Role == ECrowdyRole::RemoteProxy;
}

void UCrowdyGameModelSubsystem::MatchContainerRows(const TArray<FCrowdyGameApiCodec::FContainerRow>& Rows,
	const TMap<FString, FGuid>& NetIDByKey, const FString& ResolvedSession, TArray<FCrowdyBulkResolveHit>& OutHits)
{
	for (const FCrowdyGameApiCodec::FContainerRow& Row : Rows)
	{
		if (!Row.SessionId.Equals(ResolvedSession, ESearchCase::CaseSensitive))
		{
			continue;
		}
		const FGuid* NetID = NetIDByKey.Find(Row.BindingKey);
		if (!NetID)
		{
			continue;
		}
		FCrowdyBulkResolveHit& Hit = OutHits.AddDefaulted_GetRef();
		Hit.NetID = *NetID;
		Hit.ContainerId = Row.ContainerId;
		Hit.OwnerUserId = Row.OwnerUserId;
	}
}

bool UCrowdyGameModelSubsystem::BindResolvedRow(const FGuid& NetID, const FString& ContainerId, int64 OwnerUserId)
{
	ResolveInFlight.Remove(NetID);
	// The entity may have unregistered during the round trip; do not resurrect a binding for a destroyed entity
	// (it would leak a map entry and waste every future notification's pull on it).
	if (ContainerId.IsEmpty() || !ResolveEntityParticipant(NetID))
	{
		return false;
	}
	BindEntityContainer(NetID, ContainerId);
	if (OwnerUserId > 0)
	{
		NetIDToOwnerUserId.Add(NetID, OwnerUserId);
	}
	PendingModelEntities.Remove(NetID);
	PendingSessionByNetID.Remove(NetID);
	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[GameModel] resolved entity %s -> container %s"), *NetID.ToString(), *ContainerId);
	return true;
}

void UCrowdyGameModelSubsystem::BulkResolveType(const FString& TypeName, const FString& ResolvedSession,
	const TArray<FGuid>& NetIDs, int64 AppId)
{
	if (NetIDs.Num() == 0)
	{
		return;
	}
	const TSharedRef<FCrowdyBulkResolveRun> Run = MakeShared<FCrowdyBulkResolveRun>();
	Run->TypeName = TypeName;
	Run->ResolvedSession = ResolvedSession;
	Run->GroupKey = BulkResolveGroupKey(TypeName, ResolvedSession);
	Run->AppId = AppId;
	Run->NetIDs = NetIDs;
	for (const FGuid& NetID : NetIDs)
	{
		Run->NetIDByKey.Add(FCrowdyModelIdentity::NetIDToContainerKey(NetID), NetID);
		Run->bHasAuthoritative |= IsAuthoritativeToCreate(NetID);
		BulkResolveInFlight.Add(NetID);
	}
	// Without a shared entity every miss would be a keyed read, so the list never costs more calls than those reads.
	if (!Run->bHasAuthoritative)
	{
		Run->MaxPages = FMath::Min(BulkResolveMaxPagesPerType, NetIDs.Num());
	}
	BulkResolveGroupsInFlight.Add(Run->GroupKey);
	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[GameModel] bulk resolve %s: listing for %d pending entit%s in session %s"), *TypeName, NetIDs.Num(),
		NetIDs.Num() == 1 ? TEXT("y") : TEXT("ies"), ResolvedSession.IsEmpty() ? TEXT("<app-global>") : *ResolvedSession);
	ReadBulkResolvePage(Run, 0);
}

void UCrowdyGameModelSubsystem::ReadBulkResolvePage(const TSharedRef<FCrowdyBulkResolveRun>& Run, int32 Offset)
{
	// Resolved per page: a stale token or endpoint reinstalled on the shared host could rebuild the client mid-walk.
	FString Endpoint, Token;
	int64 AppId = 0;
	FCrowdyCppClient* Client = ResolveApiContext(Endpoint, Token, AppId) ? EnsureCppClient(Endpoint, Token) : nullptr;
	if (!Client)
	{
		FinishBulkResolve(Run, false);
		return;
	}
	++NetStats.BulkResolveListPages;
	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(
		Run->AppId, Run->TypeName, Run->ResolvedSession, BulkResolvePageSize, Offset);
	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	Client->RunRuntimeOp(TEXT("GameModelContainers"), Vars, [WeakThis, Run, Offset](FCrowdyCppJsonResult R)
	{
		UCrowdyGameModelSubsystem* S = ResolveLiveSelf(WeakThis);
		if (!S)
		{
			return;
		}
		TArray<FCrowdyGameApiCodec::FContainerRow> Rows;
		int32 RawRowCount = 0;
		if (!FCrowdyGameApiCodec::ParseContainerRowsEnvelope(BulkWrapDataEnvelope(R.Data), R.bTransportOk, TArray<FString>(), Rows, &RawRowCount))
		{
			S->FinishBulkResolve(Run, false);
			return;
		}
		++Run->PagesRead;
		UE_CLOG(CrowdyGameModelTrace::GameModel() && RawRowCount != Rows.Num(), LogCrowdyGameModel, Log,
			TEXT("[GameModel] bulk resolve %s: page %d carried %d row(s) with no id or key, skipped"),
			*Run->TypeName, Run->PagesRead, RawRowCount - Rows.Num());
		MatchContainerRows(Rows, Run->NetIDByKey, Run->ResolvedSession, Run->Hits);
		// A full page may have more behind it; a short page is the end of the type. Fullness is the server's row
		// count, not the usable one, so a keyless row cannot end paging early. Past the ceiling the rest of the
		// entities fall through to their own call, which is correct, only dearer.
		Run->bReachedEnd = RawRowCount < BulkResolvePageSize;
		if (!Run->bReachedEnd && Run->PagesRead < Run->MaxPages)
		{
			S->ReadBulkResolvePage(Run, Offset + BulkResolvePageSize);
			return;
		}
		if (!Run->bReachedEnd && !Run->bHasAuthoritative)
		{
			S->BulkResolveCutGroups.Add(Run->GroupKey);
		}
		S->FinishBulkResolve(Run, true);
	});
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyGameModelSubsystem::FinishBulkResolveForTest(const FString& TypeName, const FString& Session,
	const TArray<FGuid>& NetIDs, const TArray<FCrowdyBulkResolveHit>& Hits, bool bReachedEnd)
{
	const TSharedRef<FCrowdyBulkResolveRun> Run = MakeShared<FCrowdyBulkResolveRun>();
	Run->TypeName = TypeName;
	Run->ResolvedSession = Session;
	Run->GroupKey = BulkResolveGroupKey(TypeName, Session);
	Run->NetIDs = NetIDs;
	Run->Hits = Hits;
	Run->bReachedEnd = bReachedEnd;
	FinishBulkResolve(Run, true);
}
#endif

void UCrowdyGameModelSubsystem::FinishBulkResolve(const TSharedRef<FCrowdyBulkResolveRun>& Run, bool bListOk)
{
	for (const FGuid& NetID : Run->NetIDs)
	{
		BulkResolveInFlight.Remove(NetID);
	}
	BulkResolveGroupsInFlight.Remove(Run->GroupKey);
	if (!bListOk)
	{
		const int32 Failures = BulkResolveTypeFailures.FindOrAdd(Run->GroupKey)++;
		BulkResolveTypeNextAttempt.Add(Run->GroupKey, FApp::GetCurrentTime() + BulkResolveBackoffSeconds(Failures));
		UE_LOG(LogCrowdyGameModel, Warning, TEXT("[GameModel] bulk resolve %s: the list failed; its %d entit%s stay pending for the next sweep."),
			*Run->TypeName, Run->NetIDs.Num(), Run->NetIDs.Num() == 1 ? TEXT("y") : TEXT("ies"));
		RequestPendingModelEntitySweep();
		return;
	}
	BulkResolveTypeFailures.Remove(Run->GroupKey);
	BulkResolveTypeNextAttempt.Remove(Run->GroupKey);

	TSet<FGuid> Bound;
	TArray<TPair<FString, FGuid>> PullTargets;
	int32 Skipped = 0;
	for (const FCrowdyBulkResolveHit& Hit : Run->Hits)
	{
		// Listed twice, bound meanwhile by another path, or ensuring right now (its role moved while the list was
		// out): that resolve owns the bind and the in-flight guard, so the hit is left to it.
		if (Bound.Contains(Hit.NetID) || NetIDToContainerId.Contains(Hit.NetID) || ResolveInFlight.Contains(Hit.NetID))
		{
			++Skipped;
			continue;
		}
		// Now owned here, so it binds through its own ensure, which refuses a row another user holds.
		if (IsAuthoritativeToCreate(Hit.NetID) && !IsHostOwnedEntity(Hit.NetID))
		{
			++Skipped;
			continue;
		}
		const bool bBoundBefore = NetStats.EverBound.Contains(Hit.NetID);
		if (!BindResolvedRow(Hit.NetID, Hit.ContainerId, Hit.OwnerUserId))
		{
			++Skipped;
			continue;
		}
		// Only hits count here; a miss is counted once by its own call below.
		++NetStats.ResolveStarts;
		NetStats.ReResolves += bBoundBefore ? 1 : 0;
		Bound.Add(Hit.NetID);
		++BulkResolveHitCount;
		if (ShouldPullOnRetryForEntity(Hit.NetID))
		{
			PullTargets.Emplace(Hit.ContainerId, Hit.NetID);
		}
	}
	// The bound hits that pull on start are read in one bulk call rather than one state read each.
	PullAndApplyContainerStates(MoveTemp(PullTargets), true);

	// The misses: rows the server does not hold yet. The single ensure is the only path that creates one, and it
	// is spent under the same per-sweep bound and per-entity backoff as any other pending resolve, so a level of
	// unseeded objects (or an admin-only type nobody seeded) costs calls at the retry rate, not every sweep.
	const double Now = FApp::GetCurrentTime();
	int32 Ensured = 0;
	int32 Deferred = 0;
	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);
	for (const FGuid& NetID : Run->NetIDs)
	{
		if (Bound.Contains(NetID) || NetIDToContainerId.Contains(NetID) || !PendingModelEntities.Contains(NetID))
		{
			continue;
		}
		FCrowdyPendingBindBackoff& Backoff = PendingBindBackoff.FindOrAdd(NetID);
		// A remote copy the complete list did not name has no row yet, so it backs off without a call.
		if (Run->bReachedEnd && !IsAuthoritativeToCreate(NetID))
		{
			Backoff.NextAttemptTime = Now + static_cast<double>(ResolvePendingRetryDelaySeconds(Backoff.Attempts));
			++Backoff.Attempts;
			++Deferred;
			continue;
		}
		if (Ensured >= MaxPendingResolvesPerSweep || (Backoff.Attempts > 0 && Now < Backoff.NextAttemptTime))
		{
			++Deferred;
			continue;
		}
		Backoff.NextAttemptTime = Now + static_cast<double>(ResolvePendingRetryDelaySeconds(Backoff.Attempts));
		++Backoff.Attempts;
		++Ensured;
		ResolveOrCreateContainer(NetID, Run->TypeName, Run->ResolvedSession, [WeakThis, NetID](bool bOk, const FString& ContainerId)
		{
			UCrowdyGameModelSubsystem* Self = bOk ? ResolveLiveSelf(WeakThis) : nullptr;
			if (Self && Self->ShouldPullOnRetryForEntity(NetID))
			{
				Self->EnqueueRefreshPull(ContainerId, NetID);
			}
		});
	}
	if (Deferred > 0)
	{
		RequestPendingModelEntitySweep();
	}
	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[GameModel] bulk resolve %s: %d page(s), %d bound, %d resolved singly, %d deferred, %d hit(s) already bound, gone or handed to their own ensure"),
		*Run->TypeName, Run->PagesRead, Bound.Num(), Ensured, Deferred, Skipped);
}
