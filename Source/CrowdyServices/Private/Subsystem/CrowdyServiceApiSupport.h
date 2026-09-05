#pragma once

#include "CoreMinimal.h"
#include "CrowdyCppClient.h"
#include "CrowdyServicesLog.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Misc/Base64.h"
#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

/**
 * Shared plumbing for the services that issue their own calls against the API client: teams, channels, avatars, the
 * host checks and the persistence pull. They model different things and share no Blueprint-facing type, but the
 * calls are the same shape, so resolving a client, reading a result and classifying a failure are one job done once.
 *
 * These live in one header rather than in each service's own translation unit on purpose: this module's unity build
 * can merge two .cpp files into a single translation unit, and a helper defined file-locally in both would then
 * redefine itself.
 *
 * The templates are written against whatever error type the calling service uses. Those error types all declare the
 * same code enumerators, so the code for a given failure is chosen once here rather than at every call site.
 */
namespace CrowdyServiceApi
{
	/**
	 * How the running game addresses the API, read from the developer settings.
	 *
	 * The two values are not interchangeable and the fallback is the interesting part. GameApiHttpUrl names the one
	 * instance in the datacenter the app is served from, which is where its shards are and therefore the only place
	 * a gameplay call is answered rather than refused. It is empty until an app has been resolved, and the shared
	 * origin is the right stand-in for that gap: every datacenter answers it, so a cold client can get far enough to
	 * ask where it should actually be. Leaving it empty instead would build a client with no endpoint at all, which
	 * the merged-API era papered over by falling back to the management URL and this one cannot.
	 */
	inline FCrowdyCppClientConfig ResolveClientConfig()
	{
		FCrowdyCppClientConfig Config;
		if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
		{
			Config.DiscoveryUrl = Settings->GetDiscoveryUrl();
			Config.ApiUrl = Settings->GetGameApiHttpUrl();
			if (Config.ApiUrl.IsEmpty())
			{
				Config.ApiUrl = Config.DiscoveryUrl;
			}
		}
		return Config;
	}

	/** An error that never reached the server, so no server verdict exists to report. */
	template <typename ErrorType>
	ErrorType MakeNetworkError(const FString& Message)
	{
		ErrorType Error;
		Error.Code = decltype(Error.Code)::NetworkError;
		Error.Message = Message;
		return Error;
	}

	/** The error reported when the call never reached the network because no client could be resolved. */
	template <typename ErrorType>
	ErrorType ClientUnavailableError()
	{
		return MakeNetworkError<ErrorType>(TEXT("The Crowdy API client is unavailable, so the request was not sent."));
	}

	/**
	 * Classify a failed call. A cancellation is deliberately not run through FromMessage: its wording carries no
	 * server verdict, and the keyword match would file it as a server error, which is not what happened.
	 */
	template <typename ErrorType>
	ErrorType DescribeFailure(const FCrowdyCppJsonResult& Result)
	{
		if (Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage())
		{
			return MakeNetworkError<ErrorType>(TEXT("The request was interrupted before it completed."));
		}
		return ErrorType::FromMessage(Result.ErrorMessage);
	}

	/** The error reported when the call succeeded but its payload was not the shape this operation returns. */
	template <typename ErrorType>
	ErrorType UnreadableResponseError()
	{
		ErrorType Error;
		Error.Code = decltype(Error.Code)::ServerError;
		Error.Message = TEXT("The server's answer could not be read.");
		return Error;
	}

	/**
	 * The response payload of a successful call, or null with OutError filled.
	 *
	 * FCrowdyCppJsonResult::Data is the GraphQL response's `data` object itself, so a field of it is a field of this
	 * operation's own answer.
	 */
	template <typename ErrorType>
	const TSharedPtr<FJsonObject>* ReadPayload(const FCrowdyCppJsonResult& Result, ErrorType& OutError)
	{
		if (!Result.bTransportOk)
		{
			OutError = DescribeFailure<ErrorType>(Result);
			return nullptr;
		}

		if (!Result.Data.IsValid())
		{
			OutError = UnreadableResponseError<ErrorType>();
			return nullptr;
		}

		return &Result.Data;
	}

	/** Read the operation's object-valued answer into Out. */
	template <typename ValueType, typename ErrorType>
	bool ReadObject(const FCrowdyCppJsonResult& Result, const TCHAR* FieldName, ValueType& Out, ErrorType& OutError)
	{
		const TSharedPtr<FJsonObject>* Payload = ReadPayload(Result, OutError);
		if (!Payload)
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Field = nullptr;
		if (!(*Payload)->TryGetObjectField(FieldName, Field) || !ValueType::ParseFromJson(*Field, Out))
		{
			OutError = UnreadableResponseError<ErrorType>();
			return false;
		}

		return true;
	}

