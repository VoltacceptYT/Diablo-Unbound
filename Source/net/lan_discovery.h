#pragma once

#include <string>
#include <vector>
#include <cstdint>

#define LAN_DISCOVERY_PORT  3002

struct LanServer {
  std::string ip;
  std::vector<uint32_t> codes; // available 6-digit game codes
};

// Broadcasts UDP "DIABLO_DISCOVER" on the LAN and collects server responses
// for timeout_ms milliseconds. Returns all discovered servers.
std::vector<LanServer> scan_lan_servers(int timeout_ms = 2000);
