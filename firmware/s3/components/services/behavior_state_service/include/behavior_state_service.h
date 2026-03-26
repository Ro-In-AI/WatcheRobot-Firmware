#ifndef BEHAVIOR_STATE_SERVICE_H
#define BEHAVIOR_STATE_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t behavior_state_init(void);
esp_err_t behavior_state_load(void);
esp_err_t behavior_state_set(const char *state_id);
esp_err_t behavior_state_set_with_text(const char *state_id, const char *text, int font_size);
esp_err_t behavior_state_set_with_resources(const char *state_id,
                                            const char *text,
                                            int font_size,
                                            const char *anim_id,
                                            const char *sound_id);
esp_err_t behavior_state_set_text(const char *text, int font_size);
const char *behavior_state_get_current(void);
bool behavior_state_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* BEHAVIOR_STATE_SERVICE_H */
