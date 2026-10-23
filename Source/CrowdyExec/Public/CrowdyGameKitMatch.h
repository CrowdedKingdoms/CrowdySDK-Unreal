#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CrowdyGameKitMatch.generated.h"

class UCrowdyServerObject;
struct FCrowdyKitMatchPlace;
struct FCrowdyServerCallResult;

/** Where a Find Match search is. */
UENUM(BlueprintType)
enum class ECrowdyKitMatchStage : uint8
{
	/** Waiting in the queue for a match. */
	Queued,
	/** In a proposed match that every player must accept. */
	Proposed,
	/** Matched: Session is the Instance Id of the Kit Session reserved for the match. */
	Matched,
	/** Ended without a match: Code and Reason say why. */
	Failed
};

/** One update of a Find Match search. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMatchProgress
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitMatchStage Stage = ECrowdyKitMatchStage::Queued;

	/** Proposed: the match's players by user id. */
	UPROPERTY(BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Players;

	/** Proposed: the players who have accepted. */
	UPROPERTY(BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Accepted;

	/** Matched: the Instance Id of the Kit Session reserved for the match. */
	UPROPERTY(BlueprintReadOnly, Category = "Game Kit")
	FString Session;

	/** Failed: the matchmaker's refusal code, or cancelled, timeout, expired or left; None when a call failed for another reason. */
	UPROPERTY(BlueprintReadOnly, Category = "Game Kit")
	FName Code;

	UPROPERTY(BlueprintReadOnly, Category = "Game Kit")
	FString Reason;
};

/**
 * One player's search for a match in a Game Kit matchmaker queue: joins it, asks its status every second (the matchmaker
 * pushes nothing to a player), accepts a proposed match when asked to, and ends once Matched or Failed, leaving the queue when
 * Failed. A player who had accepted a match that fell through joins again; one who did not accept in time ends with Code
 * expired. It keeps itself alive until it ends. Game thread only.
 */
class CROWDYEXEC_API FCrowdyKitMatchSearch : public TSharedFromThis<FCrowdyKitMatchSearch>
{
public:
	using FOnProgress = TFunction<void(const FCrowdyKitMatchProgress&)>;

	/** Starts a search; OnProgress runs on each change of stage or of a proposal, last with Matched or Failed. */
	static TSharedRef<FCrowdyKitMatchSearch> Start(UCrowdyServerObject* Matchmaker, int32 Rating, bool bAutoAccept, float TimeoutSeconds, FOnProgress OnProgress);

	/** Accepts the match proposed now; nothing when none is or it was already accepted. */
	void Accept();

	/** Leaves the queue and ends the search with Code cancelled, once a call already out is answered (Matched if it made the match); nothing once it ended. */
	void Cancel();

	bool IsOver() const { return bOver; }

	/** Runs the search's next step at Now, as its ticker does; false once it ended. */
	bool Tick(double Now);

#if WITH_DEV_AUTOMATION_TESTS
	bool IsCallOutForTest() const { return bCallOut; }
#endif

	FCrowdyKitMatchSearch(UCrowdyServerObject* Matchmaker, int32 Rating, bool bAutoAccept, float TimeoutSeconds, FOnProgress OnProgress);

private:
	enum class ENext : uint8
	{
		Join,
		Accept,
		Status
	};

	void Send(UCrowdyServerObject& Object, ENext What);
	void Stop(FName Code, const FString& Reason);
	void HandleAnswer(ENext What, const FCrowdyServerCallResult& Result);
	void HandlePlace(const FCrowdyKitMatchPlace& Place, bool bAnswersAccept);
	void Report(ECrowdyKitMatchStage Stage);
	void Leave();
	void Finish(ECrowdyKitMatchStage Stage, FName Code, const FString& Reason, const FString& Session = FString());

	TWeakObjectPtr<UCrowdyServerObject> Matchmaker;
	FOnProgress OnProgress;
	FTSTicker::FDelegateHandle TickerHandle;
	FName JoinFunction;
	FName StatusFunction;
	FName AcceptFunction;
	FName LeaveFunction;
	// Why the search cannot start, reported on its first tick.
	FString Problem;
	// Cancel or the timeout while a call was out: how the search ends once it is answered.
	FName StopCode;
	FString StopReason;
	TArray<int64> Players;
	TArray<int64> Accepted;
	double TimeoutSeconds = 0.0;
	double Deadline = 0.0;
	double NextPoll = 0.0;
	// The time Tick last ran at, from which the next status is asked.
	double Clock = 0.0;
	int64 Me = 0;
	int32 Rating = 0;
	int32 FailedPolls = 0;
	ENext Next = ENext::Join;
	TOptional<ECrowdyKitMatchStage> Reported;
	bool bAutoAccept = false;
	bool bCallOut = false;
	bool bProposed = false;
	bool bAcceptedHere = false;
	bool bOver = false;
};

namespace CrowdyKit
{
	/** Starts a search for a match in the queue Matchmaker holds (a CSO_Kit_Matchmaker Server Object): see FCrowdyKitMatchSearch. TimeoutSeconds 0 waits for as long as it takes. */
	inline TSharedRef<FCrowdyKitMatchSearch> FindMatch(UCrowdyServerObject* Matchmaker, int32 Rating, bool bAutoAccept, float TimeoutSeconds, FCrowdyKitMatchSearch::FOnProgress OnProgress)
	{
		return FCrowdyKitMatchSearch::Start(Matchmaker, Rating, bAutoAccept, TimeoutSeconds, MoveTemp(OnProgress));
	}

	/** The user id of the player whose turn it is in a Kit Session while it plays; false in the lobby, once finished, before it is read, or for an object whose variables are not FCrowdyKitSessionState. */
	CROWDYEXEC_API bool GetTurnPlayer(const UCrowdyServerObject* Session, int64& OutPlayer);

	/** Whether it is the signed-in player's turn in a Kit Session. */
	CROWDYEXEC_API bool IsMyTurn(const UCrowdyServerObject* Session);
}

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FCrowdyFindMatchUpdate, const TArray<int64>&, Players, const TArray<int64>&, Accepted, const FString&, SessionId,
	FName, Code, const FString&, Reason);

