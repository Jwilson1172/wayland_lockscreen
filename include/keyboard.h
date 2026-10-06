#ifndef KEYBOARD_H_DEF
#define KEYBOARD_H_DEF

#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>

void wipe_secret(void *s, size_t len);

// for constructing keyboard listener in main.
void keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format,
                     int32_t fd, uint32_t size);
void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                    struct wl_surface *surf, struct wl_array *keys);
void keyboard_leave(void *d, struct wl_keyboard *k, uint32_t serial,
                    struct wl_surface *surf);
void keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial,
                  uint32_t time, uint32_t key, uint32_t key_state);
void keyboard_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
                        uint32_t dep, uint32_t lat, uint32_t lock,
                        uint32_t grp);
void keyboard_repeat_info(void *data, struct wl_keyboard *k, int32_t rate,
                          int32_t delay);

#endif // !KEYBOARD_H_DEF
