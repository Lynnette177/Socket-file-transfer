#include "storage.h"
#include <openssl/rand.h>
#ifdef _WIN32
#include <sddl.h>
#endif

namespace {
bool valid_utf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) { if (first < 32 || first == 127) return false; continue; }
        unsigned int value = 0;
        size_t count = 0;
        unsigned int minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 31U; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 15U; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7U; minimum = 0x10000; }
        else return false;
        if (count > text.size() - i) return false;
        for (size_t j = 0; j < count; ++j) {
            auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63U);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
void validate_component(const std::string& name) {
    if (name.empty() || name.size() > 255 || !valid_utf8(name) ||
        name.find_first_of("\\:<>\"|?*") != std::string::npos ||
        name.back() == '.' || name.back() == ' ' || internal_name(name))
        throw std::runtime_error("FAIL_INVALID_PATH");
    std::string base = name.substr(0, name.find('.'));
    for (char& letter : base) {
        if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 'a' + 'A');
    }
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
        (base.size() == 4 && (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT") &&
        base[3] >= '1' && base[3] <= '9')) throw std::runtime_error("FAIL_RESERVED_PATH");
}
void reject_link(const fs::path& path) {
    std::error_code error;
    auto status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw std::runtime_error("FAIL_PATH_ACCESS");
    if (fs::is_symlink(status)) throw std::runtime_error("FAIL_LINK_PATH");
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("FAIL_REPARSE_PATH");
#endif
}
std::string random_suffix() {
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        throw std::runtime_error("Random generation failed");
    std::string output;
    for (auto byte : random) { output += "0123456789abcdef"[byte >> 4]; output += "0123456789abcdef"[byte & 15]; }
    return output;
}
}
bool internal_name(const std::string& name) {
    std::string prefix = name.substr(0, 10);
    for (char& letter : prefix) {
        if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter - 'A' + 'a');
    }
    return prefix == ".sft-part-";
}
RootDirectory::RootDirectory(const fs::path& directory) {
    fs::create_directories(directory);
    root = fs::canonical(directory);
    if (!fs::is_directory(root)) throw std::runtime_error("Root must be a directory");
}
bool RootDirectory::contains(const fs::path& path) const {
    fs::path candidate = fs::weakly_canonical(path);
    while (!candidate.empty()) {
        std::error_code error;
        if (fs::equivalent(candidate, root, error) && !error) return true;
        auto parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return false;
}
fs::path RootDirectory::relative_path(const fs::path& currentPath, const std::string& requested) const {
    if (requested.size() > 4096 || requested.find('\0') != std::string::npos)
        throw std::runtime_error("FAIL_INVALID_PATH");
    fs::path result = !requested.empty() && requested.front() == '/' ? fs::path{} : currentPath;
    std::istringstream input(requested);
    std::string component;
    while (std::getline(input, component, '/')) {
        if (component.empty() || component == ".") continue;
        if (component == "..") {
            if (result.empty()) throw std::runtime_error("FAIL_OUTSIDE_ROOT");
            result = result.parent_path();
        }
        else { validate_component(component); result /= localPath(component); }
    }
    return result;
}
fs::path RootDirectory::resolve(const fs::path& relative) const {
    if (relative.is_absolute() || relative.has_root_name()) throw std::runtime_error("FAIL_OUTSIDE_ROOT");
    reject_link(root);
    fs::path resolved = root;
    for (const auto& component : relative) {
        auto name = pathString(component);
        validate_component(name);
        resolved /= component;
        reject_link(resolved);
    }
    return resolved;
}
std::string RootDirectory::virtual_path(const fs::path& relative) const {
    return "/" + relative.generic_u8string();
}
std::string RootDirectory::list_files(const fs::path& currentPath) const {
    std::vector<std::string> lines;
    size_t length = 0;
    for (const auto& entry : fs::directory_iterator(resolve(currentPath))) {
        auto name = pathString(entry.path().filename());
        try { validate_component(name); reject_link(entry.path()); }
        catch (const std::exception&) { continue; }
        auto status = entry.symlink_status();
        std::string line;
        if (fs::is_regular_file(status)) line = "File: ";
        else if (fs::is_directory(status)) line = "Directory: ";
        else continue;
        line += name + "\n";
        length += line.size();
        if (length > 240 * 1024) throw std::runtime_error("FAIL_DIRECTORY_TOO_LARGE");
        lines.push_back(std::move(line));
    }
    std::sort(lines.begin(), lines.end());
    std::string output;
    for (const auto& line : lines) output += line;
    return output.empty() ? "Empty Directory" : output;
}
AtomicFile::AtomicFile(const fs::path& path, bool _overwrite)
    : target_path(path), overwrite(_overwrite) {
    if (target_path.filename().empty()) throw std::runtime_error("FAIL_INVALID_FILE_PATH");
    reject_link(target_path);
    if (fs::exists(target_path) && (!overwrite || !fs::is_regular_file(target_path)))
        throw std::runtime_error("FAIL_EXISTED_NO_OVERWRITE");
    temporary_path = target_path.parent_path() / localPath(".sft-part-" + random_suffix());
#ifdef _WIN32
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GA;;;SY)(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Unable to set private file permissions");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    handle = CreateFileW(temporary_path.c_str(), GENERIC_WRITE, 0, &attributes,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("FAIL_OPEN_TEMP_FILE");
#else
    handle = open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (handle < 0) throw std::runtime_error("FAIL_OPEN_TEMP_FILE");
#endif
}
void AtomicFile::write(const unsigned char* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
        DWORD written = 0;
        DWORD count = static_cast<DWORD>(std::min<size_t>(size - offset, 1024 * 1024));
        if (!WriteFile(handle, data + offset, count, &written, nullptr) || written == 0)
            throw std::runtime_error("FAIL_WRITING_FILE");
        offset += written;
#else
        ssize_t written = ::write(handle, data + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw std::runtime_error("FAIL_WRITING_FILE");
        offset += static_cast<size_t>(written);
#endif
    }
}
void AtomicFile::close_file() {
#ifdef _WIN32
    if (handle == INVALID_HANDLE_VALUE) return;
    bool flushed = FlushFileBuffers(handle) != 0;
    bool closed = CloseHandle(handle) != 0;
    handle = INVALID_HANDLE_VALUE;
#else
    if (handle < 0) return;
    bool flushed = fsync(handle) == 0;
    bool closed = close(handle) == 0;
    handle = -1;
#endif
    if (!flushed || !closed) throw std::runtime_error("FAIL_FLUSHING_FILE");
}
void AtomicFile::commit() {
    if (committed) throw std::runtime_error("File already committed");
    close_file();
    reject_link(target_path);
    if (fs::exists(target_path) && !fs::is_regular_file(target_path))
        throw std::runtime_error("FAIL_INVALID_FILE_PATH");
#ifdef _WIN32
    DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temporary_path.c_str(), target_path.c_str(), flags))
        throw std::runtime_error("FAIL_COMMIT_FILE");
