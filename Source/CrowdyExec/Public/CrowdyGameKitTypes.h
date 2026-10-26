#pragma once

#include "CoreMinimal.h"
#include "CrowdyGameKitTypes.generated.h"

/** The world's weather, as the Game Kit's world starter rolls it. */
UENUM(BlueprintType)
enum class ECrowdyKitWeather : uint8
{
	Clear,
	Rain,
	Storm,
	Fog
};

/** What the Game Kit's world starter shows every player on its world topic: the variables of Kit World. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitWorldConditions
{
	GENERATED_BODY()

	/** 0 at midnight, 0.5 at noon. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double TimeOfDay = 0.0;

	/** The first and last quarter of the game day. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bNight = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitWeather Weather = ECrowdyKitWeather::Clear;

	/** The running liveops events' modifiers by name, such as xp or aggro; where events overlap they multiply. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, double> Modifiers;

	/** Players in the app's world, as the platform reports them to the root; Get Players Online reads it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Online = 0;

	/** False when the world's snapshot carries no player count: a world deployed from the platform's own starter pack sends none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasOnline = false;
};

/** One resource node of the world: what it holds, and when an emptied one grows back. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitWorldNode
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Id = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Amount = 0;

	/** When it grows back, in Unix milliseconds; 0 while it holds something. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 RegrowAt = 0;
};

/** A liveops event: between Start Ms and End Ms (Unix milliseconds) its modifiers apply. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitWorldEvent
{
	GENERATED_BODY()

	/** 1 to 64 characters; scheduling an event of the same name replaces it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 StartMs = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 EndMs = 0;

	/** At most 16, each above 0 and at most 100. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	TMap<FString, double> Modifiers;
};

/** Everything the world shows, as its Get State answers. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitWorldView
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitWorldConditions Conditions;

	/** Game time since the world began, advanced only while it runs. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 ClockMs = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitWorldNode> Nodes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitWorldEvent> Events;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Online = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitHarvestInputs
{
	GENERATED_BODY()

	/** The resource node's id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 Node = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitHarvestReply
{
	GENERATED_BODY()

	/** What the node holds after the harvest. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Left = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitCancelEventInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Name;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitCancelEventReply
{
	GENERATED_BODY()

	/** Whether an event of that name was scheduled. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bRemoved = false;
};

/** How many a matchmaking queue holds, never who: the variables of Kit Matchmaker. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitQueueCounts
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Waiting = 0;

	/** Matches proposed and waiting for their players to accept. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Proposals = 0;

	/** Parties whose members are still confirming. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Forming = 0;
};

/** Where the player stands in a matchmaking queue. */
UENUM(BlueprintType)
enum class ECrowdyKitMatchState : uint8
{
	Idle,
	/** In a party its members are confirming. */
	Party,
	Queued,
	/** In a proposed match that every player must accept. */
	Proposed,
	/** Matched: Session is the key of the session reserved for the match. */
	Matched
};

/** The player's own place in a matchmaking queue; only the values of its State are filled. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMatchPlace
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitMatchState State = ECrowdyKitMatchState::Idle;

	/** Party: the party's leader. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Leader = 0;

	/** Party: everyone in it, and who has confirmed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Members;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Confirmed;

	/** Queued: the rating, how long it has waited and the rating window it accepts now. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Rating = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 WaitedMs = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Window = 0;

	/** Proposed: the match's players, and who has accepted. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Players;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Accepted;

	/** Party and Proposed: the time left to confirm or accept. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 ExpiresInMs = 0;

	/** Matched: the Instance Id of the Kit Session reserved for the match. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Session;
};

/** Joins the queue alone; a party's members confirm with the party's leader, which these inputs do not carry. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMatchJoinInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 Rating = 0;
};

/** A matchmaking queue's settings. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMatchmakerConfig
{
	GENERATED_BODY()

	/** 1 to 64. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 PlayersPerMatch = 2;

	/** The rating difference a player accepts at first. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 Window = 100;

	/** How much the window widens each second the player waits. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 WidenPerSec = 10;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 MaxWindow = 500;

	/** How long the players of a proposed match have to accept; at least 1000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 AcceptMs = 15000;

	/** How long a party's members have to confirm; at least 1000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 PartyMs = 60000;
};

/** Where a game session is, from lobby to result. */
UENUM(BlueprintType)
enum class ECrowdyKitSessionPhase : uint8
{
	Lobby,
	Playing,
	Finished
};

/** A game session's settings. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitSessionConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 MinPlayers = 2;

	/** At most 64. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 MaxPlayers = 8;

	/** How long a player has for a move before the turn passes; at least 1000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 TurnMs = 30000;

	/** The score that wins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 TargetScore = 10;

	/** The session ends after this many rounds; the highest score wins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 MaxRounds = 20;

	/** A lobby nobody joins closes after this long. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 LobbyIdleMs = 600000;

	/** The Kit Leaderboard a finished session posts each player's score to; empty for none. Only the session's Config in its definition sets it; Configure leaves it as it is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Board;
};

/** One move a game session played. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitSessionMove
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Round = 0;

	/** What the move scored; 0 when the player ran out of time. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Points = 0;
};

/** One game session as everyone in it sees it: the variables of Kit Session. Player ids are user ids. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitSessionState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitSessionConfig Config;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitSessionPhase Phase = ECrowdyKitSessionPhase::Lobby;

	/** When not empty, only these players may join (a matchmaker's reservation). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Reserved;

	/** In turn order. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Players;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Ready;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Connected;

	/** Who starts the session; 0 for nobody. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Host = 0;

	/** The index in Players of whose turn it is, while the session plays. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Turn = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Round = 0;

	/** Each player's score, by user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<int64, int64> Scores;

	/** The winner once the session finishes; 0 before. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Winner = 0;

	/** The last 20 moves. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitSessionMove> Moves;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitSessionReadyInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	bool bReady = true;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitSessionReadyReply
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bReady = false;
};

/** The starter's move: 0 to 3 points, which the session's own rule in its server code decides. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitSessionActInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Points = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitSessionActReply
{
	GENERATED_BODY()

	/** False when the move ended the session. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bPlaysOn = false;
};

/** How a leaderboard makes a player's score from their submissions. */
UENUM(BlueprintType)
enum class ECrowdyKitBoardKeep : uint8
{
	/** Their best submission. */
	Best,
	/** Every submission added up. */
	Sum,
	/** Their last submission. */
	Latest
};

/** Which way a leaderboard ranks. */
UENUM(BlueprintType)
enum class ECrowdyKitBoardOrder : uint8
{
	/** Higher scores first, as for points. */
	High,
	/** Lower scores first, as for times. */
	Low
};

