#pragma once

#include "CoreMinimal.h"
#include "Serialization/MemoryReader.h"

// An FMemoryReader whose ArMaxSerializeSize is pinned to the blob length so a forged length prefix in an
// untrusted payload cannot drive an unbounded allocation. A plain FMemoryReader leaves ArMaxSerializeSize == 0,
// which disables the FString/FName load path's `(MaxSerializeSize > 0) && (SaveNum > MaxSerializeSize)`
// self-protection: the untrusted int32 length would then call AddUninitialized() for a multi-GB allocation
// before the short char read is detected (remote OOM from a tiny packet). No length prefix inside a blob can
// legitimately exceed the blob itself, so the blob size is the tightest correct cap, and the engine rejects
// SaveNum > blob-size up front so the decode drops cleanly.
class FCrowdyBoundedMemoryReader : public FMemoryReader
{
public:
	FCrowdyBoundedMemoryReader(const TArray<uint8>& InBytes, bool bIsPersistent)
		: FMemoryReader(InBytes, bIsPersistent)
	{
		ArMaxSerializeSize = InBytes.Num();
	}
};
