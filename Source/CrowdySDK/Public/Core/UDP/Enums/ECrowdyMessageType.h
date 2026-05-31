#pragma once

#include "CoreMinimal.h"
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
 * - ACTOR_UPDATE_RESPONSE: Server response after processing a failed actor update request.
 * - ACTOR_UPDATE_NOTIFICATION: Notification of actor data updates to subscribers.
 * - VOXEL_UPDATE_REQUEST: Client request to update voxel data.
 * - VOXEL_UPDATE_RESPONSE: Server response after processing a voxel update request.
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
    
    
    
    ACTOR_UPDATE_REQUEST = 128 UMETA(DisplayName = "Actor Update Request", Hidden),
    ACTOR_UPDATE_RESPONSE = 129 UMETA(DisplayName = "Actor Update Response"),
    ACTOR_UPDATE_NOTIFICATION = 130 UMETA(DisplayName = "Actor Update Notification"),
    VOXEL_UPDATE_REQUEST = 131 UMETA(DisplayName = "Voxel Update Request", Hidden),
    VOXEL_UPDATE_RESPONSE = 132 UMETA(DisplayName = "Voxel Update Response"),
    VOXEL_UPDATE_NOTIFICATION = 133 UMETA(DisplayName = "Voxel Update Notification"),
    CLIENT_AUDIO_PACKET = 134 UMETA(DisplayName = "Client Audio Packet", Hidden),
    CLIENT_AUDIO_NOTIFICATION = 135 UMETA(DisplayName = "Client Audio Notification", Hidden),
    CLIENT_TEXT_PACKET = 136 UMETA(DisplayName = "Client Text Packet", Hidden),
    CLIENT_TEXT_NOTIFICATION = 137 UMETA(DisplayName = "Client Text Notification", Hidden),
    CLIENT_EVENT_NOTIFICATION = 138 UMETA(DisplayName = "Client Event Notification"),
    SERVER_EVENT_NOTIFICATION = 139 UMETA(DisplayName = "Server Event Notification", Hidden),
    GENERIC_SPATIAL_1 = 140 UMETA(DisplayName = "Generic Spatial 1", Hidden),
};


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
    Linear_100 = 2 UMETA(DisplayName = "Linear 100 Decay"),
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