#pragma once

#include "RakNetTypes.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_State;
class Entity;

class ModManager final {
public:
	static ModManager& Instance();

	void Startup();
	void Shutdown();
	bool ReloadAll();

	// Intercepts only DLU mod-owned ChoiceBox callbacks. Returns true when the
	// response belonged to the mod system (including stale responses after reload).
	bool HandleChoiceBoxResponse(Entity* callbackEntity, Entity* sender, int32_t button,
		const std::u16string& buttonIdentifier, const std::u16string& identifier);

	[[nodiscard]] size_t GetLoadedModCount() const;
	[[nodiscard]] std::string GetLoadedModsSummary() const;
	[[nodiscard]] const std::string& GetLastReloadStatus() const;

private:
	struct ModRuntime;
	struct CommandBinding {
		ModRuntime* runtime = nullptr;
		int functionRef = -2;
		int32_t gmLevel = 0;
	};
	struct ChoiceBinding {
		lua_State* state = nullptr;
		int functionRef = -2;
		int32_t gmLevel = 0;
		std::string identifier;
	};

	ModManager() = default;
	~ModManager() = default;
	ModManager(const ModManager&) = delete;
	ModManager& operator=(const ModManager&) = delete;

	std::unique_ptr<ModRuntime> LoadModCandidate(const std::filesystem::path& path);
	bool ValidateCandidates(const std::vector<std::unique_ptr<ModRuntime>>& candidates, std::string& error) const;
	void ActivateCandidates(std::vector<std::unique_ptr<ModRuntime>> candidates);
	void EnsureDispatcher(const std::string& alias, const std::string& help, const std::string& info);
	void RegisterManagementCommands();
	void DispatchCommand(const std::string& alias, Entity* entity, const SystemAddress& sysAddr, const std::string& args);
	void InvokeCommand(ModRuntime* runtime, int functionRef, Entity* entity, const SystemAddress& sysAddr, const std::string& args);
	void ClearChoiceBindings();

	static void RegisterExtendedApi(lua_State* state);
	static ModRuntime* GetRuntime(lua_State* state);
	static int ApiDeclareMod(lua_State* state);
	static int ApiRegisterCommand(lua_State* state);
	static int ApiFeedback(lua_State* state);
	static int ApiSetLevel(lua_State* state);
	static int ApiGiveItem(lua_State* state);
	static int ApiSpawn(lua_State* state);
	static int ApiSpawnGroup(lua_State* state);
	static int ApiTestMap(lua_State* state);
	static int ApiRefill(lua_State* state);
	static int ApiSetCoins(lua_State* state);
	static int ApiLookup(lua_State* state);
	static int ApiPosition(lua_State* state);

	// Extended API used by graphical/admin mods.
	static int ApiChoiceBox(lua_State* state);
	static int ApiZones(lua_State* state);
	static int ApiItems(lua_State* state);
	static int ApiCurrencyItems(lua_State* state);
	static int ApiMissions(lua_State* state);
	static int ApiMaxLevel(lua_State* state);
	static int ApiPlayerInfo(lua_State* state);
	static int ApiSetUScore(lua_State* state);
	static int ApiGiveUScore(lua_State* state);
	static int ApiSetReputation(lua_State* state);
	static int ApiCompleteMission(lua_State* state);
	static int ApiCompleteAllMissions(lua_State* state);
	static int ApiResetMission(lua_State* state);
	static int ApiResetAllMissions(lua_State* state);
	static int ApiForceSave(lua_State* state);
	static int ApiFreecam(lua_State* state);
	static int ApiFly(lua_State* state);
	static int ApiAttackImmune(lua_State* state);
	static int ApiGmImmune(lua_State* state);
	static int ApiSetFlag(lua_State* state);
	static int ApiSetInventorySize(lua_State* state);

	Entity* m_CurrentEntity = nullptr;
	const SystemAddress* m_CurrentSysAddr = nullptr;
	std::filesystem::path m_ModsDirectory;
	std::vector<std::unique_ptr<ModRuntime>> m_Mods;
	std::unordered_map<std::string, CommandBinding> m_CommandBindings;
	std::unordered_set<std::string> m_RegisteredDispatchers;
	std::unordered_map<uint64_t, ChoiceBinding> m_ChoiceBindings;
	uint64_t m_ChoiceSequence = 0;
	std::string m_LastReloadStatus;
	bool m_ManagementCommandsRegistered = false;
};