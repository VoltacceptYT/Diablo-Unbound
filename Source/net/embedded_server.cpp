// embedded_server.cpp — WinSock2-based relay server (no Boost / no external deps)
//
// Implements the same wire protocol as the old server.js:
//   TCP port 3001 — WebSocket relay (create/join/message/turn)
//   UDP port 3002 — LAN discovery responder

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "embedded_server.hpp"
#include "../../Server/packet.hpp"

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr uint16_t ES_WS_PORT    = 3001;
static constexpr uint16_t ES_UDP_PORT   = 3002;
static constexpr uint32_t ES_SERVER_VER = 1;
static constexpr int      ES_MAX_PLAYERS = 4;

// ---------------------------------------------------------------------------
// Random 6-digit game-code generator.
//
// Uses its own std::mt19937 seeded from std::random_device instead of the
// global libc rand()/srand() pair. This avoids any dependency on when (or
// whether) srand() has been called elsewhere in the process, and avoids
// producing the same/similar first draw across quick relaunches, which is
// what caused every hosted game to get the same code.
// ---------------------------------------------------------------------------
static uint32_t es_random_code() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    static thread_local std::uniform_int_distribution<uint32_t> dist(100000, 999999);
    return dist(rng);
}

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------
static int es_recv_all(SOCKET s, uint8_t* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int n = ::recv(s, (char*)buf + total, (int)(len - total), 0);
        if (n <= 0) return (int)total;
        total += (size_t)n;
    }
    return (int)total;
}

// ---- Minimal SHA-1 (RFC 3174) used for the WebSocket handshake ----------
static void es_sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu,
             h3 = 0x10325476u, h4 = 0xC3D2E1F0u;

    // Pre-processing: pad to a multiple of 64 bytes
    size_t plen = ((len + 9 + 63) / 64) * 64;
    std::vector<uint8_t> msg(plen, 0);
    memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i)
        msg[plen - 1 - i] = (uint8_t)(bits >> (i * 8));

    auto rol32 = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

    for (size_t blk = 0; blk < plen; blk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)msg[blk+i*4]   << 24) | ((uint32_t)msg[blk+i*4+1] << 16)
                 | ((uint32_t)msg[blk+i*4+2] <<  8) |  (uint32_t)msg[blk+i*4+3];
        for (int i = 16; i < 80; ++i) {
            uint32_t x = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = rol32(x, 1);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | (~b & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b&c)|(b&d)|(c&d);           k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    auto wb = [&](int off, uint32_t v) {
        out[off]   = (uint8_t)(v >> 24); out[off+1] = (uint8_t)(v >> 16);
        out[off+2] = (uint8_t)(v >>  8); out[off+3] = (uint8_t) v;
    };
    wb(0,h0); wb(4,h1); wb(8,h2); wb(12,h3); wb(16,h4);
}

// ---- Base64 (same table as websocket_native.hpp) -------------------------
static const char ES_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string es_base64(const uint8_t* d, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)d[i] << 16;
        if (i+1 < len) n |= (uint32_t)d[i+1] << 8;
        if (i+2 < len) n |=  (uint32_t)d[i+2];
        out += ES_B64[(n>>18)&63]; out += ES_B64[(n>>12)&63];
        out += (i+1 < len) ? ES_B64[(n>>6)&63] : '=';
        out += (i+2 < len) ? ES_B64[ n    &63] : '=';
    }
    return out;
}

// ---- Derive the Sec-WebSocket-Accept response value ---------------------
static std::string es_ws_accept(const std::string& key) {
    std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    es_sha1((const uint8_t*)s.data(), s.size(), digest);
    return es_base64(digest, 20);
}

// ---- Detect host LAN IP (UDP connect trick — no packets sent) -----------
static std::string es_get_lan_ip() {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "127.0.0.1";
    sockaddr_in dest{}; dest.sin_family = AF_INET; dest.sin_port = htons(80);
    ::inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr);
    ::connect(s, (sockaddr*)&dest, sizeof(dest));
    sockaddr_in local{}; int sz = sizeof(local);
    ::getsockname(s, (sockaddr*)&local, &sz);
    ::closesocket(s);
    char buf[INET_ADDRSTRLEN] = "127.0.0.1";
    ::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
    return buf;
}

