#include "Customizations/CrowdyCustomEventCustomization.h"

#include "CrowdySDKEditor.h"
#include "DetailLayoutBuilder.h"
#include "Kismet2/BlueprintEditorUtils.h"

void FCrowdyCustomEventCustomization::CustomizeDetails(
	IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.IsEmpty()) return;

	EditedNode = Cast<UK2Node_CustomEvent>(Objects[0].Get());
	if (!EditedNode.IsValid()) return;

	BuildCrowdyCategory(DetailBuilder);
}

bool FCrowdyCustomEventCustomization::IsStamped() const
{
	if (!EditedNode.IsValid()) return false;
	return EditedNode->GetUserDefinedMetaData().HasMetaData(
		CrowdyMetaKeys::CrowdyEvent);
}

void FCrowdyCustomEventCustomization::SetStamped(bool bStamped)
{
	if (!EditedNode.IsValid()) return;

	if (bStamped)
	{
		EditedNode->GetUserDefinedMetaData().SetMetaData(
			CrowdyMetaKeys::CrowdyEvent, FString());
	}
	else
	{
		EditedNode->GetUserDefinedMetaData().RemoveMetaData(
			CrowdyMetaKeys::CrowdyEvent);
	}

	UBlueprint* Blueprint = EditedNode->GetBlueprint();
	if (!Blueprint) return;

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->MarkPackageDirty();

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdySDK] Custom event '%s' CrowdyEvent stamp %s. Recompile to activate."),
		*EditedNode->CustomFunctionName.ToString(),
		bStamped ? TEXT("added") : TEXT("removed"));
}

const TArray<UEdGraphPin*>& FCrowdyCustomEventCustomization::GetNodePins() const
{
	static const TArray<UEdGraphPin*> Empty;
	return EditedNode.IsValid() ? EditedNode->Pins : Empty;
}

int32 FCrowdyCustomEventCustomization::GetNodeFunctionFlags() const
{
	return EditedNode.IsValid() ? EditedNode->FunctionFlags : 0;
}

FString FCrowdyCustomEventCustomization::GetNodeDisplayName() const
{
	if (!EditedNode.IsValid()) return TEXT("(invalid)");
	return EditedNode->CustomFunctionName.ToString();
}
