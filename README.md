# CrowdySDK for Unreal Engine 5

CrowdySDK is a multiplayer networking and state-management plugin for Unreal Engine 5, built for
games with large numbers of concurrent players. It connects your project to the Crowded Kingdoms
platform and gives you a small C++ and Blueprint surface for real-time replication, voice, teams,
avatars, and saved progress.

**Current version: 2.1.0** (Unreal Engine 5.8). See the
[changelog](https://docs.crowdedkingdoms.com/releases/intro).

Full usage docs: **[docs.crowdedkingdoms.com/unreal-sdk](https://docs.crowdedkingdoms.com/unreal-sdk/intro)**

## What it does

- **Entities.** Turn any actor into a replicated entity with a `UCrowdyEntityComponent`. Every
  entity has a shared `NetID`, exactly one owner, and remote proxies on every other client.
  Identity is deterministic across clients, with no hand-typed seeds.
- **Real-time replication, two planes:**
  - **Actor State** -- a continuous per-tick snapshot for movement and animation, produced by a
    `UActorUpdateExecutor`.
  - **Crowdy State** -- direct property replication: mark a `UPROPERTY` with `meta=(CrowdyState)`
    and the owning client diffs and ships just that value, with no snapshot struct. *(New in 2.1.)*
- **RPC events (`CrowdyEvent`).** Call a function across the network with typed parameters --
  including arrays, sets, maps, and object references -- addressed to everyone nearby, everyone on
  a channel, the entity's owner, or the host. Works from C++ and Blueprint.
- **Host authority.** One client is elected host by convention to run shared world logic, with
  per-entity ownership controls, an explicit request/grant ownership-transfer flow, and both
  client-side and server-validated host checks. *(Expanded in 2.1.)*
- **Replicated subsystems.** A host-owned UE Subsystem can join the view planes (Crowdy State and
  CrowdyEvents) without being an actor. *(New in 2.1.)*
- **Channels.** Named, non-spatial delivery groups for chat, lobby state, and game-wide signals.
- **Voice chat.** Spatial and channel voice, driven through `UCrowdySDKSubsystem`.
- **Player services.** Teams and roles, per-app avatars and profiles, and tagged save/load
  persistence.
- **Authentication.** Passwordless sign-in -- email + password, magic link, social/OAuth, and a
  developer bypass -- with a short-lived, app-scoped gameplay token minted after sign-in.
- **Pluggable rendering.** Choose how entities are drawn per map: Actor Pool (default), Skelot
  skeletal LOD, or Mass Entity.
- **Crowdy Studio.** An in-editor console to sign in, sync your app configuration, and author
  teams, channels, and grids, plus an embedded web console for members, billing, and secrets.

## Requirements

- Unreal Engine 5.8
- A C++ project (Visual Studio 2022, Desktop C++ workload). Gameplay can be driven entirely from
  Blueprint, but the plugin itself compiles as C++.
- A Crowded Kingdoms account and app. Sign up and reach the team on
  [Discord](https://discord.gg/crowdedkingdoms).

## Installation

1. Copy the `CrowdySDK` plugin folder into your project's `Plugins/` directory.
2. Open the project, go to **Edit > Plugins**, find **CrowdySDK**, make sure it is enabled, and
   restart the editor. Regenerate project files if you build from C++.
3. Open **Crowdy Studio** in the editor, sign in, and sync your app. Nothing on the network works
   until your project knows which app it belongs to.

See [Installation](https://docs.crowdedkingdoms.com/unreal-sdk/installation) and
[Crowdy Studio](https://docs.crowdedkingdoms.com/unreal-sdk/studio/overview) for the full setup.

## Module layout

The plugin is split into focused modules under `Source/`:

| Module | Purpose |
|---|---|
| `CrowdySDK` | Foundation interfaces, the game-instance subsystem, and shared utilities. |
| `CrowdyNet` | Networking: UDP transport, GraphQL client, messaging, and token/security storage. |
| `CrowdyReplication` | Entity subsystem, RPC event routing, Crowdy State property replication, and the rendering backends. |
| `CrowdyServices` | High-level subsystems: authentication, teams, avatars, persistence, channels, and host. |
| `CrowdyVoice` | Voice capture and playback. |
| `CKSharedTypes` | Shared data structures (entity state, enums, voxel data). |
| `CrowdySDKEditor` | Editor tooling: registry baking, Blueprint customizations, schema sync. |
| `CrowdyStudio` | The in-editor management console and authoring UI. |

## Quick start

1. Add a `UCrowdyEntityComponent` to an actor to make it a networked entity.
2. For continuous state (movement, animation), set the component to Dynamic mode and assign a
   `UActorUpdateExecutor`. For a single replicated value, mark a `UPROPERTY` with
   `meta=(CrowdyState)`.
3. For discrete actions, declare a `CrowdyEvent`:
   ```cpp
   UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "SpatialMulticast"))
   void OnHit_Implementation(FVector Location);
   CROWDY_EVENT(OnHit)
   ```
   Then call `OnHit(Location)` like a normal function; the SDK runs `OnHit_Implementation` on the
   other clients.
4. Gate shared, one-time world logic on the host with
   `UCrowdyUtilities::GetCrowdyHasAuthority(this)`.

A complete, interaction-driven example project (one "switch" per feature, C++ and Blueprint at
parity) is described in the
[sample project guide](https://docs.crowdedkingdoms.com/unreal-sdk/guides/sample-project).

## Documentation

- [SDK guide](https://docs.crowdedkingdoms.com/unreal-sdk/intro) -- start here
- [Reference: classes and subsystems](https://docs.crowdedkingdoms.com/unreal-sdk/reference/subsystems)
- [Changelog](https://docs.crowdedkingdoms.com/releases/intro)

## In development

Actively in progress, not yet part of a release:

- Mass Entity-based RPCs, as a first-class replacement for the Actor Pool rendering backend.
- Server-authoritative game logic (Game Models).
- More authoring and debugging tools in Crowdy Studio.

Feedback and suggestions are welcome on [Discord](https://discord.gg/crowdedkingdoms).

## Support

- Docs: [docs.crowdedkingdoms.com](https://docs.crowdedkingdoms.com/)
- Discord: [discord.gg/crowdedkingdoms](https://discord.gg/crowdedkingdoms)
