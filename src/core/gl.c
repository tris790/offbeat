#include "gl.h"

#include <stdio.h>

Core_GlApi core_gl;

b32 core_gl_load(Core_GlLoadProc load) {
    if (!load) return false;

    b32 ok = true;
    #define X(ret, name, args) \
        core_gl.name = (ret (*) args)load("gl" #name); \
        if (!core_gl.name) { fprintf(stderr, "[gl] missing gl%s\n", #name); ok = false; }
    CORE_GL_FUNCS(X)
    #undef X
    return ok;
}
