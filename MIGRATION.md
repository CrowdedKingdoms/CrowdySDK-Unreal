# CrowdySDK for Unreal: migration notes

This file lists, for each release, what changed in the CrowdySDK plugin and the exact steps to
move a project onto it. The newest release is first.

If you are an AI agent upgrading a project, apply every section between the version the project
is on and the version it is moving to, oldest first, and run each section's Verify block before
moving on to the next.

Releases before 2.24.0 are described in the Unreal SDK docs' "What's Changed" page:
https://docs.crowdedkingdoms.com/unreal-sdk/guides/whats-changed

## How each release section is laid out

Every release has the same subsections, in this order:

- **Verdict**: whether anything breaks, and whether anything is deprecated.
- **Requires**: the engine version, and anything to deploy or regenerate.
- **What changed**: a table of old behaviour, new behaviour, and the kind of change.
- **Do this, in order**: the steps to take, numbered.
- **CoreRedirects**: any redirect entries to add to your project's config.
- **Blueprint assets**: how to find the Blueprints a change affects, and whether to re-save them.
- **Behaviour changes**: what runs differently with no code change on your side.
- **Verify**: how to confirm the upgrade worked.
- **Removed in the next release**: what is deprecated now and goes away next.

A subsection with nothing to say is written as "None." so you never have to guess whether
something was left out.

## Unreleased (after 2.24.0)

**Verdict.** One behaviour change: there is no elected host by default. `Leader Source` now defaults to
`Server Object`, where the Leader of a Server Object type the SDK deploys writes replicated subsystems, nothing
writes World entities, and an actor's Host event is dropped. A project that relies on the elected host sets
`Elected Host (Deprecated)` (step 10). The entity component's Ownership setting, the host check nodes and the ownership-taking subsystem
registration are deprecated: they keep working and warn when you compile.

