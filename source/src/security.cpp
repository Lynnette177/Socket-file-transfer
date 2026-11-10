#include "security.h"
#include "storage.h"

namespace {
template<class T, void (*Free)(T*)> using OpenSSLPtr = std::unique_ptr<T, decltype(Free)>;
[[noreturn]] void crypto_error(const std::string& text) {
    unsigned long error = ERR_get_error();
    char buffer[256]{};
    if (error) ERR_error_string_n(error, buffer, sizeof(buffer));
    throw std::runtime_error(text + (error ? std::string(": ") + buffer : ""));
}
void require(long result, const char* text) { if (result <= 0) crypto_error(text); }
PublicKey raw_public_key(EVP_PKEY* key) {
    PublicKey output{};
    size_t size = output.size();
    if (!key || EVP_PKEY_is_a(key, "ED25519") != 1 ||
        EVP_PKEY_get_raw_public_key(key, output.data(), &size) != 1 || size != output.size())
        throw std::runtime_error("Expected an Ed25519 public key");
    return output;
}
OpenSSLPtr<BIO, BIO_free_all> read_pem(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to open key: " + pathString(path));
    input.seekg(0, std::ios::end);
    auto size = input.tellg();
    if (size <= 0 || size > 65536) throw std::runtime_error("Invalid PEM key size");
    input.seekg(0);
    std::string data(static_cast<size_t>(size), '\0');
    input.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!input) throw std::runtime_error("Unable to read key");
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) crypto_error("BIO allocation failed");
    OpenSSLPtr<BIO, BIO_free_all> result(bio, BIO_free_all);
    int written = BIO_write(bio, data.data(), static_cast<int>(data.size()));
    OPENSSL_cleanse(data.data(), data.size());
    if (written != size) crypto_error("BIO write failed");
    return result;
}
std::vector<unsigned char> pem_bytes(BIO* bio) {
    char* data = nullptr;
    long size = BIO_get_mem_data(bio, &data);
    if (size <= 0) crypto_error("Empty PEM output");
    return {data, data + size};
}
int no_password(char*, int, int, void*) { return 0; }
int select_alpn(SSL*, const unsigned char** output, unsigned char* output_length,
    const unsigned char* input, unsigned int length, void*) {
    const unsigned char expected[] = {5, 's', 'f', 't', '/', '1'};
    if (length != sizeof(expected) || CRYPTO_memcmp(input, expected, sizeof(expected)) != 0)
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    *output = input + 1;
    *output_length = 5;
    return SSL_TLSEXT_ERR_OK;
}
}

