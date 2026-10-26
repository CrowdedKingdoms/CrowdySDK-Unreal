#include "CrowdyGameKitStarters.h"

#include "Algo/Find.h"
#include "Algo/Reverse.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyGameKitTypes.h"
#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectSpec.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace CrowdyGameKitStartersDetail
{
	FCrowdyServerFunction Function(const TCHAR* Name, const TCHAR* Method, UScriptStruct* Params, UScriptStruct* Reply, bool bSendOnce)
	{
		FCrowdyServerFunction Out;
		Out.Name = Name;
		Out.ServerName = Method;
		Out.ParamsForm = ECrowdyServerValuesForm::Struct;
		Out.Params = Params;
		Out.ReplyForm = ECrowdyServerValuesForm::Struct;
		Out.Reply = Reply;
		Out.bSendOnce = bSendOnce;
		return Out;
	}

	FCrowdyServerMessage Message(const TCHAR* Name, const TCHAR* Topic, UScriptStruct* Payload)
	{
		FCrowdyServerMessage Out;
		Out.Name = Name;
		Out.ServerName = Topic;
		Out.PayloadForm = ECrowdyServerValuesForm::Struct;
		Out.Payload = Payload;
		Out.bPlayersCanListen = true;
		return Out;
	}

	/** Each starter's State, topic and functions, as its Rust has them. A change in one is a change to make in both. */
	void Describe(const CrowdyGameKitStarters::FStarter& Starter, UCrowdyServerObjectDefinition& Out)
	{
		const FString Type = Starter.TypeName;
		if (Type == TEXT("world"))
		{
			Out.State = FCrowdyKitWorldConditions::StaticStruct();
			Out.KitStateTopic = TEXT("world");
			Out.KitCollectionTopic = TEXT("nodes");
			Out.KitRecord = FCrowdyKitWorldNode::StaticStruct();
			Out.bOnlyOneInstance = true;
			Out.SaveIntervalSeconds = 10;
			Out.Functions = {
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitWorldView::StaticStruct(), false),
				Function(TEXT("Harvest"), TEXT("harvest"), FCrowdyKitHarvestInputs::StaticStruct(), FCrowdyKitHarvestReply::StaticStruct(), true),
				Function(TEXT("ScheduleEvent"), TEXT("schedule"), FCrowdyKitWorldEvent::StaticStruct(), nullptr, true),
				Function(TEXT("CancelEvent"), TEXT("cancel"), FCrowdyKitCancelEventInputs::StaticStruct(), FCrowdyKitCancelEventReply::StaticStruct(), true),
			};
			return;
		}
		if (Type == TEXT("matchmaker"))
		{
			Out.State = FCrowdyKitQueueCounts::StaticStruct();
			Out.KitStateTopic = TEXT("queue");
			Out.Functions = {
				Function(TEXT("Join"), TEXT("join"), FCrowdyKitMatchJoinInputs::StaticStruct(), FCrowdyKitMatchPlace::StaticStruct(), true),
				Function(TEXT("Leave"), TEXT("leave"), nullptr, nullptr, true),
				Function(TEXT("Accept"), TEXT("accept"), nullptr, FCrowdyKitMatchPlace::StaticStruct(), true),
				Function(TEXT("Decline"), TEXT("decline"), nullptr, nullptr, true),
				Function(TEXT("GetStatus"), TEXT("status"), nullptr, FCrowdyKitMatchPlace::StaticStruct(), false),
				Function(TEXT("Configure"), TEXT("configure"), FCrowdyKitMatchmakerConfig::StaticStruct(), nullptr, true),
			};
			return;
		}
		if (Type == TEXT("leaderboard"))
		{
			Out.State = FCrowdyKitBoardView::StaticStruct();
			Out.KitStateTopic = TEXT("board");
			Out.Functions = {
				Function(TEXT("GetTop"), TEXT("top"), FCrowdyKitBoardPageInputs::StaticStruct(), FCrowdyKitBoardPlaces::StaticStruct(), false),
				Function(TEXT("GetAround"), TEXT("around"), FCrowdyKitBoardAroundInputs::StaticStruct(), FCrowdyKitBoardPlaces::StaticStruct(), false),
				Function(TEXT("GetMyPlace"), TEXT("me"), nullptr, FCrowdyKitBoardPlace::StaticStruct(), false),
				Function(TEXT("GetSeason"), TEXT("season"), nullptr, FCrowdyKitBoardSeason::StaticStruct(), false),
				Function(TEXT("Submit"), TEXT("submit"), FCrowdyKitBoardSubmitInputs::StaticStruct(), FCrowdyKitBoardSubmitReply::StaticStruct(), true),
				Function(TEXT("Configure"), TEXT("configure"), FCrowdyKitBoardConfig::StaticStruct(), nullptr, true),
				Function(TEXT("Roll"), TEXT("roll"), nullptr, FCrowdyKitBoardRollReply::StaticStruct(), true),
			};
			return;
		}
		// The hub-only escrow methods (hold, settle, release, exchange) are left out: only a market's server code may call them.
		if (Type == TEXT("player"))
		{
			Out.State = FCrowdyKitPlayerState::StaticStruct();
			Out.KitStateTopic = TEXT("player");
			Out.bKitKeyedByPlayer = true;
			Out.KitOwnerValues = {
				{TEXT("inventory"), TEXT("inventory"), TEXT("Inventory")},
				{TEXT("wallet"), TEXT("wallet"), TEXT("Wallet")},
				{TEXT("progress"), TEXT("progress"), TEXT("Progress")},
			};
			Out.Functions = {
				Function(TEXT("GetMe"), TEXT("me"), nullptr, FCrowdyKitPlayerMe::StaticStruct(), false),
				Function(TEXT("GetInventory"), TEXT("inventory"), nullptr, FCrowdyKitInventory::StaticStruct(), false),
				Function(TEXT("GetWallet"), TEXT("wallet"), nullptr, FCrowdyKitWallet::StaticStruct(), false),
				Function(TEXT("GetProgress"), TEXT("progress"), nullptr, FCrowdyKitProgress::StaticStruct(), false),
				Function(TEXT("GetProfile"), TEXT("profile"), nullptr, FCrowdyKitPlayerProfile::StaticStruct(), false),
				Function(TEXT("GetRules"), TEXT("rules"), nullptr, FCrowdyKitPlayerRules::StaticStruct(), false),
				Function(TEXT("Craft"), TEXT("craft"), FCrowdyKitCraftInputs::StaticStruct(), FCrowdyKitCraftReply::StaticStruct(), true),
				Function(TEXT("OpenLoot"), TEXT("open"), FCrowdyKitOpenLootInputs::StaticStruct(), FCrowdyKitOpenLootReply::StaticStruct(), true),
				Function(TEXT("Collect"), TEXT("collect"), nullptr, FCrowdyKitBundle::StaticStruct(), true),
				Function(TEXT("AcceptQuest"), TEXT("accept"), FCrowdyKitQuestInputs::StaticStruct(), nullptr, true),
				Function(TEXT("ClaimQuest"), TEXT("claim"), FCrowdyKitQuestInputs::StaticStruct(), FCrowdyKitBundle::StaticStruct(), true),
				Function(TEXT("Step"), TEXT("step"), FCrowdyKitStepInputs::StaticStruct(), FCrowdyKitEarned::StaticStruct(), true),
				Function(TEXT("Discard"), TEXT("discard"), FCrowdyKitDiscardInputs::StaticStruct(), nullptr, true),
				Function(TEXT("TakeMail"), TEXT("take_mail"), nullptr, FCrowdyKitBundle::StaticStruct(), true),
				Function(TEXT("Grant"), TEXT("grant"), FCrowdyKitGrantInputs::StaticStruct(), FCrowdyKitEarned::StaticStruct(), true),
				Function(TEXT("Record"), TEXT("record"), FCrowdyKitRecordInputs::StaticStruct(), FCrowdyKitEarned::StaticStruct(), true),
				Function(TEXT("Deposit"), TEXT("deposit"), FCrowdyKitDepositInputs::StaticStruct(), nullptr, true),
			};
			return;
		}
		if (Type == TEXT("market"))
		{
			Out.State = FCrowdyKitMarketState::StaticStruct();
			Out.KitStateTopic = TEXT("market");
			Out.KitOwnerValues = {
				{TEXT("orders"), TEXT("orders"), TEXT("Orders")},
				{TEXT("trades"), TEXT("trades"), TEXT("Trades")},
			};
			Out.Functions = {
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitMarketState::StaticStruct(), false),
				Function(TEXT("GetVendors"), TEXT("vendors"), nullptr, FCrowdyKitVendors::StaticStruct(), false),
				Function(TEXT("GetBook"), TEXT("book"), FCrowdyKitBookInputs::StaticStruct(), FCrowdyKitBook::StaticStruct(), false),
				Function(TEXT("GetOrders"), TEXT("orders"), nullptr, FCrowdyKitOrders::StaticStruct(), false),
				Function(TEXT("GetTrades"), TEXT("trades"), nullptr, FCrowdyKitTrades::StaticStruct(), false),
				Function(TEXT("Buy"), TEXT("buy"), FCrowdyKitShopInputs::StaticStruct(), FCrowdyKitBundle::StaticStruct(), true),
				Function(TEXT("Sell"), TEXT("sell"), FCrowdyKitShopInputs::StaticStruct(), FCrowdyKitBundle::StaticStruct(), true),
				Function(TEXT("PlaceOrder"), TEXT("order"), FCrowdyKitOrderInputs::StaticStruct(), FCrowdyKitPlacedOrder::StaticStruct(), true),
				Function(TEXT("CancelOrder"), TEXT("cancel"), FCrowdyKitCancelOrderInputs::StaticStruct(), nullptr, true),
				Function(TEXT("OpenTrade"), TEXT("trade"), FCrowdyKitOpenTradeInputs::StaticStruct(), FCrowdyKitTradeReply::StaticStruct(), true),
				Function(TEXT("Offer"), TEXT("offer"), FCrowdyKitTradeOfferInputs::StaticStruct(), nullptr, true),
				Function(TEXT("AcceptTrade"), TEXT("accept"), FCrowdyKitTradeInputs::StaticStruct(), FCrowdyKitAcceptTradeReply::StaticStruct(), true),
				Function(TEXT("DeclineTrade"), TEXT("decline"), FCrowdyKitTradeInputs::StaticStruct(), nullptr, true),
			};
			return;
		}
		// An area's State is empty and has no topic: its mobs, NPCs and pets are its records, of three shapes told apart by their type_name.
		if (Type == TEXT("npcs"))
		{
			UScriptStruct* Member = FCrowdyKitAreaMember::StaticStruct();
			UEnum* MemberType = StaticEnum<ECrowdyKitAreaMemberType>();
			Out.State = FCrowdyKitAreaState::StaticStruct();
			Out.KitCollectionTopic = TEXT("area");
			Out.KitRecord = Member;
			Out.bReadsWorld = true;
			// The hub reads its key as a place on this grid, so This Actor's Area names the area a player stands in.
			Out.AreaSizeChunks = 4;
			Out.FieldNames = {{Member, GET_MEMBER_NAME_CHECKED(FCrowdyKitAreaMember, Type), TEXT("type_name")}};
			Out.EnumValueNames = {{MemberType, TEXT("Mob"), TEXT("Mob")}, {MemberType, TEXT("Npc"), TEXT("Npc")}, {MemberType, TEXT("Pet"), TEXT("Pet")}};
			for (const TCHAR* Kind : {TEXT("mob"), TEXT("npc"), TEXT("pet")})
			{
				Out.KitActors.Add(Kind);
			}
			Out.Functions = {
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitAreaMembers::StaticStruct(), false),
				Function(TEXT("Attack"), TEXT("attack"), FCrowdyKitAttackInputs::StaticStruct(), FCrowdyKitAttackReply::StaticStruct(), true),
				Function(TEXT("Talk"), TEXT("talk"), FCrowdyKitTalkInputs::StaticStruct(), Member, false),
				Function(TEXT("Adopt"), TEXT("adopt"), FCrowdyKitAdoptInputs::StaticStruct(), FCrowdyKitAdoptReply::StaticStruct(), true),
				Function(TEXT("RenamePet"), TEXT("rename"), FCrowdyKitRenamePetInputs::StaticStruct(), nullptr, true),
				Function(TEXT("SummonPet"), TEXT("summon"), FCrowdyKitPetInputs::StaticStruct(), nullptr, true),
				Function(TEXT("DismissPet"), TEXT("dismiss"), FCrowdyKitPetInputs::StaticStruct(), nullptr, true),
				Function(TEXT("ReleasePet"), TEXT("release"), FCrowdyKitPetInputs::StaticStruct(), nullptr, true),
				Function(TEXT("GetPets"), TEXT("pets"), nullptr, FCrowdyKitAreaMembers::StaticStruct(), false),
			};
			return;
		}
		if (Type == TEXT("minigame"))
		{
			Out.State = FCrowdyKitMinigameState::StaticStruct();
			Out.KitStateTopic = TEXT("duel");
			Out.Functions = {
				Function(TEXT("Join"), TEXT("join"), nullptr, FCrowdyKitMinigameJoinReply::StaticStruct(), true),
				Function(TEXT("Leave"), TEXT("leave"), nullptr, nullptr, true),
				Function(TEXT("Throw"), TEXT("throw"), FCrowdyKitMinigameThrowInputs::StaticStruct(), FCrowdyKitMinigameThrowReply::StaticStruct(), true),
				Function(TEXT("Rematch"), TEXT("rematch"), nullptr, nullptr, true),
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitMinigameState::StaticStruct(), false),
			};
			return;
		}
		if (Type == TEXT("tactics"))
		{
			Out.State = FCrowdyKitTacticsState::StaticStruct();
			Out.KitStateTopic = TEXT("battle");
			Out.Functions = {
				Function(TEXT("Join"), TEXT("join"), nullptr, nullptr, true),
				Function(TEXT("Start"), TEXT("start"), nullptr, nullptr, true),
				Function(TEXT("Move"), TEXT("move"), FCrowdyKitTacticsMoveInputs::StaticStruct(), nullptr, true),
				Function(TEXT("Attack"), TEXT("attack"), FCrowdyKitTacticsAttackInputs::StaticStruct(), FCrowdyKitTacticsAttackReply::StaticStruct(), true),
				Function(TEXT("EndTurn"), TEXT("end_turn"), nullptr, nullptr, true),
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitTacticsState::StaticStruct(), false),
			};
			return;
		}
		// The next three send all their events as one message on their State Topic, whose Type says which.
		if (Type == TEXT("director"))
		{
			Out.State = FCrowdyKitDirectorState::StaticStruct();
			Out.KitStateTopic = TEXT("encounter");
			Out.Messages = {Message(TEXT("EncounterEvent"), TEXT("encounter"), FCrowdyKitDirectorEvent::StaticStruct())};
			Out.Functions = {
				Function(TEXT("Join"), TEXT("join"), nullptr, nullptr, true),
				Function(TEXT("Start"), TEXT("start"), nullptr, nullptr, true),
				Function(TEXT("Build"), TEXT("build"), FCrowdyKitDirectorBuildInputs::StaticStruct(), FCrowdyKitDirectorBuildReply::StaticStruct(), true),
				Function(TEXT("Hit"), TEXT("hit"), FCrowdyKitDirectorHitInputs::StaticStruct(), FCrowdyKitDirectorHitReply::StaticStruct(), true),
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitDirectorState::StaticStruct(), false),
			};
			return;
		}
		if (Type == TEXT("instance"))
		{
			Out.State = FCrowdyKitInstanceState::StaticStruct();
			Out.KitStateTopic = TEXT("run");
			Out.Functions = {
				Function(TEXT("Open"), TEXT("open"), FCrowdyKitInstanceOpenInputs::StaticStruct(), FCrowdyKitInstanceOpenReply::StaticStruct(), true),
				Function(TEXT("Invite"), TEXT("invite"), FCrowdyKitInstanceInviteInputs::StaticStruct(), nullptr, true),
				Function(TEXT("Join"), TEXT("join"), nullptr, nullptr, true),
				Function(TEXT("Enter"), TEXT("enter"), nullptr, FCrowdyKitInstanceEnterReply::StaticStruct(), true),
				Function(TEXT("Descend"), TEXT("descend"), nullptr, FCrowdyKitInstanceDescendReply::StaticStruct(), true),
				Function(TEXT("Abandon"), TEXT("abandon"), nullptr, nullptr, true),
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitInstanceState::StaticStruct(), false),
			};
			return;
		}
		if (Type == TEXT("territory"))
		{
			Out.State = FCrowdyKitTerritoryState::StaticStruct();
			Out.KitStateTopic = TEXT("territory");
			Out.bReadsWorld = true;
			Out.Messages = {Message(TEXT("TerritoryEvent"), TEXT("territory"), FCrowdyKitTerritoryEvent::StaticStruct())};
			Out.Functions = {
				Function(TEXT("Join"), TEXT("join"), FCrowdyKitTerritoryJoinInputs::StaticStruct(), nullptr, true),
				Function(TEXT("Reset"), TEXT("reset"), nullptr, nullptr, true),
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitTerritoryState::StaticStruct(), false),
			};
			return;
		}
		if (Type == TEXT("zone"))
		{
			Out.State = FCrowdyKitZoneState::StaticStruct();
			Out.KitStateTopic = TEXT("zone");
			Out.bReadsWorld = true;
			Out.Messages = {Message(TEXT("ZoneEvent"), TEXT("zone"), FCrowdyKitZoneEvent::StaticStruct())};
			Out.Functions = {
				Function(TEXT("Join"), TEXT("join"), nullptr, nullptr, true),
				Function(TEXT("Start"), TEXT("start"), nullptr, nullptr, true),
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitZoneState::StaticStruct(), false),
			};
			return;
		}
		// The hub sends each card as text and as {id, rank, suit}; the overrides read the latter and skip the text keys.
		if (Type == TEXT("deck"))
		{
			UScriptStruct* Hand = FCrowdyKitDeckHand::StaticStruct();
			UScriptStruct* Played = FCrowdyKitDeckPlayed::StaticStruct();
			UScriptStruct* DeckState = FCrowdyKitDeckState::StaticStruct();
			UEnum* Suit = StaticEnum<ECrowdyKitCardSuit>();
			Out.State = DeckState;
			Out.KitStateTopic = TEXT("table");
			Out.KitOwnerValues = {{TEXT("hand"), TEXT("hand"), TEXT("Hand")}};
			Out.FieldNames = {
				{Hand, GET_MEMBER_NAME_CHECKED(FCrowdyKitDeckHand, Cards), TEXT("faces")},
				{Hand, GET_MEMBER_NAME_CHECKED(FCrowdyKitDeckHand, Playable), TEXT("playable_faces")},
				{DeckState, GET_MEMBER_NAME_CHECKED(FCrowdyKitDeckState, Top), TEXT("top_face")},
				{Played, GET_MEMBER_NAME_CHECKED(FCrowdyKitDeckPlayed, Card), TEXT("face")},
			};
			Out.EnumValueNames = {{Suit, TEXT("Spades"), TEXT("S")}, {Suit, TEXT("Hearts"), TEXT("H")}, {Suit, TEXT("Diamonds"), TEXT("D")}, {Suit, TEXT("Clubs"), TEXT("C")}};
			Out.Functions = {
				Function(TEXT("Join"), TEXT("join"), nullptr, nullptr, true),
				Function(TEXT("Leave"), TEXT("leave"), nullptr, nullptr, true),
				Function(TEXT("Start"), TEXT("start"), nullptr, nullptr, true),
				Function(TEXT("Play"), TEXT("play"), FCrowdyKitDeckPlayInputs::StaticStruct(), FCrowdyKitDeckPlayReply::StaticStruct(), true),
				Function(TEXT("Draw"), TEXT("draw"), nullptr, Hand, true),
				Function(TEXT("Pass"), TEXT("pass"), nullptr, nullptr, true),
				Function(TEXT("GetHand"), TEXT("hand"), nullptr, Hand, false),
				Function(TEXT("GetState"), TEXT("state"), nullptr, DeckState, false),
			};
			return;
		}
		// Every position is in world metres; a Crowdy Server Object component reports its owner's through Move.
		if (Type == TEXT("arena"))
		{
			UScriptStruct* Fighter = FCrowdyKitArenaFighter::StaticStruct();
			UScriptStruct* JoinInputs = FCrowdyKitArenaJoinInputs::StaticStruct();
			Out.State = FCrowdyKitArenaState::StaticStruct();
			Out.KitStateTopic = TEXT("arena");
			Out.Messages = {Message(TEXT("ArenaEvent"), TEXT("arena"), FCrowdyKitArenaEvent::StaticStruct())};
			Out.FieldNames = {
				{Fighter, GET_MEMBER_NAME_CHECKED(FCrowdyKitArenaFighter, HeroClass), TEXT("class")},
				{JoinInputs, GET_MEMBER_NAME_CHECKED(FCrowdyKitArenaJoinInputs, HeroClass), TEXT("class")},
			};
			Out.KitPositionFunction = TEXT("Move");
			Out.KitPositionRate = 5.f;
			Out.Functions = {
				Function(TEXT("Join"), TEXT("join"), JoinInputs, nullptr, true),
				Function(TEXT("Leave"), TEXT("leave"), nullptr, nullptr, true),
				Function(TEXT("Start"), TEXT("start"), nullptr, nullptr, true),
				Function(TEXT("Move"), TEXT("move"), FCrowdyKitArenaMoveInputs::StaticStruct(), FCrowdyKitGroundPosition::StaticStruct(), false),
				Function(TEXT("Cast"), TEXT("cast"), FCrowdyKitArenaCastInputs::StaticStruct(), FCrowdyKitArenaCastReply::StaticStruct(), true),
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitArenaState::StaticStruct(), false),
			};
			return;
		}
		if (Type == TEXT("race"))
		{
			UScriptStruct* Ground = FCrowdyKitGroundPosition::StaticStruct();
			Out.State = FCrowdyKitRaceState::StaticStruct();
			Out.KitStateTopic = TEXT("race");
			Out.Messages = {Message(TEXT("RaceEvent"), TEXT("race"), FCrowdyKitRaceEvent::StaticStruct())};
			Out.KitPositionFunction = TEXT("Move");
			Out.KitPositionRate = 5.f;
			Out.Functions = {
				Function(TEXT("Join"), TEXT("join"), nullptr, nullptr, true),
				Function(TEXT("Leave"), TEXT("leave"), nullptr, nullptr, true),
				Function(TEXT("Start"), TEXT("start"), nullptr, nullptr, true),
				Function(TEXT("Move"), TEXT("move"), Ground, Ground, false),
				Function(TEXT("GetGhost"), TEXT("ghost"), nullptr, FCrowdyKitRaceGhost::StaticStruct(), false),
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitRaceState::StaticStruct(), false),
			};
			return;
		}
		// The ball is also shown to players near it as a Kit Actor of kind ball, which the project draws as a class of its choosing.
		if (Type == TEXT("ball"))
		{
			UScriptStruct* Ground = FCrowdyKitGroundPosition::StaticStruct();
			Out.State = FCrowdyKitBallState::StaticStruct();
			Out.KitStateTopic = TEXT("ball");
			Out.Messages = {Message(TEXT("BallEvent"), TEXT("ball"), FCrowdyKitBallEvent::StaticStruct())};
			Out.KitActors.Add(TEXT("ball"));
			Out.KitPositionFunction = TEXT("Move");
			Out.KitPositionRate = 5.f;
			Out.Functions = {
				Function(TEXT("Join"), TEXT("join"), FCrowdyKitBallJoinInputs::StaticStruct(), nullptr, true),
				Function(TEXT("Leave"), TEXT("leave"), nullptr, nullptr, true),
				Function(TEXT("Start"), TEXT("start"), nullptr, nullptr, true),
				Function(TEXT("Move"), TEXT("move"), Ground, Ground, false),
				Function(TEXT("Claim"), TEXT("claim"), nullptr, nullptr, true),
				Function(TEXT("Pass"), TEXT("pass"), FCrowdyKitBallPassInputs::StaticStruct(), nullptr, true),
				Function(TEXT("Shoot"), TEXT("shoot"), Ground, nullptr, true),
				Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitBallState::StaticStruct(), false),
			};
			return;
		}
		Out.State = FCrowdyKitSessionState::StaticStruct();
		Out.KitStateTopic = TEXT("session");
		Out.Functions = {
			Function(TEXT("Join"), TEXT("join"), nullptr, nullptr, true),
			Function(TEXT("Leave"), TEXT("leave"), nullptr, nullptr, true),
			Function(TEXT("Ready"), TEXT("ready"), FCrowdyKitSessionReadyInputs::StaticStruct(), FCrowdyKitSessionReadyReply::StaticStruct(), true),
			Function(TEXT("Start"), TEXT("start"), nullptr, nullptr, true),
			Function(TEXT("Act"), TEXT("act"), FCrowdyKitSessionActInputs::StaticStruct(), FCrowdyKitSessionActReply::StaticStruct(), true),
			Function(TEXT("GetState"), TEXT("state"), nullptr, FCrowdyKitSessionState::StaticStruct(), false),
			Function(TEXT("Configure"), TEXT("configure"), FCrowdyKitSessionConfig::StaticStruct(), nullptr, true),
		};
	}

	FString PackageOf(const CrowdyGameKitStarters::FStarter& Starter)
	{
		return FString(CrowdyGameKitStarters::ContentPath) / Starter.AssetName;
	}

	void Visit(const CrowdyGameKitStarters::FStarter& Starter, TArray<const CrowdyGameKitStarters::FStarter*>& Visited, TArray<const CrowdyGameKitStarters::FStarter*>& OutOrder)
	{
		if (Visited.Contains(&Starter))
		{
			return;
		}
		Visited.Add(&Starter);
		if (const CrowdyGameKitStarters::FStarter* Parent = CrowdyGameKitStarters::FindStarter(Starter.Parent))
		{
			Visit(*Parent, Visited, OutOrder);
		}
		OutOrder.Add(&Starter);
		for (const TCHAR* Called : Starter.Calls)
		{
			if (const CrowdyGameKitStarters::FStarter* Callee = CrowdyGameKitStarters::FindStarter(Called))
			{
				Visit(*Callee, Visited, OutOrder);
			}
		}
	}

	/** A numeric Config key the starter's server code reads as an integer, and the range it takes; a wrong type or range fails the starter's start. */
	struct FConfigLimit
	{
		const TCHAR* Starter;
		const TCHAR* Key;
		int64 Min;
		int64 Max;
	};

	constexpr int64 MaxExactMilliseconds = 9007199254740992;

	const FConfigLimit ConfigLimits[] = {
		{TEXT("world"), TEXT("day_ms"), 0, MaxExactMilliseconds},
		{TEXT("world"), TEXT("nodes"), 0, 65535},
		{TEXT("world"), TEXT("node_amount"), 0, 4294967295},
		{TEXT("world"), TEXT("regrow_ms"), 0, MaxExactMilliseconds},
		{TEXT("npcs"), TEXT("z"), -4294967296, 4294967296},
	};

	/** A top-level Config key as the starters' server code reads it: the name compares with case, and a number keeps its text. */
	struct FConfigEntry
	{
		FString Key;
		EJsonNotation Kind = EJsonNotation::Null;
		FString Text;
	};

	TArray<FConfigEntry> ReadConfigEntries(const FString& Config)
	{
		TArray<FConfigEntry> Entries;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Config);
		EJsonNotation Notation;
		if (!Reader->ReadNext(Notation) || Notation != EJsonNotation::ObjectStart)
		{
			return Entries;
		}
		while (Reader->ReadNext(Notation) && Notation != EJsonNotation::ObjectEnd && Notation != EJsonNotation::Error)
		{
			FConfigEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Key = Reader->GetIdentifier();
			Entry.Kind = Notation;
			if (Notation == EJsonNotation::String)
			{
				Entry.Text = Reader->GetValueAsString();
			}
			else if (Notation == EJsonNotation::Number)
			{
				Entry.Text = Reader->GetValueAsNumberString();
			}
			else if (Notation == EJsonNotation::ObjectStart)
			{
				Reader->SkipObject();
			}
			else if (Notation == EJsonNotation::ArrayStart)
			{
				Reader->SkipArray();
			}
		}
		return Entries;
	}

	const FConfigEntry* FindEntry(const TArray<FConfigEntry>& Entries, const TCHAR* Key)
	{
		return Entries.FindByPredicate([Key](const FConfigEntry& Entry) { return Entry.Key.Equals(Key, ESearchCase::CaseSensitive); });
	}

	/** Whether a number's text is an integer within the limit as the server code's parser takes it: no fraction or exponent, no minus on an unsigned key, no -0, and nothing longer than any limit's digits (so the parse cannot overflow). */
	bool IsWholeWithin(const FString& Text, const FConfigLimit& Limit)
	{
		if (Text.Len() > 17 || Text.Contains(TEXT("."), ESearchCase::CaseSensitive) || Text.Contains(TEXT("e"), ESearchCase::IgnoreCase) || Text == TEXT("-0") || (Limit.Min == 0 && Text.StartsWith(TEXT("-"))))
		{
			return false;
		}
		const int64 Number = FCString::Atoi64(*Text);
		return Number >= Limit.Min && Number <= Limit.Max;
	}

	bool CopyCrate(const CrowdyGameKitStarters::FStarter& Starter, const FString& Directory, FString& OutError)
	{
		const FString Source = CrowdyGameKitStarters::GetCrateSource(Starter);
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *(Source / TEXT("src")), TEXT("*.rs"), true, false);
		Files.Add(Source / TEXT("Cargo.toml"));
		for (const FString& File : Files)
		{
			FString Relative = File;
			FPaths::MakePathRelativeTo(Relative, *(Source + TEXT("/")));
			if (IFileManager::Get().Copy(*(Directory / Relative), *File) != COPY_OK)
			{
				OutError = FString::Printf(TEXT("could not copy %s to %s"), *File, *(Directory / Relative));
				return false;
			}
		}
		return true;
	}

	bool SaveDefinition(const CrowdyGameKitStarters::FStarter& Starter, const FString& Config, UCrowdyServerObjectDefinition*& OutSaved, FString& OutError)
	{
		const FString PackageName = PackageOf(Starter);
		UPackage* Package = CreatePackage(*PackageName);
		UCrowdyServerObjectDefinition* Asset = CrowdyGameKitStarters::MakeDefinition(Starter, Package, FName(Starter.AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("%s does not bake; reinstall the Crowdy SDK"), Starter.AssetName);
			return false;
		}
		Asset->KitConfig = Config;
		FAssetRegistryModule::AssetCreated(Asset);
		if (!CrowdyServerObjectSpec::SaveDefinitionPackage(*Asset, OutError))
		{
			return false;
		}
		OutSaved = Asset;
		return true;
	}
}

