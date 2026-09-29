#ifndef CORE_GL_H
#define CORE_GL_H

/*
 * Minimal self-contained OpenGL 4.6 loader.
 *
 * We deliberately avoid GLEW/glad: we only need a few dozen modern entry
 * points, so they are listed once in CORE_GL_FUNCS (an X-macro) which expands
 * into the function-pointer table, the loader and the short aliases. Adding a
 * function is one line.
 */

#include "types.h"
#include <GL/gl.h> /* GLenum/GLuint/GLfloat and the classic 1.x entry points */

#include "renderer.h" /* Core_GlLoadProc */

typedef char      GLchar;
typedef ptrdiff_t GLsizeiptr_;
typedef ptrdiff_t GLintptr_;

/* Constants beyond GL 1.1 that the system header may not define. */
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER                   0x8892
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW                    0x88E0
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER                0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER                  0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS                 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS                    0x8B82
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                  0x812F
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0                       0x84C0
#endif
#ifndef GL_R8
#define GL_R8                             0x8229
#endif
#ifndef GL_RED
#define GL_RED                            0x1903
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL              0x813D
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                    0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER                   0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0              0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT                  0x0002
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT      0x0008
#endif
#ifndef GL_MAP_UNSYNCHRONIZED_BIT
#define GL_MAP_UNSYNCHRONIZED_BIT         0x0020
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                          0x8058
#endif
#ifndef GL_TEXTURE_SWIZZLE_RGBA
#define GL_TEXTURE_SWIZZLE_RGBA           0x8E46
#endif

