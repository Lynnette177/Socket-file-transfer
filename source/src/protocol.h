#pragma once
#include "security.h"
#include "storage.h"

enum class MessageType : uint16_t { Command = 1, Response = 2, FileMeta = 3, FileData = 4, FileEnd = 5 };
struct Message { MessageType type; std::vector<unsigned char> data; };
struct Response { bool success; std::string text; };

class Protocol {
private:
    SecureChannel& channel;
public:
    explicit Protocol(SecureChannel& _channel) : channel(_channel) {}
    void send_message(MessageType type, const std::vector<unsigned char>& data);
    Message receive_message();
    void send_command(const std::vector<std::string>& command);
    std::vector<std::string> receive_command();
    void send_response(bool success, const std::string& text);
    Response receive_response();
    static Response parse_response(const Message& message);
    void send_file_meta(uint64_t size);
    static uint64_t parse_file_meta(const Message& message);
    void send_file(std::ifstream& file, uint64_t size, bool progress);
    void receive_file(AtomicFile& output, uint64_t size, bool progress);
};

void printProgressBar(uint64_t completed, uint64_t total);