/** One place on a leaderboard; players with the same score share a rank. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBoardPlace
{
	GENERATED_BODY()

	/** 1 for the top; 0 when the player is not on the board. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Rank = 0;

	/** The player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Score = 0;
};

/** The top of a leaderboard and its season, as every player sees it: the variables of Kit Leaderboard. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBoardView
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Season = 0;

	/** When the season started, in Unix milliseconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 SeasonStarted = 0;

	/** How many players the board holds this season. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Entries = 0;

	/** The best places, as many as the board's Top Size. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitBoardPlace> Top;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBoardPageInputs
{
	GENERATED_BODY()

	/** At most 100; 0 for the board's Top Size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 Limit = 0;

	/** How many places to skip from the top. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 Offset = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBoardAroundInputs
{
	GENERATED_BODY()

	/** The player whose neighbours to list; 0 for yourself. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Player = 0;

	/** How many places above and below; at most 50. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 Radius = 2;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBoardPlaces
{
	GENERATED_BODY()

	/** Best first; empty when the player asked about is not on the board. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitBoardPlace> Places;
};

/** A leaderboard's season, and the final standings of the one before. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBoardSeason
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Season = 0;

	/** When it started, in Unix milliseconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Started = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Entries = 0;

	/** The season before's top 100, best first. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitBoardPlace> Last;
};

/** A score for the board; only the app's developers and other server code may submit, never players. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBoardSubmitInputs
{
	GENERATED_BODY()

	/** The player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Player = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Score = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBoardSubmitReply
{
	GENERATED_BODY()

	/** The player's score on the board after the submission. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Score = 0;
};

/** A leaderboard's settings. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBoardConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	ECrowdyKitBoardKeep Keep = ECrowdyKitBoardKeep::Best;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	ECrowdyKitBoardOrder Order = ECrowdyKitBoardOrder::High;

	/** The places the board's variables carry; 1 to 100. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 TopSize = 10;

	/** A cron schedule that ends each season, such as 0 0 * * 1 for every Monday; empty for none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString SeasonCron;

	/** The most players the board holds; at least 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 MaxEntries = 100000;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBoardRollReply
{
	GENERATED_BODY()

	/** The season that began. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Season = 0;
};

/** Items and in-game soft currency moved together: a price, a reward, an offer. Soft currency is game state, never real money. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBundle
{
	GENERATED_BODY()

	/** How many of each item, by item id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	TMap<FString, int64> Items;

	/** How much of each soft currency, such as gold or gems. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	TMap<FString, int64> Currency;
};

/** A player as anyone sees them. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitPlayerProfile
{
	GENERATED_BODY()

	/** The player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Level = 0;

	/** The ids of the achievements they unlocked. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Achievements;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 QuestsDone = 0;
};

/** Some of one item in one inventory slot. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitStack
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Item;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Count = 0;
};

/** A player's items, which only they read. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitInventory
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Slots = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitStack> Stacks;

	/** Items given that did not fit, waiting for Take Mail. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int64> Mail;

	/** Items put up for a market order or trade. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int64> Held;
};

/** A player's in-game soft currency, which only they read; never real money. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitWallet
{
	GENERATED_BODY()

	/** Each soft currency's balance, never below zero. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int64> Balances;

	/** Soft currency put up for a market order or trade. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int64> Held;
};

/** One thing a quest asks for: a counter reaching its target. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitObjective
{
	GENERATED_BODY()

	/** What is counted, such as crafted or collected. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Counter;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Target = 0;

	/** How far the player is; 0 in a quest's definition. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Count = 0;
};

UENUM(BlueprintType)
enum class ECrowdyKitQuestState : uint8
{
	/** Its required quests or level are not met yet. */
	Locked,
	Available,
	Active,
	/** Every objective is met; its reward waits for Claim Quest. */
	Complete,
	Done
};

/** One quest as the player stands on it. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitQuest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitQuestState State = ECrowdyKitQuestState::Locked;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitObjective> Objectives;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBundle Reward;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Xp = 0;

	/** A first-session tutorial, taken on as the player starts. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bTutorial = false;
};

/** One achievement as the player stands on it; hidden ones appear once unlocked. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitAchievement
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Counter;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Target = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Count = 0;

	/** When it was unlocked, in Unix milliseconds; 0 while locked. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 UnlockedAt = 0;
};

/** A player's experience, achievements and quests, which only they read. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitProgress
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Level = 0;

	/** Experience into this level. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Xp = 0;

	/** The experience this level takes to finish; 0 at the top level, and the int64 maximum when the Config's levels grow past it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Next = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 TotalXp = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitAchievement> Achievements;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitQuest> Quests;
};

/** One idle producer and its rate. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitProducer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double PerMinute = 0.0;
};

/** A player's idle production: what has piled up for Collect, and how much each resource can pile up. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitProduction
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitProducer> Producers;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int64> Stock;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int64> Capacity;
};

/** A player's profile, and what only they read: the variables of Kit Player. Inventory, Wallet and Progress are filled for the signed-in player's own player only. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitPlayerState
{
	GENERATED_BODY()

	/** The player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Level = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Achievements;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 QuestsDone = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitInventory Inventory;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitWallet Wallet;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitProgress Progress;
};

/** Everything a player has, as their Get Me answers. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitPlayerMe
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitPlayerProfile Profile;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitInventory Inventory;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitWallet Wallet;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitProgress Progress;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitProduction Production;
};

/** One kind of item in the game's catalog. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitItemDef
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	/** What one slot holds; 1 for an item that does not stack. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 MaxStack = 1;

	/** Whether it may change hands on a market. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bTradable = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Name;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitRecipe
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBundle Inputs;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBundle Outputs;

	/** The level a player needs; 0 for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Level = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitLootEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Item;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Count = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Weight = 0;

	/** Entries of one pity group are guaranteed within its Pity Within rolls; empty for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString PityGroup;
};

/** A loot table and what one roll costs. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitLootTable
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBundle Cost;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitLootEntry> Entries;

	/** Each pity group, and the rolls within which it is guaranteed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int32> PityWithin;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitQuestDef
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitObjective> Objectives;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBundle Reward;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Xp = 0;

	/** The quests done first. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Requires;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Level = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bTutorial = false;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitAchievementDef
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Counter;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Target = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBundle Reward;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Xp = 0;
};

/** The player starter's rules, from its Config: the same for every player. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitPlayerRules
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Slots = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitItemDef> Items;

	/** The soft currencies, such as gold and gems. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Currencies;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitRecipe> Recipes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitLootTable> Loot;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitQuestDef> Quests;

	/** The achievements that are not hidden. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitAchievementDef> Achievements;

	/** The counters the game's client may report with Step, such as tutorial steps. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> ClientCounters;
};

/** What one change earned the player. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitEarned
{
	GENERATED_BODY()

	/** Levels gained. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Levels = 0;

	/** Achievements unlocked. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Achievements;

	/** Quests whose objectives are all met. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Quests;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitCraftInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Recipe;

	/** At most 100. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Times = 1;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitCraftReply
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBundle Made;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitEarned Earned;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitOpenLootInputs
{
	GENERATED_BODY()

	/** The loot table's id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Table;
};

/** What one loot roll gave. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitLootRoll
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Item;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Count = 0;

	/** The pity rule picked it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bPity = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Roll = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitOpenLootReply
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitLootRoll Roll;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitEarned Earned;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitQuestInputs
{
	GENERATED_BODY()

	/** The quest's id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Quest;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitStepInputs
{
	GENERATED_BODY()

	/** One of the rules' Client Counters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Counter;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDiscardInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Item;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Count = 1;
};

/** A reward for a player, given by the app's developers or its server code, never by players. Soft currency only, never real money. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitGrantInputs
{
	GENERATED_BODY()

	/** Up to 128 characters, unique per grant: a grant sent again with a Ref already used answers as the first did and gives nothing more. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Ref;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FCrowdyKitBundle Bundle;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Xp = 0;

	/** Counters to add to, such as quest or achievement progress. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	TMap<FString, int64> Counters;
};

/** Counts something a player did, from the app's developers or its server code, never players. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitRecordInputs
{
	GENERATED_BODY()

	/** Up to 128 characters, unique per record: one sent again with a Ref already used counts nothing more. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Ref;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Counter;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 N = 1;
};

/** Items and soft currency put into a player's hub by the app's developers or its server code, never players; nothing is counted toward quests. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDepositInputs
{
	GENERATED_BODY()

	/** Up to 128 characters, unique per deposit: one sent again with a Ref already used gives nothing more. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Ref;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FCrowdyKitBundle Bundle;
};

/** One item a vendor sells, and what it pays for one back. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitVendorOffer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Item;

	/** What one costs. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBundle Price;

	/** Left on the shelf; -1 for no limit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Stock = -1;

	/** How many one player may buy between restocks; -1 for no limit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 PerPlayer = -1;

	/** What the vendor pays for one with Sell; empty when it does not buy them. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBundle BuysFor;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitVendor
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitVendorOffer> Offers;

	/** When its shelf last restocked, in Unix milliseconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 RestockedAt = 0;
};

/** An item's best prices on the market's order book; 0 for none. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitQuote
{
	GENERATED_BODY()

	/** The highest buy order. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Bid = 0;

	/** The lowest sell order. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Ask = 0;

	/** The last price it traded at. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Last = 0;
};

UENUM(BlueprintType)
enum class ECrowdyKitOrderSide : uint8
{
	Buy,
	Sell
};

/** One of the player's open orders on a market's order book. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitOrder
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Id = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Item;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitOrderSide Side = ECrowdyKitOrderSide::Buy;

	/** The price of one, in the market's currency. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Price = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Count = 0;

	/** What is still open. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Open = 0;

	/** The hold in the player's hub that backs it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Hold;
};

UENUM(BlueprintType)
enum class ECrowdyKitTradeState : uint8
{
	Open,
	/** Both accepted; the market is moving each side's offer to the other player, which their wallets and inventories show a moment later. */
	Settling,
	Done,
	Cancelled
};

