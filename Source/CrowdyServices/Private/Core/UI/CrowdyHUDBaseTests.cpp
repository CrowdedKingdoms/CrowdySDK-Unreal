#include "Core/UI/CrowdyHUDBase.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

// The two HUD configuration properties are authored on a blueprint, so they have to survive a save.
// They were declared Transient, which drops the authored value on load and forces every consuming
// project to reassign it from a graph at runtime. This pins the flag rather than the behaviour: it
// cannot save and reload a blueprint, but it does fail if the flag comes back.
namespace CrowdyHUDBaseTestSupport
{
	constexpr EAutomationTestFlags HudTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FProperty* FindHudProperty(const TCHAR* PropertyName)
	{
		return FindFProperty<FProperty>(ACrowdyHUDBase::StaticClass(), PropertyName);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyHudConfigPropertiesArePersistent,
	"CrowdySDK.Services.HudConfigPropertiesArePersistent",
	CrowdyHUDBaseTestSupport::HudTestFlags)

bool FCrowdyHudConfigPropertiesArePersistent::RunTest(const FString& Parameters)
{
	using namespace CrowdyHUDBaseTestSupport;

	for (const TCHAR* PropertyName : { TEXT("HUDWidgetClass"), TEXT("WidgetSetConfig") })
	{
		FProperty* Property = FindHudProperty(PropertyName);
		if (!TestNotNull(FString::Printf(TEXT("%s exists on ACrowdyHUDBase"), PropertyName), Property))
		{
			continue;
		}

		TestFalse(FString::Printf(TEXT("%s is not Transient, so a value authored on a HUD blueprint survives a save"), PropertyName),
			Property->HasAnyPropertyFlags(CPF_Transient));

		TestTrue(FString::Printf(TEXT("%s is still editable, so the control that fixed it is the flag and not the visibility"), PropertyName),
			Property->HasAnyPropertyFlags(CPF_Edit));
	}

	return true;
}

#endif
