#include "Subsystem/CrowdyClaims.h"
#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyServicesLog.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Subsystem/CrowdyGameSession.h"

using namespace CrowdyServiceApi;

namespace
{
	const TCHAR* const ClaimsLogName = TEXT("CrowdyClaims");
	const TCHAR* const ClaimsNotSignedIn = TEXT("The player has no app token yet.");
	const TCHAR* const ClaimsNoClient = TEXT("The Crowdy API client is unavailable, so the request was not sent.");
	const TCHAR* const ClaimsAppChanged = TEXT("The app changed while the call was in flight.");

	// More grids than this over one box is not an answer to it; the box itself holds at most MaxSearchChunks claims.
	constexpr int32 ClaimsMaxGridsListed = 8192;

	const TCHAR* ClaimsResultName(ECrowdyClaimResult Result)
	{
		switch (Result)
		{
		case ECrowdyClaimResult::Success:        return TEXT("Success");
		case ECrowdyClaimResult::NotSignedIn:    return TEXT("NotSignedIn");
		case ECrowdyClaimResult::NotClaimable:   return TEXT("NotClaimable");
		case ECrowdyClaimResult::AlreadyClaimed: return TEXT("AlreadyClaimed");
		case ECrowdyClaimResult::Refused:        return TEXT("Refused");
		case ECrowdyClaimResult::Failed:         return TEXT("Failed");
		case ECrowdyClaimResult::BadReply:       return TEXT("BadReply");
		}
		return TEXT("Unknown");
	}