/** What one player offers in a trade. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTradeSide
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBundle Bundle;

	/** The hold in the player's hub that backs it; empty while nothing is offered. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Hold;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bAccepted = false;
};

/** A trade between two players; a new offer from either side takes back both acceptances. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTrade
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Id = 0;

	/** The two players' user ids; Sides is in the same order. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Players;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitTradeSide> Sides;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitTradeState State = ECrowdyKitTradeState::Open;

	/** When an open trade is called off, in Unix milliseconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 ExpiresAt = 0;

	/** Offers made so far. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Offers = 0;
};

/** A market as everyone sees it, and the signed-in player's own orders and trades: the variables of Kit Market. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMarketState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitVendor> Vendors;

	/** The soft currency its order books trade in. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Currency;

	/** Each item with an order book, and its best prices. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, FCrowdyKitQuote> Quotes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 OpenTrades = 0;

	/** Transfers between players' hubs still to finish. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 PendingSteps = 0;

	/** The signed-in player's open orders. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitOrder> Orders;

	/** The signed-in player's open and settling trades. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitTrade> Trades;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitVendors
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitVendor> Vendors;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitShopInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Vendor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Item;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Count = 1;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBookInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Item;
};

/** Some count at one price on an order book. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitPriceLevel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Price = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Count = 0;
};

/** One item's order book, best prices first, ten deep. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBook
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Item;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Currency;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitPriceLevel> Bids;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitPriceLevel> Asks;

	/** The last price it traded at; 0 for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Last = 0;
};

/** An order for the book: a buy holds its price times count of the market's currency, a sell holds the items, until it fills or is cancelled. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitOrderInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Item;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	ECrowdyKitOrderSide Side = ECrowdyKitOrderSide::Buy;

	/** The price of one; above 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Price = 0;

	/** Above 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Count = 1;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitPlacedOrder
{
	GENERATED_BODY()

	/** The order's id, for Cancel Order. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Order = 0;

	/** What it traded at once. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitPriceLevel> Fills;

	/** What rests on the book. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Open = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitCancelOrderInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Order = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitOrders
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitOrder> Orders;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitOpenTradeInputs
{
	GENERATED_BODY()

	/** The other player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 With = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTradeReply
{
	GENERATED_BODY()

	/** The trade's id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Trade = 0;
};

/** The caller's side of a trade; an empty Bundle takes their offer back. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTradeOfferInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Trade = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FCrowdyKitBundle Bundle;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTradeInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Trade = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitAcceptTradeReply
{
	GENERATED_BODY()

	/** Settling once both players accepted. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitTradeState State = ECrowdyKitTradeState::Open;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTrades
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitTrade> Trades;
};

UENUM(BlueprintType)
enum class ECrowdyKitAreaMemberType : uint8
{
	Mob,
	Npc,
	/** A player's pet. */
	Pet
};

/** One member of an npcs area: a mob, an NPC or a pet. Only the fields of its Type are filled; X and Y are in chunks of 1600 units. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitAreaMember
{
	GENERATED_BODY()

	/** Its id in the area's hub, which Attack, Talk and the pet functions take. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Id = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitAreaMemberType Type = ECrowdyKitAreaMemberType::Mob;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;

	/** A mob's kind, such as wolf. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Kind;

	/** A mob's health; 0 while it is down. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Hp = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 MaxHp = 0;

	/** A mob's idle, chase, return or down; a pet's stay or follow. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Mode;

	/** An NPC's or a pet's name. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Name;

	/** What an NPC says. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Line;

	/** A pet's species, such as cat. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Species;

	/** How often its owner summoned it; filled only for its owner. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Bond = 0;
};

/** An npcs area's State, which is empty: everything it keeps is its records, of FCrowdyKitAreaMember. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitAreaState
{
	GENERATED_BODY()
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitAreaMembers
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitAreaMember> Members;
};

/** A hit on a mob, which the player must be within reach of. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitAttackInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Mob = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitAttackReply
{
	GENERATED_BODY()

	/** The mob's health after the hit; 0 means it is down until it respawns. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Hp = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTalkInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Npc = 0;
};

/** A new pet for the caller, where they stand: Species and Name are 1 to 24 characters. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitAdoptInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Species;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Name;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitAdoptReply
{
	GENERATED_BODY()

	/** The new pet's id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Pet = 0;
};

/** One of the caller's own pets. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitPetInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Pet = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitRenamePetInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Pet = 0;

	/** 1 to 24 characters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Name;
};

/** Where a minigame duel is. */
UENUM(BlueprintType)
enum class ECrowdyKitMinigamePhase : uint8
{
	/** Fewer than two players are seated. */
	Waiting,
	Playing,
	Finished
};

