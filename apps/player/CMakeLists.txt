add_executable(anyswf-player main.c png_write.c)
target_link_libraries(anyswf-player PRIVATE swfrt)
target_include_directories(anyswf-player PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)

if(PSP)
    create_pbp_file(
        TARGET anyswf-player
        TITLE "AnySWF player"
        VERSION 01.00
        ICON_PATH NULL
        BACKGROUND_PATH NULL
        PREVIEW_PATH NULL
    )
endif()
