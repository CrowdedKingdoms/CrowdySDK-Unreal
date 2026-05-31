#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
/**
 * @enum ECrowdyMessageType
 * @brief Represents the different types of messages used in the Crowdy system.
 *
 * This enumeration defines identifiers for various message types that can be
 * used within the Crowdy application or system for categorization and
 * processing of messages.
 */
enum class ECrowdyMessageType : uint8;
/**
 * @class ICrowdyMessage
 * @brief Interface representing a message in the Crowdy system.
 *
 * This interface defines the structure and behavior required for
 * any message implemented in the Crowdy system. It serves as a
 * contract ensuring all messages conform to a standard format
 * and functionality.
 *
 * The ICrowdyMessage interface should be implemented by any class
 * that aims to handle messages within the Crowdy system, allowing
 * for consistent access and manipulation.
 *
 * Responsibilities of implementations include but are not limited to:
 * - Representing message content in a structured manner.
 * - Providing metadata or attributes associated with the message.
 * - Supporting operations related to message lifecycle or processing.
 */
class ICrowdyMessage;

/**
 * @class ICrowdyReceptionLayer
 * @brief Interface representing a reception layer for managing and processing
 *        crowd-based reception or interaction events.
 */
class CROWDYSDK_API ICrowdyReceptionLayer
{
public:
	
	virtual ~ICrowdyReceptionLayer() = default;
	/**
	 * Handles the reception of a message and processes it accordingly.
	 *
	 * @param Message The message object received that contains the data to be processed.
	 */
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) = 0;
	/**
	 * Determines if the given message type is supported or handled by the system.
	 *
	 * @param MessageType The type of message to check for support.
	 * @return true if the message type is handled, false otherwise.
	 */
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const = 0;
	
	virtual TArray<FName> GetSupportedActorUpdateTypes() const
	{
		return {};
	}
	
	// New function to allow selected dispatch of events
	UE_DEPRECATED(5.8, "Override GetSupportedEvents Instead and return an array of UScriptStructs*")
	virtual TArray<FName> GetSupportedEventTypes() const
	{
		return {};
	}
	
	virtual TArray<UScriptStruct*> GetSupportedEvents() const
	{
		return {};
	}
	
};
