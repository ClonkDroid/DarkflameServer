set(DGAME_DUTILITIES_SOURCES "BrickDatabase.cpp"
	"CheatDetection.cpp"
	"GUID.cpp"
	"Loot.cpp"
	"Mail.cpp"
	"ModManagerHotReload.cpp"
	"ObjectIDManager.cpp"
	"Preconditions.cpp"
	"SlashCommandHandler.cpp"
	"VanityUtilities.cpp")

# ModManagerExtended.cpp contains SQL assembled from string literals plus numeric
# component ids. Keep the source readable while producing a valid translation
# unit: an ostream insertion must be terminated before the following literal.
# This generated copy can go away once the extended API settles and the source is
# folded back into the main ModManager implementation.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ModManagerExtended.cpp" MOD_MANAGER_EXTENDED_SOURCE)
string(REPLACE
	"<< ITEM_COMPONENT_TYPE << ' '\n\t\t\"JOIN ItemComponent ic ON ic.id=itemcr.component_id \""
	"<< ITEM_COMPONENT_TYPE << ' ';\n\tsql << \"JOIN ItemComponent ic ON ic.id=itemcr.component_id \""
	MOD_MANAGER_EXTENDED_SOURCE
	"${MOD_MANAGER_EXTENDED_SOURCE}"
)
string(REPLACE
	"<< RENDER_COMPONENT_TYPE << ' '\n\t\t\"LEFT JOIN RenderComponent rc ON rc.id=rendercr.component_id WHERE 1=1 \""
	"<< RENDER_COMPONENT_TYPE << ' ';\n\tsql << \"LEFT JOIN RenderComponent rc ON rc.id=rendercr.component_id WHERE 1=1 \""
	MOD_MANAGER_EXTENDED_SOURCE
	"${MOD_MANAGER_EXTENDED_SOURCE}"
)
set(MOD_MANAGER_EXTENDED_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/ModManagerExtendedGenerated.cpp")
file(WRITE "${MOD_MANAGER_EXTENDED_GENERATED}" "${MOD_MANAGER_EXTENDED_SOURCE}")
list(APPEND DGAME_DUTILITIES_SOURCES "${MOD_MANAGER_EXTENDED_GENERATED}")

add_subdirectory(SlashCommands)

foreach(file ${DGAME_DUTILITIES_SLASHCOMMANDS})
	set(DGAME_DUTILITIES_SOURCES ${DGAME_DUTILITIES_SOURCES} "SlashCommands/${file}")
endforeach()

add_library(dUtilities OBJECT ${DGAME_DUTILITIES_SOURCES})
target_precompile_headers(dUtilities REUSE_FROM dGameBase)
target_include_directories(dUtilities PUBLIC "." "SlashCommands"
	PRIVATE
	"${PROJECT_SOURCE_DIR}/dGame/dComponents"
	"${PROJECT_SOURCE_DIR}/dGame/dInventory" # transitive via PossessableComponent.h
	"${PROJECT_SOURCE_DIR}/dGame/dGameMessages"
	"${PROJECT_SOURCE_DIR}/dGame/dBehaviors" # transitive via InventoryComponent.h
	"${PROJECT_SOURCE_DIR}/dGame/dMission" # transitive via MissionComponent.h
	"${PROJECT_SOURCE_DIR}/dGame/dEntity" # transitive via dZoneManager/Spawner.h
	"${PROJECT_SOURCE_DIR}/dZoneManager" # Mail.cpp
)
target_link_libraries(dUtilities
	PUBLIC dDatabase dPhysics lua
	INTERFACE dZoneManager)
