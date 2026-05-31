// Fill out your copyright notice in the Description page of Project Settings.


#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Queries/UDP/FUDPAddressNotify.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Threading/FUDPListener.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Utils/SerializationFunctionLibrary.h"


void UCrowdyUDPSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	GameSession = GetWorld()->GetGameInstance()->GetSubsystem<UCrowdyGameSession>();
	
	if (!IsValid(GameSession))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to get CrowdyGameSession instance"));
	}
	
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: UDP Subsystem Initialized."))
}

void UCrowdyUDPSubsystem::Deinitialize()
{
	
	// Signal all in-flight AsyncTasks to abort before we tear anything down
	bIsShuttingDown = true;

	StopUDPListener();
	CleanupSockets();
	
	// Clear GameSession reference so any racing lambda sees nullptr
	GameSession = nullptr;
	
	Super::Deinitialize();
}


bool UCrowdyUDPSubsystem::SendMessage(TArray<uint8>&& Message)
{
	if (!bAllowUdpEvents)
		return false;

	TArray<uint8> OwnedBytes = MoveTemp(Message);

	// Route to whichever socket was chosen during InitializeUDP.
	// Runtime protocol switching is intentionally not supported here —
	// if the active socket fails, the timeout monitor will detect it and
	// trigger a clean reconnect via HandleUDPAddressNotify.
	if (bUseIPv4)
		return SendUDPv4(OwnedBytes);

	return SendUDPv6(OwnedBytes);
}

void UCrowdyUDPSubsystem::HandleUDPMessage(const uint8* Data, const int32 Size)
{
	if (Size <= 0)
		return;
	
	ReceivedBytesThisSecond += Size;
	++ReceivedDatagramsThisSecond;
	
	if (bDiscardReceivedMessages)
	{
		OnMessageReceived();
		return;
	}
	
	TArray<uint8> Message;
	Message.SetNumUninitialized(Size);
	FMemory::Memcpy(Message.GetData(), Data, Size);

	if (const bool HmacVerify = USerializationFunctionLibrary::AuthenticateHMAC(Message, GameSession->GetGameToken()); !HmacVerify)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CrowdySDK]: HMAC verification failed for received message"));
		return;
	}
	
	if (bIsShuttingDown)
		return;
		
	if (IsValid(GameSession))
	{
		OnMessageReceived();
		GameSession->EnqueueMessageToReceive(Message);
	}
	
}

void UCrowdyUDPSubsystem::OnMessageReceived()
{
	if (bTimeoutEnabled)
		LastMessageTime = std::chrono::steady_clock::now();

	if (!bUDPConnected)
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			if (bIsShuttingDown)
				return;

			SetConnectionState(EUDPConnectionState::Connected);
			bUDPConnected = true;
			OnUDPConnectionSuccessful.Broadcast();
		});
	}
}

FUDPNetworkStatistics UCrowdyUDPSubsystem::GetUDPNetworkStats() const
{
	FUDPNetworkStatistics Stats;
	Stats.BytesSent = LastSecondSentBytes;
	Stats.BytesReceived = LastSecondReceivedBytes;
	Stats.DatagramsSent = LastSecondSentDatagrams;
	Stats.DatagramsReceived = LastSecondReceivedDatagrams;
	Stats.MessagesSentPerSecond = LastSecondMessagesSent;
	Stats.MessagesReceivedPerSecond = LastSecondMessagesReceived;
	Stats.TotalClientNotifiesSent = TotalClientNotifiesSent;
	Stats.TotalClientNotifiesReceived = TotalClientNotifiesReceived;
	Stats.Ping = PingTime;
	Stats.TotalPendingClientNotifies = TotalClientNotifiesSent.load() - TotalClientNotifiesReceived.load();
	
	if (Stats.DatagramsReceived > 0)
		Stats.SendRecvRatio = static_cast<float>(Stats.BytesSent) / static_cast<float>(Stats.BytesReceived);
	
	if (Stats.TotalClientNotifiesSent > 0)
		Stats.ClientNotifyLossPercentage = ((Stats.TotalClientNotifiesSent - Stats.TotalClientNotifiesReceived) /
			static_cast<float>(Stats.TotalClientNotifiesSent)) * 100.0f;
	
	return Stats;
}