	// An answer known before any request still arrives on a later tick, so a caller never sees it inside its own call.
	void ClaimsDefer(const TSharedPtr<uint8>& LiveToken, TFunction<void()> Work)
	{
		TWeakPtr<uint8> Weak = LiveToken;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak, Work = MoveTemp(Work)](float)
		{
			if (Weak.IsValid())
			{
				Work();
			}
			return false;
		}));
	}

	// Only the result and the platform's code are logged: the platform's text may name a player.
	void ClaimsWarn(const TCHAR* Call, ECrowdyClaimResult Result, const FString& Code)
	{
		UE_CLOG(Result != ECrowdyClaimResult::Success, LogCrowdyServices, Warning, TEXT("[%s] %s ended %s%s%s"), ClaimsLogName, Call, ClaimsResultName(Result),
			Code.IsEmpty() ? TEXT("") : TEXT(": "), *Code);
	}

	// Sends one operation and hands its answer to Body on a later tick if the client answers inside the send, as it does
	// when it cannot send at all.
	void ClaimsSend(FCrowdyCppClient& Client, ECrowdyCppApiDomain Domain, const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables,
		const TSharedPtr<uint8>& LiveToken, TFunction<void(FCrowdyCppJsonResult)> Body)
	{
		const TSharedRef<bool> bInCall = MakeShared<bool>(true);
		TWeakPtr<uint8> WeakToken = LiveToken;
		Client.RunOp(Domain, Operation, Variables, GuardLifetime<FCrowdyCppJsonResult>(LiveToken,
			[bInCall, WeakToken, Body = MoveTemp(Body)](FCrowdyCppJsonResult Result) mutable
			{
				if (!*bInCall)
				{
					Body(MoveTemp(Result));
					return;
				}
				ClaimsDefer(WeakToken.Pin(), [Body = MoveTemp(Body), Result = MoveTemp(Result)]() mutable { Body(MoveTemp(Result)); });
			}));
		*bInCall = false;
	}

	// The platform names a grid refusal in the error's code, its reason, or, as the live API does under a CONFLICT code, at the
	// start of its message ("GRID_ALREADY_CLAIMED: ..."), so all three are read.
	bool ClaimsNames(const FCrowdyCppJsonResult& Result, const TCHAR* Code)
	{
		return Result.ErrorCode == Code || Result.ErrorReason == Code || Result.ErrorMessage.StartsWith(FString::Printf(TEXT("%s:"), Code), ESearchCase::CaseSensitive);
	}

	ECrowdyClaimResult ClaimsFailureResult(const FCrowdyCppJsonResult& Result)
	{
		if (ClaimsNames(Result, TEXT("GRID_NOT_CLAIMABLE")))
		{
			return ECrowdyClaimResult::NotClaimable;
		}
		if (ClaimsNames(Result, TEXT("GRID_ALREADY_CLAIMED")))
		{
			return ECrowdyClaimResult::AlreadyClaimed;
		}
		const bool bCanceled = Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage();
		return Result.ErrorCode.IsEmpty() || bCanceled ? ECrowdyClaimResult::Failed : ECrowdyClaimResult::Refused;
	}

	// The platform's code, with its reason when there is one; empty for a request that did not complete.
	FString ClaimsFailureCode(const FCrowdyCppJsonResult& Result)
	{
		return Result.ErrorReason.IsEmpty() ? Result.ErrorCode : FString::Printf(TEXT("%s %s"), *Result.ErrorCode, *Result.ErrorReason);
	}

	FString ClaimsFailureMessage(const FCrowdyCppJsonResult& Result)
	{
		return Result.ErrorCode.IsEmpty() ? Result.ErrorMessage : FString::Printf(TEXT("%s: %s"), *ClaimsFailureCode(Result), *Result.ErrorMessage);
	}

	// A BigInt field arrives as a decimal string; a number is accepted too. Text that is not exactly one int64, such as
	// "12abc" or one past the range, is unread.
	bool ClaimsReadInt64(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64& Out)
	{
		FString Text;
		if (Object->TryGetStringField(Field, Text))
		{
			return LexTryParseString(Out, *Text) && FString::Printf(TEXT("%lld"), Out) == Text;
		}
		double Number = 0.0;
		if (!Object->TryGetNumberField(Field, Number) || Number != FMath::FloorToDouble(Number) || FMath::Abs(Number) > 9.0e15)
		{
			return false;
		}
		Out = static_cast<int64>(Number);
		return true;
	}

	bool ClaimsReadChunk(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FInt64Vector& Out)
	{
		const TSharedPtr<FJsonObject>* Point = nullptr;
		return Object->TryGetObjectField(Field, Point) && ClaimsReadInt64(*Point, TEXT("x"), Out.X)
			&& ClaimsReadInt64(*Point, TEXT("y"), Out.Y) && ClaimsReadInt64(*Point, TEXT("z"), Out.Z);
	}

	TSharedPtr<FJsonObject> ClaimsChunkJson(const FInt64Vector& Chunk)
	{
		TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
		Point->SetStringField(TEXT("x"), BigInt(Chunk.X));
		Point->SetStringField(TEXT("y"), BigInt(Chunk.Y));
		Point->SetStringField(TEXT("z"), BigInt(Chunk.Z));
		return Point;
	}

	// The answer's field as an object, or null when the answer is missing it.
	TSharedPtr<FJsonObject> ClaimsAnswerObject(const FCrowdyCppJsonResult& Result, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Result.Data.IsValid() && Result.Data->TryGetObjectField(Field, Object) ? *Object : nullptr;
	}

	void FinishClaim(const TFunction<void(const FCrowdyChunkClaim&)>& OnDone, FCrowdyChunkClaim Claim, ECrowdyClaimResult Result, const FString& Message,
		const FString& Code = FString())
	{
		Claim.Result = Result;
		Claim.Message = Message;
		if (Result != ECrowdyClaimResult::Success)
		{
			Claim.GridId = 0;
		}
		ClaimsWarn(TEXT("Claim Chunk"), Result, Code);
		OnDone(Claim);
	}

	void FinishRelease(const TFunction<void(const FCrowdyClaimRelease&)>& OnDone, int64 GridId, ECrowdyClaimResult Result, const FString& Message,
		const FString& Code = FString())
	{
		FCrowdyClaimRelease Release;
		Release.Result = Result;
		Release.GridId = GridId;
		Release.Message = Message;
		ClaimsWarn(TEXT("Release Claim"), Result, Code);
		OnDone(Release);
	}

	void FinishSearch(const TFunction<void(const FCrowdyClaimSearch&)>& OnDone, ECrowdyClaimResult Result, const FString& Message, TArray<FCrowdyChunkClaimInfo> Claims,
		const FString& Code = FString())
	{
		FCrowdyClaimSearch Search;
		Search.Result = Result;
		Search.Message = Message;
		if (Result == ECrowdyClaimResult::Success)
		{
			Search.Claims = MoveTemp(Claims);
		}
		ClaimsWarn(TEXT("Find Claims"), Result, Code);
		OnDone(Search);
	}

	// Ownership reads a search keeps in flight at once: well under the 16 connections the engine's HTTP pool allows.
	constexpr int32 ClaimsReadsInFlight = 4;
}

