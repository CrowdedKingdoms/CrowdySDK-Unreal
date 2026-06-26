// Fill out your copyright notice in the Description page of Project Settings.

#include "Debug/CrowdyNetStatsDisplay.h"

#if !UE_BUILD_SHIPPING

#include "CanvasTypes.h"
#include "ConsoleSettings.h"
#include "Engine/Canvas.h"
#include "Engine/Console.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"

namespace
{
	// Engine-stat registry keys. The console strips the "STAT_" prefix, so these toggle as
	// `stat crowdyudp` and `stat crowdyquery`. Both live under the "Crowdy" stat category.
	const FName UDPStatName    = TEXT("STAT_CrowdyUDP");
	const FName QueryStatName  = TEXT("STAT_CrowdyQuery");
	const FName CrowdyCategory = TEXT("STATCAT_Crowdy");

	// Muted grey for row labels; values are drawn in their own (often state-driven) colour.
	const FColor LabelColor(170, 170, 170);

	
	// Handle for our entry in the console's `stat ` auto-complete drop-down.
	FDelegateHandle GAutoCompleteHandle;

	// UConsole broadcasts this while (re)building its auto-complete list, so our commands
	// always appear the same hook the engine uses internally. Registering the engine stat
	// alone does NOT add it to the drop-down; the list is sourced from here + ManualAutoCompleteList.
	void AddCrowdyAutoCompleteEntries(TArray<FAutoCompleteCommand>& AutoCompleteList)
	{
		const UConsoleSettings* ConsoleSettings = GetDefault<UConsoleSettings>();
		const FColor CmdColor = ConsoleSettings ? ConsoleSettings->AutoCompleteCommandColor : FColor::White;

		auto Add = [&](const TCHAR* Command, const TCHAR* Desc)
		{
			FAutoCompleteCommand& Entry = AutoCompleteList.AddDefaulted_GetRef();
			Entry.Command = Command;
			Entry.Desc = Desc;
			Entry.Color = CmdColor;
		};

		Add(TEXT("stat CrowdyUDP"),   TEXT("Crowdy: UDP connection, throughput, ping & message rates"));
		Add(TEXT("stat CrowdyQuery"), TEXT("Crowdy: GraphQL query / response rates"));
	}

	// Colour ramps mirror how `stat unit` reds-out bad values.
	FColor PingColor(const int64 PingMs)
	{
		if (PingMs <= 0)  return FColor::Silver; // not yet measured
		if (PingMs < 80)  return FColor::Green;
		if (PingMs < 150) return FColor::Yellow;
		return FColor::Red;
	}

	const TCHAR* ConnStateText(const EUDPConnectionState State)
	{
		switch (State)
		{
		case EUDPConnectionState::Disconnected: return TEXT("Disconnected");
		case EUDPConnectionState::Connecting:   return TEXT("Connecting");
		case EUDPConnectionState::Connected:    return TEXT("Connected");
		case EUDPConnectionState::Reconnecting: return TEXT("Reconnecting");
		case EUDPConnectionState::GateKeep:     return TEXT("Gate Kept");
		default:                                return TEXT("Unknown");
		}
	}

	FColor ConnStateColor(const EUDPConnectionState State)
	{
		switch (State)
		{
		case EUDPConnectionState::Connected:    return FColor::Green;
		case EUDPConnectionState::Connecting:
		case EUDPConnectionState::Reconnecting: return FColor::Yellow;
		case EUDPConnectionState::Disconnected:
		case EUDPConnectionState::GateKeep:     return FColor::Red;
		default:                                return FColor::Silver;
		}
	}

	// Human-readable per-second throughput. Input is bytes observed in the last second.
	FString FormatRate(const int32 BytesPerSec)
	{
		if (BytesPerSec >= 1024 * 1024) return FString::Printf(TEXT("%.1f MB/s"), BytesPerSec / (1024.0f * 1024.0f));
		if (BytesPerSec >= 1024)        return FString::Printf(TEXT("%.1f KB/s"), BytesPerSec / 1024.0f);
		return FString::Printf(TEXT("%d B/s"), BytesPerSec);
	}

