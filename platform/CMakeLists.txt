add_library(plat STATIC common/plat_common.c)
target_include_directories(plat
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)

if(PSP)
    target_sources(plat PRIVATE psp/psp_core.c psp/psp_audio.c psp/psp_files.c)
    target_link_libraries(plat PUBLIC pspgu pspge pspdisplay pspctrl pspaudio psppower psprtc psputility m)
else()
    target_sources(plat PRIVATE sdl/sdl_core.c sdl/sdl_audio.c sdl/sdl_files.c sdl/sdl_overlay.c)
    if(ANDROID)
        # OpenGL ES 1.1 (plat_gl.h); SDL itself loads the driver through EGL.
        target_link_libraries(plat PUBLIC SDL3::SDL3 GLESv1_CM log)
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(plat PUBLIC SDL3::SDL3 OpenGL::GL)
    endif()
    if(NOT WIN32)
        target_link_libraries(plat PUBLIC m)
    endif()
endif()
