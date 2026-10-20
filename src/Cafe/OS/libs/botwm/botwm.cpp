#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/MMU/MMU.h"
#include "config/ActiveSettings.h"
#include "Common/socket.h"
#include "util/helpers/helpers.h"
#include "botwm.h"

#if BOOST_OS_UNIX
#include <netinet/tcp.h>
#endif

#include <fstream>
#include <functional>
#include <zlib.h>

namespace botwm
{
	/* ---------------------------------------------------------------------------------------------
	 * Guest memory helpers
	 * ------------------------------------------------------------------------------------------- */

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

	static bool IsAccessible(MPTR addr, uint32 size)
	{
		return addr != MPTR_NULL && memory_isAddressRangeAccessible(addr, size);
	}

	struct Vec3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};

	struct Quat
	{
		float v[4]{};
	};

	// the parts of a player the server relays that we use so far
	struct PlayerPose
	{
		Vec3 position;
		Quat rotation[4];
		uint32 animation = 0; // hash of the animation Link is playing
	};

	// An actor's Havok motion state, found through the same pointer chain the original mod used.
	// Layout (big-endian floats): rotation matrix columns at +0x00/+0x10/+0x20, translation at +0x30,
	// centre of mass at +0x40 and +0x50, rotation quaternions at +0x60 and +0x70.
	static MPTR ResolveMotionState(MPTR actor)
	{
		static const uint32 kChain[] = { 0x3A0, 0x50, 0x4, 0x80, 0x0, 0x5C, 0x18 };
		MPTR p = actor;
		for (uint32 offset : kChain)
		{
			if (!IsAccessible(p + offset, 4))
				return MPTR_NULL;
			p = memory_readU32(p + offset);
		}
		if (!IsAccessible(p, 0x80))
			return MPTR_NULL;
		return p;
	}

	static Vec3 ReadVec3(MPTR addr)
	{
		return { memory_readFloat(addr), memory_readFloat(addr + 4), memory_readFloat(addr + 8) };
	}

	static void WriteVec3(MPTR addr, const Vec3& v)
	{
		memory_writeFloat(addr, v.x);
		memory_writeFloat(addr + 4, v.y);
		memory_writeFloat(addr + 8, v.z);
	}

	static Quat ReadQuat(MPTR addr)
	{
		Quat q;
		for (int i = 0; i < 4; i++)
			q.v[i] = memory_readFloat(addr + i * 4);
		return q;
	}

	static void WriteQuat(MPTR addr, const Quat& q)
	{
		for (int i = 0; i < 4; i++)
			memory_writeFloat(addr + i * 4, q.v[i]);
	}

	// Name of the animation (AS) an actor is playing, found with a test build that searched the
	// pointers reachable from Link: actor+0x4 -> +0x388 -> +0xD4 points at e.g. "Wait", "Jump", "Fall".
	static std::string ReadAnimationName(MPTR actor)
	{
		static const uint32 kChain[] = { 0x4, 0x388, 0xD4 };
		MPTR p = actor;
		for (uint32 offset : kChain)
		{
			if (!IsAccessible(p + offset, 4))
				return {};
			p = memory_readU32(p + offset);
		}
		return ReadGuestString(p, 64);
	}

	// Name of the animation file Link's main animation slot is playing ("Nml_Move_Run_Jump_FootR"),
	// found with a test build: actor+0x4 -> +0x388 -> +0x0 -> +0x118 -> +0xB4, then a name pointer at
	// +0x1CC (further blend slots follow every 0x14 bytes). Empty if it doesn't look like a name.
	static std::string ReadAnimationFile(MPTR actor)
	{
		static const uint32 kChain[] = { 0x4, 0x388, 0x0, 0x118, 0xB4, 0x1CC };
		MPTR p = actor;
		for (uint32 offset : kChain)
		{
			if (p == MPTR_NULL || !IsAccessible(p + offset, 4))
				return {};
			p = memory_readU32(p + offset);
		}
		std::string name = ReadGuestString(p, 64);
		if (name.size() < 3 || name.size() >= 63)
			return {};
		for (char c : name)
			if (!isalnum((unsigned char)c) && c != '_')
				return {};
		return name;
	}

	// The puppets' AS list is keyed on animation file names ("Anim_<crc32 of "Nml_Move_Run">"), while
	// Link only exposes his state ("Move"). Pick the file that fits the state, using his speed
	// (metres per second) to tell walking, running and sprinting apart. nullptr keeps the last one.
	static const char* PickAnimationFile(std::string_view state, float speed)
	{
		auto has = [&](const char* part) { return state.find(part) != std::string_view::npos; };
		if (state == "Fall")
			return "Fall";
		if (state == "Jump")
			return speed < 2.0f ? "Nml_Wait_Jump" : "Nml_Move_Run_Jump_FootL";
		if (state == "Land")
			return speed < 2.0f ? "Nml_Wait_Land" : "Nml_Move_Run_Land_FootL";
		if (has("Swim"))
			return speed < 0.3f ? "Swim_Wait" : speed < 2.5f ? "Swim_Move_Slow_F" : "Swim_Move_Fast_F";
		if (has("Climb"))
			return speed < 0.2f ? "Climb_Wall_Wait_FootL" : "Climb_Wall_Move_U";
		if (has("Squat") || has("Sneak"))
			return speed < 0.3f ? "Squat_Wait_FootL" : "Squat_Move_Walk";
		if (has("Float") || has("Glide") || has("Para"))
			return "Float_Float";
		if (has("Board"))
			return "Shield_Board_Go2";
		if (has("Horse") || has("Ride"))
			return "Horse_Wait";
		if (has("Attention"))
			return speed < 0.3f ? "Nml_Lockon_Wait" : speed < 3.0f ? "Nml_Lockon_Walk_F" : "Nml_Lockon_Run_F";
		if (has("Move"))
			return speed < 0.3f ? "Nml_Wait" : speed < 3.0f ? "Nml_Move_Walk" : speed < 6.5f ? "Nml_Move_Run" : "Nml_Move_Dash";
		if (state == "Brake")
			return "Nml_Move_Run_Brake_FootL";
		if (has("Wait") || state == "Turn")
			return "Nml_Wait";
		return nullptr;
	}

	static uint32 AnimationHash(const char* file)
	{
		return (uint32)crc32(0, (const Bytef*)file, (uInt)strlen(file));
	}

	static bool ReadPose(MPTR actor, PlayerPose& pose)
	{
		MPTR ms = ResolveMotionState(actor);
		if (ms == MPTR_NULL)
			return false;
		pose.position = ReadVec3(ms + 0x50);
		pose.rotation[0] = ReadQuat(ms + 0x20);
		pose.rotation[1] = ReadQuat(ms + 0x10);
		pose.rotation[2] = ReadQuat(ms + 0x00);
		pose.rotation[3] = ReadQuat(ms + 0x60);
		return true;
	}

	static bool WritePose(MPTR actor, const PlayerPose& pose)
	{
		MPTR ms = ResolveMotionState(actor);
		if (ms == MPTR_NULL)
			return false;
		WriteVec3(ms + 0x50, pose.position);
		WriteVec3(ms + 0x40, pose.position);
		WriteVec3(ms + 0x30, pose.position);
		WriteQuat(ms + 0x20, pose.rotation[0]);
		WriteQuat(ms + 0x10, pose.rotation[1]);
		WriteQuat(ms + 0x00, pose.rotation[2]);
		WriteQuat(ms + 0x60, pose.rotation[3]);
		WriteQuat(ms + 0x70, pose.rotation[3]);
		return true;
	}

	/* ---------------------------------------------------------------------------------------------
	 * State shared between the game thread and the network thread
	 * ------------------------------------------------------------------------------------------- */

	static constexpr int kMaxPlayers = 32;

	struct RemotePlayer
	{
		bool present = false; // in the latest server update
		bool close = false; // close enough that the server sent the full pose
		PlayerPose pose;
		MPTR actor = MPTR_NULL; // our "JugadorN" puppet, once spawned
		std::chrono::steady_clock::time_point nextSpawnTime{}; // don't ask for a puppet before this
		uint32 shownAnimation = 0; // animation last written for the puppet
		std::chrono::steady_clock::time_point appearedTime{}; // when the current puppet was created
	};

	struct SpawnRequest
	{
		std::string name;
		Vec3 position;
	};

	static std::mutex s_stateMutex;
	static MPTR s_linkActor = MPTR_NULL;
	static PlayerPose s_localPose;
	static bool s_localPoseValid = false;
	static RemotePlayer s_remote[kMaxPlayers];
	static int s_localPlayerNumber = -1;
	static std::deque<SpawnRequest> s_spawnQueue;
	static bool s_interceptArmed = false;
	static bool s_templateWanted = false; // asked the patch for a fresh copy of the game's create arguments
	static uint64 s_frame = 0;

	/* ---------------------------------------------------------------------------------------------
	 * Puppet animations
	 *
	 * The mod's MultiplayerEvent flow has every puppet run Demo_PlayASForDemo with the AS name
	 * "JugadorN_animationthing". That name is a string in the loaded event flow, so the original mod
	 * overwrote it in memory with "Anim_<hash>" (hash in decimal), and the Jugador actors' AS list
	 * has an "Anim_<hash>" entry for each of Link's animations. We do the same.
	 * ------------------------------------------------------------------------------------------- */

	struct AnimNameSlot
	{
		MPTR addr;
		size_t capacity; // length of the original string, we never write past it
		std::string written; // what we last wrote there ("" = still the original)
	};
	static std::vector<AnimNameSlot> s_animNames[kMaxPlayers];
	static bool s_animScanWanted = false;

	static std::string OriginalAnimName(int playerNumber)
	{
		return fmt::format("Jugador{}_animationthing", playerNumber + 1);
	}

	// Find every "JugadorN_animationthing" string in the game heap. Runs on the network thread.
	static void ScanAnimNames()
	{
		if (!mmuRange_MEM2.isMapped())
			return;
		const uint8* base = memory_getPointerFromVirtualOffset(mmuRange_MEM2.getBase());
		const uint8* end = base + mmuRange_MEM2.getSize();
		static const uint8 kSuffix[] = "_animationthing"; // includes the terminating NUL in the search
		std::boyer_moore_horspool_searcher searcher(std::begin(kSuffix), std::end(kSuffix));
		std::vector<std::pair<int, MPTR>> found;
		for (const uint8* it = base;;)
		{
			it = std::search(it, end, searcher);
			if (it == end)
				break;
			// walk back over the player number to "Jugador"
			const uint8* digits = it;
			while (digits > base && digits[-1] >= '0' && digits[-1] <= '9' && it - digits < 2)
				digits--;
			if (digits != it && digits - base >= 7 && memcmp(digits - 7, "Jugador", 7) == 0)
			{
				int n = 0;
				for (const uint8* d = digits; d < it; d++)
					n = n * 10 + (*d - '0');
				if (n >= 1 && n <= kMaxPlayers)
					found.emplace_back(n - 1, mmuRange_MEM2.getBase() + (MPTR)(digits - 7 - base));
			}
			it += sizeof(kSuffix);
		}

		std::unique_lock _l(s_stateMutex);
		for (auto& [player, addr] : found)
		{
			auto& slots = s_animNames[player];
			if (std::none_of(slots.begin(), slots.end(), [a = addr](const AnimNameSlot& sl) { return sl.addr == a; }))
				slots.push_back({ addr, OriginalAnimName(player).size(), "" });
		}
		s_animScanWanted = false;
		static size_t s_lastFoundCount = SIZE_MAX;
		if (found.size() != s_lastFoundCount)
			cemuLog_log(LogType::Force, "[BotWM] Found {} puppet animation names", found.size());
		s_lastFoundCount = found.size();
	}

	// Point puppet N's animation name at "Anim_<hash>". Called on the game thread with s_stateMutex held.
	static void SetPuppetAnimation(int playerNumber, uint32 animation)
	{
		std::string name = fmt::format("Anim_{}", animation);
		std::string original = OriginalAnimName(playerNumber);
		auto& slots = s_animNames[playerNumber];
		for (auto it = slots.begin(); it != slots.end();)
		{
			// drop strings that were unloaded or reused since we found them
			std::string current = IsAccessible(it->addr, (uint32)it->capacity + 1) ? ReadGuestString(it->addr, it->capacity + 1) : "";
			bool ours = current == original || (!it->written.empty() && current == it->written);
			if (!ours || name.size() > it->capacity)
			{
				it = slots.erase(it);
				continue;
			}
			uint8* p = memory_getPointerFromVirtualOffset(it->addr);
			memset(p, 0, it->capacity);
			memcpy(p, name.data(), name.size());
			it->written = name;
			++it;
		}
		if (slots.empty())
			s_animScanWanted = true;
	}

	// Put puppet N's animation name back to "JugadorN_animationthing" before a new puppet is created,
	// so every puppet starts the way the first one does
	static void RestorePuppetAnimation(int playerNumber)
	{
		std::string original = OriginalAnimName(playerNumber);
		for (auto& slot : s_animNames[playerNumber])
		{
			if (slot.written.empty() || !IsAccessible(slot.addr, (uint32)slot.capacity + 1))
				continue;
			if (ReadGuestString(slot.addr, slot.capacity + 1) != slot.written)
				continue;
			uint8* p = memory_getPointerFromVirtualOffset(slot.addr);
			memset(p, 0, slot.capacity);
			memcpy(p, original.data(), original.size());
			slot.written.clear();
		}
	}

	static void ResetGameState()
	{
		std::unique_lock _l(s_stateMutex);
		s_linkActor = MPTR_NULL;
		s_localPoseValid = false;
		for (auto& rp : s_remote)
		{
			rp.actor = MPTR_NULL;
			rp.nextSpawnTime = {};
			rp.shownAnimation = 0;
		}
		for (auto& slots : s_animNames)
			slots.clear();
		s_animScanWanted = false;
		s_spawnQueue.clear();
		s_interceptArmed = false;
		s_templateWanted = false;
	}

	/* ---------------------------------------------------------------------------------------------
	 * Actor spawning through the SpawnActors patch
	 *
	 * The patch calls fnCallMain every frame with a small control block (startData, big-endian):
	 *   +0x00 u8  Enabled            - when set, the patch jumps to FunctionToJump with our r3-r10
	 *   +0x01 u8  InterceptRegisters - when set, the patch records the registers of the next natural
	 *                                  call to requestCreateActor into F_R3..F_R10
	 *   +0x04 u32 FunctionToJump
	 *   +0x08 u32 RingPtr            - next free 0x100 byte slot in the patch's storage area
	 *   +0x0C u32 F_R3 .. +0x28 F_R10
	 * ------------------------------------------------------------------------------------------- */

	static constexpr MPTR kRequestCreateActor = 0x037B6040;

	static MPTR ReadInterceptedReg(MPTR startData, int reg)
	{
		return memory_readU32(startData + 0x0C + (reg - 3) * 4);
	}

	// Build a create request in the next storage slot and have the patch call requestCreateActor.
	// The request mirrors what the original mod wrote, starting from the game's own last request.
	static void IssueSpawn(PPCInterpreter_t* hCPU, MPTR startData, const SpawnRequest& req)
	{
		MPTR storageStart = hCPU->gpr[4];
		MPTR storageEnd = hCPU->gpr[5];
		MPTR slot = memory_readU32(startData + 0x08);
		MPTR templateArgs = ReadInterceptedReg(startData, 7);
		if (slot < storageStart || slot + 0x100 > storageEnd || !IsAccessible(templateArgs, 0x68))
			return;

		uint8* slotPtr = memory_getPointerFromVirtualOffset(slot);
		memcpy(slotPtr, memory_getPointerFromVirtualOffset(templateArgs), 0x68);
		memset(slotPtr + 0x68, 0, 0x98);
		memory_writeU32(slot + 0x00, 0x16);
		memory_writeU32(slot + 0x04, 0x043B0000);
		memory_writeU32(slot + 0x08, 0);
		memory_writeU32(slot + 0x18, 0);
		memory_writeU32(slot + 0x1C, 0xFFFFFFFF);
		memory_writeU32(slot + 0x20, 0);
		memory_writeU64(slot + 0x24, 0);
		memory_writeU64(slot + 0x2C, 0xFFFFFFFFFFFFFFFFull);
		memory_writeU32(slot + 0x34, 0);
		memory_writeFloat(slot + 0x38, req.position.x);
		memory_writeFloat(slot + 0x3C, req.position.y);
		memory_writeFloat(slot + 0x40, req.position.z);
		memory_writeU32(slot + 0x44, 0);
		memory_writeU16(slot + 0x48, 0);
		size_t nameLen = std::min<size_t>(req.name.size(), 0x97);
		memcpy(slotPtr + 0x68, req.name.data(), nameLen);

		hCPU->gpr[3] = ReadInterceptedReg(startData, 3);
		hCPU->gpr[4] = slot + 0x68; // actor name
		hCPU->gpr[5] = ReadInterceptedReg(startData, 5);
		hCPU->gpr[6] = slot + 0x1C;
		hCPU->gpr[7] = slot;
		hCPU->gpr[8] = 0;
		hCPU->gpr[9] = 1;
		hCPU->gpr[10] = 0;

		MPTR next = slot + 0x100;
		if (next >= storageEnd)
			next = storageStart;
		memory_writeU32(startData + 0x08, next);
		memory_writeU32(startData + 0x04, kRequestCreateActor);
		memory_writeU8(startData + 0x01, 0);
		memory_writeU8(startData + 0x00, 1);
		cemuLog_log(LogType::Force, "[BotWM] Spawning {} at {:.1f} {:.1f} {:.1f}", req.name, req.position.x, req.position.y, req.position.z);
	}

	static int ParsePuppetName(const std::string& name);

	static std::string PuppetName(int playerNumber)
	{
		return fmt::format("Jugador{}", playerNumber + 1);
	}

	// spawnactors.fnCallMain(startData, storageStart, storageEnd), called by the game every frame.
	// All game-side work happens here, so emulated memory is only changed from the game's own thread.
	static void SpawnActors_fnCallMain(PPCInterpreter_t* hCPU)
	{
		MPTR startData = hCPU->gpr[3];
		hCPU->instructionPointer = hCPU->spr.LR;
		if (!IsAccessible(startData, 0x2C))
			return;

		std::unique_lock _l(s_stateMutex);
		s_frame++;
		if (!s_interceptArmed)
		{
			// ask the patch to record one of the game's own requestCreateActor calls; we reuse its arguments
			memory_writeU8(startData + 0x01, 1);
			s_interceptArmed = true;
			cemuLog_log(LogType::Force, "[BotWM] spawnactors.fnCallMain reached (startData 0x{:08x})", startData);
			return;
		}

		// our own Link
		if (s_linkActor != MPTR_NULL)
		{
			uint32 previousAnimation = s_localPose.animation;
			Vec3 previousPosition = s_localPose.position;
			bool hadPose = s_localPoseValid;
			s_localPoseValid = ReadPose(s_linkActor, s_localPose);
			s_localPose.animation = previousAnimation;
			if (s_localPoseValid)
			{
				// Link's speed, measured over at least 0.2 s (the hook can run several times per frame),
				// then sorted into standing / walking / running / sprinting with some slack between each
				// so the puppet doesn't flicker between two animations near a boundary
				static auto s_speedStartTime = std::chrono::steady_clock::now();
				static Vec3 s_speedStartPos;
				static bool s_speedStarted = false;
				static int s_gear = 0;
				auto now = std::chrono::steady_clock::now();
				float elapsed = std::chrono::duration<float>(now - s_speedStartTime).count();
				if (!hadPose || !s_speedStarted || elapsed > 1.0f)
				{
					s_speedStartTime = now;
					s_speedStartPos = s_localPose.position;
					s_speedStarted = true;
				}
				else if (elapsed >= 0.2f)
				{
					float dx = s_localPose.position.x - s_speedStartPos.x;
					float dz = s_localPose.position.z - s_speedStartPos.z;
					float speed = std::sqrt(dx * dx + dz * dz) / elapsed;
					static const float kUp[] = { 0.5f, 2.6f, 5.8f };
					static const float kDown[] = { 0.25f, 1.8f, 4.8f };
					while (s_gear < 3 && speed > kUp[s_gear])
						s_gear++;
					while (s_gear > 0 && speed < kDown[s_gear - 1])
						s_gear--;
					s_speedStartTime = now;
					s_speedStartPos = s_localPose.position;
				}
				static const float kGearSpeed[] = { 0.0f, 1.5f, 4.0f, 7.0f };
				float gearSpeed = kGearSpeed[s_gear];
				// prefer the file Link is really playing; while moving, his slots blend walk/run/sprint,
				// so pick one of those from his speed instead
				std::string state = ReadAnimationName(s_linkActor);
				std::string playing = ReadAnimationFile(s_linkActor);
				const char* file = nullptr;
				bool fromSlot = !playing.empty() && state.find("Move") == std::string::npos;
				if (fromSlot)
					file = playing.c_str();
				else if (!state.empty())
					file = PickAnimationFile(state, gearSpeed);
				if (file)
					s_localPose.animation = AnimationHash(file);
				// log the first animation changes and unknown states so a test log shows what Link does
				static int s_animLogCount = 0;
				static std::vector<std::string> s_unknownStates;
				if (file && s_localPose.animation != previousAnimation && s_animLogCount < 40)
				{
					s_animLogCount++;
					cemuLog_log(LogType::Force, "[BotWM] Link {} -> {}{}", state, file, fromSlot ? " (playing)" : "");
				}
				if (!file && !state.empty() && s_unknownStates.size() < 40 &&
					std::find(s_unknownStates.begin(), s_unknownStates.end(), state) == s_unknownStates.end())
				{
					s_unknownStates.push_back(state);
					cemuLog_log(LogType::Force, "[BotWM] Link state {} has no animation yet", state);
				}
			}
		}

		// move the puppets of nearby players and queue spawns for missing ones
		for (int i = 0; i < kMaxPlayers; i++)
		{
			RemotePlayer& rp = s_remote[i];
			if (i == s_localPlayerNumber || !rp.present || !rp.close)
				continue;
			if (rp.actor != MPTR_NULL)
			{
				WritePose(rp.actor, rp.pose);
				if (rp.pose.animation != 0 && (rp.pose.animation != rp.shownAnimation || s_animNames[i].empty()))
				{
					SetPuppetAnimation(i, rp.pose.animation);
					if (!s_animNames[i].empty())
						rp.shownAnimation = rp.pose.animation;
				}
				continue;
			}
			// give a request 10 seconds to show up before trying again. This is timed in seconds, not
			// calls: the hook can run many times per frame (it did on a Mac), and counting calls made
			// it ask for several puppets at once
			auto now = std::chrono::steady_clock::now();
			bool queued = std::any_of(s_spawnQueue.begin(), s_spawnQueue.end(), [n = PuppetName(i)](const SpawnRequest& r) { return r.name == n; });
			if (now >= rp.nextSpawnTime && s_localPoseValid && !queued)
			{
				rp.nextSpawnTime = now + std::chrono::seconds(10);
				s_spawnQueue.push_back({ PuppetName(i), rp.pose.position });
			}
		}

		// One spawn per frame. The game's create arguments we copy can point at memory the game
		// reuses later (old copies made puppets vanish right away and once crashed the game), so
		// before each spawn have the patch record a fresh set from the game's next own request.
		bool patchIdle = memory_readU8(startData + 0x00) == 0;
		if (!s_spawnQueue.empty() && patchIdle)
		{
			if (!s_templateWanted)
			{
				memory_writeU32(startData + 0x0C + (7 - 3) * 4, MPTR_NULL);
				memory_writeU8(startData + 0x01, 1);
				s_templateWanted = true;
			}
			else if (memory_readU8(startData + 0x01) == 0 && ReadInterceptedReg(startData, 7) != MPTR_NULL)
			{
				SpawnRequest req = s_spawnQueue.front();
				s_spawnQueue.pop_front();
				s_templateWanted = false;
				int player = ParsePuppetName(req.name);
				if (player >= 0)
					RestorePuppetAnimation(player);
				IssueSpawn(hCPU, startData, req);
			}
		}
	}

	// multiplayer.WeatherSync(weatherMap)
	// r3 points to a 9-entry weather remap table. Weather isn't synced yet, so it is left untouched.
	static void Multiplayer_WeatherSync(PPCInterpreter_t* hCPU)
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			cemuLog_log(LogType::Force, "[BotWM] multiplayer.WeatherSync reached (map 0x{:08x})", hCPU->gpr[3]);
			s_logged = true;
		}
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	// "Jugador12" -> player index 11, anything else -> -1
	static int ParsePuppetName(const std::string& name)
	{
		if (name.size() < 8 || name.size() > 9 || name.compare(0, 7, "Jugador") != 0)
			return -1;
		int n = 0;
		for (size_t i = 7; i < name.size(); i++)
		{
			if (name[i] < '0' || name[i] > '9')
				return -1;
			n = n * 10 + (name[i] - '0');
		}
		return (n >= 1 && n <= kMaxPlayers) ? n - 1 : -1;
	}

	// ukl_actorinterceptor.OnActorCreate: game registers as they were at actor creation, r3 = actor
	static void ActorInterceptor_OnActorCreate(PPCInterpreter_t* hCPU)
	{
		MPTR actor = hCPU->gpr[3];
		hCPU->instructionPointer = hCPU->spr.LR;
		std::string name = ReadGuestString(actor + 0x10);
		if (name == "GameROMPlayer")
		{
			std::unique_lock _l(s_stateMutex);
			s_linkActor = actor;
			cemuLog_log(LogType::Force, "[BotWM] Found Link (0x{:08x})", actor);
			return;
		}
		int puppet = ParsePuppetName(name);
		if (puppet >= 0)
		{
			std::unique_lock _l(s_stateMutex);
			s_remote[puppet].actor = actor;
			s_remote[puppet].shownAnimation = 0;
			s_remote[puppet].appearedTime = std::chrono::steady_clock::now();
			cemuLog_log(LogType::Force, "[BotWM] Player {} appeared as {} (0x{:08x})", puppet + 1, name, actor);
		}
	}

	static void ForgetActor(MPTR actor)
	{
		std::unique_lock _l(s_stateMutex);
		if (actor == s_linkActor)
		{
			s_linkActor = MPTR_NULL;
			s_localPoseValid = false;
		}
		for (int i = 0; i < kMaxPlayers; i++)
		{
			if (s_remote[i].actor == actor)
			{
				s_remote[i].actor = MPTR_NULL;
				// wait a moment before spawning a new one, so a puppet the game removes again and
				// again doesn't turn into a constant respawn loop
				s_remote[i].nextSpawnTime = std::chrono::steady_clock::now() + std::chrono::seconds(3);
				// how long it lived and how far from our Link it was, to tell why the game removed it
				float alive = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_remote[i].appearedTime).count();
				float dist = -1.0f;
				if (s_localPoseValid)
				{
					float dx = s_remote[i].pose.position.x - s_localPose.position.x;
					float dy = s_remote[i].pose.position.y - s_localPose.position.y;
					float dz = s_remote[i].pose.position.z - s_localPose.position.z;
					dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				}
				cemuLog_log(LogType::Force, "[BotWM] Player {} puppet removed after {:.1f}s, {:.0f}m from Link", i + 1, alive, dist);
			}
		}
	}

	static void ActorInterceptor_OnActorDeleteLater(PPCInterpreter_t* hCPU)
	{
		MPTR actor = hCPU->gpr[3];
		hCPU->instructionPointer = hCPU->spr.LR;
		ForgetActor(actor);
	}

	static void ActorInterceptor_OnActorErase(PPCInterpreter_t* hCPU)
	{
		MPTR actor = hCPU->gpr[3];
		hCPU->instructionPointer = hCPU->spr.LR;
		ForgetActor(actor);
	}

	static void RemoteBombAIInterceptor_OnCalc(PPCInterpreter_t* hCPU)
	{
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	static void TimeMgrInterceptor_OnInit(PPCInterpreter_t* hCPU)
	{
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	/* ---------------------------------------------------------------------------------------------
	 * Server connection (BotW Multiplayer dedicated server protocol)
	 *
	 * Client messages are 6144 byte packets: a message type byte followed by fields in declaration
	 * order, little-endian (strings and lists are prefixed with a one byte length or count).
	 * The server answers a connect with JSON and an update with a 2 byte length plus binary data.
	 * ------------------------------------------------------------------------------------------- */

	enum class MessageType : uint8
	{
		Error = 0,
		Ping = 1,
		Connect = 2,
		Update = 3,
		Disconnect = 4,
	};

	static constexpr size_t kPacketSize = 6144;

	// the protocol is little-endian, as is every platform Cemu runs on
	class PacketWriter
	{
	public:
		explicit PacketWriter(MessageType type) { m_data.push_back((uint8)type); }

		void U8(uint8 v) { m_data.push_back(v); }
		void Bool(bool v) { U8(v ? 1 : 0); }
		void I16(sint16 v) { Raw(&v, 2); }
		void I32(sint32 v) { Raw(&v, 4); }
		void F32(float v) { Raw(&v, 4); }
		void Str(const std::string& s)
		{
			size_t len = std::min<size_t>(s.size(), 255);
			U8((uint8)len);
			Raw(s.data(), len);
		}
		void V3(const Vec3& v) { F32(v.x); F32(v.y); F32(v.z); }
		void Q(const Quat& q) { for (float f : q.v) F32(f); }

		std::vector<uint8> Finish()
		{
			m_data.resize(kPacketSize, 0);
			return std::move(m_data);
		}

	private:
		void Raw(const void* p, size_t n)
		{
			const uint8* b = (const uint8*)p;
			m_data.insert(m_data.end(), b, b + n);
		}

		std::vector<uint8> m_data;
	};

	class PacketReader
	{
	public:
		PacketReader(const uint8* data, size_t size) : m_data(data), m_size(size) {}

		bool Ok() const { return m_ok; }
		uint8 U8() { uint8 v = 0; Raw(&v, 1); return v; }
		bool Bool() { return U8() != 0; }
		sint32 I32() { sint32 v = 0; Raw(&v, 4); return v; }
		float F32() { float v = 0; Raw(&v, 4); return v; }
		void Str() { Skip(U8()); }
		Vec3 V3() { Vec3 v; v.x = F32(); v.y = F32(); v.z = F32(); return v; }
		Quat Q() { Quat q; for (float& f : q.v) f = F32(); return q; }
		void Skip(size_t n)
		{
			if (m_pos + n > m_size) { m_ok = false; m_pos = m_size; return; }
			m_pos += n;
		}

	private:
		void Raw(void* out, size_t n)
		{
			if (m_pos + n > m_size) { m_ok = false; m_pos = m_size; return; }
			memcpy(out, m_data + m_pos, n);
			m_pos += n;
		}

		const uint8* m_data;
		size_t m_size;
		size_t m_pos = 0;
		bool m_ok = true;
	};

	struct ServerConfig
	{
		std::string host = "127.0.0.1";
		uint16 port = 5050;
		std::string name = "Player";
		std::string password;
	};

	static std::mutex s_netMutex;
	static std::thread s_netThread;
	static std::atomic_bool s_netStop = false;
	static std::mutex s_statusMutex;
	static std::string s_statusText = "Not connected";

	static void SetStatus(const std::string& text)
	{
		{
			std::unique_lock _l(s_statusMutex);
			if (s_statusText == text)
				return;
			s_statusText = text;
		}
		cemuLog_log(LogType::Force, "[BotWM] {}", text);
	}

	std::string GetStatusText()
	{
		std::unique_lock _l(s_statusMutex);
		return s_statusText;
	}

	static fs::path ConfigPath()
	{
		return ActiveSettings::GetUserDataPath("botwm.txt");
	}

	static ServerConfig LoadConfig()
	{
		ServerConfig cfg;
		std::ifstream f(ConfigPath());
		std::string line;
		while (std::getline(f, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			std::string key = line.substr(0, eq);
			std::string value = line.substr(eq + 1);
			if (key == "host")
				cfg.host = value;
			else if (key == "port")
				cfg.port = (uint16)std::strtoul(value.c_str(), nullptr, 10);
			else if (key == "name")
				cfg.name = value;
			else if (key == "password")
				cfg.password = value;
		}
		return cfg;
	}

	static void SaveConfig(const ServerConfig& cfg)
	{
		std::ofstream f(ConfigPath(), std::ios::trunc);
		f << "host=" << cfg.host << "\n";
		f << "port=" << cfg.port << "\n";
		f << "name=" << cfg.name << "\n";
		f << "password=" << cfg.password << "\n";
	}

	void GetSavedServer(std::string& address, std::string& name, std::string& password)
	{
		ServerConfig cfg = LoadConfig();
		address = fmt::format("{}:{}", cfg.host, cfg.port);
		name = cfg.name;
		password = cfg.password;
	}

	static bool SendAll(SOCKET s, const uint8* data, size_t size)
	{
		while (size > 0)
		{
			int r = send(s, (const char*)data, (int)size, 0);
			if (r <= 0)
				return false;
			data += r;
			size -= r;
		}
		return true;
	}

	static bool RecvAll(SOCKET s, uint8* data, size_t size)
	{
		while (size > 0)
		{
			int r = recv(s, (char*)data, (int)size, 0);
			if (r <= 0)
				return false;
			data += r;
			size -= r;
		}
		return true;
	}

	static SOCKET ConnectSocket(const ServerConfig& cfg)
	{
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		addrinfo* result = nullptr;
		std::string port = fmt::format("{}", cfg.port);
		if (getaddrinfo(cfg.host.c_str(), port.c_str(), &hints, &result) != 0 || !result)
			return INVALID_SOCKET;
		SOCKET s = INVALID_SOCKET;
		for (addrinfo* ai = result; ai; ai = ai->ai_next)
		{
			s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (s == INVALID_SOCKET)
				continue;
			if (connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0)
				break;
			closesocket(s);
			s = INVALID_SOCKET;
		}
		freeaddrinfo(result);
		if (s == INVALID_SOCKET)
			return s;
		int noDelay = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
#if BOOST_OS_WINDOWS
		DWORD timeout = 5000;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
		timeval timeout{ 5, 0 };
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
		return s;
	}

	// pull an integer field out of the server's JSON reply
	static std::optional<sint32> JsonInt(const std::string& json, const char* key)
	{
		std::string pattern = fmt::format("\"{}\":", key);
		size_t p = json.find(pattern);
		if (p == std::string::npos)
			return std::nullopt;
		return (sint32)std::strtol(json.c_str() + p + pattern.size(), nullptr, 10);
	}

	// Returns our player number, or -1. The reply is a JSON object without a length prefix.
	static int Handshake(SOCKET s, const ServerConfig& cfg)
	{
		PacketWriter w(MessageType::Connect);
		w.Str(cfg.name);
		w.Str(cfg.password);
		std::vector<uint8> packet = w.Finish();
		if (!SendAll(s, packet.data(), packet.size()))
			return -1;
		std::string json;
		int depth = 0;
		bool started = false;
		char c;
		while (json.size() < 65536)
		{
			if (recv(s, &c, 1, 0) != 1)
				return -1;
			json.push_back(c);
			if (c == '{')
			{
				depth++;
				started = true;
			}
			else if (c == '}')
				depth--;
			if (started && depth == 0)
				break;
		}
		sint32 response = JsonInt(json, "Response").value_or(0);
		if (response != 1)
		{
			if (response == 2)
				SetStatus("The server is full");
			else if (response == 3)
				SetStatus("Wrong server password");
			else
				SetStatus(fmt::format("The server refused the connection (response {})", response));
			return -1;
		}
		return JsonInt(json, "PlayerNumber").value_or(-1);
	}

	static std::vector<uint8> BuildUpdate()
	{
		PlayerPose pose;
		{
			std::unique_lock _l(s_stateMutex);
			pose = s_localPose;
		}
		PacketWriter w(MessageType::Update);
		// WorldDTO: time, day, weather (not synced yet)
		w.F32(0.0f);
		w.I32(0);
		w.I32(0);
		// ClientPlayerDTO
		w.V3(pose.position);
		for (auto& q : pose.rotation)
			w.Q(q);
		w.I32((sint32)pose.animation);
		w.I32(0); // health
		w.F32(0.0f); // attack up
		w.Bool(false); // weapon equipped
		w.U8(0); // equipment: weapon type, then sword, shield, bow, head, upper, lower
		for (int i = 0; i < 6; i++)
			w.I16(0);
		w.U8(0); // location map
		w.U8(0); // location section
		for (int i = 0; i < 4; i++)
			w.V3({}); // bomb positions
		w.U8(0); // EnemyDTO: no enemy updates
		w.U8(0); // QuestsDTO: no completed quests
		return w.Finish();
	}

	static bool ParseServerUpdate(const std::vector<uint8>& data)
	{
		PacketReader r(data.data(), data.size());
		// WorldDTO
		r.F32();
		r.I32();
		r.I32();
		// NamesDTO
		uint8 nameCount = r.U8();
		for (uint8 i = 0; i < nameCount; i++)
		{
			r.U8();
			r.Str();
		}
		bool present[kMaxPlayers]{};
		bool close[kMaxPlayers]{};
		PlayerPose poses[kMaxPlayers];
		// ClosePlayers
		uint8 closeCount = r.U8();
		for (uint8 i = 0; i < closeCount; i++)
		{
			uint8 number = r.U8();
			r.Bool(); // updated
			PlayerPose pose;
			pose.position = r.V3();
			for (auto& q : pose.rotation)
				q = r.Q();
			pose.animation = (uint32)r.I32();
			r.Skip(4 + 4 + 1); // health, attack up, weapon equipped
			r.Skip(1 + 6 * 2); // equipment
			r.Skip(2); // location
			r.Skip(4 * 12); // bombs
			if (number < kMaxPlayers)
			{
				present[number] = true;
				close[number] = true;
				poses[number] = pose;
			}
		}
		// FarPlayers
		uint8 farCount = r.U8();
		for (uint8 i = 0; i < farCount; i++)
		{
			uint8 number = r.U8();
			r.Bool();
			Vec3 position = r.V3();
			r.Skip(2);
			if (number < kMaxPlayers)
			{
				present[number] = true;
				poses[number].position = position;
			}
		}
		if (!r.Ok())
			return false;
		// the rest (enemies, quests, network settings, game modes, teleports) is not used yet

		std::unique_lock _l(s_stateMutex);
		for (int i = 0; i < kMaxPlayers; i++)
		{
			RemotePlayer& rp = s_remote[i];
			if (present[i] && !rp.present)
				cemuLog_log(LogType::Force, "[BotWM] Player {} is in the game", i + 1);
			else if (!present[i] && rp.present)
				cemuLog_log(LogType::Force, "[BotWM] Player {} left", i + 1);
			rp.present = present[i];
			rp.close = close[i];
			if (close[i])
				rp.pose = poses[i];
			else if (present[i])
				rp.pose.position = poses[i].position;
		}
		return true;
	}

	static void WaitBeforeRetry()
	{
		for (int i = 0; i < 50 && !s_netStop; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	static void NetworkThread(ServerConfig cfg)
	{
		SetThreadName("BotWM network");
#if BOOST_OS_WINDOWS
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
		while (!s_netStop)
		{
			SetStatus(fmt::format("Connecting to {}:{} as {}", cfg.host, cfg.port, cfg.name));
			SOCKET s = ConnectSocket(cfg);
			if (s == INVALID_SOCKET)
			{
				SetStatus(fmt::format("Could not reach the server at {}:{}, retrying", cfg.host, cfg.port));
				WaitBeforeRetry();
				continue;
			}
			int playerNumber = Handshake(s, cfg);
			if (playerNumber < 0)
			{
				closesocket(s);
				WaitBeforeRetry();
				continue;
			}
			{
				std::unique_lock _l(s_stateMutex);
				s_localPlayerNumber = playerNumber;
			}
			SetStatus(fmt::format("Connected as player {}", playerNumber + 1));

			bool updateErrorLogged = false;
			auto lastAnimScan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
			while (!s_netStop)
			{
				auto frameStart = std::chrono::steady_clock::now();
				std::vector<uint8> packet = BuildUpdate();
				if (!SendAll(s, packet.data(), packet.size()))
					break;
				uint16 length = 0;
				if (!RecvAll(s, (uint8*)&length, 2))
					break;
				std::vector<uint8> reply(length);
				if (!RecvAll(s, reply.data(), reply.size()))
					break;
				if (!ParseServerUpdate(reply) && !updateErrorLogged)
				{
					cemuLog_log(LogType::Force, "[BotWM] Could not read a server update ({} bytes)", length);
					updateErrorLogged = true;
				}
				// look for the puppets' animation names when a puppet needs them, at most every 3 seconds
				bool scan;
				{
					std::unique_lock _l(s_stateMutex);
					scan = s_animScanWanted;
				}
				if (scan && frameStart - lastAnimScan > std::chrono::seconds(3))
				{
					lastAnimScan = frameStart;
					ScanAnimNames();
				}
				std::this_thread::sleep_until(frameStart + std::chrono::milliseconds(33));
			}

			if (s_netStop)
			{
				PacketWriter w(MessageType::Disconnect);
				std::vector<uint8> bye = w.Finish();
				SendAll(s, bye.data(), bye.size());
			}
			closesocket(s);
			{
				std::unique_lock _l(s_stateMutex);
				s_localPlayerNumber = -1;
				for (auto& rp : s_remote)
					rp.present = false;
			}
			if (!s_netStop)
				SetStatus("Lost the connection to the server, reconnecting");
		}
		SetStatus("Not connected");
	}

	void Disconnect()
	{
		std::unique_lock _l(s_netMutex);
		s_netStop = true;
		if (s_netThread.joinable())
			s_netThread.join();
		s_netStop = false;
	}

	bool Connect(const std::string& address, const std::string& name, const std::string& password)
	{
		ServerConfig cfg;
		cfg.name = name.empty() ? "Player" : name;
		cfg.password = password;
		// accepts "host", "host:port" or "[ipv6]:port"
		std::string host = address;
		if (!address.empty() && address.front() == '[')
		{
			size_t end = address.find(']');
			if (end == std::string::npos)
				return false;
			host = address.substr(1, end - 1);
			if (end + 1 < address.size() && address[end + 1] == ':')
				cfg.port = (uint16)std::strtoul(address.c_str() + end + 2, nullptr, 10);
		}
		else if (size_t colon = address.find(':'); colon != std::string::npos && colon == address.rfind(':'))
		{
			host = address.substr(0, colon);
			cfg.port = (uint16)std::strtoul(address.c_str() + colon + 1, nullptr, 10);
		}
		if (host.empty() || cfg.port == 0)
			return false;
		cfg.host = host;
		SaveConfig(cfg);

		Disconnect();
		std::unique_lock _l(s_netMutex);
		s_netThread = std::thread(NetworkThread, cfg);
		return true;
	}

	/* ---------------------------------------------------------------------------------------------
	 * Module registration
	 * ------------------------------------------------------------------------------------------- */

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
		void RPLUnmapped() override
		{
			// the game was closed; any actors we knew about are gone
			ResetGameState();
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
