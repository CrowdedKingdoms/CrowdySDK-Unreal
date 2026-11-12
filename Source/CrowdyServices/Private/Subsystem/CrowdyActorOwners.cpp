#include "Subsystem/CrowdyActorOwners.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyCppClient.h"
#include "CrowdyServicesLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformTime.h"
#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/HelperFunctions.h"

using namespace CrowdyServiceApi;

namespace
{
	constexpr const TCHAR* ActorOwnersLogName = TEXT("CrowdyActorOwners");

	FCrowdyActorOwner MakeActorOwner(ECrowdyActorOwnerResult Result, const FString& Message, int64 UserId = 0)
	{
		FCrowdyActorOwner Owner;
		Owner.Result = Result;
		Owner.UserId = UserId;
		Owner.Message = Message;
		return Owner;
	}

	FCrowdyActorOwner UnknownActorOwner()
	{
		return MakeActorOwner(ECrowdyActorOwnerResult::Unknown, TEXT("No record ties this actor id to a user."));
	}

	// Every record whose id is the one derived from its user id; false when the answer cannot be read at all.
	bool ReadRegisteredOwners(const FCrowdyCppJsonResult& Result, TMap<FGuid, int64>& OutOwners)
	{
		const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
		if (!Result.bTransportOk || !Result.Data.IsValid() || !Result.Data->TryGetArrayField(TEXT("batchLookupActors"), Records))
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Records)
		{
			const TSharedPtr<FJsonObject>* Record = nullptr;
			FString Uuid;
			FGuid RecordId;
			int64 UserId = 0;
			if (!Value.IsValid() || !Value->TryGetObject(Record) || !(*Record)->TryGetStringField(TEXT("uuid"), Uuid)
				|| !FGuid::ParseExact(Uuid, EGuidFormats::Digits, RecordId))
			{
				continue;
			}
			if (ReadWholeInt64(*Record, TEXT("userId"), UserId) && UserId > 0 && UHelperFunctions::GetDeterministicID(UserId) == RecordId)
			{
				OutOwners.Add(RecordId, UserId);
			}
		}
		return true;
	}
}

void UCrowdyActorOwners::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LiveSessionToken = MakeShared<uint8>(0);
	Collection.InitializeDependency<UCrowdyCppClientSubsystem>();
	WatchSession(Collection.InitializeDependency<UCrowdyGameSession>());
}

void UCrowdyActorOwners::Deinitialize()
{
	LiveSessionToken.Reset();
	CancelFlush();
	CancelRecordRetry();
	if (UCrowdyGameSession* Session = GameSession.Get())
	{
		Session->OnOwnerUUIDUpdated.RemoveDynamic(this, &UCrowdyActorOwners::HandleOwnerUuidUpdated);
	}

	// Every waiter is answered once, so a caller never waits forever on an ask the game instance outlived.
	TMap<FGuid, TArray<FOwnerCallback>> Waiting = MoveTemp(Pending);
	Pending.Reset();
	InFlight.Reset();
	const FCrowdyActorOwner Ended = MakeActorOwner(ECrowdyActorOwnerResult::Failed, TEXT("The game instance ended before the platform answered."));
	for (const TPair<FGuid, TArray<FOwnerCallback>>& Entry : Waiting)
	{
		for (const FOwnerCallback& Waiter : Entry.Value)
		{
			Waiter(Ended);
		}
	}

	Super::Deinitialize();
}

int64 UCrowdyActorOwners::GetAppId() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (AppIdForTest != 0)
	{
		return AppIdForTest;
	}
#endif
	return ResolveAppId(GetGameInstance());
}

FCrowdyCppClient* UCrowdyActorOwners::ResolveClient() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (const TSharedPtr<FCrowdyCppClient> Override = ClientForTest.Pin())
	{
		return Override.Get();
	}
#endif
	return ResolveApiClient(GetGameInstance(), ActorOwnersLogName);
}

double UCrowdyActorOwners::Now() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (NowForTest > 0.0)
	{
		return NowForTest;
	}
#endif
	return FPlatformTime::Seconds();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyActorOwners::InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId, UCrowdyGameSession* Session)
{
	LiveSessionToken = MakeShared<uint8>(0);
	ClientForTest = Client;
	AppIdForTest = AppId;
	WatchSession(Session);
}

bool UCrowdyActorOwners::FlushLookupsForTest()
{
	if (!FlushHandle.IsValid())
	{
		return false;
	}
	CancelFlush();
	FlushLookups();
	return true;
}

bool UCrowdyActorOwners::RunRecordRetryForTest()
{
	if (!RecordRetryHandle.IsValid())
	{
		return false;
	}
	CancelRecordRetry();
	SendRecord();
	return true;
}
#endif

void UCrowdyActorOwners::ResolveActorOwner(FGuid ActorId, FOnActorOwnerResolved OnResolved)
{
	ResolveActorOwner(ActorId, [OnResolved](const FCrowdyActorOwner& Owner) { OnResolved.ExecuteIfBound(Owner); });
}

void UCrowdyActorOwners::ResolveActorOwner(const FGuid& ActorId, FOwnerCallback OnResolved)
{
	if (!ActorId.IsValid())
	{
		OnResolved(MakeActorOwner(ECrowdyActorOwnerResult::Unknown, TEXT("The actor id is not set.")));
		return;
	}
	if (const int64* UserId = ConfirmedOwners.Find(ActorId))
	{
		OnResolved(MakeActorOwner(ECrowdyActorOwnerResult::Confirmed, FString(), *UserId));
		return;
	}
	const double* Until = UnknownUntil.Find(ActorId);
	if (Until && Now() < *Until)
	{
		OnResolved(UnknownActorOwner());
		return;
	}

	Pending.FindOrAdd(ActorId).Add(MoveTemp(OnResolved));
	if (InFlight.Contains(ActorId) || FlushHandle.IsValid())
	{
		return;
	}
	FlushHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float)
	{
		FlushHandle.Reset();
		FlushLookups();
		return false;
	}));
}

void UCrowdyActorOwners::FlushLookups()
{
	TArray<FGuid> Queued;
	for (const TPair<FGuid, TArray<FOwnerCallback>>& Entry : Pending)
	{
		if (!InFlight.Contains(Entry.Key))
		{
			Queued.Add(Entry.Key);
		}
	}
	if (Queued.IsEmpty())
	{
		return;
	}

	FCrowdyCppClient* Client = LiveSessionToken.IsValid() ? ResolveClient() : nullptr;
	if (!Client)
	{
		const FCrowdyActorOwner Unsent = MakeActorOwner(ECrowdyActorOwnerResult::Failed, TEXT("The Crowdy API client is unavailable, so the platform was not asked."));
		for (const FGuid& ActorId : Queued)
		{
			AnswerWaiters(ActorId, Unsent);
		}
		return;
	}

	for (int32 First = 0; First < Queued.Num(); First += MaxIdsPerLookup)
	{
		SendLookup(*Client, TArray<FGuid>(Queued.GetData() + First, FMath::Min(MaxIdsPerLookup, Queued.Num() - First)));
	}
}

void UCrowdyActorOwners::SendLookup(FCrowdyCppClient& Client, TArray<FGuid> ActorIds)
{
	TArray<TSharedPtr<FJsonValue>> Uuids;
	Uuids.Reserve(ActorIds.Num());
	for (const FGuid& ActorId : ActorIds)
	{
		InFlight.Add(ActorId);
		Uuids.Add(MakeShared<FJsonValueString>(ActorId.ToString(EGuidFormats::Digits)));
	}
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetArrayField(TEXT("uuids"), Uuids);

	TWeakObjectPtr<UCrowdyActorOwners> WeakThis(this);
	const int64 AppId = GetAppId();
	Client.RunOp(ECrowdyCppApiDomain::Actors, TEXT("BatchLookupActors"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, ActorIds = MoveTemp(ActorIds), AppId](FCrowdyCppJsonResult Result)
		{
			if (UCrowdyActorOwners* Self = WeakThis.Get())
			{
				Self->HandleLookup(ActorIds, AppId, Result);
			}
		}));
}

