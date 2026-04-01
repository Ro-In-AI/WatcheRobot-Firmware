#ifndef CONTROL_INGRESS_H
#define CONTROL_INGRESS_H

#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool has_x;
    bool has_y;
    int x_deg;
    int y_deg;
    int duration_ms;
} control_servo_request_t;

typedef struct {
    char status[64];
    char message[256];
    char image_name[64];
    char action_file[64];
    char sound_file[64];
} control_ai_status_request_t;

typedef struct {
    char state_id[32];
} control_state_set_request_t;

typedef struct {
    char state_id[32];
    char text[256];
    int font_size;
} control_state_text_request_t;

esp_err_t control_ingress_init(void);
esp_err_t control_ingress_submit_servo(const control_servo_request_t *req);
esp_err_t control_ingress_submit_ai_status(const control_ai_status_request_t *req);
esp_err_t control_ingress_submit_state_set(const control_state_set_request_t *req);
esp_err_t control_ingress_submit_state_text(const control_state_text_request_t *req);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_INGRESS_H */
