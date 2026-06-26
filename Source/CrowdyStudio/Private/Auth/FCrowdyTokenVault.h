// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Per-user storage for the studio admin token, kept under Saved/ never in Config or VCS.
 * On Windows the bytes are DPAPI-encrypted for the current user; on other platforms the token
 * is written in the clear with a loud warning, since there is no equivalent at-rest protection.
 */
class FCrowdyTokenVault
{
public:
	static bool Save(const FString& Token);
	static bool Load(FString& OutToken);
	static void Clear();

private:
	static FString GetCredentialFilePath();
};
