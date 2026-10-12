#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "botwm.h"

namespace botwm
{
	// read a short NUL-terminated string from emulated memory
	static std::string ReadGuestString(MPTR addr, size_t maxLen = 100)
	{
		std::string s;
		if (addr == MPTR_NULL || !memory_isAddressRangeAccessible(addr, (uint32)maxLen))
			return s;
		const char* p = (const char*)memory_getPointerFromVirtualOffset(addr);
		for (size_t i = 0; i < maxLen && p[i] != '\0'; i++)
		{
			if (p[i] < 0x20 || p[i] > 0x7E)
				break;
			s.push_back(p[i]);
		}
		return s;
	}

	// spawnactors.fnCallMain(startData, storageStart, storageEnd)
	// Called every frame by the spawn patch. Setting the "Enabled" byte in startData makes the game call a queued function.
	// Step 1: observe only, never queue anything.
	static void SpawnActors_fnCallMain(PPCInterpreter_t* hCPU)
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			cemuLog_log(LogType::Force, "[BotWM] spawnactors.fnCallMain reached (startData 0x{:08x})", hCPU->gpr[3]);
			s_logged = true;
		}
		osLib_returnFromFunction(hCPU, 0);
	}

	// multiplayer.WeatherSync(weatherMap)
	// r3 points to a 9-entry weather remap table. Step 1: leave it untouched (no weather change).
	static void Multiplayer_WeatherSync(PPCInterpreter_t* hCPU)
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			cemuLog_log(LogType::Force, "[BotWM] multiplayer.WeatherSync reached (map 0x{:08x})", hCPU->gpr[3]);
			s_logged = true;
		}
		osLib_returnFromFunction(hCPU, 0);
	}

	// actor hooks fire constantly, so only the first few are logged in full and the rest are counted
	static constexpr uint32 kActorLogLimit = 200;

	// ukl_actorinterceptor.OnActorCreate: game registers as they were at actor creation, r3 = actor
	static void ActorInterceptor_OnActorCreate(PPCInterpreter_t* hCPU)
	{
		static uint32 s_count = 0;
		MPTR actor = hCPU->gpr[3];
		s_count++;
		if (s_count <= kActorLogLimit)
			cemuLog_log(LogType::Force, "[BotWM] OnActorCreate 0x{:08x} {}", actor, ReadGuestString(actor + 0x10));
		else if (s_count % 1000 == 0)
			cemuLog_log(LogType::Force, "[BotWM] OnActorCreate called {} times", s_count);
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	static void ActorInterceptor_OnActorDeleteLater(PPCInterpreter_t* hCPU)
	{
		static uint32 s_count = 0;
		MPTR actor = hCPU->gpr[3];
		s_count++;
		if (s_count <= kActorLogLimit)
			cemuLog_log(LogType::Force, "[BotWM] OnActorDeleteLater 0x{:08x} {}", actor, ReadGuestString(actor + 0x10));
		else if (s_count % 1000 == 0)
			cemuLog_log(LogType::Force, "[BotWM] OnActorDeleteLater called {} times", s_count);
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	static void ActorInterceptor_OnActorErase(PPCInterpreter_t* hCPU)
	{
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	static void RemoteBombAIInterceptor_OnCalc(PPCInterpreter_t* hCPU)
	{
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	static void TimeMgrInterceptor_OnInit(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::Force, "[BotWM] TimeMgr OnInit 0x{:08x}", hCPU->gpr[3]);
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	class BotWMModule : public COSModule
	{
	public:
		using RegisterFn = void(*)();
		BotWMModule(std::string_view name, RegisterFn registerFn) : m_name(name), m_register(registerFn) {}
		std::string_view GetName() override { return m_name; }
		void RPLMapped() override
		{
			cemuLog_log(LogType::Force, "[BotWM] module {} loaded", m_name);
			m_register();
		}
	private:
		std::string_view m_name;
		RegisterFn m_register;
	};

	static BotWMModule s_spawnActors("spawnactors", [] {
		osLib_addFunction("spawnactors", "fnCallMain", SpawnActors_fnCallMain);
	});
	static BotWMModule s_multiplayer("multiplayer", [] {
		osLib_addFunction("multiplayer", "WeatherSync", Multiplayer_WeatherSync);
	});
	static BotWMModule s_actorInterceptor("ukl_actorinterceptor", [] {
		osLib_addFunction("ukl_actorinterceptor", "OnActorCreate", ActorInterceptor_OnActorCreate);
		osLib_addFunction("ukl_actorinterceptor", "OnActorDeleteLater", ActorInterceptor_OnActorDeleteLater);
		osLib_addFunction("ukl_actorinterceptor", "OnActorErase", ActorInterceptor_OnActorErase);
	});
	static BotWMModule s_remoteBombAI("ukl_remotebombaiinterceptor", [] {
		osLib_addFunction("ukl_remotebombaiinterceptor", "OnCalc", RemoteBombAIInterceptor_OnCalc);
	});
	static BotWMModule s_timeMgr("ukl_timemgrinterceptor", [] {
		osLib_addFunction("ukl_timemgrinterceptor", "OnInit", TimeMgrInterceptor_OnInit);
	});

	COSModule* GetModuleSpawnActors() { return &s_spawnActors; }
	COSModule* GetModuleMultiplayer() { return &s_multiplayer; }
	COSModule* GetModuleActorInterceptor() { return &s_actorInterceptor; }
	COSModule* GetModuleRemoteBombAIInterceptor() { return &s_remoteBombAI; }
	COSModule* GetModuleTimeMgrInterceptor() { return &s_timeMgr; }
}
