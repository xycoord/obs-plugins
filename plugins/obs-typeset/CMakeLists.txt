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
	src/rich-text.cpp
	src/typeset-source.cpp
	src/settings.cpp
	src/properties.cpp
	src/raster-worker.cpp
	src/preview-editor.cpp
	src/obs-preview.cpp
	src/edit-model.cpp
	src/overlay.cpp
	src/format-bar.cpp)
target_link_libraries(obs-typeset PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets
	PkgConfig::PANGOCAIRO)
target_compile_options(obs-typeset PRIVATE -Wall -Wextra)
# The drawing's pixel loops (the shadow's spread and blur) run several times
# faster when the compiler does many pixels at once, which -O3 lets it.
set_source_files_properties(src/text-engine.cpp PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:-O3>")
# A symbol missing from the module fails the build, not OBS at the moment it's first called.
target_link_options(obs-typeset PRIVATE -Wl,--no-undefined)
set_target_properties(obs-typeset PROPERTIES PREFIX "" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
	CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden)
