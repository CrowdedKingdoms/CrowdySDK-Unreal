#include "Customizations/CrowdyChannelPicker.h"

#include "CrowdyStudioModule.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Textures/SlateIcon.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Widgets/Layout/SBox.h"

namespace CrowdyChannelPicker
{
	namespace
	{
		// Fetched at edit time and shared by every open details panel; game thread only.
		TArray<FString> GChannelNames;
		bool GFetchInFlight = false;

		TSharedRef<SWidget> MakeMenuContent(const TFunction<void(const FString&)>& OnPick, const FText& EmptyLabel, const FText& EmptyToolTip)
		{
			FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ true, nullptr);

			MenuBuilder.AddMenuEntry(EmptyLabel, EmptyToolTip, FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([OnPick] { OnPick(FString()); })));

			if (CrowdyStudioAuth::GetSignedInToken().IsEmpty())
			{
				MenuBuilder.AddMenuEntry(
					FText::FromString(TEXT("Sign in to Crowdy Studio to load channels")),
					FText::GetEmpty(), FSlateIcon(),
					FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
				return MenuBuilder.MakeWidget();
			}

			if (GChannelNames.Num() == 0)
			{
				MenuBuilder.AddMenuEntry(
					FText::FromString(GFetchInFlight ? TEXT("Loading channels...") : TEXT("No channels found")),
					FText::GetEmpty(), FSlateIcon(),
					FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
			}
			else
			{
				MenuBuilder.BeginSection(NAME_None, FText::FromString(TEXT("Channels")));
				for (const FString& Name : GChannelNames)
				{
					MenuBuilder.AddMenuEntry(FText::FromString(Name), FText::GetEmpty(), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([OnPick, Name] { OnPick(Name); })));
				}
				MenuBuilder.EndSection();
			}

			MenuBuilder.AddSeparator();
			MenuBuilder.AddMenuEntry(FText::FromString(TEXT("Refresh")), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([] { FetchChannelNamesIfStale(); })));

			return MenuBuilder.MakeWidget();
		}
	}

	void FetchChannelNamesIfStale()
	{
		if (GFetchInFlight)
		{
			return;
		}

		const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
		if (!Settings || Settings->AppID <= 0 || CrowdyStudioAuth::GetSignedInToken().IsEmpty())
		{
			return;
		}

		// Channels live on the Game API, which takes only an app-scoped token, so Crowdy Studio mints one and asks.
		GFetchInFlight = true;
		CrowdyStudioAuth::FetchAppChannelNames(Settings->AppID, [](const TArray<FString>& Names)
		{
			GFetchInFlight = false;
			GChannelNames = Names;
		});
	}

	TSharedRef<SWidget> MakeMenu(TFunction<void(const FString&)> OnPick, const FText& EmptyLabel, const FText& EmptyToolTip)
	{
		FetchChannelNamesIfStale();

		TSharedRef<SBox> MenuBox = SNew(SBox);
		MenuBox->SetContent(MakeMenuContent(OnPick, EmptyLabel, EmptyToolTip));

		// The popup is built once, so a list that arrives while it is open is drawn in place until the fetch settles.
		const TWeakPtr<SBox> WeakMenuBox = MenuBox;
		MenuBox->RegisterActiveTimer(0.15f, FWidgetActiveTimerDelegate::CreateLambda(
			[WeakMenuBox, OnPick, EmptyLabel, EmptyToolTip, LastCount = GChannelNames.Num(), LastInFlight = GFetchInFlight]
			(double, float) mutable -> EActiveTimerReturnType
			{
				const TSharedPtr<SBox> Box = WeakMenuBox.Pin();
				if (!Box.IsValid())
				{
					return EActiveTimerReturnType::Stop;
				}

				if (GChannelNames.Num() != LastCount || GFetchInFlight != LastInFlight)
				{
					Box->SetContent(MakeMenuContent(OnPick, EmptyLabel, EmptyToolTip));
					LastCount = GChannelNames.Num();
					LastInFlight = GFetchInFlight;
				}

				return GFetchInFlight ? EActiveTimerReturnType::Continue : EActiveTimerReturnType::Stop;
			}));

		return MenuBox;
	}
}
