#pragma once

#include "CoreMinimal.h"

/** The CLIENT half's starter crate on crowdy-client-sdk; the platform serves none. */
namespace CrowdyStudioClientStarter
{
	inline const TCHAR* const CargoPath = TEXT("Cargo.toml");
	inline const TCHAR* const LibPath = TEXT("src/lib.rs");

	inline const TCHAR* const CargoToml =
		TEXT("[package]\n")
		TEXT("name = \"client-half\"\n")
		TEXT("version = \"0.1.0\"\n")
		TEXT("edition = \"2021\"\n")
		TEXT("\n")
		TEXT("[lib]\n")
		TEXT("crate-type = [\"cdylib\"]\n")
		TEXT("\n")
		TEXT("[dependencies]\n")
		TEXT("crowdy-client-sdk = \"0.1.0\"\n");

	inline const TCHAR* const LibRs =
		TEXT("use crowdy_client_sdk as crowdy;\n")
		TEXT("\n")
		TEXT("fn init() {\n")
		TEXT("    crowdy::log(1, \"ready\");\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn tick(_dt_ms: u32) {}\n")
		TEXT("\n")
		TEXT("fn invoke(payload: &[u8]) -> Vec<u8> {\n")
		TEXT("    payload.to_vec()\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn event(_event: &[u8]) {}\n")
		TEXT("\n")
		TEXT("crowdy::register_module!(init: init, tick: tick, invoke: invoke, event: event);\n");
}
