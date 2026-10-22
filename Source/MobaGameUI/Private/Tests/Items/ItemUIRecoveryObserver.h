#pragma once

#if WITH_DEV_AUTOMATION_TESTS
namespace MobaItemUIRecovery
{
	// 仅显式原生 Items fixture；所有订阅与 ticker 随模块退出清理。
	void RegisterObserver();

	void UnregisterObserver();
}
#endif
