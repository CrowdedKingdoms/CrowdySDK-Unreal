// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyCppReplication.h"

namespace CrowdyTokenAuthorization
{
	/**
	 * Whether a rotated app token lets the connection keep the server it is on, instead of re-assigning.
	 *
	 * A replication server silently drops datagrams signed with a token it was never told about, so this is false
	 * unless the Game API said it installed THIS token on THIS server. Three different situations collapse into
	 * that one answer, and none of them is an error: a rotation that named no server, an answer carrying no
	 * authorized server because the node can no longer serve the app, and an answer naming a different node.
	 *
	 * A wrong yes is the expensive direction. It leaves a client signing with a token no server knows, sending
	 * into silence with nothing reporting a failure, which is why the empty answer is checked rather than assumed
	 * away by the comparison.
	 */
	inline bool KeepsCurrentServer(const FString& AuthorizedIp4, const int32 AuthorizedClientPort,
		const FCrowdyCppCurrentServer* Current)
	{
		if (!Current || AuthorizedIp4.IsEmpty() || AuthorizedClientPort <= 0)
		{
			return false;
		}

		// Case-sensitive deliberately. FString comparison folds case by default, which says nothing either way
		// about a dotted quad but would quietly widen this if the server ever names a node any other way.
		return AuthorizedClientPort == Current->ClientPort
			&& AuthorizedIp4.Equals(Current->Ip4, ESearchCase::CaseSensitive);
	}
}