**Requires.** Unreal Engine 5.8. Under `Server Object`, deploy the app's server code once (Server Compute) so it
carries the Leader type, then restart what still runs older code (`-run=CrowdyServerCompute -op=restart -yes`, or
Server Compute's restart): a running root keeps the code it started with. Until the server code is deployed, each
signed-in client tries to reach the Leader type every 10 seconds and logs one warning.

**What changed**

| Old | New | Kind |
|---|---|---|
| `Ownership` on the `Crowdy Entity Component`: `Local Client` or `Host`. | `Owned By`: `Auto`, `Player` or `World`. Local Client is Player, Host is Auto. Auto is World for an actor placed in the level and Player for one spawned during play; World also belongs to the spawner on a spawned actor, as Host did. An asset saved earlier has Owned By set from its Ownership when it loads. A C++ constructor that still sets `Ownership` keeps its meaning while `OwnedBy` is `Auto`. | Property (Details panel, C++); `Ownership` deprecated. |
| `Get Ownership` | `Get Owned By` (the property, read-only in Blueprint). | Blueprint node, C++ `GetOwnership()` deprecated. |
| `Is Crowdy Entity Locally Controlled` (actors only) | `Is Crowdy Entity Locally Owned`, and the exec version `Switch Is Crowdy Entity Locally Owned`. Both take an entity actor or a replicated subsystem. | Blueprint nodes; C++ `UCrowdyUtilities::IsCrowdyEntityLocallyOwned`, `SwitchIsCrowdyEntityLocallyOwned` (new), `IsCrowdyEntityLocallyControlled` deprecated. |
| `Switch Crowdy Has Authority`, `Crowdy Has Authority` (is this client the elected host) | `Switch Is Crowdy Entity Locally Owned` on the entity or replicated subsystem you are about to write. Logic that only the host should run, with no entity to ask about, belongs in a Server Object. | Blueprint nodes; C++ `CrowdyHasAuthority`, `GetCrowdyHasAuthority` deprecated. |
| `Register Replicated Subsystem` (Subsystem, Ownership) | `Register Crowdy Replicated Subsystem` (Subsystem). A replicated subsystem is always written by one client for everyone: the app's Leader (the elected host under `Elected Host`). | Blueprint node; C++ `UCrowdyReplicatedSubsystemLibrary::RegisterCrowdyReplicatedSubsystem` and `RegisterReplicatedSubsystemInto(Registry, Subsystem)` (new), the ownership-taking versions deprecated. |
| Overriding `GetReplicatedOwnership()` on `UCrowdyReplicatedWorldSubsystem` or `UCrowdyReplicatedGameInstanceSubsystem` | Nothing to override: the bases no longer call it. | C++ virtual deprecated. |
| An elected host always existed. | `Leader Source` in Project Settings > Plugins > Crowdy SDK > Replication: `Server Object`, the default, or `Elected Host (Deprecated)`. Server Object turns the elected host off (no host polling; World entities have no writer, and a Host event sent from any actor, Player-owned ones included, is dropped with a warning naming the setting) and has the app's Leader write replicated subsystems and run their Host events. | Project setting, C++ `UCrowdySDKDeveloperSettings::LeaderSource`, `ECrowdyLeaderSource`, `UsesElectedHost()` (new). |
| No Leader. | Every deploy carries the Crowdy SDK's Leader type (`crowdy_leader`, shown as the SDK's own in Server Compute). Under `Server Object` each signed-in client joins it once its realtime connection is up; the first to join leads, the longest member takes over, and the server removes a Leader who leaves the realtime world. A client leaves when the SDK reports its realtime connection failed. | Server code (deployed by the SDK), C++ `UCrowdyLeaderSubsystem` (new). |
| No way to ask who leads. | `Get Crowdy Leader Subsystem` with `Get Leader User ID`, `Is Local Leader` and the `On Leader Changed` event. Under `Elected Host` there is no Leader: the user id is 0. | Blueprint nodes; C++ `UCrowdyLeaderSubsystem::GetLeaderUserID`, `IsLocalLeader`, `OnLeaderChanged` (new). |
| A Host event on a replicated subsystem ran on the elected host. | Under `Server Object` it runs on the Leader. The deprecated host functions (`Get Host User ID`, `Is Host`, `On Host Elected`, `Is Crowdy Entity Host`, `Check Entity Is Host` for your own avatar) report the Leader. | Behaviour. |
| `UCrowdyEntitySubsystem::IsLocallyOwned(NetID)` was false for a World entity even on the host, while the component's `IsLocallyOwned` was true. | Both are true on the host. One rule, `FCrowdyEntityRecord::IsWrittenBy`, answers every ownership check; `UCrowdyEntitySubsystem::GetWorldOwnerID()` and `IsRecordLocallyOwned(Record)` expose it. An `Owning Client` event on a World entity now runs on the host instead of nowhere. | Behaviour and C++ API (new functions). |
| On a map drawn with the Crowdy Mass Rendering Backend, a Dynamic entity spawned with `Spawn Entity` was drawn on other clients as a crowd row and also as the actor its spawn event made, which never moved. | Mass is its only body: the spawn event spawns no actor for it there. Its `InitialState` reaches each Crowdy State fragment field registered with `RegisterStateFragmentField` whose name it carries as the type that field reads. `On Crowdy Spawned` does not fire for it on those clients. A Static entity keeps its actor. | Behaviour: C++ `UCrowdyEntitySubsystem`, `UCrowdyMassRenderingBackend`, `UCrowdyMassEntitySubsystem::ApplySpawnState` (new); Blueprint `On Crowdy Spawned`. |
| A destroy event left a Mass-drawn entity's row until its updates stopped, about 5 seconds later. | The row goes with the destroy event. | Behaviour: C++ `UCrowdyActorManager::HandleRemoteDestroyEvent` (new), `UCrowdyRenderingBackend::ReleaseDestroyedInstance` (new). |
| When a moving entity's first update arrived before its spawn event, the actor pool's copy kept an empty owner id. | The spawn event fills it in. | Behaviour: C++ `UCrowdyEntitySubsystem`; Blueprint `Get Owner ID`, `On Crowdy Spawned`. |
| No hook for a crowd-row backend to receive spawn data or a destroy event. | `UCrowdyRenderingBackend::TakeSpawnData` and `ReleaseDestroyedInstance`, optional, doing nothing by default. | C++ API (new virtuals). |
| The actor pool drew remote entities 100 ms behind the server's clock on every map, with no setting. | `Interpolation Delay` in the map profile's Remote Entities group, 100 by default. Mass-drawn rows ignore it. | Property (map profile, C++ `FCrowdyActorManagementConfigStruct::InterpolationDelayMs`, new). |
| The map profile's Details panel listed every setting in one category, with Actor Management as a nested row. Labels: `Enable Networking`, `Use Auto Replicator`, `Replication Interval (Hertz)` (greyed out when movement was off, though Crowdy State still used it), `Send Actor State Only On Change`, `Use State Replicator`, `Use Crowdy Actor Tracker`. | Groups `Events`, `Remote Entities` (Actor Management's settings, tuning under Advanced), `Movement`, `Crowdy State`. Labels: `Handle Events and Spawns`, `Send Movement`, `Send Rate` (editable while either sender is on), `Send Only On Change`, `Replicate Crowdy State`, `Draw Remote Entities`, `Actor Timeout`. Property names are unchanged. | Details panel only. |
| A map profile saved with settings that drop idle entities, or a Backend Config of another backend's class, said nothing until play. | Saving or validating the profile reports both as errors. | Editor validation (`UCrowdyMapProfile::IsDataValid`). |
| `Crowdy Actor Management Config` data asset (nothing reads it at runtime). | Deprecated, hidden from the create menu; existing assets still load. Set the values on a map profile. | Asset class deprecated. |
| No way to say which Backend Config class a rendering backend reads. | `UCrowdyRenderingBackend::GetBackendConfigClass()`, optional, null by default (no check). The actor pool and Mass backends return their config classes. | C++ API (new virtual). |
| Assets carried no CrowdySDK save version. | A package that saves a `Crowdy Entity Component` records the `CrowdySDK` custom version, and the component saves `Owned By` and a Replicate Movement setting that nothing reads yet. Older SDKs warn on these packages (see Behaviour changes). | Asset format. |

