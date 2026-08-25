#include "ModManager.h"

#include "Amf3.h"
#include "CDClientDatabase.h"
#include "Character.h"
#include "CharacterComponent.h"
#include "DEVGMCommands.h"
#include "DestroyableComponent.h"
#include "Entity.h"
#include "EntityManager.h"
#include "GMGreaterThanZeroCommands.h"
#include "GameMessages.h"
#include "GeneralUtils.h"
#include "LevelProgressionComponent.h"
#include "Logger.h"
#include "MissionComponent.h"
#include "dServer.h"
#include "eGameMasterLevel.h"
#include "eMissionState.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
	constexpr int32_t DEFAULT_DEBUG_GM_LEVEL = static_cast<int32_t>(eGameMasterLevel::DEVELOPER);
	constexpr int32_t ITEM_COMPONENT_TYPE = 11;
	constexpr int32_t RENDER_COMPONENT_TYPE = 2;

	int32_t ClampLimit(lua_Integer value, int32_t fallback = 48) {
		if (value <= 0) return fallback;
		return static_cast<int32_t>(std::clamp<lua_Integer>(value, 1, 100));
	}

	std::string LuaOptionalString(lua_State* state, int index, const std::string& fallback = "") {
		if (lua_isnoneornil(state, index)) return fallback;
		size_t length = 0;
		const char* value = luaL_checklstring(state, index, &length);
		return std::string(value, length);
	}

	void SetStringField(lua_State* state, const char* key, const std::string& value) {
		lua_pushlstring(state, value.data(), value.size());
		lua_setfield(state, -2, key);
	}

	void SetIntegerField(lua_State* state, const char* key, lua_Integer value) {
		lua_pushinteger(state, value);
		lua_setfield(state, -2, key);
	}

	void SetBooleanField(lua_State* state, const char* key, bool value) {
		lua_pushboolean(state, value);
		lua_setfield(state, -2, key);
	}

	bool MissionIsAlreadyCompleted(eMissionState state) {
		return state == eMissionState::COMPLETE
			|| state == eMissionState::COMPLETE_AVAILABLE
			|| state == eMissionState::COMPLETE_ACTIVE
			|| state == eMissionState::COMPLETE_READY_TO_COMPLETE;
	}
}

void ModManager::RegisterExtendedApi(lua_State* state) {
	// The dlu table must be on top of the stack when this is called.
	const luaL_Reg functions[] = {
		{ "choice_box", ApiChoiceBox },
		{ "zones", ApiZones },
		{ "items", ApiItems },
		{ "currency_items", ApiCurrencyItems },
		{ "missions", ApiMissions },
		{ "max_level", ApiMaxLevel },
		{ "player_info", ApiPlayerInfo },
		{ "set_uscore", ApiSetUScore },
		{ "give_uscore", ApiGiveUScore },
		{ "set_reputation", ApiSetReputation },
		{ "complete_mission", ApiCompleteMission },
		{ "complete_all_missions", ApiCompleteAllMissions },
		{ "reset_mission", ApiResetMission },
		{ "reset_all_missions", ApiResetAllMissions },
		{ "force_save", ApiForceSave },
		{ "freecam", ApiFreecam },
		{ "fly", ApiFly },
		{ "attack_immune", ApiAttackImmune },
		{ "gm_immune", ApiGmImmune },
		{ "set_flag", ApiSetFlag },
		{ "set_inventory_size", ApiSetInventorySize },
		{ nullptr, nullptr }
	};
	luaL_setfuncs(state, functions, 0);
}

void ModManager::ClearChoiceBindings() {
	for (auto& [_, binding] : m_ChoiceBindings) {
		if (binding.state && binding.functionRef != LUA_NOREF && binding.functionRef != LUA_REFNIL) {
			luaL_unref(binding.state, LUA_REGISTRYINDEX, binding.functionRef);
		}
	}
	m_ChoiceBindings.clear();
}