/** One player at a minigame table. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMinigameSeat
{
	GENERATED_BODY()

	/** The player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** Rounds this player has won. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Wins = 0;

	/** Whether this round's throw is in. Never which hand: that stays hidden until the round is revealed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bThrown = false;
};

/** A round once both throws were in or its time ran out. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMinigameReveal
{
	GENERATED_BODY()

	/** The round's number, from 1; 0 while no round has been revealed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Round = 0;

	/** Each seat's hand, in seat order; empty text for a throw that was not in on time. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Hands;

	/** The round's winner as a user id, not a seat; 0 for a draw or when nobody threw. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Winner = 0;
};

/** One minigame table as everyone at it sees it: the variables of Kit Minigame. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMinigameState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitMinigamePhase Phase = ECrowdyKitMinigamePhase::Waiting;

	/** In join order; none, one or two. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitMinigameSeat> Seats;

	/** Rounds in a match; the first to win a majority wins it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 BestOf = 3;

	/** The current round, from 1; 0 before the match starts. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Round = 0;

	/** When the round closes, in Unix milliseconds; 0 when no round is waiting on throws. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Deadline = 0;

	/** The last revealed round; its Round is 0 until one has been. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitMinigameReveal Last;

	/** The match's winner as a user id once it finishes; 0 before. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Winner = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMinigameJoinReply
{
	GENERATED_BODY()

	/** True when this join seated the second player and started the match; false for the first player, or one already seated. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bStarted = false;
};

/** A hidden throw, kept by the hub until both players' throws are in or the round's time runs out. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMinigameThrowInputs
{
	GENERATED_BODY()

	/** rock, paper or scissors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Hand;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitMinigameThrowReply
{
	GENERATED_BODY()

	/** True when this throw was the last one in and the round was revealed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bRevealed = false;
};

/** A cell of a game board. A board keeps its server's x, z axes: it is not the world, so z is not height. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitGridCell
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 X = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Z = 0;
};

/** Where a tactics battle is. */
UENUM(BlueprintType)
enum class ECrowdyKitTacticsPhase : uint8
{
	Lobby,
	/** The players' turn: each unit may move and attack once. */
	Players,
	/** The hub's enemies are moving and striking. */
	Enemies,
	Won,
	Lost
};

UENUM(BlueprintType)
enum class ECrowdyKitTacticsTeam : uint8
{
	Players,
	Enemies
};

/** A tactics unit's numbers. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTacticsStats
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Hp = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Attack = 0;

	/** Taken off the attacker's attack for each hit, which still does at least 1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Defense = 0;

	/** How many cells it may move in a turn. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Moves = 0;

	/** How far an attack reaches, in cells (Manhattan). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Range = 0;
};

/** One unit on a tactics board. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTacticsUnit
{
	GENERATED_BODY()

	/** Its id on the board, which Attack takes; from 1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Id = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitTacticsTeam Team = ECrowdyKitTacticsTeam::Players;

	/** The user id of the player it belongs to; 0 for the hub's own units. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Owner = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 X = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Z = 0;

	/** Its health now; 0 or less when it is down. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Hp = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitTacticsStats Stats;

	/** Whether it has moved this turn. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bMoved = false;

	/** Whether it has attacked this turn. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bActed = false;
};

/** One tactics battle as everyone in it sees it: the variables of Kit Tactics. Player ids are user ids. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTacticsState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitTacticsPhase Phase = ECrowdyKitTacticsPhase::Lobby;

	/** 0 in the lobby, 1 when the battle starts, and one more after each enemy phase. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Round = 0;

	/** The board's size in cells, from 4 to 32 each way. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Width = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Height = 0;

	/** The blocked cells. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitGridCell> Obstacles;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitTacticsUnit> Units;

	/** In join order; at most 4. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Players;

	/** The first to join, who starts the battle; 0 for nobody yet. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Host = 0;

	/** The players who ended their turn this phase. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> EndedTurn;

	/** When the players' phase ends, in Unix milliseconds; 0 outside it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 PhaseEnds = 0;

	/** The last 24 lines of what moved and hit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Log;
};

/** A move to a cell the unit can reach this turn: one move a turn, within its Moves and around obstacles and other units. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTacticsMoveInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 X = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 Z = 0;
};

/** An attack on a unit within reach: one attack a turn. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTacticsAttackInputs
{
	GENERATED_BODY()

	/** The target unit's Id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Target = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTacticsAttackReply
{
	GENERATED_BODY()

	/** The target's health after the hit; 0 means it is down. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Hp = 0;
};

UENUM(BlueprintType)
enum class ECrowdyKitDirectorPhase : uint8
{
	Lobby,
	Running,
	Won,
	Lost
};

/** What happened in an encounter, as the type of its Encounter Event message. */
UENUM(BlueprintType)
enum class ECrowdyKitDirectorEventType : uint8
{
	/** A wave came out of the spawn; the event's Wave is its index. */
	WaveStarted,
	/** A wave was cleared; the event's Wave is its index. The last wave's end is Won, not this. */
	WaveCleared,
	/** The boss weakened into a new phase; the event's Phase is 1 to 3. */
	BossPhase,
	Won,
	Lost
};

