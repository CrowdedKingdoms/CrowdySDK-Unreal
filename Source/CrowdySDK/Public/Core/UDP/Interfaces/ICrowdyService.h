#pragma once

#include "CoreMinimal.h"
class UCrowdySDKSubsystem;

/**
 * Interface defining the core functionality of the ICrowdyService.
 *
 * The ICrowdyService serves as an abstraction layer for implementing
 * services related to crowd management, supporting extensibility
 * and standardized behavior. Specific implementations of this
 * interface should provide concrete definitions for the methods declared
 * in the interface to enable custom behavior as per service requirements.
 */
class CROWDYSDK_API ICrowdyService
{
public:
	/**
	 * Virtual destructor for the ICrowdyService interface.
	 *
	 * This destructor ensures that the destruction of objects
	 * implementing the ICrowdyService interface is handled
	 * properly, allowing derived classes to clean up resources
	 * as needed.
	 */
	virtual ~ICrowdyService();
	/**
	 * Initializes the Crowdy service with the provided SDK subsystem.
	 *
	 * This method is responsible for setting up the service using the
	 * specified subsystem object, enabling it to interact with the
	 * Crowdy SDK functionalities.
	 *
	 * @param SDK A pointer to the UCrowdySDKSubsystem object containing
	 *            the necessary configurations and context for initialization.
	 */
	virtual void Initialize(UCrowdySDKSubsystem* SDK) = 0;
};
