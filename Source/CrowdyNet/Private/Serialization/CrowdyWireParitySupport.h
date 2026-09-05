#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Serialization/CrowdyFrame.h"
#include "Serialization/CrowdyFrameSplit.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "CrowdyCppCrypto.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/wire/protocol.hpp"
#include "crowdy/wire/codec.hpp"
#include "crowdy/core/crypto.hpp"
THIRD_PARTY_INCLUDES_END

// Shared fixtures for the datagram byte-parity harness. Everything here is inline in one header on
// purpose: duplicating a file-local helper across two translation units of a module is a redefinition
// hazard under adaptive unity builds.
namespace CrowdyWireParity
{
	// The harness drives the same crypto provider the plugin uses at runtime, exported from the
	// bridge module rather than duplicated here. That is what makes the golden vectors below
	// establish the correctness of the real provider, not a test-only twin of it.
	inline const crowdy::core::ICrypto& Crypto()
	{
		return GetCrowdyCppCrypto();
	}

	// The token and actor id behind both golden vectors. The token must be exactly 64 characters; the
	// signing scheme uses its raw octets both as the HMAC key and as a suffix on the signed message.
	inline const ANSICHAR* GoldenToken()
	{
		return "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+/";
	}

	inline const ANSICHAR* GoldenUuid()
	{
		return "0123456789abcdef0123456789abcdef";
	}

	// The same 32 octets as an actor id. The raw characters above are kept alongside it because the byte
	// fixtures and the shared codec's parameter structs want the octets, not the id type.
	inline FCrowdyActorId GoldenActorId()
	{
		return FCrowdyActorId::FromAnsiLiteral("0123456789abcdef0123456789abcdef");
	}

	// A signed opcode-140 datagram, derived independently from the published wire-format and HMAC
	// documentation rather than from either implementation. This is the harness's only third-party
	// anchor: cross-checking the two implementations against each other cannot catch them drifting
	// together, and this vector can.
	// appId=7, chunk=(1,-2,3), distance=8, decay=Exponential, payload=de:ad:be:ef,
	// gameTokenId=123456789, sequence=42.
	inline TArray<uint8> GoldenSpatialDatagram()
	{
		static const uint8 Bytes[] = {
			0x8c, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x08, 0x01, 0x01, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
			0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
			0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xde, 0xad, 0xbe, 0xef, 0x43, 0xec, 0xd4,
			0x68, 0xd2, 0x59, 0x3f, 0x17, 0xf3, 0xfb, 0x06, 0x36, 0x8a, 0x2b, 0x6f, 0x47, 0x32, 0xfc,
			0x2d, 0xa1, 0x65, 0xac, 0xf4, 0xca, 0x2b, 0xfe, 0x16, 0x39, 0x6c, 0x43, 0x36, 0x2a, 0x15,
			0xcd, 0x5b, 0x07, 0x00, 0x00, 0x00, 0x00, 0x2a };
		return TArray<uint8>(Bytes, UE_ARRAY_COUNT(Bytes));
	}

	// A COMMAND_RECONNECT frame for the same token: opcode 22 followed by the 32-byte tag over a
	// single covered byte. Unreal has no encoder or verifier for this opcode today.
	inline TArray<uint8> GoldenReconnectFrame()
	{
		static const uint8 Bytes[] = {
			0x16, 0x74, 0x8a, 0x0e, 0x41, 0x6b, 0xaa, 0x63, 0xbe, 0xfb, 0xa6,
			0xff, 0x5f, 0x52, 0x61, 0x1c, 0xc0, 0x93, 0x85, 0x5e, 0x04, 0xe6,
			0x0a, 0xc0, 0x57, 0xe1, 0x4e, 0x8f, 0xbe, 0x19, 0x82, 0x81, 0xe2 };
		return TArray<uint8>(Bytes, UE_ARRAY_COUNT(Bytes));
	}

	inline crowdy::wire::Token64 ParityToken()
	{
		const std::optional<crowdy::wire::Token64> Token = crowdy::wire::Token64::fromString(GoldenToken());
		check(Token.has_value());
		return *Token;
	}

