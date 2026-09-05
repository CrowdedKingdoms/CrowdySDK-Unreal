#pragma once

// This header names CrowdyCPP types, so it includes the CrowdyCPP header
// directly (no UE THIRD_PARTY guard, which would not be defined when this
// private header is the first include in a bridge translation unit).
// websocket.hpp pulls no UE or winsock headers, so plain inclusion is safe.
#include "crowdy/graphql/websocket.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The bridge's WebSocket transports for GraphQL subscriptions. The subscription client drives an injected
// IWebSocketTransport, so the only thing it needs from the engine is a socket behind this interface.
// This header is private to the bridge because it names third-party types; dependent modules never see it.
namespace CrowdyCppTransport
{
	// Transport over Unreal's FWebSocketsModule. Returns null on a platform built without WebSocket support, which
	// the subscription client reports as an unavailable transport rather than treating as a failure to retry.
	std::shared_ptr<crowdy::graphql::IWebSocketTransport> MakeFWebSocketTransport();

	/**
	 * Let go of every socket whose connection has already been dropped, now, on the calling thread.
	 *
	 * A connection can be dropped from inside a socket event or from a background thread, and in neither case can
	 * the socket be released there, so it is parked and released from a queued game-thread task. That task is fine
	 * as the ordinary path and useless as a teardown guarantee: nothing promises it runs before the engine's
	 * WebSocket support is torn down, and a socket released after that point is a crash rather than a late tidy-up.
	 * So an owner disposing of its client calls this while it still holds the game thread, and knows on return that
	 * it left nothing behind. Game thread only. Safe to call when there is nothing parked.
	 */
	void FlushPendingWebSocketReleases();

	/**
	 * The server side of a connection, for tests. It replaces the socket entirely: nothing is created, no bytes
	 * leave the process, and the test plays the server by hand. That is what makes the graphql-transport-ws
	 * handshake testable headlessly, since it is a conversation rather than a single round trip.
	 *
	 * Everything is behind one lock because the two sides genuinely run on different threads: a test drives the
	 * server half from the game thread, while the subscription client opens a replacement connection from its own
	 * timer thread whenever it reconnects. Plain standard types throughout, since this header sees only the
	 * CrowdyCPP includes above.
	 */
	class FScriptedWebSocketServer
	{
	public:
		// Report the connection as established. The client answers with connection_init.
		void Open();

		// Deliver one text frame.
		void ReceiveText(const std::string& Text);

		// Close from the server side. A non-clean close is what the client's reconnect policy reacts to.
		void CloseFromServer(std::uint16_t Code, const std::string& Reason, bool bClean);

		// How many connections the subscription client has asked for. More than one means it reconnected.
		int ConnectionsCreated() const;

		// Whether the client closed the live connection from its side, and with which code.
		bool WasClosedByClient() const;
		std::uint16_t ClientCloseCode() const;

		// Every text frame the client has sent since the last call, oldest first.
		std::vector<std::string> TakeSentFrames();

		// Driven by the connection this server stands in for.
		void NoteConnectionCreated();
		void NoteStarted(crowdy::graphql::WebSocketEventCallback Callback);
		bool NoteSent(std::string Text);
		void NoteClosedByClient(std::uint16_t Code);

	private:
		// Taken under the lock and invoked outside it: a delivered event can make the client send or close, which
		// comes straight back through this same server.
		crowdy::graphql::WebSocketEventCallback LiveCallback() const;

		mutable std::mutex Mutex;
		crowdy::graphql::WebSocketEventCallback Callback;
		std::vector<std::string> SentFrames;
		int ConnectionsCreatedCount = 0;
		std::uint16_t CloseCode = 0;
		bool bClosedByClient = false;
	};

	// Transport that hands every connection to Server instead of opening a socket.
	std::shared_ptr<crowdy::graphql::IWebSocketTransport> MakeScriptedWebSocketTransport(
		std::shared_ptr<FScriptedWebSocketServer> Server);
}
