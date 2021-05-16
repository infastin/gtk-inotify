cmake_minimum_required(VERSION 3.1.3)

project(test C)

# Set standart
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

# Compile commands for lang server
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set output dirs
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Set GCC compile options
add_compile_options(
	-g -O2 -pipe 
	-D_FORTIFY_SOURCE=2 
	-Werror=implicit-function-declaration -Wall
	-funsigned-char -std=c11 -fms-extensions
)


# Load gtk4
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
add_definitions(${GTK4_CFLAGS_OTHER})

# Load libmagic
find_library(MAGIC_LIBRARY magic)

# Set working dirs
set(RESOURCES_DIR ${CMAKE_SOURCE_DIR}/resources)
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)

set(WORKING_DIRS 
	${SRC_DIR}
	${GTK4_INCLUDE_DIRS}
	${MAGIC_LIBRARY_DIR}
)

# glib-compile-resources
find_program(GLIB_COMPILE_RESOURCES NAMES glib-compile-resources REQUIRED)

set(GRESOURCE_C inotifyapp.gresource.c)
set(GRESOURCE_XML inotifyapp.gresource.xml)

if(EXISTS ${CMAKE_BINARY_DIR}/${GRESOURCE_C})
	file(REMOVE ${CMAKE_BINARY_DIR}/${GRESOURCE_C})
endif()

add_custom_command(
	OUTPUT ${GRESOURCE_C}
	WORKING_DIRECTORY ${RESOURCES_DIR}
	COMMAND ${GLIB_COMPILE_RESOURCES}
	ARGS
		--generate-source
		--target=${CMAKE_BINARY_DIR}/${GRESOURCE_C}
		${RESOURCES_DIR}/${GRESOURCE_XML}
	VERBATIM
	MAIN_DEPENDENCY ${RESOURCES_DIR}/${GRESOURCE_XML}
)

add_custom_target(inotify-resource
	DEPENDS ${CMAKE_BINARY_DIR}/${GRESOURCE_C}
)

# Set libs
add_library(base STATIC
	${SRC_DIR}/inotify_app.c
	${SRC_DIR}/inotify_app_win.c
)

target_link_libraries(base 
	${GTK4_LIBRARIES}
	${MAGIC_LIBRARY}
)

target_include_directories(base PUBLIC ${WORKING_DIRS})

# Exec
add_executable(main main.c ${CMAKE_BINARY_DIR}/${GRESOURCE_C})
set_source_files_properties(
	${CMAKE_BINARY_DIR}/${GRESOURCE_C}
	PROPERTIES GENERATED TRUE
)
add_dependencies(main inotify-resource)
target_link_libraries(main base)
