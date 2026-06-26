#pragma once

#include "CoreMinimal.h"
#include "Replication/RPC/CrowdyRPC.h"
#include <type_traits>

/**
 * Declares the call-site thunk for an RPC-style CrowdyEvent on any actor or component
 * that carries a UCrowdyEntityComponent — no Crowdy base class required.
 *
 * Calling FuncName(Args...) marshals the arguments by reflection and routes them
 * through the Crowdy transport; the matching FuncName_Implementation runs on the
 * receiving clients.
 *
 * The receiver is a normal reflected UFUNCTION that you declare yourself, tagged
 * meta=(CrowdyEvent) plus the per-function routing keys. It must be a literal
 * UFUNCTION: UnrealHeaderTool does not expand macros when scanning for reflected
 * declarations, so a UFUNCTION emitted from inside this macro would be invisible to
 * reflection. CROWDY_EVENT therefore generates only the (non-reflected) thunk, and
 * the routing values live on the receiver's metadata, where Phase 2 reads and bakes
 * them.
 *
 *   UCLASS()
 *   class AMyWeapon : public AActor
 *   {
 *       GENERATED_BODY()
 *   public:
 *       // Receiver — runs on remotes. Routing lives in its metadata.
 *       UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "OwningClient",
 *           CrowdyDecay = "Exponential_Decay", CrowdyDistance = "Four_Chunks"))
 *       void FireWeapon_Implementation(int32 AmmoUsed, FVector Direction);
 *
 *       // Call site — FireWeapon(...) replicates to FireWeapon_Implementation.
 *       CROWDY_EVENT(FireWeapon)
 *   };
 *
 *   void AMyWeapon::FireWeapon_Implementation(int32 AmmoUsed, FVector Direction)
 *   {
 *       // runs on remotes
 *   }
 *
 * CrowdyRecipient is an ECrowdyEventRecipient name, CrowdyDecay an ECrowdyDecayRate,
 * and CrowdyDistance an ECrowdyReplicationDistance; pass the short enumerator name.
 * Omit a key to take its default (Broadcast / No_Decay / Eight_Chunks).
 */
#define CROWDY_EVENT(FuncName)                                                      \
	template <typename... CrowdyArgTypes>                                           \
	void FuncName(CrowdyArgTypes&&... CrowdyArgs)                                   \
	{                                                                              \
		using CrowdyThisClass = std::remove_pointer_t<decltype(this)>;             \
		FCrowdyRPC::SendChecked(this, TEXT(#FuncName "_Implementation"),            \
			&CrowdyThisClass::FuncName##_Implementation,                           \
			Forward<CrowdyArgTypes>(CrowdyArgs)...);                               \
	}
