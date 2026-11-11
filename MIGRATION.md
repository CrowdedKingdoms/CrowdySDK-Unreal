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

## Next release (unreleased): a session released for idleness, and CrowdyCPP 0.78.0

**Verdict.** Nothing breaks and nothing is deprecated. One behaviour change: a session the server releases for
idleness is no longer reconnected (below).

**Requires.** Unreal Engine 5.8. The vendored CrowdyCPP moves from 0.76.0 to 0.78.0 (`dev/v0.78.0`); regenerate it
with your sync script to confirm the snapshot.

**What changed**

| Old | New | Kind |
|---|---|---|
| A session the replication server released for idleness (no gameplay input for the app's idle timeout, 15 minutes unless the app sets its own) was read as a failed connection, and the reconnect ladder put the player back on a server. | The connection reports `Inactive`, `On UDP Session Inactive` fires on the Crowdy SDK subsystem, and nothing reconnects. `Get UDP Connection State` reads `Inactive`. | Behaviour; Blueprint event and enum value (new). |
| No way to tell an idle release from a timeout. | `EUDPConnectionState::Inactive`, `ECrowdyCppConnState::Inactive`, `UCrowdyUDPSubsystem::OnUDPInactive`. | Blueprint enum value; C++ API (new). |
| The Leader type kept an idle client until its realtime connection timed out. | An idle release leaves it, as a lost connection does. | Behaviour. |

**Do this, in order**

1. Bind `On UDP Session Inactive` where you bind `On UDP Timed Out`. Show the player that they were disconnected
   for inactivity, if you want to.
2. On the player's next input, call `Request UDP Access`. It re-assigns a server on the same app token.
3. A `switch` on `EUDPConnectionState` that must be exhaustive needs a case for `Inactive`.

**CoreRedirects.** None.

**Blueprint assets.** A Blueprint with a `Switch on EUDPConnectionState` node gains an `Inactive` pin; re-save it.

**Behaviour changes.** See the table: an idle release no longer reconnects, and the Leader type is left.

**Verify.** Set the app's idle timeout to 60 seconds in Crowdy Studio (Idle timeout card), connect, and send nothing
for a minute. `On UDP Session Inactive` fires, the state reads `Inactive`, and the log says "released the session for
idleness; not reconnecting". `Request UDP Access` brings the connection back. Put the app's timeout back afterwards.

**Removed in the next release.** None.

## 2.25.0

**Verdict.** One behaviour change: there is no elected host by default. `Leader Source` now defaults to
`Server Object`, where the Leader of a Server Object type the SDK deploys writes replicated subsystems, nothing
writes World entities, and an actor's Host event is dropped. A project that relies on the elected host sets
`Elected Host (Deprecated)` (step 10). The entity component's Ownership setting, every host node and the ownership-taking subsystem
registration are deprecated: they warn when you compile and still run. Every deprecated item and its replacement, in one list:
https://docs.crowdedkingdoms.com/unreal-sdk/guides/deprecated Under `Server Object` the host nodes report the Leader, and
`Grant Ownership To Host` leaves an entity with no writer.

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
| `Get Host User ID`, `Get Host ID` (on the host subsystem, the game session and the entity subsystem), `Is Host Set`, `Crowdy Get Host ID` | `Get Leader User ID` on `Get Crowdy Leader Subsystem`; 0 means there is no Leader. It is a user id: where a graph needs the entity id, pass it through `Get Deterministic ID`, and check it is not 0 first, because 0 gives a valid-looking id where these nodes gave an invalid one. | Blueprint nodes; C++ `UCrowdyHostSubsystem::GetHostUserID`, `GetHostID`, `IsHostSet`, `UCrowdyGameSession::GetHostID`, `UCrowdyEntitySubsystem::GetHostID`, `UCrowdyUtilities::CrowdyGetHostID` deprecated. |
| `Is Host` | `Is Local Leader` on the Crowdy Leader Subsystem. | Blueprint node; C++ `UCrowdyHostSubsystem::IsHost` deprecated. |
| `On Host Elected`, `On Host ID Updated` | `On Leader Changed` on the Crowdy Leader Subsystem. It carries user ids, not entity ids. | Blueprint events; C++ `UCrowdyHostSubsystem::OnHostElected`, `UCrowdyGameSession::OnHostIDUpdated` deprecated. |
| `Is Crowdy Entity Host`, `Is Crowdy Entity Host (Server)` | For a gate that should run on one machine, `Is Local Leader`. To ask whether an actor is the Leader's avatar, compare its entity id with `Get Deterministic ID` of `Get Leader User ID`. `Is Crowdy Entity Locally Owned` is a different question: it is true on every client for its own avatar, so swapping it in runs a host-only branch everywhere. A client-side answer can be changed by a forged message, so a decision that must be enforced belongs in a Server Object. | Blueprint nodes; C++ `UCrowdyUtilities::IsCrowdyEntityHost`, `UCrowdyHostSubsystem::CheckEntityIsHost`, `UCrowdyIsEntityHostServer::IsCrowdyEntityHostServer` deprecated. |
| `Grant Ownership To Host` | No replacement for handing an entity back at runtime. Keep the entity's truth in a Server Object, or set `Owned By` `Leader` on the class if one client should simulate it; such an entity refuses grants, so it cannot also be handed to a player. | Blueprint node; C++ `UCrowdyOwnershipTransfer::GrantOwnershipToHost` deprecated. |
| `Host Override` on the `Crowdy Entity Component`, `Get Host Override Policy` | No replacement: host corrections end with the elected host. State that must be protected belongs in a Server Object. The setting stays editable and a saved value keeps working under Elected Host. Under Server Object no host-sourced update reaches an owned entity, so it has nothing left to refuse. The Details panel shows no warning for it; a C++ read or write does. | Component property and Blueprint node; C++ `HostOverride`, `GetHostOverridePolicy()` deprecated. |
| `Register Replicated Subsystem` (Subsystem, Ownership) | `Register Crowdy Replicated Subsystem` (Subsystem). A replicated subsystem is always written by one client for everyone: the app's Leader (the elected host under `Elected Host`). | Blueprint node; C++ `UCrowdyReplicatedSubsystemLibrary::RegisterCrowdyReplicatedSubsystem` and `RegisterReplicatedSubsystemInto(Registry, Subsystem)` (new), the ownership-taking versions deprecated. |
| Overriding `GetReplicatedOwnership()` on `UCrowdyReplicatedWorldSubsystem` or `UCrowdyReplicatedGameInstanceSubsystem` | Nothing to override: the bases no longer call it. | C++ virtual deprecated. |
| An elected host always existed. | `Leader Source` in Project Settings > Plugins > Crowdy SDK > Replication: `Server Object`, the default, or `Elected Host (Deprecated)`. Server Object turns the elected host off (no host polling; World entities have no writer, and a Host event sent from any actor, Player-owned ones included, is dropped with a warning naming the setting) and has the app's Leader write replicated subsystems and run their Host events. | Project setting, C++ `UCrowdySDKDeveloperSettings::LeaderSource`, `ECrowdyLeaderSource`, `UsesElectedHost()` (new). |
| No Leader. | Every deploy carries the Crowdy SDK's Leader type (`crowdy_leader`, shown as the SDK's own in Server Compute). Under `Server Object` each signed-in client joins it once its realtime connection is up and a Player Derived pawn has given it a player id for this sign-in, so a client that has not spawned one yet stays out and a project with no such pawn has no Leader; the first to join leads, the longest member takes over, and the server removes a Leader who leaves the realtime world. A client leaves when the SDK reports its realtime connection failed. | Server code (deployed by the SDK), C++ `UCrowdyLeaderSubsystem` (new). |
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
| An entity a client simulates for everyone needed the elected host. | `Owned By` `Leader`: the client that leads writes it, and the next Leader takes it over. Only the Leader spawns or destroys one, and ownership requests and grants on one are refused with a warning. On other clients its body stays where it stopped through a hand-off. For players who share a map, on an actor pool map. | Property value (new). |
| A moving entity handed to another player vanished for everyone about 5 seconds later, because the server keeps an actor id for its first sender. | It keeps one body on every client, there and back. Each owner sends under an id of its own, made from the entity's id and the owner's. The entity's `NetID` does not change, so game code sees the same id. | Behaviour, wire: `FCrowdyActorId::ForOwner`, `TryReadOwnerTaggedNetID` (new). |
| An `Owning Client` event on an entity that changed hands still went to the old owner. | It goes to the current owner. | Behaviour: C++ `UCrowdySDKBridgeSubsystem::DispatchSingleActorMessageFn` and `UCrowdySDKSubsystem::DispatchSingleActorMessage` take an `FCrowdyActorId` instead of an `FGuid`. |
| A client that joined after a Player entity spawned could not take it: the grant was dropped, and its pool actor could not send. | The grant lands and the entity moves on from there. | Behaviour. |
| While an email sign-in link was awaited, the local listener accepted any callback, so a web page the player opened could sign the game, or Crowdy Studio, in to another account. | `Begin Magic Link Sign In` and Studio's email sign-in use a random callback path for each sign-in. The address sent with the link now ends in that path; its origin is unchanged, so an allow-list by origin needs no change. A link from an earlier attempt no longer completes a new one. | Behaviour. |
| A grant could move a player's avatar. | An avatar never changes hands: the grant is ignored. | Behaviour. |
| `Host` in the Recipient dropdown and pin. | Hidden and deprecated: a saved value keeps routing as before, and warns. Use `Owning Client`, which reaches whichever client writes the entity, or move logic only one machine should run to a Server Object. | Enum value `ECrowdyEventRecipient::Host` deprecated. Blueprint compile warning; `CrowdyRecipient="Host"` warns once per function where the SDK reads metadata, the cook included. |
| An `Owning Client` event on an entity that sends no movement, or one this client has not heard under its current id, went to an id nobody had sent and vanished with no message. | It goes over the session channel, named for the client that writes the entity. Every other client ignores it. A client that has not heard of the change of owner yet holds the call for up to 0.3 s. | Behaviour, wire: `CrowdyChannelRpcFlagAddressed`, and an optional `Addressee` on `FCrowdyRPC::EncodeChannelRpc`, `DecodeChannelRpc` and `UCrowdyEventRouter::ReceiveChannelRpcCall`. An older peer reads the call and ignores the addressee. |
| An `Owning Client` event at a moving entity went as a direct message, was lost while the entity crossed a chunk border, and needed `use_direct_messages` on both players. | It goes as a spatial event around the entity, one chunk in every direction, named for the client that writes it. An event with a parameter that can hold text (`FString`, `FText`, or a container or struct holding one) stays a direct message, and is dropped with a warning rather than use the session channel. | Behaviour, wire: the event carries `ECrowdyTarget::Owner` and the client's id. C++: `UCrowdyEntitySubsystem::DispatchGameEventToClientAt`. |
| A value that had to travel with the movement needed a custom executor, a state struct with the fields `Location` and `Rotation`, and a pool policy. | Mark the property `CrowdyContinuous`, or set a Blueprint variable's `Crowdy Replication` to `Replicated (Continuous)`. An existing executor keeps working. | New: meta key `CrowdyStateMetaKeys::Continuous`, Blueprint mode, `FCrowdyRepProperty::bContinuous`, `FCrowdyRepLayout::ContinuousBits`, `UCrowdyEventRouter::DispatchContinuousExtras`, wire: extras after the state, which older peers do not read. |
| A Crowdy State update over about 1 KB was refused on the way out but counted as sent, so clients kept the old value until it next changed. | It splits into several deltas, measured against 1029 bytes on an actor and 969 on a replicated subsystem. A single property too big for one delta stays unsent and warns once with its name and entity; the owner's notify does not run for it until it goes. A host push that large is dropped with a warning. | Bug fix, behaviour. C++: `CrowdyStateBlobLimit`. |
| A long `FName` in another client's value could stop the receiver with a fatal check. | The read fails and the value is dropped. | Bug fix. |
| A `Preloaded Entity Classes` entry nothing else referenced was freed by the first level load. | The registry holds the classes it preloaded. | Bug fix. |
| A remote entity appearing while Mass was processing hit a fatal check. | Appearances wait for `UCrowdyActorTracker`'s tick, so `OnRemoteEntityAppeared` can fire up to one frame later. | Bug fix, behaviour. |
| An owner adopted a host-sourced Crowdy State update for its own entity under every Leader Source, and the client that writes a World entity or replicated subsystem adopted another client's, so a modified client could set the flag and make the writer keep a forged value. | Under Server Object an owner drops a host-sourced update for an entity it owns, and a writer drops any other client's update for what it writes. Other clients' copies can still show a forged value until the writer's next update. A 2.24 host's correction no longer reaches a 2.25 owner, so upgrade every client together. Elected Host is otherwise unchanged. | Bug fix, behaviour. |
| A failed HTTP/2 request logged its headers, bearer token included, through the engine's curl report, in any build with logging on. | `LogHttp` is held at `Error` while an SDK request is in flight, and a failure logs one line on `LogCrowdyCpp`. A game's own `LogHttp` warnings in that window are hidden. `VeryVerbose` is left alone. | Bug fix, behaviour. |
| A value converted for a `const` reference parameter of a `CROWDY_EVENT`, such as a string literal for a `const FString&`, was read after it was freed, so a packaged build could send garbage. | The call holds its own copy of every argument. | Bug fix. |
| An account with no age failed sign-in with the server's raw text, and a refused token refresh retried for ever. | `AGE_REQUIRED` holds the sign-in or refresh. The built-in age screen asks, or a game binds `OnAgeRequired` and calls `RecordPlayerAge`. New: `OnAgeRequired`, `OnAgeEntryEnded`, `RecordPlayerAge`, `DeclineAgeEntry`, `IsAgeEntryPending`, `IsAgeEntryLocked`, `ClearAgeEntryLock`, node `Record Player Age`. | Behaviour, Blueprint, C++. |
| `AGE_REQUIREMENT_NOT_MET`, `TERRITORY_NOT_SUPPORTED`, `SAFETY_MODERATOR_REQUIRED` and `SAFETY_LOG_REQUIRED` showed the server's text and rotation kept retrying. | They end access: `OnAccessEnded` fires, rotation stops, and the text names no age and no reason. An age refusal locks the install for a day. | Behaviour. |
| The consent prompt listed five retired documents and asked for an age-of-majority box. | It lists the platform's current documents and the app's own, with one box. New: `RequestLegalDocuments`. `GetLegalDocuments` now returns the Player Terms and Player Privacy Policy, as this release knew them. `RecordPlayerConsents` needs only `bAcceptLegal`. | Behaviour, Blueprint, C++. |
| Crowdy Studio asked only for the player terms, and a new account could not create an app from it. | After a sign-in Studio asks for the developer documents and, if the account has none, an age. Its terms window lists the live documents. | Behaviour, editor. |
| Sign-up and a recorded age sent no device id. | Both send `x-ck-device-id`, a random id the install keeps, so an age refusal locks a device and not a network. | Wire. |
| A game had no way to reach a player's safety controls. | New subsystem `UCrowdySafety`: `Get Communication Settings`, `Set Communication Setting`, `Request Parental Consent`, contacts, blocks and `File Report`, each also a Blueprint node. | Blueprint, C++, added. |
| Teams, Avatars, Channels and Kit Social named the app from the project setting, even after `Set App ID` changed the session's app. | They name the session's app, as the app token does, and fall back to the setting. | Behaviour. |
| `Mode` on the `Crowdy Entity Component`: `Static` or `Dynamic`. | `Replicate Movement`: `Auto`, `On` or `Off`. Auto is On for a pawn with a movement component, else Off. Saved assets are mapped. | Property, C++ `Mode` deprecated. |
| `Get Mode` | `Replicates Movement`, and C++ `ReplicatesMovementFor(ActorClass)` for a class. | Blueprint node, C++ `GetMode()` deprecated. |
| A remote character on the actor pool slid along in its idle pose unless you wrote a policy. | It animates with no code: its movement component gets a velocity and an acceleration, and the pawn a remote view pitch. No jump or fall animation: its movement mode is None. | Behaviour. |
| `FCrowdyActorState` was a transform. | It also carries `AimPitch`, filled by the default executor from where a pawn looks. | Wire, appended. |
| A game could not tell what the app records. | The SDK reads it when the player connects and when their token refreshes. New on `UCrowdySafety`: `On Recording State Changed`, `Get Recording State`, `Refresh Recording State`. A log warning fires once if voice opens in an app that records it with nothing bound. | Blueprint, C++, added. The notice and indicator are required while voice or video is recorded, and the game draws them. |
| An `Owning Client` event reaching a client that does not write the entity ran there. | It runs only on the writer. | Behaviour. |
| Under `Elected Host`, an `Owning Client` event on a World entity needed the host's avatar in range. | It goes over the session channel, named for the host. | Behaviour. |
| A forged channel payload could ask a client to allocate up to 2 GB. | The call's size is checked against the bytes that arrived. | Bug fix. |
| A crowd row kept the owner and role it was created with, and a stale row could speak for an entity this client no longer owned. | Rows follow ownership, sends from a row ask the entity registry, and a row is released when its entity becomes this client's. | Behaviour: C++ `UCrowdyActorManager`, `UCrowdyMassEntitySubsystem`, `ICrowdyEventSource` senders. |
| `SendEvent` on the `Crowdy Entity Component`, with `ECrowdyEventScope`. | `Send Crowdy Event` (C++ `UCrowdyUtilities::SendCrowdyEvent`), which takes a Recipient. | Blueprint node and C++ function deprecated. |

