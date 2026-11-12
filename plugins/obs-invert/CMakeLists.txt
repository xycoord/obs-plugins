cmake_minimum_required(VERSION 3.16)
project(obs-invert VERSION 1.0.0 LANGUAGES C)

find_package(libobs REQUIRED)

add_library(obs-invert MODULE invert-filter.c)
target_link_libraries(obs-invert PRIVATE OBS::libobs)
target_compile_options(obs-invert PRIVATE -Wall -Wextra)
set_target_properties(obs-invert PROPERTIES PREFIX "" C_STANDARD 11 C_STANDARD_REQUIRED ON)