struct UCrowdyClaims::FSearch
{
	int64 AppId = 0;
	int64 UserId = 0;
	// The one-chunk grids in grid id order, and for each whether its ownership made it a claim.
	TArray<FCrowdyChunkClaimInfo> Candidates;
	TArray<bool> bIsClaim;
	int32 NextToSend = 0;
	int32 InFlight = 0;
	int32 Answered = 0;
	bool bFinished = false;
	TFunction<void(const FCrowdyClaimSearch&)> OnDone;

	void Finish(ECrowdyClaimResult Result, const FString& Message, const FString& Code = FString())
	{
		if (bFinished)
		{
			return;
		}
		bFinished = true;
		TArray<FCrowdyChunkClaimInfo> Claims;
		for (int32 Index = 0; Result == ECrowdyClaimResult::Success && Index < Candidates.Num(); ++Index)
		{
			if (bIsClaim[Index])
			{
				Claims.Add(Candidates[Index]);
			}
		}
		FinishSearch(OnDone, Result, Message, MoveTemp(Claims), Code);
	}
};

void UCrowdyClaims::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LiveSessionToken = MakeShared<uint8>(0);
}

void UCrowdyClaims::Deinitialize()
{
	LiveSessionToken.Reset();
	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyClaims::InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId, int64 UserId)
{
	LiveSessionToken = MakeShared<uint8>(0);
	ClientForTest = Client;
	AppIdForTest = AppId;
	UserIdForTest = UserId;
}
#endif

int64 UCrowdyClaims::GetAppId() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (AppIdForTest != 0)
	{
		return AppIdForTest;
	}
#endif
	return ResolveAppId(GetGameInstance());
}

int64 UCrowdyClaims::GetUserId() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UserIdForTest != 0)
	{
		return UserIdForTest;
	}
#endif
	const UGameInstance* GameInstance = GetGameInstance();
	const UCrowdyGameSession* Session = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	return Session ? Session->GetUserID() : 0;
}

bool UCrowdyClaims::HasAppToken() const
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

FCrowdyCppClient* UCrowdyClaims::ResolveClient() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (const TSharedPtr<FCrowdyCppClient> Override = ClientForTest.Pin())
	{
		return Override.Get();
	}
#endif
	return ResolveApiClient(GetGameInstance(), ClaimsLogName);
}

void UCrowdyClaims::ClaimChunk(int64 ChunkX, int64 ChunkY, int64 ChunkZ, TFunction<void(const FCrowdyChunkClaim&)> OnDone)
{
	FCrowdyChunkClaim Asked;
	Asked.ChunkX = ChunkX;
	Asked.ChunkY = ChunkY;
	Asked.ChunkZ = ChunkZ;
	const FInt64Vector Chunk(ChunkX, ChunkY, ChunkZ);
	FCrowdyCppClient* Client = HasAppToken() ? ResolveClient() : nullptr;
	if (!Client)
	{
		const bool bSignedIn = HasAppToken();
		ClaimsDefer(LiveSessionToken, [OnDone = MoveTemp(OnDone), Asked, bSignedIn]()
		{
			FinishClaim(OnDone, Asked, bSignedIn ? ECrowdyClaimResult::Failed : ECrowdyClaimResult::NotSignedIn, bSignedIn ? ClaimsNoClient : ClaimsNotSignedIn);
		});
		return;
	}
	const int64 AppId = GetAppId();
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(AppId));
	Variables->SetObjectField(TEXT("chunk"), ClaimsChunkJson(Chunk));

	TWeakObjectPtr<UCrowdyClaims> WeakThis(this);
	ClaimsSend(*Client, ECrowdyCppApiDomain::Marketplace, TEXT("MarketplaceClaimGridChunk"), Variables, LiveSessionToken,
		[WeakThis, AppId, Asked, Chunk, OnDone = MoveTemp(OnDone)](FCrowdyCppJsonResult Result)
		{
			const UCrowdyClaims* Self = WeakThis.Get();
			if (!Self || Self->GetAppId() != AppId)
			{
				FinishClaim(OnDone, Asked, ECrowdyClaimResult::Failed, ClaimsAppChanged);
				return;
			}
			if (!Result.bTransportOk)
			{
				FinishClaim(OnDone, Asked, ClaimsFailureResult(Result), ClaimsFailureMessage(Result), ClaimsFailureCode(Result));
				return;
			}
			const TSharedPtr<FJsonObject> Answer = ClaimsAnswerObject(Result, TEXT("claimGridChunk"));
			FCrowdyChunkClaim Claim = Asked;
			FInt64Vector Low;
			FInt64Vector High;
			if (!Answer || !ClaimsReadInt64(Answer, TEXT("gridId"), Claim.GridId) || Claim.GridId <= 0
				|| !ClaimsReadChunk(Answer, TEXT("lowChunk"), Low) || !ClaimsReadChunk(Answer, TEXT("highChunk"), High))
			{
				FinishClaim(OnDone, Asked, ECrowdyClaimResult::BadReply, TEXT("The answer holds no well-formed claim."));
				return;
			}
			if (Low != Chunk || High != Chunk)
			{
				FinishClaim(OnDone, Asked, ECrowdyClaimResult::BadReply, TEXT("The platform answered a claim over a different chunk than the one asked for."));
				return;
			}
			FinishClaim(OnDone, Claim, ECrowdyClaimResult::Success, FString());
		});
}

