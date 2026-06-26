#pragma once

#include "CoreMinimal.h"
class UCrowdySDKBridgeSubsystem;

class CROWDYNET_API ICrowdyService
{
public:
	virtual ~ICrowdyService();
	virtual void Initialize(UCrowdySDKBridgeSubsystem* Bridge) = 0;
};