**Do this, in order**

1. In C++, replace `EntityComponent->Ownership = ECrowdyOwnership::LocalClient` with
   `EntityComponent->OwnedBy = ECrowdyOwnedBy::Player`, and `ECrowdyOwnership::Host` with
   `ECrowdyOwnedBy::World` (or delete the line: Auto already gives World for a placed actor). Replace
   `GetOwnership()` reads with `OwnedBy`.
2. Replace `Is Crowdy Entity Locally Controlled` with `Is Crowdy Entity Locally Owned`.
3. Replace `Switch Crowdy Has Authority` and `Crowdy Has Authority` with `Switch Is Crowdy Entity Locally Owned`
   or `Is Crowdy Entity Locally Owned` on the entity or replicated subsystem the gated code writes. In a
   replicated subsystem, pass the subsystem itself. Replace the other host nodes as the table above says. Under
   `Elected Host` there is no Leader, so a project that keeps it moves its Leader reads when it leaves that setting.
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
11. If you call `DispatchSingleActorMessageFn` or `DispatchSingleActorMessage` yourself, pass an `FCrowdyActorId`
   (`FCrowdyActorId::FromGuid(NetID)`), or call `UCrowdyEntitySubsystem::DispatchSingleActorMessageTo`, which
   finds the id the entity's owner sends under.