void UCrowdyActorOwners::HandleLookup(const TArray<FGuid>& ActorIds, int64 AppId, const FCrowdyCppJsonResult& Result)
{
	TMap<FGuid, int64> Registered;
	const bool bRead = ReadRegisteredOwners(Result, Registered);
	const FCrowdyActorOwner Failed = MakeActorOwner(ECrowdyActorOwnerResult::Failed,
		Result.bTransportOk ? FString(TEXT("The server's answer could not be read.")) : FailureMessage(Result));

	// An answer about another app, after the session moved, is passed on but not kept.
	const bool bSameApp = AppId == GetAppId();
	for (const FGuid& ActorId : ActorIds)
	{
		InFlight.Remove(ActorId);
		const int64* UserId = Registered.Find(ActorId);
		const FCrowdyActorOwner Owner = !bRead ? Failed
			: UserId ? MakeActorOwner(ECrowdyActorOwnerResult::Confirmed, FString(), *UserId) : UnknownActorOwner();
		if (bSameApp)
		{
			Remember(ActorId, Owner);
		}
		AnswerWaiters(ActorId, Owner);
	}
}

void UCrowdyActorOwners::Remember(const FGuid& ActorId, const FCrowdyActorOwner& Owner)
{
	if (Owner.Result == ECrowdyActorOwnerResult::Confirmed)
	{
		ConfirmedOwners.Add(ActorId, Owner.UserId);
		UnknownUntil.Remove(ActorId);
		return;
	}
	if (Owner.Result != ECrowdyActorOwnerResult::Unknown)
	{
		return;
	}

	const double CurrentTime = Now();
	UnknownUntil.Add(ActorId, CurrentTime + UnknownSeconds);
	if (UnknownUntil.Num() <= MaxUnknownEntries)
	{
		return;
	}
	for (auto It = UnknownUntil.CreateIterator(); It; ++It)
	{
		if (It.Value() <= CurrentTime)
		{
			It.RemoveCurrent();
		}
	}
	if (UnknownUntil.Num() <= MaxUnknownEntries)
	{
		return;
	}

	// Forgetting an Unknown early only costs asking again, so the ones expiring soonest go.
	UnknownUntil.ValueSort([](double A, double B) { return A > B; });
	int32 Kept = 0;
	for (auto It = UnknownUntil.CreateIterator(); It; ++It)
	{
		if (++Kept > MaxUnknownEntries)
		{
			It.RemoveCurrent();
		}
	}
}

void UCrowdyActorOwners::AnswerWaiters(const FGuid& ActorId, const FCrowdyActorOwner& Owner)
{
	TArray<FOwnerCallback> Waiters;
	Pending.RemoveAndCopyValue(ActorId, Waiters);
	for (const FOwnerCallback& Waiter : Waiters)
	{
		Waiter(Owner);
	}
}

void UCrowdyActorOwners::HandleSignedOut()
{
	// Queued lookups stay: what a record says is the same for every player in the app.
	CancelRecordRetry();
	++RecordAttempt;
	RecordUserId = 0;
	RecordUuid.Reset();
	RecordTries = 0;
	ConfirmedOwners.Reset();
	UnknownUntil.Reset();
}

void UCrowdyActorOwners::WatchSession(UCrowdyGameSession* Session)
{
	if (!Session)
	{
		return;
	}
	GameSession = Session;
	Session->OnOwnerUUIDUpdated.AddUniqueDynamic(this, &UCrowdyActorOwners::HandleOwnerUuidUpdated);
	HandleOwnerUuidUpdated(Session->GetUUID());
}

