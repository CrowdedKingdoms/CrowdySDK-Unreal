// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/CrowdyEffectCustomization.h"

#include "Baking/CrowdyRegistryBaker.h"
#include "ClassViewerFilter.h"
#include "Containers/Ticker.h"
#include "Customizations/CrowdyEffectCompileSummary.h"
#include "Customizations/CrowdyEffectPickerOptions.h"
#include "DetailCategoryBuilder.h"
#include "Editor.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "EditorClassUtils.h"
#include "Engine/Blueprint.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameModel/CrowdyContainerBlueprintExtension.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailPropertyRow.h"
#include "IPropertyUtilities.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyHandle.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectCustomization"

namespace
{
	template <typename TEnum>
	TEnum GetEnumHandleValue(const TSharedPtr<IPropertyHandle>& Handle, TEnum Default)
	{
		uint8 Value = static_cast<uint8>(Default);
		if (Handle.IsValid())
		{
			Handle->GetValue(Value);
		}
		return static_cast<TEnum>(Value);
	}

	FString GetStringHandleValue(const TSharedPtr<IPropertyHandle>& Handle)
	{
		FString Value;
		if (Handle.IsValid())
		{
			Handle->GetValue(Value);
		}
		return Value;
	}

	// A class is a Game Model container if it is a Blueprint marked (or compiled) as one, or a native/compiled class
	// carrying the CrowdyContainer tag. Mirrors UCrowdyEffect::Compile's own container resolution, so the picker
	// only offers classes an effect can actually compile against.
	bool IsCrowdyContainerClass(const UClass* Class)
	{
		if (!Class)
		{
			return false;
		}
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
		{
			return CrowdyContainerMarker::IsGameModelContainerClass(Blueprint);
		}
		FString TypeName;
		return FCrowdyAttributeRegistry::GetContainerTypeName(Class, TypeName);
	}

	// The class-viewer filter behind the ContainerClass picker. Marked container Blueprints are force-loaded before
	// the picker opens, so every real container is a loaded class and reaches IsClassAllowed; an unloaded class is
	// not a container we can confirm here, so it is filtered out.
	class FCrowdyContainerClassFilter : public IClassViewerFilter
	{
	public:
		virtual bool IsClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const UClass* InClass,
			TSharedRef<FClassViewerFilterFuncs> InFilterFuncs) override
		{
			return IsCrowdyContainerClass(InClass);
		}

		virtual bool IsUnloadedClassAllowed(const FClassViewerInitializationOptions& InInitOptions,
			const TSharedRef<const IUnloadedBlueprintData> InUnloadedClassData,
			TSharedRef<FClassViewerFilterFuncs> InFilterFuncs) override
		{
			return false;
		}
	};
}

void FCrowdyEffectCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.Num() != 1)
	{
		return;
	}

	EditedEffect = Cast<UCrowdyEffect>(Objects[0].Get());
	if (!EditedEffect.IsValid())
	{
		return;
	}

	PropertyUtilities = DetailBuilder.GetPropertyUtilities();
	bRefreshRequested = false;

	const TSharedRef<IPropertyHandle> SourceHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffect, Source));
	const TSharedRef<IPropertyHandle> ContainerClassHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffect, ContainerClass));
	const TSharedRef<IPropertyHandle> FunctionNameHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffect, FunctionName));
	const TSharedRef<IPropertyHandle> EffectScriptHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffect, EffectScript));
	const TSharedRef<IPropertyHandle> MagnitudesHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffect, Magnitudes));
	const TSharedRef<IPropertyHandle> SourceContainerTypeHandle =
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffect, SourceContainerType));

	// ContainerClass / Source change the row SET, so they force a structural rebuild. FunctionName, EffectScript, and
	// Magnitudes only feed the missing-magnitudes list, so they get the cheap in-place refresh.
	SourceHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FCrowdyEffectCustomization::RequestStructuralRefresh));
	ContainerClassHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FCrowdyEffectCustomization::RequestStructuralRefresh));
	// The source container type changes which attributes a Source row offers, so it changes the row set the same way
	// the target class does.
	SourceContainerTypeHandle->SetOnPropertyValueChanged(
		FSimpleDelegate::CreateSP(this, &FCrowdyEffectCustomization::RequestStructuralRefresh));
	FunctionNameHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FCrowdyEffectCustomization::RefreshCompilePreview));
	EffectScriptHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FCrowdyEffectCustomization::RefreshCompilePreview));
	MagnitudesHandle->SetOnChildPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FCrowdyEffectCustomization::RefreshCompilePreview));

	const ECrowdyEffectSource CurrentSource = GetEnumHandleValue(SourceHandle.ToSharedPtr(), ECrowdyEffectSource::Text);

	// The Script/Graph segmented control below is the only authoring-mode switch, so hide the raw Source enum dropdown
	// (its default row would be a redundant second picker).
	DetailBuilder.HideProperty(SourceHandle);

	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(
		TEXT("Crowdy Effect"), FText::GetEmpty(), ECategoryPriority::Important);

	// The authoring-mode switch sits at the very top so the panel reads "how am I authoring this, then what".
	BuildAuthoringModeRow(Category, DetailBuilder, CurrentSource);

	BuildContainerClassRow(Category, DetailBuilder, ContainerClassHandle);

	// Directly under the target, so the panel reads "what this runs on, then what its source is".
	BuildSourceContainerTypeRow(Category, DetailBuilder, SourceContainerTypeHandle);

	// The body is edited in its own surface, not inline here: the EffectScript body in the dedicated Script editor
	// window (its canvas), and a node graph in its own toolkit. Either way this panel only carries the switch, the
	// container, and the tuning values, so hide the default multi-line property box for Text.
	if (CurrentSource == ECrowdyEffectSource::Text)
	{
		DetailBuilder.HideProperty(EffectScriptHandle);
	}

	// Tuning parameters balance every authoring surface, so the list shows in all modes.
	BuildTuningParametersSection(Category, DetailBuilder, MagnitudesHandle);

	// Both the Script and Graph toolkits dock their own live compile readout directly under their canvas, so this
	// panel does not duplicate one.

	// The Automation category otherwise stays entirely default-generated; only its three free-text
	// property/container-type fields get a picker, left in their normal declared position.
	BuildAutomationSection(DetailBuilder);

	RefreshCompilePreview();
}

void FCrowdyEffectCustomization::BuildAuthoringModeRow(
	IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder, ECrowdyEffectSource CurrentSource)
{
	Category.AddCustomRow(LOCTEXT("AuthoringModeFilter", "Authoring Mode"))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("AuthoringModeLabel", "Authoring Mode"))
		.ToolTipText(LOCTEXT("AuthoringModeTip",
			"How this effect is authored. Script edits the EffectScript body here; Graph edits a node graph in its own "
			"window. Both compile to the same function. Switching reopens the asset in the right editor."))
		.Font(DetailBuilder.GetDetailFontBold())
	]
	.ValueContent()
	.MinDesiredWidth(200.f)
	[
		SNew(SSegmentedControl<ECrowdyEffectSource>)
		.Value(CurrentSource)
		.OnValueChanged(this, &FCrowdyEffectCustomization::SwitchAuthoringMode)
		+ SSegmentedControl<ECrowdyEffectSource>::Slot(ECrowdyEffectSource::Text)
			.Text(LOCTEXT("ModeScript", "Script"))
			.ToolTip(LOCTEXT("ModeScriptTip", "Edit the EffectScript body as text"))
		+ SSegmentedControl<ECrowdyEffectSource>::Slot(ECrowdyEffectSource::Graph)
			.Text(LOCTEXT("ModeGraph", "Graph"))
			.ToolTip(LOCTEXT("ModeGraphTip", "Edit as a node graph in the graph editor"))
	];
}

