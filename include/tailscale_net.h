#pragma once

#include <Arduino.h>
#include "config_ui.h"

void tailscale_net_start(const BridgeConfig& cfg);
bool tailscale_net_is_enabled();
bool tailscale_net_is_connected();
String tailscale_net_state_name();
String tailscale_net_vpn_ip();
int tailscale_net_peer_count();
void tailscale_net_factory_reset();
