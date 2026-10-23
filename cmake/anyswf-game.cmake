# anyswf_add_game(<target> <game.c> [TITLE <title>])
# Builds a player executable for one compiled movie (swf2c compile output).
function(anyswf_add_game target game_c)
    cmake_parse_arguments(ARG "" "TITLE" "" ${ARGN})
    if(NOT ARG_TITLE)
        set(ARG_TITLE "${target}")
    endif()
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    add_executable(${target} ${root}/apps/player/main.c ${root}/apps/player/png_write.c ${game_c})
    target_link_libraries(${target} PRIVATE swfrt)
    target_include_directories(${target} PRIVATE ${root}/runtime/src)
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
