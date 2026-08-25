#include "ModManager.h"

#include "BinaryPathFinder.h"
#include "DEVGMCommands.h"
#include "Database.h"
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
		if (!lua_isstring(state, -1)) luaL_error(state, "field '%s' must be a string", field);
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
	struct PendingCommand {
		std::string help;
		std::string info;
		std::vector<std::string> aliases;
		int32_t gmLevel = static_cast<int32_t>(eGameMasterLevel::DEVELOPER);
		int functionRef = LUA_NOREF;
	};

	lua_State* state = nullptr;
	std::filesystem::path path;
	std::string id;
	std::string name;
	std::string version;
	int32_t apiVersion = 0;
	bool manifestDeclared = false;
	std::vector<PendingCommand> commands;

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
		m_LastReloadStatus = "Unable to create mod directory: " + error.message();
		LOG("[Mods] %s", m_LastReloadStatus.c_str());
		return;
	}

	RegisterManagementCommands();
	ReloadAll();
}

void ModManager::Shutdown() {
	ClearChoiceBindings();
	m_CommandBindings.clear();
	m_Mods.clear();
}

bool ModManager::ReloadAll() {
	std::vector<std::filesystem::path> paths;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(m_ModsDirectory, error)) {
		if (error) break;
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() == ".dlumod") paths.push_back(entry.path());
	}
	if (error) {
		m_LastReloadStatus = "Mod reload failed while scanning directory: " + error.message();
		LOG("[Mods] %s", m_LastReloadStatus.c_str());
		return false;
	}

	std::ranges::sort(paths);
	std::vector<std::unique_ptr<ModRuntime>> candidates;
	candidates.reserve(paths.size());
	for (const auto& path : paths) {
		auto candidate = LoadModCandidate(path);
		if (!candidate) {
			m_LastReloadStatus = "Mod reload rejected; existing mods remain active. Failed file: " + path.filename().string();
			LOG("[Mods] %s", m_LastReloadStatus.c_str());
			return false;
		}
		candidates.push_back(std::move(candidate));
	}

	std::string validationError;
	if (!ValidateCandidates(candidates, validationError)) {
		m_LastReloadStatus = "Mod reload rejected; existing mods remain active. " + validationError;
		LOG("[Mods] %s", m_LastReloadStatus.c_str());
		return false;
	}

	ActivateCandidates(std::move(candidates));
	m_LastReloadStatus = "Reloaded " + std::to_string(m_Mods.size()) + " mod(s) successfully.";
	LOG("[Mods] %s", m_LastReloadStatus.c_str());
	return true;
}

size_t ModManager::GetLoadedModCount() const { return m_Mods.size(); }

std::string ModManager::GetLoadedModsSummary() const {
	if (m_Mods.empty()) return "No mods loaded.";
	std::ostringstream summary;
	summary << "Loaded mods (" << m_Mods.size() << "):";
	for (const auto& mod : m_Mods) summary << "\n- " << mod->name << " " << mod->version << " [" << mod->id << "]";
	return summary.str();
}

const std::string& ModManager::GetLastReloadStatus() const { return m_LastReloadStatus; }

std::unique_ptr<ModManager::ModRuntime> ModManager::LoadModCandidate(const std::filesystem::path& path) {
	auto runtime = std::make_unique<ModRuntime>();
	runtime->path = path;
	runtime->state = luaL_newstate();
	if (!runtime->state) return nullptr;
	luaL_openlibs(runtime->state);
	for (const char* global : { "io", "os", "package", "require", "dofile", "loadfile", "debug" }) DisableGlobal(runtime->state, global);

	lua_pushlightuserdata(runtime->state, runtime.get());
	lua_setfield(runtime->state, LUA_REGISTRYINDEX, RUNTIME_REGISTRY_KEY);
	lua_newtable(runtime->state);
	lua_pushinteger(runtime->state, MOD_API_VERSION);
	lua_setfield(runtime->state, -2, "api_version");
	const luaL_Reg apiFunctions[] = {
		{ "mod", ApiDeclareMod }, { "command", ApiRegisterCommand }, { "feedback", ApiFeedback },
		{ "set_level", ApiSetLevel }, { "give_item", ApiGiveItem }, { "spawn", ApiSpawn },
		{ "spawn_group", ApiSpawnGroup }, { "test_map", ApiTestMap }, { "refill", ApiRefill },
		{ "set_coins", ApiSetCoins }, { "lookup", ApiLookup }, { "position", ApiPosition }, { nullptr, nullptr }
	};
	luaL_setfuncs(runtime->state, apiFunctions, 0);
	RegisterExtendedApi(runtime->state);
	lua_setglobal(runtime->state, "dlu");

	if (luaL_loadfile(runtime->state, path.string().c_str()) != LUA_OK) {
		LOG("[Mods] Failed loading %s: %s", path.filename().string().c_str(), lua_tostring(runtime->state, -1));
		return nullptr;
	}
	if (lua_pcall(runtime->state, 0, 0, 0) != LUA_OK) {
		LOG("[Mods] Failed executing %s: %s", path.filename().string().c_str(), lua_tostring(runtime->state, -1));
		return nullptr;
	}
	if (!runtime->manifestDeclared) {
		LOG("[Mods] %s did not call dlu.mod{...}; rejecting mod", path.filename().string().c_str());
		return nullptr;
	}
	return runtime;
}

