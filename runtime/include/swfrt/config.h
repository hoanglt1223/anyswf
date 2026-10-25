/* Per-game settings from anyswf.toml (format: docs/user/USAGE.md). */
#ifndef SWFRT_CONFIG_H
#define SWFRT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWF_CONFIG_BUTTONS 12   /* PLAT_BTN_* bits 0..11 */
#define SWF_KEY_NONE 0

typedef struct swf_config {
    uint8_t keys[SWF_CONFIG_BUTTONS];  /* Flash key code per button; 255 = mouse button, 0 = none */
    uint8_t scale_mode;                /* swf_scale_mode */
    float cursor_speed;                /* pixels per second with the nub fully tilted */
    float cursor_accel;                /* extra speed factor reached after holding the nub 1 s */
    bool cursor_visible;               /* draw the cursor (Mouse.hide still hides it) */
    bool profiler;                     /* start with the profiler overlay shown */
} swf_config;

void swf_config_defaults(swf_config *c);
/* Applies anyswf.toml text over `c`. Unknown keys and bad values are logged and skipped;
 * returns false if anything was skipped. */
bool swf_config_parse(swf_config *c, const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif
