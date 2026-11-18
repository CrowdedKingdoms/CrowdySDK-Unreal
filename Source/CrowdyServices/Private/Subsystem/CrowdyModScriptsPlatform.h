#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include <atomic>
#include "Subsystem/CrowdyClaims.h"
#include "Subsystem/CrowdyModScripts.h"
#include "Subsystem/CrowdyVoxelWorld.h"

/** The platform calls Mod Scripts make. */
enum class ECrowdyModScriptsCall : uint8
{
	GridPermissions,
	ListMods,
	TrustAuthor,
	ConsentMod,
	RevokeConsent,
	RevokeTrust,
	FetchArtifact,
	UserStateGet,
	UserStateSet,
	GridChannels
};

struct UCrowdyModScripts::FPlatformCall
{
	ECrowdyModScriptsCall Call = ECrowdyModScriptsCall::ListMods;

	/** GridPermissions. */
	FInt64Vector Chunk = FInt64Vector(0, 0, 0);

	/** ListMods, TrustAuthor, RevokeTrust and GridChannels. */
	int64 GridId = 0;

	/** TrustAuthor and RevokeTrust. */
	int64 AuthorId = 0;

	/** ConsentMod, RevokeConsent and FetchArtifact. */
	FString ModId;

	/** The capability hash agreed to: the author's union for TrustAuthor, the script's own for ConsentMod. */
	FString Hash;

	/** UserStateSet: the base64 state, or none to clear it. */
	FString State;
	bool bClearState = false;
};

/** One spatial client event or channel message a Mod Script sends, under its derived sender id. */
struct UCrowdyModScripts::FRealtimeSend
{
	bool bChannel = false;
	int64 ChannelId = 0;
	FInt64Vector Chunk = FInt64Vector(0, 0, 0);
	uint8 Distance = 0;

	/** 32 lowercase hex characters, as octets. */
	TArray<uint8> Sender;

	/** A client event's payload starts with its event type, two octets little-endian. */
	TArray<uint8> Payload;
};

struct UCrowdyModScripts::FPlatformReply
{
	bool bOk = false;

	/** The platform's code, such as NOT_FOUND or RATE_LIMITED; empty when the request did not complete. */
	FString Code;

	/** The same error's reason, on the codes that carry one. */
	FString Reason;

	/** The platform's own words, which can also lead with a code ("RATE_LIMITED: ..."). */
	FString Message;

	/** GridPermissions: every grid covering the chunk. */
	TArray<FCrowdyGridPermissionRow> Grids;

	/** ListMods: the execGridClientMods rows. */
	TArray<TSharedPtr<FJsonValue>> Rows;

	/** UserStateGet and UserStateSet: the answer's data object as MessagePack, read on the script's worker. */
	TArray<uint8> Packed;

	/** GridChannels: the grid's channel ids. */
	TArray<int64> ChannelIds;

	/** FetchArtifact: the module, already checked against its digest, the CLIENT ABI and its summary. */
	TArray<uint8> Bytes;
	FString Digest;
	int64 FuelPerDispatch = 0;
	int32 TickIntervalMs = 0;

	/** FetchArtifact: the module failed that check. */
	bool bCheckRefused = false;
};

namespace CrowdyModScriptsHostCalls
{
	/** The largest request parsed, the arguments' limit plus room for the envelope; a larger one is refused unread. */
	inline constexpr int32 MaxRequestBytes = 257 * 1024;

	/** The largest arguments a call may carry, serialized. */
	inline constexpr int32 MaxArgsBytes = 256 * 1024;

	/** The largest answer a script receives, as the browser's reply buffer. */
	inline constexpr int32 MaxReplyBytes = 1024 * 1024;

	/** More host calls than this in one second, of any kind, open the script's circuit. */
	inline constexpr int32 MaxCallsPerSecond = 1000;

	inline constexpr int32 MaxHudChars = 500;
	inline constexpr int32 MaxHudLines = 8;
	inline constexpr int32 MaxOverlayShapes = 64;

	/** An event whose cascade depth passes this reaches nobody, as on the platform's buses. */
	inline constexpr int32 MaxEventCascade = 8;

	/** emit_spatial reaches at most this many chunks out. */
	inline constexpr int32 MaxSpatialDistance = 8;

	/** A channel message's payload limit, as the wire's. */
	inline constexpr int32 MaxChannelPayloadBytes = 1024;
	inline constexpr int32 NumGroups = static_cast<int32>(ECrowdyModScriptGroup::Input) + 1;

	/** Admits a call at NowSeconds into a ring of at most Cap times, or refuses it when Cap fell within the second before. */
	bool AdmitWindow(TArray<double>& Times, int32& Oldest, double NowSeconds, int32 Cap);

	/** The calls admitted in the last second, as a ring of at most Cap times. */
	struct FWindow
	{
		TArray<double> Times;
		int32 Oldest = 0;

		/** Admits a call at NowSeconds, or refuses it when Cap calls already fell within the second before. */
		bool Admit(double NowSeconds, int32 Cap);
	};

	/** One run's call windows: every call, then one per ECrowdyModScriptGroup. */
	struct FBudget
	{
		FWindow All;
		FWindow Groups[NumGroups];

		/** The breaker opened: every later call of the run is refused. */
		bool bTripped = false;
	};