bool ModManager::ValidateCandidates(const std::vector<std::unique_ptr<ModRuntime>>& candidates, std::string& error) const {
	std::unordered_set<std::string> ids;
	std::unordered_set<std::string> aliases;
	for (const auto& runtime : candidates) {
		if (!ids.insert(runtime->id).second) { error = "Duplicate mod id: " + runtime->id; return false; }
		for (const auto& command : runtime->commands) {
			for (const auto& alias : command.aliases) {
				if (alias.empty()) { error = "Mod " + runtime->id + " declared an empty command alias."; return false; }
				if (!aliases.insert(alias).second) { error = "Duplicate mod command alias: /" + alias; return false; }
			}
		}
	}
	return true;
}

void ModManager::ActivateCandidates(std::vector<std::unique_ptr<ModRuntime>> candidates) {
	for (const auto& runtime : candidates) {
		for (const auto& command : runtime->commands) {
			for (const auto& alias : command.aliases) EnsureDispatcher(alias, command.help, command.info);
		}
	}

	// Choice callbacks contain Lua registry references and must be released
	// before the old Lua states are destroyed by the runtime swap.
	ClearChoiceBindings();
	m_CommandBindings.clear();
	m_Mods = std::move(candidates);
	for (const auto& runtime : m_Mods) {
		for (const auto& command : runtime->commands) {
			for (const auto& alias : command.aliases) {
				m_CommandBindings.insert_or_assign(alias, CommandBinding{ runtime.get(), command.functionRef, command.gmLevel });
			}
		}
	}
}

void ModManager::EnsureDispatcher(const std::string& alias, const std::string& help, const std::string& info) {
	if (!m_RegisteredDispatchers.insert(alias).second) return;
	Command command{
		.help = help,
		.info = info,
		.aliases = { alias },
		.handle = [alias](Entity* entity, const SystemAddress& sysAddr, const std::string& args) {
			ModManager::Instance().DispatchCommand(alias, entity, sysAddr, args);
		},
		.requiredLevel = eGameMasterLevel::CIVILIAN
	};
	SlashCommandHandler::RegisterCommand(std::move(command));
}

void ModManager::RegisterManagementCommands() {
	if (m_ManagementCommandsRegistered) return;
	Command listCommand{
		.help = "List loaded DLU mods", .info = "Shows all currently active .dlumod files.",
		.aliases = { "mods", "modlist" },
		.handle = [](Entity* entity, const SystemAddress&, const std::string&) {
			GameMessages::SendSlashCommandFeedbackText(entity, GeneralUtils::UTF8ToUTF16(ModManager::Instance().GetLoadedModsSummary()));
		}, .requiredLevel = eGameMasterLevel::DEVELOPER
	};
	SlashCommandHandler::RegisterCommand(std::move(listCommand));
	Command reloadCommand{
		.help = "Reload all DLU mods transactionally",
		.info = "Validates every .dlumod in fresh Lua states and activates the new set only when all mods are valid.",
		.aliases = { "modreload", "reloadmods" },
		.handle = [](Entity* entity, const SystemAddress&, const std::string&) {
			auto& manager = ModManager::Instance(); manager.ReloadAll();
			GameMessages::SendSlashCommandFeedbackText(entity, GeneralUtils::UTF8ToUTF16(manager.GetLastReloadStatus() + "\n" + manager.GetLoadedModsSummary()));
		}, .requiredLevel = eGameMasterLevel::DEVELOPER
	};
	SlashCommandHandler::RegisterCommand(std::move(reloadCommand));
	m_ManagementCommandsRegistered = true;
}

