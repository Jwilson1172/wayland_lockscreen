#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>

#include "main.h"
/*
 * OpenGL ES: Shader
 */
// shader definitions + default shader source
static const char *vert_src =
        "atrribute vec2 pos;\n"
        "void main() { gl_Position = vec4(pos, 0.0, 1.0);\n";
static const char *frag_prelude = "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
                                  "precision highp float;\n"
                                  "#else\n"
                                  "precision mediump float;\n"
                                  "#endif\n"
                                  "uniform vec2 iResolution;\n"
                                  "uniform float iTime;\n";
// alpha is forced to 1.0 so a shader can never make the lock see through
static const char *frag_epiloge = "\nvoid main() {\n"
                                  "    vec4 c = vec4(0.0);\n"
                                  "mainImage(c, gl_FragCoord.xy);\n"
                                  "gl_FragCord = vec4(c.rgb, 1.0);\n"
                                  "}\n";

static const char *default_shader =
        "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
        "    vec2 uv = fragCoord / iResolution.xy;\n"
        "    vec3 col = 0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0.0, 2.0, 4.0));\n"
        "    fragColor = vec4(col * 0.25, 1.0);\n"
        "}\n";

static GLuint compile_shader(GLenum type, const char **srcs, GLsizei count)
{
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, count, srcs, NULL);
        glCompileShader(sh);

        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
                char log[1024];
                glGetShaderInfoLog(sh, sizeof(log), NULL, log);
                (void)fprintf(stderr, "Shader compile error: \n%s\n", log);
                glDeleteShader(sh);
                return 0;
        }
        return sh;
}

static GLuint build_program(const char *user_source)
{
        const char *vsrc[] = { vert_src };
        const char *fsrc[] = { frag_prelude, user_source, frag_epiloge };

        GLuint vs   = compile_shader(GL_VERTEX_SHADER, vsrc, 1);
        GLuint fs   = vs ? compile_shader(GL_FRAGMENT_SHADER, fsrc, 3) : 0;
        GLuint prog = 0;

        if (vs && fs) {
                prog = glCreateProgram();
                glAttachShader(prog, vs);
                glAttachShader(prog, fs);
                glBindAttribLocation(prog, 0, "pos");
                glLinkProgram(prog);

                GLint ok = 0;
                glGetProgramiv(prog, GL_LINK_STATUS, &ok);
                if (!ok) {
                        char log[1024];
                        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
                        (void)fprintf(stderr, "program link error:\n%s\n", log);
                        glDeleteProgram(prog);
                        prog = 0;
                }
        }
        if (vs) {
                glDeleteShader(vs);
        }
        if (fs) {
                glDeleteShader(fs);
        }
        return prog;
}

// needs a current EGL context.
static int gl_init(struct lock_state *s)
{
        if (s->user_shader) {
                s->program = build_program(s->user_shader);
        }
        if (!s->program) {
                if (s->user_shader) {
                        (void)fprintf(
                                stderr,
                                "Failed to build shader, falling back to built-in shader\n");
                }
                s->program = build_program(default_shader);
        }
        free(s->user_shader);
        s->user_shader = NULL;
        if (!s->program) {
                return -1;
        }
        s->u_resolution = glGetUniformLocation(s->program, "iResolution");
        s->u_time       = glGetUniformLocation(s->program, "iTime");
        return 0;
}

// EGL setup

static int egl_setup(struct lock_state *s)
{
        s->egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,
                                               s->display, NULL);
        if (s->egl_display == EGL_NO_DISPLAY) {
                return -1;
        }
        EGLint major;
        EGLint minor;
        if (!eglInitialize(s->egl_display, &major, &minor)) {
                return -1;
        }
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
                return -1;
        }

        static const EGLint cfg_attr[] = {
                EGL_SURFACE_TYPE,
                EGL_WINDOW_BIT,
                EGL_RENDERABLE_TYPE,
                EGL_OPENGL_ES2_BIT,
                EGL_RED_SIZE,
                8,
                EGL_GREEN_SIZE,
                8,
                EGL_BLUE_SIZE,
                8,
                EGL_NONE,
        };
        EGLint n = 0;
        if (!eglChooseConfig(s->egl_display, cfg_attr, &s->egl_config, 1, &n) ||
            n < 1) {
                return -1;
        }
        static const EGLint ctx_attr[] = {
                EGL_CONTEXT_CLIENT_VERSION,
                2,
                EGL_NONE,
        };
        s->egl_ctx = eglCreateContext(s->egl_display, s->egl_config,
                                      EGL_NO_CONTEXT, ctx_attr);
        return s->egl_ctx == EGL_NO_CONTEXT ? -1 : 0;
}

// per-output rendering
static float elapsed_seconds(const struct timespec *t0)
{
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        float ret = (float)((double)(t.tv_sec - t0->tv_sec) +
                            ((double)(t.tv_nsec - t0->tv_nsec) / 1e9));
        return ret;
}

static void render(struct output *out);

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
        (void)time;
        struct output *out = data;
        wl_callback_destroy(cb);
        out->frame_cb = NULL;
        render(out);
}
static const struct wl_callback_listener frame_listener = {
        .done = frame_done,
};
static void render(struct output *out)
{
        struct lock_state   *s     = out->state;
        static const GLfloat tri[] = { -1.0F, -1.0F, 3.0F, -1.0F, -1.0F, 3.0F };
        if (!eglMakeCurrent(s->egl_display, out->egl_surface, out->egl_surface,
                            s->egl_ctx)) {
                (void)fprintf(stderr, "eglMakeCurrent failed: 0x%x\n",
                              eglGetError());
                return;
        }

        glViewport(0, 0, (int32_t)out->width, (int32_t)out->height);
        glUseProgram(s->program);
        glUniform2f(s->u_resolution, (float)out->width, (float)out->height);
        glUniform1f(s->u_time, elapsed_seconds(&s->t0));
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // ask for the next frame
        out->frame_cb = wl_surface_frame(out->wl_surface);
        wl_callback_add_listener(out->frame_cb, &frame_listener, out);
        eglSwapBuffers(s->egl_display, out->egl_surface);
}
