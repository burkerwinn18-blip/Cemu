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

	// Connection to a BotW Multiplayer dedicated server. address is "host" or "host:port".
	// Returns false if the address can't be parsed. The connection runs in the background and
	// reconnects on its own until Disconnect() is called.
	bool Connect(const std::string& address, const std::string& name, const std::string& password);
	void Disconnect();
	std::string GetStatusText();
	// last values passed to Connect(), saved in botwm.txt in the user data folder
	void GetSavedServer(std::string& address, std::string& name, std::string& password);
}
