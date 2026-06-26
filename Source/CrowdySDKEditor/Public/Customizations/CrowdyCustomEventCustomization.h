#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "K2Node_CustomEvent.h"
#include "Types/SlateEnums.h"

class UEnum;
class UEdGraphPin;
class IDetailLayoutBuilder;
class IDetailCategoryBuilder;

class FCrowdyCustomEventCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance()
	{
		return MakeShared<FCrowdyCustomEventCustomization>();
	}

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	const TArray<UEdGraphPin*>& GetNodePins() const;
	int32 GetNodeFunctionFlags() const;

	// "Crowdy Replicates" turns the event into an RPC: calling it routes over the transport
	// and runs the body on every client. It reveals the per-event routing dropdowns when enabled.
	void BuildReplicateCategory(IDetailLayoutBuilder& DetailBuilder);

	bool IsReplicated() const;
	void SetReplicated(bool bReplicated);
	ECheckBoxState GetReplicateCheckState() const;
	void OnReplicateCheckChanged(ECheckBoxState NewState);
	EVisibility GetRoutingVisibility() const;

	// Routing dropdowns are stored as enum-name metadata on the node, the same spelling the
	// runtime resolves back into FCrowdyFnInfo.
	int32 GetRoutingValue(FName MetaKey, const UEnum* EnumType, int32 DefaultValue) const;
	void SetRoutingValue(FName MetaKey, const UEnum* EnumType, int32 NewValue);
	void OnRoutingChanged(int32 NewValue, ESelectInfo::Type SelectInfo, FName MetaKey, const UEnum* EnumType);

	// bSpatialOnly greys the row out unless the recipient is Spatial Multicast — Decay and Distance
	// only affect the spatial path; the channel (Multicast) and 142 (Owning Client/Host) transports
	// ignore them, so editing them there would be misleading.
	void AddRoutingRow(IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder,
		const FText& Label, const FText& ToolTip, FName MetaKey, const UEnum* EnumType, int32 DefaultValue,
		bool bSpatialOnly = false);

	// True when the recipient is Spatial Multicast — gates the Decay/Distance rows' editability.
	bool IsSpatialRoutingEnabled() const;

	FText GetReplicateStatusText() const;
	FSlateColor GetReplicateStatusColor() const;

	// Channel field (Multicast only): which channel the event routes over, by name. A live dropdown
	// lists the app's channels (queried with the Crowdy Studio sign-in token); the text box is always
	// editable as a fallback, so a name not yet in the list can still be typed.
	EVisibility GetChannelRowVisibility() const;
	FText GetChannelText() const;
	void OnChannelTextCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void SetChannelValueAction(FString ChannelName);
	void WriteChannelMeta(const FString& ChannelName);
	TSharedRef<SWidget> BuildChannelPickerMenu();

	// Mirrors the compiler's RPC parameter check so a bad signature shows in the panel
	// before a compile.
	struct FRpcSignatureResult
	{
		bool bValid = false;
		int32 ParamCount = 0;
		FString Problem;
	};
	FRpcSignatureResult InspectRpcSignature() const;

	TWeakObjectPtr<UK2Node_CustomEvent> EditedNode;
};
