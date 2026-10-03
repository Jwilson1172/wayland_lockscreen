#define _POSIX_C_SOURCE 199309L
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>

#include "protocols/ext-session-lock-protocol-v1.h"
// moved here to enable __USE_MISC 1
// Idk if that effects other headers so we're seperating it so it can't effect
// other headers
// This gives us explicit_bzero(void *buf, u_size size)
#define __USE_MISC 1
#include <string.h>

#define PASSWORD_MAX 256
#define MAX_SHADER_SIZE (size_t)(16 * 1024 * 1024) // 16MiB
#define PAM_SERVICE_NAME "nyanlock"
#define PAM_SERVICE_FILE "/etc/pam.d/" PAM_SERVICE_NAME
#define MIN(a, b) ((a) < (b) ? (a) : (b))

enum lock_status {
        LOCK_NONE,
        LOCK_REQUESTED,
        LOCK_LOCKED,
        LOCK_FINISHED,
        LOCK_UNLOCKED,
};

struct lock_state;

struct output {
        struct lock_state                  *state;
        uint32_t                            global_name;
        struct wl_output                   *wl_output;
        struct wl_surface                  *wl_surface;
        struct ext_session_lock_surface_v1 *lock_surface;
        struct wl_egl_window               *egl_window;
        EGLSurface                          egl_surface;
        struct wl_callback                 *frame_cb;
        uint32_t                            width, height;
        struct wl_list                      link;
};

struct lock_state {
        struct wl_display                  *display;
        struct wl_registry                 *resistry;
        struct wl_compositor               *compositor;
        struct wl_seat                     *seat;
        struct wl_keyboard                 *keyboard;
        struct ext_session_lock_manager_v1 *lock_manager;
        struct ext_session_lock_v1         *lock;

        struct xkb_context *xkb_context;
        struct xkb_keymap  *xkb_keymap;
        struct xkb_state   *xkb_state;

        EGLDisplay      egl_display;
        EGLConfig       egl_config;
        EGLContext      egl_ctx;
        GLuint          program;
        GLint           u_resolution, u_time;
        char           *user_shader;
        struct timespec t0;

        struct wl_list outputs;

        char   password[PASSWORD_MAX];
        size_t password_len;
        int    running;
        int    lock_status;
};
// Helper functions
// file helpers
static char *read_file(const char *path)
{
        FILE *fp = fopen(path, "rb");
        if (!fp) {
                return NULL;
        }
        char  *buf = NULL;
        size_t len;

        // checks if we can read the file and gets the length of the file then
        // checks size constrains and if we can reset the index back to 0.
        if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 0 ||
            len >= MAX_SHADER_SIZE || fseek(fp, 0, SEEK_SET) != 0) {
                goto done;
        }
        buf = malloc(len + 1);
        if (!buf) {
                goto done;
        }

        // Reads and checks if we actually read the file.
        if (fread(buf, 1, len, fp) != len) {
                free(buf);
                buf = NULL;
                goto done;
        }
        // Make it a CString
        buf[len] = '\0';
done:
        fclose(fp);
        return buf;
}

// wayland helpers
static void output_destroy(struct output *out)
{
        struct lock_state *s = out->state;
        // TODO: Most of this code does nothing since we haven't finished
        // implementing it all.
        if (out->frame_cb) {
                wl_callback_destroy(out->frame_cb);
        }
        if (out->egl_surface != EGL_NO_SURFACE) {
                if (eglGetCurrentSurface(EGL_DRAW) == out->egl_surface) {
                        eglMakeCurrent(s->egl_display, EGL_NO_SURFACE,
                                       EGL_NO_SURFACE, EGL_NO_CONTEXT);
                }
                eglDestroySurface(s->egl_display, out->egl_surface);
        }
        if (out->egl_window) {
                wl_egl_window_destroy(out->egl_window);
        }
        if (out->lock_surface) {
                ext_session_lock_surface_v1_destroy(out->lock_surface);
        }
        if (out->wl_surface) {
                wl_surface_destroy(out->wl_surface);
        }
        wl_output_release(out->wl_output);
        wl_list_remove(&out->link);
        free(out);
}
static void cleanup(struct lock_state *s)
{
        (void)s;
        (void)fprintf(stderr, "Cleanup Not implemented yet! LEAKY");
}

// keyboard helpers
static void wipe_secret(void *buf, size_t len)
{
        explicit_bzero(buf, len);
}

