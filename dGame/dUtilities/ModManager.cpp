#include "ModManager.h"

#include "BinaryPathFinder.h"
#include "DEVGMCommands.h"
#include "Entity.h"
#include "GameMessages.h"
#include "GeneralUtils.h"
#include "Logger.h"
#include "SlashCommandHandler.h"
#include "eGameMasterLevel.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace {
	constexpr int32_t MOD_API_VERSION = 1;
	constexpr const char* RUNTIME_REGISTRY_KEY = "DLU_MOD_RUNTIME";

	std::string GetRequiredString(lua_State* state, int tableIndex, const char* field) {
		lua_getfield(state, tableIndex, field);
		if (!lua_isstring(state, -1)) {
			luaL_error(state, "field '%s' must be a string", field);
		}
		std::string value = lua_tostring(state, -1);
		lua_pop(state, 1);
		return value;
	}

	std::string GetOptionalString(lua_State* state, int tableIndex, const char* field, const std::string& fallback = "") {
		lua_getfield(state, tableIndex, field);
		std::string value = fallback;
		if (lua_isstring(state, -1)) value = lua_tostring(state, -1);
		lua_pop(state, 1);
		return value;
	}

	int32_t GetOptionalInteger(lua_State* state, int tableIndex, const char* field, int32_t fallback) {
		lua_getfield(state, tableIndex, field);
		int32_t value = fallback;
		if (lua_isinteger(state, -1)) value = static_cast<int32_t>(lua_tointeger(state, -1));
		lua_pop(state, 1);
		return value;
	}

	void DisableGlobal(lua_State* state, const char* name) {
		lua_pushnil(state);
		lua_setglobal(state, name);
	}
}

struct ModManager::ModRuntime {
	struct RegisteredCommand {
		std::string primaryAlias;
		int functionRef = LUA_NOREF;
	};

	lua_State* state = nullptr;
	std::filesystem::path path;
	std::string id;
	std::string name;
	std::string version;
	int32_t apiVersion = 0;
	bool manifestDeclared = false;
	std::vector<RegisteredCommand> commands;

	~ModRuntime() {
		if (state) lua_close(state);
	}
};

ModManager& ModManager::Instance() {
	static ModManager instance;
	return instance;
}

void ModManager::Startup() {
	const char* configuredDirectory = std::getenv("DLU_MODS_DIR");
	m_ModsDirectory = configuredDirectory && *configuredDirectory
		? std::filesystem::path(configuredDirectory)
		: BinaryPathFinder::GetBinaryDir() / "mods";

	std::error_code error;
	std::filesystem::create_directories(m_ModsDirectory, error);
	if (error) {
		LOG("[Mods] Unable to create mod directory %s: %s", m_ModsDirectory.string().c_str(), error.message().c_str());
		return;
	}

	RegisterManagementCommands();
	ReloadAll();
}

void ModManager::Shutdown() {
	UnloadMods();
	UnregisterManagementCommands();
}

void ModManager::ReloadAll() {
	UnloadMods();

	std::vector<std::filesystem::path> candidates;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(m_ModsDirectory, error)) {
		if (error) break;
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() == ".dlumod") candidates.push_back(entry.path());
	}

	std::ranges::sort(candidates);
	LOG("[Mods] Found %zu drop-in mod file(s) in %s", candidates.size(), m_ModsDirectory.string().c_str());

	for (const auto& candidate : candidates) LoadMod(candidate);

	LOG("[Mods] Loaded %zu mod(s)", m_Mods.size());
}

size_t ModManager::GetLoadedModCount() const {
	return m_Mods.size();
}

std::string ModManager::GetLoadedModsSummary() const {
	if (m_Mods.empty()) return "No mods loaded.";

	std::ostringstream summary;
	summary << "Loaded mods (" << m_Mods.size() << "):";
	for (const auto& mod : m_Mods) {
		summary << "\n- " << mod->name << " " << mod->version << " [" << mod->id << "]";
	}
	return summary.str();
}

