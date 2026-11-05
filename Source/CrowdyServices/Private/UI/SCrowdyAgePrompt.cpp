// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyAgePrompt.h"

#include "GenericPlatform/ICursor.h"
#include "Internationalization/Text.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyBrandStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyAgePrompt"

namespace
{
	struct FCountry
	{
		const TCHAR* Code;
		const TCHAR* Name;
	};

	// ISO 3166-1, by name. Countries the platform does not serve stay listed: the refusal must come from the server.
	const FCountry Countries[] =
	{
		{TEXT("AF"), TEXT("Afghanistan")}, {TEXT("AX"), TEXT("Aland Islands")}, {TEXT("AL"), TEXT("Albania")},
		{TEXT("DZ"), TEXT("Algeria")}, {TEXT("AS"), TEXT("American Samoa")}, {TEXT("AD"), TEXT("Andorra")},
		{TEXT("AO"), TEXT("Angola")}, {TEXT("AI"), TEXT("Anguilla")}, {TEXT("AQ"), TEXT("Antarctica")},
		{TEXT("AG"), TEXT("Antigua and Barbuda")}, {TEXT("AR"), TEXT("Argentina")}, {TEXT("AM"), TEXT("Armenia")},
		{TEXT("AW"), TEXT("Aruba")}, {TEXT("AU"), TEXT("Australia")}, {TEXT("AT"), TEXT("Austria")},
		{TEXT("AZ"), TEXT("Azerbaijan")}, {TEXT("BS"), TEXT("Bahamas")}, {TEXT("BH"), TEXT("Bahrain")},
		{TEXT("BD"), TEXT("Bangladesh")}, {TEXT("BB"), TEXT("Barbados")}, {TEXT("BY"), TEXT("Belarus")},
		{TEXT("BE"), TEXT("Belgium")}, {TEXT("BZ"), TEXT("Belize")}, {TEXT("BJ"), TEXT("Benin")},
		{TEXT("BM"), TEXT("Bermuda")}, {TEXT("BT"), TEXT("Bhutan")}, {TEXT("BO"), TEXT("Bolivia")},
		{TEXT("BQ"), TEXT("Bonaire, Sint Eustatius and Saba")}, {TEXT("BA"), TEXT("Bosnia and Herzegovina")},
		{TEXT("BW"), TEXT("Botswana")}, {TEXT("BV"), TEXT("Bouvet Island")}, {TEXT("BR"), TEXT("Brazil")},
		{TEXT("IO"), TEXT("British Indian Ocean Territory")}, {TEXT("BN"), TEXT("Brunei")}, {TEXT("BG"), TEXT("Bulgaria")},
		{TEXT("BF"), TEXT("Burkina Faso")}, {TEXT("BI"), TEXT("Burundi")}, {TEXT("CV"), TEXT("Cabo Verde")},
		{TEXT("KH"), TEXT("Cambodia")}, {TEXT("CM"), TEXT("Cameroon")}, {TEXT("CA"), TEXT("Canada")},
		{TEXT("KY"), TEXT("Cayman Islands")}, {TEXT("CF"), TEXT("Central African Republic")}, {TEXT("TD"), TEXT("Chad")},
		{TEXT("CL"), TEXT("Chile")}, {TEXT("CN"), TEXT("China")}, {TEXT("CX"), TEXT("Christmas Island")},
		{TEXT("CC"), TEXT("Cocos (Keeling) Islands")}, {TEXT("CO"), TEXT("Colombia")}, {TEXT("KM"), TEXT("Comoros")},
		{TEXT("CG"), TEXT("Congo")}, {TEXT("CD"), TEXT("Congo, Democratic Republic of the")}, {TEXT("CK"), TEXT("Cook Islands")},
		{TEXT("CR"), TEXT("Costa Rica")}, {TEXT("CI"), TEXT("Cote d'Ivoire")}, {TEXT("HR"), TEXT("Croatia")},
		{TEXT("CU"), TEXT("Cuba")}, {TEXT("CW"), TEXT("Curacao")}, {TEXT("CY"), TEXT("Cyprus")},
		{TEXT("CZ"), TEXT("Czechia")}, {TEXT("DK"), TEXT("Denmark")}, {TEXT("DJ"), TEXT("Djibouti")},
		{TEXT("DM"), TEXT("Dominica")}, {TEXT("DO"), TEXT("Dominican Republic")}, {TEXT("EC"), TEXT("Ecuador")},
		{TEXT("EG"), TEXT("Egypt")}, {TEXT("SV"), TEXT("El Salvador")}, {TEXT("GQ"), TEXT("Equatorial Guinea")},
		{TEXT("ER"), TEXT("Eritrea")}, {TEXT("EE"), TEXT("Estonia")}, {TEXT("SZ"), TEXT("Eswatini")},
		{TEXT("ET"), TEXT("Ethiopia")}, {TEXT("FK"), TEXT("Falkland Islands")}, {TEXT("FO"), TEXT("Faroe Islands")},
		{TEXT("FJ"), TEXT("Fiji")}, {TEXT("FI"), TEXT("Finland")}, {TEXT("FR"), TEXT("France")},
		{TEXT("GF"), TEXT("French Guiana")}, {TEXT("PF"), TEXT("French Polynesia")}, {TEXT("TF"), TEXT("French Southern Territories")},
		{TEXT("GA"), TEXT("Gabon")}, {TEXT("GM"), TEXT("Gambia")}, {TEXT("GE"), TEXT("Georgia")},
		{TEXT("DE"), TEXT("Germany")}, {TEXT("GH"), TEXT("Ghana")}, {TEXT("GI"), TEXT("Gibraltar")},
		{TEXT("GR"), TEXT("Greece")}, {TEXT("GL"), TEXT("Greenland")}, {TEXT("GD"), TEXT("Grenada")},
		{TEXT("GP"), TEXT("Guadeloupe")}, {TEXT("GU"), TEXT("Guam")}, {TEXT("GT"), TEXT("Guatemala")},
		{TEXT("GG"), TEXT("Guernsey")}, {TEXT("GN"), TEXT("Guinea")}, {TEXT("GW"), TEXT("Guinea-Bissau")},
		{TEXT("GY"), TEXT("Guyana")}, {TEXT("HT"), TEXT("Haiti")}, {TEXT("HM"), TEXT("Heard Island and McDonald Islands")},
		{TEXT("VA"), TEXT("Holy See")}, {TEXT("HN"), TEXT("Honduras")}, {TEXT("HK"), TEXT("Hong Kong")},
		{TEXT("HU"), TEXT("Hungary")}, {TEXT("IS"), TEXT("Iceland")}, {TEXT("IN"), TEXT("India")},
		{TEXT("ID"), TEXT("Indonesia")}, {TEXT("IR"), TEXT("Iran")}, {TEXT("IQ"), TEXT("Iraq")},
		{TEXT("IE"), TEXT("Ireland")}, {TEXT("IM"), TEXT("Isle of Man")}, {TEXT("IL"), TEXT("Israel")},
		{TEXT("IT"), TEXT("Italy")}, {TEXT("JM"), TEXT("Jamaica")}, {TEXT("JP"), TEXT("Japan")},
		{TEXT("JE"), TEXT("Jersey")}, {TEXT("JO"), TEXT("Jordan")}, {TEXT("KZ"), TEXT("Kazakhstan")},
		{TEXT("KE"), TEXT("Kenya")}, {TEXT("KI"), TEXT("Kiribati")}, {TEXT("KP"), TEXT("Korea, North")},
		{TEXT("KR"), TEXT("Korea, South")}, {TEXT("KW"), TEXT("Kuwait")}, {TEXT("KG"), TEXT("Kyrgyzstan")},
		{TEXT("LA"), TEXT("Laos")}, {TEXT("LV"), TEXT("Latvia")}, {TEXT("LB"), TEXT("Lebanon")},
		{TEXT("LS"), TEXT("Lesotho")}, {TEXT("LR"), TEXT("Liberia")}, {TEXT("LY"), TEXT("Libya")},
		{TEXT("LI"), TEXT("Liechtenstein")}, {TEXT("LT"), TEXT("Lithuania")}, {TEXT("LU"), TEXT("Luxembourg")},
		{TEXT("MO"), TEXT("Macao")}, {TEXT("MG"), TEXT("Madagascar")}, {TEXT("MW"), TEXT("Malawi")},
		{TEXT("MY"), TEXT("Malaysia")}, {TEXT("MV"), TEXT("Maldives")}, {TEXT("ML"), TEXT("Mali")},
		{TEXT("MT"), TEXT("Malta")}, {TEXT("MH"), TEXT("Marshall Islands")}, {TEXT("MQ"), TEXT("Martinique")},
		{TEXT("MR"), TEXT("Mauritania")}, {TEXT("MU"), TEXT("Mauritius")}, {TEXT("YT"), TEXT("Mayotte")},
		{TEXT("MX"), TEXT("Mexico")}, {TEXT("FM"), TEXT("Micronesia")}, {TEXT("MD"), TEXT("Moldova")},
		{TEXT("MC"), TEXT("Monaco")}, {TEXT("MN"), TEXT("Mongolia")}, {TEXT("ME"), TEXT("Montenegro")},
		{TEXT("MS"), TEXT("Montserrat")}, {TEXT("MA"), TEXT("Morocco")}, {TEXT("MZ"), TEXT("Mozambique")},
		{TEXT("MM"), TEXT("Myanmar")}, {TEXT("NA"), TEXT("Namibia")}, {TEXT("NR"), TEXT("Nauru")},
		{TEXT("NP"), TEXT("Nepal")}, {TEXT("NL"), TEXT("Netherlands")}, {TEXT("NC"), TEXT("New Caledonia")},
		{TEXT("NZ"), TEXT("New Zealand")}, {TEXT("NI"), TEXT("Nicaragua")}, {TEXT("NE"), TEXT("Niger")},
		{TEXT("NG"), TEXT("Nigeria")}, {TEXT("NU"), TEXT("Niue")}, {TEXT("NF"), TEXT("Norfolk Island")},
		{TEXT("MK"), TEXT("North Macedonia")}, {TEXT("MP"), TEXT("Northern Mariana Islands")}, {TEXT("NO"), TEXT("Norway")},
		{TEXT("OM"), TEXT("Oman")}, {TEXT("PK"), TEXT("Pakistan")}, {TEXT("PW"), TEXT("Palau")},
		{TEXT("PS"), TEXT("Palestine")}, {TEXT("PA"), TEXT("Panama")}, {TEXT("PG"), TEXT("Papua New Guinea")},
		{TEXT("PY"), TEXT("Paraguay")}, {TEXT("PE"), TEXT("Peru")}, {TEXT("PH"), TEXT("Philippines")},
		{TEXT("PN"), TEXT("Pitcairn")}, {TEXT("PL"), TEXT("Poland")}, {TEXT("PT"), TEXT("Portugal")},
		{TEXT("PR"), TEXT("Puerto Rico")}, {TEXT("QA"), TEXT("Qatar")}, {TEXT("RE"), TEXT("Reunion")},
		{TEXT("RO"), TEXT("Romania")}, {TEXT("RU"), TEXT("Russia")}, {TEXT("RW"), TEXT("Rwanda")},
		{TEXT("BL"), TEXT("Saint Barthelemy")}, {TEXT("SH"), TEXT("Saint Helena")}, {TEXT("KN"), TEXT("Saint Kitts and Nevis")},
		{TEXT("LC"), TEXT("Saint Lucia")}, {TEXT("MF"), TEXT("Saint Martin")}, {TEXT("PM"), TEXT("Saint Pierre and Miquelon")},
		{TEXT("VC"), TEXT("Saint Vincent and the Grenadines")}, {TEXT("WS"), TEXT("Samoa")}, {TEXT("SM"), TEXT("San Marino")},
		{TEXT("ST"), TEXT("Sao Tome and Principe")}, {TEXT("SA"), TEXT("Saudi Arabia")}, {TEXT("SN"), TEXT("Senegal")},
		{TEXT("RS"), TEXT("Serbia")}, {TEXT("SC"), TEXT("Seychelles")}, {TEXT("SL"), TEXT("Sierra Leone")},
		{TEXT("SG"), TEXT("Singapore")}, {TEXT("SX"), TEXT("Sint Maarten")}, {TEXT("SK"), TEXT("Slovakia")},
		{TEXT("SI"), TEXT("Slovenia")}, {TEXT("SB"), TEXT("Solomon Islands")}, {TEXT("SO"), TEXT("Somalia")},
		{TEXT("ZA"), TEXT("South Africa")}, {TEXT("GS"), TEXT("South Georgia and the South Sandwich Islands")}, {TEXT("SS"), TEXT("South Sudan")},
		{TEXT("ES"), TEXT("Spain")}, {TEXT("LK"), TEXT("Sri Lanka")}, {TEXT("SD"), TEXT("Sudan")},
		{TEXT("SR"), TEXT("Suriname")}, {TEXT("SJ"), TEXT("Svalbard and Jan Mayen")}, {TEXT("SE"), TEXT("Sweden")},
		{TEXT("CH"), TEXT("Switzerland")}, {TEXT("SY"), TEXT("Syria")}, {TEXT("TW"), TEXT("Taiwan")},
		{TEXT("TJ"), TEXT("Tajikistan")}, {TEXT("TZ"), TEXT("Tanzania")}, {TEXT("TH"), TEXT("Thailand")},
		{TEXT("TL"), TEXT("Timor-Leste")}, {TEXT("TG"), TEXT("Togo")}, {TEXT("TK"), TEXT("Tokelau")},
		{TEXT("TO"), TEXT("Tonga")}, {TEXT("TT"), TEXT("Trinidad and Tobago")}, {TEXT("TN"), TEXT("Tunisia")},
		{TEXT("TR"), TEXT("Turkiye")}, {TEXT("TM"), TEXT("Turkmenistan")}, {TEXT("TC"), TEXT("Turks and Caicos Islands")},
		{TEXT("TV"), TEXT("Tuvalu")}, {TEXT("UG"), TEXT("Uganda")}, {TEXT("UA"), TEXT("Ukraine")},
		{TEXT("AE"), TEXT("United Arab Emirates")}, {TEXT("GB"), TEXT("United Kingdom")}, {TEXT("US"), TEXT("United States")},
		{TEXT("UM"), TEXT("United States Minor Outlying Islands")}, {TEXT("UY"), TEXT("Uruguay")}, {TEXT("UZ"), TEXT("Uzbekistan")},
		{TEXT("VU"), TEXT("Vanuatu")}, {TEXT("VE"), TEXT("Venezuela")}, {TEXT("VN"), TEXT("Viet Nam")},
		{TEXT("VG"), TEXT("Virgin Islands, British")}, {TEXT("VI"), TEXT("Virgin Islands, U.S.")}, {TEXT("WF"), TEXT("Wallis and Futuna")},
		{TEXT("EH"), TEXT("Western Sahara")}, {TEXT("YE"), TEXT("Yemen")}, {TEXT("ZM"), TEXT("Zambia")},
		{TEXT("ZW"), TEXT("Zimbabwe")},
	};

}

