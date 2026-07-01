// Fill out your copyright notice in the Description page of Project Settings.

#include "Security/FCrowdySecretFile.h"

#include "CrowdyNetLog.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincrypt.h>
#include <dpapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

bool FCrowdySecretFile::SaveBytes(const FString& Path, const TArray<uint8>& Plain)
{
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(Path));

#if PLATFORM_WINDOWS
	DATA_BLOB In;
	// CryptProtectData treats the input as read-only; the Win32 struct field is simply non-const.
	In.pbData = const_cast<uint8*>(Plain.GetData());
	In.cbData = static_cast<DWORD>(Plain.Num());

	DATA_BLOB Out = {};
	if (!CryptProtectData(&In, L"CrowdySDK", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out))
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("Secret store: CryptProtectData failed (0x%08x)."),
			static_cast<uint32>(GetLastError()));
		return false;
	}

	TArray<uint8> Encrypted;
	Encrypted.Append(Out.pbData, static_cast<int32>(Out.cbData));
	LocalFree(Out.pbData);

	if (!FFileHelper::SaveArrayToFile(Encrypted, *Path))
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("Secret store: failed to write %s."), *Path);
		return false;
	}
	return true;
#else
	UE_LOG(LogCrowdyNet, Warning,
		TEXT("Secret store: this platform has no DPAPI — the secret is stored UNENCRYPTED at %s."), *Path);
	return FFileHelper::SaveArrayToFile(Plain, *Path);
#endif
}

bool FCrowdySecretFile::LoadBytes(const FString& Path, TArray<uint8>& OutPlain)
{
	OutPlain.Reset();

	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *Path))
	{
		return false;
	}

#if PLATFORM_WINDOWS
	DATA_BLOB In;
	In.pbData = FileBytes.GetData();
	In.cbData = static_cast<DWORD>(FileBytes.Num());

	DATA_BLOB Out = {};
	if (!CryptUnprotectData(&In, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out))
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("Secret store: CryptUnprotectData failed (0x%08x)."),
			static_cast<uint32>(GetLastError()));
		return false;
	}

	OutPlain.Append(Out.pbData, static_cast<int32>(Out.cbData));
	LocalFree(Out.pbData);
	return true;
#else
	OutPlain = MoveTemp(FileBytes);
	return true;
#endif
}

bool FCrowdySecretFile::SaveString(const FString& Path, const FString& Secret)
{
	const FTCHARToUTF8 Utf8(*Secret);
	TArray<uint8> PlainBytes;
	PlainBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return SaveBytes(Path, PlainBytes);
}

bool FCrowdySecretFile::LoadString(const FString& Path, FString& OutSecret)
{
	OutSecret.Empty();

	TArray<uint8> PlainBytes;
	if (!LoadBytes(Path, PlainBytes))
	{
		return false;
	}

	const FUTF8ToTCHAR Converter(reinterpret_cast<const char*>(PlainBytes.GetData()), PlainBytes.Num());
	OutSecret = FString(Converter.Length(), Converter.Get());
	return true;
}

void FCrowdySecretFile::Delete(const FString& Path)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*Path))
	{
		PlatformFile.DeleteFile(*Path);
	}
}
