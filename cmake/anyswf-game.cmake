# anyswf_add_game(<target> <game.c> [TITLE <title>] [APP_ID <id>])
# Builds a player for one compiled movie (swf2c compile output): an executable, an
# EBOOT.PBP on PSP, or libmain.so (loaded by SDLActivity) on Android. APP_ID names the
# save folder ([A-Za-z0-9_-], at most 32 characters; default: the target name).
function(anyswf_add_game target game_c)
    cmake_parse_arguments(ARG "" "TITLE;APP_ID" "" ${ARGN})
    if(NOT ARG_TITLE)
        set(ARG_TITLE "${target}")
    endif()
    if(NOT ARG_APP_ID)
        set(ARG_APP_ID "${target}")
    endif()
    if(NOT ARG_APP_ID MATCHES "^[A-Za-z0-9_-]+$")
        message(FATAL_ERROR "anyswf_add_game: APP_ID '${ARG_APP_ID}' may only use A-Z a-z 0-9 _ -")
    endif()
    string(LENGTH "${ARG_APP_ID}" id_len)
    if(id_len GREATER 32)
        message(FATAL_ERROR "anyswf_add_game: APP_ID '${ARG_APP_ID}' is longer than 32 characters")
    endif()
    # The title becomes a C string literal.
    string(REGEX REPLACE "[\"\\\\]" "" c_title "${ARG_TITLE}")

    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(sources ${root}/apps/player/main.c ${root}/apps/player/png_write.c ${game_c})
    if(ANDROID)
        add_library(${target} SHARED ${sources})
        set_target_properties(${target} PROPERTIES OUTPUT_NAME main)
        # SDL's Java side finds JNI_OnLoad (static SDL, nothing else references it) and
        # SDL_main by name. 16 KB pages for Android 15+ devices.
        target_link_options(${target} PRIVATE -Wl,--undefined=JNI_OnLoad -Wl,--no-undefined
                            -Wl,-z,max-page-size=16384)
        # SDL's Android HIDAPI code is C++; this C-only link needs the static C++ runtime.
        target_link_libraries(${target} PRIVATE c++_static c++abi)
    else()
        add_executable(${target} ${sources})
    endif()
    target_link_libraries(${target} PRIVATE swfrt)
    target_include_directories(${target} PRIVATE ${root}/runtime/src)
    target_compile_definitions(${target} PRIVATE "ANYSWF_APP_ID=\"${ARG_APP_ID}\"" "ANYSWF_TITLE=\"${c_title}\"")
    # A game is a GUI program: no console window, still entered through main().
    if(MSVC)
        target_link_options(${target} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    elseif(MINGW)
        target_link_options(${target} PRIVATE -mwindows)
    endif()
    if(PSP)
        create_pbp_file(
            TARGET ${target}
            TITLE "${ARG_TITLE}"
            VERSION 01.00
            ICON_PATH NULL
            BACKGROUND_PATH NULL
            PREVIEW_PATH NULL
        )
    endif()
endfunction()