bool ModManager::HandleChoiceBoxResponse(Entity*, Entity* sender, int32_t button,
	const std::u16string& buttonIdentifier, const std::u16string& identifier) {
	const std::string identifierUtf8 = GeneralUtils::UTF16ToWTF8(identifier);
	constexpr std::string_view prefix = "dlu:";
	if (!identifierUtf8.starts_with(prefix)) return false;

	const auto sequence = GeneralUtils::TryParse<uint64_t>(identifierUtf8.substr(prefix.size()));
	if (!sequence) return true;

	const auto iterator = m_ChoiceBindings.find(sequence.value());
	if (iterator == m_ChoiceBindings.end()) return true; // stale/reloaded panel: consume safely

	ChoiceBinding binding = iterator->second;
	m_ChoiceBindings.erase(iterator);
	if (!binding.state || binding.functionRef == LUA_NOREF || binding.functionRef == LUA_REFNIL) return true;

	if (!sender || static_cast<int32_t>(sender->GetGMLevel()) < binding.gmLevel) {
		luaL_unref(binding.state, LUA_REGISTRYINDEX, binding.functionRef);
		return true;
	}

	const auto& sysAddr = sender->GetSystemAddress();
	m_CurrentEntity = sender;
	m_CurrentSysAddr = &sysAddr;

	lua_rawgeti(binding.state, LUA_REGISTRYINDEX, binding.functionRef);
	const std::string selected = GeneralUtils::UTF16ToWTF8(buttonIdentifier);
	lua_pushlstring(binding.state, selected.data(), selected.size());
	lua_pushinteger(binding.state, button);
	if (lua_pcall(binding.state, 2, 0, 0) != LUA_OK) {
		const char* error = lua_tostring(binding.state, -1);
		LOG("[Mods] ChoiceBox callback error: %s", error ? error : "unknown Lua error");
		GameMessages::SendSlashCommandFeedbackText(sender, u"The mod panel action failed. Check the WorldServer log.");
		lua_pop(binding.state, 1);
	}
	luaL_unref(binding.state, LUA_REGISTRYINDEX, binding.functionRef);

	m_CurrentEntity = nullptr;
	m_CurrentSysAddr = nullptr;
	return true;
}

int ModManager::ApiChoiceBox(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	auto& manager = Instance();
	if (!manager.m_CurrentEntity || !manager.m_CurrentSysAddr) {
		return luaL_error(state, "dlu.choice_box may only be used while handling a player action");
	}

	const int tableIndex = lua_absindex(state, 1);
	lua_getfield(state, tableIndex, "title");
	const std::string title = lua_isstring(state, -1) ? lua_tostring(state, -1) : "DLU Mod";
	lua_pop(state, 1);

	lua_getfield(state, tableIndex, "gm_level");
	const int32_t gmLevel = lua_isinteger(state, -1)
		? static_cast<int32_t>(lua_tointeger(state, -1))
		: DEFAULT_DEBUG_GM_LEVEL;
	lua_pop(state, 1);

	lua_getfield(state, tableIndex, "on_select");
	if (!lua_isfunction(state, -1)) {
		lua_pop(state, 1);
		return luaL_error(state, "choice_box field 'on_select' must be a function");
	}
	const int callbackRef = luaL_ref(state, LUA_REGISTRYINDEX);

	const uint64_t sequence = ++manager.m_ChoiceSequence;
	const std::string identifier = "dlu:" + std::to_string(sequence);

	AMFArrayValue args;
	args.Insert("callbackClient", std::to_string(manager.m_CurrentEntity->GetObjectID()));
	args.Insert("strIdentifier", identifier);
	args.Insert("title", title);
	AMFArrayValue* options = args.InsertArray("options");

	lua_getfield(state, tableIndex, "options");
	if (!lua_istable(state, -1)) {
		lua_pop(state, 1);
		luaL_unref(state, LUA_REGISTRYINDEX, callbackRef);
		return luaL_error(state, "choice_box field 'options' must be an array");
	}

	const lua_Integer count = luaL_len(state, -1);
	if (count < 1 || count > 24) {
		lua_pop(state, 1);
		luaL_unref(state, LUA_REGISTRYINDEX, callbackRef);
		return luaL_error(state, "choice_box requires between 1 and 24 options");
	}

	for (lua_Integer index = 1; index <= count; ++index) {
		lua_geti(state, -1, index);
		if (!lua_istable(state, -1)) {
			lua_pop(state, 2);
			luaL_unref(state, LUA_REGISTRYINDEX, callbackRef);
			return luaL_error(state, "choice_box options must be tables");
		}

		auto* option = options->PushArray();
		lua_getfield(state, -1, "id");
		if (!lua_isstring(state, -1)) {
			lua_pop(state, 3);
			luaL_unref(state, LUA_REGISTRYINDEX, callbackRef);
			return luaL_error(state, "choice_box option id must be a string");
		}
		option->Insert("identifier", std::string(lua_tostring(state, -1)));
		lua_pop(state, 1);

		lua_getfield(state, -1, "caption");
		option->Insert("caption", lua_isstring(state, -1) ? std::string(lua_tostring(state, -1)) : std::string("Option"));
		lua_pop(state, 1);

		lua_getfield(state, -1, "tooltip");
		option->Insert("tooltipText", lua_isstring(state, -1) ? std::string(lua_tostring(state, -1)) : std::string(""));
		lua_pop(state, 1);

		lua_getfield(state, -1, "image");
		if (lua_isstring(state, -1) && *lua_tostring(state, -1)) option->Insert("image", std::string(lua_tostring(state, -1)));
		lua_pop(state, 1);
		lua_pop(state, 1);
	}
	lua_pop(state, 1);

	manager.m_ChoiceBindings.insert_or_assign(sequence, ChoiceBinding{ state, callbackRef, gmLevel, identifier });
	GameMessages::SendUIMessageServerToSingleClient(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, "QueueChoiceBox", args);
	return 0;
}