	// Signature must match UEngine::FEngineStatRender.
	int32 RenderStatCrowdyUDP(UWorld* World, FViewport* /*Viewport*/, FCanvas* Canvas,
	                          int32 X, int32 Y, const FVector* /*ViewLocation*/, const FRotator* /*ViewRotation*/)
	{
		const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		UCrowdyUDPSubsystem* UDP = GI ? GI->GetSubsystem<UCrowdyUDPSubsystem>() : nullptr;
		if (!UDP || !GEngine)
		{
			return Y;
		}

		const FUDPNetworkStatistics S = UDP->GetUDPNetworkStats();
		const EUDPConnectionState State = UDP->GetConnectionState();
		const UFont* Font = GEngine->GetSmallFont();
		const int32 RowHeight = FMath::TruncToInt(Font->GetMaxCharHeight() * 1.1f);
		// Value column starts past the widest label so labels and values line up.
		const int32 ValueX = X + Font->GetStringSize(TEXT("Total msgs")) + 16;

		auto DrawRow = [&](const TCHAR* Label, const FString& Value, const FColor& ValueColor)
		{
			Canvas->DrawShadowedString(X, Y, Label, Font, LabelColor);
			Canvas->DrawShadowedString(ValueX, Y, *Value, Font, ValueColor);
			Y += RowHeight;
		};

		// Header: title + live connection state.
		Canvas->DrawShadowedString(X, Y, TEXT("CROWDY UDP"), Font, FColor::White);
		Canvas->DrawShadowedString(X + Font->GetStringSize(TEXT("CROWDY UDP  ")), Y,
			ConnStateText(State), Font, ConnStateColor(State));
		Y += RowHeight;

		DrawRow(TEXT("Ping"),
			S.Ping > 0 ? FString::Printf(TEXT("%lld ms"), S.Ping) : FString(TEXT("--")),
			PingColor(S.Ping));

		DrawRow(TEXT("Recv"),
			FString::Printf(TEXT("%s   %d dg/s"), *FormatRate(S.BytesReceived), S.DatagramsReceived),
			FColor::Silver);

		DrawRow(TEXT("Sent"),
			FString::Printf(TEXT("%s   %d dg/s"), *FormatRate(S.BytesSent), S.DatagramsSent),
			FColor::Silver);

		DrawRow(TEXT("Msg/s"),
			FString::Printf(TEXT("%d sent / %d recv"), S.MessagesSentPerSecond, S.MessagesReceivedPerSecond),
			FColor::Silver);

		DrawRow(TEXT("Total msgs"),
			FString::Printf(TEXT("%lld sent / %lld recv"), S.TotalMessagesSent, S.TotalMessagesReceived),
			FColor::Silver);

		return Y;
	}

	// Signature must match UEngine::FEngineStatRender.
	int32 RenderStatCrowdyQuery(UWorld* World, FViewport* /*Viewport*/, FCanvas* Canvas,
	                            int32 X, int32 Y, const FVector* /*ViewLocation*/, const FRotator* /*ViewRotation*/)
	{
		const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		UCrowdyQuerySubsystem* Query = GI ? GI->GetSubsystem<UCrowdyQuerySubsystem>() : nullptr;
		if (!Query || !GEngine)
		{
			return Y;
		}

		const FQueryStats S = Query->GetQueryStats();
		const UFont* Font = GEngine->GetSmallFont();
		const int32 RowHeight = FMath::TruncToInt(Font->GetMaxCharHeight() * 1.1f);
		const int32 ValueX = X + Font->GetStringSize(TEXT("Responses/s")) + 16;

		auto DrawRow = [&](const TCHAR* Label, const FString& Value, const FColor& ValueColor)
		{
			Canvas->DrawShadowedString(X, Y, Label, Font, LabelColor);
			Canvas->DrawShadowedString(ValueX, Y, *Value, Font, ValueColor);
			Y += RowHeight;
		};

		// Header: title + active/idle indicator.
		Canvas->DrawShadowedString(X, Y, TEXT("CROWDY QUERY (GraphQL)"), Font, FColor::White);
		Canvas->DrawShadowedString(X + Font->GetStringSize(TEXT("CROWDY QUERY (GraphQL)  ")), Y,
			S.bIsReceivingData ? TEXT("Active") : TEXT("Idle"),
			Font, S.bIsReceivingData ? FColor::Green : LabelColor);
		Y += RowHeight;

		// Queries sent per second (with the matching outbound byte rate).
		DrawRow(TEXT("Queries/s"),
			FString::Printf(TEXT("%d   (%s)"), S.QueriesSentPerSecond, *FormatRate(S.QueryBytesSentPerSecond)),
			FColor::Silver);

		// Responses received per second (with the matching inbound byte rate).
		DrawRow(TEXT("Responses/s"),
			FString::Printf(TEXT("%d   (%s)"), S.ResponseReceivedPerSecond, *FormatRate(S.ResponseBytesReceivedPerSecond)),
			FColor::Silver);

		return Y;
	}
}

namespace CrowdyNetStats
{
	void Register()
	{
		if (!GEngine)
		{
			return;
		}

		// bIsRHS = false render in the left column. The right column right-aligns short
		// readouts (fps/unit) and clips our wider lines, so we use the left side.
		GEngine->AddEngineStat(
			UDPStatName, CrowdyCategory,
			FText::FromString(TEXT("Display Crowdy UDP connection state, throughput, ping and client-notify loss.")),
			UEngine::FEngineStatRender::CreateStatic(&RenderStatCrowdyUDP),
			UEngine::FEngineStatToggle(),
			/*bIsRHS*/ false);

		GEngine->AddEngineStat(
			QueryStatName, CrowdyCategory,
			FText::FromString(TEXT("Display Crowdy GraphQL query / response rates.")),
			UEngine::FEngineStatRender::CreateStatic(&RenderStatCrowdyQuery),
			UEngine::FEngineStatToggle(),
			/*bIsRHS*/ false);

		// Make the commands appear in the console `stat ` auto-complete drop-down.
		if (!GAutoCompleteHandle.IsValid())
		{
			GAutoCompleteHandle = UConsole::RegisterConsoleAutoCompleteEntries.AddStatic(&AddCrowdyAutoCompleteEntries);
		}
	}

	void Unregister()
	{
		if (GAutoCompleteHandle.IsValid())
		{
			UConsole::RegisterConsoleAutoCompleteEntries.Remove(GAutoCompleteHandle);
			GAutoCompleteHandle.Reset();
		}

		if (!GEngine)
		{
			return;
		}

		GEngine->RemoveEngineStat(UDPStatName);
		GEngine->RemoveEngineStat(QueryStatName);
	}
}

#endif // !UE_BUILD_SHIPPING
