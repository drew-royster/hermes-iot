#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "gateway_client.h"

esp_err_t device_identity_load(GatewayBootstrapInfo *out_info);
esp_err_t device_identity_save(const GatewayBootstrapInfo *info);
esp_err_t device_identity_clear(void);
bool device_identity_is_saved(void);
