#pragma once
#include "protocol.h"

#define LS_COMMAND "ls"
#define DOWNLOAD_COMMAND "get"
#define UPLOAD_COMMAND "upload"
#define DELETE_COMMAND "rm"
#define CD_COMMAND "cd"
#define MKDIR_COMMAND "mkdir"

struct Options {
    std::string ip = "127.0.0.1";
    int port = 5005;
    int timeout = 30;
    int retries = 3;
    size_t max_clients = 32;
    uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE;
    fs::path private_path;
    fs::path peer_path;
    fs::path trust_dir;
    fs::path root = "shared";
    fs::path local = ".";
    fs::path commands;
    bool read_only = false;
    bool progress = true;
};

extern std::atomic<bool> stop_requested;

class ClientHandler {
private:
    SocketPtr clnt_sock;
    SecurityContext& security;
    RootDirectory& directory;
    std::mutex& file_mutex;
    const Options& options;
    fs::path currentPath;
public:
    ClientHandler(SocketPtr _clnt_sock, SecurityContext& _security, RootDirectory& _directory,
        std::mutex& _file_mutex, const Options& _options);
    void handle();
};

class Server {
private:
    struct ClientThread {
        SocketPtr sock;
        std::shared_ptr<std::atomic<bool>> finished;
        std::thread thread;
    };
    Options options;
    RootDirectory directory;
    SecurityContext security;
    SocketPtr serv_sock;
    std::vector<ClientThread> client_threads;
    std::mutex file_mutex;
    std::atomic<bool> stop_server{false};
    void reap_clients();
public:
    explicit Server(const Options& _options);
    ~Server();
    void start_server();
    void stop();
};

class ClientProcessor {
private:
    SecureChannel& channel;
    Protocol protocol;
    const Options& options;
    bool show_response(const Response& response);
    bool upload_file(const std::vector<std::string>& command);
    bool download_file(const std::vector<std::string>& command);
public:
    std::string currentPath = "/";
    ClientProcessor(SecureChannel& _channel, const Options& _options);
    bool handle(const std::vector<std::string>& command);
};

class Client {
private:
    Options options;
    SecurityContext security;
    std::unique_ptr<SecureChannel> channel;
    std::unique_ptr<ClientProcessor> C_process;
    void disconnect();
    bool input_handler(std::istream& input, bool interactive);
public:
    explicit Client(const Options& _options);
    ~Client();
    bool createProcessor();
    bool start_client();
    void stop();
};
