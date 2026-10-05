#include "CrowdyServerFunctionCustomization.h"

#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailGroup.h"
#include "IDetailPropertyRow.h"
#include "PropertyHandle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyServerFunctionCustomization"

namespace CrowdyServerFunctionCustomizationDetail
{
	TSharedRef<SWidget> Labelled(const FText& Label, const FText& ToolTip, const TSharedRef<SWidget>& Value)
	{
		return SNew(SHorizontalBox)
			.ToolTipText(ToolTip)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				Value
			];
	}
}

TSharedRef<IPropertyTypeCustomization> FCrowdyServerFunctionCustomization::MakeInstance()
{
	return MakeShared<FCrowdyServerFunctionCustomization>();
}

void FCrowdyServerFunctionCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& Utils)
{
	using namespace CrowdyServerFunctionCustomizationDetail;
	const TSharedPtr<IPropertyHandle> NameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, Name));
	const TSharedPtr<IPropertyHandle> CallerHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, WhoCanCall));
	if (!NameHandle.IsValid() || !CallerHandle.IsValid())
	{
		HeaderRow.NameContent()
		[
			PropertyHandle->CreatePropertyNameWidget()
		];
		return;
	}
	HeaderRow
		.NameContent()
		.MinDesiredWidth(180.0f)
		[
			Labelled(LOCTEXT("FunctionLabel", "Function"),
				LOCTEXT("FunctionToolTip", "The function's name, used to call it from Blueprint and C++ (the server's method name is made from it)"),
				NameHandle->CreatePropertyValueWidget())
		]
		.ValueContent()
		.MinDesiredWidth(200.0f)
		[
			Labelled(LOCTEXT("CallerLabel", "Callable By"),
				LOCTEXT("CallerToolTip", "Players: any player. Members or Leader: only the object's members or its leader (needs Members From). Server Only: only other server code and developer tools."),
				CallerHandle->CreatePropertyValueWidget())
		];
}

void FCrowdyServerFunctionCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& Utils)
{
	const FName Rows[] = {
		GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, CooldownSeconds),
		GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ParamsForm),
		GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, Params),
		GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ParamsList),
		GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ReplyForm),
		GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, Reply),
		GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ReplyList)};
	for (const FName Row : Rows)
	{
		const TSharedPtr<IPropertyHandle> Handle = PropertyHandle->GetChildHandle(Row);
		if (!Handle.IsValid())
		{
			continue;
		}
		IDetailPropertyRow& Added = ChildBuilder.AddProperty(Handle.ToSharedRef());
		if (Row == GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, CooldownSeconds))
		{
			CrowdyServerObjectRows::ShowSeconds(Added, Handle.ToSharedRef());
		}
		else if (Row == GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ParamsForm))
		{
			CrowdyServerObjectRows::ShowUseStruct(Added, Handle.ToSharedRef(), LOCTEXT("InputsUseStruct", "Use Struct for Inputs"));
		}
		else if (Row == GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ReplyForm))
		{
			CrowdyServerObjectRows::ShowUseStruct(Added, Handle.ToSharedRef(), LOCTEXT("OutputsUseStruct", "Use Struct for Outputs"));
		}
	}
	const TSharedPtr<IPropertyHandle> ServerNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerFunction, ServerName));
	if (!ServerNameHandle.IsValid())
	{
		return;
	}
	IDetailGroup& Advanced = ChildBuilder.AddGroup(TEXT("Advanced"), LOCTEXT("AdvancedGroup", "Advanced"));
	Advanced.AddPropertyRow(ServerNameHandle.ToSharedRef());
}

#undef LOCTEXT_NAMESPACE