	/**
	 * Read the operation's list-valued answer into Out. An element that will not parse is skipped rather than
	 * failing the whole read, which keeps one malformed row from hiding every good one.
	 */
	template <typename ValueType, typename ErrorType>
	bool ReadArray(const FCrowdyCppJsonResult& Result, const TCHAR* FieldName, TArray<ValueType>& Out,
		ErrorType& OutError)
	{
		const TSharedPtr<FJsonObject>* Payload = ReadPayload(Result, OutError);
		if (!Payload)
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Field = nullptr;
		if (!(*Payload)->TryGetArrayField(FieldName, Field))
		{
			OutError = UnreadableResponseError<ErrorType>();
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Element : *Field)
		{
			const TSharedPtr<FJsonObject>* ElementObj = nullptr;
			if (!Element->TryGetObject(ElementObj))
			{
				continue;
			}

			ValueType Value;
			if (ValueType::ParseFromJson(*ElementObj, Value))
			{
				Out.Add(MoveTemp(Value));
			}
		}

		return true;
	}

	/**
	 * Accept a mutation whose answer carries nothing worth surfacing. The server returns a boolean here, and it is
	 * deliberately not treated as the verdict: these operations report a refusal as a GraphQL error, and a false
	 * with no error means the row was already gone, which is the state the caller asked for.
	 */
	template <typename ErrorType>
	bool ReadAcknowledgement(const FCrowdyCppJsonResult& Result, ErrorType& OutError)
	{
		return ReadPayload(Result, OutError) != nullptr;
	}

	/**
	 * The API client for this game instance, or null when the request cannot be issued at all.
	 *
	 * The endpoints come from the configured settings rather than from anywhere per-call, because asking the host
	 * for a different pair rebuilds the shared client and cancels whatever else was in flight on it. Every caller
	 * agreeing on one source is what keeps the client built exactly once.
	 */
	inline FCrowdyCppClient* ResolveApiClient(UGameInstance* GameInstance, const TCHAR* CallerName)
	{
		UCrowdyCppClientSubsystem* Host = GameInstance ? GameInstance->GetSubsystem<UCrowdyCppClientSubsystem>() : nullptr;
		if (!Host)
		{
			UE_LOG(LogCrowdyServices, Warning,
				TEXT("[%s] No API client host on this game instance; the call cannot proceed."), CallerName);
			return nullptr;
		}

		// Refresh the bearer on every call from the live session rather than trusting whatever the shared client
		// still holds. The client is long-lived and keeps the last token installed on it, so a call issued after a
		// sign-out would otherwise go out authenticated as the account that just left, and answer about them. After
		// a sign-out this is empty, which is what makes the call fail instead.
		const UCrowdyGameSession* GameSession = GameInstance->GetSubsystem<UCrowdyGameSession>();
		Host->SetGameToken(GameSession ? GameSession->GetGameToken() : FString());

		FCrowdyCppClient* Client = Host->GetClient(CrowdyServiceApi::ResolveClientConfig());
		if (!Client)
		{
			UE_LOG(LogCrowdyServices, Warning,
				TEXT("[%s] Could not construct the API client; the call cannot proceed."), CallerName);
			return nullptr;
		}

		return Client;
	}

	/**
	 * Hold a completion against the issuing subsystem's lifetime. The API client belongs to a different
	 * game-instance subsystem with its own pump, and the order the two are torn down in is not guaranteed, so a
	 * completion can still arrive after the issuer has cleared its state.
	 *
	 * A guarded completion that lands on a released token does not run. That is only safe because both the issuer
	 * and every latent Blueprint action waiting on it are rooted on the same game instance, so they end together;
	 * a world-scoped issuer must instead let its completion run and gate only the state it touches, or it strands
	 * a latent action whose pins never fire.
	 */
	template <typename ResultType>
	TFunction<void(ResultType)> GuardLifetime(const TSharedPtr<uint8>& LiveToken, TFunction<void(ResultType)> Body)
	{
		TWeakPtr<uint8> Weak = LiveToken;
		return [Weak, Body = MoveTemp(Body)](ResultType Result)
		{
			if (!Weak.IsValid())
			{
				return;
			}
			Body(MoveTemp(Result));
		};
	}

	/** The BigInt JSON string the Game API expects for every id-valued variable. */
	inline FString BigInt(int64 Value)
	{
		return FString::Printf(TEXT("%lld"), Value);
	}

	/** Build the variables object for an operation whose single argument is an input wrapper. */
	inline TSharedPtr<FJsonObject> WrapInput(const TSharedPtr<FJsonObject>& Input)
	{
		TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetObjectField(TEXT("input"), Input);
		return Variables;
	}

	/**
	 * Read the boolean amIGameHost answers with. False for anything that is not a definite yes or no, so a caller
	 * cannot mistake an unread answer for "not the host".
	 */
	inline bool ReadAmIGameHost(const FCrowdyCppJsonResult& Result, bool& bOutAmHost)
	{
		bOutAmHost = false;
		return Result.bTransportOk && Result.Data.IsValid()
			&& Result.Data->TryGetBoolField(TEXT("amIGameHost"), bOutAmHost);
	}

