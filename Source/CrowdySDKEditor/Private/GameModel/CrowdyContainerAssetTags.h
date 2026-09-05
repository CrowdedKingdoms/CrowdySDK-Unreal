// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Publishes two asset-registry tags on every Blueprint save so the Game Model schema scan can tell a container
 * from a non-container without loading the package.
 *
 * The tags are a pair and only mean anything together. CrowdyScan says "this asset has been described"; only then
 * does the absence of CrowdyContainerType mean "not a container". An asset carrying neither has never been
 * described, and the scan loads it, which is how every asset behaved before these tags existed.
 *
 * The write refuses to guess. A Blueprint whose class did not compile, or whose compiled class disagrees with the
 * authoring marker, publishes NOTHING: the scan then loads it and reads the live class, so a container that is
 * momentarily broken stays visible instead of quietly disappearing from the schema.
 */
namespace CrowdyContainerAssetTags
{
	/** What the tag writer can see about one Blueprint at save time. */
	struct FContainerTagInput
	{
		// The Blueprint has a generated class with a class-default object, so its compiled form exists to be read.
		bool bClassFormed = false;

		// The compiled class reflects the current source (the Blueprint compiled without error this session).
		bool bClassUpToDate = false;

		// The Blueprint carries the persisted container marker, which is true from the moment a designer marks it.
		bool bMarkedContainer = false;

		// The compiled class carries the CrowdyContainer tag, which only appears after a successful compile.
		bool bClassCarriesContainerTag = false;

		// The container type name the compiled class declares. Empty when it declares none.
		FString ClassContainerTypeName;
	};

	/** The tags one Blueprint should publish. Both empty means "publish nothing", which asks for a load. */
	struct FContainerTagDecision
	{
		bool bWriteScanTag = false;
		FString ContainerTypeName;
	};

	/**
	 * Pure: decides what to publish from what the save could see. Split out from the delegate so every refusal
	 * case is testable without saving a package.
	 */
	FContainerTagDecision DecideTags(const FContainerTagInput& Input);

	/** Subscribes to (and releases) the engine's extra-object-tags broadcast. Called from the module lifecycle. */
	void Register();
	void Unregister();
}
