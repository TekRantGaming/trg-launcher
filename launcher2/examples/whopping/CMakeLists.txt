# The Whopping Recompilation Collection's launcher, built from launcher2/examples/whopping/project.cpp.
# A project of its own does the same in its CMakeLists.txt:
#
#   set(TRG_LAUNCHER_STANDALONE ON CACHE BOOL "" FORCE)
#   set(TRG_LAUNCHER_2 ON CACHE BOOL "" FORCE)
#   add_subdirectory(third_party/trg-launcher)
#   add_executable(my_launcher WIN32 src/project.cpp src/launcher.rc)
#   target_link_libraries(my_launcher PRIVATE trg::launcher2)
#
# plus the version definitions and MSVC options below.

set(WHOPPING_EXAMPLE_VERSION 1.0.6)
string(REPLACE "." "," _commas "${WHOPPING_EXAMPLE_VERSION}")

add_executable(whopping_launcher WIN32 project.cpp launcher.rc)
target_link_libraries(whopping_launcher PRIVATE trg::launcher2)
# PROJECT_VERSION is shown in the launcher and compared with releases by its self updater; the .rc file uses both.
target_compile_definitions(whopping_launcher PRIVATE PROJECT_VERSION="${WHOPPING_EXAMPLE_VERSION}"
  PROJECT_VERSION_COMMAS=${_commas},0)
if(MSVC)
  target_compile_options(whopping_launcher PRIVATE /W4 /utf-8 /permissive-)
  # A WIN32 program (no console window) that starts at main().
  target_link_options(whopping_launcher PRIVATE /ENTRY:mainCRTStartup)
endif()

