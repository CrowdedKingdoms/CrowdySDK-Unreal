// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Base64.h" // the base64 form a channel payload may arrive in
#include "UObject/UnrealType.h" // FProperty, FField::HasMetaData

/**
 * UPROPERTY/UCLASS meta-keys that bind an Unreal type to a Game Model container Plane B, the
 * server-authoritative TRUTH plane. Deliberately distinct from CrowdyStateMetaKeys (the fast view plane):
 * a property lives in exactly one plane, so a variable is either CrowdyState or CrowdyModel, never both
 * (the unified "Crowdy Replication" dropdown enforces this; C++ that hand-writes both is rejected at discovery).
 * The OnRep notify reuses CrowdyStateMetaKeys::OnRep ("CrowdyOnRep"), one parameterless OnRep convention across
 * both planes, fired by the same ProcessEvent(nullptr) mechanism.
 *
 * Model and Container are read from live UPROPERTY metadata in the editor. Cooked builds strip that metadata, so
 * a packaged runtime reads a baked attribute table instead (UCrowdyBakedRegistry).
 */
namespace CrowdyGameModelMetaKeys
{
	// UPROPERTY marker: a server-authoritative Game Model attribute ("Server Owned" in the BP dropdown).
	inline const TCHAR* Model = TEXT("CrowdyModel");

	// UCLASS marker: the Game Model container type name this class maps to.
	inline const TCHAR* Container = TEXT("CrowdyContainer");

	// Asset-registry tag written on every Blueprint save, naming the format the asset's Crowdy tags were written
	// under. Its ABSENCE is the whole point: an asset carrying the current value has been described, so the
	// container tag below can be trusted to be there or genuinely not there, and the asset never has to be loaded
	// to find out. An asset without it has never been described and is loaded, exactly as every asset used to be.
	// Bump the value whenever the meaning of the container tag changes; every asset tagged under the older value
	// then falls back to being loaded until it is saved again.
	inline const TCHAR* ScanAssetTag = TEXT("CrowdyScan");
	inline const TCHAR* ScanAssetTagValue = TEXT("1");

	// Asset-registry tag naming the Game Model container type a Blueprint's compiled class declares. Written only
	// alongside ScanAssetTag, so on an asset carrying the current scan value its absence means "definitively not a
	// container" rather than "unknown". Never written empty: a container's type name always resolves to something
	// (a signals-only container with no attributes at all still has one), and the registry's fixed-size tag store
	// rejects a zero-length value outright.
	inline const TCHAR* ContainerTypeAssetTag = TEXT("CrowdyContainerType");

	// UCLASS override: whether this container fetches its server state once as soon as it binds. Absent means it
	// does, which is the default and keeps the tag off every container that never opts out; the only value that
	// changes behaviour is "False". Read through UCrowdyBakedRegistry so a cooked build, where this metadata is
	// stripped, answers from the baked table instead.
	inline const TCHAR* PullOnStart = TEXT("CrowdyPullOnStart");

	// UCLASS key-only marker: a test-only container fixture. It is reflected like any UCLASS (its CDO exists in
	// an editor build), so it would otherwise be gathered as a real container type and upserted to a live app on
	// a Studio "Sync Schema from Code". The schema sync's class gather skips a class carrying this (via
	// FCrowdyAttributeRegistry::IsTestContainer); it usually sits alongside the CrowdyContainer tag on a fixture,
	// since the fixture is a valid-looking container that simply must never reach the server.
	inline const TCHAR* ContainerTest = TEXT("CrowdyContainerTest");

	// UCLASS key-only marker: an SDK test fixture class, filtered out of the heartbeat advisory and the registry baker's sweep.
	inline const TCHAR* TestFixture = TEXT("CrowdyTestFixture");

	// UPROPERTY override: the server property key for this attribute. Absent, the key is the lowercased
	// property name (ServerKeyForProperty). Present, it pins a stable server key independent of a later C++/BP
	// rename, and lets an intentional same-key collision be authored (which the discovery duplicate-key guard
	// then rejects). DiscoverForClass lowercases/trims the override so it compares against derived keys.
	inline const TCHAR* Key = TEXT("CrowdyKey");

