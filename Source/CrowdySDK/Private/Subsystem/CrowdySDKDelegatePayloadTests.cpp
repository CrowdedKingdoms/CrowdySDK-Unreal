#include "Subsystem/CrowdySDKSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

// The sign-in delegates on this subsystem are BlueprintAssignable, so whatever they carry can be bound
// and printed by a consuming project. They used to broadcast the app-scoped game token, and OnRegister
// did it through a parameter named Message, which reads like display text. Nothing that resolves to a
// credential may appear in these signatures again.
namespace CrowdySDKDelegatePayloadTestSupport
{
	constexpr EAutomationTestFlags PayloadTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const UFunction* FindDelegateSignature(const TCHAR* PropertyName)
	{
		const FMulticastDelegateProperty* Property =
			FindFProperty<FMulticastDelegateProperty>(UCrowdySDKSubsystem::StaticClass(), PropertyName);

		return Property ? Property->SignatureFunction : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKSignInDelegatesNameNoCredential,
	"CrowdySDK.SDK.SignInDelegatesNameNoCredential",
	CrowdySDKDelegatePayloadTestSupport::PayloadTestFlags)

bool FCrowdySDKSignInDelegatesNameNoCredential::RunTest(const FString& Parameters)
{
	using namespace CrowdySDKDelegatePayloadTestSupport;

	// Substrings that name a credential rather than a value safe to display. This checks the NAMES only:
	// a credential passed through a parameter called Message, which is exactly how OnRegister leaked one,
	// is invisible here and only a reading of the broadcast sites catches it.
	const TArray<FString> ForbiddenNames = { TEXT("Token"), TEXT("Secret"), TEXT("Bearer"), TEXT("Password") };

	for (const TCHAR* PropertyName : { TEXT("OnLogin"), TEXT("OnRegister"), TEXT("OnLogout") })
	{
		const UFunction* Signature = FindDelegateSignature(PropertyName);
		if (!TestNotNull(FString::Printf(TEXT("%s is a multicast delegate on UCrowdySDKSubsystem"), PropertyName),
			const_cast<UFunction*>(Signature)))
		{
			continue;
		}

		for (TFieldIterator<FProperty> It(Signature); It; ++It)
		{
			const FString ParameterName = It->GetName();
			for (const FString& Forbidden : ForbiddenNames)
			{
				TestFalse(
					FString::Printf(TEXT("%s parameter '%s' does not name a credential"), PropertyName, *ParameterName),
					ParameterName.Contains(Forbidden));
			}
		}
	}

	return true;
}

#endif
