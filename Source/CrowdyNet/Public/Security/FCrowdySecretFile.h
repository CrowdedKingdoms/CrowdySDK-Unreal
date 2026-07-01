// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Reads and writes a secret to a file under Saved/, encrypted per-user with DPAPI on Windows
 * (CryptProtectData / CryptUnprotectData). On platforms with no DPAPI equivalent, the bytes are
 * written in the clear with a loud warning, since there is no equivalent at-rest protection.
 *
 * Shared SDK primitive: the editor admin-token vault (FCrowdyTokenVault, CrowdyStudio) and the
 * runtime session-token store (UCrowdyAuthentication, CrowdyServices) both build on it.
 */
class CROWDYNET_API FCrowdySecretFile
{
public:
	/** Encrypt and write raw bytes which is the lowest-level primitive the others delegate to. */
	static bool SaveBytes(const FString& Path, const TArray<uint8>& Plain);

	/** Read and decrypt bytes written by SaveBytes. Returns false if the file is absent or
	 *  could not be decrypted (e.g. written by a different user). */
	static bool LoadBytes(const FString& Path, TArray<uint8>& OutPlain);

	/** UTF-8 string convenience over the byte API. */
	static bool SaveString(const FString& Path, const FString& Secret);
	static bool LoadString(const FString& Path, FString& OutSecret);

	static void Delete(const FString& Path);
};
