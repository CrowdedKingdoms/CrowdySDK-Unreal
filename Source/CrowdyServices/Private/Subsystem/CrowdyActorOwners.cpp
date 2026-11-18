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

	bool IsRecordableId(const int64 UserId, const FString& Uuid)
	{
		FGuid Parsed;
		return UserId > 0 && FGuid::ParseExact(Uuid, EGuidFormats::Digits, Parsed) && Parsed.IsValid();
	}

	// Every record that names its owner by a positive userId; false when the answer cannot be read at all.
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
			if (ReadWholeInt64(*Record, TEXT("userId"), UserId) && UserId > 0)
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
	// A sign-out still waiting on its deletes ends its server session now, unless the API client host has already gone,
	// which a call would only rebuild; the session then expires on its own.
	if (!CanStillSend())
	{
		OnSignOutDeleted = nullptr;
	}
	FinishSignOut();
	OnRecordSettled = nullptr;
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

bool UCrowdyActorOwners::RunSignOutDeadlineForTest()
{
	if (!SignOutDeadlineHandle.IsValid())
	{
		return false;
	}
	FinishSignOut();
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

void UCrowdyActorOwners::HandleSignedIn(TFunction<void()> OnSettled)
{
	bSignedIn = true;
	OnRecordSettled = MoveTemp(OnSettled);
	bSettled = false;
	Remints = 0;
	CancelRecordRetry();

	// Made afresh whatever was recorded before: a create sent before this sign-in's token existed proves nothing.
	const UCrowdyGameSession* Session = GameSession.Get();
	RecordUserId = Session ? Session->GetUserID() : 0;
	RecordUuid = Session ? Session->GetUUID() : FString();
	bRecordIsOthers = false;
	RecordTries = 0;
	if (!IsRecordableId(RecordUserId, RecordUuid))
	{
		RecordUserId = 0;
		Settle();
		return;
	}
	SendRecord();
}

void UCrowdyActorOwners::HandleSignedOut(TFunction<void()> OnDeleted)
{
	// Every delete of this sign-in goes under its own app token, which the caller is about to forget.
	const UCrowdyGameSession* Session = GameSession.Get();
	const FString Token = Session ? Session->GetGameToken() : FString();

	// A sign-out still waiting folds into this one, so neither ends its server session before its deletes answer.
	const bool bWaiting = SignOutDeadlineHandle.IsValid();
	const int32 Generation = bWaiting ? SignOutGeneration : ++SignOutGeneration;

	// Only records this sign-in made; queued lookups stay, as a record reads the same for everyone.
	TArray<FString, TInlineAllocator<2>> Ours;
	if (!PreviousUuid.IsEmpty())
	{
		Ours.Add(PreviousUuid);
	}
	const bool bOwnRecord = RecordUserId > 0 && !bRecordIsOthers;
	const bool bCreateInFlight = bOwnRecord && CreateInFlightAttempt != 0 && CreateInFlightAttempt == RecordAttempt;
	if (bOwnRecord && !bCreateInFlight)
	{
		Ours.Add(RecordUuid);
	}

	// A delete sent now could reach the server before the create it is meant to undo, so it waits for the create.
	if (bCreateInFlight)
	{
		LateCreates.Add({CreateInFlightAttempt, Generation, RecordUuid, Token});
	}

	bSignedIn = false;
	OnRecordSettled = nullptr;
	bSettled = false;
	Remints = 0;
	CancelRecordRetry();
	++RecordAttempt;
	RecordUserId = 0;
	RecordUuid.Reset();
	PreviousUuid.Reset();
	bRecordIsOthers = false;
	RecordTries = 0;
	ConfirmedOwners.Reset();
	UnknownUntil.Reset();

	if (OnDeleted)
	{
		OnSignOutDeleted = OnSignOutDeleted
			? TFunction<void()>([First = MoveTemp(OnSignOutDeleted), Second = MoveTemp(OnDeleted)]() { First(); Second(); })
			: MoveTemp(OnDeleted);
	}
	SignOutDeletesLeft += Ours.Num() + (bCreateInFlight ? 1 : 0);
	TWeakObjectPtr<UCrowdyActorOwners> WeakThis(this);
	for (const FString& Uuid : Ours)
	{
		const bool bSent = DeleteRecord(Uuid, [WeakThis, Generation]()
		{
			if (UCrowdyActorOwners* Self = WeakThis.Get())
			{
				Self->HandleSignOutDelete(Generation);
			}
		}, Token);
		SignOutDeletesLeft -= bSent ? 0 : 1;
	}
	if (SignOutDeletesLeft <= 0)
	{
		FinishSignOut();
		return;
	}
	if (bWaiting)
	{
		return;
	}
	SignOutDeadlineHandle =FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float)
	{
		SignOutDeadlineHandle.Reset();
		FinishSignOut();
		return false;
	}), SignOutDeleteSeconds);
}