bool ModManager::LoadMod(const std::filesystem::path& path) {
	auto runtime = std::make_unique<ModRuntime>();
	runtime->path = path;
	runtime->state = luaL_newstate();
	if (!runtime->state) {
		LOG("[Mods] Failed to create Lua state for %s", path.string().c_str());
		return false;
	}

	luaL_openlibs(runtime->state);

	// The first API version is intentionally conservative. Mods get the normal Lua
	// language/runtime libraries, but not direct filesystem/process/module access.
	DisableGlobal(runtime->state, "io");
	DisableGlobal(runtime->state, "os");
	DisableGlobal(runtime->state, "package");
	DisableGlobal(runtime->state, "require");
	DisableGlobal(runtime->state, "dofile");
	DisableGlobal(runtime->state, "loadfile");
	DisableGlobal(runtime->state, "debug");

	lua_pushlightuserdata(runtime->state, runtime.get());
	lua_setfield(runtime->state, LUA_REGISTRYINDEX, RUNTIME_REGISTRY_KEY);

	lua_newtable(runtime->state);
	lua_pushinteger(runtime->state, MOD_API_VERSION);
	lua_setfield(runtime->state, -2, "api_version");

	const luaL_Reg apiFunctions[] = {
		{ "mod", ApiDeclareMod },
		{ "command", ApiRegisterCommand },
		{ "feedback", ApiFeedback },
		{ "set_level", ApiSetLevel },
		{ "give_item", ApiGiveItem },
		{ "spawn", ApiSpawn },
		{ "spawn_group", ApiSpawnGroup },
		{ "test_map", ApiTestMap },
		{ "refill", ApiRefill },
		{ "set_coins", ApiSetCoins },
		{ "lookup", ApiLookup },
		{ "position", ApiPosition },
		{ nullptr, nullptr }
	};
	luaL_setfuncs(runtime->state, apiFunctions, 0);
	lua_setglobal(runtime->state, "dlu");

	if (luaL_loadfile(runtime->state, path.string().c_str()) != LUA_OK) {
		LOG("[Mods] Failed loading %s: %s", path.filename().string().c_str(), lua_tostring(runtime->state, -1));
		return false;
	}

	if (lua_pcall(runtime->state, 0, 0, 0) != LUA_OK) {
		LOG("[Mods] Failed executing %s: %s", path.filename().string().c_str(), lua_tostring(runtime->state, -1));
		for (const auto& command : runtime->commands) SlashCommandHandler::UnregisterCommand(command.primaryAlias);
		return false;
	}

	if (!runtime->manifestDeclared) {
		LOG("[Mods] %s did not call dlu.mod{...}; rejecting mod", path.filename().string().c_str());
		for (const auto& command : runtime->commands) SlashCommandHandler::UnregisterCommand(command.primaryAlias);
		return false;
	}

	LOG("[Mods] Loaded %s %s (%s) with %zu command(s)", runtime->name.c_str(), runtime->version.c_str(), runtime->id.c_str(), runtime->commands.size());
	m_Mods.push_back(std::move(runtime));
	return true;
}

void ModManager::UnloadMods() {
	for (auto& mod : m_Mods) {
		for (const auto& command : mod->commands) SlashCommandHandler::UnregisterCommand(command.primaryAlias);
	}
	m_Mods.clear();
}

void ModManager::RegisterManagementCommands() {
	if (m_ManagementCommandsRegistered) return;

	Command listCommand{
		.help = "List loaded DLU mods",
		.info = "Shows all currently loaded .dlumod files.",
		.aliases = { "mods", "modlist" },
		.handle = [](Entity* entity, const SystemAddress&, const std::string&) {
			GameMessages::SendSlashCommandFeedbackText(entity, GeneralUtils::UTF8ToUTF16(ModManager::Instance().GetLoadedModsSummary()));
		},
		.requiredLevel = eGameMasterLevel::DEVELOPER
	};

	Command reloadCommand{
		.help = "Reload all DLU mods",
		.info = "Unloads and reloads all .dlumod files from the configured mod directory.",
		.aliases = { "modreload", "reloadmods" },
		.handle = [](Entity* entity, const SystemAddress&, const std::string&) {
			ModManager::Instance().ReloadAll();
			GameMessages::SendSlashCommandFeedbackText(entity, GeneralUtils::UTF8ToUTF16(ModManager::Instance().GetLoadedModsSummary()));
		},
		.requiredLevel = eGameMasterLevel::DEVELOPER
	};

	const bool listRegistered = SlashCommandHandler::RegisterCommand(std::move(listCommand));
	const bool reloadRegistered = SlashCommandHandler::RegisterCommand(std::move(reloadCommand));
	m_ManagementCommandsRegistered = listRegistered && reloadRegistered;
}

