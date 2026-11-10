#include "server.h"
#include <map>

namespace {
void request_stop(int) { stop_requested = 1; }
#ifdef _WIN32
BOOL WINAPI console_stop(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        stop_requested = 1;
        return TRUE;
    }
    return FALSE;
}
#endif
void print_help() {
    std::cout << "Secure File Transfer (one executable, server and client)\n"
        "sft keygen --key identity.key --public identity.pub\n"
        "sft fingerprint --public peer.pub\n"
        "sft server --key server.key --peer client.pub --root shared [--host 127.0.0.1] [--port 5005]\n"
        "sft server --key server.key --trust-dir trusted_clients --root shared\n"
        "sft client --key client.key --peer server.pub [--host 127.0.0.1] [--port 5005] [--local downloads]\n"
        "Options: --timeout 30 --max-file-size 10737418240 --max-clients 32\n"
        "         --read-only (server), --retries 3 --commands commands.txt --no-progress (client)\n"
        "Commands: ls, pwd, cd, mkdir, rm [-r], upload [-o], get [-o], rekey, reconnect, exit\n"
        "Quote paths containing spaces. Exchange public keys through a trusted channel before connecting.\n";
}
int run(const std::vector<std::string>& args) {
    if (args.size() < 2 || args[1] == "--help" || args[1] == "help") { print_help(); return 0; }
    std::string mode = args[1];
    if (mode != "server" && mode != "client" && mode != "keygen" && mode != "fingerprint")
        throw std::runtime_error("Unknown mode; use --help");
    std::map<std::string, std::string> arguments;
    for (size_t i = 2; i < args.size(); ++i) {
        const auto& name = args[i];
        if (name == "--help") { print_help(); return 0; }
        if (name.rfind("--", 0) != 0 || arguments.count(name)) throw std::runtime_error("Invalid or duplicate option: " + name);
        if (name == "--read-only" || name == "--no-progress") arguments[name] = "1";
        else {
            if (i + 1 >= args.size()) throw std::runtime_error("Missing value for " + name);
            arguments[name] = args[++i];
        }
    }
    auto value = [&](const std::string& name, const std::string& fallback) {
        auto it = arguments.find(name);
        if (it == arguments.end()) return fallback;
        std::string output = it->second;
        arguments.erase(it);
        return output;
    };
    auto finish = [&]() {
        if (!arguments.empty()) throw std::runtime_error("Unsupported option for this mode: " + arguments.begin()->first);
    };
    if (mode == "keygen") {
        auto private_path = localPath(value("--key", "identity.key"));
        auto public_path = localPath(value("--public", "identity.pub"));
        finish();
        generate_identity(private_path, public_path);
        return 0;
    }
    if (mode == "fingerprint") {
        auto public_path = localPath(value("--public", "identity.pub"));
        finish();
        std::cout << fingerprint(read_public_key(public_path)) << "\n";
        return 0;
    }
    Options options;
    options.private_path = localPath(value("--key", "identity.key"));
    options.peer_path = localPath(value("--peer", ""));
    options.ip = value("--host", "127.0.0.1");
    options.port = static_cast<int>(parse_number(value("--port", "5005"), 65535));
    options.timeout = static_cast<int>(parse_number(value("--timeout", "30"), 86400));
    options.max_file_size = parse_number(value("--max-file-size", std::to_string(DEFAULT_MAX_FILE_SIZE)),
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    if (options.port == 0 || options.timeout == 0) throw std::runtime_error("Port and timeout must be positive");
    if (mode == "server") {
        options.root = localPath(value("--root", "shared"));
        options.trust_dir = localPath(value("--trust-dir", ""));
        options.max_clients = static_cast<size_t>(parse_number(value("--max-clients", "32"), 256));
        options.read_only = value("--read-only", "0") == "1";
        if (options.max_clients == 0) throw std::runtime_error("max-clients must be positive");
        if (options.peer_path.empty() == options.trust_dir.empty())
            throw std::runtime_error("Server requires exactly one of --peer or --trust-dir");
        finish();
        Server server(options);
        server.start_server();
        return 0;
    }
    options.local = localPath(value("--local", "."));
    options.commands = localPath(value("--commands", ""));
    options.retries = static_cast<int>(parse_number(value("--retries", "3"), 10));
    options.progress = value("--no-progress", "0") != "1";
    if (options.peer_path.empty() || options.retries == 0) throw std::runtime_error("Client requires --peer and positive --retries");
    finish();
    Client client(options);
    return client.start_client() ? 0 : 1;
}
int entry(const std::vector<std::string>& args) {
    try {
        NetworkRuntime network;
        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
#ifdef _WIN32
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCtrlHandler(console_stop, TRUE);
#endif
        return run(args);
    }
    catch (const std::exception& error) { std::cerr << "Error: " << error.what() << "\n"; return 1; }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 1;
        std::string text(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, text.data(), size, nullptr, nullptr);
        text.pop_back();
        args.push_back(std::move(text));
    }
    return entry(args);
}
#else
int main(int argc, char** argv) { return entry({argv, argv + argc}); }
#endif
