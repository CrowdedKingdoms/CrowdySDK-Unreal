#pragma once

#include "CoreMinimal.h"
#include "CrowdyCppClient.h"
#include "Templates/SharedPointer.h"

/**
 * Owns one admin-scoped API client for the length of a single editor flow, such as deploying a kit or syncing
 * schema to the server. It is the counterpart to the game instance's runtime client: the same client, a different
 * bearer, and a lifetime measured in one operation rather than one session.
 *
 * The point of the type is that ownership is the only thing a caller has to get right. Creating it registers the
 * completion pump; releasing the last reference disposes the client and stops the pump. There is no close call to
 * remember and no ticker handle to unregister, so an early return or an error path cannot leave a client polling
 * for the rest of the editor session.
 *
 * Hold it with a shared pointer for as long as the flow needs its completions to arrive, and drop it when the flow
 * finishes. A request still in flight when the last reference goes completes as canceled, so an abandoned flow ends
 * in a reported failure rather than in a callback that never comes.
 */
class CROWDYNET_API FCrowdyCppAdminClientHost : public TSharedFromThis<FCrowdyCppAdminClientHost>
{
public:
	/**
	 * Build an admin-scoped client and start pumping it. Returns null when the client could not be constructed.
	 *
	 * An editor flow normally has no app endpoint resolved yet, so pointing ApiUrl at the shared origin is the
	 * right default: authoring operations are answered there, and a WRONG_DATACENTER redirect moves the client for
	 * the ones that are not.
	 */
	static TSharedPtr<FCrowdyCppAdminClientHost> Create(const FCrowdyCppClientConfig& Config,
		const FString& AdminToken);

	~FCrowdyCppAdminClientHost();

	FCrowdyCppAdminClientHost(const FCrowdyCppAdminClientHost&) = delete;
	FCrowdyCppAdminClientHost& operator=(const FCrowdyCppAdminClientHost&) = delete;

	// The hosted client. Never null for a host that Create returned.
	TSharedRef<FCrowdyCppClient> GetClient() const;

private:
	FCrowdyCppAdminClientHost() = default;

	TSharedPtr<FCrowdyCppClient> Client;
};
