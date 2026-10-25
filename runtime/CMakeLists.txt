add_library(swfrt STATIC
    src/assets.c
    src/config.c
    src/audio/mixer.c
    src/audio/mp3.c
    src/display/display_list.c
    src/display/timeline.c
    src/display/movieclip.c
    src/display/edit_text.c
    src/display/geometry.c
    src/builtins/drawing.c
    src/builtins/events.c
    src/builtins/key.c
    src/builtins/mouse.c
    src/builtins/button.c
    src/builtins/movieclip.c
    src/builtins/textfield.c
    src/builtins/timers.c
    src/builtins/date.c
    src/builtins/sound.c
    src/builtins/sharedobject.c
    src/builtins/stage.c
    src/render/color_transform.c
    src/avm1/value.c
    src/avm1/casemap.c
    src/avm1/gc.c
    src/avm1/object.c
    src/avm1/coerce.c
    src/avm1/call.c
    src/avm1/scope.c
    src/avm1/ops.c
    src/avm1/clip.c
    src/avm1/builtins.c
    src/avm1/builtins_array.c
    src/avm1/builtins_string.c
    src/avm1/vm.c)
target_include_directories(swfrt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(swfrt PUBLIC plat)
# minimp3 is third-party code: its own include path, no project warnings.
set_source_files_properties(src/audio/mp3.c PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minimp3
    COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
if(NOT WIN32)
    target_link_libraries(swfrt PUBLIC m)
endif()

if(PSP)
    target_sources(swfrt PRIVATE src/render/ge.c)
else()
    target_sources(swfrt PRIVATE src/render/gl.c)
endif()

# A movie without ActionScript: the code table is empty.
add_library(swfrt_nocode STATIC src/avm1/no_code.c)
target_link_libraries(swfrt_nocode PUBLIC swfrt)