	// UPROPERTY: the server READ-visibility of this attribute, one of "public" | "owner" | "hidden"
	// (UpsertPropertyDefInput.visibility). Absent or unrecognized falls back to "public" (the type default).
	// public = anyone who can read the container; owner = only the container's owner; hidden = server/admin
	// only. Consumed by the schema sync's property def; the runtime cache/OnRep does not read it.
	inline const TCHAR* Visibility = TEXT("CrowdyVisibility");

	// Reserved key name, retained so no authored property key ever collides with it. The runtime no longer writes
	// or reads it: a container is now identified by its server bindingKey (the NetID digest passed to
	// gameModelEnsureContainer), not by a NetID digest stamped into metadataJson. Kept as a reserved name only;
	// deliberately double-underscore-prefixed so it can never be an authored, designer-visible property key.
	inline const TCHAR* ReservedNetIdMetaKey = TEXT("__crowdy_netid");

	// The server property key for a Model property is its lowercased name unless a meta=(CrowdyKey=...) override
	// is present (applied by DiscoverForClass). Centralized here so the receive/apply path and the schema sync
	// agree on the derived form.
	inline FString ServerKeyForProperty(const FProperty* Property)
	{
		return Property ? Property->GetName().ToLower() : FString();
	}

#if WITH_METADATA
	/**
	 * The server property key an attribute is actually addressed by: its lowercased name, unless a
	 * meta=(CrowdyKey=...) override pins a different one. A blank override is ignored so a stray tag never
	 * yields an empty key; bOutBlankOverride reports that case for the caller that wants to warn about it.
	 *
	 * Everything that names an attribute to the server resolves it through here, and so does the receive path
	 * that matches an incoming key back to a member. They have to agree: if one honours the override and the
	 * other does not, the server writes a value under a key the client never recognizes and the attribute
	 * silently never applies.
	 */
	inline FString ResolvedServerKeyForProperty(const FProperty* Property, bool* bOutBlankOverride = nullptr)
	{
		if (bOutBlankOverride)
		{
			*bOutBlankOverride = false;
		}
		if (Property && Property->HasMetaData(Key))
		{
			const FString Override = Property->GetMetaData(Key).TrimStartAndEnd().ToLower();
			if (!Override.IsEmpty())
			{
				return Override;
			}
			if (bOutBlankOverride)
			{
				*bOutBlankOverride = true;
			}
		}
		return ServerKeyForProperty(Property);
	}
#endif

	// The default server read-visibility when a property declares no (valid) CrowdyVisibility.
	inline const TCHAR* DefaultVisibility = TEXT("public");

	// True for an accepted server read-visibility (UpsertPropertyDefInput.visibility). Anything else (absent, a
	// typo) is treated as DefaultVisibility by discovery, with a warning.
	inline bool IsValidVisibility(const FString& Value)
	{
		return Value == TEXT("public") || Value == TEXT("owner") || Value == TEXT("hidden");
	}

	// The reserved uint16 event_type that a Game Model function's spatial model-driven notification stamps,
	// so the SERVER_EVENT_NOTIFICATION (opcode 139) receive path recognizes "model changed" and ignores
	// unrelated server events. Deliberately high to avoid colliding with a game's own small EEventType values.
	// A hand-authored function's notification MUST emit this exact value in its event_type arg for the client
	// to recognize it. The schema sync stamps this value automatically when it authors the notification.
	inline constexpr uint16 ModelChangedEventType = 60000;

