cmake_minimum_required(VERSION 3.16)
project(obs-scene-library VERSION 1.0.0 LANGUAGES C CXX)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(obs-scene-library MODULE
	src/plugin.cpp
	src/snapshot.cpp
	src/library.cpp
	src/dock.cpp
	src/websocket.cpp)
target_link_libraries(obs-scene-library PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_options(obs-scene-library PRIVATE -Wall -Wextra)
# A symbol missing from the module fails the build, not OBS at the moment it's first called.
target_link_options(obs-scene-library PRIVATE -Wl,--no-undefined)
set_target_properties(obs-scene-library PROPERTIES PREFIX "" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
	CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden)
