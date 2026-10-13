This folder holds the map profile CrowdySDK ships, alongside the SDK's other shipped data assets.

Expected asset: DA_CrowdySDKDefaultProfile (a Crowdy Map Profile data asset).
Full path: /CrowdySDK/Data/DA_CrowdySDKDefaultProfile

A map that names no profile of its own, in a project whose Default Profile is unset, runs on this
one. It carries pure defaults, which is what makes it select the actor pool: Backend Class already
defaults to Crowdy Actor Pool Backend, and networking is already enabled.

To create it: Content Browser -> Plugins -> CrowdySDK Content -> Data -> right click ->
Miscellaneous -> Data Asset -> Crowdy Map Profile. Name it exactly DA_CrowdySDKDefaultProfile and
save it without changing anything.

Because it changes nothing from the defaults, the asset stores no property values at all, and what it
means follows the C++ defaults on FCrowdyActorManagementConfigStruct. Changing the default Backend
Class in C++ therefore changes what every project running on this profile draws with. That is the
intended behaviour, but it is not visible in the asset, so change that default deliberately.

Drawing entities through a different backend, Mass included, is a choice a project makes by authoring
a profile that names one. No rendering plugin offers a default of its own; if one did, whichever
registered first would win and the answer would depend on module load order.

Until the asset exists, any Game or PIE map that configures nothing says so in the log and names this
path, so a missing default is never silent.

CSO_PlayersOnline (a Crowdy Server Object definition, /CrowdySDK/Data/CSO_PlayersOnline) is the root
Server Object every app has. Deploying from Server Compute generates the root's server code from it,
and Get Players Online reads its one variable, Players Online, by path; so it lives here, where every
cook includes it. Its Type Name is root, it has Only One Instance (the root's Instance Id is empty),
its State Struct is Crowdy Players Online State, and Players Online is Visible to Players. Do not
change it: the root's server code is the SDK's, and a project deploys it as it is.
