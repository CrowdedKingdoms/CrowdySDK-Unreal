// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"

// What the Project page's app rail is narrowed by. OrgId 0 means every org the account can see; an empty
// Status means every status; Search matches the name, slug, or id, case-insensitively.
struct FCrowdyAppListFilter
{
	int64 OrgId = 0;
	FString Status;
	FString Search;
};

inline bool CrowdyAppMatchesFilter(const FStudioApp& App, const FCrowdyAppListFilter& Filter)
{
	if (Filter.OrgId != 0 && App.OrgId != Filter.OrgId)
	{
		return false;
	}
	if (!Filter.Status.IsEmpty() && App.Status != Filter.Status)
	{
		return false;
	}
	if (Filter.Search.IsEmpty())
	{
		return true;
	}
	return App.Name.Contains(Filter.Search) || App.Slug.Contains(Filter.Search)
		|| FString::Printf(TEXT("%lld"), App.AppId).Contains(Filter.Search);
}

inline void CrowdyFilterApps(const TArray<TSharedPtr<FStudioApp>>& Apps, const FCrowdyAppListFilter& Filter,
	TArray<TSharedPtr<FStudioApp>>& OutVisible)
{
	OutVisible.Reset();
	for (const TSharedPtr<FStudioApp>& App : Apps)
	{
		if (App.IsValid() && CrowdyAppMatchesFilter(*App, Filter))
		{
			OutVisible.Add(App);
		}
	}
}