// ---- Perform HTTP→WebSocket upgrade handshake (server side) -------------
// Returns true and leaves sock ready for frame I/O; false on error.
static bool es_ws_handshake(SOCKET s) {
    // Read headers until \r\n\r\n
    std::string hdrs;
    hdrs.reserve(512);
    char c;
    while (true) {
        if (::recv(s, &c, 1, 0) != 1) return false;
        hdrs += c;
        size_t n = hdrs.size();
        if (n >= 4 && hdrs[n-4]=='\r' && hdrs[n-3]=='\n'
                   && hdrs[n-2]=='\r' && hdrs[n-1]=='\n') break;
        if (n > 8192) return false; // DOS guard
    }
    // Extract Sec-WebSocket-Key
    const char* tag = "Sec-WebSocket-Key: ";
    size_t pos = hdrs.find(tag);
    if (pos == std::string::npos) return false;
    pos += strlen(tag);
    size_t end = hdrs.find("\r\n", pos);
    if (end == std::string::npos) return false;
    std::string key = hdrs.substr(pos, end - pos);

    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + es_ws_accept(key) + "\r\n"
        "\r\n";
    return ::send(s, resp.c_str(), (int)resp.size(), 0) == (int)resp.size();
}

// ---- Read one WebSocket frame (client→server frames are masked) ---------
static bool es_read_frame(SOCKET s, std::vector<uint8_t>& out) {
    for (;;) {
        uint8_t hdr[2];
        if (es_recv_all(s, hdr, 2) != 2) return false;
        uint8_t opcode   = hdr[0] & 0x0F;
        bool    is_masked = (hdr[1] & 0x80) != 0;
        uint64_t plen    = hdr[1] & 0x7F;
        if (plen == 126) {
            uint8_t ext[2];
            if (es_recv_all(s, ext, 2) != 2) return false;
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            uint8_t ext[8];
            if (es_recv_all(s, ext, 8) != 8) return false;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
        }
        uint8_t mask[4] = {};
        if (is_masked && es_recv_all(s, mask, 4) != 4) return false;
        out.resize((size_t)plen);
        if (plen > 0 && es_recv_all(s, out.data(), (size_t)plen) != (int)plen)
            return false;
        if (is_masked)
            for (size_t i = 0; i < out.size(); ++i) out[i] ^= mask[i%4];
        if (opcode == 0x8) return false; // close
        if (opcode == 0x9) {             // ping → pong
            uint8_t pong[2] = { 0x8A, 0x00 };
            ::send(s, (char*)pong, 2, 0);
            continue;
        }
        return true;
    }
}

