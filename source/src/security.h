#pragma once
#include "network.h"
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/rand.h>

using PublicKey = std::array<unsigned char, 32>;
void generate_identity(const fs::path& private_path, const fs::path& public_path);
PublicKey read_public_key(const fs::path& path);
std::string fingerprint(const PublicKey& key);

class SecurityContext {
private:
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx{nullptr, SSL_CTX_free};
    std::vector<PublicKey> trusted_keys;
    static int verify_peer(X509_STORE_CTX* store, void* arg) noexcept;
public:
    SecurityContext(bool server, const fs::path& private_path,
        const fs::path& peer_path, const fs::path& trust_dir);
    SecurityContext(const SecurityContext&) = delete;
    SecurityContext& operator=(const SecurityContext&) = delete;
    SecurityContext(SecurityContext&&) = delete;
    SecurityContext& operator=(SecurityContext&&) = delete;
    SSL_CTX* native() const { return ctx.get(); }
};

class SecureChannel {
private:
    SocketPtr sock;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl{nullptr, SSL_free};
    int timeout;
    uint64_t bytes_sent = 0;
    void wait_for_ssl(int result, std::chrono::steady_clock::time_point deadline);
public:
    SecureChannel(SecurityContext& context, SocketPtr _sock, bool server, int _timeout);
    ~SecureChannel();
    void read_all(unsigned char* output, size_t length);
    void write_all(const unsigned char* input, size_t length);
    void update_keys();
    std::string peer_fingerprint() const;
    void cancel() { sock->cancel(); }
};
