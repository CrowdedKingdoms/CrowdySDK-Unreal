#pragma once

#include "CoreMinimal.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"

class ICrowdyQueryResponse;

/**
 * Singleton factory that constructs the correct ICrowdyQueryResponse subtype
 * for a given EQueryResponseType.
 *
 * All registrations live in FCrowdyResponseFactory.cpp — one line per response
 * type inside RegisterAll().  Nothing else needs to change when adding a new
 * response: no switch statements, no scattered includes.
 *
 * Usage:
 *   TSharedPtr<ICrowdyQueryResponse> Resp =
 *       FCrowdyResponseFactory::Get().Create(EQueryResponseType::Login);
 */
class CROWDYSDK_API FCrowdyResponseFactory
{
public:
	using FFactoryFn = TFunction<TSharedPtr<ICrowdyQueryResponse>()>;

	static FCrowdyResponseFactory& Get();

	/** Returns nullptr and logs a warning if the type is unregistered. */
	TSharedPtr<ICrowdyQueryResponse> Create(EQueryResponseType ResponseType) const;

	bool IsRegistered(EQueryResponseType ResponseType) const;

private:
	FCrowdyResponseFactory();

	void Register(EQueryResponseType ResponseType, FFactoryFn Factory);
	void RegisterAll();

	TMap<EQueryResponseType, FFactoryFn> Factories;
};
