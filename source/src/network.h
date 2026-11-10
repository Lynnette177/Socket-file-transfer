#pragma once
#include "includes.h"
extern std::atomic<bool> stop_requested;

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};

// Socket 的关闭由所有者负责；其他线程只 shutdown，避免句柄复用竞争。
class Socket {
private:
    SOCKET sock;
    std::mutex socket_mutex;
    std::atomic<bool> cancelled{false};
public:
    explicit Socket(SOCKET _sock);
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    SOCKET native() const { return sock; }
    void cancel();
    void wait(bool writing, std::chrono::steady_clock::time_point deadline);
};

using SocketPtr = std::shared_ptr<Socket>;
SocketPtr connect_server(const std::string& ip, int port, int timeout);
SocketPtr listen_server(const std::string& ip, int port);
SocketPtr accept_client(const SocketPtr& listener);
bool socket_readable(SOCKET sock, int milliseconds);
std::string client_address(SOCKET sock);
