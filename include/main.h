// vim: ft=C
#ifndef MAIN_H_DEF
#define MAIN_H_DEF

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdint.h>
#include <time.h>
#include <wayland-client.h>

#define PASSWORD_MAX 256
#define MAX_SHADER_SIZE (size_t)(16 * 1024 * 1024) // 16MiB
#define MIN(a, b) ((a) < (b) ? (a) : (b))

enum lock_status {
        LOCK_NONE,
        LOCK_REQUESTED,
        LOCK_LOCKED,
        LOCK_FINISHED,
        LOCK_UNLOCKED,
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

        size_t password_cursor;
        size_t password_sel_anchor;
        size_t password_sel_cursor;

        int    running;
        int    lock_status;
};

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

static void try_unlock(struct lock_state *s);

#endif
