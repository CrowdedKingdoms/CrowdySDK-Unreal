// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "HAL/IConsoleManager.h"

// Shared logging conventions for the CrowdySDK modules.
//
// Each module declares its own log category (DECLARE/DEFINE_LOG_CATEGORY) so its output is
// filterable on its own — e.g. `Log LogCrowdyNet Verbose` — instead of disappearing into the
// engine's generic LogTemp.
//
// On top of that, chatty per-frame / per-packet / per-message trace lines are gated behind an
// off-by-default console variable, mirroring the original crowdy.rpc.trace. The macro below is
// the single place the convention lives: an int32 CVar that defaults to 0 (off) and is editable
// at runtime. Define each one exactly once, in its module's .cpp, inside an anonymous namespace:
//
//     namespace
//     {
//         CROWDY_DEFINE_TRACE_CVAR(CVarCrowdyNetTrace, TEXT("crowdy.net.trace"),
//             TEXT("When non-zero, logs CrowdyNet UDP transport activity."));
//     }
//     bool CrowdyNetTrace::Net() { return CVarCrowdyNetTrace.GetValueOnAnyThread() != 0; }
//
// Expose a small accessor (like CrowdyNetTrace::Net above) from the module's log header so call
// sites read `if (CrowdyNetTrace::Net()) { UE_LOG(LogCrowdyNet, Log, ...); }` without seeing the
// CVar directly. Use GetValueOnAnyThread for anything that can log off the game thread.
#define CROWDY_DEFINE_TRACE_CVAR(VarName, CommandText, HelpText) \
	TAutoConsoleVariable<int32> VarName(CommandText, 0, HelpText, ECVF_Default)
