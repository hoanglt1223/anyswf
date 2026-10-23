# runtime (libswfrt)

C11 runtime linked into every compiled game: display list and timeline, AVM1
value/object model and GC, built-in Flash classes, and the GE/OpenGL renderer.
It sits on top of the platform layer (`../platform/platform.h`) and never calls
pspsdk or SDL directly except inside its renderer backends.

Empty until the SWF parsing, renderer, and AVM1 phases land; the CMake target is
added with the first source file.