int ModManager::ApiZones(lua_State* state) {
	auto result = CDClientDatabase::ExecuteQuery(
		"SELECT zoneID, zoneName, thumbnail, DisplayDescription FROM ZoneTable WHERE zoneID > 0 ORDER BY zoneID");
	lua_newtable(state);
	lua_Integer outputIndex = 1;
	while (!result.eof()) {
		lua_newtable(state);
		SetIntegerField(state, "id", result.getIntField("zoneID", 0));
		SetStringField(state, "name", result.getStringField("zoneName", ""));
		SetStringField(state, "thumbnail", result.getStringField("thumbnail", ""));
		SetStringField(state, "description", result.getStringField("DisplayDescription", ""));
		lua_seti(state, -2, outputIndex++);
		result.nextRow();
	}
	result.finalize();
	return 1;
}

int ModManager::ApiItems(lua_State* state) {
	const int32_t itemType = static_cast<int32_t>(luaL_optinteger(state, 1, -1));
	const int32_t offset = std::max<int32_t>(0, static_cast<int32_t>(luaL_optinteger(state, 2, 0)));
	const int32_t limit = ClampLimit(luaL_optinteger(state, 3, 48));
	const std::string search = LuaOptionalString(state, 4);

	std::ostringstream sql;
	sql << "SELECT o.id, COALESCE(NULLIF(o.displayName,''),o.name) AS displayName, o.name, o.type, "
		"ic.itemType, ic.rarity, COALESCE(rc.icon_asset,'') AS icon "
		"FROM Objects o "
		"JOIN ComponentsRegistry itemcr ON itemcr.id=o.id AND itemcr.component_type=" << ITEM_COMPONENT_TYPE << ' '
		"JOIN ItemComponent ic ON ic.id=itemcr.component_id "
		"LEFT JOIN ComponentsRegistry rendercr ON rendercr.id=o.id AND rendercr.component_type=" << RENDER_COMPONENT_TYPE << ' '
		"LEFT JOIN RenderComponent rc ON rc.id=rendercr.component_id WHERE 1=1 ";
	if (itemType >= 0) sql << "AND ic.itemType=" << itemType << ' ';
	if (!search.empty()) sql << "AND (o.displayName LIKE ?1 OR o.name LIKE ?1 OR o.description LIKE ?1) ";
	sql << "ORDER BY displayName COLLATE NOCASE, o.id LIMIT " << limit << " OFFSET " << offset;

	auto query = CDClientDatabase::CreatePreppedStmt(sql.str());
	if (!search.empty()) query.bind(1, ("%" + search + "%").c_str());
	auto result = query.execQuery();

	lua_newtable(state);
	lua_Integer outputIndex = 1;
	while (!result.eof()) {
		lua_newtable(state);
		SetIntegerField(state, "lot", result.getIntField("id", 0));
		SetStringField(state, "name", result.getStringField("displayName", ""));
		SetStringField(state, "internal_name", result.getStringField("name", ""));
		SetStringField(state, "object_type", result.getStringField("type", ""));
		SetIntegerField(state, "item_type", result.getIntField("itemType", -1));
		SetIntegerField(state, "rarity", result.getIntField("rarity", 0));
		SetStringField(state, "image", result.getStringField("icon", ""));
		lua_seti(state, -2, outputIndex++);
		result.nextRow();
	}
	result.finalize();
	return 1;
}