TConstArrayView<CrowdyGameKitStarters::FStarter> CrowdyGameKitStarters::GetStarters()
{
	static const TArray<FStarter> Starters = {
		{TEXT("world"), TEXT("CSO_Kit_World"),
			TEXT("The Game Kit's world: its clock, weather, resource nodes and liveops events, and the players online. While the project has it, it is the app's root."),
			TEXT(""), {}, {TEXT("schedule"), TEXT("cancel")}},
		{TEXT("matchmaker"), TEXT("CSO_Kit_Matchmaker"),
			TEXT("Matchmaking queues, one per Instance Id (ranked, casual): a rating window that widens while players wait, and a match every player must accept, which reserves a session."),
			TEXT("world"), {TEXT("session")}, {TEXT("configure")}},
		// A session's configure is the host's too, so players may call it.
		{TEXT("session"), TEXT("CSO_Kit_Session"),
			TEXT("A game session, one per Instance Id, from lobby to result: a host, ready checks, turns with a time limit, scores and a winner. With a board in its Config, a finished session posts each player's score to that leaderboard."),
			TEXT("matchmaker"), {}, {}, {TEXT("leaderboard")}},
		{TEXT("leaderboard"), TEXT("CSO_Kit_Leaderboard"),
			TEXT("Leaderboards, one per Instance Id (weekly-score, fastest-lap): the hub ranks the scores other server code submits, with ties, seasons on a schedule and the last season's standings."),
			TEXT("world"), {}, {TEXT("submit"), TEXT("configure"), TEXT("roll")}},
		{TEXT("player"), TEXT("CSO_Kit_Player"),
			TEXT("A player's own hub, one per player (the Instance Id is their user id): their items, in-game soft currency (never real money), crafting, loot, idle production, levels, achievements and quests. Only they read their inventory, wallet and progress; the app's developers and its server code grant and deposit."),
			TEXT("world"), {}, {TEXT("grant"), TEXT("record"), TEXT("deposit")}, {}, true},
		{TEXT("market"), TEXT("CSO_Kit_Market"),
			TEXT("Markets, one per Instance Id (town, harbor): vendors with fixed prices in in-game soft currency, an order book per listed item, and trades between two players. It holds nothing itself: what a player puts up waits in their own player hub until it settles."),
			TEXT("world"), {TEXT("player")}},
		{TEXT("npcs"), TEXT("CSO_Kit_Npcs"),
			TEXT("An area's mobs, NPCs and pets, one area per Instance Id (its x,y on a grid of 4 by 4 chunks, as This Actor's Area gives it): mobs that chase and take players' hits, NPCs that walk and talk, and players' pets. They are shown to players near them as actors at height 0, drawn as the classes in Kit Actors."),
			TEXT("world"), {TEXT("world")}},
		{TEXT("minigame"), TEXT("CSO_Kit_Minigame"),
			TEXT("A duel, one per Instance Id (a table): two players each throw a hand that stays hidden until both are in or the round's time runs out, and the first to win a majority of the rounds wins. Rock, paper, scissors is the game; replace its hands and rule with yours."),
			TEXT("world")},
		{TEXT("tactics"), TEXT("CSO_Kit_Tactics"),
			TEXT("A grid battle, one per Instance Id: each player brings a unit and the host starts it; in the players' phase every unit moves and attacks once, then the hub's enemies move and strike, until one side has no unit standing. Its board is x, z cells."),
			TEXT("world")},
		{TEXT("director"), TEXT("CSO_Kit_Director"),
			TEXT("An encounter, one per Instance Id: waves of enemies walk a lane to the players' base, players build towers with the gold kills pay and strike enemies themselves, and the last wave brings a boss that changes phase as it weakens. Its events arrive as the Encounter Event message."),
			TEXT("world")},
		{TEXT("instance"), TEXT("CSO_Kit_Instance"),
			TEXT("A dungeon run, one per Instance Id: a party plays down floors the hub generates from the run's seed, room by room (fight, treasure, rest, boss), until it clears the last floor, is wiped, abandons or leaves it idle. With a board in its Config, a cleared or wiped run posts each member's depth to that leaderboard."),
			TEXT("world"), {}, {}, {TEXT("leaderboard")}},
		{TEXT("territory"), TEXT("CSO_Kit_Territory"),
			TEXT("A war over control points, one per Instance Id (a map), z up: players join a faction, a faction captures a point by having more members standing on it than any other, points pay income, and the first faction to the winning score wins. Each scan reads where players are in the world."),
			TEXT("world"), {}, {TEXT("reset")}},
		{TEXT("zone"), TEXT("CSO_Kit_Zone"),
			TEXT("A shrinking circle, one per Instance Id (a match), z up: it holds, then shrinks phase by phase, and wears down every player outside it until one is left. Each scan reads where players are in the world."),
			TEXT("world")},
		{TEXT("deck"), TEXT("CSO_Kit_Deck"),
			TEXT("A card table, one per Instance Id: two to six players are dealt five cards from a deck of 52 and take turns on a time limit, playing a card that matches the top card's suit or rank or drawing one, until one empties their hand. An eight can go on anything and names the next suit; the starter's game is crazy eights, and you replace its rule with yours. Everyone sees the top card, the suit in play and how many cards each player holds; each player reads only their own hand."),
			TEXT("world"), {}, {}, {}, true},
		{TEXT("arena"), TEXT("CSO_Kit_Arena"),
			TEXT("A fight, one per Instance Id (an arena), z up: players join as a hero class, move about a rectangle of the world and cast abilities at points in it, instants and projectiles and areas, until one reaches the kill target or time runs out. Every position is in world metres, x and y on the ground, and a Crowdy Server Object component reports its owner's place as it moves; the hub keeps a fighter to what their class can do and sets them back when they ask for more. Its events arrive as the Arena Event message."),
			TEXT("world")},
		{TEXT("race"), TEXT("CSO_Kit_Race"),
			TEXT("A race, one per Instance Id, z up: racers join, a countdown holds them on the start line, then they drive a course of gates for a number of laps, with checkpoint and lap times, places and the winner's run to race against as a ghost. Every position is in world metres, x and y on the ground, and a Crowdy Server Object component reports its owner's place as it moves; the hub counts a gate passed when the path between two reports crosses it, and sets back a racer who goes faster or jumps further than the rules allow. The race closes when every racer has finished, left or stopped reporting, a grace after the winner finishes (30 seconds by default), or when its time limit runs out. With a board in its Config, a finished race posts each finisher's time to that leaderboard. Its events arrive as the Race Event message."),
			TEXT("world"), {}, {}, {TEXT("leaderboard")}},
		{TEXT("ball"), TEXT("CSO_Kit_Ball"),
			TEXT("A ball game for two teams, red and blue, one per Instance Id (a match), z up: players claim the ball, carry it, pass it and shoot it at the other team's goal, and the first team to the winning score, or the higher score when time runs out, wins. The hub moves the ball. Every position is in world metres, x and y on the ground, and a Crowdy Server Object component reports its owner's place as it moves; the hub sets back a player who goes faster or jumps further than the rules allow. When its Config's realtime is on, its ball is shown to players near it as an actor of kind ball, drawn as the class in Kit Actors. Its events arrive as the Ball Event message."),
			TEXT("world")},
	};
	return Starters;
}

