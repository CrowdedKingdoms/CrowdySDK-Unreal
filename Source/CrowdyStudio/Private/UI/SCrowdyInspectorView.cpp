// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyInspectorView.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyTeams.h"
#include "Subsystem/CrowdyChannels.h"
#include "Subsystem/CrowdyAvatars.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyInspectorView::Construct(const FArguments& InArgs)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[ SNew(STextBlock).Text(LOCTEXT("InspectorHeader", "Runtime Inspector")).TextStyle(&Style, "Crowdy.Text.Title") ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body")
			.Text(LOCTEXT("InspectorHint", "Read-only view of the running Play-In-Editor session's Crowdy state. Nothing here changes the game. Press Refresh after the session connects or fetches data."))
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(11.0f, 6.0f))
				.OnClicked(this, &SCrowdyInspectorView::OnRefreshClicked)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)
					[ CrowdyStudioWidgets::Icon(TEXT("refresh"), 14.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[ SNew(STextBlock).Text(LOCTEXT("InspectorRefresh", "Refresh")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() { return bAutoRefresh ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { SetAutoRefresh(NewState == ECheckBoxState::Checked); })
				[ SNew(STextBlock).Text(LOCTEXT("AutoRefresh", "Auto-refresh")).TextStyle(&Style, "Crowdy.Text.Body") ]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
			[
				CrowdyStudioWidgets::SegmentedEnum(
					{ TEXT("1"), TEXT("2"), TEXT("5") },
					{ LOCTEXT("Interval1", "1s"), LOCTEXT("Interval2", "2s"), LOCTEXT("Interval5", "5s") },
					TAttribute<FString>::CreateLambda([this]() { return IntervalChoice; }),
					[this](const FString& V) { IntervalChoice = V; SetRefreshInterval(FCString::Atof(*V)); })
			]
		]

		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			CrowdyStudioWidgets::Card(
				SNew(SScrollBox)
				+ SScrollBox::Slot()[ SAssignNew(BodyBox, SVerticalBox) ],
				FMargin(12.0f), /*bFlat*/ true)
		]
	];

	Rebuild();
}

FReply SCrowdyInspectorView::OnRefreshClicked()
{
	Rebuild();
	return FReply::Handled();
}

UGameInstance* SCrowdyInspectorView::FindRunningGameInstance()
{
	if (!GEngine)
	{
		return nullptr;
	}

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE && Context.WorldType != EWorldType::Game)
		{
			continue;
		}
		if (UGameInstance* GameInstance = Context.OwningGameInstance)
		{
			return GameInstance;
		}
	}
	return nullptr;
}

