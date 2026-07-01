// Fill out your copyright notice in the Description page of Project Settings.

#include "Auth/FCrowdyTokenVault.h"

#include "Security/FCrowdySecretFile.h"
#include "Misc/Paths.h"

FString FCrowdyTokenVault::GetCredentialFilePath()
{
	return FPaths::ProjectSavedDir() / TEXT("CrowdyStudio/credentials.bin");
}

bool FCrowdyTokenVault::Save(const FString& Token)
{
	return FCrowdySecretFile::SaveString(GetCredentialFilePath(), Token);
}

bool FCrowdyTokenVault::Load(FString& OutToken)
{
	return FCrowdySecretFile::LoadString(GetCredentialFilePath(), OutToken);
}

void FCrowdyTokenVault::Clear()
{
	FCrowdySecretFile::Delete(GetCredentialFilePath());
}
