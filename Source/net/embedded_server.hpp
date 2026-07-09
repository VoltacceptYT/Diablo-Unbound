#pragma once
#include <memory>

// ---------------------------------------------------------------------------
// EmbeddedServer — in-process WebSocket relay server (WinSock2, no Boost)
//
// When the local player creates a multiplayer game, their application starts
// this server so other players can connect directly to their LAN IP on
// port 3001.  No external Node.js process is required.
// ---------------------------------------------------------------------------
class EmbeddedServer {
public:
    EmbeddedServer();
    ~EmbeddedServer();
    void start(); // bind + begin accepting; returns immediately
    void stop();  // close acceptor; in-flight client threads finish naturally
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
