# The Lua mod API can own interactive QueueChoiceBox callbacks. Keep the
# upstream Entity.cpp source untouched while the mod framework is developed and
# inject a single guarded interception before the normal script callback.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Entity.cpp" ENTITY_SOURCE)
string(REPLACE
	"#include \"PlayerManager.h\""
	"#include \"PlayerManager.h\"\n#include \"ModManager.h\""
	ENTITY_SOURCE
	"${ENTITY_SOURCE}"
)
string(REPLACE
	"void Entity::OnChoiceBoxResponse(Entity* sender, int32_t button, const std::u16string& buttonIdentifier, const std::u16string& identifier) {\n\tGetScript()->OnChoiceBoxResponse(this, sender, button, buttonIdentifier, identifier);\n}"
	"void Entity::OnChoiceBoxResponse(Entity* sender, int32_t button, const std::u16string& buttonIdentifier, const std::u16string& identifier) {\n\tif (ModManager::Instance().HandleChoiceBoxResponse(this, sender, button, buttonIdentifier, identifier)) return;\n\tGetScript()->OnChoiceBoxResponse(this, sender, button, buttonIdentifier, identifier);\n}"
	ENTITY_SOURCE
	"${ENTITY_SOURCE}"
)
set(MODDED_ENTITY_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/EntityWithMods.cpp")
file(WRITE "${MODDED_ENTITY_SOURCE}" "${ENTITY_SOURCE}")

set(DGAME_SOURCES "Character.cpp"
		"${MODDED_ENTITY_SOURCE}"
		"EntityManager.cpp"
		"LeaderboardManager.cpp"
		"PlayerManager.cpp"
		"TeamManager.cpp"
		"TradingManager.cpp"
		"User.cpp"
		"UserManager.cpp")

include_directories(
	${PROJECT_SOURCE_DIR}/dScripts
	${PROJECT_SOURCE_DIR}/dGame
)

add_library(dGameBase OBJECT ${DGAME_SOURCES})
target_precompile_headers(dGameBase PRIVATE ${HEADERS_DGAME})
target_include_directories(dGameBase PUBLIC "." "dEntity"
	PRIVATE "dComponents" "dGameMessages" "dBehaviors" "dMission" "dUtilities" "dInventory"
	$<TARGET_PROPERTY:dPropertyBehaviors,INTERFACE_INCLUDE_DIRECTORIES>
	"${PROJECT_SOURCE_DIR}/dCommon"
	"${PROJECT_SOURCE_DIR}/dCommon/dEnums"
	"${PROJECT_SOURCE_DIR}/dCommon/dClient"
	# dDatabase
	"${PROJECT_SOURCE_DIR}/dDatabase/CDClientDatabase"
	"${PROJECT_SOURCE_DIR}/dDatabase/CDClientDatabase/CDClientTables"
	"${PROJECT_SOURCE_DIR}/dDatabase/GameDatabase"
	"${PROJECT_SOURCE_DIR}/dDatabase/GameDatabase/ITables"
	# dPhysics
	"${PROJECT_SOURCE_DIR}/thirdparty/recastnavigation/Recast/Include"
	"${PROJECT_SOURCE_DIR}/thirdparty/recastnavigation/Detour/Include"
	"${PROJECT_SOURCE_DIR}/dZoneManager"
)

add_subdirectory(dBehaviors)
add_subdirectory(dComponents)
add_subdirectory(dEntity)
add_subdirectory(dGameMessages)
add_subdirectory(dInventory)
add_subdirectory(dMission)
add_subdirectory(dPropertyBehaviors)
add_subdirectory(dUtilities)

add_library(dGame STATIC
	$<TARGET_OBJECTS:dGameBase>
	$<TARGET_OBJECTS:dBehaviors>
	$<TARGET_OBJECTS:dComponents>
	$<TARGET_OBJECTS:dEntity>
	$<TARGET_OBJECTS:dGameMessages>
	$<TARGET_OBJECTS:dInventory>
	$<TARGET_OBJECTS:dMission>
	$<TARGET_OBJECTS:dPropertyBehaviors>
	$<TARGET_OBJECTS:dUtilities>
)
# dGame contains the dUtilities object files, including the Lua-backed mod
# manager. Static libraries do not link their object-library dependencies into
# themselves, so every consumer of dGame must also receive the Lua archive.
target_link_libraries(dGame INTERFACE dNet lua)
target_include_directories(dGame INTERFACE
	$<TARGET_PROPERTY:dGameBase,INTERFACE_INCLUDE_DIRECTORIES>
	$<TARGET_PROPERTY:dBehaviors,INTERFACE_INCLUDE_DIRECTORIES>
	$<TARGET_PROPERTY:dComponents,INTERFACE_INCLUDE_DIRECTORIES>
	$<TARGET_PROPERTY:dEntity,INTERFACE_INCLUDE_DIRECTORIES>
	$<TARGET_PROPERTY:dGameMessages,INTERFACE_INCLUDE_DIRECTORIES>
	$<TARGET_PROPERTY:dInventory,INTERFACE_INCLUDE_DIRECTORIES>
	$<TARGET_PROPERTY:dMission,INTERFACE_INCLUDE_DIRECTORIES>
	$<TARGET_PROPERTY:dPropertyBehaviors,INTERFACE_INCLUDE_DIRECTORIES>
	$<TARGET_PROPERTY:dUtilities,INTERFACE_INCLUDE_DIRECTORIES>
)
