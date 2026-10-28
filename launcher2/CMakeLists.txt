# TRG Launcher 2.0: the console style dashboard for one or more recompiled games (trg::launcher2).
# Built on the 1.x library in the folder above, with its standalone SDL2 + OpenGL 3 window.
# A project links trg::launcher2, fills in a trg::hub::ProjectConfig and calls trg::hub::Run. See README.md.

if(NOT TARGET trg::launcher_standalone)
  message(FATAL_ERROR "TRG Launcher 2.0 needs TRG_LAUNCHER_STANDALONE ON.")
endif()

# stb_image: PNG and JPEG decoding for the game art.
include(FetchContent)
FetchContent_Declare(trg_stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG f58f558c120e9b32c217290b80bad1a0729fbb2c
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(trg_stb)

add_library(trg_launcher2 STATIC
  src/main.cpp
  src/achievements.cpp
  src/app.cpp
  src/art.cpp
  src/art_source.cpp
  src/dash.cpp
  src/disc_finder.cpp
  src/games.cpp
  src/game_settings.cpp
  src/home.cpp
  src/hub.cpp
  src/install.cpp
  src/install_screen.cpp
  src/package.cpp
  src/package_finder.cpp
  src/saves.cpp
  src/projects.cpp
  src/projects_screen.cpp
  src/psx.cpp
  src/settings_screen.cpp
  src/sounds.cpp
  src/steam_button.cpp
  src/steam_shortcut.cpp
  src/stfs_package.cpp)
add_library(trg::launcher2 ALIAS trg_launcher2)
target_include_directories(trg_launcher2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${trg_stb_SOURCE_DIR})
target_link_libraries(trg_launcher2 PUBLIC trg::launcher_standalone)
if(MSVC)
  target_compile_options(trg_launcher2 PRIVATE /W4 /utf-8 /permissive-)
  target_compile_definitions(trg_launcher2 PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(trg_launcher2 PRIVATE -Wall -Wextra)
endif()

if(TRG_LAUNCHER2_BUILD_EXAMPLE)
  add_subdirectory(examples/whopping)
endif()
