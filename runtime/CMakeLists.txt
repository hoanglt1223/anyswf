add_library(swfrt STATIC
    src/assets.c
    src/display/display_list.c
    src/display/timeline.c
    src/display/movieclip.c
    src/display/edit_text.c
    src/render/color_transform.c)
target_include_directories(swfrt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(swfrt PUBLIC plat)

if(PSP)
    target_sources(swfrt PRIVATE src/render/ge.c)
else()
    target_sources(swfrt PRIVATE src/render/gl.c)
endif()
