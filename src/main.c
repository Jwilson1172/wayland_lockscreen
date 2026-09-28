#define _GNU_SOURCE
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

#include "ext-session-lock-v1.h"

#define PASSWORD_MAX 256
#define MAX_SHADER_SIZE (16 * 1024 * 1024) // 16MiB
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
};

static char *read_file(const char *path)
{
        FILE *fp = fopen(path, "rb");
        if (!fp)
                return NULL;
        char *buf = NULL;
        long  len;

        // checks if we can read the file and gets the length of the file then
        // checks size contrains and if we can reset the index back to 0.
        if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 0 ||
            len > MAX_SHADER_SIZE || fseek(fp, 0, SEEK_SET) != 0)
                goto done;

        buf = malloc(len + 1);
        if (!buf)
                goto done;

        // Reads and checks if we actually read the file.
        if (fread(buf, 1, len, fp) != (size_t)len) {
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
                fprintf(stderr, "usage: %s [shader.frag]\n", argv[0]);
                return 2;
        }

        // Check if the pam service file is present
        if (access(PAM_SERVICE_FILE, R_OK) != 0) {
                fprintf(stderr,
                        "%s not found; refusing to lock(See README.md)\n",
                        PAM_SERVICE_FILE);
                return 1;
        }

        if (argc == 2) {
                s.user_shader = read_file(argv[1]);
                if (!s.user_shader) {
                        fprintf(stderr, "cannot read shader %s: %s\n", argv[1],
                                strerror(errno));
                        return 1;
                }
        }
        return 0;
}