int ModManager::ApiCurrencyItems(lua_State* state) {
	auto result = CDClientDatabase::ExecuteQuery(
		"SELECT DISTINCT c.lot, COALESCE(NULLIF(o.displayName,''),o.name) AS displayName, "
		"COALESCE(rc.icon_asset,'') AS icon FROM ("
		"SELECT currencyLOT AS lot FROM ItemComponent WHERE currencyLOT > 0 "
		"UNION SELECT commendationLOT AS lot FROM ItemComponent WHERE commendationLOT > 0"
		") c JOIN Objects o ON o.id=c.lot "
		"LEFT JOIN ComponentsRegistry rendercr ON rendercr.id=o.id AND rendercr.component_type=2 "
		"LEFT JOIN RenderComponent rc ON rc.id=rendercr.component_id "
		"ORDER BY displayName COLLATE NOCASE, c.lot");
	lua_newtable(state);
	lua_Integer outputIndex = 1;
	while (!result.eof()) {
		lua_newtable(state);
		SetIntegerField(state, "lot", result.getIntField("lot", 0));
		SetStringField(state, "name", result.getStringField("displayName", ""));
		SetStringField(state, "image", result.getStringField("icon", ""));
		lua_seti(state, -2, outputIndex++);
		result.nextRow();
	}
	result.finalize();
	return 1;
}

int ModManager::ApiMissions(lua_State* state) {
	auto& manager = Instance();
	if (!manager.m_CurrentEntity) return luaL_error(state, "missions requires a player context");
	const int32_t offset = std::max<int32_t>(0, static_cast<int32_t>(luaL_optinteger(state, 1, 0)));
	const int32_t limit = ClampLimit(luaL_optinteger(state, 2, 48));
	const bool missionsOnly = lua_isnoneornil(state, 3) ? true : lua_toboolean(state, 3);

	std::ostringstream sql;
	sql << "SELECT id, defined_type, defined_subtype, isMission FROM Missions ";
	if (missionsOnly) sql << "WHERE isMission=1 ";
	sql << "ORDER BY id LIMIT " << limit << " OFFSET " << offset;
	auto result = CDClientDatabase::ExecuteQuery(sql.str());
	auto* component = manager.m_CurrentEntity->GetComponent<MissionComponent>();

	lua_newtable(state);
	lua_Integer outputIndex = 1;
	while (!result.eof()) {
		const auto id = static_cast<uint32_t>(result.getIntField("id", 0));
		lua_newtable(state);
		SetIntegerField(state, "id", id);
		SetStringField(state, "type", result.getStringField("defined_type", ""));
		SetStringField(state, "subtype", result.getStringField("defined_subtype", ""));
		SetBooleanField(state, "is_mission", result.getIntField("isMission", 0) == 1);
		SetIntegerField(state, "state", component ? static_cast<int32_t>(component->GetMissionState(id)) : -1);
		lua_seti(state, -2, outputIndex++);
		result.nextRow();
	}
	result.finalize();
	return 1;
}

