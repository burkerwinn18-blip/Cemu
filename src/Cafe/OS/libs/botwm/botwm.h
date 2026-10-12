#pragma once
#include "Cafe/OS/RPL/COSModule.h"

// Breath of the Wild Multiplayer support.
// The BotW Multiplayer graphic pack patches call into native code through
// OSDynLoad_Acquire/OSDynLoad_FindExport on these module names. On Windows the
// original mod injected a DLL into Cemu to provide them; here Cemu provides them
// itself so the same patches work on every platform.
namespace botwm
{
	COSModule* GetModuleSpawnActors();
	COSModule* GetModuleMultiplayer();
	COSModule* GetModuleActorInterceptor();
	COSModule* GetModuleRemoteBombAIInterceptor();
	COSModule* GetModuleTimeMgrInterceptor();
}