**Do this, in order**

1. In C++, replace `EntityComponent->Ownership = ECrowdyOwnership::LocalClient` with
   `EntityComponent->OwnedBy = ECrowdyOwnedBy::Player`, and `ECrowdyOwnership::Host` with
   `ECrowdyOwnedBy::World` (or delete the line: Auto already gives World for a placed actor). Replace
   `GetOwnership()` reads with `OwnedBy`.
2. Replace `Is Crowdy Entity Locally Controlled` with `Is Crowdy Entity Locally Owned`.
3. Replace `Switch Crowdy Has Authority` and `Crowdy Has Authority` with `Switch Is Crowdy Entity Locally Owned`
   or `Is Crowdy Entity Locally Owned` on the entity or replicated subsystem the gated code writes. In a
   replicated subsystem, pass the subsystem itself.
4. Replace `Register Replicated Subsystem` with `Register Crowdy Replicated Subsystem`, and in C++ drop the
   `Ownership` argument from `RegisterReplicatedSubsystemInto`. Delete any `GetReplicatedOwnership()` override.
5. Open and re-save the Blueprints the search below finds, so they stop warning.
6. If a Mass map's game code reads a spawned entity's data in `On Crowdy Spawned`, move that read to a
   Crowdy State fragment field: register it with `RegisterStateFragmentField` and give your
   `InitialState` struct a property with the field's name and type.
7. If you wrote a rendering backend that returns true from `DrawsEntitiesAsCrowdRows()`, override
   `TakeSpawnData` to land spawn data on your representation and `ReleaseDestroyedInstance` to stop
   drawing a destroyed entity. Bound anything you hold for an entity not drawn yet: spawn events can be
   forged.
8. If you wrote a rendering backend with a config class of its own, override `GetBackendConfigClass()` to return
   it, so a map profile that pairs your backend with another config is reported on save.
9. If a map profile switches the SDK off with `Enable Networking` (now `Handle Events and Spawns`) alone,
   also untick `Draw Remote Entities`, `Send Movement` and `Replicate Crowdy State`: the first switch never
   stopped movement.