int ModManager::ApiMaxLevel(lua_State* state) {
	auto result = CDClientDatabase::ExecuteQuery("SELECT MAX(id) AS maxLevel FROM LevelProgressionLookup");
	lua_pushinteger(state, result.eof() ? 1 : result.getIntField("maxLevel", 1));
	result.finalize();
	return 1;
}

int ModManager::ApiPlayerInfo(lua_State* state) {
	auto& manager = Instance();
	auto* entity = manager.m_CurrentEntity;
	if (!entity) return luaL_error(state, "player_info requires a player context");

	lua_newtable(state);
	SetIntegerField(state, "gm_level", static_cast<int32_t>(entity->GetGMLevel()));
	SetIntegerField(state, "zone", Game::server ? Game::server->GetZoneID() : 0);
	if (const auto* character = entity->GetCharacter()) SetIntegerField(state, "coins", character->GetCoins());
	if (auto* characterComponent = entity->GetComponent<CharacterComponent>()) {
		SetIntegerField(state, "uscore", characterComponent->GetUScore());
		SetIntegerField(state, "reputation", characterComponent->GetReputation());
	}
	if (auto* level = entity->GetComponent<LevelProgressionComponent>()) SetIntegerField(state, "level", level->GetLevel());
	if (auto* stats = entity->GetComponent<DestroyableComponent>()) {
		SetIntegerField(state, "health", static_cast<lua_Integer>(stats->GetHealth()));
		SetIntegerField(state, "max_health", static_cast<lua_Integer>(stats->GetMaxHealth()));
		SetIntegerField(state, "armor", static_cast<lua_Integer>(stats->GetArmor()));
		SetIntegerField(state, "max_armor", static_cast<lua_Integer>(stats->GetMaxArmor()));
		SetIntegerField(state, "imagination", static_cast<lua_Integer>(stats->GetImagination()));
		SetIntegerField(state, "max_imagination", static_cast<lua_Integer>(stats->GetMaxImagination()));
	}
	const auto& position = entity->GetPosition();
	lua_pushnumber(state, position.x); lua_setfield(state, -2, "x");
	lua_pushnumber(state, position.y); lua_setfield(state, -2, "y");
	lua_pushnumber(state, position.z); lua_setfield(state, -2, "z");
	return 1;
}

int ModManager::ApiSetUScore(lua_State* state) {
	auto& manager = Instance();
	const auto value = luaL_checkinteger(state, 1);
	if (manager.m_CurrentEntity) {
		if (auto* character = manager.m_CurrentEntity->GetComponent<CharacterComponent>()) character->SetUScore(value);
		Game::entityManager->SerializeEntity(manager.m_CurrentEntity);
	}
	return 0;
}

int ModManager::ApiGiveUScore(lua_State* state) {
	const auto value = luaL_checkinteger(state, 1);
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) DEVGMCommands::GiveUScore(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, std::to_string(value));
	return 0;
}

int ModManager::ApiSetReputation(lua_State* state) {
	const auto value = luaL_checkinteger(state, 1);
	auto& manager = Instance();
	if (manager.m_CurrentEntity) {
		if (auto* character = manager.m_CurrentEntity->GetComponent<CharacterComponent>()) character->SetReputation(value);
		Game::entityManager->SerializeEntity(manager.m_CurrentEntity);
	}
	return 0;
}

int ModManager::ApiCompleteMission(lua_State* state) {
	const auto missionId = static_cast<uint32_t>(luaL_checkinteger(state, 1));
	auto& manager = Instance();
	if (manager.m_CurrentEntity) {
		if (auto* missions = manager.m_CurrentEntity->GetComponent<MissionComponent>()) {
			if (!missions->HasMission(missionId)) missions->AcceptMission(missionId, true);
			missions->CompleteMission(missionId, true);
		}
	}
	return 0;
}