void SCrowdyInspectorView::Rebuild()
{
	if (!BodyBox.IsValid())
	{
		return;
	}

	BodyBox->ClearChildren();

	auto AddHeader = [this](const FText& Title)
	{
		BodyBox->AddSlot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 4.0f)
		[ SNew(STextBlock).Text(Title).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Heading") ];
	};

	auto AddLine = [this](const FString& Line)
	{
		BodyBox->AddSlot().AutoHeight().Padding(4.0f, 1.0f)
		[
			SNew(STextBlock).Text(FText::FromString(Line)).AutoWrapText(true)
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
		];
	};

	auto AddNote = [this](const FText& Note)
	{
		BodyBox->AddSlot().AutoHeight().Padding(4.0f, 1.0f)
		[ SNew(STextBlock).Text(Note).AutoWrapText(true).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ];
	};

	UGameInstance* GameInstance = FindRunningGameInstance();
	if (!GameInstance)
	{
		BodyBox->AddSlot().AutoHeight().Padding(0.0f, 30.0f, 0.0f, 0.0f)
		[ CrowdyStudioWidgets::EmptyState(TEXT("inspector"), LOCTEXT("InspectorIdle", "No running session.\nStart Play-In-Editor to inspect live Crowdy state.")) ];
		return;
	}

	if (UCrowdyGameSession* Session = GameInstance->GetSubsystem<UCrowdyGameSession>())
	{
		AddHeader(LOCTEXT("InspectorSession", "Session"));
		AddLine(FString::Printf(TEXT("App ID: %lld"), Session->GetAppID()));
		AddLine(FString::Printf(TEXT("User ID: %lld"), Session->GetUserID()));
		AddLine(FString::Printf(TEXT("Game Token ID: %lld"), Session->GetGameTokenID()));
		AddLine(FString::Printf(TEXT("UUID: %s"), *Session->GetUUID()));
		AddLine(FString::Printf(TEXT("Host ID: %s"), *Session->GetHostID().ToString(EGuidFormats::DigitsWithHyphens)));
		// The token itself is a secret; only report whether one is held, never its value.
		AddLine(Session->GetGameToken().IsEmpty() ? TEXT("Game Token: not set") : TEXT("Game Token: set (hidden)"));
	}

	if (UCrowdyTeams* Teams = GameInstance->GetSubsystem<UCrowdyTeams>())
	{
		const TArray<FCrowdyGroupMembership> MyTeams = Teams->GetCachedMyTeams();
		AddHeader(FText::Format(LOCTEXT("InspectorTeams", "Teams ({0})"), FText::AsNumber(MyTeams.Num())));
		if (!Teams->HasCachedTeams())
		{
			AddNote(LOCTEXT("InspectorTeamsEmpty", "Cache not populated yet (the game has not fetched teams)."));
		}
		for (const FCrowdyGroupMembership& Membership : MyTeams)
		{
			AddLine(FString::Printf(TEXT("#%lld  %s"), Membership.Group.GroupId, *Membership.Group.Name));
		}
	}

	if (UCrowdyChannels* Channels = GameInstance->GetSubsystem<UCrowdyChannels>())
	{
		const TArray<FCrowdyGroupMembership> MyChannels = Channels->GetCachedMyChannels();
		AddHeader(FText::Format(LOCTEXT("InspectorChannels", "Channels ({0})"), FText::AsNumber(MyChannels.Num())));
		if (!Channels->HasCachedChannels())
		{
			AddNote(LOCTEXT("InspectorChannelsEmpty", "Cache not populated yet (the game has not fetched channels)."));
		}
		for (const FCrowdyGroupMembership& Membership : MyChannels)
		{
			AddLine(FString::Printf(TEXT("#%lld  %s"), Membership.Group.GroupId, *Membership.Group.Name));
		}
		AddLine(FString::Printf(TEXT("Reliable RPC channels ready: %s"), Channels->AreReliableChannelsReady() ? TEXT("yes") : TEXT("no")));
		AddLine(FString::Printf(TEXT("Session channel id: %lld"), Channels->GetSessionChannelId()));
	}

	if (UCrowdyAvatars* Avatars = GameInstance->GetSubsystem<UCrowdyAvatars>())
	{
		const TArray<FCrowdyAvatar> MyAvatars = Avatars->GetCachedMyAvatars();
		AddHeader(FText::Format(LOCTEXT("InspectorAvatars", "Avatars ({0})"), FText::AsNumber(MyAvatars.Num())));
		if (!Avatars->HasCachedAvatars())
		{
			AddNote(LOCTEXT("InspectorAvatarsEmpty", "Cache not populated yet (the game has not fetched avatars)."));
		}
		for (const FCrowdyAvatar& Avatar : MyAvatars)
		{
			AddLine(FString::Printf(TEXT("#%lld  %s"), Avatar.AvatarId, *Avatar.Name));
		}
	}

	if (UWorld* World = GameInstance->GetWorld())
	{
		if (UCrowdyEntitySubsystem* Entities = World->GetSubsystem<UCrowdyEntitySubsystem>())
		{
			const FGuid LocalPlayerId = Entities->GetLocalPlayerID();
			AddHeader(LOCTEXT("InspectorEntities", "Entities"));
			AddLine(FString::Printf(TEXT("Local player ID: %s"), *LocalPlayerId.ToString(EGuidFormats::DigitsWithHyphens)));
			AddLine(FString::Printf(TEXT("Host ID: %s"), *Entities->GetHostID().ToString(EGuidFormats::DigitsWithHyphens)));

			// There is no "every entity" accessor, so list what this client owns. That is the most
			// useful slice for confirming spawns and ownership while debugging.
			const TArray<AActor*> Owned = Entities->GetEntitiesByOwner(LocalPlayerId);
			AddLine(FString::Printf(TEXT("Owned by this client: %d"), Owned.Num()));
			for (const AActor* Actor : Owned)
			{
				if (Actor)
				{
					AddLine(FString::Printf(TEXT("  %s"), *Actor->GetName()));
				}
			}
		}
	}
}

void SCrowdyInspectorView::SetAutoRefresh(bool bEnable)
{
	bAutoRefresh = bEnable;
	RestartAutoRefreshTimer();
}

void SCrowdyInspectorView::SetRefreshInterval(float Seconds)
{
	RefreshInterval = Seconds > 0.0f ? Seconds : 2.0f;
	if (bAutoRefresh)
	{
		RestartAutoRefreshTimer();
	}
}

void SCrowdyInspectorView::RestartAutoRefreshTimer()
{
	if (const TSharedPtr<FActiveTimerHandle> Existing = AutoRefreshTimer.Pin())
	{
		UnRegisterActiveTimer(Existing.ToSharedRef());
		AutoRefreshTimer.Reset();
	}
	if (bAutoRefresh)
	{
		AutoRefreshTimer = RegisterActiveTimer(RefreshInterval, FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyInspectorView::OnAutoRefreshTick));
	}
}

EActiveTimerReturnType SCrowdyInspectorView::OnAutoRefreshTick(double /*InCurrentTime*/, float /*InDeltaTime*/)
{
	if (!bAutoRefresh)
	{
		AutoRefreshTimer.Reset();
		return EActiveTimerReturnType::Stop;
	}
	Rebuild();
	return EActiveTimerReturnType::Continue;
}

#undef LOCTEXT_NAMESPACE
