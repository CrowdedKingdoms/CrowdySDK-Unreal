#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "ECrowdyMessageType.generated.h"

/**
 * @brief Enum representing the various types of messages handled by the Crowdy system.
 *
 * This enumeration defines the possible message types that can be exchanged
 * between clients and servers in the Crowdy framework. Each message type is
 * assigned a unique identifier to facilitate identification and processing.
 * The DisplayName specified for each message type is primarily used for
 * human-readable representation within Unreal Engine's editor interfaces.
 *
 * The values included in this enumeration are:
 * - BAD_MESSAGE: Represents an invalid or unrecognized message.
 * - ACTOR_UPDATE_REQUEST: Client request to update actor data.
 * - ACTOR_UPDATE_RESPONSE: No longer sent by the server; kept as a reserved wire identifier.
 * - ACTOR_UPDATE_NOTIFICATION: Notification of actor data updates to subscribers.
 * - VOXEL_UPDATE_REQUEST: Client request to update voxel data.
 * - VOXEL_UPDATE_RESPONSE: No longer sent by the server; kept as a reserved wire identifier.
 * - VOXEL_UPDATE_NOTIFICATION: Notification of voxel data updates to subscribers.
 * - CLIENT_AUDIO_PACKET: Packet containing client-side audio data.
 * - CLIENT_AUDIO_NOTIFICATION: Notification of audio-related events on the client.
 * - CLIENT_TEXT_PACKET: Packet containing client-side text data.
 * - CLIENT_TEXT_NOTIFICATION: Notification of text-related events on the client.
 * - CLIENT_EVENT_NOTIFICATION: Notification of a client-side event.
 * - SERVER_EVENT_NOTIFICATION: Notification of a server-side event.
 * - RESERVED_13: Reserved for future use.
 * - MESSAGE_BUNDLE: Bundle of multiple messages for efficient transmission.
 */
UENUM(BlueprintType)
enum class ECrowdyMessageType : uint8
{
    BAD_MESSAGE = 0  UMETA(DisplayName = "!! INVALID - Do not use !!", Hidden),
    RESERVED_1 = 1 UMETA(DisplayName = "Reserved 1", Hidden),
    MESSAGE_BUNDLE = 2 UMETA(DisplayName = "Message Bundle", Hidden),
    GENERIC_ERROR_MESSAGE = 3 UMETA(DisplayName = "Generic Error Message", Hidden),
    RESERVED_13 = 13 UMETA(DisplayName = "Reserved 13", Hidden),

    // Non-spatial channel transport (high bit clear): a message reaches every active member of
    // the channel regardless of location. Request is client->server (publish), Notification is
    // server->client (deliver to every member except the sender).
    CHANNEL_MESSAGE_REQUEST = 17 UMETA(DisplayName = "Channel Message Request", Hidden),
    CHANNEL_MESSAGE_NOTIFICATION = 18 UMETA(DisplayName = "Channel Message Notification", Hidden),

    // Says an actor is still present without restating its state. Carries the spatial header and nothing else, so
    // it costs a fraction of an actor update and is what an idle actor sends between full ones.
    CLIENT_ACTOR_HEARTBEAT = 26 UMETA(DisplayName = "Client Actor Heartbeat", Hidden),
    
    
    
    ACTOR_UPDATE_REQUEST = 128 UMETA(DisplayName = "Actor Update Request", Hidden),

    // Retired. The server stopped answering an actor update and reports every failure as a generic error
    // instead, so this opcode never arrives and nothing decodes it. The value stays reserved so the numbering
    // behind it does not shift. Do not add handling for it.
    ACTOR_UPDATE_RESPONSE = 129 UMETA(DisplayName = "Actor Update Response (Retired)", Hidden),

    ACTOR_UPDATE_NOTIFICATION = 130 UMETA(DisplayName = "Actor Update Notification"),
    VOXEL_UPDATE_REQUEST = 131 UMETA(DisplayName = "Voxel Update Request", Hidden),

    // Retired, on the same terms as the actor response above. This one used to carry the rejection that
    // reverted an optimistic local voxel placement, so a rejected edit now leaves that placement standing.
    // Reviving the revert means reading the generic error, which is a server-side conversation first.
    VOXEL_UPDATE_RESPONSE = 132 UMETA(DisplayName = "Voxel Update Response (Retired)", Hidden),

