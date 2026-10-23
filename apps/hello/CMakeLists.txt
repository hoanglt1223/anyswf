add_executable(hello main.c)
target_link_libraries(hello PRIVATE plat)

if(PSP)
    target_sources(hello PRIVATE hello_draw_psp.c)
    create_pbp_file(
        TARGET hello
        TITLE "AnySWF hello"
        VERSION 01.00
        ICON_PATH NULL
        BACKGROUND_PATH NULL
        PREVIEW_PATH NULL
    )
else()
    target_sources(hello PRIVATE hello_draw_gl.c)
endif()
