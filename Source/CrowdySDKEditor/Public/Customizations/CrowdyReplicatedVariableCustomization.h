#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "Layout/Visibility.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "UObject/WeakFieldPtr.h"
#include "Customizations/CrowdyReplicationMode.h" // ECrowdyReplicationMode + the pure key-scrub decision

class UBlueprint;
class IBlueprintEditor;
class IDetailLayoutBuilder;
class IDetailCategoryBuilder;
class IPropertyUtilities;
class FProperty;
class SWidget;

/**
 * Details customization for a Blueprint VARIABLE. Adds the unified "Crowdy Replication" mode dropdown to
 * the variable's Details panel: choosing Replicated stamps the CrowdyState metadata keys onto the
 * variable so it flows through the same discovery + bake as a C++ meta=(CrowdyState) property (a
 * Blueprint variable becomes an FProperty on the generated class, which the CrowdyState layout builder and
 * CrowdyStateMetaKeys::HasStateMeta read with no runtime change).
 *
 * Mirrors FCrowdyCustomEventCustomization's read/write-metadata + row-visibility idioms, but targets a
 * BP variable's FProperty (via FBlueprintEditorUtils::Get/Set/RemoveBlueprintVariableMetaData) instead
 * of a custom event node's FKismetUserDeclaredFunctionMetadata.
 *
 * The customization is registered per Blueprint editor through the Kismet module's
 * RegisterVariableCustomization (see FCrowdySDKEditorModule::RegisterVariableCustomization); MakeInstance
 * returns null unless the edited Blueprint is an AActor / UActorComponent (CrowdyState view state), so the row
 * appears only where CrowdyState applies.
 */
class FCrowdyReplicatedVariableCustomization : public IDetailCustomization
{
public:
	// Factory bound via FOnGetVariableCustomizationInstance::CreateStatic. Resolves the UBlueprint from the
	// editor and returns null unless it is an actor / actor-component Blueprint.
	static TSharedPtr<IDetailCustomization> MakeInstance(TSharedPtr<IBlueprintEditor> InBlueprintEditor);

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	// The modes the dropdown offers: { None, Replicated }. Adding a future mode means appending it here
	// rather than editing the widget code.
	static TArray<ECrowdyReplicationMode> SelectableModes();

	// Human-readable name / tooltip for a mode, shared by the combo's current-selection text and its
	// per-row entries.
	static FText ModeDisplayText(ECrowdyReplicationMode Mode);
	static FText ModeTooltipText(ECrowdyReplicationMode Mode);

	// Builds the "Crowdy Replication" category: the mode combo, then the Replicated-only RepNotify and
	// Advanced (owner-only / manual-dirty) rows.
	void BuildReplicationCategory(IDetailLayoutBuilder& DetailBuilder);

	TSharedRef<SWidget> BuildModeComboContent();

	// The variable's current mode read back from its metadata: CrowdyState present -> Replicated, else None.
	ECrowdyReplicationMode GetCurrentMode() const;
	FText GetCurrentModeText() const;

	// Writes the mode switch: scrubs the keys the previous mode owned that the new mode does not (per
	// CrowdyReplicationModeDecision::KeysToScrubOnSwitch), then writes the new mode's marker, so switching
	// never leaves stale metadata behind. CrowdyOnRep is cleared only on leaving Crowdy entirely (None).
	void SetMode(ECrowdyReplicationMode NewMode);

	// Auto-create a parameterless OnRep_<Variable> Blueprint function (matching the engine's native RepNotify
	// naming) unless a usable one already exists, and return its name. Mirrors the engine's own
	// FBlueprintVarActionDetails::OnChangeReplication RepNotify branch: the FindObject / FindFunctionByName
	// pre-guard is load-bearing because CreateNewGraph renames a name-colliding graph aside rather than
	// reusing it, so calling it unconditionally would clobber a user's existing OnRep body. Returns NAME_None
	// only if the Blueprint/variable is invalid.
	FName EnsureOnRepGraph();

	// Stamp CrowdyOnRep with an auto-created OnRep_<Var> when the variable has no notify yet. Shared by both
	// Replicated mode. No-op if one is already set.
	void EnsureDefaultOnRep();

