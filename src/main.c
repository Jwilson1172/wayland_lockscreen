// vim: ft=c
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>

#include "gl.h"
#include "keyboard.h"
#include "main.h"
#include "pam.h"
#include "protocols/ext-session-lock-protocol-v1.h"

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
        struct output *out;
        struct output *tmp;

        // Destroy the gl stack
        // we have to switch the existing eglSurface, display, and context to
        // current in order to destroy them.
        if (s->egl_display != EGL_NO_DISPLAY && s->egl_ctx != EGL_NO_CONTEXT) {
                if (s->program) {
                        EGLSurface surf = EGL_NO_SURFACE;
                        wl_list_for_each(out, &s->outputs, link)
                        {
                                if (out->egl_surface != EGL_NO_SURFACE) {
                                        surf = out->egl_surface;
                                        break;
                                }
                        }
                        if (surf != EGL_NO_SURFACE &&
                            eglMakeCurrent(s->egl_display, surf, surf,
                                           s->egl_ctx)) {
                                glDeleteProgram(s->program);
                        }
                }
                eglMakeCurrent(s->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               EGL_NO_CONTEXT);
        }

        // Now actually try to destroy them
        wl_list_for_each_safe(out, tmp, &s->outputs, link)
        {
                output_destroy(out);
        }

        if (s->egl_ctx != EGL_NO_CONTEXT) {
                eglDestroyContext(s->egl_display, s->egl_ctx);
        }
        if (s->egl_display != EGL_NO_DISPLAY) {
                eglTerminate(s->egl_display);
        }
        // be 'finished'
        eglReleaseThread();

        // if our lock is finished then destroy it, if
        // it's still locked we are going to want to stay locked.
        if (s->lock && s->lock_status == LOCK_FINISHED) {
                ext_session_lock_v1_destroy(s->lock);
        }

        if (s->keyboard) {
                wl_keyboard_release(s->keyboard);
        }
        if (s->seat) {
                wl_seat_release(s->seat);
        }
        if (s->lock_manager) {
                ext_session_lock_manager_v1_destroy(s->lock_manager);
        }
        if (s->compositor) {
                wl_compositor_destroy(s->compositor);
        }
        if (s->resistry) {
                wl_registry_destroy(s->resistry);
        }
        if (s->display) {
                wl_display_disconnect(s->display);
        }
        if (s->xkb_state) {
                xkb_state_unref(s->xkb_state);
        }
        if (s->xkb_keymap) {
                xkb_keymap_unref(s->xkb_keymap);
        }
        if (s->xkb_context) {
                xkb_context_unref(s->xkb_context);
        }
        free(s->user_shader);
        wipe_secret(s->password, sizeof(s->password));
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
static const struct wl_keyboard_listener keyboard_listener = {
        .keymap      = keyboard_keymap,
        .enter       = keyboard_enter,
        .leave       = keyboard_leave,
        .key         = keyboard_key,
        .modifiers   = keyboard_modifiers,
        .repeat_info = keyboard_repeat_info,
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

        if (!s.compositor || !s.lock_manager) {
                (void)fprintf(
                        stderr,
                        "Compositor lacks wl_compositor or ext-session-lock-v1\n");
                goto done;
        }
        if (!s.seat) {
                (void)fprintf(
                        stderr,
                        "no wl_seat (v5+): you couldn't have typed a password\n");
                goto done;
        }
        if (egl_setup(&s) < 0) {
                (void)fprintf(stderr, "EGL/GLES2 failed setup (0x%x)\n",
                              eglGetError());
                goto done;
        }

        s.lock = ext_session_lock_manager_v1_lock(s.lock_manager);
        ext_session_lock_v1_add_listener(s.lock, &lock_listener, &s);
        s.lock_status = LOCK_REQUESTED;

        wl_display_roundtrip(s.display);
        while (s.running && wl_display_dispatch(s.display) != -1) {
                ;
        }
        if (s.lock_status == LOCK_UNLOCKED) {
                if (wl_display_roundtrip(s.display) != -1) {
                        exit_code = 0;
                }
        }
done:
        cleanup(&s);
        return exit_code;
}
