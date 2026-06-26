// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Reads and writes a secret string to a file under Saved/, encrypted per-user with DPAPI on
 * Windows. On platforms with no DPAPI equivalent the bytes are written in the clear with a
 * loud warning. Shared by the admin-token vault and the per-environment private-key store.
 */
class FCrowdySecretFile
{
public:
	static bool SaveString(const FString& Path, const FString& Secret);
	static bool LoadString(const FString& Path, FString& OutSecret);
	static void Delete(const FString& Path);
};