/* X(return type, name, parameter list) */
#define CORE_GL_FUNCS(X) \
    X(void,   GenBuffers,              (GLsizei, GLuint *)) \
    X(void,   BindBuffer,              (GLenum, GLuint)) \
    X(void,   BufferData,              (GLenum, GLsizeiptr_, const void *, GLenum)) \
    X(void,   BufferSubData,           (GLenum, GLintptr_, GLsizeiptr_, const void *)) \
    X(void *, MapBufferRange,          (GLenum, GLintptr_, GLsizeiptr_, GLbitfield)) \
    X(GLboolean, UnmapBuffer,          (GLenum)) \
    X(void,   DeleteBuffers,           (GLsizei, const GLuint *)) \
    X(void,   GenVertexArrays,         (GLsizei, GLuint *)) \
    X(void,   BindVertexArray,         (GLuint)) \
    X(void,   DeleteVertexArrays,      (GLsizei, const GLuint *)) \
    X(void,   EnableVertexAttribArray, (GLuint)) \
    X(void,   VertexAttribPointer,     (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void,   VertexAttribIPointer,    (GLuint, GLint, GLenum, GLsizei, const void *)) \
    X(void,   VertexAttribDivisor,     (GLuint, GLuint)) \
    X(void,   DrawArraysInstanced,     (GLenum, GLint, GLsizei, GLsizei)) \
    X(GLuint, CreateShader,            (GLenum)) \
    X(void,   ShaderSource,            (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void,   CompileShader,           (GLuint)) \
    X(void,   GetShaderiv,             (GLuint, GLenum, GLint *)) \
    X(void,   GetShaderInfoLog,        (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,   DeleteShader,            (GLuint)) \
    X(GLuint, CreateProgram,           (void)) \
    X(void,   AttachShader,            (GLuint, GLuint)) \
    X(void,   LinkProgram,             (GLuint)) \
    X(void,   GetProgramiv,            (GLuint, GLenum, GLint *)) \
    X(void,   GetProgramInfoLog,       (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,   UseProgram,              (GLuint)) \
    X(void,   DeleteProgram,           (GLuint)) \
    X(GLint,  GetUniformLocation,      (GLuint, const GLchar *)) \
    X(void,   Uniform1i,               (GLint, GLint)) \
    X(void,   Uniform1f,               (GLint, GLfloat)) \
    X(void,   Uniform2f,               (GLint, GLfloat, GLfloat)) \
    X(void,   Uniform4f,               (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,   Uniform4fv,              (GLint, GLsizei, const GLfloat *)) \
    X(void,   UniformMatrix4fv,        (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void,   ActiveTexture,           (GLenum)) \
    X(void,   GenerateMipmap,          (GLenum)) \
    X(void,   BlendFuncSeparate,       (GLenum, GLenum, GLenum, GLenum)) \
    X(void,   GenFramebuffers,         (GLsizei, GLuint *)) \
    X(void,   BindFramebuffer,         (GLenum, GLuint)) \
    X(void,   FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, CheckFramebufferStatus,  (GLenum)) \
    X(void,   DeleteFramebuffers,      (GLsizei, const GLuint *)) \
    X(void,   GenRenderbuffers,        (GLsizei, GLuint *)) \
    X(void,   BindRenderbuffer,        (GLenum, GLuint)) \
    X(void,   RenderbufferStorage,     (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void,   DeleteRenderbuffers,     (GLsizei, const GLuint *))

typedef struct {
    #define X(ret, name, args) ret (*name) args;
    CORE_GL_FUNCS(X)
    #undef X
} Core_GlApi;

extern Core_GlApi core_gl;

/* Resolve every pointer through `load`. Returns false if any is missing. */
b32 core_gl_load(Core_GlLoadProc load);

/* Short aliases so backend code reads like normal GL. */
#define glGenBuffers              core_gl.GenBuffers
#define glBindBuffer              core_gl.BindBuffer
#define glBufferData              core_gl.BufferData
#define glBufferSubData           core_gl.BufferSubData
#define glMapBufferRange          core_gl.MapBufferRange
#define glUnmapBuffer             core_gl.UnmapBuffer
#define glDeleteBuffers           core_gl.DeleteBuffers
#define glGenVertexArrays         core_gl.GenVertexArrays
#define glBindVertexArray         core_gl.BindVertexArray
#define glDeleteVertexArrays      core_gl.DeleteVertexArrays
#define glEnableVertexAttribArray core_gl.EnableVertexAttribArray
#define glVertexAttribPointer     core_gl.VertexAttribPointer
#define glVertexAttribIPointer    core_gl.VertexAttribIPointer
#define glVertexAttribDivisor     core_gl.VertexAttribDivisor
#define glDrawArraysInstanced     core_gl.DrawArraysInstanced
#define glCreateShader            core_gl.CreateShader
#define glShaderSource            core_gl.ShaderSource
#define glCompileShader           core_gl.CompileShader
#define glGetShaderiv             core_gl.GetShaderiv
#define glGetShaderInfoLog        core_gl.GetShaderInfoLog
#define glDeleteShader            core_gl.DeleteShader
#define glCreateProgram           core_gl.CreateProgram
#define glAttachShader            core_gl.AttachShader
#define glLinkProgram             core_gl.LinkProgram
#define glGetProgramiv            core_gl.GetProgramiv
#define glGetProgramInfoLog       core_gl.GetProgramInfoLog
#define glUseProgram              core_gl.UseProgram
#define glDeleteProgram           core_gl.DeleteProgram
#define glGetUniformLocation      core_gl.GetUniformLocation
#define glUniform1i               core_gl.Uniform1i
#define glUniform1f               core_gl.Uniform1f
#define glUniform2f               core_gl.Uniform2f
#define glUniform4f               core_gl.Uniform4f
#define glUniform4fv              core_gl.Uniform4fv
#define glUniformMatrix4fv        core_gl.UniformMatrix4fv
#define glActiveTexture           core_gl.ActiveTexture
#define glGenerateMipmap          core_gl.GenerateMipmap
#define glBlendFuncSeparate       core_gl.BlendFuncSeparate
#define glGenFramebuffers         core_gl.GenFramebuffers
#define glBindFramebuffer         core_gl.BindFramebuffer
#define glFramebufferRenderbuffer core_gl.FramebufferRenderbuffer
#define glCheckFramebufferStatus  core_gl.CheckFramebufferStatus
#define glDeleteFramebuffers      core_gl.DeleteFramebuffers
#define glGenRenderbuffers        core_gl.GenRenderbuffers
#define glBindRenderbuffer        core_gl.BindRenderbuffer
#define glRenderbufferStorage     core_gl.RenderbufferStorage
#define glDeleteRenderbuffers     core_gl.DeleteRenderbuffers

#endif /* CORE_GL_H */