void UCrowdyUDPSubsystem::ResetUDPNetworkStats()
{
	LastSecondSentBytes = 0;
	LastSecondReceivedBytes = 0;
	LastSecondSentDatagrams = 0;
	LastSecondReceivedDatagrams = 0;
	LastSecondMessagesSent = 0;
	LastSecondMessagesReceived = 0;
	TotalClientNotifiesReceived = 0;
	TotalClientNotifiesSent = 0;
}

void UCrowdyUDPSubsystem::StartTimeoutMonitoring(const float ThresholdSeconds)
{
	AsyncTask(ENamedThreads::GameThread, [this, ThresholdSeconds]()
	{
		if (bTimeoutEnabled) return;
	
	const UWorld* World = GetWorld();
	
	if (!IsValid(World))
		return;
	
	TimeoutThresholdSeconds = ThresholdSeconds;
	bTimeoutEnabled = true;
	
	LastMessageTime = std::chrono::steady_clock::now();
	
	World->GetTimerManager().SetTimer(TimeoutCheckTimerHandle, 
		this, 
		&UCrowdyUDPSubsystem::CheckForTimeout, 
		5.0f, 
		true);
	
	UE_LOG(LogTemp, Log, TEXT("UDP timeout monitoring started with threshold of %.2f seconds"), ThresholdSeconds);
	});
}

void UCrowdyUDPSubsystem::StopTimeoutMonitoring()
{
	bTimeoutEnabled = false;
	
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(TimeoutCheckTimerHandle);
		UE_LOG(LogTemp, Log, TEXT("UDP timeout monitoring stopped"));
	}
}

void UCrowdyUDPSubsystem::IncrementReceivedMessageCount()
{
	++MessagesReceivedThisSecond;
}

void UCrowdyUDPSubsystem::ToggleUdpEvents(const bool bAllow)
{
	bAllowUdpEvents = bAllow;
}

void UCrowdyUDPSubsystem::IncrementTotalClientNotifiesReceived()
{
	++TotalClientNotifiesReceived;
}

void UCrowdyUDPSubsystem::UpdatePingTime(const int64 NewPingTime)
{
	PingTime = NewPingTime;
}

bool UCrowdyUDPSubsystem::InitializeUDP(const FUDPAddressNotify& UDPAddressNotify)
{
	bUseIPv4 = false;

	bool bInitialised = false;

	switch (PreferredProtocol)
	{
	case ECrowdyUDPProtocol::IPv4:
		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK] UDP: protocol forced to IPv4"));
		if (InitializeV4UDPSocket(UDPAddressNotify.IPv4Address, UDPAddressNotify.Port))
		{
			// InitializeV4UDPSocket already created the listener — just flag
			// the send path to use the V4 socket.
			bUseIPv4 = true;
			bInitialised = true;
		}
		break;

	case ECrowdyUDPProtocol::IPv6:
		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK] UDP: protocol forced to IPv6"));
		bInitialised = InitializeUDPSocket(UDPAddressNotify.IPv6Address, UDPAddressNotify.Port);
		break;

	case ECrowdyUDPProtocol::Auto:
	default:
		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK] UDP: protocol auto — trying IPv6 first"));
		if (InitializeUDPSocket(UDPAddressNotify.IPv6Address, UDPAddressNotify.Port))
		{
			bInitialised = true;
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("[CrowdySDK] UDP: IPv6 failed, falling back to IPv4"));
			if (InitializeV4UDPSocket(UDPAddressNotify.IPv4Address, UDPAddressNotify.Port))
			{
				bUseIPv4 = true;
				bInitialised = true;
			}
		}
		break;
	}

	if (bInitialised)
	{
		// Allow outbound messages now that a socket is live.
		bAllowUdpEvents = true;
	}

	return bInitialised;
}

void UCrowdyUDPSubsystem::CheckForTimeout()
{
	if (!bTimeoutEnabled)
	{
		return;
	}

	const auto CurrentTime = std::chrono::steady_clock::now();
	const auto LastMsgTime = LastMessageTime.load();

	const auto TimeSinceLastMessage = std::chrono::duration_cast<std::chrono::seconds>(
		CurrentTime - LastMsgTime).count();

	if (TimeSinceLastMessage >= TimeoutThresholdSeconds)
	{
		UE_LOG(LogTemp, Warning, TEXT("UDP timeout detected - no messages received for %lld seconds"),
			   TimeSinceLastMessage);
		
		StopTimeoutMonitoring();
		
		// Stop all UDP operations
		StopAllOperations();

		// Broadcast the timeout event
		OnUDPTimeout.Broadcast();
	}
}

