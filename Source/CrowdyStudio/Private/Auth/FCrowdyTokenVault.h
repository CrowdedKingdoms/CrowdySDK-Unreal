// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Per-user storage for the studio admin token, kept under Saved/ never in Config or VCS.
 * On Windows the bytes are DPAPI-encrypted for the current user; on other platforms the token
 * is written in the clear with a loud warning, since there is no equivalent at-rest protection.
 *
 * The record persists the sign-in *scope* and user id alongside the token so a remembered SESSION
 * token is restored as session-scoped (mint-capable) rather than being downgraded to an org token
 * on the next editor launch otherwise game-plane authoring would refuse to mint an app token
 * until the user signed in again. Scope is an int (mirrors ECrowdyStudioAuthScope) so the vault
 * stays free of the controller's enum. A legacy file that holds only a bare token still loads: it
 * is treated as an org token (the pre-record behaviour), so nothing that was already remembered
 * breaks the correct scope is captured on the next sign-in.
 */
class FCrowdyTokenVault
{
public:
	// Persist the token plus its sign-in scope and user id.
	static bool Save(const FString& Token, int32 Scope, int64 UserId);
	// Full record load. On a legacy bare-token file: OutToken is the token, OutScope is the org-token
	// value (2), OutUserId is 0. Returns false only when nothing is stored.
	static bool Load(FString& OutToken, int32& OutScope, int64& OutUserId);
	// Convenience for callers that only need the raw token (e.g. edit-time authenticated queries).
	static bool Load(FString& OutToken);
	static void Clear();

private:
	static FString GetCredentialFilePath();
};
