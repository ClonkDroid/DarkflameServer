#pragma once

#include "RakNetTypes.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct lua_State;
class Entity;

class ModManager final {
public:
	static ModManager& Instance();

	void Startup();
	void Shutdown();
	void ReloadAll();

	[[nodiscard]] size_t GetLoadedModCount() const;
	[[nodiscard]] std::string GetLoadedModsSummary() const;

private:
	struct ModRuntime;

	ModManager() = default;
	~ModManager() = default;
	ModManager(const ModManager&) = delete;
	ModManager& operator=(const ModManager&) = delete;

	bool LoadMod(const std::filesystem::path& path);
	void UnloadMods();
	void RegisterManagementCommands();
	void UnregisterManagementCommands();
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
	bool m_ManagementCommandsRegistered = false;
};