void UCrowdyClaims::ReleaseClaim(int64 GridId, TFunction<void(const FCrowdyClaimRelease&)> OnDone)
{
	FCrowdyCppClient* Client = HasAppToken() ? ResolveClient() : nullptr;
	if (!Client)
	{
		const bool bSignedIn = HasAppToken();
		ClaimsDefer(LiveSessionToken, [OnDone = MoveTemp(OnDone), GridId, bSignedIn]()
		{
			FinishRelease(OnDone, GridId, bSignedIn ? ECrowdyClaimResult::Failed : ECrowdyClaimResult::NotSignedIn, bSignedIn ? ClaimsNoClient : ClaimsNotSignedIn);
		});
		return;
	}
	const int64 AppId = GetAppId();
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(AppId));
	Variables->SetStringField(TEXT("gridId"), BigInt(GridId));

	TWeakObjectPtr<UCrowdyClaims> WeakThis(this);
	ClaimsSend(*Client, ECrowdyCppApiDomain::Marketplace, TEXT("MarketplaceReleaseClaimedGrid"), Variables, LiveSessionToken,
		[WeakThis, AppId, GridId, OnDone = MoveTemp(OnDone)](FCrowdyCppJsonResult Result)
		{
			const UCrowdyClaims* Self = WeakThis.Get();
			if (!Self || Self->GetAppId() != AppId)
			{
				FinishRelease(OnDone, GridId, ECrowdyClaimResult::Failed, ClaimsAppChanged);
				return;
			}
			if (!Result.bTransportOk)
			{
				FinishRelease(OnDone, GridId, ClaimsFailureResult(Result), ClaimsFailureMessage(Result), ClaimsFailureCode(Result));
				return;
			}
			const TSharedPtr<FJsonObject> Answer = ClaimsAnswerObject(Result, TEXT("releaseClaimedGrid"));
			int64 Echoed = 0;
			bool bReleased = false;
			if (!Answer || !ClaimsReadInt64(Answer, TEXT("gridId"), Echoed) || Echoed != GridId || !Answer->TryGetBoolField(TEXT("released"), bReleased))
			{
				FinishRelease(OnDone, GridId, ECrowdyClaimResult::BadReply, TEXT("The answer holds no well-formed release of that grid."));
				return;
			}
			if (!bReleased)
			{
				FinishRelease(OnDone, GridId, ECrowdyClaimResult::Refused, TEXT("The platform did not release the grid."));
				return;
			}
			FinishRelease(OnDone, GridId, ECrowdyClaimResult::Success, FString());
		});
}