/** One enemy walking the lane. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDirectorEnemy
{
	GENERATED_BODY()

	/** Its id in the encounter, which Hit takes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Id = 0;

	/** Its kind's id from the Config, such as grunt. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Kind;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 X = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Z = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Hp = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 MaxHp = 0;

	/** Whether it is the last wave's boss. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bBoss = false;

	/** Ticks until its next step, which are 500 milliseconds apart. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Wait = 0;
};

/** A tower a player built. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDirectorTower
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 X = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Z = 0;

	/** The user id of the player who built it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Owner = 0;
};

/** One encounter as everyone in it sees it: the variables of Kit Director. Player ids are user ids. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDirectorState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitDirectorPhase Phase = ECrowdyKitDirectorPhase::Lobby;

	/** In join order; at most 8. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Players;

	/** The field's size in cells. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Width = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Height = 0;

	/** Where the enemies come in. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitGridCell Spawn;

	/** The players' base, which the enemies walk to. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitGridCell Base;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 BaseHp = 0;

	/** The gold all the players share: kills pay it and towers cost it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Gold = 0;

	/** The index, from 0, of the wave under way, or of the last one cleared while the next counts down. Valid only while Has Wave is true. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Wave = 0;

	/** False before the first wave starts and after the last: the server sent none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasWave = false;

	/** How many waves the Config has. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Waves = 0;

	/** When the next wave starts, in Unix milliseconds, while one counts down; 0 otherwise. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 NextWaveAt = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitDirectorEnemy> Enemies;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitDirectorTower> Towers;

	/** 0 to 3: how far the boss has weakened. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 BossPhase = 0;

	/** Each player's kills, by user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<int64, int32> Kills;
};

/** One thing that happened in an encounter, sent as its Encounter Event message. Only the fields of its Type are filled. An event arrives just before the snapshot that shows its effect: read the event's own fields; the state catches up on the next snapshot. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDirectorEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitDirectorEventType Type = ECrowdyKitDirectorEventType::WaveStarted;

	/** False when this client does not know the event's type: ignore the event. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasType = false;

	/** For a wave started or cleared: the wave's index, from 0. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Wave = 0;

	/** For a boss phase: the phase it entered. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Phase = 0;
};

/** A tower on a free cell of the field, off the spawn and the base, that leaves the enemies a way to the base. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDirectorBuildInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 X = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int32 Z = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDirectorBuildReply
{
	GENERATED_BODY()

	/** The gold left after paying for the tower. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Gold = 0;
};

/** A strike on an enemy; a player may strike again only after the Config's cooldown. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDirectorHitInputs
{
	GENERATED_BODY()

	/** The enemy's Id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Enemy = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDirectorHitReply
{
	GENERATED_BODY()

	/** The enemy's health after the strike; 0 means it is down. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Hp = 0;
};

/** Where an instance run is. */
UENUM(BlueprintType)
enum class ECrowdyKitInstancePhase : uint8
{
	/** Nobody has opened it yet. */
	Unopened,
	Running,
	/** The party cleared the last floor. */
	Cleared,
	/** Nobody in the party is standing. */
	Wiped,
	Abandoned,
	/** Nobody touched it for the Config's idle time. */
	Expired
};

UENUM(BlueprintType)
enum class ECrowdyKitInstanceRoomKind : uint8
{
	Fight,
	Treasure,
	Rest,
	/** The last room of the last floor. */
	Boss
};

/** One room of the current floor. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitInstanceRoom
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitInstanceRoomKind Kind = ECrowdyKitInstanceRoomKind::Fight;

	/** What a fight here takes to beat. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Power = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bDone = false;
};

/** One player in a run's party. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitInstanceMember
{
	GENERATED_BODY()

	/** The player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** Their health now; 0 when they are down. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Hp = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 MaxHp = 0;

	/** What they add to the party's strength in a fight. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Power = 0;
};

/** One run as everyone in it sees it: the variables of Kit Instance. Player ids are user ids. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitInstanceState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitInstancePhase Phase = ECrowdyKitInstancePhase::Unopened;

	/** The player whose run it is, who invites and descends; 0 before it opens. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Owner = 0;

	/** The seed its floors come from, which replays them; public, so it reveals the whole dungeon. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Seed = 0;

	/** The current floor, from 1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Floor = 0;

	/** How many floors the run has. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Floors = 0;

	/** The index in Rooms of the room the party is at, from 0; equal to the number of rooms once the floor is cleared. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Room = 0;

	/** The current floor's rooms. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitInstanceRoom> Rooms;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitInstanceMember> Members;

	/** Players the owner invited who have not joined yet. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Invited;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Gold = 0;

	/** The relics the party found. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Relics;

	/** The id of the grid of the world reserved for the run; 0 when its Config has no volume. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Grid = 0;

	/** The last 20 lines of what the party did. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Log;
};

/** Opens the run for the caller's party. A developer opens it for Party instead; a player's party is always themselves. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitInstanceOpenInputs
{
	GENERATED_BODY()

	/** The seed its floors come from, 0 to 2^63-1, sent only when Has Seed is set: the same seed replays the same floors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Seed = 0;

	/** Whether to send Seed; when not, the hub draws a seed, and Open's reply says which. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	bool bHasSeed = false;

	/** The user ids of the party, which only a developer's call uses; at least 1 and at most the Config's party size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	TArray<int64> Party;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitInstanceOpenReply
{
	GENERATED_BODY()

	/** The seed the run uses, given or picked. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Seed = 0;
};

/** Invites a player to the run; only its owner may. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitInstanceInviteInputs
{
	GENERATED_BODY()

	/** The invited player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 Player = 0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitInstanceEnterReply
{
	GENERATED_BODY()

	/** What happened in the room, as the run's log has it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Line;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitInstanceDescendReply
{
	GENERATED_BODY()

	/** The floor the party is now on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Floor = 0;
};

UENUM(BlueprintType)
enum class ECrowdyKitTerritoryPhase : uint8
{
	Open,
	/** A faction reached the Config's winning score. */
	Won
};

/** What happened in a territory war, as the type of its Territory Event message. */
UENUM(BlueprintType)
enum class ECrowdyKitTerritoryEventType : uint8
{
	/** A faction took a point; the event's Point, Owner and Previous say which. */
	Captured,
	/** Every faction was paid for its points; the event's Paid says how much. */
	Income,
	/** A faction reached the winning score; the event's Faction says which. */
	Won
};

/** One control point as everyone sees it. Its place in the world is in the war's Config, not here. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTerritoryPoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	/** The faction that holds it; empty for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Owner;

	/** The faction pressing a capture; empty for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Capturing;

	/** How far the capture is, 0 to 1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Progress = 0.0;

	/** How many members of each faction stood on it at the last scan, by faction. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int32> Present;
};

/** One territory war as everyone sees it: the variables of Kit Territory. The world is z up: the ground is x, y and height is z. Faction names must differ by more than case. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTerritoryState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitTerritoryPhase Phase = ECrowdyKitTerritoryPhase::Open;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitTerritoryPoint> Points;

	/** Each faction's score, by faction. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int64> Scores;

	/** How many players each faction has, by faction. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int32> Members;

	/** The winning faction once the war is won; empty before. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Winner;
};

/** One thing that happened in a territory war, sent as its Territory Event message. Only the fields of its Type are filled. An event arrives just before the snapshot that shows its effect: read the event's own fields; the state catches up on the next snapshot. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTerritoryEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitTerritoryEventType Type = ECrowdyKitTerritoryEventType::Captured;

	/** False when this client does not know the event's type: ignore the event. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasType = false;

	/** For a capture: the point's Id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Point;

	/** For a capture: the faction that took the point. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Owner;

	/** For a capture: the faction that held it before; empty for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Previous;

	/** For income: what each faction was paid, by faction. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<FString, int64> Paid;

	/** For a win: the winning faction. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Faction;
};

/** Joins a faction, once: a player keeps theirs. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitTerritoryJoinInputs
{
	GENERATED_BODY()

	/** One of the Config's factions, such as red. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Faction;
};

UENUM(BlueprintType)
enum class ECrowdyKitZonePhase : uint8
{
	Lobby,
	Running,
	Finished
};

/** What happened in a zone match, as the type of its Zone Event message. */
UENUM(BlueprintType)
enum class ECrowdyKitZoneEventType : uint8
{
	/** The circle will shrink; the event's Phase, To Radius and Shrinks At say how. */
	Warning,
	/** The circle started shrinking; the event's Phase and To Radius say to what. */
	Shrinking,
	/** The circle stopped at the event's Radius. */
	Settled,
	/** The circle closed to the event's Radius, the last. */
	Final,
	/** A player fell; the event's Player and Place say who and where they placed. */
	Eliminated,
	/** The match ended; the event's Player is the winner, or 0 for none. */
	Won
};

