#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#include "CrowdyCppClient.h"
#include "CrowdyGameModelLog.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"

namespace
{
	TAutoConsoleVariable<int32> CVarCrowdyGameModelBulkResolve(
		TEXT("crowdy.gamemodel.bulkresolve"), 1,
		TEXT("1: shared (Host-owned) entities bind their Game Model containers from one paged list per type; 0: one ensure per entity."),
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

	FString BulkResolveGroupKey(const FString& TypeName, const FString& Session)
	{
		return TypeName + TEXT("|") + Session;
	}
}

struct UCrowdyGameModelSubsystem::FCrowdyBulkResolveRun
{
	FString TypeName;
	FString ResolvedSession;
	FString GroupKey;
	FString Endpoint;
	FString Token;
	int64 AppId = 0;
	TArray<FGuid> NetIDs;
	TMap<FString, FGuid> NetIDByKey;
	TArray<FCrowdyBulkResolveHit> Hits;
	int32 PagesRead = 0;
};

bool UCrowdyGameModelSubsystem::IsBulkResolveEnabled()
{
	return CVarCrowdyGameModelBulkResolve.GetValueOnGameThread() != 0;
}

bool UCrowdyGameModelSubsystem::IsBulkResolveEligible(const FGuid& NetID) const
{
	// A class-derived binding reads another entity's row and must never ensure; a per-player entity has a row of
	// its own that nobody else lists. Only a shared entity's row is the same on every client.
	return !ClassDerivedBindings.Contains(NetID) && IsHostOwnedEntity(NetID);
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
	const TArray<FGuid>& NetIDs, const FString& Endpoint, const FString& Token, int64 AppId)
{
	if (NetIDs.Num() == 0)
	{
		return;
	}
	const TSharedRef<FCrowdyBulkResolveRun> Run = MakeShared<FCrowdyBulkResolveRun>();
	Run->TypeName = TypeName;
	Run->ResolvedSession = ResolvedSession;
	Run->GroupKey = BulkResolveGroupKey(TypeName, ResolvedSession);
	Run->Endpoint = Endpoint;
	Run->Token = Token;
	Run->AppId = AppId;
	Run->NetIDs = NetIDs;
	for (const FGuid& NetID : NetIDs)
	{
		Run->NetIDByKey.Add(FCrowdyModelIdentity::NetIDToContainerKey(NetID), NetID);
		BulkResolveInFlight.Add(NetID);
	}
	BulkResolveGroupsInFlight.Add(Run->GroupKey);
	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[GameModel] bulk resolve %s: listing for %d pending entit%s in session %s"), *TypeName, NetIDs.Num(),
		NetIDs.Num() == 1 ? TEXT("y") : TEXT("ies"), ResolvedSession.IsEmpty() ? TEXT("<app-global>") : *ResolvedSession);
	ReadBulkResolvePage(Run, 0);
}

void UCrowdyGameModelSubsystem::ReadBulkResolvePage(const TSharedRef<FCrowdyBulkResolveRun>& Run, int32 Offset)
{
	FCrowdyCppClient* Client = EnsureCppClient(Run->Endpoint, Run->Token);
	if (!Client)
	{
		FinishBulkResolve(Run, false);
		return;
	}
	++BulkResolveListCallCount;
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
		// entities fall through to their own ensure, which is correct, only dearer.
		if (RawRowCount >= BulkResolvePageSize && Run->PagesRead < BulkResolveMaxPagesPerType)
		{
			S->ReadBulkResolvePage(Run, Offset + BulkResolvePageSize);
			return;
		}
		S->FinishBulkResolve(Run, true);
	});
}

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
		if (!BindResolvedRow(Hit.NetID, Hit.ContainerId, Hit.OwnerUserId))
		{
			++Skipped;
			continue;
		}
		Bound.Add(Hit.NetID);
		++BulkResolveHitCount;
		if (ShouldPullOnRetryForEntity(Hit.NetID))
		{
			PullTargets.Emplace(Hit.ContainerId, Hit.NetID);
		}
	}
	// The bound hits that pull on start are read in one bulk call rather than one state read each.
	PullAndApplyContainerStates(MoveTemp(PullTargets));

	// The misses: rows the server does not hold yet. The single ensure is the only path that creates one, and it
	// is spent under the same per-sweep bound and per-entity backoff as any other pending resolve, so a level of
	// unseeded objects (or an admin-only type nobody seeded) costs the allowance at the retry rate, not every sweep.
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
		if (Ensured >= MaxPendingResolvesPerSweep || (Backoff.Attempts > 0 && Now < Backoff.NextAttemptTime))
		{
			++Deferred;
			continue;
		}
		Backoff.NextAttemptTime = Now + static_cast<double>(ResolvePendingRetryDelaySeconds(Backoff.Attempts));
		++Backoff.Attempts;
		++Ensured;
		ResolveOrCreateContainer(NetID, Run->TypeName, Run->ResolvedSession, [WeakThis, NetID](bool bOk, const FString&)
		{
			UCrowdyGameModelSubsystem* Self = bOk ? ResolveLiveSelf(WeakThis) : nullptr;
			if (Self && Self->ShouldPullOnRetryForEntity(NetID))
			{
				Self->HandleModelChanged(NetID);
			}
		});
	}
	if (Deferred > 0)
	{
		RequestPendingModelEntitySweep();
	}
	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[GameModel] bulk resolve %s: %d page(s), %d bound, %d ensured, %d deferred, %d hit(s) already bound or gone"),
		*Run->TypeName, Run->PagesRead, Bound.Num(), Ensured, Deferred, Skipped);
}