    VOXEL_UPDATE_NOTIFICATION = 133 UMETA(DisplayName = "Voxel Update Notification"),
    CLIENT_AUDIO_PACKET = 134 UMETA(DisplayName = "Client Audio Packet", Hidden),
    CLIENT_AUDIO_NOTIFICATION = 135 UMETA(DisplayName = "Client Audio Notification", Hidden),
    CLIENT_TEXT_PACKET = 136 UMETA(DisplayName = "Client Text Packet", Hidden),
    CLIENT_TEXT_NOTIFICATION = 137 UMETA(DisplayName = "Client Text Notification", Hidden),
    CLIENT_EVENT_NOTIFICATION = 138 UMETA(DisplayName = "Client Event Notification"),
    SERVER_EVENT_NOTIFICATION = 139 UMETA(DisplayName = "Server Event Notification", Hidden),
    GENERIC_SPATIAL_1 = 140 UMETA(DisplayName = "Generic Spatial 1", Hidden),
    // Actor-to-actor send: identical wire layout to a game event, but the server delivers it
    // only to the single client that owns the destination actor (the message's UUID) instead
    // of broadcasting to everyone in range.
    SINGLE_ACTOR_MESSAGE = 142 UMETA(DisplayName = "Single Actor Message", Hidden),

    // Webcam video, shaped like the audio pair above. One datagram carries a single FRAGMENT of an
    // encoded frame, never a whole one, so a consumer reassembles before it has an image. Gated by the
    // app's use_video_chat capability, which the server enforces.
    CLIENT_VIDEO_PACKET = 143 UMETA(DisplayName = "Client Video Packet", Hidden),
    CLIENT_VIDEO_NOTIFICATION = 144 UMETA(DisplayName = "Client Video Notification", Hidden),

    // Server->client only: the server stopped considering an actor present. Sent once over the actor's
    // last chunk, and never for a move between chunks. A later update for the same actor is a rejoin.
    ACTOR_LEFT_NOTIFICATION = 145 UMETA(DisplayName = "Actor Left Notification"),
};

/**
 * How the bytes of a video frame are encoded.
 *
 * Jpeg and WebP are the two the protocol assigns, and a fragment naming anything else is dropped before it
 * reaches a decoder. Unknown is therefore not a value that arrives: it is what a byte outside the assigned
 * pair maps to when one is read in isolation, so a future codec reads as Unknown rather than as WebP. It
 * sits well below 255 because UHT gives a uint8 enum an implicit _MAX one past the highest entry.
 */
UENUM(BlueprintType, meta=(DisplayName="Video Codec"))
enum class ECrowdyVideoCodec : uint8
{
    Jpeg = 0 UMETA(DisplayName = "JPEG"),
    WebP = 1 UMETA(DisplayName = "WebP"),
    // A codec byte this build has no entry for.
    Unknown = 254 UMETA(DisplayName = "Unknown"),
};

/**
 * The layout of the header every video fragment leads with, and the limits a frame is split against.
 *
 * These mirror the shared contract both SDKs implement byte for byte. FCrowdyMessageParser holds the
 * static assertions that tie them to the vendored constants, so a re-vendor that moved one breaks the
 * build rather than the wire.
 *
 *   offset  size  field
 *   0       1     version, always 1
 *   1       1     codec
 *   2       2     frame id, big endian, per sender
 *   4       1     fragment index
 *   5       1     fragment count, 1 to 16
 *   6       ...   this fragment's slice of the encoded frame
 */
namespace CrowdyVideoFragment
{
    constexpr int32 HeaderBytes = 6;
    constexpr uint8 Version = 1;
    constexpr int32 MaxBodyBytes = 1117;
    constexpr int32 MaxFragments = 16;
    constexpr int32 FrameTimeoutMs = 500;

    constexpr int32 VersionOffset = 0;
    constexpr int32 CodecOffset = 1;
    constexpr int32 FrameIdOffset = 2;
    constexpr int32 IndexOffset = 4;
    constexpr int32 CountOffset = 5;
}

/** The codec a fragment names, or Unknown for a byte outside the assigned pair. */
inline ECrowdyVideoCodec CrowdyVideoCodecFromByte(const uint8 Codec)
{
    if (Codec == static_cast<uint8>(ECrowdyVideoCodec::Jpeg))
    {
        return ECrowdyVideoCodec::Jpeg;
    }

    if (Codec == static_cast<uint8>(ECrowdyVideoCodec::WebP))
    {
        return ECrowdyVideoCodec::WebP;
    }

    return ECrowdyVideoCodec::Unknown;
}

/**
 * Why the server stopped considering an actor present.
 *
 * The wire carries a single byte, and the protocol reserves everything past the two values below with the
 * instruction to treat them as a stale drop. There is deliberately no Unknown case: a reserved byte is not
 * a third outcome, it is a stale drop this build cannot describe any further, and offering a case that no
 * payload can produce would put a branch in front of a designer that can never be taken. The raw byte is
 * kept on the decoded message for anyone who needs to tell the two apart.
 */
