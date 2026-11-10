#include "network.h"

namespace {
#ifdef _WIN32
using AddressLength = int;
#else
using AddressLength = socklen_t;
#endif
int last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
bool interrupted(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}
void close_socket(SOCKET sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}
void nonblocking(SOCKET sock) {
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0)
        throw std::runtime_error("Unable to set nonblocking socket");
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("Unable to set nonblocking socket");
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
}
int select_socket(SOCKET sock, bool writing, int milliseconds) {
#ifndef _WIN32
    if (sock >= FD_SETSIZE) throw std::runtime_error("Socket exceeds select capacity");
#endif
    fd_set ready;
    fd_set exceptional;
    FD_ZERO(&ready);
    FD_ZERO(&exceptional);
    FD_SET(sock, &ready);
    FD_SET(sock, &exceptional);
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
#ifdef _WIN32
    return select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, &exceptional, &timeout);
#else
    return select(sock + 1, writing ? nullptr : &ready, writing ? &ready : nullptr, &exceptional, &timeout);
#endif
}
}

NetworkRuntime::NetworkRuntime() {
#ifdef _WIN32
    WSADATA wsadata{};
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
        throw std::runtime_error("Network initialization failed");
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
}
NetworkRuntime::~NetworkRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
}
Socket::Socket(SOCKET _sock) : sock(_sock) {
    try { nonblocking(sock); }
    catch (...) { close_socket(sock); throw; }
}
Socket::~Socket() {
    std::lock_guard<std::mutex> lock(socket_mutex);
    close_socket(sock);
}
void Socket::cancel() {
    std::lock_guard<std::mutex> lock(socket_mutex);
    cancelled = true;
#ifdef _WIN32
    shutdown(sock, SD_BOTH);
#else
    shutdown(sock, SHUT_RDWR);
#endif
}
void Socket::wait(bool writing, std::chrono::steady_clock::time_point deadline) {
    while (!cancelled && !stop_requested) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) throw std::runtime_error("Network operation timed out");
        int result = select_socket(sock, writing, static_cast<int>(std::min<int64_t>(left, 200)));
        if (result > 0) return;
        if (result < 0 && !interrupted(last_error()))
            throw std::runtime_error("Socket wait failed");
    }
    throw std::runtime_error("Connection cancelled");
}
bool socket_readable(SOCKET sock, int milliseconds) {
    int result = select_socket(sock, false, milliseconds);
    if (result < 0 && !interrupted(last_error()))
        throw std::runtime_error("Socket wait failed");
    return result > 0;
}
SocketPtr connect_server(const std::string& ip, int port, int timeout) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    std::string port_string = std::to_string(port);
    if (getaddrinfo(ip.c_str(), port_string.c_str(), &hints, &addresses) != 0)
        throw std::runtime_error("Unable to resolve server address");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(addresses, freeaddrinfo);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    for (auto address = addresses; address; address = address->ai_next) {
        SOCKET raw = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (raw == INVALID_SOCKET) continue;
        auto sock = std::make_shared<Socket>(raw);
        int result = connect(raw, address->ai_addr, static_cast<AddressLength>(address->ai_addrlen));
        if (result == 0) return sock;
        int error = last_error();
#ifdef _WIN32
        bool pending = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
        bool pending = error == EINPROGRESS;
#endif
        if (!pending) continue;
        sock->wait(true, deadline);
        int connect_error = 0;
#ifdef _WIN32
        int length = sizeof(connect_error);
#else
        socklen_t length = sizeof(connect_error);
#endif
        if (getsockopt(raw, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connect_error), &length) == 0
            && connect_error == 0) return sock;
    }
    throw std::runtime_error("Unable to connect to server");
}
SocketPtr listen_server(const std::string& ip, int port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    std::string port_string = std::to_string(port);
    if (getaddrinfo(ip.c_str(), port_string.c_str(), &hints, &addresses) != 0)
        throw std::runtime_error("Invalid listen address");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(addresses, freeaddrinfo);
    for (auto address = addresses; address; address = address->ai_next) {
        SOCKET raw = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (raw == INVALID_SOCKET) continue;
        auto sock = std::make_shared<Socket>(raw);
        int one = 1;
#ifdef _WIN32
        setsockopt(raw, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
        setsockopt(raw, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
        if (bind(raw, address->ai_addr, static_cast<AddressLength>(address->ai_addrlen)) == 0 && listen(raw, 32) == 0)
            return sock;
    }
    throw std::runtime_error("Unable to bind listening socket");
}
SocketPtr accept_client(const SocketPtr& listener) {
    SOCKET raw = accept(listener->native(), nullptr, nullptr);
    if (raw == INVALID_SOCKET) return {};
    return std::make_shared<Socket>(raw);
}
std::string client_address(SOCKET sock) {
    sockaddr_storage address{};
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getpeername(sock, reinterpret_cast<sockaddr*>(&address), &length) != 0) return "unknown";
    char host[NI_MAXHOST]{};
    char service[NI_MAXSERV]{};
    if (getnameinfo(reinterpret_cast<sockaddr*>(&address), static_cast<AddressLength>(length),
        host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) return "unknown";
    return std::string(host) + ":" + service;
}