	// Force this variable's native UE replication fully off (mirrors the engine None-branch of
	// OnChangeReplication): clear CPF_Net + CPF_RepNotify, clear the native RepNotify function, and reset the
	// native ReplicationCondition to COND_None. Makes Crowdy replication and native replication mutually
	// exclusive: a Crowdy-replicated variable never also carries native rep. Only touches the variable in hand.
	void ClearNativeReplication();

	// True when the variable's type can ride CrowdyState (mirrors the shared CrowdyState type classifier).
	// When false the Replicated entry is disabled and an inline reason is shown.
	bool IsVariableStateReplicatable() const;

	// Show the inline "type cannot be replicated" reason whenever the variable's type is unsupported,
	// independent of the current mode (an unsupported type can never enter Replicated mode).
	EVisibility GetTypeWarningVisibility() const;

	// The parameterless-RepNotify (CrowdyOnRep) row (GAS-style notify convention).
	void BuildRepNotifyRow(IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder);

	// RepNotify (CrowdyOnRep): the parameterless notify function name. The text box is authoritative;
	// committing an empty string removes the key. The Pick menu lists only this Blueprint's own user-created
	// zero-parameter functions (not inherited/native ones).
	FText GetRepNotifyText() const;
	void OnRepNotifyCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void SetRepNotifyValue(FString FunctionName);
	TSharedRef<SWidget> BuildRepNotifyPickerMenu();

	// Advanced checkboxes: presence of CrowdyOwnerOnly / CrowdyManualDirty (both stored as empty-string
	// key-only tags, like the C++ meta flags).
	ECheckBoxState GetOwnerOnlyCheckState() const;
	void OnOwnerOnlyCheckChanged(ECheckBoxState NewState);
	ECheckBoxState GetManualDirtyCheckState() const;
	void OnManualDirtyCheckChanged(ECheckBoxState NewState);

	// Heartbeat (CrowdyHeartbeat) checkbox: whether this variable rides the periodic keyframe heartbeat.
	// Unlike the C++ default (opt-in), SetMode pre-writes this key when a variable is first made Replicated, so
	// the toggle defaults On for Blueprint authors; unchecking removes the key (replicate-on-change only).
	ECheckBoxState GetHeartbeatCheckState() const;
	void OnHeartbeatCheckChanged(ECheckBoxState NewState);

	// Metadata helpers over FBlueprintEditorUtils, keyed on VariableProperty->GetFName() with a null local
	// scope (a plain, non-local BP variable). Read returns false and clears Out when the key is absent.
	bool HasVariableMeta(const TCHAR* Key) const;
	FString GetVariableMeta(const TCHAR* Key) const;

	// Sets (value present) or removes (unset value) one CrowdyState key on the variable, inside a scoped
	// transaction. Does NOT mark/log; call MarkForMetaChange once after a batch of stamps.
	void StampVariableMeta(const TCHAR* Key, const TOptional<FString>& Value, const FText& TransactionLabel);

	// Marks the Blueprint structurally modified + the package dirty and logs. The stamped metadata is already
	// live on the generated FProperty synchronously; the user's next Compile fires the incremental
	// UpdateClassRepLayout (no full sweep), matching the "Crowdy Replicates" event checkbox. Batched: one call
	// after a group of StampVariableMeta writes (a mode switch scrubs several keys but marks once).
	void RecompileForMetaChange(const TCHAR* LoggedKey, bool bValueSet);

	// Convenience for the single-key edits (RepNotify commit, the Advanced checkboxes): stamp one key then
	// mark once.
	void WriteVariableMeta(const TCHAR* Key, const TOptional<FString>& Value, const FText& TransactionLabel);

	TWeakObjectPtr<UBlueprint> Blueprint;

	// The variable's FProperty on the generated class, resolved from the UPropertyWrapper being edited.
	TWeakFieldPtr<FProperty> VariableProperty;

	// Cached in CustomizeDetails so a mode switch can rebuild the panel (RequestForceRefresh) from the combo
	// callback. The Replicated-only rows (RepNotify + the Advanced group) are structural now (created only in
	// Replicated mode), because an IDetailGroup has no per-row Visibility hook, so its disclosure header would
	// linger in None mode. Switching mode therefore has to regenerate the layout, not just toggle row
	// visibility.
	TSharedPtr<IPropertyUtilities> PropertyUtilities;
};
