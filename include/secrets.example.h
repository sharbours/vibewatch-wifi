#pragma once
// Copy this file to include/secrets.h and fill in your values.
// include/secrets.h is git-ignored so your Wi-Fi password never gets committed.

#define VIBE_WIFI_SSID     "YourWiFiName"
#define VIBE_WIFI_PASSWORD "YourWiFiPassword"

// Where hermes_bridge.py is running. If you run the bridge inside the same
// Proxmox container as Hermes Agent, this is the container's IP.
#define VIBE_BRIDGE_HOST   "192.168.0.197"
#define VIBE_BRIDGE_PORT   8765

// Shared secret. Must match WATCH_TOKEN in the bridge's .env file.
#define VIBE_BRIDGE_TOKEN  "change-me-to-a-long-random-string"