void SCrowdyAgePrompt::Construct(const FArguments& InArgs)
{
	OnContinueDelegate = InArgs._OnContinue;
	OnDeclineDelegate = InArgs._OnDecline;

	const ISlateStyle& Style = FCrowdyBrandStyle::Get();

	for (int32 Month = 1; Month <= 12; ++Month)
	{
		MonthOptions.Add(MakeShared<int32>(Month));
	}
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Countries); ++Index)
	{
		CountryOptions.Add(MakeShared<int32>(Index));
	}

	const TSharedRef<SWidget> MonthCombo = SNew(SComboBox<TSharedPtr<int32>>)
		.OptionsSource(&MonthOptions)
		.OnGenerateWidget_Lambda([](TSharedPtr<int32> Month)
		{
			return SNew(STextBlock).Text(FText::AsNumber(*Month));
		})
		.OnSelectionChanged_Lambda([this](TSharedPtr<int32> Month, ESelectInfo::Type)
		{
			BirthMonth = Month.IsValid() ? *Month : 0;
		})
		[
			SNew(STextBlock).Text_Lambda([this]()
			{
				return BirthMonth > 0 ? FText::AsNumber(BirthMonth) : LOCTEXT("MonthHint", "Month");
			})
		];
	FirstControl = MonthCombo;

	TSharedRef<SWidget> Card =
		SNew(SBorder)
		.BorderImage(Style.GetBrush("Crowdy.Card"))
		.Padding(FMargin(24.0f, 22.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("Title", "About you")).TextStyle(&Style, "Crowdy.Text.Title")
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([this]() { return bRefused ? EVisibility::Collapsed : EVisibility::Visible; })
				.IsEnabled_Lambda([this]() { return !bDateFixed; })
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("BirthLabel", "When were you born?")).TextStyle(&Style, "Crowdy.Text.Body")
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					MonthCombo
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SEditableTextBox)
					.HintText(LOCTEXT("YearHint", "Year"))
					.OnTextChanged_Lambda([this](const FText& Text) { YearText = Text.ToString(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("CountryLabel", "Where do you live?")).TextStyle(&Style, "Crowdy.Text.Body")
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(SComboBox<TSharedPtr<int32>>)
				.OptionsSource(&CountryOptions)
				.MaxListHeight(280.0f)
				.OnGenerateWidget_Lambda([](TSharedPtr<int32> Index)
				{
					return SNew(STextBlock).Text(FText::FromString(Countries[*Index].Name));
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<int32> Index, ESelectInfo::Type)
				{
					CountryCode = Index.IsValid() ? Countries[*Index].Code : TEXT("");
					CountryName = Index.IsValid() ? FText::FromString(Countries[*Index].Name) : FText::GetEmpty();
				})
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return CountryName.IsEmpty() ? LOCTEXT("CountryHint", "Country") : CountryName;
					})
				]
			]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(SCheckBox)
				.Style(&Style.GetWidgetStyle<FCheckBoxStyle>("Crowdy.CheckBox"))
				.Visibility_Lambda([this]() { return bDateFixed && !bRefused ? EVisibility::Visible : EVisibility::Collapsed; })
				.IsChecked_Lambda([this]() { return bParentalChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bParentalChecked = State == ECheckBoxState::Checked; })
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Parental", "A parent or guardian agreed to the terms for me."))
					.TextStyle(&Style, "Crowdy.Text.Body").WrapTextAt(396.0f)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]() { return ErrorText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text_Lambda([this]() { return ErrorText; })
				.TextStyle(&Style, "Crowdy.Text.Error")
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNullWidget::NullWidget
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Secondary")
					.ContentPadding(FMargin(16.0f, 8.0f))
					.IsEnabled_Lambda([this]() { return !bBusy; })
					.OnClicked_Lambda([this]() { Decline(); return FReply::Handled(); })
					[
						SNew(STextBlock).Text_Lambda([this]() { return bRefused ? LOCTEXT("Close", "Close") : LOCTEXT("Decline", "Not now"); })
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Primary")
					.ContentPadding(FMargin(22.0f, 8.0f))
					.IsEnabled_Lambda([this]() { return CanContinue(); })
					.OnClicked_Lambda([this]() { Continue(); return FReply::Handled(); })
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return bBusy ? LOCTEXT("Saving", "Saving...") : LOCTEXT("Continue", "Continue"); })
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
		];

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(InArgs._bShowBackdrop ? Style.GetBrush("Crowdy.Backdrop") : FStyleDefaults::GetNoBrush())
		.Cursor(EMouseCursor::Default)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(FMargin(24.0f))
		[
			SNew(SBox).WidthOverride(480.0f)
			[
				Card
			]
		]
	];
}