void UCrowdyActorOwners::HandleSignOutDelete(const int32 Generation)
{
	if (Generation != SignOutGeneration || SignOutDeletesLeft <= 0)
	{
		return;
	}
	if (--SignOutDeletesLeft == 0)
	{
		FinishSignOut();
	}
}

void UCrowdyActorOwners::FinishSignOut()
{
	if (SignOutDeadlineHandle.IsValid())
	{
		FTSTicker::RemoveTicker(SignOutDeadlineHandle);
		SignOutDeadlineHandle.Reset();
	}
	SignOutDeletesLeft = 0;
	if (!OnSignOutDeleted)
	{
		return;
	}
	const TFunction<void()> Done = MoveTemp(OnSignOutDeleted);
	OnSignOutDeleted = nullptr;
	Done();
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
	if (!bSignedIn || !IsRecordableId(UserId, Uuid))
	{
		return;
	}
	if (UserId == RecordUserId && Uuid.Equals(RecordUuid, ESearchCase::CaseSensitive))
	{
		return;
	}

	// The previous record goes only once this one is made, so the player always holds one; another player's is not ours.
	if (UserId == RecordUserId && !bRecordIsOthers)
	{
		DeleteRecord(PreviousUuid);
		PreviousUuid = RecordUuid;
	}
	// The session closed the connection for the new id, so the new record's answer is what opens it again.
	CancelRecordRetry();
	bSettled = false;
	RecordUserId = UserId;
	RecordUuid = Uuid;
	bRecordIsOthers = false;
	RecordTries = 0;
	SendRecord();
}

bool UCrowdyActorOwners::DeleteRecord(const FString& Uuid, TFunction<void()> OnAnswered, const FString& GameToken)
{
	FCrowdyCppClient* Client = LiveSessionToken.IsValid() && !Uuid.IsEmpty() ? ResolveClient() : nullptr;
	if (!Client)
	{
		return false;
	}

	// A fresh key per delete: the same id is recorded again at the next sign-in, and a replayed key would not delete it again.
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("uuid"), Uuid);
	Variables->SetStringField(TEXT("idempotencyKey"), FString::Printf(TEXT("delete-actor-%s-%s"), *Uuid, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	TFunction<void(FCrowdyCppJsonResult)> OnDone = GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [Uuid, OnAnswered](FCrowdyCppJsonResult Result)
	{
		UE_CLOG(!Result.bTransportOk, LogCrowdyServices, Warning, TEXT("[%s] Could not delete this player's actor record %s, so it stays: %s."),
			ActorOwnersLogName, *Uuid, *FailureMessage(Result));
		if (OnAnswered)
		{
			OnAnswered();
		}
	});
	if (GameToken.IsEmpty())
	{
		Client->RunOp(ECrowdyCppApiDomain::Actors, TEXT("DeleteActor"), Variables, MoveTemp(OnDone));
		return true;
	}
	Client->RunOpWithBearer(ECrowdyCppApiDomain::Actors, TEXT("DeleteActor"), Variables, GameToken, MoveTemp(OnDone));
	return true;
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
	CreateInFlightAttempt = Attempt;
	Client->RunOp(ECrowdyCppApiDomain::Actors, TEXT("CreateActor"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, Attempt](FCrowdyCppJsonResult Result)
		{
			UCrowdyActorOwners* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			Self->CreateInFlightAttempt = Self->CreateInFlightAttempt == Attempt ? 0 : Self->CreateInFlightAttempt;
			const int32 Late = Self->LateCreates.IndexOfByPredicate([Attempt](const FLateCreate& Create) { return Create.Attempt == Attempt; });
			if (Late != INDEX_NONE)
			{
				const FLateCreate Create = Self->LateCreates[Late];
				Self->LateCreates.RemoveAtSwap(Late);
				Self->HandleCreateAfterSignOut(Create, Result);
				return;
			}
			if (Self->RecordAttempt == Attempt)
			{
				Self->HandleRecord(Result);
			}
		}));
}

void UCrowdyActorOwners::HandleCreateAfterSignOut(const FLateCreate& Create, const FCrowdyCppJsonResult& Result)
{
	// Only a record the answer says this player made is deleted.
	const bool bMade = Result.bTransportOk || (NamesCode(Result, TEXT("ACTOR_EXISTS")) && Result.OwnedByCaller.Get(false));
	TWeakObjectPtr<UCrowdyActorOwners> WeakThis(this);
	const int32 Generation = Create.Generation;
	const bool bSent = bMade && DeleteRecord(Create.Uuid, [WeakThis, Generation]()
	{
		if (UCrowdyActorOwners* Self = WeakThis.Get())
		{
			Self->HandleSignOutDelete(Generation);
		}
	}, Create.Token);
	if (!bSent)
	{
		HandleSignOutDelete(Generation);
	}
}

bool UCrowdyActorOwners::CanStillSend() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ClientForTest.IsValid())
	{
		return true;
	}
#endif
	const UGameInstance* GameInstance = GetGameInstance();
	const UCrowdyCppClientSubsystem* Host = GameInstance ? GameInstance->GetSubsystem<UCrowdyCppClientSubsystem>() : nullptr;
	return Host && Host->IsLive();
}

void UCrowdyActorOwners::HandleRecord(const FCrowdyCppJsonResult& Result)
{
	if (Result.bTransportOk)
	{
		KeepRecord();
		return;
	}
	if (NamesCode(Result, TEXT("ACTOR_EXISTS")))
	{
		if (Result.OwnedByCaller.IsSet() && !Result.OwnedByCaller.GetValue())
		{
			ReplaceSquattedId();
			return;
		}
		// Used as the player's, but never deleted unless the answer said it is the player's.
		bRecordIsOthers = !Result.OwnedByCaller.IsSet();
		KeepRecord();
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
	Settle();
}

void UCrowdyActorOwners::KeepRecord()
{
	DeleteRecord(PreviousUuid);
	PreviousUuid.Reset();
	Settle();
}

void UCrowdyActorOwners::ReplaceSquattedId()
{
	bRecordIsOthers = true;
	UCrowdyGameSession* Session = GameSession.Get();
	if (!Session || Remints >= MaxRemints)
	{
		WarnRecord(TEXT("Another player's actor record holds this player's actor id, so the realtime servers refuse this player's sends under it and other players cannot resolve it to this player."));
		Settle();
		return;
	}

	// The realtime servers refuse a send under an id another player's record holds, so the session takes a fresh one,
	// closing the connection first; the new record's answer opens it again.
	++Remints;
	Session->ReplaceActorID();
}

void UCrowdyActorOwners::Settle()
{
	if (bSettled || !bSignedIn)
	{
		return;
	}
	bSettled = true;
	if (OnRecordSettled)
	{
		const TFunction<void()> Open = OnRecordSettled;
		Open();
	}
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