void UCrowdyActorOwners::HandleOwnerUuidUpdated(FString Uuid)
{
	const UCrowdyGameSession* Session = GameSession.Get();
	const int64 UserId = Session ? Session->GetUserID() : 0;
	if (UserId <= 0 || UserId == RecordUserId)
	{
		return;
	}
	// Only an id derived from the user id can be resolved by other players, so any other is not recorded.
	if (!Uuid.Equals(UHelperFunctions::GetDeterministicID(UserId).ToString(EGuidFormats::Digits), ESearchCase::CaseSensitive))
	{
		return;
	}

	CancelRecordRetry();
	RecordUserId = UserId;
	RecordUuid = Uuid;
	RecordTries = 0;
	SendRecord();
}

void UCrowdyActorOwners::SendRecord()
{
	++RecordTries;
	const int32 Attempt = ++RecordAttempt;
	FCrowdyCppClient* Client = LiveSessionToken.IsValid() ? ResolveClient() : nullptr;
	if (!Client)
	{
		HandleRecord(FCrowdyCppJsonResult());
		return;
	}

	const TSharedPtr<FJsonObject> Chunk = MakeShared<FJsonObject>();
	Chunk->SetStringField(TEXT("x"), TEXT("0"));
	Chunk->SetStringField(TEXT("y"), TEXT("0"));
	Chunk->SetStringField(TEXT("z"), TEXT("0"));

	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Input->SetStringField(TEXT("uuid"), RecordUuid);
	Input->SetObjectField(TEXT("chunk"), Chunk);

	TWeakObjectPtr<UCrowdyActorOwners> WeakThis(this);
	Client->RunOp(ECrowdyCppApiDomain::Actors, TEXT("CreateActor"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, Attempt](FCrowdyCppJsonResult Result)
		{
			UCrowdyActorOwners* Self = WeakThis.Get();
			if (Self && Self->RecordAttempt == Attempt)
			{
				Self->HandleRecord(Result);
			}
		}));
}

void UCrowdyActorOwners::HandleRecord(const FCrowdyCppJsonResult& Result)
{
	if (Result.bTransportOk)
	{
		return;
	}
	if (NamesCode(Result, TEXT("ACTOR_EXISTS")))
	{
		if (Result.OwnedByCaller.IsSet() && !Result.OwnedByCaller.GetValue())
		{
			WarnRecord(TEXT("Another player's actor record holds this player's actor id, so other players cannot resolve it to this player."));
		}
		return;
	}
	// Any failure is tried again: a create that had landed answers ACTOR_EXISTS owned by the caller, which is done.
	if (RecordTries < MaxRecordTries)
	{
		RecordRetryHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float)
		{
			RecordRetryHandle.Reset();
			SendRecord();
			return false;
		}), RecordRetrySeconds);
		return;
	}

	// Forgotten, so the next id update for this player tries again.
	RecordUserId = 0;
	const FString Reason = Result.ErrorCode.IsEmpty() ? FString(TEXT("the request did not complete")) : FailureCode(Result);
	WarnRecord(FString::Printf(TEXT("Could not record this player's actor, so other players cannot resolve it to this player: %s."), *Reason));
}

void UCrowdyActorOwners::WarnRecord(const FString& Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
#if WITH_DEV_AUTOMATION_TESTS
	++RecordWarningsForTest;
#endif
}

void UCrowdyActorOwners::CancelRecordRetry()
{
	if (!RecordRetryHandle.IsValid())
	{
		return;
	}
	FTSTicker::RemoveTicker(RecordRetryHandle);
	RecordRetryHandle.Reset();
}

void UCrowdyActorOwners::CancelFlush()
{
	if (!FlushHandle.IsValid())
	{
		return;
	}
	FTSTicker::RemoveTicker(FlushHandle);
	FlushHandle.Reset();
}
