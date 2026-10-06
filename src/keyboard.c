#include "main.h"
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

// keyboard helpers

// checks if the character is a utf-8 boundry
static bool password_is_continuation(unsigned char c)
{
        return (c & 0xC0) == 0x80;
}

// scans backwards to last boundry
static size_t password_prev_boundary(const struct lock_state *s, size_t pos)
{
        if (pos == 0) {
                return 0;
        }
        do {
                pos--;
        } while (pos > 0 &&
                 password_is_continuation((unsigned char)s->password[pos]));
        return pos;
}

// scans to next wordbyte boundry
static size_t password_next_boundary(const struct lock_state *s, size_t pos)
{
        if (pos >= s->password_len) {
                return s->password_len;
        }
        pos++;
        while (pos < s->password_len &&
               password_is_continuation((unsigned char)s->password[pos])) {
                pos++;
        }
        return pos;
}

// functions dealing with selections i.e Shift+A, Shift+Left, Shift+Right
static size_t password_sel_start(const struct lock_state *s)
{
        return s->password_sel_anchor < s->password_sel_cursor ?
                       s->password_sel_anchor :
                       s->password_sel_cursor;
}

static size_t password_sel_end(const struct lock_state *s)
{
        return s->password_sel_anchor > s->password_sel_cursor ?
                       s->password_sel_anchor :
                       s->password_sel_cursor;
}

static bool password_has_selection(const struct lock_state *s)
{
        return s->password_sel_anchor != s->password_sel_cursor;
}

static void password_clear_selection(struct lock_state *s)
{
        s->password_sel_anchor = s->password_cursor;
        s->password_sel_cursor = s->password_cursor;
}

// defines the backspace behavior
static void password_backspace(struct lock_state *s)
{
        size_t start;
        size_t end;
        size_t removed;

        if (password_has_selection(s)) {
                start = password_sel_start(s);
                end   = password_sel_end(s);
        } else {
                if (s->password_cursor == 0) {
                        return;
                }
                start = password_prev_boundary(s, s->password_len);
                end   = s->password_cursor;
        }
        removed = end - start;

        memmove(s->password + start, s->password + end, s->password_len - end);

        s->password_len -= removed;
        s->password_cursor = start;
        password_clear_selection(s);
        explicit_bzero(s->password + s->password_len, removed);
}

// TODO:refactor this out or, switch everything to use it.
static void wipe_secret(void *buf, size_t len)
{
        explicit_bzero(buf, len);
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
        if (size <= 0 || size >= MAX_SHADER_SIZE) {
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
        // If we fail to make a new state then just throw away the keymap an
        // exit we can't apply it anyway.
        if (!xkb_state) {
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
        // TODO: REMOVE THIS BLOCK AND THE OTHER ONE IN MAIN BEFORE PROD!
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
        char utf8[8];
        int  n = xkb_state_key_get_utf8(s->xkb_state, key + 8, utf8,
                                        sizeof(utf8));

        switch (sym) {
        case XKB_KEY_Return:
                try_unlock(s);
                break;
        case XKB_KEY_BackSpace:
                password_backspace(s);
                break;
        case XKB_KEY_Escape:
                wipe_secret(s->password, s->password_len) s->password_len = 0;
                s->password_cursor                                        = 0;
                s->password_sel_cursor                                    = 0;
                s->password_sel_cursor = s->password_cursor;
                break;
        default:
        }
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
