This folder holds a ready-made map profile that selects the Crowdy Mass rendering backend.

Asset: DA_CrowdyMassDefaultProfile (a Crowdy Map Profile data asset).
Full path: /CrowdySDK/Mass/Data/DA_CrowdyMassDefaultProfile

Nothing registers this profile and nothing resolves it on its own. It is a starting point a project
opts into, by pointing Project Settings, Plugins, Crowdy SDK, Map Profiles at it: either as the
Default Profile, to draw every map through Mass, or as the entry for one map.

It selects the Crowdy Mass Rendering Backend, has Enable Networking on, and leaves Backend Config
empty, which means the backend reads Project Settings, Plugins, Crowdy Mass.

A project that configures no profile runs on the profile the SDK registers, which draws actors. Which
backend draws a project's entities is the project's decision. The crowd-only authoring surfaces follow
the same fact, so they stay hidden until some profile the project can reach selects a crowd backend.
