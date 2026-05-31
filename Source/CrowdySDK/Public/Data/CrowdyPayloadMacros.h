#pragma once


//Struct authoring

//  Mark any USTRUCT as a Crowdy event payload:
//
//    USTRUCT(BlueprintType, CROWDY_EVENT)
//    struct FPlayerDiedEvent { GENERATED_BODY() ... };
//
//  Mark any USTRUCT as a Crowdy actor update payload:
//
//    USTRUCT(BlueprintType, CROWDY_ACTOR_UPDATE)
//    struct FPlayerTransformUpdate { GENERATED_BODY() ... };
//
//  The ID is derived from the struct's asset path at startup.

// NOTE: These macros CANNOT be used directly inside USTRUCT() specifiers.
// UHT parses specifiers before the C++ preprocessor runs, so macros
// are invisible to it. Write the meta directly instead:
//
//   USTRUCT(BlueprintType, meta=(CrowdyCategory="Event"))
//   USTRUCT(BlueprintType, meta=(CrowdyCategory="ActorUpdate"))
//
// The macros below are kept only as documentation of valid category strings.

#define CROWDY_EVENT meta =(CrowdyRep="Event")
#define CROWDY_ACTOR_UPDATE meta(CrowdyRep="ActorUpdate")