/** A point on the ground of the z up world: x and y, in chunks as the Config gives them. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitZoneCenter
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;
};

/** One player in a zone match. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitZonePlayer
{
	GENERATED_BODY()

	/** The player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** Their health now; the circle wears it down while they are outside it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Hp = 0;

	/** Whether they were outside the circle at the last scan. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bOutside = false;

	/** Where they placed, from 1 for the winner; 0 while they are still standing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Place = 0;
};

/** One zone match as everyone in it sees it: the variables of Kit Zone. The world is z up: the circle is on the ground, x and y, at the Config's height z. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitZoneState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitZonePhase Phase = ECrowdyKitZonePhase::Lobby;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitZoneCenter Center;

	/** The circle's radius in chunks now: the Config's in the lobby, then the live, shrinking one. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Radius = 0.0;

	/** The index, from 0, of the shrink phase the circle is in. Valid only while Has Stage is true. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Stage = 0;

	/** False until the first warning: the server sent none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasStage = false;

	/** The radius the circle shrinks to in its Stage; 0 while Has Stage is false. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double NextRadius = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitZonePlayer> Players;

	/** How many players are still standing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Alive = 0;

	/** The last player standing, as a user id, once the match finishes; 0 before. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Winner = 0;
};

/** One thing that happened in a zone match, sent as its Zone Event message. Only the fields of its Type are filled. An event arrives just before the snapshot that shows its effect: read the event's own fields; the state catches up on the next snapshot. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitZoneEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitZoneEventType Type = ECrowdyKitZoneEventType::Warning;

	/** False when this client does not know the event's type: ignore the event. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasType = false;

	/** For a warning, a shrinking and a settling: the shrink phase's index, from 0. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Phase = 0;

	/** For a warning and a shrinking: the radius the circle shrinks to, in chunks. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	float ToRadius = 0.0f;

	/** For a settling and the final close: the radius the circle stopped at, in chunks. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	float Radius = 0.0f;

	/** For a warning: when the circle starts shrinking, in Unix milliseconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 ShrinksAt = 0;

	/** For an elimination: the fallen player's user id. For a win: the winner's, or 0 for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** For an elimination: where they placed, counting down from the number standing; 1 is the winner. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Place = 0;
};

UENUM(BlueprintType)
enum class ECrowdyKitCardSuit : uint8
{
	Spades,
	Hearts,
	Diamonds,
	Clubs
};

/** Where a deck table is. */
UENUM(BlueprintType)
enum class ECrowdyKitDeckPhase : uint8
{
	Lobby,
	Playing,
	Finished
};

/** What a move on a deck table was. */
UENUM(BlueprintType)
enum class ECrowdyKitDeckAction : uint8
{
	Play,
	Draw,
	Pass,
	/** The player's time ran out and the table moved on for them. */
	Timeout
};

/** A playing card. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitCard
{
	GENERATED_BODY()

	/** The card as text, such as 9H or 10S: what Play takes. Empty for no card. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Id;

	/** 1 for an ace, 11 jack, 12 queen, 13 king. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Rank = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitCardSuit Suit = ECrowdyKitCardSuit::Spades;
};

/** The signed-in player's own cards at a deck table. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDeckHand
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitCard> Cards;

	/** The cards they may play now; empty when it is not their turn. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitCard> Playable;

	/** Whether they already drew this turn. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bDrawn = false;
};

/** The last move at a deck table. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDeckPlayed
{
	GENERATED_BODY()

	/** The player's user id; 0 before the first move. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** The card played; its Id is empty for a draw, a pass and a timeout. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitCard Card;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitDeckAction Action = ECrowdyKitDeckAction::Play;
};

/** One deck table as everyone at it sees it: the variables of Kit Deck. Player ids are user ids. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDeckState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitDeckPhase Phase = ECrowdyKitDeckPhase::Lobby;

	/** In join order. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int64> Players;

	/** The first to join, who starts the game; 0 for nobody yet. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Host = 0;

	/** How many cards each player holds, by user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<int64, int32> HandCounts;

	/** The card on top of the pile. Its Id is empty before the first card is turned. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitCard Top;

	/** The suit in play, which an eight changes. Valid only while Has Suit is true. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitCardSuit Suit = ECrowdyKitCardSuit::Spades;

	/** False when no suit is in play: none in the lobby. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasSuit = false;

	/** How many cards are left to draw. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 DrawCount = 0;

	/** Whose turn it is, as a user id; 0 outside a game. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Turn = 0;

	/** When the turn ends, in Unix milliseconds; 0 outside a game. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 TurnEnds = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitDeckPlayed Last;

	/** The winner's user id once the game is over; 0 before. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Winner = 0;

	/** Each player's points, by user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TMap<int64, int64> Scores;

	/** Only the signed-in player's own hand: read with its own call, and again whenever the table says it changed for them (their cards changed, or their turn began or ended). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitDeckHand Hand;
};

/** A card from the player's hand. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDeckPlayInputs
{
	GENERATED_BODY()

	/** The card's Id, such as 9H. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Card;

	/** For an eight: the suit it names next. Set Has Suit to send it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	ECrowdyKitCardSuit Suit = ECrowdyKitCardSuit::Spades;

	/** Whether to send Suit; leave it off for any card but an eight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	bool bHasSuit = false;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitDeckPlayReply
{
	GENERATED_BODY()

	/** Whether the game goes on: false when this play emptied the player's hand and won. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bPlaying = false;
};

/** Where an arena fight is. */
UENUM(BlueprintType)
enum class ECrowdyKitArenaPhase : uint8
{
	Waiting,
	Running,
	Finished
};