static void password_backspace(struct lock_state *s)
{
        if (s->password_len == 0) {
                return;
        }
        size_t n = s->password_len;
        do {
                n--;
        } while (n > 0 && ((unsigned char)s->password[n] & 0xC0) == 0x80);
        explicit_bzero(s->password + n, s->password_len - n);
        s->password_len = n;
}

// PAM

// Handles the pam conversation
// Since our app is just dealing with the local user login context we can safely
// assume that the first PAM_PROMPT_ECHO_OFF msg we get is probably(%95) a
// password field. COULDDO: Support HSM/FIDO logins that'd be neat.
static int pam_conv_fn(int num_msg, const struct pam_message **msg,
                       struct pam_response **response, void *data)
{
        const char *password = data;
        if (num_msg <= 0) {
                return PAM_CONV_ERR;
        }
        struct pam_response *r = calloc((size_t)num_msg, sizeof(*r));
        if (!r) {
                return PAM_BUF_ERR;
        }
        for (int i = 0; i < num_msg; i++) {
                if (msg[i]->msg_style != PAM_PROMPT_ECHO_OFF) {
                        continue;
                }
                r[i].resp = strdup(password);
                // deconstruct message safe wiping the secret on fail.
                if (!r[i].resp) {
                        for (int j = 0; j < i; j++) {
                                if (r[j].resp) {
                                        explicit_bzero(r[j].resp,
                                                       strlen(r[j].resp));
                                        free(r[j].resp);
                                }
                        }
                        free(r);
                        return PAM_BUF_ERR;
                }
        }
        *response = r;
        return PAM_SUCCESS;
}
// Returns 1 if 'password' is correct for the current user, else 0
static int check_password(const char *password)
{
        struct passwd *pw = getpwuid(getuid());
        if (!pw) {
                return 0;
        }
        struct pam_conv conv = { pam_conv_fn, (void *)password };
        pam_handle_t   *pamh = NULL;

        int ret = pam_start(PAM_SERVICE_NAME, pw->pw_name, &conv, &pamh);
        if (ret != PAM_SUCCESS) {
                return 0;
        }

        ret = pam_authenticate(pamh, 0);
        pam_end(pamh, ret);
        return ret == PAM_SUCCESS;
}
// unlocking
static void unlock(struct lock_state *s)
{
        if (s->lock_status != LOCK_LOCKED) {
                return;
        }
        ext_session_lock_v1_unlock_and_destroy(s->lock);
        s->lock        = NULL;
        s->lock_status = LOCK_UNLOCKED;
        s->running     = 0;
}

static void try_unlock(struct lock_state *s)
{
        int ok = 0;
        if (s->lock_status == LOCK_LOCKED) {
                s->password[s->password_len] = '\0';
                ok                           = check_password(s->password);
        }
        wipe_secret(s->password, sizeof(s->password));
        s->password_len = 0;
        if (ok) {
                unlock(s);
        }
}

// XKB KEYBOARD LISTENER
static void keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format,
                            int32_t fd, uint32_t size)
{
        (void)k;
        struct lock_state *s = data;
        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
                close(fd);
                return;
        }
        // I have no idea how large one of these can get.
        // Assuming you're not a polylangual super power user
        // that types in 6 languages and has 5 sets of macros for every
        // single key on their full size keyboard 16 million lines
        // of configuration should be enough.
        // Has nothing to do with shaders btw, but I already have 16MiB defined
        // So why would I make a new def when this comment exists.
        if(size <=0 || size >= MAX_SHADER_SIZE) {
            close(fd);
            return;
        }
        char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (map_str == MAP_FAILED) {
                return;
        }
        struct xkb_keymap *keymap = xkb_keymap_new_from_string(
                s->xkb_context, map_str, XKB_KEYMAP_FORMAT_TEXT_V1,
                XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(map_str, size);
        if (!keymap) {
                return;
        }
        struct xkb_state *xkb_state = xkb_state_new(keymap);
        // If we fail to make a new state then just throw away the keymap an exit we can't apply it anyway.
        if(!xkb_state) {
            xkb_keymap_unref(keymap);
            return;
        }
        if (s->xkb_state) {
                xkb_state_unref(s->xkb_state);
        }
        if (s->xkb_keymap) {
                xkb_keymap_unref(s->xkb_keymap);
        }
        s->xkb_keymap = keymap;
        s->xkb_state  = xkb_state;
}
static void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                           struct wl_surface *surf, struct wl_array *keys)
{
        (void)d;
        (void)k;
        (void)serial;
        (void)surf;
        (void)keys;
}
static void keyboard_leave(void *d, struct wl_keyboard *k, uint32_t serial,
                           struct wl_surface *surf)
{
        (void)d;
        (void)k;
        (void)serial;
        (void)surf;
}
static void keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t key_state)
{
        (void)time;
        (void)serial;
        (void)k;
        struct lock_state *s = data;
        if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED || !s->xkb_state) {
                return;
        }
        xkb_keysym_t sym = xkb_state_key_get_one_sym(s->xkb_state, key + 8);
