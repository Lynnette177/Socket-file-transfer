#pragma once
#include "includes.h"
#include <openssl/evp.h>

constexpr uint64_t DEFAULT_MAX_FILE_SIZE = 10ULL * 1024 * 1024 * 1024;
constexpr size_t FILE_CHUNK_SIZE = 64 * 1024;
bool internal_name(const std::string& name);

class RootDirectory {
private:
    fs::path root;
public:
    explicit RootDirectory(const fs::path& directory);
    fs::path relative_path(const fs::path& currentPath, const std::string& requested) const;
    fs::path resolve(const fs::path& relative) const;
    std::string virtual_path(const fs::path& relative) const;
    std::string list_files(const fs::path& currentPath) const;
    bool contains(const fs::path& path) const;
};

class AtomicFile {
private:
    fs::path target_path;
    fs::path temporary_path;
    bool overwrite;
    bool committed = false;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int handle = -1;
#endif
    void close_file();
public:
    AtomicFile(const fs::path& path, bool _overwrite);
    ~AtomicFile();
    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;
    void write(const unsigned char* data, size_t size);
    void commit();
};

class FileDigest {
private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{nullptr, EVP_MD_CTX_free};
public:
    FileDigest();
    void update(const unsigned char* data, size_t size);
    std::vector<unsigned char> finish();
};

std::vector<std::string> split_command(const std::string& input);
uint64_t parse_number(const std::string& text, uint64_t maximum);