void ModManager::DispatchCommand(const std::string& alias, Entity* entity, const SystemAddress& sysAddr, const std::string& args) {
	const auto it = m_CommandBindings.find(alias);
	if (it == m_CommandBindings.end()) {
		GameMessages::SendSlashCommandFeedbackText(entity, u"This mod command is currently inactive.");
		return;
	}
	const auto& binding = it->second;
	if (static_cast<int32_t>(entity->GetGMLevel()) < binding.gmLevel) {
		GameMessages::SendSlashCommandFeedbackText(entity, GeneralUtils::UTF8ToUTF16("You are not high enough GM level to use /" + alias));
		return;
	}
	if (binding.gmLevel > static_cast<int32_t>(eGameMasterLevel::CIVILIAN)) {
		std::string usage = "/" + alias; if (!args.empty()) usage += " " + args;
		Database::Get()->InsertSlashCommandUsage(entity->GetObjectID(), usage);
	}
	InvokeCommand(binding.runtime, binding.functionRef, entity, sysAddr, args);
}

void ModManager::InvokeCommand(ModRuntime* runtime, int functionRef, Entity* entity, const SystemAddress& sysAddr, const std::string& args) {
	if (!runtime || !runtime->state || functionRef == LUA_NOREF) return;
	m_CurrentEntity = entity; m_CurrentSysAddr = &sysAddr;
	lua_rawgeti(runtime->state, LUA_REGISTRYINDEX, functionRef); lua_pushlstring(runtime->state, args.c_str(), args.size());
	if (lua_pcall(runtime->state, 1, 0, 0) != LUA_OK) {
		const char* error = lua_tostring(runtime->state, -1);
		LOG("[Mods] Command error in %s: %s", runtime->id.c_str(), error ? error : "unknown Lua error");
		GameMessages::SendSlashCommandFeedbackText(entity, u"The mod command failed. Check the WorldServer log for details.");
		lua_pop(runtime->state, 1);
	}
	m_CurrentEntity = nullptr; m_CurrentSysAddr = nullptr;
}

ModManager::ModRuntime* ModManager::GetRuntime(lua_State* state) {
	lua_getfield(state, LUA_REGISTRYINDEX, RUNTIME_REGISTRY_KEY);
	auto* runtime = static_cast<ModRuntime*>(lua_touserdata(state, -1)); lua_pop(state, 1); return runtime;
}

int ModManager::ApiDeclareMod(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE); auto* runtime = GetRuntime(state);
	if (!runtime) return luaL_error(state, "missing DLU mod runtime");
	if (runtime->manifestDeclared) return luaL_error(state, "dlu.mod may only be called once");
	runtime->id = GetRequiredString(state, 1, "id"); runtime->name = GetRequiredString(state, 1, "name");
	runtime->version = GetRequiredString(state, 1, "version"); runtime->apiVersion = GetOptionalInteger(state, 1, "api", MOD_API_VERSION);
	if (runtime->id.empty()) return luaL_error(state, "mod id may not be empty");
	if (runtime->apiVersion != MOD_API_VERSION) return luaL_error(state, "mod requests API %d but server provides API %d", runtime->apiVersion, MOD_API_VERSION);
	runtime->manifestDeclared = true; return 0;
}

int ModManager::ApiRegisterCommand(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE); auto* runtime = GetRuntime(state);
	if (!runtime || !runtime->manifestDeclared) return luaL_error(state, "call dlu.mod{...} before registering commands");
	ModRuntime::PendingCommand command; command.aliases.push_back(GetRequiredString(state, 1, "name"));
	command.help = GetOptionalString(state, 1, "help", "Mod command"); command.info = GetOptionalString(state, 1, "info", command.help);
	command.gmLevel = GetOptionalInteger(state, 1, "gm_level", static_cast<int32_t>(eGameMasterLevel::DEVELOPER));
	lua_getfield(state, 1, "aliases");
	if (lua_istable(state, -1)) {
		const lua_Integer length = luaL_len(state, -1);
		for (lua_Integer i = 1; i <= length; ++i) {
			lua_geti(state, -1, i); if (!lua_isstring(state, -1)) { lua_pop(state, 2); return luaL_error(state, "command aliases must be strings"); }
			std::string alias = lua_tostring(state, -1); if (std::ranges::find(command.aliases, alias) == command.aliases.end()) command.aliases.push_back(std::move(alias)); lua_pop(state, 1);
		}
	}
	lua_pop(state, 1); lua_getfield(state, 1, "run");
	if (!lua_isfunction(state, -1)) { lua_pop(state, 1); return luaL_error(state, "command field 'run' must be a function"); }
	command.functionRef = luaL_ref(state, LUA_REGISTRYINDEX); runtime->commands.push_back(std::move(command)); return 0;
}

