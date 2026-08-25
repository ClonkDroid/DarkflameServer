#pragma once

#include "RakNetTypes.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
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

	Entity* m_CurrentEntity = nullptr;
	const SystemAddress* m_CurrentSysAddr = nullptr;
	std::filesystem::path m_ModsDirectory;
	std::vector<std::unique_ptr<ModRuntime>> m_Mods;
	std::unordered_map<std::string, CommandBinding> m_CommandBindings;
	std::unordered_set<std::string> m_RegisteredDispatchers;
	std::string m_LastReloadStatus;
	bool m_ManagementCommandsRegistered = false;
};