void UCrowdyClaims::FindClaims(int64 LowX, int64 LowY, int64 LowZ, int64 HighX, int64 HighY, int64 HighZ, TFunction<void(const FCrowdyClaimSearch&)> OnDone)
{
	const FInt64Vector Low(LowX, LowY, LowZ);
	const FInt64Vector High(HighX, HighY, HighZ);
	// Each side is measured before the product, so a box spanning the whole int64 range cannot overflow into a small one.
	const bool bOrdered = Low.X <= High.X && Low.Y <= High.Y && Low.Z <= High.Z;
	const uint64 SideX = bOrdered ? static_cast<uint64>(High.X) - static_cast<uint64>(Low.X) + 1 : 0;
	const uint64 SideY = bOrdered ? static_cast<uint64>(High.Y) - static_cast<uint64>(Low.Y) + 1 : 0;
	const uint64 SideZ = bOrdered ? static_cast<uint64>(High.Z) - static_cast<uint64>(Low.Z) + 1 : 0;
	const uint64 Most = static_cast<uint64>(MaxSearchChunks);
	const bool bBoxFits = bOrdered && SideX > 0 && SideY > 0 && SideZ > 0 && SideX <= Most && SideY <= Most && SideZ <= Most && SideX * SideY * SideZ <= Most;
	FCrowdyCppClient* Client = bBoxFits && HasAppToken() ? ResolveClient() : nullptr;
	if (!Client)
	{
		const ECrowdyClaimResult Result = bBoxFits && !HasAppToken() ? ECrowdyClaimResult::NotSignedIn : ECrowdyClaimResult::Failed;
		const FString Message = !bBoxFits ? FString::Printf(TEXT("The box must be ordered low to high and hold at most %lld chunks."), MaxSearchChunks)
			: Result == ECrowdyClaimResult::NotSignedIn ? FString(ClaimsNotSignedIn) : FString(ClaimsNoClient);
		ClaimsDefer(LiveSessionToken, [OnDone = MoveTemp(OnDone), Result, Message]()
		{
			FinishSearch(OnDone, Result, Message, {});
		});
		return;
	}
	const TSharedRef<FSearch> Search = MakeShared<FSearch>();
	Search->AppId = GetAppId();
	Search->UserId = GetUserId();
	Search->OnDone = MoveTemp(OnDone);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), BigInt(Search->AppId));
	Input->SetObjectField(TEXT("lowChunk"), ClaimsChunkJson(Low));
	Input->SetObjectField(TEXT("highChunk"), ClaimsChunkJson(High));

	TWeakObjectPtr<UCrowdyClaims> WeakThis(this);
	ClaimsSend(*Client, ECrowdyCppApiDomain::GameApps, TEXT("NearbyGrids"), WrapInput(Input), LiveSessionToken,
		[WeakThis, Search, Low, High](FCrowdyCppJsonResult Result)
		{
			UCrowdyClaims* Self = WeakThis.Get();
			if (!Self || Self->GetAppId() != Search->AppId)
			{
				Search->Finish(ECrowdyClaimResult::Failed, ClaimsAppChanged);
				return;
			}
			if (!Result.bTransportOk)
			{
				Search->Finish(ClaimsFailureResult(Result), ClaimsFailureMessage(Result), ClaimsFailureCode(Result));
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* Grids = nullptr;
			if (!Result.Data.IsValid() || !Result.Data->TryGetArrayField(TEXT("nearbyGrids"), Grids) || Grids->Num() > ClaimsMaxGridsListed)
			{
				Search->Finish(ECrowdyClaimResult::BadReply, TEXT("The answer holds no well-formed grid list."));
				return;
			}
			TSet<int64> Seen;
			for (const TSharedPtr<FJsonValue>& Value : *Grids)
			{
				const TSharedPtr<FJsonObject>* Grid = nullptr;
				FCrowdyChunkClaimInfo Info;
				FInt64Vector GridLow;
				FInt64Vector GridHigh;
				if (!Value.IsValid() || !Value->TryGetObject(Grid) || !ClaimsReadInt64(*Grid, TEXT("gridId"), Info.GridId)
					|| !ClaimsReadChunk(*Grid, TEXT("lowChunk"), GridLow) || !ClaimsReadChunk(*Grid, TEXT("highChunk"), GridHigh))
				{
					Search->Finish(ECrowdyClaimResult::BadReply, TEXT("The answer holds a malformed grid."));
					return;
				}
				// Only a grid over exactly one chunk inside the box can be a claim there; wider grids are not read further.
				const bool bOneChunk = GridLow == GridHigh;
				const bool bInside = GridLow.X >= Low.X && GridLow.X <= High.X && GridLow.Y >= Low.Y && GridLow.Y <= High.Y
					&& GridLow.Z >= Low.Z && GridLow.Z <= High.Z;
				bool bAlready = false;
				if (!bOneChunk || !bInside || Info.GridId <= 0)
				{
					continue;
				}
				Seen.Add(Info.GridId, &bAlready);
				if (bAlready)
				{
					continue;
				}
				Info.ChunkX = GridLow.X;
				Info.ChunkY = GridLow.Y;
				Info.ChunkZ = GridLow.Z;
				Search->Candidates.Add(Info);
			}
			if (Search->Candidates.Num() > MaxSearchChunks)
			{
				Search->Finish(ECrowdyClaimResult::BadReply, TEXT("The platform answered more one-chunk grids than the box holds."));
				return;
			}
			Search->Candidates.Sort([](const FCrowdyChunkClaimInfo& A, const FCrowdyChunkClaimInfo& B) { return A.GridId < B.GridId; });
			Search->bIsClaim.Init(false, Search->Candidates.Num());
			Self->ReadOwners(Search);
		});
}