void FCrowdyEffectCustomization::SwitchAuthoringMode(ECrowdyEffectSource NewSource)
{
	// Nothing to do if we are already in the requested mode. This guards against a redundant reopen (and the source
	// control churn / dirty it would cause) if the segmented control ever reports the current value.
	if (!EditedEffect.IsValid() || EditedEffect->Source == NewSource)
	{
		return;
	}

	// Reopening the asset editor tears down this very Details panel (the click that got us here lives inside it), so
	// defer the mode change + reopen to the next tick, off the Slate callstack. The ticker captures only the effect
	// (a weak pointer), never this customization, so it is safe even after the panel is destroyed.
	TWeakObjectPtr<UCrowdyEffect> EffectPtr = EditedEffect;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[EffectPtr, NewSource](float) -> bool
		{
			UCrowdyEffect* Effect = EffectPtr.Get();

			// Skip entirely if the effect is gone or already in the target mode (a second ticker stacked by rapid
			// toggling), so the asset is dirtied and the editor reopened at most once per real change.
			if (!Effect || Effect->Source == NewSource)
			{
				return false;
			}

			{
				const FScopedTransaction Transaction(LOCTEXT("SwitchAuthoringModeTx", "Switch Effect Authoring Mode"));
				Effect->Modify();
				Effect->Source = NewSource;
			}

			if (GEditor)
			{
				if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					AssetEditorSubsystem->CloseAllEditorsForAsset(Effect);
					AssetEditorSubsystem->OpenEditorForAsset(Effect);
				}
			}
			return false;
		}), 0.0f);
}

void FCrowdyEffectCustomization::BuildTuningParametersSection(
	IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder, TSharedRef<IPropertyHandle> MagnitudesHandle)
{
	// Re-added at this position so the list sits after the body and before the missing-parameters helper; its element
	// type customization is untouched. The array labels itself ("Tuning Parameters"), so it carries no separate
	// heading row above it.
	DetailBuilder.HideProperty(MagnitudesHandle);
	Category.AddProperty(MagnitudesHandle);

	BuildAddMissingTuningParametersRow(Category, DetailBuilder, MagnitudesHandle);
}

void FCrowdyEffectCustomization::BuildAutomationSection(IDetailLayoutBuilder& DetailBuilder)
{
	const TSharedRef<IPropertyHandle> ChangePropertyKeyHandle =
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffect, AutomationChangePropertyKey));
	BuildAutomationPropertyKeyRow(DetailBuilder, ChangePropertyKeyHandle);

	const TSharedRef<IPropertyHandle> ChangeContainerTypeHandle =
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffect, AutomationChangeContainerType));
	BuildAutomationContainerTypeRow(DetailBuilder, ChangeContainerTypeHandle);

	const TSharedRef<IPropertyHandle> TargetTypeOverrideHandle =
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UCrowdyEffect, AutomationTargetTypeOverride));
	BuildAutomationContainerTypeRow(DetailBuilder, TargetTypeOverrideHandle);
}

void FCrowdyEffectCustomization::BuildAutomationPropertyKeyRow(IDetailLayoutBuilder& DetailBuilder, TSharedRef<IPropertyHandle> Handle)
{
	// AddProperty (with the default EPropertyLocation) claims the property's existing row in place rather than
	// moving it, so it keeps its declared position among the category's other (still default-generated)
	// properties, and CustomWidget() only swaps the name/value content; the row's EditConditionHides visibility
	// still comes straight from the property's own metadata either way.
	IDetailPropertyRow& Row = DetailBuilder.EditCategory(TEXT("Automation")).AddProperty(Handle);
	Row.CustomWidget()
	.NameContent()
	[
		Handle->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(250.f)
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.VAlign(VAlign_Center)
		[
			Handle->CreatePropertyValueWidget()
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.f, 0.f, 0.f, 0.f)
		[
			SNew(SComboButton)
			.OnGetMenuContent_Lambda([this, Handle]() { return BuildAutomationPropertyKeyPickerMenu(Handle); })
			.ToolTipText(LOCTEXT("PickAutoPropertyKeyTip",
				"Pick a Server Owned attribute of the target container class. This writes the attribute's server "
				"key (not its property name), since the automation trigger matches the server key directly."))
			.ButtonContent()
			[
				SNew(STextBlock).Text(LOCTEXT("PickAttribute2", "Pick"))
			]
		]
	];
}