#else
    if (overwrite) {
        if (rename(temporary_path.c_str(), target_path.c_str()) != 0)
            throw std::runtime_error("FAIL_COMMIT_FILE");
    }
    else {
        // link 原子地拒绝已存在目标，避免 exists + rename 之间覆盖别人的文件。
        if (link(temporary_path.c_str(), target_path.c_str()) != 0)
            throw std::runtime_error("FAIL_COMMIT_FILE");
        unlink(temporary_path.c_str());
    }
#endif
    committed = true;
}
AtomicFile::~AtomicFile() {
#ifdef _WIN32
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
    if (handle >= 0) close(handle);
#endif
    if (!committed) { std::error_code error; fs::remove(temporary_path, error); }
}
FileDigest::FileDigest() {
    ctx.reset(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA256 initialization failed");
}
void FileDigest::update(const unsigned char* data, size_t size) {
    if (EVP_DigestUpdate(ctx.get(), data, size) != 1) throw std::runtime_error("SHA256 update failed");
}
std::vector<unsigned char> FileDigest::finish() {
    std::vector<unsigned char> digest(32);
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &size) != 1 || size != digest.size())
        throw std::runtime_error("SHA256 finalization failed");
    return digest;
}
std::vector<std::string> split_command(const std::string& input) {
    std::vector<std::string> output;
    std::string token;
    char quote = 0;
    bool started = false;
    for (char letter : input) {
        if (letter == '\0') throw std::runtime_error("Invalid command");
        if (quote) {
            if (letter == quote) quote = 0;
            else token += letter;
        }
        else if (letter == '\'' || letter == '"') { quote = letter; started = true; }
        else if (letter == ' ' || letter == '\t' || letter == '\r') {
            if (started) { output.push_back(token); token.clear(); started = false; }
        }
        else { token += letter; started = true; }
    }
    if (quote) throw std::runtime_error("Unclosed quote");
    if (started) output.push_back(token);
    return output;
}
uint64_t parse_number(const std::string& text, uint64_t maximum) {
    if (text.empty()) throw std::runtime_error("Invalid numeric argument");
    uint64_t result = 0;
    for (char letter : text) {
        if (letter < '0' || letter > '9' || static_cast<uint64_t>(letter - '0') > maximum ||
            result > (maximum - static_cast<uint64_t>(letter - '0')) / 10)
            throw std::runtime_error("Numeric argument out of range");
        result = result * 10 + static_cast<uint64_t>(letter - '0');
    }
    return result;
}
