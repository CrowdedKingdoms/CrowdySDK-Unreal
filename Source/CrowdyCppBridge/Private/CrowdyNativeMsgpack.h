#pragma once

#include "CoreMinimal.h"

namespace CrowdyNativeMsgpack
{
	// CrowdyCPP's converter refuses MessagePack nested deeper than this.
	constexpr int32 MaxDepth = 64;

	// CrowdyCPP's converter writes a non-string map key as escaped JSON text, which doubles at every nesting level, so
	// every untrusted value is scanned first and a container, binary or extension key is refused.
	inline bool KeysSafe(TConstArrayView<uint8> Bytes)
	{
		struct FFrame
		{
			uint64 Left;
			bool bMap;
		};
		TArray<FFrame, TInlineAllocator<MaxDepth>> Stack;
		const uint64 Size = static_cast<uint64>(Bytes.Num());
		uint64 Pos = 0;
		bool bRootRead = false;
		auto ReadWidth = [&Bytes, &Pos, Size](int32 Width, uint64& Out)
		{
			if (Size - Pos < static_cast<uint64>(Width))
			{
				return false;
			}
			Out = 0;
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out = (Out << 8) | Bytes[static_cast<int32>(Pos++)];
			}
			return true;
		};
		while (true)
		{
			bool bKey = false;
			if (!Stack.IsEmpty())
			{
				FFrame& Top = Stack.Last();
				if (Top.Left == 0)
				{
					Stack.Pop(EAllowShrinking::No);
					continue;
				}
				bKey = Top.bMap && Top.Left % 2 == 0;
				--Top.Left;
			}
			else if (bRootRead)
			{
				return true;
			}
			bRootRead = true;
			if (Pos >= Size)
			{
				return false;
			}
			const uint8 Tag = Bytes[static_cast<int32>(Pos++)];
			uint64 Count = 0;
			uint64 Skip = 0;
			int32 Kind = 0;
			if (Tag <= 0x7f || Tag >= 0xe0 || Tag == 0xc0 || Tag == 0xc2 || Tag == 0xc3)
			{
				Kind = 0;
			}
			else if (Tag >= 0xa0 && Tag <= 0xbf)
			{
				Skip = Tag & 0x1f;
			}
			else if (Tag >= 0x90 && Tag <= 0x9f)
			{
				Kind = 1;
				Count = Tag & 0x0f;
			}
			else if (Tag >= 0x80 && Tag <= 0x8f)
			{
				Kind = 2;
				Count = Tag & 0x0f;
			}
			else
			{
				switch (Tag)
				{
				case 0xc4: case 0xc5: case 0xc6:
					Kind = 3;
					if (!ReadWidth(Tag == 0xc4 ? 1 : (Tag == 0xc5 ? 2 : 4), Skip)) { return false; }
					break;
				case 0xc7: case 0xc8: case 0xc9:
					Kind = 3;
					if (!ReadWidth(Tag == 0xc7 ? 1 : (Tag == 0xc8 ? 2 : 4), Skip)) { return false; }
					++Skip;
					break;
				case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8:
					Kind = 3;
					Skip = 1 + (1ull << (Tag - 0xd4));
					break;
				case 0xca: case 0xce: case 0xd2: Skip = 4; break;
				case 0xcb: case 0xcf: case 0xd3: Skip = 8; break;
				case 0xcc: case 0xd0: Skip = 1; break;
				case 0xcd: case 0xd1: Skip = 2; break;
				case 0xd9: case 0xda: case 0xdb:
					if (!ReadWidth(Tag == 0xd9 ? 1 : (Tag == 0xda ? 2 : 4), Skip)) { return false; }
					break;
				case 0xdc: case 0xdd:
					Kind = 1;
					if (!ReadWidth(Tag == 0xdc ? 2 : 4, Count)) { return false; }
					break;
				case 0xde: case 0xdf:
					Kind = 2;
					if (!ReadWidth(Tag == 0xde ? 2 : 4, Count)) { return false; }
					break;
				default:
					return false;
				}
			}
			if (bKey && Kind != 0)
			{
				return false;
			}
			if (Size - Pos < Skip)
			{
				return false;
			}
			Pos += Skip;
			if (Kind == 1 || Kind == 2)
			{
				if (Stack.Num() >= MaxDepth)
				{
					return false;
				}
				Stack.Add({Kind == 2 ? Count * 2 : Count, Kind == 2});
			}
		}
	}
}