	/** A call that passed every check, with its arguments. */
	struct FChecked
	{
		FString Name;
		TSharedPtr<FJsonObject> Args;
	};

	/** The calls the game thread answers. */
	enum class ECall : uint8
	{
		None,
		Hud,
		Overlay,
		Clicks,
		ChunkGet,
		VoxelsList,
		Actors,
		VoxelSet,
		Event,

		/** Answered through the platform. */
		UserStateGet,
		UserStateSet,
		Spatial,
		Channel,

		/** Answered on the worker alone. */
		GridInfo,
		Clock,
		Permission
	};

	/** A call the worker checked and read, for the game thread. */
	struct FPrepared
	{
		ECall Call = ECall::None;
		FString HudText;
		TArray<FCrowdyModScriptShape> Shapes;

		/** The chunk calls' chunk. */
		FInt64Vector Chunk = FInt64Vector::ZeroValue;

		/** The actors calls' chunks, clipped to the grid. */
		FCrowdyModScriptGrid Area;
		FCrowdyVoxelWrite Write;

		/** emit_event: the event's JSON up to its cascade depth, which the game thread adds; and whom it is for. */
		TArray<uint8> EventPrefix;
		FString EventTarget;
		bool bEventTargeted = false;

		/** user_state_set: the state, or none to clear it. */
		FString State;
		bool bClearState = false;

		/** emit_spatial and emit_channel. */
		UCrowdyModScripts::FRealtimeSend Send;
	};

	/** What the game thread read for a world or input call, for the worker to write as JSON. */
	struct FWorldAnswer
	{
		ECall Call = ECall::None;

		/** The chunk calls: null when the chunk is not loaded. */
		TSharedPtr<const FCrowdyModScriptChunk, ESPMode::ThreadSafe> Chunk;
		FCrowdyModScriptClicks Clicks;
		int64 NowMs = 0;
		TArray<FCrowdyModScriptPlayer> Players;
		FString VoxelError;
		bool bVoxelOk = false;

		/** The user state calls: the platform's answer as MessagePack. */
		TArray<uint8> Packed;
	};

	/** The game thread's whole answer when it left a FWorldAnswer on the gate for the worker to write. */
	inline constexpr uint8 WorldAnswerMarker = 0x01;

	/** One run's checks, made on its worker so the game thread never parses or writes a script's JSON. */
	struct FGate
	{
		TArray<FString> Agreed;
		FCrowdyModScriptGrid Box;
		FBudget Budget;

		/** The script's own name, its events' source. */
		FString ModuleName;

		/** The signed-in player, and their own keys on the grid under Lock, refreshed with each lookup, for grid_permission_check. */
		int64 PlayerId = 0;
		TArray<FString> Keys;

		/** Set on the worker when the run's breaker opens; the game thread then stops the run. */
		std::atomic<bool> bTripped{false};

		/** One call is in flight per run; these hand it across under Lock. */
		FCriticalSection Lock;
		FPrepared Prepared;
		FWorldAnswer Answer;
	};

	/**
	 * Worker thread: checks Request and reads its arguments. False replaces it with the answer to give at once, a refusal or
	 * an answer the SDK alone gives; true replaces it with the call's name, leaving what it read on Gate.
	 */
	bool Prepare(FGate& Gate, TArray<uint8>& InOutRequest, double NowSeconds);

	/** Worker thread: writes the world answer the game thread left on Gate; any other answer passes unchanged. */
	void Finish(FGate& Gate, TArray<uint8>& InOutAnswer);

	/** The JSON answer to a world or input call, refused when over 256 KiB. */
	TArray<uint8> WorldAnswer(const FWorldAnswer& Answer);

	/** Whether this build answers the host call; one the game answers is served only with a host set. */
	bool IsServed(const FString& Name, bool bHostSet);

	/**
	 * Checks one request, {"fn":...,"args":...} in UTF-8, against the calls the player agreed to and the run's budget.
	 * True fills Out; false fills OutDenied with the refusal, and sets Budget.bTripped when the call opened the breaker.
	 */
	bool Check(TConstArrayView<uint8> RequestUtf8, const TArray<FString>& Agreed, bool bHostSet, FBudget& Budget, double NowSeconds,
		FChecked& Out, TArray<uint8>& OutDenied);

	/**
	 * The sender id a script's sends carry on GridId for the actor it names: the first 16 bytes of SHA-256 over both, as 32
	 * lowercase hex. Name is octets, each read as one character, as the browser decodes a uuidHex. Empty without the runtime.
	 */
	FString ClientHalfActorUuid(int64 GridId, TConstArrayView<uint8> Name);

	/** The platform's refusal of kind denied. */
	TArray<uint8> Denied(const FString& Message);

	/** A presentation call's answer. */
	TArray<uint8> Delivered(bool bDelivered);

	/** hud_set's text: the payload's string text, cut to 8 lines and 500 characters; empty removes the card. */
	FString HudText(const FJsonObject& Args);

	/** overlay_draw's shapes from construct.scene.v1 rows, placed inside Box in voxel units; empty clears them. */
	void OverlayShapes(const FJsonObject& Args, const FCrowdyModScriptGrid& Box, TArray<FCrowdyModScriptShape>& Out);
}