#ifdef MINIMAL_LOCK_DEV
        //TODO: REMOVE THIS BLOCK AND THE OTHER ONE IN MAIN BEFORE PROD!
        if (sym == XKB_KEY_Escape &&
            xkb_state_mod_name_is_active(s->xkb_state, XKB_MOD_NAME_CTRL,
                                         XKB_STATE_MODS_EFFECTIVE) > 0 &&
            xkb_state_mod_name_is_active(s->xkb_state, XKB_MOD_NAME_SHIFT,
                                         XKB_STATE_MODS_EFFECTIVE) > 0) {
                (void)fprintf(stderr,
                              "DEV ESCAPE HATCH: unlocking without auth");
                unlock(s);
                return;
        }
#endif
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                try_unlock(s);
                return;
        }
        if (sym == XKB_KEY_BackSpace) {
                password_backspace(s);
                return;
        }
        if (sym == XKB_KEY_Escape) {
                wipe_secret(s->password, sizeof(s->password));
                s->password_len = 0;
                return;
        }

        char utf8[8];
        int  n = xkb_state_key_get_utf8(s->xkb_state, key + 8, utf8,
                                        sizeof(utf8));
        (void)n;
        // TODO: add utf-8 handling and command specific handling (e.g Ctrl+A,
        // left arrow, right arrow, tab to focus next)
}
static void keyboard_modifiers(void *data, struct wl_keyboard *k,
                               uint32_t serial, uint32_t dep, uint32_t lat,
                               uint32_t lock, uint32_t grp)
{
        (void)k;
        (void)serial;
        struct lock_state *s = data;
        if (s->xkb_state) {
                xkb_state_update_mask(s->xkb_state, dep, lat, lock, 0, 0, grp);
        }
}
static void keyboard_repeat_info(void *data, struct wl_keyboard *k,
                                 int32_t rate, int32_t delay)
{
        (void)data;
        (void)rate;
        (void)delay;
        (void)k;
}
static const struct wl_keyboard_listener keyboard_listener = {
        .keymap      = keyboard_keymap,
        .enter       = keyboard_enter,
        .leave       = keyboard_leave,
        .key         = keyboard_key,
        .modifiers   = keyboard_modifiers,
        .repeat_info = keyboard_repeat_info
};
// SEAT LISTENER
// this configuires the session seat to allow us to use the keyboard capability
// Without this we wouldn't be able to listen to the keyboard

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
        struct lock_state *s = data;
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !s->keyboard) {
                s->keyboard = wl_seat_get_keyboard(seat);
                wl_keyboard_add_listener(s->keyboard, &keyboard_listener, s);
        } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && s->keyboard) {
                wl_keyboard_release(s->keyboard);
                s->keyboard = NULL;
        }
}

static void seat_name(void *d, struct wl_seat *seat, const char *name)
{
        (void)seat;
        (void)name;
        (void)d;
}

static const struct wl_seat_listener seat_listener = {
        .capabilities = seat_capabilities,
        .name         = seat_name,
};
/*
 * OpenGL ES: Shader
 */
// shader definitions + default shader source
static const char *vert_src = "atrribute vec2 pos;\n"
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
    if(!ok)
    {
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

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vsrc, 1);
    GLuint fs = vs ? compile_shader(GL_FRAGMENT_SHADER, fsrc, 3) : 0;
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
    if (vs){
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
    if (s->user_shader)
    {
        s->program = build_program(s->user_shader);
    }
    if (!s->program)
    {
        if (s->user_shader)
        {
            (void)fprintf(stderr, "Failed to build shader, falling back to built-in shader\n");
        }
        s->program = build_program(default_shader);
    }
    free(s->user_shader);
    s->user_shader = NULL;
    if(!s->program)
    {
        return -1;
    }
    s->u_resolution = glGetUniformLocation(s->program, "iResolution");
    s->u_time = glGetUniformLocation(s->program, "iTime");
    return 0;
}

//EGL setup