void UCrowdyUDPSubsystem::StopAllOperations()
{
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK] UDP: stopping all operations"));

	// Block outbound sends before tearing down the socket.
	bAllowUdpEvents = false;

	StopUDPListener();
	CleanupSockets();

	bUDPConnected = false;
	SetConnectionState(EUDPConnectionState::Disconnected);

	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(UDPStatsTimerHandle);
	}
}

bool UCrowdyUDPSubsystem::InitializeUDPSocket(const FString& IPAddress, const int32 Port)
{
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	
	if (!SocketSubsystem) return false;
	
	const TSharedRef<FInternetAddr> TargetAddress = SocketSubsystem->CreateInternetAddr();
	bool bIsValidAddress = false;
	TargetAddress->SetIp(*IPAddress, bIsValidAddress);
	
	if (!bIsValidAddress)
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid IP address: %s"), *IPAddress);
		CleanupSockets();
		return false;
	}
	
	TargetAddress->SetPort(Port);
	
	// Determine if this is IPv4 or IPv6 based on the target address
	const bool bIsIPv6 = TargetAddress->GetProtocolType() == FNetworkProtocolTypes::IPv6;
	const FName ProtocolType = bIsIPv6 ? FNetworkProtocolTypes::IPv6 : FNetworkProtocolTypes::IPv4;
	
	// Create a UDP socket with the correct protocol
	UDPSocketV6 = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("UDP Socket v6"), ProtocolType);
	if (!UDPSocketV6)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create UDP socket"));
		return false;
	}
	
	// Create local address with matching protocol
	const TSharedRef<FInternetAddr> LocalAddr = SocketSubsystem->CreateInternetAddr(ProtocolType);
	
	// Set any appropriate address based on protocol
	if (bIsIPv6)
	{
		// For IPv6, we need to set any address differently
		bool bIsValid = false;
		LocalAddr->SetIp(TEXT("::"), bIsValid); // IPv6 any address
		if (!bIsValid)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to set IPv6 any address"));
			CleanupSockets();
			return false;
		}
	}
	
	LocalAddr->SetPort(0); // Let the system assign a local port
	
	// Bind socket to local address
	if (!UDPSocketV6->Bind(*LocalAddr))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to bind UDP socket to local address"));
		// Cleanup socket inline
		CleanupSockets();
		return false;
	}
	
	// Connect to the target address (for UDP, this sets the default destination)
	if (!UDPSocketV6->Connect(*TargetAddress))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to connect UDP socket to %s:%d"), *IPAddress, Port);
		// Cleanup socket inline
		CleanupSockets();
		return false;
	}
	
	// Set socket to non-blocking mode
	UDPSocketV6->SetNonBlocking(true);
	int32 NewSendBufferSize = 0;
	int32 NewRecvBufferSize = 0;
	UDPSocketV6->SetSendBufferSize(16000000, NewSendBufferSize);
	UDPSocketV6->SetReceiveBufferSize(16000000, NewRecvBufferSize);
	UE_LOG(LogTemp, Log, TEXT("UDP (%s) buffers set: snd=%d, rcv=%d"),
		   bIsIPv6 ? TEXT("IPv6") : TEXT("IPv4"), NewSendBufferSize, NewRecvBufferSize);

	UE_LOG(LogTemp, Log, TEXT("Successfully connected UDP socket (%s) to %s:%d"),
		   bIsIPv6 ? TEXT("IPv6") : TEXT("IPv4"), *IPAddress, Port);
	
	
	ListenerRunnable = new FUDPListener(UDPSocketV6, this);
	ListenerThread = FRunnableThread::Create(ListenerRunnable, TEXT("UDPListenerThread"), 0, TPri_AboveNormal);
	
	
	// Start stats timer on game thread
	AsyncTask(
		ENamedThreads::GameThread,
		[this]()
		{
			if (const UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimer(UDPStatsTimerHandle, this, &UCrowdyUDPSubsystem::UpdateUDPStats,
												  1.0f, true);
			}
		});

	return true;
}

