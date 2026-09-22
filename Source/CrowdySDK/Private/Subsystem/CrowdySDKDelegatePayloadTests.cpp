#include "Subsystem/CrowdySDKSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Interfaces/IPluginManager.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

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

namespace CrowdySDKDelegatePayloadTestSupport
{
	int32 CheckArrayParametersAreConstRef(FAutomationTestBase& Test, const UFunction* Signature, const FString& Owner)
	{
		int32 Checked = 0;
		for (TFieldIterator<FProperty> ParamIt(Signature); ParamIt; ++ParamIt)
		{
			if (!ParamIt->IsA<FArrayProperty>())
			{
				continue;
			}
			++Checked;
			Test.TestTrue(
				FString::Printf(TEXT("%s parameter '%s' is a const TArray reference"), *Owner, *ParamIt->GetName()),
				ParamIt->HasAllPropertyFlags(CPF_ConstParm | CPF_ReferenceParm));
		}
		return Checked;
	}
}

// A Blueprint cannot bind a dynamic delegate that carries a TArray by value: the node fails to compile with
// "Signature Error" as soon as it is placed, for a multicast property bound with Bind Event and for a single-cast
// parameter bound through Create Event alike. Every array an SDK delegate exposes to Blueprint is const TArray<T>&.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKDynamicDelegatesPassArraysByConstRef,
	"CrowdySDK.SDK.DynamicDelegatesPassArraysByConstRef",
	CrowdySDKDelegatePayloadTestSupport::PayloadTestFlags)

bool FCrowdySDKDynamicDelegatesPassArraysByConstRef::RunTest(const FString& Parameters)
{
	using namespace CrowdySDKDelegatePayloadTestSupport;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
	if (!TestTrue(TEXT("CrowdySDK plugin is registered"), Plugin.IsValid()))
	{
		return false;
	}

	TSet<FName> SdkPackages;
	for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
	{
		SdkPackages.Add(*FString::Printf(TEXT("/Script/%s"), *Module.Name.ToString()));
	}

	int32 ArrayParametersChecked = 0;
	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;
		if (!SdkPackages.Contains(Class->GetPackage()->GetFName()))
		{
			continue;
		}

		const bool bAsyncAction = Class->IsChildOf<UBlueprintAsyncActionBase>();
		for (TFieldIterator<FMulticastDelegateProperty> DelegateIt(Class, EFieldIteratorFlags::ExcludeSuper); DelegateIt; ++DelegateIt)
		{
			if (!bAsyncAction && !DelegateIt->HasAnyPropertyFlags(CPF_BlueprintAssignable))
			{
				continue;
			}
			ArrayParametersChecked += CheckArrayParametersAreConstRef(*this, DelegateIt->SignatureFunction,
				FString::Printf(TEXT("%s.%s"), *Class->GetName(), *DelegateIt->GetName()));
		}

		// A single-cast delegate a BlueprintCallable takes as a parameter is bound with Create Event, same rule.
		for (TFieldIterator<UFunction> FunctionIt(Class, EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
		{
			if (!FunctionIt->HasAnyFunctionFlags(FUNC_BlueprintCallable))
			{
				continue;
			}
			for (TFieldIterator<FDelegateProperty> ParamIt(*FunctionIt); ParamIt; ++ParamIt)
			{
				ArrayParametersChecked += CheckArrayParametersAreConstRef(*this, ParamIt->SignatureFunction,
					FString::Printf(TEXT("%s::%s(%s)"), *Class->GetName(), *FunctionIt->GetName(), *ParamIt->GetName()));
			}
		}
	}

	TestTrue(TEXT("at least one array-carrying SDK delegate was checked"), ArrayParametersChecked > 0);
	return true;
}

#endif
