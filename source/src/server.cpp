#include "server.h"

static_assert(std::atomic<bool>::is_always_lock_free, "Signal flag must be lock-free");
std::atomic<bool> stop_requested{false};
namespace {
std::mutex output_mutex;
void print_line(const std::string& text) {
    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << text << std::endl;
}
uint64_t file_size(std::ifstream& file, uint64_t maximum) {
    file.seekg(0, std::ios::end);
    auto position = file.tellg();
    if (position < 0) throw std::runtime_error("FAIL_READ_FILE_SIZE");
    uint64_t size = static_cast<uint64_t>(position);
    if (size > maximum) throw std::runtime_error("FAIL_FILE_TOO_LARGE");
    file.seekg(0, std::ios::beg);
    if (!file) throw std::runtime_error("FAIL_READ_FILE");
    return size;
}
void require_count(const std::vector<std::string>& command, size_t count) {
    if (command.size() != count) throw std::runtime_error("FAIL_INVALID_COMMAND_FORMAT");
}
bool read_input(std::istream& input, bool interactive, std::string& line) {
#ifndef _WIN32
    if (interactive) {
        // 直接读取 stdin，避免 select 看不到 stdio 预读缓冲区中的下一行。
        while (!stop_requested) {
            if (!socket_readable(STDIN_FILENO, 200)) continue;
            char letter = 0;
            ssize_t count = ::read(STDIN_FILENO, &letter, 1);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return !line.empty();
            if (letter == '\n') return true;
            if (line.size() >= 16384) throw std::runtime_error("Command too long");
            line += letter;
        }
        return false;
    }
#else
    (void)interactive;
#endif
    return static_cast<bool>(std::getline(input, line));
}
}

ClientHandler::ClientHandler(SocketPtr _clnt_sock, SecurityContext& _security,
    RootDirectory& _directory, std::mutex& _file_mutex, const Options& _options)
    : clnt_sock(std::move(_clnt_sock)), security(_security), directory(_directory),
      file_mutex(_file_mutex), options(_options) {}

