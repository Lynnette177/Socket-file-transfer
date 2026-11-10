#include "protocol.h"

namespace {
constexpr size_t MAX_FRAME_SIZE = 256 * 1024;
void append_number(std::vector<unsigned char>& bytes, uint64_t value, size_t size) {
    for (size_t i = size; i > 0; --i) bytes.push_back(static_cast<unsigned char>(value >> ((i - 1) * 8)));
}
uint64_t read_number(const std::vector<unsigned char>& bytes, size_t& offset, size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset) throw std::runtime_error("Invalid protocol field");
    uint64_t result = 0;
    for (size_t i = 0; i < size; ++i) result = (result << 8) | bytes[offset++];
    return result;
}
std::vector<unsigned char> encode_fields(const std::vector<std::string>& fields) {
    if (fields.empty() || fields.size() > 16) throw std::runtime_error("Invalid field count");
    std::vector<unsigned char> data;
    append_number(data, fields.size(), 2);
    for (const auto& field : fields) {
        if (field.size() > MAX_FRAME_SIZE) throw std::runtime_error("Field too long");
        append_number(data, field.size(), 4);
        data.insert(data.end(), field.begin(), field.end());
    }
    return data;
}
std::vector<std::string> decode_fields(const std::vector<unsigned char>& data) {
    size_t offset = 0;
    size_t count = static_cast<size_t>(read_number(data, offset, 2));
    if (count == 0 || count > 16) throw std::runtime_error("Invalid field count");
    std::vector<std::string> fields;
    for (size_t i = 0; i < count; ++i) {
        size_t length = static_cast<size_t>(read_number(data, offset, 4));
        if (length > data.size() - offset) throw std::runtime_error("Invalid field length");
        fields.emplace_back(reinterpret_cast<const char*>(data.data() + offset), length);
        offset += length;
    }
    if (offset != data.size()) throw std::runtime_error("Trailing protocol data");
    return fields;
}
}
void Protocol::send_message(MessageType type, const std::vector<unsigned char>& data) {
    if (data.size() > MAX_FRAME_SIZE) throw std::runtime_error("Frame too large");
    std::vector<unsigned char> header{'S', 'F', 'T', '1'};
    append_number(header, 1, 2);
    append_number(header, static_cast<uint16_t>(type), 2);
    append_number(header, data.size(), 4);
    channel.write_all(header.data(), header.size());
    if (!data.empty()) channel.write_all(data.data(), data.size());
}
Message Protocol::receive_message() {
    std::vector<unsigned char> header(12);
    channel.read_all(header.data(), header.size());
    if (std::memcmp(header.data(), "SFT1", 4) != 0) throw std::runtime_error("Protocol magic mismatch");
    size_t offset = 4;
    if (read_number(header, offset, 2) != 1) throw std::runtime_error("Unsupported protocol version");
    auto type = read_number(header, offset, 2);
    auto length = read_number(header, offset, 4);
    if (type < 1 || type > 5 || length > MAX_FRAME_SIZE) throw std::runtime_error("Invalid frame header");
    Message message{static_cast<MessageType>(type), std::vector<unsigned char>(static_cast<size_t>(length))};
    if (length) channel.read_all(message.data.data(), message.data.size());
    return message;
}
void Protocol::send_command(const std::vector<std::string>& command) {
    send_message(MessageType::Command, encode_fields(command));
}
std::vector<std::string> Protocol::receive_command() {
    auto message = receive_message();
    if (message.type != MessageType::Command) throw std::runtime_error("Expected command");
    return decode_fields(message.data);
}
void Protocol::send_response(bool success, const std::string& text) {
    send_message(MessageType::Response, encode_fields({success ? "OK" : "ERROR", text}));
}
Response Protocol::parse_response(const Message& message) {
    if (message.type != MessageType::Response) throw std::runtime_error("Expected response");
    auto fields = decode_fields(message.data);
    if (fields.size() != 2 || (fields[0] != "OK" && fields[0] != "ERROR"))
        throw std::runtime_error("Invalid response");
    return {fields[0] == "OK", fields[1]};
}
Response Protocol::receive_response() { return parse_response(receive_message()); }
void Protocol::send_file_meta(uint64_t size) {
    std::vector<unsigned char> data;
    append_number(data, size, 8);
    send_message(MessageType::FileMeta, data);
}
uint64_t Protocol::parse_file_meta(const Message& message) {
    if (message.type != MessageType::FileMeta || message.data.size() != 8)
        throw std::runtime_error("Expected file metadata");
    size_t offset = 0;
    return read_number(message.data, offset, 8);
}
void printProgressBar(uint64_t completed, uint64_t total) {
    double progress = total ? static_cast<double>(completed) / static_cast<double>(total) : 1.0;
    int barWidth = 40;
    std::cout << "[";
    int pos = static_cast<int>(barWidth * progress);
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << static_cast<int>(progress * 100) << "% " << completed << "/" << total << " bytes\r";
    std::cout.flush();
}
void Protocol::send_file(std::ifstream& file, uint64_t size, bool progress) {
    FileDigest digest;
    uint64_t sent = 0;
    unsigned int last_percent = 101;
    std::vector<unsigned char> buffer(FILE_CHUNK_SIZE);
    while (sent < size) {
        size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), size - sent));
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
        if (file.gcount() != static_cast<std::streamsize>(count)) throw std::runtime_error("File changed or read failed");
        std::vector<unsigned char> data(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
        digest.update(data.data(), data.size());
        send_message(MessageType::FileData, data);
        sent += count;
        unsigned int percent = static_cast<unsigned int>(static_cast<double>(sent) / static_cast<double>(size) * 100);
        if (progress && percent != last_percent) { printProgressBar(sent, size); last_percent = percent; }
    }
    if (file.peek() != std::char_traits<char>::eof() || file.bad()) throw std::runtime_error("File changed or read failed");
    send_message(MessageType::FileEnd, digest.finish());
    if (progress) {
        if (last_percent != 100) printProgressBar(sent, size);
        std::cout << "\nConfirming...\n";
    }
}
void Protocol::receive_file(AtomicFile& output, uint64_t size, bool progress) {
    FileDigest digest;
    uint64_t received = 0;
    unsigned int last_percent = 101;
    while (received < size) {
        auto message = receive_message();
        if (message.type != MessageType::FileData || message.data.empty() ||
            message.data.size() > FILE_CHUNK_SIZE || message.data.size() > size - received)
            throw std::runtime_error("Invalid file block");
        output.write(message.data.data(), message.data.size());
        digest.update(message.data.data(), message.data.size());
        received += message.data.size();
        unsigned int percent = static_cast<unsigned int>(static_cast<double>(received) / static_cast<double>(size) * 100);
        if (progress && percent != last_percent) { printProgressBar(received, size); last_percent = percent; }
    }
    auto ending = receive_message();
    auto expected = digest.finish();
    if (ending.type != MessageType::FileEnd || ending.data.size() != expected.size() ||
        CRYPTO_memcmp(ending.data.data(), expected.data(), expected.size()) != 0)
        throw std::runtime_error("File SHA256 verification failed");
    if (progress) {
        if (last_percent != 100) printProgressBar(received, size);
        std::cout << "\nConfirming...\n";
    }
}
