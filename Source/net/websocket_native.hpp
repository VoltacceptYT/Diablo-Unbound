#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "../appfat.h"
#include "../../Server/packet.hpp"

// Receive exactly 'len' bytes; returns bytes read (< len on disconnect)
static int ws_recv_all(SOCKET s, uint8_t* buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    int n = ::recv(s, (char*)buf + total, (int)(len - total), 0);
    if (n <= 0) return (int)total;
    total += n;
  }
  return (int)total;
}

// Minimal base64 encoding for WebSocket handshake key
static const char ws_b64chars[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string ws_base64(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = ((uint32_t)data[i] << 16);
    if (i + 1 < len) n |= ((uint32_t)data[i + 1] << 8);
    if (i + 2 < len) n |=  (uint32_t)data[i + 2];
    out += ws_b64chars[(n >> 18) & 63];
    out += ws_b64chars[(n >> 12) & 63];
    out += (i + 1 < len) ? ws_b64chars[(n >>  6) & 63] : '=';
    out += (i + 2 < len) ? ws_b64chars[ n        & 63] : '=';
  }
  return out;
}

class websocket_impl {
public:
  websocket_impl(const std::string& host, unsigned short port = 3001) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET) {
      app_fatal("WebSocket: failed to create socket (%d)", WSAGetLastError());
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
      app_fatal("WebSocket: invalid address '%s'", host.c_str());
    }

    if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
      app_fatal("WebSocket: connect to %s:%d failed (%d). Is server.js running?",
                host.c_str(), port, WSAGetLastError());
    }

    // WebSocket HTTP upgrade handshake
    uint8_t raw_key[16];
    srand((unsigned)time(nullptr) ^ (unsigned)(uintptr_t)this);
    for (int i = 0; i < 16; ++i) raw_key[i] = (uint8_t)(rand() & 0xFF);
    std::string key = ws_base64(raw_key, 16);

    std::string hs =
      "GET / HTTP/1.1\r\n"
      "Host: " + host + ":" + std::to_string(port) + "\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: " + key + "\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n";
    ::send(sock_, hs.c_str(), (int)hs.size(), 0);

    // Read HTTP response (wait for blank line)
    std::string resp;
    char c;
    while (true) {
      if (::recv(sock_, &c, 1, 0) != 1) {
        app_fatal("WebSocket: handshake recv failed");
      }
      resp += c;
      if (resp.size() >= 4 &&
          resp[resp.size()-4] == '\r' && resp[resp.size()-3] == '\n' &&
          resp[resp.size()-2] == '\r' && resp[resp.size()-1] == '\n') {
        break;
      }
    }
    if (resp.find("101") == std::string::npos) {
      app_fatal("WebSocket: upgrade failed: %.200s", resp.c_str());
    }

    recv_thread_ = std::thread([this]() { recv_loop(); });
  }

  ~websocket_impl() {
    closed_ = true;
    ::closesocket(sock_);
    if (recv_thread_.joinable()) recv_thread_.join();
    WSACleanup();
  }

  void send(const packet& p) {
    size_t len = p.size();
    std::vector<uint8_t> payload(len);
    p.serialize(payload.data());
    send_binary(payload.data(), len);
  }

  void receive(std::function<void(const uint8_t*, size_t)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!input_.empty()) {
      auto& buf = input_.front();
      handler(buf.data(), buf.size());
      input_.pop();
    }
  }

  bool is_closed() const { return closed_; }

private:
  SOCKET sock_ = INVALID_SOCKET;
  std::thread recv_thread_;
  std::mutex mutex_;
  std::queue<std::vector<uint8_t>> input_;
  std::atomic<bool> closed_{false};

  void send_binary(const uint8_t* data, size_t len) {
    // Build frame header
    std::vector<uint8_t> frame;
    frame.push_back(0x82); // FIN + binary opcode

    if (len < 126) {
      frame.push_back(0x80 | (uint8_t)len);
    } else if (len < 65536) {
      frame.push_back(0x80 | 126);
      frame.push_back((uint8_t)(len >> 8));
      frame.push_back((uint8_t)(len & 0xFF));
    } else {
      frame.push_back(0x80 | 127);
      for (int i = 7; i >= 0; --i)
        frame.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
    }

    // Masking key (client->server frames must be masked per RFC 6455)
    uint8_t mask[4];
    mask[0] = (uint8_t)(rand() & 0xFF);
    mask[1] = (uint8_t)(rand() & 0xFF);
    mask[2] = (uint8_t)(rand() & 0xFF);
    mask[3] = (uint8_t)(rand() & 0xFF);
    for (int i = 0; i < 4; ++i) frame.push_back(mask[i]);

    // Masked payload
    for (size_t i = 0; i < len; ++i)
      frame.push_back(data[i] ^ mask[i % 4]);

    ::send(sock_, (char*)frame.data(), (int)frame.size(), 0);
  }

  void recv_loop() {
    while (!closed_) {
      uint8_t hdr[2];
      if (ws_recv_all(sock_, hdr, 2) != 2) { closed_ = true; return; }

      uint8_t opcode    = hdr[0] & 0x0F;
      bool    is_masked = (hdr[1] & 0x80) != 0;
      uint64_t plen     = hdr[1] & 0x7F;

      if (plen == 126) {
        uint8_t ext[2];
        if (ws_recv_all(sock_, ext, 2) != 2) { closed_ = true; return; }
        plen = ((uint16_t)ext[0] << 8) | ext[1];
      } else if (plen == 127) {
        uint8_t ext[8];
        if (ws_recv_all(sock_, ext, 8) != 8) { closed_ = true; return; }
        plen = 0;
        for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
      }

      uint8_t mask[4] = {};
      if (is_masked) {
        if (ws_recv_all(sock_, mask, 4) != 4) { closed_ = true; return; }
      }

      std::vector<uint8_t> payload((size_t)plen);
      if (plen > 0) {
        if (ws_recv_all(sock_, payload.data(), (size_t)plen) != (int)plen) {
          closed_ = true; return;
        }
        if (is_masked) {
          for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= mask[i % 4];
        }
      }

      if (opcode == 0x8) { closed_ = true; return; } // Close frame
      if (opcode == 0x9) {                            // Ping: send pong
        uint8_t pong[2] = { 0x8A, 0x00 };
        ::send(sock_, (char*)pong, 2, 0);
        continue;
      }
      // Binary (0x2), text (0x1), continuation (0x0)
      if (opcode <= 0x2) {
        std::lock_guard<std::mutex> lock(mutex_);
        input_.push(std::move(payload));
      }
    }
  }
};