void ClientHandler::handle() {
    auto address = client_address(clnt_sock->native());
    try {
        SecureChannel channel(security, clnt_sock, true, options.timeout);
        Protocol protocol(channel);
        print_line("New Connection Established. Client: " + address + " " + channel.peer_fingerprint());
        protocol.send_response(true, directory.virtual_path(currentPath));
        while (!stop_requested) {
            auto command = protocol.receive_command();
            bool transferring = false;
            try {
                const auto& operation = command[0];
                if (operation == "exit") {
                    require_count(command, 1);
                    protocol.send_response(true, "SUCCESS_BYE");
                    break;
                }
                else if (operation == "rekey") {
                    require_count(command, 1);
                    protocol.send_response(true, "SUCCESS_KEYS_UPDATED");
                }
                else if (operation == "pwd") {
                    require_count(command, 1);
                    protocol.send_response(true, directory.virtual_path(currentPath));
                }
                else if (operation == LS_COMMAND) {
                    require_count(command, 1);
                    std::string listing;
                    { std::lock_guard<std::mutex> lock(file_mutex); listing = directory.list_files(currentPath); }
                    protocol.send_response(true, listing);
                }
                else if (operation == CD_COMMAND) {
                    require_count(command, 2);
                    auto relative = directory.relative_path(currentPath, command[1]);
                    {
                        std::lock_guard<std::mutex> lock(file_mutex);
                        if (!fs::is_directory(directory.resolve(relative))) throw std::runtime_error("FAIL_NO_DIR");
                        currentPath = relative;
                    }
                    protocol.send_response(true, directory.virtual_path(currentPath));
                }
                else if (operation == MKDIR_COMMAND) {
                    require_count(command, 2);
                    if (options.read_only) throw std::runtime_error("FAIL_READ_ONLY");
                    auto relative = directory.relative_path(currentPath, command[1]);
                    {
                        std::lock_guard<std::mutex> lock(file_mutex);
                        if (relative.empty() || !fs::create_directory(directory.resolve(relative)))
                            throw std::runtime_error("FAIL_UNABLE_CREATE_DIR");
                    }
                    protocol.send_response(true, "SUCCESS_CREATED");
                }
                else if (operation == DELETE_COMMAND) {
                    require_count(command, 3);
                    if (options.read_only) throw std::runtime_error("FAIL_READ_ONLY");
                    if (command[1] != "0" && command[1] != "1") throw std::runtime_error("FAIL_INVALID_COMMAND_FORMAT");
                    auto relative = directory.relative_path(currentPath, command[2]);
                    {
                        std::lock_guard<std::mutex> lock(file_mutex);
                        auto path = directory.resolve(relative);
                        if (relative.empty()) throw std::runtime_error("FAIL_ROOT_OPERATION");
                        if (command[1] == "1") {
                            if (!fs::is_directory(path)) throw std::runtime_error("FAIL_NOT_A_DIR");
                            if (fs::remove_all(path) == 0) throw std::runtime_error("FAIL_UNABLE_DELETE_DIR");
                        }
                        else {
                            if (!fs::is_regular_file(path) || !fs::remove(path)) throw std::runtime_error("FAIL_UNABLE_DELETE");
                        }
                    }
                    protocol.send_response(true, "SUCCESS_DELETED");
                }
                else if (operation == UPLOAD_COMMAND) {
                    require_count(command, 4);
                    if (options.read_only) throw std::runtime_error("FAIL_READ_ONLY");
                    if (command[1] != "0" && command[1] != "1") throw std::runtime_error("FAIL_INVALID_COMMAND_FORMAT");
                    uint64_t size = parse_number(command[3], options.max_file_size);
                    auto relative = directory.relative_path(currentPath, command[2]);
                    if (relative.empty()) throw std::runtime_error("FAIL_INVALID_FILE_PATH");
                    std::unique_ptr<AtomicFile> output;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex);
                        output = std::make_unique<AtomicFile>(directory.resolve(relative), command[1] == "1");
                    }
                    transferring = true;
                    protocol.send_response(true, "SUCCESS_START_SEND");
                    protocol.receive_file(*output, size, false);
                    {
                        std::lock_guard<std::mutex> lock(file_mutex);
                        directory.resolve(relative);
                        output->commit();
                    }
                    protocol.send_response(true, "SUCCESS_FINISH_RECV");
                    print_line("Upload committed: " + directory.virtual_path(relative) + " (" + std::to_string(size) + " bytes)");
                }
                else if (operation == DOWNLOAD_COMMAND) {
                    require_count(command, 2);
                    auto relative = directory.relative_path(currentPath, command[1]);
                    std::ifstream file;
                    uint64_t size = 0;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex);
                        auto path = directory.resolve(relative);
                        if (!fs::is_regular_file(path)) throw std::runtime_error("FAIL_NOT_EXIST");
                        file.open(path, std::ios::binary);
                        if (!file) throw std::runtime_error("FAIL_OPEN_FILE");
                        size = file_size(file, options.max_file_size);
                    }
                    transferring = true;
                    protocol.send_file_meta(size);
                    auto ready = protocol.receive_response();
                    if (!ready.success) continue;
                    if (ready.text != "SUCCESS_READY") throw std::runtime_error("Invalid download state");
                    protocol.send_file(file, size, false);
                    auto result = protocol.receive_response();
                    if (!result.success || result.text != "SUCCESS_FINISH_RECV")
                        throw std::runtime_error("Client did not commit downloaded file");
                    // 完成双向确认后才能开始下一条命令。
                    protocol.send_response(true, "SUCCESS_SEND_FINISH");
                }
                else throw std::runtime_error("FAIL_INVALID_COMMAND");
            }
            catch (const fs::filesystem_error&) {
                if (transferring) throw;
                protocol.send_response(false, "FAIL_FILESYSTEM_OPERATION");
            }
            catch (const std::exception& error) {
                if (transferring) throw;
                protocol.send_response(false, error.what());
            }
        }
    }
    catch (const std::exception& error) {
        print_line("Connection closed (" + address + "): " + error.what());
    }
}

