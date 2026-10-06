#include "CrowdyServerObjectFactory.h"

#include "CrowdyServerObjectAssetTypeActions.h"
#include "CrowdyServerObjectDefinition.h"

#define LOCTEXT_NAMESPACE "CrowdyServerObjectFactory"

UCrowdyServerObjectFactory::UCrowdyServerObjectFactory()
{
	SupportedClass = UCrowdyServerObjectDefinition::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UCrowdyServerObjectFactory::FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
	UObject* Context, FFeedbackContext* Warn)
{
	return NewObject<UCrowdyServerObjectDefinition>(InParent, InClass, InName, Flags);
}

uint32 UCrowdyServerObjectFactory::GetMenuCategories() const
{
	return FCrowdyServerObjectAssetTypeActions::FindOrRegisterCrowdyCategory();
}

FText UCrowdyServerObjectFactory::GetDisplayName() const
{
	return LOCTEXT("DisplayName", "Server Object");
}

FText UCrowdyServerObjectFactory::GetToolTip() const
{
	return LOCTEXT("ToolTip", "A Server Object definition: the state, functions and rules the server runs for one kind of object.");
}

FString UCrowdyServerObjectFactory::GetDefaultNewAssetName() const
{
	return TEXT("CSO_NewServerObject");
}

#undef LOCTEXT_NAMESPACE
