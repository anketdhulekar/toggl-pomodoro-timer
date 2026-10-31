#pragma once
#include <stdbool.h>

void net_init(void);      // joins the Wi-Fi network from secrets.h (no-op if none) and syncs the clock
bool net_configured(void); // a network is set in secrets.h
bool net_connected(void);  // has an IP address
bool net_time_ok(void);   // wall clock set by SNTP
int net_rssi(void);        // signal strength in dBm, 0 if not connected
void net_debug_enable(bool on); // debug: drop the connection and stay off (false), or reconnect (true)
