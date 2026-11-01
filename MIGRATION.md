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

## 2.24.0 (unreleased)

**Verdict.** No breaking change. Nothing is deprecated.

**Requires.** Unreal Engine 5.8. Nothing to deploy or regenerate.

**What changed**

| Old | New | Kind |
|---|---|---|
| Host user id 0 ("no host": after sign-out, before the first election on a map, or a host poll answer naming user 0) became a valid-looking host id, and sign-out did not always clear the host. | It becomes an empty (invalid) host id, and sign-out clears the host at once. | Behaviour: C++ `UCrowdyHostSubsystem::SetHostUserID`, `UCrowdySDKSubsystem::Logout`; Blueprint `Is Host Set`, `Get Host ID`, `On Host Elected`. |
| A packaged plugin (UAT BuildPlugin, which the SDK's package script uses) left out the plugin's `Config` folder, so `Config/DefaultCrowdySDK.ini` and the CoreRedirects the SDK carries for itself were missing from packaged installs. | The package includes `Config/`, `MIGRATION.md`, `README.md` and the license file. | Packaging: `Config/FilterPlugin.ini`. |
| No migration file. | `MIGRATION.md` at the plugin root. | Docs. |

**Do this, in order**

1. If you bind `On Host Elected` (Blueprint) or `OnHostElected` (C++), treat an invalid `HostID`
   or `PreviousHostID` as "no host": check it with `Is Valid (Guid)` / `FGuid::IsValid` before
   using it.
2. If you install the SDK from a packaged zip, replace it with a 2.24.0 package.
3. If you copied the SDK's CoreRedirects into your own project's `Config/DefaultEngine.ini`
   because they were missing, you may keep them. Duplicate redirects are harmless and removing
   them is optional.

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

**Verify**

1. Build your project's editor target and confirm the log shows `Compile` and `Link` lines for
   the CrowdySDK modules.
2. In Play In Editor, sign in, then sign out, and confirm `Is Host Set` returns false.
3. For a packaged install, confirm the plugin folder contains `Config/DefaultCrowdySDK.ini` and
   `MIGRATION.md`.

**Removed in the next release.** None.
