// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Small shared JSON field helpers for the Studio GraphQL builders. These stay module-local because they are used across
// the whole Studio mutation surface (grids, tiers, sessions, schema sync, and more). They live here, as inline functions
// in a named namespace, so there is exactly one definition and every Studio caller shares it rather than re-declaring
// them in an anonymous namespace (which a unity build would merge into one translation unit and redefine).
//
// The Game Model function-upsert marshaller that once lived here now has a single exported home in CrowdyReplication
// (CrowdyGameModelMarshalling::BuildFunctionUpsertInput), so it is also reachable from the editor lowered-payload
// preview. Include "Replication/GameModel/Effect/CrowdyGameModelFunctionMarshaller.h" for it.
namespace CrowdyStudioMarshalling
{
	// BigInt travels as a decimal string on the wire, never a JSON number.
	inline void SetBigIntField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64 Value)
	{
		Object->SetStringField(Field, FString::Printf(TEXT("%lld"), Value));
	}

	// Optional string inputs are dropped when empty so an upsert leaves the server's existing value alone instead of
	// blanking it.
	inline void SetOptionalStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Object->SetStringField(Field, Value);
		}
	}
}