12. To move an NPC that needs the navmesh or physics off the elected host, set its `Owned By` to `Leader` and
   gate its simulation on `Is Crowdy Entity Locally Owned` each tick. Spawn it only where `Is Local Leader` is
   true. For NPCs across maps or a large open world, use Game Kit's hub NPCs instead: one Leader serves the
   whole app and writes only what it has a body for.
13. Replace `CrowdyRecipient="Host"` and the Host recipient in Blueprint with `OwningClient`, or move the logic to
   a Server Object. On a replicated subsystem `OwningClient` is the same client. On a World entity it is the
   elected host, and nobody under `Server Object`. Unlike Host, it never runs on the host for an entity a player
   owns: it runs on that player.
14. Replace `SendEvent(Payload, Everyone)` with `Send Crowdy Event` and `Spatial Multicast`, and `OwnerOnly` with
   `Owning Client`.
15. If you turned **Show Consent Prompt** off, bind `OnAgeRequired` as well as `OnLegalAcceptanceRequired`, and show
   `RequestLegalDocuments` rather than `GetLegalDocuments`. Remove any age-of-majority box. Your age screen must follow
   the platform's rules: see the Terms section of the authentication page.
16. In C++, replace `EntityComponent->Mode = ECrowdyEntityMode::Dynamic` with
   `EntityComponent->ReplicateMovement = ECrowdyReplicateMovement::On`, and `Static` with `Off`. Replace the line
   rather than deleting it: a Blueprint child saved before this release, under a C++ parent left at Static, stays
   without movement. Replace `Get Mode` with `Replicates Movement`.
