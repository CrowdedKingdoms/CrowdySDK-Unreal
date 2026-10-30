#pragma once
#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyFrame.h"

/** Proximity text from another player: the body is the message as UTF-8, and the envelope's actor id is the sender. */
struct FTextMessageNotification : ICrowdyMessage
{
	FString Message;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_TEXT_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Text Message Notification";
	}

	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		ApplyEnvelope(Frame);
		if (!ApplyEnvelopeActorId(Frame))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FTextMessageNotification::DecodePayload - the frame carries no actor id"));
			return false;
		}

		// Text ends at a zero byte, so binary from an older SDK's text format shows as a few characters at most.
		const TConstArrayView<uint8> Body = Frame.Body;
		int32 Length = Body.Find(0);
		Length = Length == INDEX_NONE ? Body.Num() : Length;
		if (Length == 0)
		{
			return false;
		}

		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Length);
		Message = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
		return true;
	}

};
