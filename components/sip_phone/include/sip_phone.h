#pragma once

#include "esp_err.h"

typedef enum {
    SIP_PHONE_STATE_OFFLINE,
    SIP_PHONE_STATE_REGISTERING,
    SIP_PHONE_STATE_REGISTERED,
    SIP_PHONE_STATE_CALLING,
    SIP_PHONE_STATE_RINGING,
    SIP_PHONE_STATE_IN_CALL,
    SIP_PHONE_STATE_ERROR,
} sip_phone_state_t;

esp_err_t sip_phone_init(void);
sip_phone_state_t sip_phone_get_state(void);
esp_err_t sip_phone_start_registration(void);
esp_err_t sip_phone_answer(void);
esp_err_t sip_phone_hangup(void);
esp_err_t sip_phone_call_default_extension(void);