void ModManager::UnregisterManagementCommands() {
	if (!m_ManagementCommandsRegistered) return;
	SlashCommandHandler::UnregisterCommand("mods");
	SlashCommandHandler::UnregisterCommand("modreload");
	m_ManagementCommandsRegistered = false;
}

void ModManager::InvokeCommand(ModRuntime* runtime, int functionRef, Entity* entity, const SystemAddress& sysAddr, const std::string& args) {
	if (!runtime || !runtime->state || functionRef == LUA_NOREF) return;

	m_CurrentEntity = entity;
	m_CurrentSysAddr = &sysAddr;

	lua_rawgeti(runtime->state, LUA_REGISTRYINDEX, functionRef);
	lua_pushlstring(runtime->state, args.c_str(), args.size());

	if (lua_pcall(runtime->state, 1, 0, 0) != LUA_OK) {
		const char* error = lua_tostring(runtime->state, -1);
		LOG("[Mods] Command error in %s: %s", runtime->id.c_str(), error ? error : "unknown Lua error");
		GameMessages::SendSlashCommandFeedbackText(entity, u"The mod command failed. Check the WorldServer log for details.");
		lua_pop(runtime->state, 1);
	}

	m_CurrentEntity = nullptr;
	m_CurrentSysAddr = nullptr;
}

ModManager::ModRuntime* ModManager::GetRuntime(lua_State* state) {
	lua_getfield(state, LUA_REGISTRYINDEX, RUNTIME_REGISTRY_KEY);
	auto* runtime = static_cast<ModRuntime*>(lua_touserdata(state, -1));
	lua_pop(state, 1);
	return runtime;
}

int ModManager::ApiDeclareMod(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	auto* runtime = GetRuntime(state);
	if (!runtime) return luaL_error(state, "missing DLU mod runtime");
	if (runtime->manifestDeclared) return luaL_error(state, "dlu.mod may only be called once");

	runtime->id = GetRequiredString(state, 1, "id");
	runtime->name = GetRequiredString(state, 1, "name");
	runtime->version = GetRequiredString(state, 1, "version");
	runtime->apiVersion = GetOptionalInteger(state, 1, "api", MOD_API_VERSION);

	if (runtime->id.empty()) return luaL_error(state, "mod id may not be empty");
	if (runtime->apiVersion != MOD_API_VERSION) {
		return luaL_error(state, "mod requests API %d but server provides API %d", runtime->apiVersion, MOD_API_VERSION);
	}

	runtime->manifestDeclared = true;
	return 0;
}

