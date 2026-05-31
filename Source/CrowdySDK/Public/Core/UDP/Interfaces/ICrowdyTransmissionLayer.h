#pragma once
#include "CoreMinimal.h"

/**
 * @brief Abstract interface for implementing transmission layers within the CrowdySDK system.
 *
 * This interface provides a mechanism to send serialized data through a transmission medium.
 * Implementations must define their specific behavior for transmitting data by overriding the SendBytes method.
 */
class CROWDYSDK_API ICrowdyTransmissionLayer
{
public:
	/**
	 * Virtual destructor for the ICrowdyTransmissionLayer interface.
	 *
	 * Ensures proper cleanup of resources in derived classes when an object of a
	 * type inheriting from ICrowdyTransmissionLayer is deleted through a pointer
	 * to the base class.
	 */
	virtual ~ICrowdyTransmissionLayer() = default;
	/**
	 * Sends a byte array to the transmission layer.
	 *
	 * This method is responsible for transmitting a set of bytes encapsulated
	 * in a TArray<uint8> to the underlying transmission mechanism. The data
	 * is transferred using a move operation to ensure efficient memory handling.
	 *
	 * @param Data An rvalue reference to a TArray of uint8 representing the byte
	 *             array to be transmitted.
	 * @param bRequiresAuth
	 * @param SequenceNumber
	 */
	virtual void SendBytes(TArray<uint8>&& Data, bool bRequiresAuth, uint8 SequenceNumber) = 0;
};