PublicKey read_public_key(const fs::path& path) {
    auto bio = read_pem(path);
    OpenSSLPtr<EVP_PKEY, EVP_PKEY_free> key(
        PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    return raw_public_key(key.get());
}
std::string fingerprint(const PublicKey& key) {
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    require(EVP_Digest(key.data(), key.size(), digest.data(), &size, EVP_sha256(), nullptr), "Digest failed");
    const char* digits = "0123456789abcdef";
    std::string result = "SHA256:";
    for (auto byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
void generate_identity(const fs::path& private_path, const fs::path& public_path) {
    if (fs::exists(private_path) || fs::exists(public_path) || private_path == public_path)
        throw std::runtime_error("Key output already exists; refusing to overwrite");
    OpenSSLPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free> generation(
        EVP_PKEY_CTX_new_from_name(nullptr, "ED25519", nullptr), EVP_PKEY_CTX_free);
    if (!generation) crypto_error("Ed25519 context failed");
    require(EVP_PKEY_keygen_init(generation.get()), "Keygen initialization failed");
    EVP_PKEY* generated = nullptr;
    require(EVP_PKEY_generate(generation.get(), &generated), "Keygen failed");
    OpenSSLPtr<EVP_PKEY, EVP_PKEY_free> key(generated, EVP_PKEY_free);
    OpenSSLPtr<BIO, BIO_free_all> private_bio(BIO_new(BIO_s_mem()), BIO_free_all);
    OpenSSLPtr<BIO, BIO_free_all> public_bio(BIO_new(BIO_s_mem()), BIO_free_all);
    if (!private_bio || !public_bio) crypto_error("BIO allocation failed");
    require(PEM_write_bio_PrivateKey(private_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr), "Private key encoding failed");
    require(PEM_write_bio_PUBKEY(public_bio.get(), key.get()), "Public key encoding failed");
    auto private_bytes = pem_bytes(private_bio.get());
    auto public_bytes = pem_bytes(public_bio.get());
    try {
        AtomicFile private_output(private_path, false);
        AtomicFile public_output(public_path, false);
        private_output.write(private_bytes.data(), private_bytes.size());
        public_output.write(public_bytes.data(), public_bytes.size());
        private_output.commit();
        try { public_output.commit(); }
        catch (...) { std::error_code ec; fs::remove(private_path, ec); throw; }
    }
    catch (...) { OPENSSL_cleanse(private_bytes.data(), private_bytes.size()); throw; }
    OPENSSL_cleanse(private_bytes.data(), private_bytes.size());
    char* private_data = nullptr;
    long private_size = BIO_get_mem_data(private_bio.get(), &private_data);
    if (private_size > 0) OPENSSL_cleanse(private_data, static_cast<size_t>(private_size));
    std::cout << "Identity created. " << fingerprint(raw_public_key(key.get())) << "\n";
}
int SecurityContext::verify_peer(X509_STORE_CTX* store, void* arg) noexcept {
    try {
        auto* self = static_cast<SecurityContext*>(arg);
        X509* certificate = X509_STORE_CTX_get0_cert(store);
        if (!certificate || X509_cmp_current_time(X509_get0_notBefore(certificate)) >= 0 ||
            X509_cmp_current_time(X509_get0_notAfter(certificate)) <= 0) return 0;
        OpenSSLPtr<EVP_PKEY, EVP_PKEY_free> key(X509_get_pubkey(certificate), EVP_PKEY_free);
        auto raw = raw_public_key(key.get());
        if (X509_verify(certificate, key.get()) != 1) return 0;
        for (const auto& trusted : self->trusted_keys) {
            if (CRYPTO_memcmp(raw.data(), trusted.data(), raw.size()) == 0) {
                X509_STORE_CTX_set_error(store, X509_V_OK);
                return 1;
            }
        }
    }
    catch (...) {}
    X509_STORE_CTX_set_error(store, X509_V_ERR_CERT_REJECTED);
    return 0;
}
SecurityContext::SecurityContext(bool server, const fs::path& private_path,
    const fs::path& peer_path, const fs::path& trust_dir) {
#ifndef _WIN32
    struct stat private_status{};
    if (lstat(private_path.c_str(), &private_status) != 0 || !S_ISREG(private_status.st_mode) ||
        (private_status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        throw std::runtime_error("Private key must be a regular owner-only file (chmod 600)");
#endif
    if (!peer_path.empty()) trusted_keys.push_back(read_public_key(peer_path));
    if (!trust_dir.empty()) {
        for (const auto& entry : fs::directory_iterator(trust_dir)) {
            if (entry.is_regular_file() && (entry.path().extension() == ".pub" || entry.path().extension() == ".pem"))
                trusted_keys.push_back(read_public_key(entry.path()));
        }
    }
    if (trusted_keys.empty()) throw std::runtime_error("No trusted peer public keys configured");
    auto bio = read_pem(private_path);
    OpenSSLPtr<EVP_PKEY, EVP_PKEY_free> key(
        PEM_read_bio_PrivateKey(bio.get(), nullptr, no_password, nullptr), EVP_PKEY_free);
    raw_public_key(key.get());
    ctx.reset(SSL_CTX_new(server ? TLS_server_method() : TLS_client_method()));
    if (!ctx) crypto_error("TLS context failed");
    require(SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION), "TLS version failed");
    require(SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION), "TLS version failed");
    require(SSL_CTX_set_ciphersuites(ctx.get(), "TLS_AES_256_GCM_SHA384"), "AES-GCM configuration failed");
    require(SSL_CTX_set1_groups_list(ctx.get(), "X25519"), "X25519 configuration failed");
    require(SSL_CTX_set1_sigalgs_list(ctx.get(), "ed25519"), "Ed25519 configuration failed");
    SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_OFF);
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_TICKET | SSL_OP_NO_COMPRESSION);
    require(SSL_CTX_set_num_tickets(ctx.get(), 0), "Ticket configuration failed");
    require(SSL_CTX_set_max_early_data(ctx.get(), 0), "Early data configuration failed");
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER | (server ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0), nullptr);
    // 这里采用显式公钥固定，而不是系统 CA；没有匹配公钥就拒绝握手。
    SSL_CTX_set_cert_verify_callback(ctx.get(), verify_peer, this);
    if (server) SSL_CTX_set_alpn_select_cb(ctx.get(), select_alpn, nullptr);

    OpenSSLPtr<X509, X509_free> certificate(X509_new(), X509_free);
    if (!certificate) crypto_error("Certificate allocation failed");
    require(X509_set_version(certificate.get(), 2), "Certificate version failed");
    std::array<unsigned char, 16> serial{};
    require(RAND_bytes(serial.data(), static_cast<int>(serial.size())), "Random serial failed");
    serial[0] &= 0x7f;
    OpenSSLPtr<BIGNUM, BN_free> number(BN_bin2bn(serial.data(), static_cast<int>(serial.size()), nullptr), BN_free);
    ASN1_INTEGER* serial_number = number ? BN_to_ASN1_INTEGER(number.get(), nullptr) : nullptr;
    if (!serial_number) crypto_error("Certificate serial failed");
    std::unique_ptr<ASN1_INTEGER, decltype(&ASN1_INTEGER_free)> serial_owner(serial_number, ASN1_INTEGER_free);
    require(X509_set_serialNumber(certificate.get(), serial_number), "Certificate serial failed");
    if (!X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -300) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 365L * 24L * 3600L))
        crypto_error("Certificate time failed");
    require(X509_set_pubkey(certificate.get(), key.get()), "Certificate key failed");
    X509_NAME* name = X509_get_subject_name(certificate.get());
    require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("SFT pinned identity"), -1, -1, 0), "Certificate name failed");
    require(X509_set_issuer_name(certificate.get(), name), "Certificate issuer failed");
    require(X509_sign(certificate.get(), key.get(), nullptr), "Certificate signature failed");
    require(SSL_CTX_use_certificate(ctx.get(), certificate.get()), "TLS certificate failed");
    require(SSL_CTX_use_PrivateKey(ctx.get(), key.get()), "TLS private key failed");
    require(SSL_CTX_check_private_key(ctx.get()), "TLS identity mismatch");
}
SecureChannel::SecureChannel(SecurityContext& context, SocketPtr _sock, bool server, int _timeout)
    : sock(std::move(_sock)), timeout(_timeout) {
    ssl.reset(SSL_new(context.native()));
    if (!ssl) crypto_error("TLS allocation failed");
    require(SSL_set_fd(ssl.get(), static_cast<int>(sock->native())), "TLS socket failed");
    if (!server) {
        const unsigned char alpn[] = {5, 's', 'f', 't', '/', '1'};
        if (SSL_set_alpn_protos(ssl.get(), alpn, sizeof(alpn)) != 0) crypto_error("ALPN failed");
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    while (true) {
        ERR_clear_error();
        int result = server ? SSL_accept(ssl.get()) : SSL_connect(ssl.get());
        if (result == 1) break;
        wait_for_ssl(result, deadline);
    }
    if (SSL_session_reused(ssl.get()) || SSL_get_verify_result(ssl.get()) != X509_V_OK)
        throw std::runtime_error("Fresh authenticated handshake required");
    const unsigned char* selected = nullptr;
    unsigned int selected_size = 0;
    SSL_get0_alpn_selected(ssl.get(), &selected, &selected_size);
    if (selected_size != 5 || CRYPTO_memcmp(selected, "sft/1", 5) != 0)
        throw std::runtime_error("Protocol negotiation failed");
}
SecureChannel::~SecureChannel() {
    // 不等待对端 close_notify；底层非阻塞 socket 由 RAII 回收。
    if (ssl) { ERR_clear_error(); SSL_shutdown(ssl.get()); }
}
bool SecureChannel::wait_for_input(int milliseconds) {
    return SSL_has_pending(ssl.get()) || socket_readable(sock->native(), milliseconds);
}
void SecureChannel::wait_for_ssl(int result, std::chrono::steady_clock::time_point deadline) {
    int error = SSL_get_error(ssl.get(), result);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        sock->wait(error == SSL_ERROR_WANT_WRITE, deadline);
        return;
    }
    if (error == SSL_ERROR_ZERO_RETURN) throw std::runtime_error("Peer disconnected");
    crypto_error("TLS connection failed (peer, key, or record rejected)");
}
void SecureChannel::read_all(unsigned char* output, size_t length) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    size_t received = 0;
    while (received < length) {
        ERR_clear_error();
        size_t count = 0;
        int result = SSL_read_ex(ssl.get(), output + received, length - received, &count);
        if (result == 1 && count > 0) received += count;
        else wait_for_ssl(result, deadline);
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("TLS read timed out");
    }
}
void SecureChannel::write_all(const unsigned char* input, size_t length) {
    if (bytes_sent >= 256ULL * 1024 * 1024) update_keys();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    size_t sent = 0;
    while (sent < length) {
        ERR_clear_error();
        size_t count = 0;
        int result = SSL_write_ex(ssl.get(), input + sent, length - sent, &count);
        if (result == 1 && count > 0) sent += count;
        else wait_for_ssl(result, deadline);
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("TLS write timed out");
    }
    bytes_sent += length;
}
void SecureChannel::update_keys() {
    require(SSL_key_update(ssl.get(), SSL_KEY_UPDATE_REQUESTED), "TLS key update failed");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    while (true) {
        ERR_clear_error();
        int result = SSL_do_handshake(ssl.get());
        if (result == 1) break;
        wait_for_ssl(result, deadline);
    }
    bytes_sent = 0;
}
std::string SecureChannel::peer_fingerprint() const {
    OpenSSLPtr<X509, X509_free> certificate(SSL_get1_peer_certificate(ssl.get()), X509_free);
    if (!certificate) throw std::runtime_error("Peer identity missing");
    OpenSSLPtr<EVP_PKEY, EVP_PKEY_free> key(X509_get_pubkey(certificate.get()), EVP_PKEY_free);
    return fingerprint(raw_public_key(key.get()));
}