10. Choose `Leader Source`. If World entities in your game are written by the elected host (a turret, a camp, a
   boss simulated on one client), keep the host for this release: set `Elected Host (Deprecated)` in Project
   Settings, which writes `LeaderSource=ElectedHost` under `[/Script/CrowdyReplication.CrowdySDKDeveloperSettings]`
   in `Config/DefaultGame.ini`, and move that state to Server Objects before the host is removed. Otherwise keep
   `Server Object`, deploy the app's server code once and restart its root (see Requires). If your code compares
   `UCrowdyEntitySubsystem::IsLocallyOwned(NetID)` for a World entity against false on the host, it now reads
   true there, as the component already did.

**CoreRedirects.** None.

**Blueprint assets.** Search recursively with `grep -a`, and check the search with a control string that
must match:

```
grep -r -a -l --include=*.uasset -e "IsCrowdyEntityLocallyControlled" -e "CrowdyHasAuthority" -e "GetOwnership" -e "RegisterReplicatedSubsystem" Content Plugins
grep -r -a -l --include=*.uasset "OnCrowdySpawned" Content Plugins
grep -r -a -l --include=*.uasset "CrowdyEntityComponent" Content Plugins
```

The first finds graphs using a deprecated node: replace it as above and re-save. The second finds
Blueprints that bind `On Crowdy Spawned` on entity classes a Mass map can draw; they need no re-save.

**Behaviour changes.** An override of `GetReplicatedOwnership()` on a replicated subsystem base is no longer
called: the subsystem is written by its one writer whatever it returned. Returning anything but Host was
never supported.

The default is `Server Object`, so a project that changes nothing loses its elected host: World entities are
written by nobody, a `CrowdyRecipient="Host"` event on any actor is dropped (the SDK warns once, naming the
setting, when your code relies on a writer or a host; move such logic to a Server Object), and replicated
subsystems are written by the app's Leader once the server code is deployed. Until it is, nothing writes them.
When the lead passes, the new Leader re-sends the subsystem values it holds; a value it never received (one that
changed before it joined and is not `CrowdyHeartbeat`-marked) is not carried over, so mark subsystem state
`CrowdyHeartbeat`.

Under `Elected Host`, an `Owning Client` event on a World entity runs on the host instead of being dropped.

On a Mass map, other clients show a spawned moving entity as its row (and its
near-tier shell up close) only, where they used to show a frozen actor at the spawn point as well; Crowdy
Events aimed at it run on Mass's shared stand-in, as for any crowd entity. `On Crowdy Spawned` fires there
only on the owner. Every client should run the same SDK version: an older client still draws the extra
actor.

An asset with a `Crowdy Entity Component` that you save with this version still opens with 2.24.0 and
earlier, but each such package logs `Package was saved with a custom version that is not present` there.
A later release that raises this version will stop older SDKs from opening such packages at all, so going
back to an older SDK after re-saving content needs those packages restored from version control. Upgrade
everyone who shares the project's content together. Recook mods and downloadable content with this version:
cooked data is read against the component's current properties.

**Verify**

1. Build your project's editor target and confirm the log shows `Compile` and `Link` lines for the
   CrowdySDK modules.
2. With two clients on a Mass map, spawn a Dynamic entity on the first: the second shows one crowd row
   for it and no actor, and your registered field's ingestor receives its spawn data once.
3. Destroy it on the first: the row disappears on the second at once.
4. Open an entity Blueprint: its `Crowdy Entity Component` shows `Owned By` and no `Ownership`, set to what
   the asset meant before (Player for a former Local Client).
5. Open a map profile: it shows the Events, Remote Entities, Movement and Crowdy State groups with the
   values it had before. Run Validate Assets on your profiles; any error it reports names the setting to fix.
6. Project Settings > Plugins > Crowdy SDK > Replication shows `Leader Source` as you chose it in step 10.
7. Under `Server Object`, with two clients signed in: `Get Leader User ID` gives the same user id on both,
   `Is Local Leader` is true on exactly one, and a replicated subsystem value the Leader changes reaches the
   other. Close the Leader's game: within seconds the other client leads.

**Removed in the next release.** None. The deprecated items above are planned for removal in a later
release, which will say so here first.

## 2.24.0

**Verdict.** No breaking change. Nothing is deprecated.

**Requires.** Unreal Engine 5.8. Nothing to deploy or regenerate.

**What changed**