int ModManager::ApiRegisterCommand(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	auto* runtime = GetRuntime(state);
	if (!runtime || !runtime->manifestDeclared) return luaL_error(state, "call dlu.mod{...} before registering commands");

	const std::string name = GetRequiredString(state, 1, "name");
	const std::string help = GetOptionalString(state, 1, "help", "Mod command");
	const std::string info = GetOptionalString(state, 1, "info", help);
	const int32_t gmLevel = GetOptionalInteger(state, 1, "gm_level", static_cast<int32_t>(eGameMasterLevel::DEVELOPER));

	std::vector<std::string> aliases { name };
	lua_getfield(state, 1, "aliases");
	if (lua_istable(state, -1)) {
		const lua_Integer length = luaL_len(state, -1);
		for (lua_Integer i = 1; i <= length; ++i) {
			lua_geti(state, -1, i);
			if (!lua_isstring(state, -1)) {
				lua_pop(state, 2);
				return luaL_error(state, "command aliases must be strings");
			}
			std::string alias = lua_tostring(state, -1);
			if (std::ranges::find(aliases, alias) == aliases.end()) aliases.push_back(std::move(alias));
			lua_pop(state, 1);
		}
	}
	lua_pop(state, 1);

	lua_getfield(state, 1, "run");
	if (!lua_isfunction(state, -1)) {
		lua_pop(state, 1);
		return luaL_error(state, "command field 'run' must be a function");
	}
	const int functionRef = luaL_ref(state, LUA_REGISTRYINDEX);

	Command command{
		.help = help,
		.info = info,
		.aliases = aliases,
		.handle = [runtime, functionRef](Entity* entity, const SystemAddress& sysAddr, const std::string& args) {
			ModManager::Instance().InvokeCommand(runtime, functionRef, entity, sysAddr, args);
		},
		.requiredLevel = static_cast<eGameMasterLevel>(gmLevel)
	};

	if (!SlashCommandHandler::RegisterCommand(std::move(command))) {
		luaL_unref(state, LUA_REGISTRYINDEX, functionRef);
		return luaL_error(state, "command '%s' conflicts with an existing command or alias", name.c_str());
	}

	runtime->commands.push_back({ name, functionRef });
	return 0;
}

int ModManager::ApiFeedback(lua_State* state) {
	const char* text = luaL_checkstring(state, 1);
	auto& manager = Instance();
	if (manager.m_CurrentEntity) {
		GameMessages::SendSlashCommandFeedbackText(manager.m_CurrentEntity, GeneralUtils::UTF8ToUTF16(text));
	}
	return 0;
}

int ModManager::ApiSetLevel(lua_State* state) {
	const auto level = luaL_checkinteger(state, 1);
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) {
		DEVGMCommands::SetLevel(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, std::to_string(level));
	}
	return 0;
}

int ModManager::ApiGiveItem(lua_State* state) {
	const auto lot = luaL_checkinteger(state, 1);
	const auto count = luaL_optinteger(state, 2, 1);
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) {
		DEVGMCommands::GmAddItem(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, std::to_string(lot) + " " + std::to_string(count));
	}
	return 0;
}

int ModManager::ApiSpawn(lua_State* state) {
	const auto lot = luaL_checkinteger(state, 1);
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) {
		DEVGMCommands::Spawn(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, std::to_string(lot));
	}
	return 0;
}

int ModManager::ApiSpawnGroup(lua_State* state) {
	const auto lot = luaL_checkinteger(state, 1);
	const auto count = luaL_checkinteger(state, 2);
	const auto radius = luaL_optnumber(state, 3, 10.0);
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) {
		std::ostringstream args;
		args << lot << ' ' << count << ' ' << radius;
		DEVGMCommands::SpawnGroup(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, args.str());
	}
	return 0;
}

int ModManager::ApiTestMap(lua_State* state) {
	const auto zone = luaL_checkinteger(state, 1);
	const auto clone = luaL_optinteger(state, 2, 0);
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) {
		DEVGMCommands::TestMap(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, std::to_string(zone) + " " + std::to_string(clone));
	}
	return 0;
}

int ModManager::ApiRefill(lua_State*) {
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) {
		DEVGMCommands::RefillStats(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, "");
	}
	return 0;
}

int ModManager::ApiSetCoins(lua_State* state) {
	const auto coins = luaL_checkinteger(state, 1);
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) {
		DEVGMCommands::SetCurrency(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, std::to_string(coins));
	}
	return 0;
}

int ModManager::ApiLookup(lua_State* state) {
	const char* query = luaL_checkstring(state, 1);
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) {
		DEVGMCommands::Lookup(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, query);
	}
	return 0;
}

int ModManager::ApiPosition(lua_State*) {
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) {
		DEVGMCommands::Pos(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, "");
	}
	return 0;
}