TSharedRef<SWidget> FCrowdyEffectCustomization::BuildAutomationPropertyKeyPickerMenu(TSharedRef<IPropertyHandle> Handle)
{
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

	const UClass* Class = ResolveContainerClass();
	if (!Class)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("PickAutoPropertyKeyNoClass", "Set a container class first"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
		return MenuBuilder.MakeWidget();
	}

	const TArray<FCrowdyAttributeDef> Attributes = CrowdyEffectPickerOptions::AttributesForClass(Class);
	if (Attributes.IsEmpty())
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("PickAutoPropertyKeyNone", "No Server Owned attributes found on this class"), FText::GetEmpty(),
			FSlateIcon(), FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
		return MenuBuilder.MakeWidget();
	}

	MenuBuilder.BeginSection(NAME_None, LOCTEXT("PickAutoPropertyKeySection", "Attributes"));
	for (const FCrowdyAttributeDef& Def : Attributes)
	{
		const FString Key = CrowdyEffectPickerOptions::PropertyKeyForAutomationTrigger(Def);
		MenuBuilder.AddMenuEntry(
			FText::FromString(FString::Printf(TEXT("%s (%s)"), *Def.PropertyName.ToString(), *Key)),
			FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Handle, Key]()
			{
				Handle->SetValue(Key);
			})));
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

void FCrowdyEffectCustomization::BuildAutomationContainerTypeRow(IDetailLayoutBuilder& DetailBuilder, TSharedRef<IPropertyHandle> Handle)
{
	IDetailPropertyRow& Row = DetailBuilder.EditCategory(TEXT("Automation")).AddProperty(Handle);
	Row.CustomWidget()
	.NameContent()
	[
		Handle->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(250.f)
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.VAlign(VAlign_Center)
		[
			Handle->CreatePropertyValueWidget()
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.f, 0.f, 0.f, 0.f)
		[
			SNew(SComboButton)
			.OnGetMenuContent_Lambda([this, Handle]() { return BuildContainerTypePickerMenu(Handle); })
			.ToolTipText(LOCTEXT("PickContainerTypeTip", "Pick a known Game Model container type name"))
			.ButtonContent()
			[
				SNew(STextBlock).Text(LOCTEXT("PickContainerType", "Pick"))
			]
		]
	];
}

TSharedRef<SWidget> FCrowdyEffectCustomization::BuildContainerTypePickerMenu(TSharedRef<IPropertyHandle> Handle)
{
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

	const TArray<FString> TypeNames = CrowdyEffectPickerOptions::KnownContainerTypeNames();
	if (TypeNames.IsEmpty())
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("PickContainerTypeNone", "No container types found"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
		return MenuBuilder.MakeWidget();
	}

	MenuBuilder.BeginSection(NAME_None, LOCTEXT("PickContainerTypeSection", "Container Types"));
	for (const FString& TypeName : TypeNames)
	{
		MenuBuilder.AddMenuEntry(
			FText::FromString(TypeName), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Handle, TypeName]()
			{
				Handle->SetValue(TypeName);
			})));
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

