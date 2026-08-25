set(DWORLDSERVER_SOURCES
	"PerformanceManager.cpp"
)

add_library(dWorldServer OBJECT ${DWORLDSERVER_SOURCES})
target_link_libraries(dWorldServer PUBLIC dGameBase dCommon)

# Keep the upstream WorldServer source untouched while injecting the mod bootstrap
# immediately after slash-command startup. This makes the framework easy to
# rebase while it is being proven; once the API settles this can become a direct
# source call in WorldServer.cpp.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/WorldServer.cpp" WORLD_SERVER_SOURCE)
string(REPLACE
	"#include \"SlashCommandHandler.h\""
	"#include \"SlashCommandHandler.h\"\n#include \"ModManager.h\""
	WORLD_SERVER_SOURCE
	"${WORLD_SERVER_SOURCE}"
)
string(REPLACE
	"if (zoneID != 0) SlashCommandHandler::Startup();"
	"if (zoneID != 0) { SlashCommandHandler::Startup(); ModManager::Instance().Startup(); }"
	WORLD_SERVER_SOURCE
	"${WORLD_SERVER_SOURCE}"
)
set(MODDED_WORLD_SERVER_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/WorldServerWithMods.cpp")
file(WRITE "${MODDED_WORLD_SERVER_SOURCE}" "${WORLD_SERVER_SOURCE}")

add_executable(WorldServer "${MODDED_WORLD_SERVER_SOURCE}")
target_include_directories(WorldServer PRIVATE "${PROJECT_SOURCE_DIR}/dChatFilter")
add_compile_definitions(WorldServer PRIVATE PROJECT_VERSION="\"${PROJECT_VERSION}\"")

target_include_directories(WorldServer PRIVATE
	"${PROJECT_SOURCE_DIR}/dServer" # BinaryPathFinder.h
)

target_link_libraries(WorldServer ${COMMON_LIBRARIES}
	dScripts
	dGameBase
	dComponents
	dUtilities
	dGameMessages
	dInventory
	dGame dChatFilter dZoneManager dPhysics Detour Recast tinyxml2 dWorldServer dNavigation dServer)

# Ship first-party example mods in the build output. Server operators can replace
# or add .dlumod files without recompiling the server.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/mods")
configure_file(
	"${PROJECT_SOURCE_DIR}/mods/DebugWorld.dlumod"
	"${CMAKE_BINARY_DIR}/mods/DebugWorld.dlumod"
	COPYONLY
)
