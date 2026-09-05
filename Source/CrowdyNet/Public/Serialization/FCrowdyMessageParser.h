#pragma once
#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Serialization/CrowdyFrame.h"
#include "Serialization/CrowdyMessagePool.h"
#include "Templates/SharedPointer.h"
#include "Templates/Function.h"

class UCrowdyGameSession;
class FCrowdyServiceRegistry;
class UCrowdyUDPSubsystem;
class ICrowdyMessage;

class CROWDYNET_API FCrowdyMessageParser
{

public:

	FCrowdyMessageParser(FCrowdyServiceRegistry* InServiceRegistry,
		UCrowdyUDPSubsystem* InUDPSubsystem,
		UCrowdyGameSession* InGameSession,
		TFunction<void()> InTokenExpiredCallback = nullptr);
	~FCrowdyMessageParser() = default;

	/**
	 * Reads one datagram: takes the envelope off it, or unpacks it when it packs several messages
	 * together. For a transport that receives bytes.
	 *
	 * The returned message borrows the datagram's octets rather than copying them out, so the caller
	 * must keep the datagram alive until that message has finished being delivered. Reusing the receive
	 * buffer between the parse and the dispatch reads recycled memory.
	 */
	[[nodiscard]] TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> ParseMessage(TConstArrayView<uint8> Datagram);

	/**
	 * Builds the message one frame describes, or the placeholder when the bytes do not decode.
	 *
	 * For a transport that is handed messages already decoded: it fills the frame's envelope and body
	 * itself and comes in here, so the same decoders and the same routing run whichever transport
	 * carried the message.
	 *
	 * The returned message borrows the frame's octets, so the frame's bytes must outlive its delivery
	 * and not merely this call.
	 */
	[[nodiscard]] TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> DecodeFrame(const FCrowdyFrame& Frame);

	void SetExpectedActorStateSize(const int32 NewSize);

	/** The pool behind the actor update, so a caller can read what it did rather than infer it from timing. */
	const TCrowdyMessagePool<FActorUpdateNotificationMessage>& GetActorUpdatePool() const { return ActorUpdatePool; }

private:

	/**
	 * Unpacks a datagram that packs several messages together, dispatching each one itself, and returns
	 * the placeholder that stands in for the bundle. The bundle is not a message a subscriber can act on.
	 * A packed datagram may contain another, and bNested marks that inner call so that a datagram which
	 * cannot be read to its end is reported once rather than once per level.
	 */
	[[nodiscard]] TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> ParseBundle(TConstArrayView<uint8> Datagram,
		bool bNested = false);

	void HandleGenericErrorMessage(const uint8 ErrorType, const uint8 SequenceNumber) const;

	UCrowdyGameSession* GameSession;
	TFunction<void()> TokenExpiredCallback;
	FCrowdyServiceRegistry* ServiceRegistry;
	UCrowdyUDPSubsystem* UDPSubsystem;
	int32 ExpectedActorStateSize = 300;

	// The actor update is the volume opcode, so it is the one whose object is pooled. The pool grows to the
	// number of messages held at once, which is a whole frame's worth wherever the receive path retains the
	// message it decoded instead of copying the payload out.
	TCrowdyMessagePool<FActorUpdateNotificationMessage> ActorUpdatePool;
};