// ---- Write one WebSocket binary frame (server→client; NOT masked) -------
static void es_write_frame(SOCKET s, std::mutex& mu,
                           const uint8_t* data, size_t len) {
    std::vector<uint8_t> frame;
    frame.reserve(10 + len);
    frame.push_back(0x82); // FIN + binary
    if (len < 126) {
        frame.push_back((uint8_t)len);
    } else if (len < 65536) {
        frame.push_back(126);
        frame.push_back((uint8_t)(len >> 8));
        frame.push_back((uint8_t)(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) frame.push_back((uint8_t)((len >> (i*8)) & 0xFF));
    }
    for (size_t i = 0; i < len; ++i) frame.push_back(data[i]);
    std::lock_guard<std::mutex> lk(mu);
    ::send(s, (char*)frame.data(), (int)frame.size(), 0);
}

// ---- Serialize a packet and send it as a WebSocket frame ----------------
static void es_send_pkt(SOCKET s, std::mutex& mu, const packet& pkt) {
    std::vector<uint8_t> buf(pkt.size());
    pkt.serialize(buf.data());
    es_write_frame(s, mu, buf.data(), buf.size());
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
struct EsGame;

// ---------------------------------------------------------------------------
// EsConn — one connected WebSocket client (one player slot in a game)
// ---------------------------------------------------------------------------
struct EsConn {
    SOCKET sock = INVALID_SOCKET;
    std::mutex send_mu;
    std::atomic<bool> closed{false};
    uint8_t id = 0xFF;
    std::weak_ptr<EsGame> game;

    void send(const packet& pkt) {
        if (!closed) es_send_pkt(sock, send_mu, pkt);
    }
    void close_sock() {
        closed = true;
        ::shutdown(sock, SD_BOTH);
    }
};

// ---------------------------------------------------------------------------
// EsGame — one game session (up to ES_MAX_PLAYERS players)
// ---------------------------------------------------------------------------
struct EsGame {
    uint32_t    code       = 0;
    std::string password;
    uint32_t    difficulty = 0;
    uint32_t    seed       = 0;
    std::mutex  mu;
    std::shared_ptr<EsConn> players[ES_MAX_PLAYERS]; // indexed by player id

    // Send pkt to every player whose id != exclude_id
    void broadcast(const packet& pkt, uint8_t exclude_id = 0xFF) {
        std::lock_guard<std::mutex> lk(mu);
        for (int i = 0; i < ES_MAX_PLAYERS; ++i)
            if (players[i] && i != exclude_id)
                players[i]->send(pkt);
    }

    // Send pkt to one specific player
    void send_to(uint8_t id, const packet& pkt) {
        std::lock_guard<std::mutex> lk(mu);
        if (id < ES_MAX_PLAYERS && players[id])
            players[id]->send(pkt);
    }

    // Returns true if no players remain
    bool empty() {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& p : players) if (p) return false;
        return true;
    }

    // Remove player 'id' and notify others.
    // IMPORTANT: game lock is released before closing the conn so we never
    // hold game lock while waiting on the player's send_mu (avoids inversion).
    void drop_player(uint8_t id, LeaveReason reason) {
        std::shared_ptr<EsConn> leaving;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (id >= ES_MAX_PLAYERS || !players[id]) return;
            leaving = std::move(players[id]);
            for (int i = 0; i < ES_MAX_PLAYERS; ++i)
                if (players[i])
                    players[i]->send(server_disconnect_packet(id, reason));
        }
        // Game lock released — safe to mutate the conn
        if (leaving) {
            leaving->id   = 0xFF;
            leaving->game.reset();
            leaving->close_sock();
        }
    }
};

// ---------------------------------------------------------------------------
// EmbeddedServer::Impl
// ---------------------------------------------------------------------------
struct EmbeddedServer::Impl {
    SOCKET      acceptor_sock = INVALID_SOCKET;
    SOCKET      udp_sock      = INVALID_SOCKET;
    std::thread acceptor_thread;
    std::thread udp_thread;
    std::atomic<bool> running{false};

    std::mutex games_mu;
    std::map<uint32_t, std::shared_ptr<EsGame>> games;

    std::string lan_ip;

    // ------------------------------------------------------------------ start
    void start() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        lan_ip  = es_get_lan_ip();
        running = true;

        // TCP acceptor
        acceptor_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int yes = 1;
        ::setsockopt(acceptor_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(ES_WS_PORT);
        addr.sin_addr.s_addr = INADDR_ANY;
        ::bind(acceptor_sock, (sockaddr*)&addr, sizeof(addr));
        ::listen(acceptor_sock, SOMAXCONN);

        // UDP discovery
        udp_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in ua{};
        ua.sin_family      = AF_INET;
        ua.sin_port        = htons(ES_UDP_PORT);
        ua.sin_addr.s_addr = INADDR_ANY;
        ::bind(udp_sock, (sockaddr*)&ua, sizeof(ua));

        acceptor_thread = std::thread([this]{ accept_loop(); });
        udp_thread      = std::thread([this]{ udp_loop();    });
    }

