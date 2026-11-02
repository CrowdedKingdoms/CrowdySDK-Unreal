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

**Verdict.** No breaking change. Nothing is deprecated.

**Requires.** Unreal Engine 5.8. Nothing to deploy or regenerate.

**What changed**

| Old | New | Kind |
|---|---|---|
| On a map drawn with the Crowdy Mass Rendering Backend, a Dynamic entity spawned with `Spawn Entity` was drawn on other clients as a crowd row and also as the actor its spawn event made, which never moved. | Mass is its only body: the spawn event spawns no actor for it there. Its `InitialState` reaches each Crowdy State fragment field registered with `RegisterStateFragmentField` whose name it carries as the type that field reads. `On Crowdy Spawned` does not fire for it on those clients. A Static entity keeps its actor. | Behaviour: C++ `UCrowdyEntitySubsystem`, `UCrowdyMassRenderingBackend`, `UCrowdyMassEntitySubsystem::ApplySpawnState` (new); Blueprint `On Crowdy Spawned`. |
| A destroy event left a Mass-drawn entity's row until its updates stopped, about 5 seconds later. | The row goes with the destroy event. | Behaviour: C++ `UCrowdyActorManager::HandleRemoteDestroyEvent` (new), `UCrowdyRenderingBackend::ReleaseDestroyedInstance` (new). |
| When a moving entity's first update arrived before its spawn event, the actor pool's copy kept an empty owner id. | The spawn event fills it in. | Behaviour: C++ `UCrowdyEntitySubsystem`; Blueprint `Get Owner ID`, `On Crowdy Spawned`. |
| No hook for a crowd-row backend to receive spawn data or a destroy event. | `UCrowdyRenderingBackend::TakeSpawnData` and `ReleaseDestroyedInstance`, optional, doing nothing by default. | C++ API (new virtuals). |

**Do this, in order**

1. If a Mass map's game code reads a spawned entity's data in `On Crowdy Spawned`, move that read to a
   Crowdy State fragment field: register it with `RegisterStateFragmentField` and give your
   `InitialState` struct a property with the field's name and type.
2. If you wrote a rendering backend that returns true from `DrawsEntitiesAsCrowdRows()`, override
   `TakeSpawnData` to land spawn data on your representation and `ReleaseDestroyedInstance` to stop
   drawing a destroyed entity. Bound anything you hold for an entity not drawn yet: spawn events can be
   forged.

**CoreRedirects.** None.

**Blueprint assets.** To find Blueprints that bind `On Crowdy Spawned` on entity classes a Mass map
can draw, search recursively with `grep -a`, and check the search with a control string that must match:

```
grep -r -a -l --include=*.uasset "OnCrowdySpawned" Content Plugins
grep -r -a -l --include=*.uasset "CrowdyEntityComponent" Content Plugins
```

No re-save is needed.

**Behaviour changes.** On a Mass map, other clients show a spawned moving entity as its row (and its
near-tier shell up close) only, where they used to show a frozen actor at the spawn point as well; Crowdy
Events aimed at it run on Mass's shared stand-in, as for any crowd entity. `On Crowdy Spawned` fires there
only on the owner. Every client should run the same SDK version: an older client still draws the extra
actor.

**Verify**

1. Build your project's editor target and confirm the log shows `Compile` and `Link` lines for the
   CrowdySDK modules.
2. With two clients on a Mass map, spawn a Dynamic entity on the first: the second shows one crowd row
   for it and no actor, and your registered field's ingestor receives its spawn data once.
3. Destroy it on the first: the row disappears on the second at once.

**Removed in the next release.** None.

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