int ModManager::ApiCompleteAllMissions(lua_State* state) {
	auto& manager = Instance();
	if (!manager.m_CurrentEntity) { lua_pushinteger(state, 0); return 1; }
	auto* missions = manager.m_CurrentEntity->GetComponent<MissionComponent>();
	if (!missions) { lua_pushinteger(state, 0); return 1; }

	auto result = CDClientDatabase::ExecuteQuery("SELECT id FROM Missions WHERE isMission=1 ORDER BY id");
	int32_t completed = 0;
	while (!result.eof()) {
		const auto missionId = static_cast<uint32_t>(result.getIntField("id", 0));
		if (missionId > 0 && !MissionIsAlreadyCompleted(missions->GetMissionState(missionId))) {
			if (!missions->HasMission(missionId)) missions->AcceptMission(missionId, true);
			missions->CompleteMission(missionId, true);
			++completed;
		}
		result.nextRow();
	}
	result.finalize();
	Game::entityManager->SerializeEntity(manager.m_CurrentEntity);
	lua_pushinteger(state, completed);
	return 1;
}

int ModManager::ApiResetMission(lua_State* state) {
	const auto missionId = static_cast<int32_t>(luaL_checkinteger(state, 1));
	auto& manager = Instance();
	if (manager.m_CurrentEntity) if (auto* missions = manager.m_CurrentEntity->GetComponent<MissionComponent>()) missions->ResetMission(missionId);
	return 0;
}

int ModManager::ApiResetAllMissions(lua_State* state) {
	auto& manager = Instance();
	if (!manager.m_CurrentEntity) { lua_pushinteger(state, 0); return 1; }
	auto* missions = manager.m_CurrentEntity->GetComponent<MissionComponent>();
	if (!missions) { lua_pushinteger(state, 0); return 1; }

	std::vector<uint32_t> ids;
	ids.reserve(missions->GetMissions().size());
	for (const auto& [id, _] : missions->GetMissions()) ids.push_back(id);
	for (const auto id : ids) missions->ResetMission(static_cast<int32_t>(id));
	Game::entityManager->SerializeEntity(manager.m_CurrentEntity);
	lua_pushinteger(state, static_cast<lua_Integer>(ids.size()));
	return 1;
}

int ModManager::ApiForceSave(lua_State*) {
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) DEVGMCommands::ForceSave(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, "");
	return 0;
}

int ModManager::ApiFreecam(lua_State*) {
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) DEVGMCommands::Freecam(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, "");
	return 0;
}

int ModManager::ApiFly(lua_State* state) {
	auto& manager = Instance();
	const std::string speed = lua_isnoneornil(state, 1) ? "" : std::to_string(luaL_checknumber(state, 1));
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) GMGreaterThanZeroCommands::Fly(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, speed);
	return 0;
}

int ModManager::ApiAttackImmune(lua_State* state) {
	auto& manager = Instance();
	const bool enabled = lua_toboolean(state, 1);
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) GMGreaterThanZeroCommands::AttackImmune(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, enabled ? "1" : "0");
	return 0;
}

int ModManager::ApiGmImmune(lua_State* state) {
	auto& manager = Instance();
	const bool enabled = lua_toboolean(state, 1);
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) GMGreaterThanZeroCommands::GmImmune(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, enabled ? "1" : "0");
	return 0;
}

int ModManager::ApiSetFlag(lua_State* state) {
	const auto flag = luaL_checkinteger(state, 1);
	const bool enabled = lua_isnoneornil(state, 2) ? true : lua_toboolean(state, 2);
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) DEVGMCommands::SetFlag(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, std::string(enabled ? "on " : "off ") + std::to_string(flag));
	return 0;
}

int ModManager::ApiSetInventorySize(lua_State* state) {
	const auto size = luaL_checkinteger(state, 1);
	const std::string inventory = LuaOptionalString(state, 2);
	auto& manager = Instance();
	if (manager.m_CurrentEntity && manager.m_CurrentSysAddr) {
		std::string args = std::to_string(size);
		if (!inventory.empty()) args += " " + inventory;
		DEVGMCommands::SetInventorySize(manager.m_CurrentEntity, *manager.m_CurrentSysAddr, args);
	}
	return 0;
}