void UCrowdyClaims::ReadOwners(const TSharedRef<FSearch>& Search)
{
	if (Search->bFinished)
	{
		return;
	}
	if (Search->Answered == Search->Candidates.Num())
	{
		Search->Finish(ECrowdyClaimResult::Success, FString());
		return;
	}
	FCrowdyCppClient* Client = Search->NextToSend < Search->Candidates.Num() ? ResolveClient() : nullptr;
	if (!Client && Search->NextToSend < Search->Candidates.Num())
	{
		Search->Finish(ECrowdyClaimResult::Failed, ClaimsNoClient);
		return;
	}
	TWeakObjectPtr<UCrowdyClaims> WeakThis(this);
	while (Client && Search->InFlight < ClaimsReadsInFlight && Search->NextToSend < Search->Candidates.Num())
	{
		const int32 Index = Search->NextToSend++;
		++Search->InFlight;
		const int64 GridId = Search->Candidates[Index].GridId;
		TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetStringField(TEXT("appId"), BigInt(Search->AppId));
		Variables->SetStringField(TEXT("gridId"), BigInt(GridId));
		ClaimsSend(*Client, ECrowdyCppApiDomain::GameApps, TEXT("GridOwnership"), Variables, LiveSessionToken,
			[WeakThis, Search, Index, GridId](FCrowdyCppJsonResult Result)
			{
				UCrowdyClaims* Self = WeakThis.Get();
				--Search->InFlight;
				if (Search->bFinished)
				{
					return;
				}
				if (!Self || Self->GetAppId() != Search->AppId)
				{
					Search->Finish(ECrowdyClaimResult::Failed, ClaimsAppChanged);
					return;
				}
				FString Problem;
				if (!Self->ReadOwner(*Search, Index, GridId, Result, Problem))
				{
					Search->Finish(Result.bTransportOk ? ECrowdyClaimResult::BadReply : ClaimsFailureResult(Result), Problem, ClaimsFailureCode(Result));
					return;
				}
				++Search->Answered;
				Self->ReadOwners(Search);
			});
	}
}

bool UCrowdyClaims::ReadOwner(FSearch& Search, int32 Index, int64 GridId, const FCrowdyCppJsonResult& Result, FString& OutProblem) const
{
	if (!Result.bTransportOk)
	{
		OutProblem = ClaimsFailureMessage(Result);
		return false;
	}
	if (!Result.Data.IsValid() || !Result.Data->HasField(TEXT("gridOwnership")))
	{
		OutProblem = TEXT("The answer holds no ownership field.");
		return false;
	}
	// A null ownership is a grid nobody owns, so it is not a claim.
	const TSharedPtr<FJsonObject> Ownership = ClaimsAnswerObject(Result, TEXT("gridOwnership"));
	int64 Echoed = GridId;
	if (Ownership && Ownership->HasField(TEXT("gridId")) && (!ClaimsReadInt64(Ownership, TEXT("gridId"), Echoed) || Echoed != GridId))
	{
		OutProblem = TEXT("The platform answered the owner of a different grid.");
		return false;
	}
	FString Kind;
	FString Via;
	int64 Owner = 0;
	const bool bClaim = Ownership && Ownership->TryGetStringField(TEXT("ownerKind"), Kind) && Kind == TEXT("USER")
		&& Ownership->TryGetStringField(TEXT("acquiredVia"), Via) && Via == TEXT("self_claim_chunk")
		&& ClaimsReadInt64(Ownership, TEXT("ownerRef"), Owner) && Owner > 0;
	if (bClaim)
	{
		FCrowdyChunkClaimInfo& Info = Search.Candidates[Index];
		Info.OwnerUserId = Owner;
		Info.bOwnedByMe = Search.UserId != 0 && Owner == Search.UserId;
		Search.bIsClaim[Index] = true;
	}
	return true;
}