    // ------------------------------------------------------------------ stop
    void stop() {
        running = false;
        if (acceptor_sock != INVALID_SOCKET) {
            ::closesocket(acceptor_sock);
            acceptor_sock = INVALID_SOCKET;
        }
        if (udp_sock != INVALID_SOCKET) {
            ::closesocket(udp_sock);
            udp_sock = INVALID_SOCKET;
        }
        if (acceptor_thread.joinable()) acceptor_thread.join();
        if (udp_thread.joinable())      udp_thread.join();
        WSACleanup();
    }

    // ------------------------------------------------------------------ accept_loop
    void accept_loop() {
        while (running) {
            SOCKET client = ::accept(acceptor_sock, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            // Detach: each client runs in its own thread
            std::thread([this, client]{ handle_client(client); }).detach();
        }
    }

    // ------------------------------------------------------------------ udp_loop
    void udp_loop() {
        // Timeout recv so we can check `running`
        DWORD to = 1000;
        ::setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

        while (running) {
            char buf[256];
            sockaddr_in from{}; int from_len = sizeof(from);
            int n = ::recvfrom(udp_sock, buf, sizeof(buf)-1, 0,
                               (sockaddr*)&from, &from_len);
            if (n <= 0) continue;
            buf[n] = '\0';
            if (strcmp(buf, "DIABLO_DISCOVER") != 0) continue;

            // Reply: "DIABLO_SERVER:<ip>:<code1>,<code2>,..."
            std::string codes;
            {
                std::lock_guard<std::mutex> lk(games_mu);
                for (auto& [code, _] : games) {
                    if (!codes.empty()) codes += ',';
                    codes += std::to_string(code);
                }
            }
            std::string reply = "DIABLO_SERVER:" + lan_ip + ":" + codes;
            ::sendto(udp_sock, reply.c_str(), (int)reply.size(), 0,
                     (sockaddr*)&from, from_len);
        }
    }

    // ------------------------------------------------------------------ handle_client
    void handle_client(SOCKET s) {
        // WebSocket upgrade
        if (!es_ws_handshake(s)) { ::closesocket(s); return; }

        // Set up a conn object shared with the game
        auto conn = std::make_shared<EsConn>();
        conn->sock = s;

        std::vector<uint8_t> frame;

        // 1. Expect PT_CLIENT_INFO
        if (!es_read_frame(s, frame) || frame.empty()) {
            ::closesocket(s); return;
        }
        if (frame[0] != PT_CLIENT_INFO) { ::closesocket(s); return; }
        // (version check omitted for simplicity — client checks server version)
        conn->send(server_info_packet(ES_SERVER_VER));

        // 2. Expect PT_CREATE_GAME or PT_JOIN_GAME
        if (!es_read_frame(s, frame) || frame.empty()) {
            ::closesocket(s); return;
        }

        std::shared_ptr<EsGame> game;

        if (frame[0] == PT_CREATE_GAME) {
            buffer_reader r(frame.data() + 1, frame.size() - 1);
            client_create_game_packet pkt(r);

            game = std::make_shared<EsGame>();
            game->difficulty = pkt.difficulty;
            game->password   = pkt.password;
            game->seed       = (uint32_t)time(nullptr) ^ (uint32_t)(uintptr_t)game.get();
            game->players[0] = conn;
            conn->id         = 0;
            conn->game       = game;

            // Generate a unique 6-digit code
            {
                std::lock_guard<std::mutex> lk(games_mu);
                do { game->code = es_random_code(); }
                while (games.count(game->code));
                games[game->code] = game;
            }

            // init_info: lower 32 = seed, upper 32 = difficulty
            uint64_t init_info = (uint64_t)game->seed
                               | ((uint64_t)game->difficulty << 32);
            conn->send(server_join_accept_packet(pkt.cookie, 0, init_info));
            conn->send(server_code_assign_packet(game->code, lan_ip));

        } else if (frame[0] == PT_JOIN_GAME) {
            buffer_reader r(frame.data() + 1, frame.size() - 1);
            client_join_game_packet pkt(r);

            std::lock_guard<std::mutex> lk(games_mu);
            auto it = games.find(pkt.code);
            if (it == games.end()) {
                conn->send(server_join_reject_packet(pkt.cookie, JOIN_GAME_NOT_FOUND));
                ::closesocket(s); return;
            }
            game = it->second;
            std::lock_guard<std::mutex> glk(game->mu);
            if (game->password != pkt.password) {
                conn->send(server_join_reject_packet(pkt.cookie, JOIN_INCORRECT_PASSWORD));
                ::closesocket(s); return;
            }
            // Find a free slot (slots 1-3)
            int slot = -1;
            for (int i = 1; i < ES_MAX_PLAYERS; ++i)
                if (!game->players[i]) { slot = i; break; }
            if (slot < 0) {
                conn->send(server_join_reject_packet(pkt.cookie, JOIN_GAME_FULL));
                ::closesocket(s); return;
            }
            game->players[slot] = conn;
            conn->id   = (uint8_t)slot;
            conn->game = game;

            // Notify existing players of new joiner
            for (int i = 0; i < ES_MAX_PLAYERS; ++i)
                if (game->players[i] && i != slot)
                    game->players[i]->send(server_connect_packet((uint8_t)slot));

            uint64_t init_info = (uint64_t)game->seed
                               | ((uint64_t)game->difficulty << 32);
            conn->send(server_join_accept_packet(pkt.cookie, (uint8_t)slot, init_info));

        } else {
            ::closesocket(s); return;
        }

        // 3. Relay loop
        relay_loop(conn, game);

        // 4. Clean up: drop this player if still in the game
        if (conn->id != 0xFF && !conn->closed) {
            game->drop_player(conn->id, LEAVE_DROP);
        }
        if (game->empty()) {
            std::lock_guard<std::mutex> lk(games_mu);
            games.erase(game->code);
        }
        ::closesocket(s);
    }

    // ------------------------------------------------------------------ relay_loop
    void relay_loop(std::shared_ptr<EsConn> conn,
                    std::shared_ptr<EsGame> game) {
        std::vector<uint8_t> frame;
        while (!conn->closed && es_read_frame(conn->sock, frame)) {
            if (frame.empty()) continue;
            PacketType type = (PacketType)frame[0];
            buffer_reader r(frame.data() + 1, frame.size() - 1);

            switch (type) {
            case PT_MESSAGE: {
                // client_message_packet: id=dest, payload
                uint8_t dest = r.read<uint8_t>();
                std::vector<uint8_t> payload; r.read(payload);
                server_message_packet fwd(conn->id, std::move(payload));
                if (dest == 0xFF)
                    game->broadcast(fwd, conn->id);
                else
                    game->send_to(dest, fwd);
                break;
            }
            case PT_TURN: {
                // client_turn_packet: turn (uint32)
                uint32_t turn = r.read<uint32_t>();
                game->broadcast(server_turn_packet(conn->id, turn), conn->id);
                break;
            }
            case PT_DROP_PLAYER: {
                // client_drop_player_packet: id (uint8), reason (uint32)
                uint8_t    drop_id = r.read<uint8_t>();
                LeaveReason reason  = r.read<LeaveReason>();
                game->drop_player(drop_id, reason);
                break;
            }
            case PT_LEAVE_GAME:
                return; // triggers drop in handle_client
            default:
                break;  // ignore unknown client packets
            }
        }
    }
};

// ---------------------------------------------------------------------------
// EmbeddedServer public interface
// ---------------------------------------------------------------------------
EmbeddedServer::EmbeddedServer()  : impl_(std::make_unique<Impl>()) {}
EmbeddedServer::~EmbeddedServer() { impl_->stop(); }
void EmbeddedServer::start()      { impl_->start(); }
void EmbeddedServer::stop()       { impl_->stop(); }