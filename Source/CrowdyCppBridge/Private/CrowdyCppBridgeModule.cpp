#include "CrowdyCppBridge.h"

#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogCrowdyCpp);

class FCrowdyCppBridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		SelfTestCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("crowdy.cpp.selftest"),
			TEXT("Link + exception-containment self test for the vendored CrowdyCPP library."),
			FConsoleCommandDelegate::CreateStatic(&FCrowdyCppBridgeModule::RunSelfTest),
			ECVF_Default);

		UE_LOG(LogCrowdyCpp, Log,
			TEXT("CrowdyCppBridge loaded; vendored CrowdyCPP present and idle. Run crowdy.cpp.selftest to verify linkage."));
	}

	virtual void ShutdownModule() override
	{
		if (SelfTestCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(SelfTestCommand);
			SelfTestCommand = nullptr;
		}
	}

private:
	static void RunSelfTest()
	{
		const bool bOk = FCrowdyCppBridge::SelfTest();
		UE_LOG(LogCrowdyCpp, Log, TEXT("crowdy.cpp.selftest: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"));
	}

	IConsoleCommand* SelfTestCommand = nullptr;
};

IMPLEMENT_MODULE(FCrowdyCppBridgeModule, CrowdyCppBridge)