	// A spatial message whose payload the caller supplies outright. The concrete message structs each
	// hardcode their own payload shape, so covering an arbitrary opcode and payload needs this.
	struct FParitySpatialMessage final : ICrowdyMessage
	{
		ECrowdyMessageType TypeOverride = ECrowdyMessageType::GENERIC_SPATIAL_1;
		TArray<uint8> PayloadBytes;

		virtual ECrowdyMessageType GetType() const override { return TypeOverride; }
		virtual FName GetTypeName() const override { return "Wire Parity Message"; }

		virtual TArray<uint8> Serialize() const override
		{
			TArray<uint8> Data = SerializeMetadata();
			Data.Append(PayloadBytes);
			return Data;
		}

		// Send-only: this type is never decoded from the wire.
		[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
		{
			return false;
		}
	};

	// The transport strips the opcode byte and takes the envelope off before a message sees a frame, so a
	// fixture that starts from whole stripped bytes has to do both to describe the same frame a decoder is
	// handed on the wire.
	//
	// Which envelope to take off is decided by the message's own opcode through the production mapping,
	// rather than by the fixture saying so, which is what keeps a fixture from testing a message against
	// a layout it never actually arrives in. Opcode is the byte written onto the frame; it does not
	// influence the split.
	inline bool DecodeStrippedPayload(ICrowdyMessage& Message, const TConstArrayView<uint8> StrippedBytes,
		const uint8 Opcode = 0)
	{
		// The frame keeps the opcode it was split as, which is the state a decoder is handed on the wire.
		// Overriding it afterwards would hand the decoder a frame that cannot arrive.
		FCrowdyFrame Frame(Opcode != 0 ? Opcode : static_cast<uint8>(Message.GetType()), StrippedBytes);
		return CrowdyFrameSplit::ReadEnvelopeForOpcode(Frame) && Message.DecodePayload(Frame);
	}

	// For a message whose decoder reads a layout its declared opcode does not describe, so no envelope
	// can be taken off for it. The whole message after the opcode becomes the body.
	inline bool DecodeWholePayload(ICrowdyMessage& Message, const TConstArrayView<uint8> StrippedBytes,
		const uint8 Opcode = 0)
	{
		return Message.DecodePayload(FCrowdyFrame(Opcode, StrippedBytes));
	}

	// Completes a message body into the datagram that actually leaves the socket. This mirrors the
	// transmission layer, which is the only place the tag and trailer are appended and which cannot be
	// called directly here because it requires a live game session.
	inline TArray<uint8> AppendSignedTail(TArray<uint8> Body, const FString& Token, const int64 GameTokenID,
		const uint8 SequenceNumber, const bool bRequiresAuth)
	{
		if (bRequiresAuth)
		{
			Body.Append(USerializationFunctionLibrary::CalculateHMAC(Body, Token));
		}

		TArray<uint8> GameTokenIDBytes;
		GameTokenIDBytes.SetNumUninitialized(8);
		FMemory::Memcpy(GameTokenIDBytes.GetData(), &GameTokenID, 8);
		Body.Append(GameTokenIDBytes);
		Body.Add(SequenceNumber);
		return Body;
	}

	inline FString ToHex(const TArray<uint8>& Bytes)
	{
		return BytesToHex(Bytes.GetData(), Bytes.Num());
	}

	// Reports the first differing offset so a failure names the field that diverged rather than just
	// the fact that two long buffers are unequal.
	inline FString DescribeDifference(const TArray<uint8>& Actual, const TArray<uint8>& Expected)
	{
		if (Actual.Num() != Expected.Num())
		{
			return FString::Printf(TEXT("length %d, expected %d"), Actual.Num(), Expected.Num());
		}

		for (int32 Index = 0; Index < Actual.Num(); ++Index)
		{
			if (Actual[Index] != Expected[Index])
			{
				return FString::Printf(TEXT("first difference at offset %d: 0x%02x, expected 0x%02x"),
					Index, Actual[Index], Expected[Index]);
			}
		}

		return TEXT("identical");
	}
}

#endif
