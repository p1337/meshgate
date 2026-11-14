#pragma once

#include <Arduino.h>
#include "config_ui.h"

void tailscale_net_start(const BridgeConfig& cfg);
void tailscale_net_pause_for_ota();
void tailscale_net_resume_after_ota();
bool tailscale_net_is_enabled();
bool tailscale_net_is_connected();
bool tailscale_net_is_derp_connected();
String tailscale_net_state_name();
String tailscale_net_vpn_ip();
String tailscale_net_auth_url();
int tailscale_net_peer_count();
String tailscale_net_peer_ip(int index);
String tailscale_net_peer_name(int index);
bool tailscale_net_peer_online(int index);
bool tailscale_net_peer_direct(int index);
void tailscale_net_factory_reset();
