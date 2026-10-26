#include "CrowdyGameKitMatch.h"

#include "CrowdyGameKitTypes.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectTypes.h"
#include "HAL/PlatformTime.h"

namespace CrowdyGameKitMatchDetail
{
	// The matchmaker forms proposals on a one-second timer, so asking more often learns nothing sooner.
	constexpr double PollSeconds = 1.0;
	constexpr int32 MaxFailedPolls = 5;

	FName FindFunction(const UCrowdyServerObjectDefinition& Definition, const TCHAR* Method)
	{
		const FCrowdyServerFunction* Found = Definition.Functions.FindByPredicate([Method](const FCrowdyServerFunction& Function)
		{
			return Function.GetMethodName().Equals(Method, ESearchCase::CaseSensitive);
		});
		return Found ? Found->Name : NAME_None;
	}
}

FCrowdyKitMatchSearch::FCrowdyKitMatchSearch(UCrowdyServerObject* InMatchmaker, int32 InRating, bool bInAutoAccept, float InTimeoutSeconds, FOnProgress InOnProgress)
	: Matchmaker(InMatchmaker)
	, OnProgress(MoveTemp(InOnProgress))
	, TimeoutSeconds(InTimeoutSeconds)
	, Rating(InRating)
	, bAutoAccept(bInAutoAccept)
{
}

TSharedRef<FCrowdyKitMatchSearch> FCrowdyKitMatchSearch::Start(UCrowdyServerObject* Matchmaker, int32 Rating, bool bAutoAccept, float TimeoutSeconds, FOnProgress OnProgress)
{
	using namespace CrowdyGameKitMatchDetail;
	TSharedRef<FCrowdyKitMatchSearch> Search = MakeShared<FCrowdyKitMatchSearch>(Matchmaker, Rating, bAutoAccept, TimeoutSeconds, MoveTemp(OnProgress));
	const UCrowdyServerObjectDefinition* Definition = Matchmaker ? Matchmaker->GetDefinition() : nullptr;
	if (Definition)
	{
		Search->JoinFunction = FindFunction(*Definition, TEXT("join"));
		Search->StatusFunction = FindFunction(*Definition, TEXT("status"));
		Search->AcceptFunction = FindFunction(*Definition, TEXT("accept"));
		Search->LeaveFunction = FindFunction(*Definition, TEXT("leave"));
	}
	if (!Definition)
	{
		Search->Problem = TEXT("Find Match needs the matchmaker queue's Server Object");
	}
	else if (Search->JoinFunction.IsNone() || Search->StatusFunction.IsNone() || Search->AcceptFunction.IsNone() || Search->LeaveFunction.IsNone())
	{
		Search->Problem = FString::Printf(TEXT("%s has no join, status, accept and leave functions; Find Match needs a Game Kit matchmaker"), *Definition->GetName());
	}
	const double Now = FPlatformTime::Seconds();
	Search->Deadline = TimeoutSeconds > 0.f ? Now + TimeoutSeconds : 0.0;
	// The ticker holds the search until it ends, so a caller may let its handle go.
	Search->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Search](float)
	{
		return Search->Tick(FPlatformTime::Seconds());
	}));
	// A problem is reported on the first tick, so the Blueprint node's Failed never runs before the node's own output.
	if (Search->Problem.IsEmpty())
	{
		Search->Tick(Now);
	}
	return Search;
}

void FCrowdyKitMatchSearch::Accept()
{
	if (!bOver && bProposed && !bAcceptedHere)
	{
		Next = ENext::Accept;
	}
}

void FCrowdyKitMatchSearch::Cancel()
{
	if (!bOver)
	{
		Stop(TEXT("cancelled"), TEXT("Find Match was cancelled"));
	}
}

void FCrowdyKitMatchSearch::Stop(FName Code, const FString& Reason)
{
	if (!StopCode.IsNone())
	{
		return;
	}
	// A call still out could queue the player again or make the match, so the search ends when it is answered.
	if (bCallOut)
	{
		StopCode = Code;
		StopReason = Reason;
		return;
	}
	Finish(ECrowdyKitMatchStage::Failed, Code, Reason);
}

bool FCrowdyKitMatchSearch::Tick(double Now)
{
	if (bOver)
	{
		return false;
	}
	if (!Problem.IsEmpty())
	{
		Finish(ECrowdyKitMatchStage::Failed, NAME_None, Problem);
		return false;
	}
	Clock = Now;
	UCrowdyServerObject* Object = Matchmaker.Get();
	if (!Object)
	{
		Finish(ECrowdyKitMatchStage::Failed, NAME_None, TEXT("The matchmaker queue's Server Object was given back before Find Match ended"));
		return false;
	}
	if (Deadline > 0.0 && Now >= Deadline)
	{
		Stop(TEXT("timeout"), FString::Printf(TEXT("No match within %g seconds"), TimeoutSeconds));
	}
	if (bOver || bCallOut || !StopCode.IsNone() || (Next == ENext::Status && Now < NextPoll))
	{
		return !bOver;
	}
	Send(*Object, Next);
	return !bOver;
}