bool UCrowdyUDPSubsystem::InitializeV4UDPSocket(const FString& IPAddress, const int32 Port)
{
	// Get socket subsystem
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to get socket subsystem"));
		return false;
	}

	// Create UDP socket specifically for IPv4
	UDPSocketV4 = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("UDP Socket V4"), false);
	if (!UDPSocketV4)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create UDP socket (IPv4)"));
		return false;
	}

	// Create target address for connection (IPv4)
	const TSharedRef<FInternetAddr> TargetAddr = SocketSubsystem->CreateInternetAddr();
	bool bIsValidAddress = false;
	TargetAddr->SetIp(*IPAddress, bIsValidAddress);

	if (!bIsValidAddress)
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid IPv4 address: %s"), *IPAddress);
		// Cleanup socket inline
		CleanupSockets();
		return false;
	}

	TargetAddr->SetPort(Port);

	// Create local address for binding (IPv4)
	TSharedRef<FInternetAddr> LocalAddr = SocketSubsystem->CreateInternetAddr();
	LocalAddr->SetAnyAddress(); // Bind to any IPv4 address
	LocalAddr->SetPort(0); // Let system assign a local port

	// Allow socket reuse
	UDPSocketV4->SetReuseAddr(true);

	// Bind socket to local address
	if (!UDPSocketV4->Bind(*LocalAddr))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to bind UDP socket (IPv4) to local address"));
		// Cleanup socket inline
		CleanupSockets();
		return false;
	}

	// Connect to target address (for UDP, this sets the default destination)
	if (!UDPSocketV4->Connect(*TargetAddr))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to connect UDP socket (IPv4) to %s:%d"), *IPAddress, Port);
		// Cleanup socket inline
		CleanupSockets();
		return false;
	}

	// Set socket to non-blocking mode
	UDPSocketV4->SetNonBlocking(true);
	int32 NewSendBufferSize = 0;
	int32 NewRecvBufferSize = 0;
	UDPSocketV4->SetSendBufferSize(16000000, NewSendBufferSize);
	UDPSocketV4->SetReceiveBufferSize(16000000, NewRecvBufferSize);
	UE_LOG(LogTemp, Log, TEXT("UDP (IPv4) buffers set: snd=%d, rcv=%d"), NewSendBufferSize, NewRecvBufferSize);

	UE_LOG(LogTemp, Log, TEXT("Successfully connected UDP socket (%s) to %s:%d"), TEXT("IPv4"), *IPAddress, Port);

	
	ListenerRunnable = new FUDPListener(UDPSocketV4, this);
	ListenerThread = FRunnableThread::Create(ListenerRunnable, TEXT("UDPListenerThread"), 0, TPri_AboveNormal);
	
	AsyncTask(
		ENamedThreads::GameThread,
		[this]()
		{
			if (const UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimer(UDPStatsTimerHandle, this, &UCrowdyUDPSubsystem::UpdateUDPStats,
												  1.0f, true);
			}
		});

	
	return true;
}

bool UCrowdyUDPSubsystem::SendUDPv6(const TArray<uint8>& Message)
{
	if (!UDPSocketV6) return false;
	
	int32 BytesSentNow = 0;
	const bool bUDPPacketSent = UDPSocketV6->Send(Message.GetData(), Message.Num(), BytesSentNow);
	
	// Handle Stats
	if (bUDPPacketSent && BytesSentNow > 0)
	{
		SentBytesThisSecond += BytesSentNow;
		++SentDatagramsThisSecond;
		++MessagesSentThisSecond;
		
		if (static_cast<ECrowdyMessageType>(Message[0]) == ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION)
		{
			//UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Client Event Notify Sent"));
			++TotalClientNotifiesSent;
		}
		
		return bUDPPacketSent && BytesSentNow > 0;
	}
	
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	
	if (!SocketSubsystem)
	{
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv6) packet send failed - unable to get error details"));
		return false;
	}

	const ESocketErrors SocketError = SocketSubsystem->GetLastErrorCode();
	UE_LOG(LogTemp, Error, TEXT("UDP (IPv6) packet send failed with socket error: %d"), (int32)SocketError);
	
	switch (SocketError)
	{
	case SE_NO_ERROR:
		UE_LOG(LogTemp, Warning, TEXT("UDP (IPv6) packet send completed but no bytes sent"));
		break;
	case SE_EWOULDBLOCK:
		UE_LOG(LogTemp, Warning, TEXT("UDP (IPv6) send would block - socket buffer full"));
		break;
	case SE_ECONNRESET:
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv6) connection reset by peer"));
		break;
	case SE_ENETDOWN:
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv6) network is down"));
		break;
	case SE_EHOSTUNREACH:
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv6) host unreachable"));
		break;
	case SE_EMSGSIZE:
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv6) message too large"));
		break;
	default:
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv6) packet send failed with socket error: %d"),
			   (int32)SocketError);
	}
	
	return false;
	
}