	// The ASCII prefix a function's CHANNEL model-driven notification stamps at the front of its payload, so the
	// CHANNEL_MESSAGE (opcode 18) receive path recognizes "model changed" and ignores unrelated channel traffic
	// (chat, reliable-RPC frames). The bytes after the prefix are the changed container's id. The authored
	// notification emits concat("<this>", $self_container_id); the client checks the prefix and decodes the id.
	// Channels reach every member regardless of position, so this is the position-independent carrier (the spatial
	// 139 is proximity-scoped and needs chunk coords a data container has not).
	inline const TCHAR* ModelChangedChannelPrefix = TEXT("cmc:");

	// The ASCII prefix a SIGNAL's channel notification stamps at the front of its payload. A signal is a function
	// that changes no state and exists only to tell clients something happened, so it deliberately does NOT reuse
	// the model-changed prefix: a signal must not trigger a re-pull.
	//
	// Layout after the prefix is "<name>:<container id>". The name travels on the wire rather than a numeric id
	// because the client derives its handler from it directly, which is what keeps signals working in a cooked
	// build: UFUNCTION reflection survives cooking, but the metadata that would otherwise map an id to a handler
	// does not.
	//
	// Three constraints this prefix has to satisfy, all of them load-bearing:
	//   - it is not a prefix of ModelChangedChannelPrefix and that is not a prefix of this, so the two decoders
	//     can never claim each other's frames;
	//   - it contains ':' which is outside the base64 alphabet, so the raw and base64 decode paths stay
	//     unambiguous exactly as they are for the model-changed prefix;
	//   - UCrowdyChannels skips it alongside cmc:, so a signal frame is never fed to the reliable-RPC decoder.
	inline const TCHAR* SignalChannelPrefix = TEXT("csg:");

	/**
	 * The ASCII forms of a channel payload (opcode 18) that the receive path will inspect, in order: the raw bytes
	 * read as ASCII, and, when those bytes are valid base64, their decoding. The server sends both shapes, and they
	 * stay unambiguous because the base64 alphabet excludes ':', so a raw "cmc:<id>" or "csg:<name>:<id>" can never
	 * spuriously decode into the other form.
	 *
	 * Single-sourced because every reader of this opcode has to accept the same set. UCrowdyChannels skips these
	 * frames so they are never fed to the reliable-RPC decoder, and the Game Model subsystem decodes them. If the
	 * skip recognized fewer encodings than the decode did, a frame would be both handled and mis-decoded, which is
	 * the exact failure the skip exists to prevent.
	 */
	inline void GameModelChannelPayloadForms(const TArray<uint8>& Payload, TArray<FString>& OutForms)
	{
		OutForms.Reset();
		if (Payload.Num() == 0)
		{
			return;
		}

		// Interpret raw bytes as ASCII, stopping at an embedded null.
		auto BytesToAscii = [](const TArray<uint8>& Bytes) -> FString
		{
			FString Text;
			Text.Reserve(Bytes.Num());
			for (const uint8 Byte : Bytes)
			{
				if (Byte == 0)
				{
					break;
				}
				Text.AppendChar(static_cast<TCHAR>(Byte));
			}
			return Text;
		};

		FString Raw = BytesToAscii(Payload);
		TArray<uint8> Decoded;
		const bool bIsBase64 = FBase64::Decode(Raw, Decoded);
		OutForms.Add(MoveTemp(Raw));
		if (bIsBase64)
		{
			OutForms.Add(BytesToAscii(Decoded));
		}
	}

