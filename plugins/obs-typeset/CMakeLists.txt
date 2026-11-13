cmake_minimum_required(VERSION 3.16)
project(obs-typeset VERSION 0.1.0 LANGUAGES C CXX)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo>=1.50)

add_library(obs-typeset MODULE
	src/plugin.cpp
	src/text-engine.cpp
	src/typeset-source.cpp
	src/raster-worker.cpp
	src/preview-editor.cpp)
target_link_libraries(obs-typeset PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets
	PkgConfig::PANGOCAIRO)
target_compile_options(obs-typeset PRIVATE -Wall -Wextra)
set_target_properties(obs-typeset PROPERTIES PREFIX "" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
	CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden)