	/**
	 * Read the owning user of the actor this call asked about.
	 *
	 * The answer echoes the uuid it is about, and a row for a different actor is refused rather than reported as
	 * the owner of the one that was asked for. userId is a BigInt, so it arrives as a decimal string.
	 */
	inline bool ReadActorOwner(const FCrowdyCppJsonResult& Result, const FString& ExpectedUuid, int64& OutUserId)
	{
		OutUserId = 0;

		const TSharedPtr<FJsonObject>* Actor = nullptr;
		if (!Result.bTransportOk || !Result.Data.IsValid() || !Result.Data->TryGetObjectField(TEXT("actor"), Actor))
		{
			return false;
		}

		FString EchoedUuid;
		if (!(*Actor)->TryGetStringField(TEXT("uuid"), EchoedUuid) || EchoedUuid != ExpectedUuid)
		{
			return false;
		}

		FString UserIdText;
		if (!(*Actor)->TryGetStringField(TEXT("userId"), UserIdText))
		{
			return false;
		}

		OutUserId = FCString::Atoi64(*UserIdText);
		return true;
	}

	/**
	 * What one getVoxelList answer says about a chunk. bRead separates "the chunk was read and holds nothing"
	 * from "the chunk could not be read at all": the first is a normal empty slot, the second is a failure every
	 * waiting caller has to be told about.
	 */
	struct FVoxelChunkRead
	{
		bool bRead = false;
		/** The voxel's location.x, which the persistence layout uses as the instance slot, to its decoded state. */
		TMap<int16, TArray<uint8>> StateBySlot;
	};

	/** The highest instance slot the persistence layout can address. Slot 0 is reserved for a singleton type. */
	inline constexpr int32 MaxVoxelSlot = 32767;

	/**
	 * The largest state blob a push can produce, so the largest one a read should ever accept back. The bytes are
	 * handed to a struct deserializer, which trusts the counts it reads out of them, so a blob no push could have
	 * written is refused rather than decoded.
	 */
	inline constexpr int32 MaxVoxelStateBytes = MAX_uint16;

	/** Decode a getVoxelList answer. State arrives base64-encoded; a voxel that will not decode is skipped. */
	inline FVoxelChunkRead ReadVoxelChunk(const FCrowdyCppJsonResult& Result)
	{
		FVoxelChunkRead Read;

		const TSharedPtr<FJsonObject>* VoxelList = nullptr;
		if (!Result.bTransportOk || !Result.Data.IsValid()
			|| !Result.Data->TryGetObjectField(TEXT("getVoxelList"), VoxelList))
		{
			return Read;
		}

		// Only once the list itself has been read: an answer whose voxels field is missing or the wrong shape has
		// not told us the chunk is empty, and reporting it as an empty chunk invites a caller to overwrite a slot
		// whose contents it never actually saw.
		const TArray<TSharedPtr<FJsonValue>>* Voxels = nullptr;
		if (!(*VoxelList)->TryGetArrayField(TEXT("voxels"), Voxels))
		{
			return Read;
		}

		Read.bRead = true;

		for (const TSharedPtr<FJsonValue>& VoxelValue : *Voxels)
		{
			const TSharedPtr<FJsonObject>* Voxel = nullptr;
			if (!VoxelValue->TryGetObject(Voxel))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* Location = nullptr;
			if (!(*Voxel)->TryGetObjectField(TEXT("location"), Location))
			{
				continue;
			}

			// A slot that cannot be read is not slot 0. Slot 0 is the singleton row, so letting an unreadable
			// value fall through to it would answer every singleton pull with an unrelated voxel's bytes.
			FString SlotText;
			if (!(*Location)->TryGetStringField(TEXT("x"), SlotText))
			{
				continue;
			}

			const int32 Slot = FCString::Atoi(*SlotText);
			if (Slot < 0 || Slot > MaxVoxelSlot)
			{
				continue;
			}

			FString StateBase64;
			if (!(*Voxel)->TryGetStringField(TEXT("state"), StateBase64))
			{
				continue;
			}

			TArray<uint8> Decoded;
			if (FBase64::Decode(StateBase64, Decoded) && !Decoded.IsEmpty()
				&& Decoded.Num() <= MaxVoxelStateBytes)
			{
				Read.StateBySlot.Add(static_cast<int16>(Slot), MoveTemp(Decoded));
			}
		}

		return Read;
	}

	/** Set a field to the given permission keys, which the server takes as a list of strings. */
	inline void SetPermissionKeys(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Keys)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Key : Keys)
		{
			Values.Add(MakeShared<FJsonValueString>(Key));
		}
		Object->SetArrayField(TEXT("permissions"), Values);
	}
}