UENUM(BlueprintType, meta=(DisplayName="Actor Left Reason"))
enum class ECrowdyActorLeftReason : uint8
{
    // The server heard nothing from the actor for long enough to drop it, about five seconds.
    Stale = 0 UMETA(DisplayName = "Stale"),
    // The server ended the actor's session, through deauthorisation or an expired token.
    SessionReleased = 1 UMETA(DisplayName = "Session Released"),
};

/**
 * Reads the reason out of an Actor Left payload.
 *
 * An absent or empty payload reads as Stale, and so does any byte the protocol has not defined: the wire
 * format reserves 2 to 255 and requires an unrecognised value to be treated as a plain stale drop.
 */
inline ECrowdyActorLeftReason CrowdyActorLeftReasonFromPayload(const TConstArrayView<uint8> Payload)
{
    if (Payload.IsEmpty())
    {
        return ECrowdyActorLeftReason::Stale;
    }

    switch (Payload[0])
    {
    case 0:
        return ECrowdyActorLeftReason::Stale;
    case 1:
        return ECrowdyActorLeftReason::SessionReleased;
    default:
        return ECrowdyActorLeftReason::Stale;
    }
}


/**
 * @brief Represents the various error codes used within the Crowdy system.
 *
 * This enum is used to define error codes that can occur during various operations in the system.
 * These codes are primarily intended for communicating the state or result of an operation.
 *
 * The values in this enum have metadata annotations to describe their intended display names.
 */
UENUM(BlueprintType)
enum class ECrowdyErrorCode : uint8
{
    SUCCESS = 0 UMETA(DisplayName = "No Error"),
    UNKNOWN_ERROR = 1 UMETA(DisplayName = "Unknown Error"),
    EMAIL_NOT_FOUND = 2 UMETA(DisplayName = "Email Not Found"),
    BAD_PASSWORD = 3 UMETA(DisplayName = "Bad Password"),
    EMAIL_ALREADY_EXISTS = 4 UMETA(DisplayName = "Email Already Exists"),
    INVALID_TOKEN = 5 UMETA(DisplayName = "Invalid Token"),
    MAP_NOT_FOUND = 6 UMETA(DisplayName = "Map Not Found"),
    UNAUTHORIZED = 7 UMETA(DisplayName = "Unauthorized"),
    MAP_NOT_LOADED = 8 UMETA(DisplayName = "Map Not Loaded"),
    EMAIL_TOO_SHORT = 9 UMETA(DisplayName = "Email Too Short"),
    EMAIL_TOO_LONG = 10 UMETA(DisplayName = "Email Too Long"),
    PASSWORD_TOO_SHORT = 11 UMETA(DisplayName = "Password Too Short"),
    PASSWORD_TOO_LONG = 12 UMETA(DisplayName = "Password Too Long"),
    GAME_TOKEN_WRONG_SIZE = 13 UMETA(DisplayName = "Game Token Wrong Size"),
    NAME_TOO_LONG = 14 UMETA(DisplayName = "Name Too Long"),
    INVALID_REQUEST = 15 UMETA(DisplayName = "Invalid Request"),
    EMAIL_INVALID = 16 UMETA(DisplayName = "Email Invalid"),
    INVALID_TOKEN_LENGTH = 17 UMETA(DisplayName = "Invalid Token Length"),
    INVALID_MAP_ID = 18 UMETA(DisplayName = "Invalid Map ID"),
    CHUNK_NOT_FOUND = 19 UMETA(DisplayName = "Chunk Not Found"),
    USER_NOT_AUTHENTICATED = 20 UMETA(DisplayName = "User Not Authenticated")

};

/**
 * @brief Enum representing different types of Crowdy actors.
 *
 * This enum is primarily used to categorize actors in the Crowdy system.
 */
UENUM(BlueprintType)
enum class ECrowdyActorType : uint8
{
    NONE = 0  UMETA(DisplayName = "None"),
    NPC_1 = 1 UMETA(DisplayName = "Random Walker NPC"),
};