static int egl_setup(struct lock_state *s)
{
    s->egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, s->display, NULL);
    if (s->egl_display == EGL_NO_DISPLAY)
    {
        return -1;
    }
    EGLint major;
    EGLint minor;
    if (!eglInitialize(s->egl_display, &major, &minor))
    {
        return -1;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
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
    if (!eglChooseConfig(s->egl_display, cfg_attr, &s->egl_config, 1, &n) || n < 1)
    {
        return -1;
    }
    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    s->egl_ctx = eglCreateContext(s->egl_display, s->egl_config, EGL_NO_CONTEXT, ctx_attr);
    return s->egl_ctx == EGL_NO_CONTEXT ? -1 : 0;
}

// per-output rendering
static float elapsed_seconds(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    float ret = (float)((double)(t.tv_sec - t0->tv_sec) + ((double)(t.tv_nsec - t0->tv_nsec) / 1e9));
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
    struct lock_state *s = out->state;
    static const GLfloat tri[] = {-1.0F, -1.0F, 3.0F, -1.0F, -1.0F, 3.0F};
    if (!eglMakeCurrent(s->egl_display, out->egl_surface, out->egl_surface, s->egl_ctx))
    {
        (void)fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError());
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
// GLOBAL REGISTRY
static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
        struct lock_state *s = data;
        // Compositor
        if (strcmp(interface, wl_compositor_interface.name) == 0) {
                s->compositor = wl_registry_bind(registry, name,
                                                 &wl_compositor_interface,
                                                 MIN(version, 4U));

                // Seat
        } else if (strcmp(interface, wl_seat_interface.name) == 0 &&
                   version >= 5 && !s->seat) {
                s->seat =
                        wl_registry_bind(registry, name, &wl_seat_interface, 5);
                wl_seat_add_listener(s->seat, &seat_listener, s);
                // Session lock manager
        } else if (strcmp(interface,
                          ext_session_lock_manager_v1_interface.name) == 0) {
                s->lock_manager = wl_registry_bind(
                        registry, name, &ext_session_lock_manager_v1_interface,
                        1);
                // Output
        } else if (strcmp(interface, wl_output_interface.name) == 0 &&
                   version >= 3) {
                struct output *out = calloc(1, sizeof(*out));
                if (!out) {
                        return;
                }
                out->state       = s;
                out->global_name = name;
                out->wl_output   = wl_registry_bind(registry, name,
                                                    &wl_output_interface, 3);
                wl_list_insert(&s->outputs, &out->link);
                if (s->lock_status == LOCK_REQUESTED ||
                    s->lock_status == LOCK_LOCKED) {
                        (void)fprintf(stderr, "Not Implemented yet");
                }
                // output_attach(out);
        }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
        (void)registry;
        struct lock_state *s = data;
        struct output     *out;
        struct output     *tmp;
        wl_list_for_each_safe(out, tmp, &s->outputs, link)
        {
                if (out->global_name == name) {
                        output_destroy(out);
                        return;
                }
        }
}
static const struct wl_registry_listener registry_listener = {
        .global        = registry_global,
        .global_remove = registry_global_remove,
};

int main(int argc, char **argv)
{
        // init state
        struct lock_state s = { 0 };
        struct output    *o;
        int               exit_code = 1;
        wl_list_init(&s.outputs);
        s.running = 1;

        // no frag shader passed
        if (argc > 2) {
                (void)fprintf(stderr, "usage: %s [shader.frag]\n", argv[0]);
                return 2;
        }

        // Check if the pam service file is present
        if (access(PAM_SERVICE_FILE, R_OK) != 0) {
                (void)fprintf(stderr,
                              "%s not found; refusing to lock(See README.md)\n",
                              PAM_SERVICE_FILE);
                return 1;
        }

        if (argc == 2) {
                s.user_shader = read_file(argv[1]);
                if (!s.user_shader) {
                        (void)fprintf(stderr, "cannot read shader %s: %s\n",
                                      argv[1], strerror(errno));
                        return 1;
                }
        }

#ifdef MINIMAL_LOCK_DEV
        (void)fprintf(
                stderr,
                "WARNING: DEV BUILD. Ctrl+Shift+Escape unlocks without a password.\n");
#endif
        clock_gettime(CLOCK_MONOTONIC, &s.t0);
        s.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        s.display     = wl_display_connect(NULL);
        if (!s.display) {
                (void)fprintf(stderr, "failed to connect to Wayland display\n");
                goto done;
        }
        s.resistry = wl_display_get_registry(s.display);
        wl_registry_add_listener(s.resistry, &registry_listener, &s);
        wl_display_roundtrip(s.display); // collect globals
done:
        cleanup(&s);
        return exit_code;
}
