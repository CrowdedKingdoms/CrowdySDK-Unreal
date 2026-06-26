// Anchor TU: forces MSVC to emit CROWDYNET_API dllexport stubs for
// ICrowdySubscriptionHandler ctor/dtor/RTTI in CrowdyNet.dll so that
// consumer modules that subclass it can link the base ctor/dtor.
#include "Network/GraphQL/Subscription/ICrowdySubscriptionHandler.h"
