#include "CrowdyKitBridge.h"

#include "CrowdyCppClient.h"

FCrowdyKitBundle FCrowdyKitBridge::EmitBundle(int64 AppId, const TArray<FCrowdyKitLayerSpec>& Layers,
	const FString& SessionId)
{
	FCrowdyKitBundle Bundle;
	Bundle.ErrorMessage = CrowdyCppGameModelDeprecatedMessage;
	return Bundle;
}

void FCrowdyKitBridge::DeployBundle(const TSharedRef<FCrowdyCppClient>& Client, const FCrowdyKitBundle& Bundle,
	TFunction<void(FCrowdyKitDeployResult)> OnDone)
{
	if (!OnDone)
	{
		return;
	}
	FCrowdyKitDeployResult Result;
	Result.ErrorMessage = CrowdyCppGameModelDeprecatedMessage;
	OnDone(MoveTemp(Result));
}
