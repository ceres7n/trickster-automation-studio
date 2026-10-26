cmake_minimum_required(VERSION 3.20)
project(trickster_automation_studio LANGUAGES CXX)

# The client (Trickster.bin) is a 32-bit process: the hook DLL must be x86, and the
# app injects it with the address of its own LoadLibraryA, so it is x86 too.
#   cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Trickster Automation Studio must be built 32-bit (configure with -A Win32).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL runs inside the client, which ships no VC runtime of its own.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/W3 /utf-8 /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

set(OUT_DIR "${CMAKE_SOURCE_DIR}/bin")
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} "${OUT_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CFG} "${OUT_DIR}")
endforeach()

# ── Dear ImGui 1.91.8 (vendored, Win32 + DX11 backends) ─────────────────────
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/3rdparty/imgui")
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_stdlib.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# ── tas_hook.dll: lives in the client, captures + sends packets ────────
add_library(tas_hook SHARED
    src/hook/dllmain.cpp
    src/hook/hooks.cpp
    src/hook/packet_store.cpp
    src/hook/sender.cpp
    src/hook/ipc_server.cpp
    src/hook/avatar.cpp
)
target_include_directories(tas_hook PRIVATE src)

# ── TricksterAutomationStudio.exe: the ImGui control panel ───────────────────────────────────
add_executable(TricksterAutomationStudio WIN32
    src/app/main.cpp
    src/app/config.cpp
    src/app/text_util.cpp
    src/app/client_proc.cpp
    src/app/session.cpp
    src/app/packet_db.cpp
    src/app/neo_app.cpp
    src/app/neo_app_world.cpp
    src/app/yml_lite.cpp
    src/app/game_data.cpp
    src/app/item_sources.cpp
    src/app/libconfig.cpp
    src/app/quest_graph.cpp
    src/app/world.cpp
    src/app/actions.cpp
    src/app/bot/bot.cpp
    src/app/bot/act_heal.cpp
    src/app/bot/act_buffs.cpp
    src/app/bot/act_loot.cpp
    src/app/bot/act_attack.cpp
    src/app/bot/act_drill.cpp
    src/app/bot/act_quest.cpp
    src/app/bot/act_hunt.cpp
    src/app/bot/act_keyquest.cpp
    src/app/bot/act_story.cpp
    src/app/bot/quest_runner.cpp
    src/app/bot/dialog_walk.cpp
    src/app/bot/act_roam.cpp
    src/app/bot/act_town.cpp
    src/app/bot/navigator.cpp
    src/app/bot/stuck.cpp
    src/app/atlas.cpp
    src/app/neo_app_bot.cpp
    src/app/neo_app_town.cpp
    src/app/neo_app_drill.cpp
    src/app/neo_app_quest.cpp
    src/app/neo_app_hunt.cpp
    src/app/neo_app_keyquest.cpp
    src/app/neo_app_story.cpp
    src/app/notify.cpp
    src/app/ui_theme.cpp
    src/app/gfx.cpp
    res/tas.rc
)
target_include_directories(TricksterAutomationStudio PRIVATE src)
target_link_libraries(TricksterAutomationStudio PRIVATE imgui)
add_dependencies(TricksterAutomationStudio tas_hook)

# data/ + the default ini next to the exe
add_custom_command(TARGET TricksterAutomationStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${OUT_DIR}/data"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/tas.ini.default" "${OUT_DIR}/tas.ini.default"
)

# ── tas_selftest: console check of the link (not built by default) ─────
add_executable(tas_selftest EXCLUDE_FROM_ALL
    tools/selftest.cpp
    src/app/config.cpp
    src/app/text_util.cpp
    src/app/client_proc.cpp
    src/app/session.cpp
    src/app/packet_db.cpp
    src/app/yml_lite.cpp
    src/app/game_data.cpp
    src/app/item_sources.cpp
    src/app/libconfig.cpp
    src/app/quest_graph.cpp
    src/app/world.cpp
    src/app/actions.cpp
    src/app/bot/bot.cpp
    src/app/bot/act_heal.cpp
    src/app/bot/act_buffs.cpp
    src/app/bot/act_loot.cpp
    src/app/bot/act_attack.cpp
    src/app/bot/act_drill.cpp
    src/app/bot/act_quest.cpp
    src/app/bot/act_hunt.cpp
    src/app/bot/act_keyquest.cpp
    src/app/bot/act_story.cpp
    src/app/bot/quest_runner.cpp
    src/app/bot/dialog_walk.cpp
    src/app/bot/act_roam.cpp
    src/app/bot/act_town.cpp
    src/app/bot/navigator.cpp
    src/app/bot/stuck.cpp
    src/app/atlas.cpp
)
target_include_directories(tas_selftest PRIVATE src)
add_dependencies(tas_selftest tas_hook)