17. If a pawn with a movement component should not send movement, set `Replicate Movement` to `Off`. Under `Auto` it
   now sends, unless a saved Blueprint already reads `Off`.
18. If an aim offset reads `Get Base Aim Rotation` on a remote character, normalize the pitch: looking down at -40
   reads 320.

**CoreRedirects.** None.

**Blueprint assets.** Search recursively with `grep -a`, and check the search with a control string that
must match:

```
grep -r -a -l --include=*.uasset --include=*.umap -e "IsCrowdyEntityLocallyControlled" -e "CrowdyHasAuthority" -e "GetOwnership" -e "GetMode" -e "RegisterReplicatedSubsystem" -e "CrowdyRecipient" -e "SendEvent" -e "CrowdyHostSubsystem" -e "GetHostID" -e "GetHostUserID" -e "OnHostIDUpdated" -e "OnHostElected" -e "IsHost" -e "IsCrowdyEntityHost" -e "GrantOwnershipToHost" -e "GetHostOverridePolicy" Content Plugins
grep -r -a -l --include=*.uasset --include=*.umap "OnCrowdySpawned" Content Plugins
grep -r -a -l --include=*.uasset --include=*.umap "CrowdyEntityComponent" Content Plugins
```

`IsHost` also matches `IsHostSet`, and `SendEvent` matches Control Rig assets that never touch the SDK: check each hit
for a Crowdy name before you change it.