Server::Server(const Options& _options) : options(_options), directory(options.root),
    security(true, options.private_path, options.peer_path, options.trust_dir) {
    if (directory.contains(options.private_path) ||
        (!options.peer_path.empty() && directory.contains(options.peer_path)) ||
        (!options.trust_dir.empty() && directory.contains(options.trust_dir)))
        throw std::runtime_error("Identity and trust files must be outside the shared root");
}
Server::~Server() { stop(); }
void Server::reap_clients() {
    auto it = client_threads.begin();
    while (it != client_threads.end()) {
        if (*it->finished) {
            if (it->thread.joinable()) it->thread.join();
            it = client_threads.erase(it);
        }
        else ++it;
    }
}
void Server::start_server() {
    serv_sock = listen_server(options.ip, options.port);
    print_line("Server Started: " + options.ip + ":" + std::to_string(options.port));
    try {
        while (!stop_requested && !stop_server) {
            reap_clients();
            if (!socket_readable(serv_sock->native(), 200)) continue;
            auto client = accept_client(serv_sock);
            if (!client) continue;
            if (client_threads.size() >= options.max_clients) { client->cancel(); continue; }
            auto finished = std::make_shared<std::atomic<bool>>(false);
            ClientThread entry{client, finished, {}};
            // 先完成容器分配，再创建线程，避免分配失败时遗留 joinable 临时线程。
            client_threads.push_back(std::move(entry));
            try {
                client_threads.back().thread = std::thread([this, client, finished]() {
                    ClientHandler handler(client, security, directory, file_mutex, options);
                    handler.handle();
                    *finished = true;
                });
            }
            catch (...) { client_threads.pop_back(); throw; }
        }
    }
    catch (...) { stop(); throw; }
    stop();
}
void Server::stop() {
    stop_server = true;
    if (serv_sock) serv_sock->cancel();
    for (auto& client : client_threads) client.sock->cancel();
    for (auto& client : client_threads) if (client.thread.joinable()) client.thread.join();
    client_threads.clear();
    serv_sock.reset();
}