const CrowdyGameKitStarters::FStarter* CrowdyGameKitStarters::FindStarter(const FString& TypeName)
{
	return Algo::FindByPredicate(GetStarters(), [&TypeName](const FStarter& Starter) { return TypeName.Equals(Starter.TypeName, ESearchCase::CaseSensitive); });
}

FString CrowdyGameKitStarters::ConfigToAdd(const FStarter& Starter, const FString& Config)
{
	const FString Given = Config.TrimStartAndEnd();
	if (!Given.IsEmpty() || !Starter.bMakesSecret)
	{
		return Given;
	}
	return FString::Printf(TEXT("{\"secret\": \"%s\"}"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString CrowdyGameKitStarters::FindConfigProblem(const FString& TypeName, const FString& Config)
{
	using namespace CrowdyGameKitStartersDetail;
	const TArray<FConfigEntry> Entries = ReadConfigEntries(Config);
	const FStarter* Starter = FindStarter(TypeName);
	const FConfigEntry* Secret = Starter && Starter->bMakesSecret ? FindEntry(Entries, TEXT("secret")) : nullptr;
	if (Secret && Secret->Kind != EJsonNotation::String)
	{
		return TEXT("\"secret\" set to something other than text");
	}
	for (const FConfigLimit& Limit : ConfigLimits)
	{
		const FConfigEntry* Entry = TypeName.Equals(Limit.Starter, ESearchCase::CaseSensitive) ? FindEntry(Entries, Limit.Key) : nullptr;
		if (Entry && !(Entry->Kind == EJsonNotation::Number && IsWholeWithin(Entry->Text, Limit)))
		{
			return FString::Printf(TEXT("\"%s\" set to something other than a whole number from %lld to %lld"), Limit.Key, Limit.Min, Limit.Max);
		}
	}
	return FString();
}

bool CrowdyGameKitStarters::HasConfigSecret(const FString& Config)
{
	using namespace CrowdyGameKitStartersDetail;
	const TArray<FConfigEntry> Entries = ReadConfigEntries(Config);
	const FConfigEntry* Secret = FindEntry(Entries, TEXT("secret"));
	return Secret && Secret->Kind == EJsonNotation::String && !Secret->Text.IsEmpty();
}

bool CrowdyGameKitStarters::IsDeveloperOnly(const UCrowdyServerObjectDefinition& Definition, const FCrowdyServerFunction& Function)
{
	const FStarter* Starter = Definition.IsGameKitStarter() ? FindStarter(Definition.TypeName) : nullptr;
	return Starter && Starter->DeveloperMethods.ContainsByPredicate([&Function](const TCHAR* Method) { return Function.ServerName.Equals(Method, ESearchCase::CaseSensitive); });
}

UCrowdyServerObjectDefinition* CrowdyGameKitStarters::MakeDefinition(const FStarter& Starter, UObject* Outer, FName Name, EObjectFlags Flags)
{
	UCrowdyServerObjectDefinition* Definition = NewObject<UCrowdyServerObjectDefinition>(Outer, Name, Flags);
	Definition->TypeName = Starter.TypeName;
	Definition->Description = Starter.Description;
	Definition->CodeSource = ECrowdyServerCodeSource::GameKitStarter;
	Definition->StateForm = ECrowdyServerValuesForm::Struct;
	Definition->KitParent = Starter.Parent;
	for (const TCHAR* Called : Starter.Calls)
	{
		Definition->KitCalls.Add(Called);
	}
	for (const TCHAR* Called : Starter.OptionalCalls)
	{
		Definition->KitCalls.Add(Called);
	}
	CrowdyGameKitStartersDetail::Describe(Starter, *Definition);
	for (FCrowdyServerFunction& Function : Definition->Functions)
	{
		if (IsDeveloperOnly(*Definition, Function))
		{
			Function.WhoCanCall = ECrowdyServerFunctionCaller::DevelopersOnly;
		}
	}
	TArray<FString> Errors;
	return Definition->Bake(Errors) ? Definition : nullptr;
}

FString CrowdyGameKitStarters::GetCrateSource(const FStarter& Starter)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
	return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/GameKit") / Starter.TypeName) : FString();
}

TArray<const CrowdyGameKitStarters::FStarter*> CrowdyGameKitStarters::PlanAdd(const FString& TypeName, TConstArrayView<const UCrowdyServerObjectDefinition*> Project, FString& OutProblem)
{
	const FStarter* Starter = FindStarter(TypeName);
	if (!Starter)
	{
		OutProblem = FString::Printf(TEXT("The Crowdy SDK has no Game Kit starter named %s"), *TypeName);
		return {};
	}
	TArray<const FStarter*> Visited;
	TArray<const FStarter*> Order;
	CrowdyGameKitStartersDetail::Visit(*Starter, Visited, Order);
	TArray<const FStarter*> Plan;
	for (const FStarter* Each : Order)
	{
		const UCrowdyServerObjectDefinition* const* Same = Project.FindByPredicate([Each](const UCrowdyServerObjectDefinition* Definition)
		{
			return Definition && Definition->TypeName.Equals(Each->TypeName, ESearchCase::CaseSensitive);
		});
		if (!Same)
		{
			Plan.Add(Each);
			continue;
		}
		if (!(*Same)->IsGameKitStarter())
		{
			OutProblem = FString::Printf(TEXT("The project already has a Server Object type named %s (%s), which the %s starter needs; rename that type first"),
				Each->TypeName, *(*Same)->GetPathName(), Each->TypeName);
			return {};
		}
	}
	return Plan;
}

bool CrowdyGameKitStarters::AddStarter(const FString& TypeName, TConstArrayView<const UCrowdyServerObjectDefinition*> Project, const FString& ServerDirectory,
	TArray<FString>& OutAdded, FString& OutError, const FString& Config)
{
	using namespace CrowdyGameKitStartersDetail;
	OutAdded.Reset();
	const TArray<const FStarter*> Plan = PlanAdd(TypeName, Project, OutError);
	if (!OutError.IsEmpty())
	{
		return false;
	}
	TSharedPtr<FJsonObject> ConfigObject;
	if (!Config.TrimStartAndEnd().IsEmpty() && !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Config), ConfigObject))
	{
		OutError = FString::Printf(TEXT("the Config for %s is not a JSON object: %s"), *TypeName, *Config);
		return false;
	}
	const FString ConfigProblem = ConfigObject.IsValid() ? FindConfigProblem(TypeName, Config) : FString();
	if (!ConfigProblem.IsEmpty())
	{
		OutError = FString::Printf(TEXT("the Config for %s has %s"), *TypeName, *ConfigProblem);
		return false;
	}
	const bool bAddsNamed = Plan.ContainsByPredicate([&TypeName](const FStarter* Starter) { return TypeName.Equals(Starter->TypeName, ESearchCase::CaseSensitive); });
	if (!Config.TrimStartAndEnd().IsEmpty() && !bAddsNamed)
	{
		OutError = FString::Printf(TEXT("The project already has the %s starter, so this Config would not be used; set Config in its definition's Game Kit category instead"), *TypeName);
		return false;
	}
	for (const FStarter* Starter : Plan)
	{
		const FString Directory = ServerDirectory / Starter->TypeName;
		if (IFileManager::Get().DirectoryExists(*Directory))
		{
			OutError = FString::Printf(TEXT("%s already exists, so the %s starter's server code would replace what it holds; move that folder first"), *Directory, Starter->TypeName);
			return false;
		}
		if (!FPaths::FileExists(GetCrateSource(*Starter) / TEXT("Cargo.toml")))
		{
			OutError = FString::Printf(TEXT("The Crowdy SDK's copy of the %s starter is missing from %s; reinstall the Crowdy SDK"), Starter->TypeName, *GetCrateSource(*Starter));
			return false;
		}
		if (FPackageName::DoesPackageExist(PackageOf(*Starter)))
		{
			OutError = FString::Printf(TEXT("%s already exists; move or delete it to add the %s starter"), *PackageOf(*Starter), Starter->TypeName);
			return false;
		}
	}
	const auto WritesSettings = [](const FStarter* Starter)
	{
		if (FCString::Strlen(Starter->Parent) == 0)
		{
			return true;
		}
		const UCrowdyServerObjectDefinition* Made = MakeDefinition(*Starter, GetTransientPackage(), NAME_None, RF_Transient);
		return Made && !Made->KitActors.IsEmpty();
	};
	const FString SettingsProblem = CrowdyExecDeploy::FindReadOnlySettingsProblem();
	if (!SettingsProblem.IsEmpty() && Plan.ContainsByPredicate(WritesSettings))
	{
		OutError = SettingsProblem;
		return false;
	}
	for (const FStarter* Starter : Plan)
	{
		UCrowdyServerObjectDefinition* Saved = nullptr;
		const FString Directory = ServerDirectory / Starter->TypeName;
		const FString OwnConfig = ConfigToAdd(*Starter, TypeName.Equals(Starter->TypeName, ESearchCase::CaseSensitive) ? Config : FString());
		if (!CopyCrate(*Starter, Directory, OutError) || !SaveDefinition(*Starter, OwnConfig, Saved, OutError))
		{
			// The folder did not exist before this call, so removing it leaves the project as it was for this starter.
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
			OutError = FString::Printf(TEXT("%s starter: %s"), Starter->TypeName, *OutError);
			return false;
		}
		OutAdded.Add(Starter->TypeName);
		if (FCString::Strlen(Starter->Parent) == 0)
		{
			UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
			Settings->GameKitWorld = FSoftObjectPath(Saved);
			Settings->TryUpdateDefaultConfigFile();
		}
		Saved->SyncGameKitActorStarters();
	}
	return true;
}

