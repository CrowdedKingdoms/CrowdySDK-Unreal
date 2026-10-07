# CrowdySDK for Unreal Engine 5

CrowdySDK is a multiplayer networking and state-management plugin for Unreal Engine 5, built for
games with large numbers of concurrent players. It connects your project to the Crowded Kingdoms
platform and gives you a small C++ and Blueprint surface for real-time replication, server-owned
gameplay state, voice, teams, channels, and avatars.

**Current version: 2.18.0** (Unreal Engine 5.8). See the
[changelog](https://docs.crowdedkingdoms.com/releases/intro) and
[What's Changed](https://docs.crowdedkingdoms.com/unreal-sdk/guides/whats-changed) for a project
that integrated an earlier release.

**On `dev` and `test`, not yet released:** Server Objects, server-owned state on the platform's
server compute, used from Blueprint or C++, and the deprecation of the Game Model API they replace.
Their pages are on the [dev docs](https://docs.dev.crowdedkingdoms.com/unreal-sdk/exec/overview)
until the next release.

Full usage docs: **[docs.crowdedkingdoms.com/unreal-sdk](https://docs.crowdedkingdoms.com/unreal-sdk/intro)**

## Branches and releases

| Branch | What it holds |
|---|---|
| `prod` | Released versions only. Every release is a `vX.Y.Z` tag on this branch; install from a tag. |
| `test` | The branch between `dev` and `prod`. |
| `dev` | Everything merged since the last release. Builds, but the docs may describe its newest changes as "unreleased". |

How a change moves: open a pull request. You may merge your own pull request into `dev`.
@Shady-S25 merges into `test` and `prod` on this repository. Anyone else asks an org admin.
Details are in [AGENTS.md](AGENTS.md).

The plugin itself is developed in the SDK's own repository and published here; a change lands there
and appears on `dev` on the next publish.

## What it does

The SDK replicates state on two planes, and everything else follows from the split.

- **The view plane (CrowdyState).** Fast, client-owned state: movement, animation, transient
  effects. The client that owns an entity writes its state and everyone else sees it. Continuous
  state rides actor updates; a single value is replicated by marking a `UPROPERTY` with
  `meta=(CrowdyState)`.
- **The truth plane (Server Objects).** Server-owned gameplay state: hit points, stats, inventory,
  match rules, held by your app's server code on ck-exec. Clients read it and ask the server to
  change it; the server decides. A Server Object type is a definition asset: the state the server
  keeps, the values players may watch, and the functions they may call, with who may read and call
  them. The editor generates its Rust server code, leaving you one file for the logic, and deploys
  it. The game gets a Server Object by type and Instance Id, follows its watched values and calls
  its functions, from Blueprint nodes, a component, or C++.
- **Game Models (deprecated).** The earlier truth plane. Every Game Model call now fails at once
  with `GAME_MODEL_DEPRECATED`, Blueprints that use them compile with a warning naming each use, and
  a later release removes them. See
  [Move from Game Models](https://docs.dev.crowdedkingdoms.com/unreal-sdk/exec/move-from-game-models).
- **Entities.** Any actor becomes a networked entity with a `UCrowdyEntityComponent`: a shared
  identity on every client, one owner, and a remote proxy everywhere else.
- **RPC events (`CrowdyEvent`).** Call a function across the network with typed parameters,
  including arrays, sets, maps and object references, aimed at everyone nearby, everyone on a
  channel, the entity's owner, or the host. C++ and Blueprint.
- **Host authority.** One client is elected host by convention to run shared world logic, with
  per-entity ownership and an explicit ownership-transfer flow. The host is a helper, not a rule
  enforcer; rules live in your server code.
- **Replicated subsystems.** A host-owned UE subsystem can join the view plane without being an
  actor.
- **Channels, voice, teams, avatars.** Named delivery groups for chat and lobby state; spatial and
  channel voice; teams and roles; per-app avatars and profiles.
- **Authentication.** Passwordless sign-in (email and password, magic link, social sign-in) with
  a short-lived, app-scoped gameplay token minted after sign-in.
- **Pluggable rendering.** Remote entities are drawn per map by the shipped actor-pool backend,
  customised through two policy classes or replaced by a backend of your own; a Mass Entity
  backend exists as a separate, opt-in plugin.
- **Crowdy Studio.** An in-editor console to sign in, sync your app configuration, manage teams
  and channels, and reach the web console. Its **Server Compute** page deploys your server code,
  lists and restores versions, switches types off and on, and shows logs and how calls went.

## Requirements

- Unreal Engine 5.8.
- A C++ project (Visual Studio 2022, Desktop C++ workload). Gameplay can be driven entirely from
  Blueprint, but the plugin itself compiles as C++.
- A Crowded Kingdoms account and app. Sign up and reach the team on
  [Discord](https://discord.gg/crowdedkingdoms).

## Installation

1. Download the release you want from the `vX.Y.Z` tags of this repository and unpack it so the
   descriptor sits at `YourProject/Plugins/CrowdySDK/CrowdySDK.uplugin`. Keep it under `Plugins`:
   a plugin reached through an external plugin directory loses the config it ships in `Config/`.
2. Open the project, go to **Edit > Plugins**, find **CrowdySDK**, make sure it is enabled, and
   restart the editor. Regenerate project files if you build from C++.
3. Open **Crowdy Studio** in the editor, sign in, and sync your app. Nothing on the network works
   until your project knows which app it belongs to.

See [Installation](https://docs.crowdedkingdoms.com/unreal-sdk/installation) and
[Crowdy Studio](https://docs.crowdedkingdoms.com/unreal-sdk/studio/overview) for the full setup,
then the [Quickstart](https://docs.crowdedkingdoms.com/unreal-sdk/quickstart) to put your player,
one entity, one event, one replicated property and one server-owned value on screen.

## Module layout

The plugin is split into focused modules under `Source/`:

| Module | Purpose |
|---|---|
| `CrowdySDK` | Foundation interfaces, the game-instance subsystem, and shared utilities. |
| `CrowdyNet` | Networking: the realtime transport, GraphQL client, messaging, and token storage. |
| `CrowdyReplication` | Entity subsystem, RPC event routing, Crowdy State replication, the rendering backends, and the deprecated Game Models. |
| `CrowdyExec` | Server Objects: the definition asset, the subsystem, the component, and the Blueprint library. |
| `CrowdyExecNodes` | The typed Server Object Blueprint nodes: Get, On Changed and Call, with a pin for every value. |
| `CrowdyExecEditor` | Generate Server Code, the definition asset editor, and the Server Compute page and command line. |
| `CrowdyServices` | Authentication, teams, avatars, channels, and host election. |
| `CrowdyVoice` | Voice capture and playback. |
| `CrowdyCppBridge` | The vendored native client the transport is built on. You never include it directly. |
| `CKSharedTypes` | Shared data structures (entity state, enums). |
| `CrowdyNodes` | The compiler checks for replicated variables, and the deprecated Game Model Blueprint nodes. |
| `CrowdySDKEditor` | Editor tooling: registry baking, Blueprint customizations, schema sync. |
| `CrowdyStudio` | The in-editor management console and authoring UI. |

## Documentation

- [SDK guide](https://docs.crowdedkingdoms.com/unreal-sdk/intro), start here
- [Server Logic: Server Objects](https://docs.dev.crowdedkingdoms.com/unreal-sdk/exec/overview) (dev docs until the next release)
- [Reference: subsystems, delegates, meta keys, console variables](https://docs.crowdedkingdoms.com/unreal-sdk/reference/subsystems)
- [Changelog](https://docs.crowdedkingdoms.com/releases/intro)

## Support

- Docs: [docs.crowdedkingdoms.com](https://docs.crowdedkingdoms.com/)
- Discord: [discord.gg/crowdedkingdoms](https://discord.gg/crowdedkingdoms)