/** What happened in an arena fight, as the type of its Arena Event message. */
UENUM(BlueprintType)
enum class ECrowdyKitArenaEventType : uint8
{
	/** A fighter cast an ability; the event's Ability, Caster, X and Y say which and where it was aimed. */
	Cast,
	/** An ability hit; the event's Ability, Caster, Victim and Damage say how. */
	Impact,
	/** A fighter fell; the event's Killer and Victim say who. */
	Kill,
	/** A fighter came back; the event's Player says who. */
	Respawn,
	/** The server refused a fighter's move; the event's Player, What and Detail say who and why. */
	Violation,
	/** A fighter was taken out of the fight; the event's Player and Why (movement or idle) say who and why. */
	Removed,
	/** The fight ended; the event's Winner is the winner, or 0 for none. */
	Finished
};

/** A point on the ground of the z up world, x and y in metres. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitGroundPosition
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;
};

/** One fighter in an arena. Positions are world metres, x and y on the ground. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitArenaFighter
{
	GENERATED_BODY()

	/** The fighter's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** The hero class they chose, such as warrior. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString HeroClass;

	/** Where the server last kept them, in metres in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Hp = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 MaxHp = 0;

	/** Their mana or energy now, regeneration included. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Resource = 0;

	/** The effects on them, such as slow. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FString> Statuses;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Kills = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Deaths = 0;

	/** Whether they are down and waiting to respawn. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bDown = false;
};

/** A projectile in flight. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitArenaShot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Ability;

	/** The caster's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Caster = 0;

	/** Where it is now, in metres in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;
};

/** An area effect that has been cast and is about to land. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitArenaBlast
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Ability;

	/** The caster's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Caster = 0;

	/** Its centre, in metres in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;

	/** In metres. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Radius = 0.0;

	/** When it lands, in Unix milliseconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 At = 0;
};

/** One arena fight as everyone in it sees it: the variables of Kit Arena. The world is z up and every position is in metres: the ground is x, y, and a Crowdy Server Object component reports its owner's place as it moves. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitArenaState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitArenaPhase Phase = ECrowdyKitArenaPhase::Waiting;

	/** The arena's corner in the world, in metres. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitGroundPosition Origin;

	/** The arena's size along X, in metres. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Width = 0.0;

	/** The arena's size along Y, in metres. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Depth = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitArenaFighter> Fighters;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitArenaShot> Projectiles;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitArenaBlast> Areas;

	/** The kills that win the fight. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 KillTarget = 0;

	/** When the fight ends, in Unix milliseconds; 0 outside a running fight. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 EndsAt = 0;

	/** The winner's user id once the fight is over; 0 for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Winner = 0;
};

/** One thing that happened in an arena fight, sent as its Arena Event message. Only the fields of its Type are filled. An event arrives just before the snapshot that shows its effect: read the event's own fields; the state catches up on the next snapshot. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitArenaEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitArenaEventType Type = ECrowdyKitArenaEventType::Cast;

	/** False when this client does not know the event's type: ignore the event. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasType = false;

	/** For a cast and an impact: the ability's name. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Ability;

	/** For a cast and an impact: the caster's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Caster = 0;

	/** For a cast: where it was aimed, in metres in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;

	/** For an impact and a kill: the user id of whoever was hit or fell. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Victim = 0;

	/** For an impact: the health it took. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Damage = 0;

	/** For a kill: the user id of the fighter who struck the last blow. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Killer = 0;

	/** For a respawn, a violation and a removal: the fighter's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** For a violation: what the fighter did wrong. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString What;

	/** For a violation: the detail of it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Detail;

	/** For a removal: movement or idle. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Why;

	/** For the end of the fight: the winner's user id, or 0 for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Winner = 0;
};

/** Joins the fight as a hero class. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitArenaJoinInputs
{
	GENERATED_BODY()

	/** The hero class, such as warrior; the hub's Config lists them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString HeroClass;
};

/** Where the fighter is now. A Crowdy Server Object component sends this for you; call it yourself only to move without it. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitArenaMoveInputs
{
	GENERATED_BODY()

	/** In metres in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	double Y = 0.0;
};

/** Casts an ability at a point. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitArenaCastInputs
{
	GENERATED_BODY()

	/** The ability's name, such as strike. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	FString Ability;

	/** Where to aim, in metres in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	double Y = 0.0;
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitArenaCastReply
{
	GENERATED_BODY()

	/** Whether an instant ability hit; a projectile or an area answers false and lands later. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHit = false;
};

/** Where a race is. */
UENUM(BlueprintType)
enum class ECrowdyKitRacePhase : uint8
{
	/** Racers are joining. */
	Lobby,
	/** The race has started and the racers wait on the start line for Go At. */
	Countdown,
	Racing,
	Finished
};

/** What happened in a race, as the type of its Race Event message. */
UENUM(BlueprintType)
enum class ECrowdyKitRaceEventType : uint8
{
	/** The countdown ended; the event's At is when. */
	Go,
	/** A racer started, passed a checkpoint, finished a lap or finished; the event's Stage, Player, Checkpoint, Lap and Elapsed Ms say which. */
	Timing,
	/** The server refused a racer's move; the event's Player, What and Detail say who and why. */
	Violation,
	/** A racer was taken out of the race; the event's Player and Why (left or idle) say who and why. */
	Retired,
	/** The race closed; the event's Results hold every racer, the finishers first in place order. */
	Finished
};

/** What a timing event reports. */
UENUM(BlueprintType)
enum class ECrowdyKitRaceStage : uint8
{
	Started,
	Checkpoint,
	Lap,
	Finished
};