TArray<UCrowdyServerObjectDefinition*> CrowdyGameKitStarters::PlanRemove(const FString& TypeName, TConstArrayView<UCrowdyServerObjectDefinition*> Project, FString& OutProblem)
{
	UCrowdyServerObjectDefinition* const* Named = Project.FindByPredicate([&TypeName](const UCrowdyServerObjectDefinition* Definition)
	{
		return Definition && Definition->IsGameKitStarter() && Definition->TypeName.Equals(TypeName, ESearchCase::CaseSensitive);
	});
	if (!Named)
	{
		OutProblem = FString::Printf(TEXT("The project has no Game Kit starter named %s"), *TypeName);
		return {};
	}
	TArray<UCrowdyServerObjectDefinition*> Found = {*Named};
	TSet<FString> Gone = {TypeName};
	for (bool bGrew = true; bGrew;)
	{
		bGrew = false;
		for (UCrowdyServerObjectDefinition* Definition : Project)
		{
			if (!Definition || !Definition->IsGameKitStarter() || Gone.Contains(Definition->TypeName))
			{
				continue;
			}
			const FStarter* Starter = FindStarter(Definition->TypeName);
			const auto Needs = [&Gone, Starter](const FString& Called)
			{
				const bool bOptional = Starter && Starter->OptionalCalls.ContainsByPredicate([&Called](const TCHAR* Each) { return Called.Equals(Each, ESearchCase::CaseSensitive); });
				return !bOptional && Gone.Contains(Called);
			};
			if (Gone.Contains(Definition->KitParent) || Definition->KitCalls.ContainsByPredicate(Needs))
			{
				Found.Add(Definition);
				Gone.Add(Definition->TypeName);
				bGrew = true;
			}
		}
	}
	Algo::Reverse(Found);
	return Found;
}