| Old | New | Kind |
|---|---|---|
| Host user id 0 ("no host": after sign-out, before the first election on a map, or a host poll answer naming user 0) became a valid-looking host id, and sign-out did not always clear the host. | It becomes an empty (invalid) host id, and sign-out clears the host at once. | Behaviour: C++ `UCrowdyHostSubsystem::SetHostUserID`, `UCrowdySDKSubsystem::Logout`; Blueprint `Is Host Set`, `Get Host ID`, `On Host Elected`. |
| A packaged plugin (UAT BuildPlugin, which the SDK's package script uses) left out the plugin's `Config` folder, so `Config/DefaultCrowdySDK.ini` and the CoreRedirects the SDK carries for itself were missing from packaged installs. | The package includes `Config/`, `MIGRATION.md`, `README.md` and the license file. | Packaging: `Config/FilterPlugin.ini`. |
| No migration file. | `MIGRATION.md` at the plugin root. | Docs. |
| Building the plugin with UAT `BuildPlugin` (or the SDK's package script) failed: UHT requires an explicit `Category` on every editor- or Blueprint-exposed property when it builds a plugin that way, and 113 properties had none. The package script still reported success. | Every exposed property has a `Category`, so `BuildPlugin` builds the plugin; the package script stops with an error when UAT fails. | Packaging: headers in CrowdyNet, CrowdyReplication and CrowdyServices; `scripts/package-crowdy-sdk.ps1` (SDK repository only). |
| On other clients, the actor pool replaced the actor a spawn event created with a pooled actor, so what `On Crowdy Spawned` gave that actor (its spawn data, and anything your handler set on it) was lost. When an update arrived before the spawn event, `On Crowdy Spawned` never fired at all. | The pool draws the spawn event's own actor. `On Crowdy Spawned` fires once per entity on every client that receives the spawn event, whichever of the spawn event and the first update arrives first. | Behaviour: C++ `UCrowdyActorPoolBackend`, `UCrowdyEntitySubsystem`, `UCrowdyEntityComponent::DeliverSpawnData`; Blueprint `On Crowdy Spawned`. |
| An entity spawned with `Spawn Entity` whose Crowdy Entity `Mode` is Static sent nothing after its spawn event, so a player who joined later never saw it. | While a client owns it, that client sends its spawn event again every 4 seconds, from where the entity is now, with its owner and its spawn data. A player who joins later builds it as if they had seen the spawn: the actor, its owner, and `On Crowdy Spawned` with the spawn data. Clients that already have it ignore the repeats. | Behaviour and network traffic: C++ `UCrowdyEntityComponent::EnablePresence` and `SendPresence`, `UCrowdyAutoReplicator`, `ICrowdyReplicationSource::SendPresence` (new, empty by default). |
| An update that arrived just after an entity's destroy event drew the entity again, as a copy that stayed until the 12-second tracker timeout. | Updates for an entity in the half second after its destroy event are not drawn. | Behaviour: C++ `UCrowdyEntitySubsystem::WasRecentlyDestroyed`, `UCrowdyActorManager`. |
| `Destroy Entity` kept sending the entity's updates until its `Destroy Delay` ran out. | Sending stops when `Destroy Entity` is called. | Behaviour: C++ `UCrowdyEntitySubsystem::DestroyEntity`. |
| The client that owns a spawned entity also drew a second, pooled copy of it, driven by the server's echo of its own updates. | That copy is not drawn. The new pool setting `Render Local Owner` (`bRenderLocalOwner`, off by default) brings it back. | Behaviour and new property: `UCrowdyActorPoolBackendConfig::bRenderLocalOwner`. |

**Do this, in order**

1. If you bind `On Host Elected` (Blueprint) or `OnHostElected` (C++), treat an invalid `HostID`
   or `PreviousHostID` as "no host": check it with `Is Valid (Guid)` / `FGuid::IsValid` before
   using it.
2. If you install the SDK from a packaged zip, replace it with a 2.24.0 package.
3. If you copied the SDK's CoreRedirects into your own project's `Config/DefaultEngine.ini`
   because they were missing, you may keep them. Duplicate redirects are harmless and removing
   them is optional.
4. If your project relies on the owning client seeing a pooled copy of its own spawned entities
   (for example a single-client demo that hides the real actor and shows the round trip), open
   the map profile's Actor Pool Backend Config and turn on `Render Local Owner`.
5. If you kept per-instance appearance out of the spawn payload because pooled proxies never
   received it, you may now read it in `On Crowdy Spawned`. A Dynamic entity's spawn data still
   reaches only the clients there at the spawn; a Static one's reaches players who join later too.
6. If you spawn many Static entities with `Spawn Entity`, budget about 70 bytes per second of
   upload per entity on the owning client, and the same in download per entity on each client
   that sees it, plus the size of your spawn data every 4 seconds.
7. If your `On Crowdy Spawned` handler must run once per entity, it already does: it no longer
   fires a second time on a repeat. If it assumes the entity was just spawned, note that on a late
   joiner it runs when that player first sees the entity, up to 4 seconds after they arrive.

**CoreRedirects.** None.

**Blueprint assets.** To find Blueprints that bind `On Host Elected`, search the project's
assets, plugin content included. A `.uasset` is binary, so use `grep -a`; editor search tools
that skip binaries will report nothing. Search recursively (a `Content/**` glob does not recurse
in a default bash shell):

```
grep -r -a -l --include=*.uasset "OnHostElected" Content Plugins
```

Check the search with a control string that must match, for example
`grep -r -a -l --include=*.uasset "CrowdyEntityComponent" Content Plugins`. If the control finds
nothing, the search is not reading the files and an empty result for `OnHostElected` means
nothing. No re-save is needed.

**Behaviour changes.** Sign-out clears the host before `On Logout` fires: from then on
`Is Host Set` returns false, `Get Host ID` returns an invalid id, and `Is Host` returns false (as
before). Before, after a level travel or on a map without networking, the host could stay set
after sign-out. When a host had been elected on the current map, `On Host Elected` also fires
once, on the next frame, with an invalid `HostID` and the previous host as `PreviousHostID`.
The first election on each map (the host subsystem belongs to the world, so it starts again
after a level travel) fires `On Host Elected` with an invalid `PreviousHostID`; do not read that
as "a new session started". Before, both places carried a made-up, valid-looking host id that
matched no player. A host poll answer that arrives while nobody is signed in, such as one still
in flight at sign-out, is now ignored instead of electing a host for a signed-out client.
Editor-only: the Teams and Channels permission structs' fields moved from the `Crowdy|Teams` and
`Crowdy|Channels` categories to `Crowdy SDK|Teams` and `Crowdy SDK|Channels`, matching the rest of
the SDK; this changes only how the editor groups them.

Spawned entities: on clients that receive the spawn event, the actor your `On Crowdy Spawned`
handler set up is the one players see, so state your handler puts on it is no longer lost. The
owning client no longer sees a second, delayed copy of entities it spawned. A Static entity spawned
with `Spawn Entity` now reaches players who join later, built from a repeat of its spawn event with
its owner and spawn data, at the position it has when the repeat is sent. A spawn event for an
entity destroyed less than 10 seconds before is ignored, so a repeat that was already on its way
does not bring it back; if the owner itself never received the destroy, it keeps sending and the
entity reappears after those 10 seconds. When ownership of an entity moves to or from this client,
the pooled drawing of it is set up again on the next update. `Destroy Entity` stops the entity's
updates at once, so position changes made during `Destroy Delay` are no longer seen by other
clients. Every client in a session should run the same SDK version: a 2.23.0 client neither
repeats spawns nor guards against late repeats.

**Verify**

1. Build your project's editor target and confirm the log shows `Compile` and `Link` lines for
   the CrowdySDK modules.
2. In Play In Editor, sign in, then sign out, and confirm `Is Host Set` returns false.
3. For a packaged install, confirm the plugin folder contains `Config/DefaultCrowdySDK.ini` and
   `MIGRATION.md`.
4. With two clients, spawn a Static entity with `Spawn Entity` on the first, then start the second:
   within about 4 seconds of signing in, the second draws it at its position, with its owner, and
   its `On Crowdy Spawned` fires with the spawn data.
5. Spawn an entity on one client and confirm that client shows only its own actor, not a second
   copy (unless `Render Local Owner` is on).

**Removed in the next release.** None.
