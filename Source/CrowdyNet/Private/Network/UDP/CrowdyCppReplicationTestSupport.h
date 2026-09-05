#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "CrowdyCppReplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Serialization/CrowdyWireParitySupport.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

// Shared fixtures for the routed transport tests. Everything here is inline in one header on purpose:
// duplicating a file-local helper across two translation units of a module is a redefinition hazard under
// adaptive unity builds.
namespace CrowdyReplicationTestSupport
{
	constexpr EAutomationTestFlags CrowdyReplicationTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Long enough that a loaded machine still gets there, short enough that a genuine hang is reported as one
	// rather than stalling the suite.
	constexpr double DefaultWaitSeconds = 5.0;

	template <typename TPredicate>
	bool WaitUntil(TPredicate&& Predicate, const double TimeoutSeconds = DefaultWaitSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			if (Predicate())
			{
				return true;
			}
			FPlatformProcess::Sleep(0.005f);
		}
		return Predicate();
	}

	inline FCrowdyCppReplicationToken GoodToken(const int64 GameTokenId = 123456789)
	{
		FCrowdyCppReplicationToken Token;
		Token.bOk = true;
		Token.Token = FString(ANSI_TO_TCHAR(CrowdyWireParity::GoldenToken()));
		Token.GameTokenId = GameTokenId;
		// Non-expiring, so nothing in these tests can trip the proactive refresh and reach a callback that is
		// deliberately not implemented.
		Token.ExpiresAtEpochMs = 0;
		return Token;
	}

	inline FCrowdyCppAssignServer AlwaysRefuse()
	{
		return [](const FCrowdyCppShouldAbort&)
		{
			FCrowdyCppSessionAssignment Answer;
			Answer.bOk = false;
			Answer.ErrorMessage = TEXT("no server for this test");
			return Answer;
		};
	}

	inline FCrowdyCppRefreshToken NeverRefresh()
	{
		return [](const FCrowdyCppShouldAbort&)
		{
			FCrowdyCppReplicationToken Answer;
			Answer.bOk = false;
			Answer.ErrorMessage = TEXT("no refresh in this test");
			return Answer;
		};
	}

	inline FCrowdyCppAssignServer AssignTo(const int32 Port)
	{
		return [Port](const FCrowdyCppShouldAbort&)
		{
			FCrowdyCppSessionAssignment Answer;
			Answer.bOk = true;
			Answer.Ip4 = TEXT("127.0.0.1");
			Answer.ClientPort = Port;
			return Answer;
		};
	}

	inline TArrayView<const uint8> GoldenUuidView()
	{
		return TArrayView<const uint8>(
			reinterpret_cast<const uint8*>(CrowdyWireParity::GoldenUuid()), 32);
	}

	inline FString GoldenTokenString()
	{
		return FString(ANSI_TO_TCHAR(CrowdyWireParity::GoldenToken()));
	}

	/** The datagram Unreal's own encoder produces for one spatial message, signature and tail included. */
	inline TArray<uint8> ExpectedSpatial(const ECrowdyMessageType Type, const int64 AppId, const int64 ChunkX,
		const int64 ChunkY, const int64 ChunkZ, const TArray<uint8>& Payload, const uint8 Distance, const uint8 Decay,
		const int64 TailValue, const uint8 Sequence)
	{
		CrowdyWireParity::FParitySpatialMessage Message;
		Message.TypeOverride = Type;
		Message.AppID = AppId;
		Message.ChunkX = ChunkX;
		Message.ChunkY = ChunkY;
		Message.ChunkZ = ChunkZ;
		Message.ReplicationDistance = static_cast<ECrowdyReplicationDistance>(Distance);
		Message.DecayRate = static_cast<ECrowdyDecayRate>(Decay);
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.PayloadBytes = Payload;

		return CrowdyWireParity::AppendSignedTail(Message.Serialize(), GoldenTokenString(), TailValue, Sequence, true);
	}

	/**
	 * A bound local socket standing in for the replication server, so a send can be read back off the wire and
	 * compared byte for byte, and so a frame can be pushed the other way to exercise the receive path.
	 */
	struct FLoopbackServer
	{
		FSocket* Socket = nullptr;
		int32 Port = 0;
		TSharedPtr<FInternetAddr> ClientAddress;

		bool Open()
		{
			ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (!Subsystem)
			{
				return false;
			}

			Socket = Subsystem->CreateSocket(NAME_DGram, TEXT("CrowdyReplicationTestServer"),
				FNetworkProtocolTypes::IPv4);
			if (!Socket)
			{
				return false;
			}

			const TSharedRef<FInternetAddr> Bind = Subsystem->CreateInternetAddr();
			bool bValid = false;
			Bind->SetIp(TEXT("127.0.0.1"), bValid);
			Bind->SetPort(0);

			// Without this a receive that never arrives blocks forever, which turns a parity failure into a
			// stalled automation run instead of a reported one.
			if (!bValid || !Socket->Bind(*Bind) || !Socket->SetNonBlocking(true))
			{
				Close();
				return false;
			}

			Port = Socket->GetPortNo();
			if (Port <= 0)
			{
				Close();
				return false;
			}
			return true;
		}

		void Close()
		{
			if (!Socket)
			{
				return;
			}
			Socket->Close();
			if (ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
			{
				Subsystem->DestroySocket(Socket);
			}
			Socket = nullptr;
			Port = 0;
		}

		/** Read one datagram, waiting up to the timeout, and remember who sent it. Empty when nothing arrived. */
		TArray<uint8> Receive(const double TimeoutSeconds = DefaultWaitSeconds)
		{
			TArray<uint8> Out;
			ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (!Socket || !Subsystem)
			{
				return Out;
			}

			const TSharedRef<FInternetAddr> From = Subsystem->CreateInternetAddr();
			TArray<uint8> Buffer;
			Buffer.SetNumUninitialized(2048);

			const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
			while (FPlatformTime::Seconds() < Deadline)
			{
				int32 Read = 0;
				if (Socket->RecvFrom(Buffer.GetData(), Buffer.Num(), Read, *From) && Read > 0)
				{
					ClientAddress = From;
					Out.Append(Buffer.GetData(), Read);
					return Out;
				}
				FPlatformProcess::Sleep(0.005f);
			}
			return Out;
		}

		/**
		 * Read datagrams until one ends in the wanted sequence number. Identifying the datagram by its own content
		 * rather than by how many were sent before it leaves nothing to race.
		 */
		TArray<uint8> ReceiveWithSequence(const uint8 Sequence, const double TimeoutSeconds = DefaultWaitSeconds)
		{
			const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
			while (FPlatformTime::Seconds() < Deadline)
			{
				const TArray<uint8> Datagram = Receive(0.1);
				if (Datagram.Num() > 0 && Datagram.Last() == Sequence)
				{
					return Datagram;
				}
			}
			return TArray<uint8>();
		}

		/** Push a datagram back at whoever last sent one. Requires a prior Receive to have learned the address. */
		bool SendToClient(const TArray<uint8>& Datagram) const
		{
			if (!Socket || !ClientAddress.IsValid())
			{
				return false;
			}
			int32 Sent = 0;
			return Socket->SendTo(Datagram.GetData(), Datagram.Num(), Sent, *ClientAddress) && Sent == Datagram.Num();
		}
	};

	/** A connected facade plus the stand-in server it is talking to, so each test does not rebuild the same scaffold. */
	struct FConnectedFixture
	{
		FLoopbackServer Server;
		TSharedPtr<FCrowdyCppReplication> Connection;

		/**
		 * A test that deliberately provokes a re-assignment should pass zero for the floor, so the re-assignment
		 * completes instead of waiting out several seconds and then reporting a failure when the test tears down.
		 */
		bool Open(const int64 AppId = 7, const int64 GameTokenId = 123456789,
			const int64 MinReassignIntervalMs = 5000)
		{
			if (!Server.Open())
			{
				return false;
			}

			FCrowdyCppReplicationConfig Config;
			Config.AppId = AppId;
			Config.Token = GoodToken(GameTokenId);
			// Nothing but the message under test may reach the socket, and nothing may re-assign underneath it.
			Config.SessionReadyWaitMs = 0;
			Config.RefreshLeadMs = 0;
			Config.WatchdogSilenceMs = 0;
			Config.MinReassignIntervalMs = MinReassignIntervalMs;

			Connection = FCrowdyCppReplication::Make(Config, AssignTo(Server.Port), NeverRefresh());
			if (!Connection.IsValid())
			{
				Server.Close();
				return false;
			}

			Connection->ConnectAsync();
			const bool bAssigned = WaitUntil([this]() { return Connection->GetAssignment().bOk; });
			if (!bAssigned)
			{
				Shut();
				return false;
			}
			return true;
		}

		/**
		 * One send so the stand-in server learns which address to answer, since a datagram socket has nowhere to
		 * push to until it has heard from the client.
		 */
		bool Prime()
		{
			FString Error;
			const TArray<uint8> Ping = {0x01};
			if (!Connection.IsValid()
				|| !Connection->SendSpatial(140, 0, 0, 0, GoldenUuidView(), Ping, 8, 0, Error))
			{
				return false;
			}
			return Server.Receive().Num() > 0;
		}

		void Shut()
		{
			if (Connection.IsValid())
			{
				Connection->Disconnect();
				Connection.Reset();
			}
			Server.Close();
		}
	};

}

#endif
