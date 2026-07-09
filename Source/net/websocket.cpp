#include "websocket_native.hpp"

#include "websocket.h"
#include "../ui/network.h"
#include "../trace.h"

#include <chrono>
#include <thread>

const uint32_t SERVER_VERSION = 1;

namespace net {

// Connection is deferred until create() or join() so that g_ws_host is set
// correctly for joiners (who change it after the client object is created).
websocket_client::websocket_client() = default;

websocket_client::~websocket_client() = default;

// Connect to 'host' and send the client-info handshake packet.
static void connect_impl(std::unique_ptr<websocket_impl>& impl, const std::string& host) {
  impl = std::make_unique<websocket_impl>(host);
  uint32_t version = gdwProductVersion;
#ifdef SPAWN
  version |= 0x80000000;
#endif
  impl->send(client_info_packet(version));
}

void websocket_client::create(std::string name, std::string passwd, uint32_t difficulty) {
  cookie_     = (uint32_t) time(nullptr);
  is_creator_ = true;

  // Start the embedded server in this process so other players can connect
  // to our LAN IP on port 3001.  The websocket_server constructor binds and
  // calls listen() synchronously, so by the time connect_impl() returns the
  // server is already in LISTEN state and ready to accept.
  if (!server_) {
    server_ = std::make_unique<EmbeddedServer>();
    server_->start();
  }

  // Connect to our own embedded server.
  if (!impl_) {
    connect_impl(impl_, "127.0.0.1");
  }

  impl_->send(client_create_game_packet(cookie_, passwd, (uint32_t) difficulty));
}

void websocket_client::join(std::string name, std::string passwd) {
  cookie_     = (uint32_t) time(nullptr);
  is_creator_ = false;

  // g_ws_host was set to the host's LAN IP by the UI before calling join().
  // Connect there now — we do NOT start our own embedded server; we are a client.
  if (!impl_) {
    connect_impl(impl_, g_ws_host);
  }

  uint32_t code = (uint32_t) atoi(name.c_str());
  impl_->send(client_join_game_packet(cookie_, code, passwd));
}

void websocket_client::poll() {
  if (!impl_) return;
  impl_->receive([this](const uint8_t* data, size_t size) {
    handle_packet(data, size);
  });
  if (!closed_ && impl_->is_closed()) {
    closed_ = true;
    NetworkState* net_state = dynamic_cast<NetworkState*>(GameState::current());
    if (net_state) {
      net_state->onClosed();
    }
  }
}

void websocket_client::send(const packet& pkt) {
  if (impl_) impl_->send(pkt);
}

void websocket_client::handle_packet(const uint8_t* data, size_t size) {
  try {
    buffer_reader reader(data, size);
    NetworkState* net_state = dynamic_cast<NetworkState*>(GameState::current());
    int count = 1;
    if (data[0] == 0) {
      reader.read<uint8_t>();
      count = reader.read<uint16_t>();
    }
    while (count--) {
      PacketType type = reader.read<PacketType>();
      switch (type) {
      case PT_SERVER_INFO: {
        server_info_packet packet(reader);
        if (packet.version != SERVER_VERSION) {
          app_fatal("Server version mismatch. Expected %u, received %u.", SERVER_VERSION, packet.version);
        }
        break;
      }
      case PT_CODE_ASSIGN: {
        server_code_assign_packet packet(reader);
        if (net_state) {
          net_state->onCodeAssign(packet.code, packet.ip);
        }
        break;
      }
      case PT_JOIN_ACCEPT: {
        server_join_accept_packet packet(reader);
        if (packet.cookie == cookie_ && net_state) {
          handle(packet);
          net_state->onJoinAccept(packet.index, packet.init_info);
        } else {
          send(client_leave_game_packet());
        }
        break;
      }
      case PT_JOIN_REJECT: {
        server_join_reject_packet packet(reader);
        if (packet.cookie == cookie_ && net_state) {
          net_state->onJoinReject(packet.reason);
        }
        break;
      }
      case PT_CONNECT:
        handle(server_connect_packet(reader));
        break;
      case PT_DISCONNECT:
        handle(server_disconnect_packet(reader));
        break;
      case PT_MESSAGE:
        handle(server_message_packet(reader));
        break;
      case PT_TURN:
        handle(server_turn_packet(reader));
        break;
      default:
        app_fatal("Unrecognized server message type %d", (int) type);
      }
    }
    if (!reader.done()) {
      app_fatal("Invalid server message size");
    }
  } catch (parse_error) {
    app_fatal("Error parsing server message");
  }
}

} // namespace net

// -------------------------------------------------------
// LAN discovery — lives here so Winsock is already in scope
// (websocket_native.hpp is included first, before any game headers)
// -------------------------------------------------------
#include "lan_discovery.h"

std::vector<LanServer> scan_lan_servers(int timeout_ms)
{
  std::vector<LanServer> result;

  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) return result;

  int bcast = 1;
  ::setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&bcast, sizeof(bcast));

  // 100 ms per recv attempt, loop until timeout_ms total elapsed
  DWORD rv_to = 100;
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&rv_to, sizeof(rv_to));

  sockaddr_in local{};
  local.sin_family      = AF_INET;
  local.sin_port        = 0;
  local.sin_addr.s_addr = INADDR_ANY;
  ::bind(sock, (sockaddr*)&local, sizeof(local));

  sockaddr_in bcast_addr{};
  bcast_addr.sin_family      = AF_INET;
  bcast_addr.sin_port        = htons(LAN_DISCOVERY_PORT);
  bcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
  const char* msg = "DIABLO_DISCOVER";
  ::sendto(sock, msg, (int)strlen(msg), 0, (sockaddr*)&bcast_addr, sizeof(bcast_addr));

  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
    if (ms >= timeout_ms) break;

    char buf[512];
    sockaddr_in from{};
    int from_len = sizeof(from);
    int n = ::recvfrom(sock, buf, (int)sizeof(buf) - 1, 0,
                       (sockaddr*)&from, &from_len);
    if (n <= 0) {
      if (WSAGetLastError() == WSAETIMEDOUT) continue;
      break;
    }
    buf[n] = '\0';

    // Parse "DIABLO_SERVER:<ip>:<code1>,<code2>,..."
    const char* prefix = "DIABLO_SERVER:";
    size_t plen = strlen(prefix);
    std::string resp(buf);
    if (resp.size() <= plen || resp.substr(0, plen) != prefix) continue;

    resp = resp.substr(plen);
    size_t colon = resp.find(':');
    if (colon == std::string::npos) continue;

    LanServer srv;
    srv.ip = resp.substr(0, colon);
    std::string codes_str = resp.substr(colon + 1);

    size_t pos = 0;
    while (pos <= codes_str.size()) {
      size_t next = codes_str.find(',', pos);
      if (next == std::string::npos) next = codes_str.size();
      std::string tok = codes_str.substr(pos, next - pos);
      if (!tok.empty()) srv.codes.push_back((uint32_t)atoi(tok.c_str()));
      pos = next + 1;
    }

    bool dup = false;
    for (auto& s : result)
      if (s.ip == srv.ip) { dup = true; break; }
    if (!dup) result.push_back(std::move(srv));
  }

  ::closesocket(sock);
  return result;
}