The first finds graphs using a deprecated node: replace it as above and re-save. The second finds
Blueprints that bind `On Crowdy Spawned` on entity classes a Mass map can draw; they need no re-save.

**Behaviour changes.** An override of `GetReplicatedOwnership()` on a replicated subsystem base is no longer
called: the subsystem is written by its one writer whatever it returned. Returning anything but Host was
never supported. A native pawn class with a movement component and no asset, left at Static, now sends movement,
because its `Replicate Movement` is `Auto`. If it is spawned with `Spawn Entity`, it stops repeating its spawn event as
every moving entity does, so a client that joins later draws it from its updates, with no `On Crowdy Spawned` data. The
default actor state is 4 bytes longer: an older peer ignores the aim
pitch, and a newer peer reads an older peer's aim as level.

The default is `Server Object`, so a project that changes nothing loses its elected host: World entities are
written by nobody, a `CrowdyRecipient="Host"` event on any actor is dropped (the SDK warns once, naming the
setting, when your code relies on a writer or a host; move such logic to a Server Object), and replicated
subsystems are written by the app's Leader once the server code is deployed. Until it is, nothing writes them.
When the lead passes, the new Leader re-sends the subsystem values it holds; a value it never received (one that
changed before it joined and is not `CrowdyHeartbeat`-marked) is not carried over, so mark subsystem state
`CrowdyHeartbeat`.

