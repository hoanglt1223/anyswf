/*
 * OpenGL header for PC renderers drawing between plat_frame_begin and plat_frame_end.
 * Desktop: OpenGL 2.1 compatibility profile. Android: OpenGL ES 1.1, which keeps the
 * same fixed-function pipeline; the macros below cover the few spelling differences, so
 * code limited to the common subset builds for both.
 */
#ifndef PLAT_GL_H
#define PLAT_GL_H

#include <SDL3/SDL.h>

#if defined(SDL_PLATFORM_ANDROID)
#include <SDL3/SDL_opengles.h>
#define PLAT_GL_ES 1
#define glOrtho glOrthof
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 GL_DEPTH24_STENCIL8_OES
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER GL_FRAMEBUFFER_OES
#define GL_RENDERBUFFER GL_RENDERBUFFER_OES
#define GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE_OES
#define GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0_OES
#define GL_DEPTH_ATTACHMENT GL_DEPTH_ATTACHMENT_OES
#define GL_STENCIL_ATTACHMENT GL_STENCIL_ATTACHMENT_OES
#endif
/* Extension entry points carry an OES suffix on ES 1.1. */
#define PLAT_GL_EXT_NAME(name) #name "OES"
#else
#include <SDL3/SDL_opengl.h>
#define PLAT_GL_ES 0
#define PLAT_GL_EXT_NAME(name) #name
#endif

/* GL 1.5 renamed the texture combiner sources; ES 1.1 only has the new names. */
#ifndef GL_SRC0_RGB
#define GL_SRC0_RGB GL_SOURCE0_RGB
#define GL_SRC0_ALPHA GL_SOURCE0_ALPHA
#define GL_SRC1_ALPHA GL_SOURCE1_ALPHA
#endif

#endif