ClientProcessor::ClientProcessor(SecureChannel& _channel, const Options& _options)
    : channel(_channel), protocol(_channel), options(_options) {
    auto greeting = protocol.receive_response();
    if (!greeting.success || greeting.text != "/") throw std::runtime_error("Invalid server greeting");
    currentPath = greeting.text;
}
bool ClientProcessor::show_response(const Response& response) {
    std::cout << "[+]Server:: " << response.text << "\n";
    return response.success;
}
bool ClientProcessor::upload_file(const std::vector<std::string>& command) {
    size_t start = command.size() > 1 && command[1] == "-o" ? 2 : 1;
    if (command.size() < start + 1 || command.size() > start + 2) {
        std::cerr << "Usage: upload [-o] local_file [remote_file]\n"; return false;
    }
    auto path = options.local / localPath(command[start]);
    if (!fs::is_regular_file(path)) { std::cerr << "Local file does not exist\n"; return false; }
    std::ifstream file(path, std::ios::binary);
    if (!file) { std::cerr << "Unable to open local file\n"; return false; }
    uint64_t size = file_size(file, options.max_file_size);
    std::string remote = command.size() == start + 2 ? command.back() : pathString(path.filename());
    protocol.send_command({UPLOAD_COMMAND, start == 2 ? "1" : "0", remote, std::to_string(size)});
    auto response = protocol.receive_response();
    if (!show_response(response)) return false;
    if (response.text != "SUCCESS_START_SEND") throw std::runtime_error("Invalid upload state");
    protocol.send_file(file, size, options.progress);
    auto result = protocol.receive_response();
    if (!result.success || result.text != "SUCCESS_FINISH_RECV") throw std::runtime_error("Upload was not confirmed");
    return show_response(result);
}
bool ClientProcessor::download_file(const std::vector<std::string>& command) {
    size_t start = command.size() > 1 && command[1] == "-o" ? 2 : 1;
    if (command.size() < start + 1 || command.size() > start + 2) {
        std::cerr << "Usage: get [-o] remote_file [local_file]\n"; return false;
    }
    auto name = command.size() == start + 2 ? localPath(command.back()) : localPath(command[start]).filename();
    if (name.empty() || name.filename() == "." || name.filename() == "..") {
        std::cerr << "Invalid local filename\n"; return false;
    }
    auto path = options.local / name;
    if (start == 2) {
        // get -o 在目标已存在时追加 .L，避免覆盖已有下载文件。
        size_t attempts = 0;
        while (fs::exists(path) && attempts++ < 1000) path += ".L";
    }
    std::unique_ptr<AtomicFile> output;
    try { output = std::make_unique<AtomicFile>(path, false); }
    catch (const std::exception& error) { std::cerr << error.what() << "\n"; return false; }
    protocol.send_command({DOWNLOAD_COMMAND, command[start]});
    auto metadata = protocol.receive_message();
    if (metadata.type == MessageType::Response) {
        auto response = Protocol::parse_response(metadata);
        if (response.success) throw std::runtime_error("Expected file metadata, got success response");
        return show_response(response);
    }
    auto size = Protocol::parse_file_meta(metadata);
    if (size > options.max_file_size) {
        protocol.send_response(false, "FAIL_FILE_TOO_LARGE");
        std::cerr << "File exceeds configured size limit\n"; return false;
    }
    protocol.send_response(true, "SUCCESS_READY");
    protocol.receive_file(*output, size, options.progress);
    output->commit();
    protocol.send_response(true, "SUCCESS_FINISH_RECV");
    auto result = protocol.receive_response();
    if (!result.success || result.text != "SUCCESS_SEND_FINISH") throw std::runtime_error("Download confirmation failed");
    return show_response(result);
}
bool ClientProcessor::handle(const std::vector<std::string>& command) {
    if (command.empty()) return true;
    if (command[0] == UPLOAD_COMMAND) return upload_file(command);
    if (command[0] == DOWNLOAD_COMMAND) return download_file(command);
    auto fields = command;
    if (command[0] == CD_COMMAND && command.size() == 1) fields.push_back("/");
    if (command[0] == DELETE_COMMAND) {
        if (command.size() == 3 && command[1] == "-r") fields = {DELETE_COMMAND, "1", command[2]};
        else if (command.size() == 2) fields = {DELETE_COMMAND, "0", command[1]};
        else { std::cerr << "Usage: rm [-r] remote_path\n"; return false; }
    }
    if (command[0] == "rekey") { require_count(command, 1); channel.update_keys(); }
    protocol.send_command(fields);
    auto response = protocol.receive_response();
    if (response.success && command[0] == CD_COMMAND) currentPath = response.text;
    return show_response(response);
}
Client::Client(const Options& _options) : options(_options),
    security(false, options.private_path, options.peer_path, {}) {
    fs::create_directories(options.local);
}
Client::~Client() { stop(); }
void Client::disconnect() {
    C_process.reset();
    channel.reset();
}
bool Client::createProcessor() {
    disconnect();
    for (int attempt = 0; attempt < options.retries && !stop_requested; ++attempt) {
        try {
            auto sock = connect_server(options.ip, options.port, options.timeout);
            auto candidate = std::make_unique<SecureChannel>(security, sock, false, options.timeout);
            auto processor = std::make_unique<ClientProcessor>(*candidate, options);
            std::cout << "Fresh handshake complete: TLSv1.3 / Ed25519 / X25519 / AES-256-GCM\n";
            std::cout << "Peer: " << candidate->peer_fingerprint() << "\n";
            channel = std::move(candidate);
            C_process = std::move(processor);
            return true;
        }
        catch (const std::exception& error) {
            std::cerr << "Connection attempt " << attempt + 1 << " failed: " << error.what() << "\n";
            disconnect();
            if (attempt + 1 < options.retries)
                std::this_thread::sleep_for(std::chrono::milliseconds(200 * (attempt + 1)));
        }
    }
    return false;
}
bool Client::input_handler(std::istream& input, bool interactive) {
    bool success = true;
    while (!stop_requested) {
        if (interactive) std::cout << (C_process ? C_process->currentPath : "disconnected") << ">" << std::flush;
        std::string line;
        if (!read_input(input, interactive, line)) break;
        try {
            if (line.size() > 16384) throw std::runtime_error("Command too long");
            auto command = split_command(line);
            if (command.empty()) continue;
            if (command[0] == "help") {
                std::cout << "ls | pwd | cd [path] | mkdir path | rm [-r] path\n"
                    "upload [-o] local [remote] | get [-o] remote [local]\n"
                    "reconnect | rekey | exit\n";
                continue;
            }
            if (command[0] == "reconnect") {
                require_count(command, 1);
                if (!createProcessor()) success = false;
                continue;
            }
            if (command[0] == "exit" && !C_process) break;
            if (!C_process && !createProcessor()) { success = false; continue; }
            if (!C_process->handle(command)) success = false;
            if (command[0] == "exit" && command.size() == 1) break;
        }
        catch (const std::exception& error) {
            std::cerr << "Operation failed: " << error.what()
                << "\nResult may be unconfirmed. Operation was NOT replayed; reconnect before retrying.\n";
            success = false;
            disconnect();
        }
    }
    return success;
}
bool Client::start_client() {
    if (!createProcessor()) return false;
    if (options.commands.empty()) return input_handler(std::cin, true);
    std::ifstream input(options.commands);
    if (!input) throw std::runtime_error("Unable to open commands file");
    return input_handler(input, false);
}
void Client::stop() { disconnect(); }
