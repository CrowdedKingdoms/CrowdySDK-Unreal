#pragma once

#include "CoreMinimal.h"

/**
 * Entries an actor update carries after its state block, each [uint16 type][uint16 length][length octets].
 *
 * They sit past the length the state block declares, so a reader that knows only the state never reaches them:
 * that is what lets a newer sender add them without breaking an older receiver.
 */
namespace CrowdyUpdateExtras
{
	constexpr int32 EntryHeaderOctets = 2 * sizeof(uint16);

	/** Typed values that ride beside the movement, led by the int64 layout hash they were encoded with. */
	constexpr uint16 ContinuousType = 1;

	/** The most an actor update's payload may hold: what a single signed datagram carries. */
	constexpr int32 MaxPayloadOctets = 1123;

	/** Whether extras of this size still fit in one datagram beside a state block of that size and its length field. */
	inline bool FitsBesideState(const int32 StateOctets, const int32 ExtrasOctets)
	{
		return ExtrasOctets <= MaxPayloadOctets - static_cast<int32>(sizeof(int32)) - StateOctets;
	}

	/** Grows Extras by one entry header and returns where its body goes; null for type 0 or a body over 65535 octets. */
	inline uint8* AddEntry(TArray<uint8>& Extras, const uint16 Type, const int32 BodyOctets)
	{
		if (Type == 0 || BodyOctets < 0 || BodyOctets > MAX_uint16)
		{
			return nullptr;
		}

		const uint16 Length = static_cast<uint16>(BodyOctets);
		const int32 Base = Extras.Num();
		Extras.SetNumUninitialized(Base + EntryHeaderOctets + BodyOctets);
		uint8* Entry = Extras.GetData() + Base;
		FMemory::Memcpy(Entry, &Type, sizeof(Type));
		FMemory::Memcpy(Entry + sizeof(Type), &Length, sizeof(Length));
		return Entry + EntryHeaderOctets;
	}

	/** Appends one entry. False, leaving Extras unchanged, for type 0 or a body over 65535 octets. */
	inline bool AppendEntry(TArray<uint8>& Extras, const uint16 Type, const TConstArrayView<uint8> Body)
	{
		uint8* Out = AddEntry(Extras, Type, Body.Num());
		if (!Out)
		{
			return false;
		}
		if (Body.Num() > 0)
		{
			FMemory::Memcpy(Out, Body.GetData(), Body.Num());
		}
		return true;
	}

	/** Appends a ContinuousType entry: the layout hash, then the encoded values. */
	inline bool AppendContinuousEntry(TArray<uint8>& Extras, const int64 LayoutHash, const TConstArrayView<uint8> Values)
	{
		uint8* Out = AddEntry(Extras, ContinuousType, static_cast<int32>(sizeof(LayoutHash)) + Values.Num());
		if (!Out)
		{
			return false;
		}
		FMemory::Memcpy(Out, &LayoutHash, sizeof(LayoutHash));
		if (Values.Num() > 0)
		{
			FMemory::Memcpy(Out + sizeof(LayoutHash), Values.GetData(), Values.Num());
		}
		return true;
	}

	/** Splits a ContinuousType entry's body. False when it is too short to hold the layout hash. */
	inline bool ReadContinuousEntry(const TConstArrayView<uint8> Body, int64& OutLayoutHash, TConstArrayView<uint8>& OutValues)
	{
		if (Body.Num() < static_cast<int32>(sizeof(OutLayoutHash)))
		{
			return false;
		}
		FMemory::Memcpy(&OutLayoutHash, Body.GetData(), sizeof(OutLayoutHash));
		OutValues = Body.RightChop(sizeof(OutLayoutHash));
		return true;
	}

	/** Calls Visit(Type, Body) for each entry in order. False when an entry is type 0 or runs past the end. */
	template <typename FVisit>
	bool ForEachEntry(const TConstArrayView<uint8> Extras, FVisit&& Visit)
	{
		int32 Offset = 0;
		while (Offset < Extras.Num())
		{
			if (Extras.Num() - Offset < EntryHeaderOctets)
			{
				return false;
			}

			uint16 Type = 0;
			uint16 Length = 0;
			FMemory::Memcpy(&Type, Extras.GetData() + Offset, sizeof(Type));
			FMemory::Memcpy(&Length, Extras.GetData() + Offset + sizeof(Type), sizeof(Length));
			Offset += EntryHeaderOctets;

			if (Type == 0 || Length > Extras.Num() - Offset)
			{
				return false;
			}

			Visit(Type, Extras.Slice(Offset, Length));
			Offset += Length;
		}
		return true;
	}

	inline bool IsWellFormed(const TConstArrayView<uint8> Extras)
	{
		return ForEachEntry(Extras, [](uint16, TConstArrayView<uint8>) {});
	}

	/** The body of the first entry of this type, or an empty view when there is none. */
	inline TConstArrayView<uint8> FindEntry(const TConstArrayView<uint8> Extras, const uint16 Type)
	{
		TConstArrayView<uint8> Found;
		bool bFound = false;
		ForEachEntry(Extras, [&](const uint16 EntryType, const TConstArrayView<uint8> Body)
		{
			if (!bFound && EntryType == Type)
			{
				Found = Body;
				bFound = true;
			}
		});
		return Found;
	}

	/** crowdy.net.receive.updateextras: 0 makes a receiver stop at the state block, exactly as an older one does. */
	CROWDYNET_API bool IsReceiveEnabled();

	/** Counts a malformed run of extras and reports it, rate limited because a peer can send one with every update. */
	CROWDYNET_API void ReportMalformed(int32 Octets);

	CROWDYNET_API int64 GetMalformedCount();
}
