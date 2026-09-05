#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Containers/StringConv.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"

#include <type_traits>

/**
 * The 32 octets that address an actor on the wire.
 *
 * The server treats these as 32 opaque bytes with no interior structure and no terminator. They are
 * lowercase hexadecimal by convention, which is what makes a 128-bit key derivable from them, but that
 * is a convention and not a guarantee: a server-originated event writes two binary GUIDs into the same
 * region, and a server-originated channel notification leaves it empty or filled with something that is
 * not text. So the value is carried as octets and read as text only where a layout says it is text.
 *
 * Being fixed size is the point. The header writes the id with no length prefix and no padding, so a
 * value of any other length would shift every field behind it and the payload could no longer be found
 * by offset. A type that cannot hold any other length cannot cause that.
 *
 * There is deliberately no implicit conversion to or from a string. Text becomes an id only through a
 * call that checks its length, so a value of the wrong length is refused at the point it is read rather
 * than silently reshaped somewhere further down.
 *
 * Equality is byte exact, so two ids that differ only in the case of a hexadecimal digit are different
 * ids. That is stricter than comparing the same values as strings, since FString compares without regard
 * to case.
 */
struct FCrowdyActorId
{
	static constexpr int32 NumOctets = 32;

	uint8 Octets[NumOctets] = {};

	/** Copies exactly 32 octets. Returns false, leaving the id unset, for a run of any other length. */
	[[nodiscard]] static bool TryFromOctets(const TConstArrayView<uint8> InOctets, FCrowdyActorId& OutId)
	{
		if (InOctets.Num() != NumOctets)
		{
			OutId.Reset();
			return false;
		}

		FMemory::Memcpy(OutId.Octets, InOctets.GetData(), NumOctets);
		return true;
	}

	/** The same copy for a caller with nothing to do about a refusal. An unusable run leaves the id unset. */
	static FCrowdyActorId FromOctets(const TConstArrayView<uint8> InOctets)
	{
		FCrowdyActorId Result;
		if (!TryFromOctets(InOctets, Result))
		{
			Result.Reset();
		}

		return Result;
	}

	/**
	 * Reads text as the id, which requires that the text encode to exactly 32 octets in UTF-8. Returns
	 * false, leaving the id unset, for anything else. It never truncates and never pads: either the text
	 * is the id or it is not.
	 */
	[[nodiscard]] static bool TryFromString(const FString& InText, FCrowdyActorId& OutId)
	{
		OutId.Reset();

		// Converted once and measured in octets rather than characters, because the wire counts octets: a
		// string of 32 characters that encodes to 33 is not this id, and truncating it to fit would put a
		// value on the wire that addresses a different actor.
		const FTCHARToUTF8 Converted(*InText);
		if (Converted.Length() != NumOctets)
		{
			return false;
		}

		FMemory::Memcpy(OutId.Octets, Converted.Get(), NumOctets);
		return true;
	}

	/** The same read for a caller with nothing to do about a refusal. Unusable text leaves the id unset. */
	static FCrowdyActorId FromStringOrUnset(const FString& InText)
	{
		FCrowdyActorId Result;
		if (!TryFromString(InText, Result))
		{
			Result.Reset();
		}

		return Result;
	}

	/** FGuid::ToString(EGuidFormats::Digits) as octets, without the string: uppercase, and an unset guid is 32 '0' characters, not 32 zero octets. */
	static FCrowdyActorId FromGuid(const FGuid& InGuid)
	{
		static constexpr ANSICHAR HexDigits[] = "0123456789ABCDEF";

		FCrowdyActorId Result;
		const uint32 Words[4] = { InGuid.A, InGuid.B, InGuid.C, InGuid.D };

		for (int32 WordIndex = 0; WordIndex < 4; ++WordIndex)
		{
			for (int32 DigitIndex = 0; DigitIndex < 8; ++DigitIndex)
			{
				const uint32 Nibble = (Words[WordIndex] >> (28 - DigitIndex * 4)) & 0xFu;
				Result.Octets[WordIndex * 8 + DigitIndex] = static_cast<uint8>(HexDigits[Nibble]);
			}
		}

		return Result;
	}

	/**
	 * Builds an id from a literal of exactly 32 characters. A literal of any other length does not
	 * compile, which is the only construction whose length is settled before the program runs.
	 */
	static FCrowdyActorId FromAnsiLiteral(const ANSICHAR (&InLiteral)[NumOctets + 1])
	{
		FCrowdyActorId Result;
		FMemory::Memcpy(Result.Octets, InLiteral, NumOctets);
		return Result;
	}

	/**
	 * The id as text, or an empty string when it is unset. Octets that are not valid UTF-8 convert the
	 * way any other malformed text does, so this is for display and for callers that hold an id as a
	 * string; it is not a way to move the value.
	 */
	FString ToString() const
	{
		if (!IsSet())
		{
			return FString();
		}

		const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Octets), NumOctets);
		return FString(Converter.Length(), Converter.Get());
	}

	/** False only when every octet is zero, which is what an id that was never filled in looks like. */
	bool IsSet() const
	{
		for (int32 Index = 0; Index < NumOctets; ++Index)
		{
			if (Octets[Index] != 0)
			{
				return true;
			}
		}

		return false;
	}

	void Reset()
	{
		FMemory::Memzero(Octets, NumOctets);
	}

	TConstArrayView<uint8> AsOctets() const { return TConstArrayView<uint8>(Octets, NumOctets); }

	/** Appends all 32 octets, whatever they hold. */
	void AppendTo(TArray<uint8>& OutBytes) const
	{
		OutBytes.Append(Octets, NumOctets);
	}

	/** Writes all 32 octets to a buffer the caller has already sized. */
	void WriteTo(uint8* Dest) const
	{
		FMemory::Memcpy(Dest, Octets, NumOctets);
	}

	friend bool operator==(const FCrowdyActorId& A, const FCrowdyActorId& B)
	{
		return FMemory::Memcmp(A.Octets, B.Octets, NumOctets) == 0;
	}

	friend bool operator!=(const FCrowdyActorId& A, const FCrowdyActorId& B) { return !(A == B); }

	friend uint32 GetTypeHash(const FCrowdyActorId& Id)
	{
		return FCrc::MemCrc32(Id.Octets, FCrowdyActorId::NumOctets);
	}
};

static_assert(sizeof(FCrowdyActorId) == FCrowdyActorId::NumOctets,
	"An actor id is exactly the 32 octets it occupies on the wire.");
static_assert(std::is_trivially_copyable_v<FCrowdyActorId>,
	"An actor id is copied by value on every message, so copying it must be a memory copy.");