	// True when a channel payload carries one of the Game Model prefixes in any accepted encoding. A recognition
	// test only: it reports that the frame belongs to the Game Model plane, not that its body is well formed.
	inline bool HasGameModelChannelPrefix(const TArray<uint8>& Payload)
	{
		TArray<FString> Forms;
		GameModelChannelPayloadForms(Payload, Forms);
		for (const FString& Form : Forms)
		{
			if (Form.StartsWith(ModelChangedChannelPrefix, ESearchCase::CaseSensitive)
				|| Form.StartsWith(SignalChannelPrefix, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	// The prefix of the parameterless handler a signal calls on its bound container: signal "BossWave" calls
	// "OnSignal_BossWave". Resolved with FindFunction at dispatch time, so nothing needs baking.
	inline const TCHAR* SignalHandlerPrefix = TEXT("OnSignal_");

	// The server-injected system param naming the acting container's own UUID. Available in function-body, effect,
	// and notification-args expressions from Game API v0.20.0, injected per invocation (a player invoke or an
	// automation run alike) and un-spoofable by a same-named caller param. A model-driven notification names the
	// changed container with concat(ModelChangedChannelPrefix, $self_container_id), so nothing needs to pass the id
	// in. Referenced as $self_container_id in an expression; the bare key is reserved so a magnitude cannot shadow it.
	inline const TCHAR* SelfContainerIdParam = TEXT("self_container_id");

	// The server-injected system param naming the app's default session channel, resolved per invocation against the
	// app the function is running in. A channel notification addressed by name therefore follows the app that holds
	// the definition, where a literal channel id copied into another app addresses a channel nobody there is a
	// member of and is dropped for want of a recipient. Referenced as $session_channel_name in an expression.
	inline const TCHAR* SessionChannelNameParam = TEXT("session_channel_name");

	// Legacy: the function parameter earlier SDK versions used to carry the changed container's id into a
	// model-driven notification, before the server injected $self_container_id. The runtime no longer authors or
	// fills it; it is retained only as a reserved name so a hand-authored effect body referencing $notify_id is not
	// misclassified as a missing magnitude, and no authored magnitude can claim the name. Receive is unaffected: the
	// client decodes the ModelChangedChannelPrefix off the wire, never the param name.
	inline const TCHAR* NotifyIdParam = TEXT("notify_id");

	// The reserved server property key the SDK provisions on every container type so a Model Collection change
	// (an edge added or removed - a graph mutation, not a property write) still produces an observable, notifiable
	// change on the collection's owning container. The crowdy_touch function bumps it; peers watching the container
	// re-pull, observe the bumped value, and re-read the collection. It is NOT server-hidden: it must be returned
	// on a pull so the change is observable (a hidden property would make the re-pull a no-op). A designer must not
	// declare a Server Owned attribute that resolves to this key; the schema sync surfaces a collision as an error.
	inline const TCHAR* CollectionRevKey = TEXT("crowdy_rev");

	// The default edge relationship a collection uses when the caller names none ("an inventory contains items").
	inline const TCHAR* DefaultCollectionRelationship = TEXT("contains");

	// The reserved prefix of the per-type function the SDK provisions to bump CollectionRevKey and emit the
	// model-changed notification after a collection edge changes. There is one function per container type (a
	// function's upsert key is (app, name), so the name must be per-type), bound to that type so its self.crowdy_rev
	// read is statically valid. Double-underscore-prefixed so it never collides with a designer's function name.
	inline const TCHAR* CollectionTouchFunctionPrefix = TEXT("__crowdy_touch_");

	// The reserved touch-function name for a container type. The type is lowercased so the runtime (handed the
	// parent's type name by the caller) and the schema sync (reading the CrowdyContainer tag) resolve the same
	// name regardless of the caller's casing, matching how server property keys are derived (ServerKeyForProperty).
	inline FString CollectionTouchFunctionName(const FString& ContainerTypeName)
	{
		return FString(CollectionTouchFunctionPrefix) + ContainerTypeName.ToLower();
	}

	// True when the given server key is a reserved collection key a designer's Server Owned attribute must not claim.
	inline bool IsReservedCollectionKey(const FString& InKey)
	{
		return InKey == CollectionRevKey;
	}

	// True when a function name is one the SDK reserves for a collection touch function.
	inline bool IsReservedCollectionFunctionName(const FString& Name)
	{
		return Name.StartsWith(CollectionTouchFunctionPrefix);
	}

#if WITH_METADATA
	inline bool HasModelMeta(const FProperty* Property)
	{
		return Property && Property->HasMetaData(Model);
	}
#else
	inline bool HasModelMeta(const FProperty* /*Property*/)
	{
		return false;
	}
#endif
}
