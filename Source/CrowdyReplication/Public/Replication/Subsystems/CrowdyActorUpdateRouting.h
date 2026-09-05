// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Which way an inbound actor update goes, and what happens to one that was queued before its actor appeared.
 *
 * There are two routes and an actor may only ever be on one of them at a time. An actor already on screen is
 * answered where its update is received; an actor nobody has seen yet is queued, because deciding to spawn it
 * needs work that does not belong on the receive path. The two rules below are what keeps those routes from
 * overlapping, and they are stated here rather than inline so each can be exercised on its own.
 */
namespace CrowdyActorUpdateRouting
{
	/** What to do with an update as it arrives. */
	enum class EReceiveRoute : uint8
	{
		/** The actor is on screen. Gather it here; it needs no spawn decision and no other thread. */
		Gather,

		/** Nobody has seen this actor. Queue it so a consumer can decide whether to spawn it. */
		QueueForSpawnDecision
	};

	/** What to do with an update a consumer has just taken off a queue. */
	enum class EQueuedVerdict : uint8
	{
		/** Spawn the actor this update describes. */
		Spawn,

		/** Its spawn is already under way, so this adds nothing. */
		DropAlreadySpawning,

		/**
		 * The actor became tracked while this sat in the queue.
		 *
		 * Dropped rather than delivered, because it can no longer be ORDERED. From the moment the actor was
		 * tracked its updates go straight from where they are received to the drain, and this one has been
		 * waiting behind a queue and a batch window, so delivering it now can put an older position after a
		 * newer one and nothing downstream compares timestamps to catch that.
		 *
		 * The cost is real and bounded: if nothing newer has arrived yet, the actor holds the position its
		 * spawn carried for up to one more send interval. That is one dropped view update on a transport that
		 * already drops, against a visible snap backwards, which is why this is the safe direction rather than
		 * the free one.
		 */
		DropStale
	};

	inline EReceiveRoute RouteOnReceive(const bool bTracked)
	{
		return bTracked ? EReceiveRoute::Gather : EReceiveRoute::QueueForSpawnDecision;
	}

	/**
	 * Tracked is asked before pending, because an actor that finished spawning while this update waited is
	 * briefly both, and the stale answer is the one that matters.
	 */
	inline EQueuedVerdict JudgeQueuedUpdate(const bool bTracked, const bool bSpawnPending)
	{
		if (bTracked)
		{
			return EQueuedVerdict::DropStale;
		}

		return bSpawnPending ? EQueuedVerdict::DropAlreadySpawning : EQueuedVerdict::Spawn;
	}
}