Under `Elected Host`, an `Owning Client` event on a World entity runs on the host instead of being dropped.

`Owning Client` limits who runs an event, not who can see it or who can send it. Its parameters reach every client
within a chunk of the target, or on the session channel every client of the app, and any client can name any entity,
so treat them as untrusted and keep private data in a Server Object. The session channel reaches the whole app and carries 1024 bytes at most, and an
entity that sends no movement stays on it, so give anything frequent a Dynamic entity. A call around a change of
owner can still be lost if it reaches the old owner by a targeted send.

Each `Owning Client` call now costs a copy for every client near the target, so in a crowded area its traffic grows
with the square of the players there. Keep it to occasional events. The sender needs `access` in a grid covering the
target's chunk, so in an app that grants access zone by zone a call can be refused where it was not before. A
reception layer sees calls named for other clients.

An `Owning Client` event that carries text is a direct message: it can still be lost with no message while the entity
crosses a chunk border, and players without `use_direct_messages` can neither send nor receive it. Move private text
between players to a Server Function.

Every client must run the same SDK version once entities change hands or use `Owned By` `Leader`: an older
client reads an owner's id as a new entity, and draws a second body. Nothing changes for an entity that never
changes hands. An entity that has changed hands keeps its owner's id, so a reconnecting owner may have sends
refused until the server lets go of the old session.

A Leader writes only what it spawned or already held, because a spawn event or update can be forged. A new
Leader that never had an entity does not take it: other clients hold the body for 20 seconds and then release
it. A body placed in the level stays where it stopped. A client that walks out of an entity's range holds it
for those 20 seconds too, so count that when you size `Max Pool Size Per Class`.

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
8. With two clients, spawn a moving entity on the first and grant it to the second: it keeps moving on the
   first, with one body, and `Owning Client` events reach the second.
9. Under `Server Object`, spawn an `Owned By` `Leader` entity on the Leader and close that client: the body
   stops on the other client and moves on from there once that client leads, with `Is Crowdy Entity Locally
   Owned` true.
10. Spawn a Static entity with `Owned By` `Leader` on the Leader and send it an `Owning Client` event from the other
   client: its body runs on the Leader. The Recipient dropdown no longer offers Host, and a Blueprint event still
   set to it warns when compiled.
11. With two clients, walk and look down on the first: the second shows the character walking and aiming down, with
   no executor, policy or animation code of yours.

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