bool UCrowdyUDPSubsystem::SendUDPv4(const TArray<uint8>& Message)
{
	if (!UDPSocketV4) return false;

	int32 BytesSentNow = 0;
	const bool bUDPPacketSent = UDPSocketV4->Send(Message.GetData(), Message.Num(), BytesSentNow);

	// Handle Stats
	if (bUDPPacketSent && BytesSentNow > 0)
	{
		SentBytesThisSecond += BytesSentNow;
		++SentDatagramsThisSecond;
		++MessagesSentThisSecond;

		return true;
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

	if (!SocketSubsystem)
	{
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv4) packet send failed - unable to get error details"));
		return false;
	}

	const ESocketErrors SocketError = SocketSubsystem->GetLastErrorCode();
	UE_LOG(LogTemp, Error, TEXT("UDP (IPv4) packet send failed with socket error: %d"), (int32)SocketError);

	switch (SocketError)
	{
	case SE_NO_ERROR:
		UE_LOG(LogTemp, Warning, TEXT("UDP (IPv4) packet send completed but no bytes sent"));
		break;
	case SE_EWOULDBLOCK:
		UE_LOG(LogTemp, Warning, TEXT("UDP (IPv4) send would block - socket buffer full"));
		break;
	case SE_ECONNRESET:
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv4) connection reset by peer"));
		break;
	case SE_ENETDOWN:
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv4) network is down"));
		break;
	case SE_EHOSTUNREACH:
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv4) host unreachable"));
		break;
	case SE_EMSGSIZE:
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv4) message too large"));
		break;
	default:
		UE_LOG(LogTemp, Error, TEXT("UDP (IPv4) packet send failed with socket error: %d"),
			   (int32)SocketError);
	}

	return false;
}


void UCrowdyUDPSubsystem::StopUDPListener()
{
	bUDPv6Listen = false;
	
	if (ListenerRunnable)
	{
		ListenerRunnable->Stop();
	}

	if (ListenerThread)
	{
		ListenerThread->Kill(true);
		delete ListenerThread;
		ListenerThread = nullptr;
	}

	if (ListenerRunnable)
	{
		delete ListenerRunnable;
		ListenerRunnable = nullptr;
	}
	UE_LOG(LogTemp, Log, TEXT("UDP IPv6 Listener Loop Stopped"));
}




void UCrowdyUDPSubsystem::CleanupSockets()
{
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	
	if (UDPSocketV6)
	{
		UDPSocketV6->Close();
		SocketSubsystem->DestroySocket(UDPSocketV6);
		UDPSocketV6 = nullptr;
	}
	
	if (UDPSocketV4)
	{
		bUseIPv4 = false;
		UDPSocketV4->Close();
		SocketSubsystem->DestroySocket(UDPSocketV4);
		UDPSocketV4 = nullptr;
	}
}

void UCrowdyUDPSubsystem::UpdateUDPStats()
{
	LastSecondReceivedBytes = ReceivedBytesThisSecond.exchange(0);
	LastSecondSentBytes = SentBytesThisSecond.exchange(0);
	LastSecondReceivedDatagrams = ReceivedDatagramsThisSecond.exchange(0);
	LastSecondSentDatagrams = SentDatagramsThisSecond.exchange(0);
	LastSecondMessagesReceived = MessagesReceivedThisSecond.exchange(0);
	LastSecondMessagesSent = MessagesSentThisSecond.exchange(0);
}

// ─── Connection state helpers ─────────────────────────────────────────────────

EUDPConnectionState UCrowdyUDPSubsystem::GetConnectionState() const
{
	return static_cast<EUDPConnectionState>(ConnectionState.load(std::memory_order_relaxed));
}

void UCrowdyUDPSubsystem::SetConnectionState(const EUDPConnectionState NewState)
{
	ConnectionState.store(static_cast<uint8>(NewState), std::memory_order_relaxed);
}

void UCrowdyUDPSubsystem::SetPreferredProtocol(const ECrowdyUDPProtocol Protocol)
{
	PreferredProtocol = Protocol;
}