/** Find Match: a player's search for a match in a Game Kit matchmaker queue, from joining it to the session the match reserved. */
UCLASS(meta = (ExposedAsyncProxy = "Search"))
class CROWDYEXEC_API UCrowdyFindMatchAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/**
	 * Joins the queue Matchmaker holds (the CSO_Kit_Matchmaker Server Object for the queue's name) with Rating and waits for a match,
	 * asking its status every second. Queued runs when the player waits in the queue, Proposed when a match is proposed or who has
	 * accepted it changes, Matched once with the Session Id of the Kit Session reserved for the match, and Failed once with a Code:
	 * the matchmaker's refusal (such as full), cancelled, timeout (after Timeout seconds; 0 for none), expired (a proposed match this
	 * player did not accept in time) or left (the player left the queue another way). With Auto Accept the node accepts each proposed
	 * match itself; without it, call Accept on the Search pin. A match that falls through after this player accepted is searched for
	 * again, and a matchmaker that cannot reserve a session puts its players back in the queue, so give the search a Timeout. Every
	 * ending but Matched leaves the queue; Cancel and the Timeout wait for a call already out, and end Matched if it made the match.
	 * Matchmaker must stay held by its Owner while the search runs.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Kit", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Find Match", Keywords = "matchmaking queue accept game kit"))
	static UCrowdyFindMatchAction* FindMatch(UObject* WorldContextObject, UCrowdyServerObject* Matchmaker, int32 Rating = 1000, bool bAutoAccept = true, float Timeout = 0.f);

	/** Accepts the match proposed now, for a search without Auto Accept. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Kit")
	void Accept();

	/** Leaves the queue; Failed runs with Code cancelled. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Kit")
	void Cancel();

	UPROPERTY(BlueprintAssignable)
	FCrowdyFindMatchUpdate Queued;

	UPROPERTY(BlueprintAssignable)
	FCrowdyFindMatchUpdate Proposed;

	UPROPERTY(BlueprintAssignable)
	FCrowdyFindMatchUpdate Matched;

	UPROPERTY(BlueprintAssignable)
	FCrowdyFindMatchUpdate Failed;

	virtual void Activate() override;

#if WITH_DEV_AUTOMATION_TESTS
	FCrowdyKitMatchSearch* GetSearchForTest() const { return Search.Get(); }
#endif

private:
	void HandleProgress(const FCrowdyKitMatchProgress& Progress);

	UPROPERTY()
	TObjectPtr<UCrowdyServerObject> MatchmakerObject;

	TSharedPtr<FCrowdyKitMatchSearch> Search;
	int32 Rating = 0;
	float Timeout = 0.f;
	bool bAutoAccept = true;
};

/** Turns in a Game Kit session. */
UCLASS()
class CROWDYEXEC_API UCrowdyGameKitLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Whether it is the signed-in player's turn in Session, a CSO_Kit_Session Server Object; false in the lobby, once it finished, and before it is read. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Kit", meta = (DisplayName = "Is My Turn"))
	static bool IsMyTurn(const UCrowdyServerObject* Session);

	/** The user id of the player whose turn it is in Session, a CSO_Kit_Session Server Object; false, with Player 0, in the lobby, once it finished, and before it is read. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Kit", meta = (DisplayName = "Get Turn Player"))
	static bool GetTurnPlayer(const UCrowdyServerObject* Session, int64& Player);
};