/**
 * @brief Enum defining the various permissions within the Crowdy system.
 *
 * This enumeration outlines a comprehensive set of permissions that can be
 * assigned to participants or entities in the Crowdy framework. Each permission
 * is represented as an individual flag, enabling fine-grained access control
 * and specific behavioral restrictions.
 *
 * Permissions included in this enumeration are:
 * - None: Represents no permissions assigned.
 * - IsOwner: Indicates ownership permission.
 * - IsSuperAdmin: Super administrator permission with elevated privileges.
 * - IsAdmin: Administrator permission with significant control over system operations.
 * - IsModerator: Moderator permission for managing community interactions.
 * - IsMember: Regular member permission.
 * - CanFly: Permission to enable flying capabilities.
 * - CannotFly: Restriction from flying capabilities.
 * - CanCreateVoxel: Permission to create voxel structures.
 * - CannotCreateVoxel: Restriction from creating voxel structures.
 * - CanDestroyVoxel: Permission to destroy voxel structures.
 * - CannotDestroyVoxel: Restriction from destroying voxel structures.
 * - CanEnter: Permission to enter specific areas or zones.
 * - CannotEnter: Restriction from entering specific areas or zones.
 * - CanTeleportIn: Permission to teleport into a specific location.
 * - CannotTeleportIn: Restriction from teleporting into a specific location.
 * - CanTeleportOut: Permission to teleport out of a specific location.
 * - CannotTeleportOut: Restriction from teleporting out of a specific location.
 * - CanUseWeapon: Permission to use weapons.
 * - CannotUseWeapon: Restriction from using weapons.
 */
UENUM()
enum class ECrowdyPermissions : uint8
{
    None = 0,
    IsOwner = 1,
    IsSuperAdmin = 2,
    IsAdmin = 3,
    IsModerator = 4,
    IsMember = 5,
    CanFly = 6, 
    CannotFly = 7,
    CanCreateVoxel = 8, 
    CannotCreateVoxel = 9,
    CanDestroyVoxel = 10, 
    CannotDestroyVoxel = 11,
    CanEnter = 12,
    CannotEnter = 13,
    CanTeleportIn = 14,
    CannotTeleportIn = 15,
    CanTeleportOut = 16,
    CannotTeleportOut = 17,
    CanUseWeapon = 18,
    CannotUseWeapon = 19
};

UENUM(BlueprintType, meta=(DisplayName="Decay Rate"))
enum class ECrowdyDecayRate : uint8
{
    No_Decay = 0 UMETA(DisplayName = "No Decay"),
    Exponential_Decay = 1 UMETA(DisplayName="Exponential Decay"),
    Linear_50 = 2 UMETA(DisplayName = "Linear 50 Decay"),
    Linear_25 = 3 UMETA(DisplayName= "Linear 25 Decay"),
    Linear_10 = 4 UMETA(DisplayName= "Linear 10 Decay"),
    Linear_5 = 5 UMETA(DisplayName= "Linear 5 Decay"),
};

UENUM(BlueprintType, meta=(DisplayName="Replication Distance"))
enum class ECrowdyReplicationDistance : uint8
{
    None = 0 UMETA(DisplayName = "None"),
    One_Chunk = 1 UMETA(DisplayName = "One Chunk"),
    Two_Chunks = 2 UMETA(DisplayName = "Two Chunks"),
    Three_Chunks = 3 UMETA(DisplayName = "Three Chunks"),
    Four_Chunks = 4 UMETA(DisplayName = "Four Chunks"),
    Five_Chunks = 5 UMETA(DisplayName = "Five Chunks"),
    Six_Chunks = 6 UMETA(DisplayName = "Six Chunks"),
    Seven_Chunks = 7 UMETA(DisplayName = "Seven Chunks"),
    Eight_Chunks = 8 UMETA(DisplayName = "Eight Chunks"),
};

/**
 * Who runs a replicated CrowdyEvent, and over which transport. Spatial Multicast runs it on everyone
 * in range over the chunk-based path (decay-thinned, position-dependent). Multicast runs it on every
 * member of the session channel regardless of distance, over the channel transport: it ignores the
 * sender's chunk coordinates. OwningClient runs it only on the target entity's owner (a non-owner
 * routes a request to that owner); Host runs it only on the elected host (a non-host routes a request
 * to the host, and the host may act on any entity it has locally, regardless of ownership).
 */
UENUM(BlueprintType)
enum class ECrowdyEventRecipient : uint8
{
    // Everyone in range runs it, over the chunk-based spatial path (decay-thinned). The default.
    SpatialMulticast UMETA(DisplayName = "Spatial Multicast"),
    // Every member of the session channel runs it, over the channel transport: any distance, no decay.
    Multicast        UMETA(DisplayName = "Multicast"),
    // Only the client that owns the target entity runs it.
    OwningClient     UMETA(DisplayName = "Owning Client"),
    // Only the elected host runs it; the host may act on any entity it has locally.
    Host             UMETA(DisplayName = "Host"),
};