void FCrowdyKitMatchSearch::Send(UCrowdyServerObject& Object, ENext What)
{
	const FName Function = What == ENext::Join ? JoinFunction : What == ENext::Accept ? AcceptFunction : StatusFunction;
	FInstancedStruct Inputs = Object.MakeInputs(Function);
	if (FCrowdyKitMatchJoinInputs* Join = Inputs.GetMutablePtr<FCrowdyKitMatchJoinInputs>())
	{
		Join->Rating = Rating;
	}
	bCallOut = true;
	const TWeakPtr<FCrowdyKitMatchSearch> Weak = AsShared();
	Object.Call(Function, Inputs, [Weak, What](const FCrowdyServerCallResult& Result)
	{
		if (const TSharedPtr<FCrowdyKitMatchSearch> Search = Weak.Pin())
		{
			Search->HandleAnswer(What, Result);
		}
	});
}

void FCrowdyKitMatchSearch::HandleAnswer(ENext What, const FCrowdyServerCallResult& Result)
{
	using namespace CrowdyGameKitMatchDetail;
	bCallOut = false;
	if (bOver)
	{
		return;
	}
	if (const UCrowdyServerObject* Object = Matchmaker.Get())
	{
		Me = Object->GetSignedInUserId();
	}
	const FCrowdyKitMatchPlace* Place = Result.IsSuccess() ? Result.Reply.GetPtr<FCrowdyKitMatchPlace>() : nullptr;
	if (!StopCode.IsNone())
	{
		// A match the call made counts; anything else ends the search as it was asked to.
		if (Place && Place->State == ECrowdyKitMatchState::Matched)
		{
			Finish(ECrowdyKitMatchStage::Matched, NAME_None, FString(), Place->Session);
			return;
		}
		Finish(ECrowdyKitMatchStage::Failed, StopCode, StopReason);
		return;
	}
	NextPoll = Clock + PollSeconds;
	// An Accept asked for while this call was out is still to send.
	if (Next == What)
	{
		Next = ENext::Status;
	}
	if (Result.IsSuccess())
	{
		FailedPolls = 0;
		if (!Place)
		{
			Finish(ECrowdyKitMatchStage::Failed, NAME_None, TEXT("The matchmaker's answer is not an FCrowdyKitMatchPlace"));
			return;
		}
		HandlePlace(*Place, What == ENext::Accept);
		return;
	}
	// An accept the proposal did not outlive; the next status says what became of it.
	if (What == ENext::Accept && Result.Code == FName(TEXT("not_found")))
	{
		return;
	}
	if (What == ENext::Status && Result.bRetryable && ++FailedPolls < MaxFailedPolls)
	{
		return;
	}
	Finish(ECrowdyKitMatchStage::Failed, Result.Code, Result.Reason);
}

void FCrowdyKitMatchSearch::HandlePlace(const FCrowdyKitMatchPlace& Place, bool bAnswersAccept)
{
	switch (Place.State)
	{
	case ECrowdyKitMatchState::Queued:
		bProposed = false;
		bAcceptedHere = false;
		if (Reported != ECrowdyKitMatchStage::Queued)
		{
			Report(ECrowdyKitMatchStage::Queued);
		}
		return;
	case ECrowdyKitMatchState::Proposed:
	{
		const bool bSameProposal = bProposed && Players == Place.Players;
		const bool bChanged = !bSameProposal || Accepted != Place.Accepted;
		bProposed = true;
		// An accept's own answer counts too, for a player whose user id the client does not know yet.
		bAcceptedHere = (bSameProposal && bAcceptedHere) || bAnswersAccept || (Me != 0 && Place.Accepted.Contains(Me));
		Players = Place.Players;
		Accepted = Place.Accepted;
		if (bChanged)
		{
			Report(ECrowdyKitMatchStage::Proposed);
		}
		if (bAutoAccept && !bAcceptedHere)
		{
			Next = ENext::Accept;
		}
		return;
	}
	case ECrowdyKitMatchState::Matched:
		Finish(ECrowdyKitMatchStage::Matched, NAME_None, FString(), Place.Session);
		return;
	case ECrowdyKitMatchState::Idle:
		// A player who accepted is put back in the queue when a match falls through, but one left idle all the same joins it again.
		if (bProposed && bAcceptedHere)
		{
			bProposed = false;
			bAcceptedHere = false;
			Next = ENext::Join;
			return;
		}
		if (bProposed)
		{
			Finish(ECrowdyKitMatchStage::Failed, TEXT("expired"), TEXT("A match was proposed and this player did not accept it in time"));
			return;
		}
		Finish(ECrowdyKitMatchStage::Failed, TEXT("left"), TEXT("This player is no longer in the queue"));
		return;
	default:
		return;
	}
}