/** One gate of a race course: a circle on the ground in metres in the world. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitRaceGate
{
	GENERATED_BODY()

	/** Its centre, in metres in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;

	/** In metres: a racer passes the gate by coming within it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Radius = 0.0;
};

/** One racer. Positions are world metres, x and y on the ground. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitRacer
{
	GENERATED_BODY()

	/** The racer's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** Where the server last kept them, in metres in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;

	/** The lap they are on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Lap = 0;

	/** The index in the course's Gates of the gate they race for next. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 NextGate = 0;

	/** Their fastest lap in milliseconds; 0 until they finish one. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 BestLapMs = 0;

	/** Their place, 1 for the winner; 0 until they finish. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Place = 0;

	/** Their time from the start in milliseconds; 0 until they finish. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 TotalMs = 0;

	/** Whether they left or went idle before finishing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bDidNotFinish = false;
};

/** One race as everyone in it sees it: the variables of Kit Race. The world is z up and every position is in metres: the ground is x, y, and a Crowdy Server Object component reports its owner's place as it moves. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitRaceState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitRacePhase Phase = ECrowdyKitRacePhase::Lobby;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString CourseId;

	/** The course in order; a racer passes them all, then repeats for each lap. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitRaceGate> Gates;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Laps = 0;

	/** When the countdown ends and the racers go, in Unix milliseconds; 0 before the race starts. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 GoAt = 0;

	/** When the race closes after the winner finished, in Unix milliseconds; 0 until there is a winner. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 ClosesAt = 0;

	/** When the race's time limit closes it, in Unix milliseconds; 0 outside a race or with no limit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 EndsAt = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitRacer> Racers;
};

/** One racer in the result of a race; Place and Total Ms are 0 for one who did not finish. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitRaceResult
{
	GENERATED_BODY()

	/** The racer's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Place = 0;

	/** Their time from the start in milliseconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 TotalMs = 0;
};

/** One thing that happened in a race, sent as its Race Event message. Only the fields of its Type are filled. An event arrives just before the snapshot that shows its effect: read the event's own fields; the state catches up on the next snapshot. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitRaceEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitRaceEventType Type = ECrowdyKitRaceEventType::Go;

	/** False when this client does not know the event's type: ignore the event. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasType = false;

	/** For a go: when the racers went, in Unix milliseconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 At = 0;

	/** For a timing: what it reports. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitRaceStage Stage = ECrowdyKitRaceStage::Started;

	/** False when this client does not know the timing's stage: ignore the event. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasStage = false;

	/** For a timing, a violation and a retirement: the racer's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** For a timing: the gate's index, for a checkpoint. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Checkpoint = 0;

	/** For a timing: the racer's lap. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int32 Lap = 0;

	/** For a timing: the time in milliseconds, a checkpoint's split, a lap's own time or a finish's total. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 ElapsedMs = 0;

	/** For a violation: what the racer did wrong. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString What;

	/** For a violation: the detail of it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Detail;

	/** For a retirement: left or idle. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Why;

	/** For the end of the race: every racer, the finishers first in place order, then those who did not finish. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitRaceResult> Results;
};

/** One point of the winner's run. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitRaceGhostFrame
{
	GENERATED_BODY()

	/** Milliseconds from the winner's start. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 AtMs = 0;

	/** Where they were, in metres in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;
};

/** The winner's run, for racers to race against; empty before there is a winner. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitRaceGhost
{
	GENERATED_BODY()

	/** The winner's user id; 0 before there is one. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** Their time from the start in milliseconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 TotalMs = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitRaceGhostFrame> Frames;
};

/** Where a ball game is. */
UENUM(BlueprintType)
enum class ECrowdyKitBallPhase : uint8
{
	Lobby,
	Playing,
	Finished
};

/** The two teams of a ball game. */
UENUM(BlueprintType)
enum class ECrowdyKitBallTeam : uint8
{
	Red,
	Blue
};

/** What happened in a ball game, as the type of its Ball Event message. */
UENUM(BlueprintType)
enum class ECrowdyKitBallEventType : uint8
{
	/** A player took the ball; the event's Player, Team and From (who held it, or 0) say who. */
	Claimed,
	/** The holder passed the ball; the event's Player, To and Team say who to. */
	Passed,
	/** The holder shot the ball; the event's Player and Team say who. */
	Shot,
	/** A goal; the event's Team, Player (the scorer) and Score say which. */
	Goal,
	/** The server refused a player's move; the event's Player, What and Detail say who and why. */
	Violation,
	/** A player left; the event's Player and Team say who. */
	Left,
	/** A player was taken out of the game; the event's Player, Team and Why (idle) say who and why. */
	Removed,
	/** The game ended; the event's Winner (when it has one) and Score say how. */
	Finished
};

/** The ball, in metres in the world, z up: its place on the ground and its speed along each axis in metres a second. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBall
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double VX = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double VY = 0.0;

	/** The user id of the player holding it; 0 while it is loose. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Holder = 0;

	/** When the holder took it, in Unix milliseconds; 0 while it is loose. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 HeldSince = 0;

	/** The user id of the player who touched it last; 0 before anyone has. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 LastTouch = 0;
};

/** One player of a ball game. Positions are world metres, x and y on the ground. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBallPlayer
{
	GENERATED_BODY()

	/** The player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitBallTeam Team = ECrowdyKitBallTeam::Red;

	/** Where the server last kept them, in metres in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Y = 0.0;
};

/** One ball game as everyone in it sees it: the variables of Kit Ball. The world is z up and every position is in metres: the ground is x, y, and a Crowdy Server Object component reports its owner's place as it moves. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBallState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitBallPhase Phase = ECrowdyKitBallPhase::Lobby;

	/** The pitch's corner in the world, in metres. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitGroundPosition Origin;

	/** The pitch's size along X, in metres; the goals are in the two walls this far apart. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Width = 0.0;

	/** The pitch's size along Y, in metres. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double Depth = 0.0;

	/** How wide each goal is along Y, in metres, centred on the pitch. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	double GoalWidth = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FCrowdyKitBall Ball;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<FCrowdyKitBallPlayer> Players;

	/** The goals of each team: red, then blue. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int32> Score;

	/** When the game ends, in Unix milliseconds; 0 outside a game being played. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 EndsAt = 0;

	/** The winning team once the game is over. Valid only while Has Winner is true. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitBallTeam Winner = ECrowdyKitBallTeam::Red;

	/** False when no team has won: before the end, or a draw. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasWinner = false;
};

/** One thing that happened in a ball game, sent as its Ball Event message. Only the fields of its Type are filled. An event arrives just before the snapshot that shows its effect: read the event's own fields; the state catches up on the next snapshot. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBallEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitBallEventType Type = ECrowdyKitBallEventType::Claimed;

	/** False when this client does not know the event's type: ignore the event. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasType = false;

	/** For every type but a finish: the player's user id (for a goal, the scorer's). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Player = 0;

	/** For a claim, a pass, a shot, a goal, a departure and a removal: the player's team (for a goal, the team that scored). Valid only while Has Team is true. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitBallTeam Team = ECrowdyKitBallTeam::Red;

	/** False when the event has no team, or this client does not know it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasTeam = false;

	/** For a claim: the user id of the player who held the ball before; 0 when it was loose. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 From = 0;

	/** For a pass: the user id of the player it went to. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 To = 0;

	/** For a goal and the end of the game: the goals of each team, red then blue. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	TArray<int32> Score;

	/** For a violation: what the player did wrong. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString What;

	/** For a violation: the detail of it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Detail;

	/** For a removal: idle. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Why;

	/** For the end of the game: the winning team. Valid only while Has Winner is true. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	ECrowdyKitBallTeam Winner = ECrowdyKitBallTeam::Red;

	/** False when the game ended in a draw, or the event is not the end of the game. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasWinner = false;
};

/** Joins a team, or changes it outside a game being played; while one is played, a player keeps their team, and one who left comes back where they left. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBallJoinInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	ECrowdyKitBallTeam Team = ECrowdyKitBallTeam::Red;
};

/** Passes the ball to a teammate. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitBallPassInputs
{
	GENERATED_BODY()

	/** The teammate's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Kit")
	int64 To = 0;
};
