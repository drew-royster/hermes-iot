#pragma once

#include "esp_err.h"
#include "main.h"

typedef struct {
  char auth_token[256];
  char signaling_url[256];
  char conversation[128];
} GatewayBootstrapInfo;

typedef struct {
  char sdp[MAX_SIGNALING_SDP_BUFFER];
  char session_id[128];
  char conversation[128];
} GatewayOfferResponse;

bool gateway_client_is_configured(void);
esp_err_t gateway_client_health_check(void);
esp_err_t gateway_client_claim_device(GatewayBootstrapInfo *out_info);
esp_err_t gateway_client_post_offer(const GatewayBootstrapInfo *bootstrap,
                                    const char *offer_sdp,
                                    GatewayOfferResponse *out_response);