void FCrowdyKitMatchSearch::Report(ECrowdyKitMatchStage Stage)
{
	Reported = Stage;
	FCrowdyKitMatchProgress Progress;
	Progress.Stage = Stage;
	Progress.Players = Players;
	Progress.Accepted = Accepted;
	// A copy, since the handler may cancel the search and so reset it.
	const FOnProgress Callback = OnProgress;
	if (Callback)
	{
		Callback(Progress);
	}
}

void FCrowdyKitMatchSearch::Leave()
{
	if (UCrowdyServerObject* Object = Matchmaker.Get(); Object && !LeaveFunction.IsNone())
	{
		Object->Call(LeaveFunction, Object->MakeInputs(LeaveFunction), [](const FCrowdyServerCallResult&) {});
	}
}

void FCrowdyKitMatchSearch::Finish(ECrowdyKitMatchStage Stage, FName Code, const FString& Reason, const FString& Session)
{
	// A search that ends without a match leaves no player behind in the queue for others to be proposed with.
	if (Stage == ECrowdyKitMatchStage::Failed)
	{
		Leave();
	}
	bOver = true;
	Reported = Stage;
	FCrowdyKitMatchProgress Progress;
	Progress.Stage = Stage;
	Progress.Players = Players;
	Progress.Accepted = Accepted;
	Progress.Session = Session;
	Progress.Code = Code;
	Progress.Reason = Reason;
	const FOnProgress Callback = MoveTemp(OnProgress);
	OnProgress.Reset();
	if (Callback)
	{
		Callback(Progress);
	}
}

bool CrowdyKit::GetTurnPlayer(const UCrowdyServerObject* Session, int64& OutPlayer)
{
	OutPlayer = 0;
	const FCrowdyKitSessionState* State = Session ? Session->GetState().GetPtr<FCrowdyKitSessionState>() : nullptr;
	if (!State || State->Phase != ECrowdyKitSessionPhase::Playing || !State->Players.IsValidIndex(State->Turn))
	{
		return false;
	}
	OutPlayer = State->Players[State->Turn];
	return true;
}

bool CrowdyKit::IsMyTurn(const UCrowdyServerObject* Session)
{
	int64 Player = 0;
	return GetTurnPlayer(Session, Player) && Player != 0 && Player == Session->GetSignedInUserId();
}

UCrowdyFindMatchAction* UCrowdyFindMatchAction::FindMatch(UObject* WorldContextObject, UCrowdyServerObject* Matchmaker, int32 Rating, bool bAutoAccept, float Timeout)
{
	UCrowdyFindMatchAction* Action = NewObject<UCrowdyFindMatchAction>();
	Action->MatchmakerObject = Matchmaker;
	Action->Rating = Rating;
	Action->bAutoAccept = bAutoAccept;
	Action->Timeout = Timeout;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyFindMatchAction::Activate()
{
	const TWeakObjectPtr<UCrowdyFindMatchAction> Weak = this;
	Search = CrowdyKit::FindMatch(MatchmakerObject, Rating, bAutoAccept, Timeout, [Weak](const FCrowdyKitMatchProgress& Progress)
	{
		if (UCrowdyFindMatchAction* Action = Weak.Get())
		{
			Action->HandleProgress(Progress);
		}
	});
}

void UCrowdyFindMatchAction::Accept()
{
	if (Search.IsValid())
	{
		Search->Accept();
	}
}

void UCrowdyFindMatchAction::Cancel()
{
	if (Search.IsValid())
	{
		Search->Cancel();
	}
}

void UCrowdyFindMatchAction::HandleProgress(const FCrowdyKitMatchProgress& Progress)
{
	switch (Progress.Stage)
	{
	case ECrowdyKitMatchStage::Queued:
		Queued.Broadcast(Progress.Players, Progress.Accepted, Progress.Session, Progress.Code, Progress.Reason);
		return;
	case ECrowdyKitMatchStage::Proposed:
		Proposed.Broadcast(Progress.Players, Progress.Accepted, Progress.Session, Progress.Code, Progress.Reason);
		return;
	case ECrowdyKitMatchStage::Matched:
		Matched.Broadcast(Progress.Players, Progress.Accepted, Progress.Session, Progress.Code, Progress.Reason);
		break;
	case ECrowdyKitMatchStage::Failed:
		Failed.Broadcast(Progress.Players, Progress.Accepted, Progress.Session, Progress.Code, Progress.Reason);
		break;
	}
	SetReadyToDestroy();
}

bool UCrowdyGameKitLibrary::IsMyTurn(const UCrowdyServerObject* Session)
{
	return CrowdyKit::IsMyTurn(Session);
}

bool UCrowdyGameKitLibrary::GetTurnPlayer(const UCrowdyServerObject* Session, int64& Player)
{
	return CrowdyKit::GetTurnPlayer(Session, Player);
}