bool CrowdyGameKitStarters::RemoveStarter(const FString& TypeName, TConstArrayView<UCrowdyServerObjectDefinition*> Project, const FString& ServerDirectory,
	TArray<FString>& OutRemoved, FString& OutError)
{
	OutRemoved.Reset();
	const TArray<UCrowdyServerObjectDefinition*> Plan = PlanRemove(TypeName, Project, OutError);
	if (!OutError.IsEmpty())
	{
		return false;
	}
	// Everything that could stop the removal is checked before anything changes.
	TSet<FName> PlanPackages;
	for (const UCrowdyServerObjectDefinition* Definition : Plan)
	{
		PlanPackages.Add(Definition->GetPackage()->GetFName());
	}
	TArray<UObject*> Assets;
	for (UCrowdyServerObjectDefinition* Definition : Plan)
	{
		const FString Directory = ServerDirectory / Definition->TypeName;
		if (IFileManager::Get().DirectoryExists(*Directory) && CrowdyServerCodeFiles::HoldsLink(Directory))
		{
			OutError = FString::Printf(TEXT("%s is, or holds, a link to somewhere else, and deleting it could delete what the link points to; nothing was removed"), *Directory);
			return false;
		}
		const FString PackageName = Definition->GetPackage()->GetName();
		if (!FPackageName::DoesPackageExist(PackageName))
		{
			continue;
		}
		const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		if (IFileManager::Get().IsReadOnly(*FileName))
		{
			OutError = FString::Printf(TEXT("%s is read-only (checked in?); make it writable, then remove again; nothing was removed"), *FileName);
			return false;
		}
		const TArray<FName> Referencers = CrowdyExecDeploy::FindReferencers(*Definition, PlanPackages);
		if (!Referencers.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s is used by %s; remove those uses first; nothing was removed"), *PackageName,
				*FString::JoinBy(Referencers, TEXT(", "), [](FName Referencer) { return Referencer.ToString(); }));
			return false;
		}
		Assets.Add(Definition);
	}

	// Read from the definitions before deleting them: a deleted object no longer has a path or a reliable Type Name.
	UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
	const bool bNamesKitWorld = Plan.ContainsByPredicate([Settings](const UCrowdyServerObjectDefinition* Definition) { return Settings->GameKitWorld == FSoftObjectPath(Definition); });
	TArray<FString> TypeNames;
	TArray<FSoftObjectPath> Paths;
	for (const UCrowdyServerObjectDefinition* Definition : Plan)
	{
		TypeNames.Add(Definition->TypeName);
		Paths.Add(FSoftObjectPath(Definition));
	}
	for (const FSoftObjectPath& Path : Paths)
	{
		const FString SettingsProblem = CrowdyExecDeploy::FindReadOnlySettingsProblemFor(Path);
		if (!SettingsProblem.IsEmpty())
		{
			OutError = SettingsProblem;
			return false;
		}
	}

	const FString Again = TEXT("; run -op=removestarter again to finish");
	if (ObjectTools::DeleteObjectsUnchecked(Assets) != Assets.Num())
	{
		OutError = FString::Printf(TEXT("not every definition under %s could be deleted%s"), ContentPath, *Again);
		return false;
	}
	const int32 ActorStarters = Settings->GameKitActorStarters.Num();
	Settings->GameKitActorStarters.RemoveAll([&Paths](const FSoftObjectPath& Path) { return Paths.Contains(Path); });
	if (bNamesKitWorld)
	{
		Settings->GameKitWorld.Reset();
	}
	if (bNamesKitWorld || ActorStarters != Settings->GameKitActorStarters.Num())
	{
		Settings->TryUpdateDefaultConfigFile();
	}
	for (const FString& Name : TypeNames)
	{
		const FString Directory = ServerDirectory / Name;
		if (IFileManager::Get().DirectoryExists(*Directory) && !IFileManager::Get().DeleteDirectory(*Directory, false, true))
		{
			OutError = FString::Printf(TEXT("could not delete %s (a file in it may be open)%s"), *Directory, *Again);
			return false;
		}
		OutRemoved.Add(Name);
	}
	return true;
}
