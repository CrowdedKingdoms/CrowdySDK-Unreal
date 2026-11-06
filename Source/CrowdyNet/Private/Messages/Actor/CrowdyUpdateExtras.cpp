#include "Messages/Actor/CrowdyUpdateExtras.h"

#include "CrowdyNetLog.h"
#include "HAL/IConsoleManager.h"

#include <atomic>

namespace
{
	int32 GCrowdyReceiveUpdateExtras = 1;
	FAutoConsoleVariableRef CVarCrowdyReceiveUpdateExtras(
		TEXT("crowdy.net.receive.updateextras"),
		GCrowdyReceiveUpdateExtras,
		TEXT("Read the entries an actor update carries after its state block (default 1). ")
		TEXT("0 stops at the state block, as SDK versions before 2.25 do."),
		ECVF_Default);

	std::atomic<int64> GCrowdyMalformedUpdateExtras{0};
}

bool CrowdyUpdateExtras::IsReceiveEnabled()
{
	return GCrowdyReceiveUpdateExtras != 0;
}

void CrowdyUpdateExtras::ReportMalformed(const int32 Octets)
{
	const int64 Count = GCrowdyMalformedUpdateExtras.fetch_add(1, std::memory_order_relaxed) + 1;
	if (Count != 1 && Count % 1000 != 0)
	{
		return;
	}
	UE_LOG(LogCrowdyNet, Warning,
		TEXT("An actor update carried %d octets after its state that do not read as entries; they were dropped and the state applied (%lld so far)."),
		Octets, Count);
}

int64 CrowdyUpdateExtras::GetMalformedCount()
{
	return GCrowdyMalformedUpdateExtras.load(std::memory_order_relaxed);
}