int ModManager::ApiFeedback(lua_State* state) { const char* text = luaL_checkstring(state, 1); auto& m = Instance(); if (m.m_CurrentEntity) GameMessages::SendSlashCommandFeedbackText(m.m_CurrentEntity, GeneralUtils::UTF8ToUTF16(text)); return 0; }
int ModManager::ApiSetLevel(lua_State* state) { const auto v = luaL_checkinteger(state, 1); auto& m = Instance(); if (m.m_CurrentEntity && m.m_CurrentSysAddr) DEVGMCommands::SetLevel(m.m_CurrentEntity, *m.m_CurrentSysAddr, std::to_string(v)); return 0; }
int ModManager::ApiGiveItem(lua_State* state) { const auto lot = luaL_checkinteger(state, 1); const auto count = luaL_optinteger(state, 2, 1); auto& m = Instance(); if (m.m_CurrentEntity && m.m_CurrentSysAddr) DEVGMCommands::GmAddItem(m.m_CurrentEntity, *m.m_CurrentSysAddr, std::to_string(lot) + " " + std::to_string(count)); return 0; }
int ModManager::ApiSpawn(lua_State* state) { const auto lot = luaL_checkinteger(state, 1); auto& m = Instance(); if (m.m_CurrentEntity && m.m_CurrentSysAddr) DEVGMCommands::Spawn(m.m_CurrentEntity, *m.m_CurrentSysAddr, std::to_string(lot)); return 0; }
int ModManager::ApiSpawnGroup(lua_State* state) { const auto lot = luaL_checkinteger(state, 1); const auto count = luaL_checkinteger(state, 2); const auto radius = luaL_optnumber(state, 3, 10.0); auto& m = Instance(); if (m.m_CurrentEntity && m.m_CurrentSysAddr) { std::ostringstream a; a << lot << ' ' << count << ' ' << radius; DEVGMCommands::SpawnGroup(m.m_CurrentEntity, *m.m_CurrentSysAddr, a.str()); } return 0; }
int ModManager::ApiTestMap(lua_State* state) { const auto zone = luaL_checkinteger(state, 1); const auto clone = luaL_optinteger(state, 2, 0); auto& m = Instance(); if (m.m_CurrentEntity && m.m_CurrentSysAddr) DEVGMCommands::TestMap(m.m_CurrentEntity, *m.m_CurrentSysAddr, std::to_string(zone) + " " + std::to_string(clone)); return 0; }
int ModManager::ApiRefill(lua_State*) { auto& m = Instance(); if (m.m_CurrentEntity && m.m_CurrentSysAddr) DEVGMCommands::RefillStats(m.m_CurrentEntity, *m.m_CurrentSysAddr, ""); return 0; }
int ModManager::ApiSetCoins(lua_State* state) { const auto v = luaL_checkinteger(state, 1); auto& m = Instance(); if (m.m_CurrentEntity && m.m_CurrentSysAddr) DEVGMCommands::SetCurrency(m.m_CurrentEntity, *m.m_CurrentSysAddr, std::to_string(v)); return 0; }
int ModManager::ApiLookup(lua_State* state) { const char* q = luaL_checkstring(state, 1); auto& m = Instance(); if (m.m_CurrentEntity && m.m_CurrentSysAddr) DEVGMCommands::Lookup(m.m_CurrentEntity, *m.m_CurrentSysAddr, q); return 0; }
int ModManager::ApiPosition(lua_State*) { auto& m = Instance(); if (m.m_CurrentEntity && m.m_CurrentSysAddr) DEVGMCommands::Pos(m.m_CurrentEntity, *m.m_CurrentSysAddr, ""); return 0; }
