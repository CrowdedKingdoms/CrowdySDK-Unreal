// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UCrowdyEffect;

/**
 * The sync state of one authored effect asset against the live Game Model schema on the server.
 */
enum class ECrowdyEffectSyncStatus : uint8
{
	// Not determined yet, could not be determined (not signed in, no app selected, a read failed), or the effect
	// itself cannot be synced (it does not compile / is unmigrated). The message carried alongside says which.
	Unknown,

	// The server's function + container type already match the compiled effect: nothing to sync.
	Synced,

	// The effect exists on the server but differs from code: a sync would update it.
	Drifted,

	// The effect's function has never been synced: it does not exist on the server yet.
	NotOnServer,
};

/**
 * A narrow, editor-only facade for syncing a SINGLE effect asset's compiled server function and its one target
 * container type to the live Game Model schema, and for reading that effect's drift status. It reuses the shared
 * schema-sync diff cores (CrowdySchemaSync) and the module's GraphQL transport (mint an app-scoped token from the
 * signed-in session token, then post to the app's Game API endpoint), exactly like CrowdyStudioAuth::FetchAppChannelNames.
 *
 * A per-asset sync NEVER prunes: it only ever creates or updates that one effect's function + container type, leaving
 * all other server schema untouched.
 *
 * Every async callback fires on the game thread, exactly once. The app to sync against is the project's configured
 * app id (UCrowdySDKDeveloperSettings::AppID), the same app the runtime uses.
 */
namespace CrowdyStudioSyncService
{
	// Compute the effect's current drift status against the server. Async: compiles the effect, reads the relevant
	// server schema, and classifies. OnStatus receives the status and a short human-readable message. The result is
	// cached (see GetCachedStatus), so the pre-play drift check can consult it without a round-trip. Concurrent
	// requests for the same effect coalesce into one in-flight read.
	CROWDYSTUDIO_API void RequestEffectSyncStatus(
		UCrowdyEffect* Effect, TFunction<void(ECrowdyEffectSyncStatus /*Status*/, const FString& /*Message*/)> OnStatus);

	// Sync ONLY this effect's function + its target container type (its attributes) to the server, via idempotent
	// gameModelUpsert* ops. Never prunes. Async: OnDone receives whether every planned change applied and a message
	// (mirrors the console's "Applied N of M" partial-apply reporting on a mid-sequence failure). Updates the cache.
	CROWDYSTUDIO_API void SyncEffect(
		UCrowdyEffect* Effect, TFunction<void(bool /*bOk*/, const FString& /*Message*/)> OnDone);

	// The last computed status for an effect, or Unknown when it has never been computed (or was invalidated by an
	// edit). Pure and synchronous: no server round-trip. This is what the pre-play drift check consults.
	CROWDYSTUDIO_API ECrowdyEffectSyncStatus GetCachedStatus(const UCrowdyEffect* Effect);

	// Every effect whose cached status is Drifted or NotOnServer, for the pre-play check. Stale (garbage-collected)
	// entries are skipped. No server round-trip.
	CROWDYSTUDIO_API TArray<TWeakObjectPtr<UCrowdyEffect>> GetCachedOutOfSyncEffects();

	// Forget an effect's cached status (resets it to Unknown), so a stale "Synced" cannot linger after an edit. Called
	// when the effect changes.
	CROWDYSTUDIO_API void InvalidateCachedStatus(const UCrowdyEffect* Effect);

	// Forget EVERY cached status. A status is a verdict about the server schema, so any write to that schema from
	// somewhere other than a per-effect sync (the console's Apply, its prune, a staged delete) makes every verdict
	// computed before it stale. Nothing else re-reads a cached Drifted, so without this one stale verdict outlives the
	// drift forever and keeps re-raising the pre-play prompt.
	CROWDYSTUDIO_API void InvalidateAllCachedStatuses();

	// A status read is read-only: it never provisions the app's session channel, but a sync does when an effect's
	// model-changed notification needs one and the app has none yet. So an effect already on the server still has real
	// sync work while its session channel is missing, and should read as drifted (a sync will create the channel) rather
	// than synced. This governs only the empty-delta case; a non-empty delta is already drift by the normal path. Pure.
	inline bool ShouldReportDriftForPendingChannel(bool bDeltaEmpty, bool bAnyFunctionNeedsChannel, int64 SessionChannelId)
	{
		return bDeltaEmpty && bAnyFunctionNeedsChannel && SessionChannelId == 0;
	}

	// A status read's computed result must not overwrite the cache when the effect's status epoch changed while the read
	// was in flight: a sync or an edit landed a fresher value, and this read reflects the now-stale pre-change snapshot. Pure.
	inline bool ShouldCacheStatusResult(uint64 StartEpoch, uint64 CurrentEpoch)
	{
		return StartEpoch == CurrentEpoch;
	}
}
