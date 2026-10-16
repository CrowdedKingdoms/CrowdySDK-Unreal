#include "CrowdyServerMessageCustomization.h"

#include "CrowdyServerFunctionCustomization.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailGroup.h"
#include "IDetailPropertyRow.h"
#include "PropertyHandle.h"

#define LOCTEXT_NAMESPACE "CrowdyServerMessageCustomization"

TSharedRef<IPropertyTypeCustomization> FCrowdyServerMessageCustomization::MakeInstance()
{
	return MakeShared<FCrowdyServerMessageCustomization>();
}

void FCrowdyServerMessageCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& Utils)
{
	const TSharedPtr<IPropertyHandle> NameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerMessage, Name));
	if (!NameHandle.IsValid())
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
			CrowdyServerMemberRows::Labelled(LOCTEXT("MessageLabel", "Message"),
				LOCTEXT("MessageToolTip", "The message's name, used to listen for it from Blueprint and C++ (the server's topic is made from it)"),
				NameHandle->CreatePropertyValueWidget())
		];
}

void FCrowdyServerMessageCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& Utils)
{
	const FName Rows[] = {
		GET_MEMBER_NAME_CHECKED(FCrowdyServerMessage, bPlayersCanListen),
		GET_MEMBER_NAME_CHECKED(FCrowdyServerMessage, PayloadForm),
		GET_MEMBER_NAME_CHECKED(FCrowdyServerMessage, Payload),
		GET_MEMBER_NAME_CHECKED(FCrowdyServerMessage, PayloadList)};
	for (const FName Row : Rows)
	{
		const TSharedPtr<IPropertyHandle> Handle = PropertyHandle->GetChildHandle(Row);
		if (!Handle.IsValid())
		{
			continue;
		}
		IDetailPropertyRow& Added = ChildBuilder.AddProperty(Handle.ToSharedRef());
		if (Row == GET_MEMBER_NAME_CHECKED(FCrowdyServerMessage, PayloadForm))
		{
			CrowdyServerObjectRows::ShowUseStruct(Added, Handle.ToSharedRef(), LOCTEXT("PayloadUseStruct", "Use Struct for Payload"));
		}
	}
	const TSharedPtr<IPropertyHandle> ServerNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FCrowdyServerMessage, ServerName));
	if (!ServerNameHandle.IsValid())
	{
		return;
	}
	IDetailGroup& Advanced = ChildBuilder.AddGroup(TEXT("Advanced"), LOCTEXT("AdvancedGroup", "Advanced"));
	Advanced.AddPropertyRow(ServerNameHandle.ToSharedRef());
}

#undef LOCTEXT_NAMESPACE