int32 SCrowdyAgePrompt::GetBirthYear() const
{
	return YearText.Len() == 4 && YearText.IsNumeric() ? FCString::Atoi(*YearText) : 0;
}

bool SCrowdyAgePrompt::DateNeedsParentalLine(int32 Year, int32 Month, const FDateTime& Now)
{
	if (Year <= 0 || Month < 1 || Month > 12)
	{
		return false;
	}

	const int32 Birthday = FDateTime::DaysInMonth(Year, Month);
	int32 Age = Now.GetYear() - Year;
	if (Now.GetMonth() < Month || (Now.GetMonth() == Month && Now.GetDay() < Birthday))
	{
		--Age;
	}
	return Age >= 13 && Age <= 17;
}

bool SCrowdyAgePrompt::NeedsParentalLine() const
{
	return DateNeedsParentalLine(GetBirthYear(), BirthMonth, FDateTime::UtcNow());
}

bool SCrowdyAgePrompt::CanContinue() const
{
	return !bBusy && !bRefused && GetBirthYear() > 0 && BirthMonth > 0 && !CountryCode.IsEmpty() && (!bDateFixed || bParentalChecked);
}

void SCrowdyAgePrompt::ShowRefusal(const FText& Text)
{
	bRefused = true;
	ErrorText = Text;
}

void SCrowdyAgePrompt::SetBusy(bool bInBusy)
{
	bBusy = bInBusy;
	if (bBusy)
	{
		ErrorText = FText::GetEmpty();
	}
}

void SCrowdyAgePrompt::SetError(const FText& InError)
{
	ErrorText = InError;
}

void SCrowdyAgePrompt::Continue()
{
	if (!CanContinue())
	{
		return;
	}
	if (!bDateFixed && NeedsParentalLine())
	{
		bDateFixed = true;
		return;
	}
	OnContinueDelegate.ExecuteIfBound();
}

void SCrowdyAgePrompt::Decline()
{
	if (!bBusy)
	{
		OnDeclineDelegate.ExecuteIfBound();
	}
}

TSharedRef<SWidget> SCrowdyAgePrompt::GetInitialFocus()
{
	return FirstControl.IsValid() ? FirstControl.ToSharedRef() : AsShared();
}

#if WITH_DEV_AUTOMATION_TESTS
void SCrowdyAgePrompt::SetAnswersForTest(int32 Year, int32 Month, const FString& Country)
{
	YearText = FString::FromInt(Year);
	BirthMonth = Month;
	CountryCode = Country;
}
#endif

#undef LOCTEXT_NAMESPACE