void FCrowdyEffectCustomization::BuildContainerClassRow(
	IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder, TSharedRef<IPropertyHandle> ContainerClassHandle)
{
	// Force-load the marked container assets once per session so an unopened container Blueprint is a loaded class
	// the filter accepts. LoadAllTaggedAssets blocks on the asset-registry scan and force-loads every tagged asset
	// (the baker documents it as an editor freeze), so it must not run on every panel build (CustomizeDetails re-runs
	// on each structural refresh, undo/redo, etc.); a file-static guard bounds it to the first effect opened this
	// session. A container marked/created later this session is picked up on the next editor run - a rare case, and
	// the compile preview still validates whatever is selected.
	static bool bLoadedTaggedContainersThisSession = false;
	if (!bLoadedTaggedContainersThisSession)
	{
		UCrowdyRegistryBaker::LoadAllTaggedAssets();
		bLoadedTaggedContainersThisSession = true;
	}

	DetailBuilder.HideProperty(ContainerClassHandle);

	const TSharedPtr<IPropertyHandle> HandlePtr = ContainerClassHandle.ToSharedPtr();
	TArray<TSharedRef<IClassViewerFilter>> Filters;
	Filters.Add(MakeShared<FCrowdyContainerClassFilter>());

	Category.AddCustomRow(LOCTEXT("ContainerClassFilter", "Container Class"))
	.NameContent()
	[
		ContainerClassHandle->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(250.f)
	[
		SNew(SClassPropertyEntryBox)
		.MetaClass(UObject::StaticClass())
		.AllowAbstract(false)
		.AllowNone(true)
		.ShowTreeView(false)
		.ClassViewerFilters(Filters)
		.SelectedClass_Lambda([HandlePtr]() -> const UClass*
		{
			FString ClassPath;
			if (HandlePtr.IsValid())
			{
				HandlePtr->GetValueAsFormattedString(ClassPath);
			}
			return FEditorClassUtils::GetClassFromString(ClassPath);
		})
		.OnSetClass_Lambda([HandlePtr](const UClass* NewClass)
		{
			if (HandlePtr.IsValid())
			{
				HandlePtr->SetValueFromFormattedString(NewClass ? NewClass->GetPathName() : TEXT("None"));
			}
		})
	];
}

void FCrowdyEffectCustomization::BuildSourceContainerTypeRow(
	IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder,
	TSharedRef<IPropertyHandle> SourceContainerTypeHandle)
{
	// The property holds a server container type name, which the compile matches exactly, so the panel offers the
	// declared types instead of a text box: a typed name that matches none of them compiles to an error, and there is
	// nothing an author can type that the list does not already contain.
	DetailBuilder.HideProperty(SourceContainerTypeHandle);

	const TSharedPtr<IPropertyHandle> HandlePtr = SourceContainerTypeHandle.ToSharedPtr();

	Category.AddCustomRow(LOCTEXT("SourceContainerTypeFilter", "Source Container Type"))
	.NameContent()
	[
		SourceContainerTypeHandle->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(250.f)
	[
		SNew(SComboButton)
		.OnGetMenuContent_Lambda([this, SourceContainerTypeHandle]()
		{
			return BuildSourceContainerTypeMenu(SourceContainerTypeHandle);
		})
		.ToolTipText(LOCTEXT("SourceContainerTypeTip",
			"The container type this effect's Source is, when the source is a different kind of container from the "
			"target. Source attributes are then checked against that type. Leave it on 'Same as target' when the "
			"source is another container of the target's own type."))
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text_Lambda([this, HandlePtr]() { return GetSourceContainerTypeLabel(HandlePtr); })
		]
	];
}

TSharedRef<SWidget> FCrowdyEffectCustomization::BuildSourceContainerTypeMenu(TSharedRef<IPropertyHandle> Handle)
{
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

	// The empty value is a real choice, not an unset one, so it is the first entry and says what it means: the source
	// is another container of the target's own type, and its attributes are the target's.
	MenuBuilder.BeginSection(NAME_None, LOCTEXT("SourceContainerTypeDefaultSection", "Source Container Type"));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("SourceContainerTypeSameAsTarget", "Same as target"),
		LOCTEXT("SourceContainerTypeSameAsTargetTip",
			"The source is another container of this effect's own type, so source attributes are the target's."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Handle]()
		{
			Handle->SetValue(FString());
		})));
	MenuBuilder.EndSection();

	const TArray<FString> TypeNames = CrowdyEffectPickerOptions::KnownContainerTypeNames();
	if (TypeNames.IsEmpty())
	{
		return MenuBuilder.MakeWidget();
	}

	MenuBuilder.BeginSection(NAME_None, LOCTEXT("SourceContainerTypeSection", "Container Types"));
	for (const FString& TypeName : TypeNames)
	{
		MenuBuilder.AddMenuEntry(
			FText::FromString(TypeName), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Handle, TypeName]()
			{
				Handle->SetValue(TypeName);
			})));
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

FText FCrowdyEffectCustomization::GetSourceContainerTypeLabel(TSharedPtr<IPropertyHandle> Handle) const
{
	// Reads the value only: this runs on every paint, and resolving a type name to a class walks every loaded class.
	// A name the project no longer declares is shown as authored rather than blanked, and the compile readout is what
	// reports it, so the panel never hides a value the author is the only one able to correct.
	const FString Value = GetStringHandleValue(Handle).TrimStartAndEnd();
	return Value.IsEmpty()
		? LOCTEXT("SourceContainerTypeSameAsTargetLabel", "Same as target")
		: FText::FromString(Value);
}

