#include "Customizations/CrowdyFunctionEntryCustomization.h"

#include "CrowdySDKEditor.h"
#include "DetailLayoutBuilder.h"
#include "Kismet2/BlueprintEditorUtils.h"

// ─────────────────────────────────────────────────────────────────────────────
// IDetailCustomization
// ─────────────────────────────────────────────────────────────────────────────
void FCrowdyFunctionEntryCustomization::CustomizeDetails(
	IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.IsEmpty()) return;

	EditedNode = Cast<UK2Node_FunctionEntry>(Objects[0].Get());
	if (!EditedNode.IsValid()) return;

	BuildCrowdyCategory(DetailBuilder);
}

// ─────────────────────────────────────────────────────────────────────────────
// FCrowdyHandlerCustomizationBase interface
// ─────────────────────────────────────────────────────────────────────────────
bool FCrowdyFunctionEntryCustomization::IsStamped() const
{
	if (!EditedNode.IsValid()) return false;
	return EditedNode->MetaData.HasMetaData(CrowdyMetaKeys::CrowdyEvent);
}

void FCrowdyFunctionEntryCustomization::SetStamped(bool bStamped)
{
	if (!EditedNode.IsValid()) return;

	if (bStamped)
		EditedNode->MetaData.SetMetaData(
			CrowdyMetaKeys::CrowdyEvent, FString());
	else
		EditedNode->MetaData.RemoveMetaData(CrowdyMetaKeys::CrowdyEvent);

	UBlueprint* Blueprint = EditedNode->GetBlueprint();
	if (!Blueprint) return;

	// Metadata-only change — do not synchronously compile. The pre-compile
	// hook transfers this onto the UFunction on the user's next compile.
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->MarkPackageDirty();

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdySDK] Function '%s' CrowdyEvent stamp %s. Recompile to activate."),
		*EditedNode->GetGraph()->GetName(),
		bStamped ? TEXT("added") : TEXT("removed"));
}

const TArray<UEdGraphPin*>& FCrowdyFunctionEntryCustomization::GetNodePins() const
{
	static const TArray<UEdGraphPin*> Empty;
	return EditedNode.IsValid() ? EditedNode->Pins : Empty;
}

int32 FCrowdyFunctionEntryCustomization::GetNodeFunctionFlags() const
{
	return EditedNode.IsValid() ? EditedNode->GetFunctionFlags() : 0;
}

FString FCrowdyFunctionEntryCustomization::GetNodeDisplayName() const
{
	if (!EditedNode.IsValid()) return TEXT("(invalid)");
	return EditedNode->GetGraph()->GetName();
}