void FCrowdyEffectCustomization::BuildAddMissingTuningParametersRow(
	IDetailCategoryBuilder& Category, IDetailLayoutBuilder& DetailBuilder, TSharedRef<IPropertyHandle> MagnitudesHandle)
{
	Category.AddCustomRow(LOCTEXT("AddMissingFilter", "Add Missing Tuning Parameters"))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("AddMissingLabel", "Missing Parameters"))
		.ToolTipText(LOCTEXT("AddMissingTip",
			"Declares any parameter the effect references but has not yet exposed (added as int, required). Set each "
			"one's type and default afterwards."))
		.Font(DetailBuilder.GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(220.f)
	[
		SNew(SButton)
		.HAlign(HAlign_Center)
		.IsEnabled_Lambda([this]() { return CachedMissingParams.Num() > 0; })
		.ToolTipText(LOCTEXT("AddMissingButtonTip", "Expose the parameters this effect references but has not declared"))
		.OnClicked_Lambda([this, MagnitudesHandle]()
		{
			AddMissingMagnitudes(MagnitudesHandle);
			return FReply::Handled();
		})
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				const int32 Num = CachedMissingParams.Num();
				return Num > 0
					? FText::Format(LOCTEXT("AddMissingN", "Add {0} missing parameter(s)"), FText::AsNumber(Num))
					: LOCTEXT("AddMissingNone", "No missing parameters");
			})
		]
	];
}

TArray<FString> FCrowdyEffectCustomization::ComputeMissingMagnitudes() const
{
	return CrowdyEffectCompileSummary::MissingMagnitudes(EditedEffect.Get());
}

void FCrowdyEffectCustomization::AddMissingMagnitudes(TSharedRef<IPropertyHandle> MagnitudesHandle)
{
	const TArray<FString> Missing = ComputeMissingMagnitudes();
	if (Missing.IsEmpty() || !EditedEffect.IsValid())
	{
		return;
	}
	UCrowdyEffect* Effect = EditedEffect.Get();

	// Mutate the array directly under a transaction and notify through the handle (so it transacts + dirties like any
	// Details edit), rather than looping AddItem() which could rebuild the panel mid-loop and invalidate handles. A
	// new magnitude keeps the struct default type "int" and an empty default (so it reads as required). Several
	// elements may be appended, so notify a bulk change (Unspecified) rather than ArrayAdd's single-append semantics;
	// the trailing structural refresh redraws the rows regardless.
	FScopedTransaction Transaction(LOCTEXT("AddMissingMagnitudesTx", "Add Missing Tuning Parameters"));
	MagnitudesHandle->NotifyPreChange();
	for (const FString& Name : Missing)
	{
		FCrowdyEffectMagnitude Magnitude;
		Magnitude.Name = Name;
		Effect->Magnitudes.Add(MoveTemp(Magnitude));
	}
	MagnitudesHandle->NotifyPostChange(EPropertyChangeType::Unspecified);
	MagnitudesHandle->NotifyFinishedChangingProperties();

	RequestStructuralRefresh();
}

UClass* FCrowdyEffectCustomization::ResolveContainerClass() const
{
	return EditedEffect.IsValid() ? EditedEffect->ContainerClass.LoadSynchronous() : nullptr;
}

void FCrowdyEffectCustomization::RequestStructuralRefresh()
{
	if (bRefreshRequested || !PropertyUtilities.IsValid())
	{
		return;
	}
	bRefreshRequested = true;
	PropertyUtilities->RequestForceRefresh();
}

void FCrowdyEffectCustomization::RefreshCompilePreview()
{
	// Both toolkits (Script and Graph) dock their own live compile readout; this panel only needs the missing-
	// parameters list, so it does not compile the effect at all when nothing here is editing that.
	CachedMissingParams = EditedEffect.IsValid() ? ComputeMissingMagnitudes() : TArray<FString>();
}

#undef LOCTEXT_NAMESPACE
