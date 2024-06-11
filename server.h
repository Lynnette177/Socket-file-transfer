#pragma once
#include "includes.h"


#define LS_COMMAND "ls"
#define DOWNLOAD_COMMAND "get"
#define UPLOAD_COMMAND "upload"
#define DELETE_COMMAND "rm"
#define CD_COMMAND "cd"
#define MKDIR_COMMAND "mkdir"

constexpr char start_cm_response_send[3] = "\x01\xfe";
constexpr char start_bytearr_response_send[3] = "\x04\xfe";
constexpr char end_command_send[3] = "\x02\xff";
constexpr char end_bytearr_response_send[3] = "\x05\xff";

#define upload_timeout 5

namespace fs = std::filesystem;
void printProgressBar(double progress, float speed = 0.f) {
    int barWidth = 70; // Width of the progress bar
    std::cout << "[";
    int pos = barWidth * progress;
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << int(progress * 100.0) << " %" << "    " << speed << "MB/s           \r" << "           ";
    std::cout.flush();
}
int list_files_in_directory(const fs::path& directory, char** output) {
    std::ostringstream oss;
    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            fs::path relative_path = fs::relative(entry.path(), directory);
            try {
                if (fs::is_regular_file(entry.status())) {
                    oss << "File: " << relative_path.string() << '\n';
                }
                else if (fs::is_directory(entry.status())) {
                    oss << "Directory: " << relative_path.string() << '\n';
                }
                else {
                    oss << "Other: " << relative_path.string() << '\n';
                }
            }
            catch (const fs::filesystem_error& e) {
                // 如果访问单个条目时发生错误，记录错误信息但继续处理其他条目
                oss << "Error accessing: " << relative_path.string() << " - " << e.what() << '\n';
            }
        }
    }
    catch (const fs::filesystem_error& e) {
        // 如果目录本身不可访问，记录错误信息
        oss << "Error accessing directory: " << directory.string() << " - " << e.what() << '\n';
    }

    std::string oss_string = oss.str();
    *output = new char[oss_string.length() + 1];
    std::memcpy(*output, oss_string.c_str(), oss_string.length() + 1);

    return oss_string.length();
}
bool directoryExists(const fs::path& dir, const std::string& subDirName) {
    fs::path subDirPath = dir / subDirName;
    return fs::exists(subDirPath) && fs::is_directory(subDirPath);
}
bool fileExistsInDirectory(const fs::path& directory, const std::string& filename) {
    fs::path filePath = directory / filename;
    return fs::exists(filePath) && fs::is_regular_file(filePath);
}
void trimTrailingSpaces(char* str) {
    if (str == nullptr) return;

    size_t length = std::strlen(str);
    if (length == 0) return;

    // 从字符串末尾开始向前遍历
    char* end = str + length - 1;
    while (end >= str && std::isspace(static_cast<unsigned char>(*end))) {
        --end;
    }

    // 在找到的第一个非空格字符后插入终止符
    *(end + 1) = '\0';
}
bool createDirectory(const fs::path& directory) {
    try {
        if (fs::create_directory(directory)) {
            std::cout << "Directory created: " << directory << std::endl;
            return true;
        }
        else {
            std::cout << "Directory already exists: " << directory << std::endl;
            return false;
        }
    }
    catch (const fs::filesystem_error& e) {
        std::cerr << "Error creating directory: " << e.what() << std::endl;
        return false;
    }
}
bool startsWith(const char* str, const char* prefix) {
    size_t prefixLength = std::strlen(prefix);
    return std::strncmp(str, prefix, prefixLength) == 0;
}
class ClientHandler {
private:
    SOCKET clnt_sock;
    fs::path currentPath;
public:
    ClientHandler(SOCKET _clnt_sock) : clnt_sock(_clnt_sock) {
        printClientInfo(true);
    }

    bool send_message(const char* message, int command = 0) {
        try {
            int totalLength = strlen(message);
            int bytesSent = 0;
            //0代表发送带着开始和结尾的命令
            if (command == 0) {
                int iResult = send(clnt_sock,  start_cm_response_send , 2, 0);
                if (iResult == SOCKET_ERROR) {
                    throw std::runtime_error("Send failed: " + std::to_string(WSAGetLastError()));
                }
                Sleep(100);

                while (bytesSent < totalLength) {
                    int chunkSize = (totalLength - bytesSent > 1024) ? 1024 : totalLength - bytesSent;
                    int iResult = send(clnt_sock, message + bytesSent, chunkSize, 0);
                    if (iResult == SOCKET_ERROR) {
                        throw std::runtime_error("Send failed: " + std::to_string(WSAGetLastError()));
                    }
                    bytesSent += iResult;
                }
                Sleep(100);
                iResult = send(clnt_sock, end_command_send, 2, 0);
                if (iResult == SOCKET_ERROR) {
                    throw std::runtime_error("Send failed: " + std::to_string(WSAGetLastError()));
                }
                printf("Message sent: %s\n", message);
                return true;
            }
            else if (command == 1) {
                //1代表发送文件开头
                int iResult = send(clnt_sock, start_bytearr_response_send, 2, 0);
                if (iResult == SOCKET_ERROR) {
                    throw std::runtime_error("Send failed: " + std::to_string(WSAGetLastError()));
                }
            }
            else if (command == 2) {
                //2代表发送文件结尾
                int iResult = send(clnt_sock, end_bytearr_response_send, 2, 0);
                if (iResult == SOCKET_ERROR) {
                    throw std::runtime_error("Send failed: " + std::to_string(WSAGetLastError()));
                }
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Exception: " << e.what() << std::endl;
            closesocket(clnt_sock);
            WSACleanup();
            system("pause");
            abort();
            return false;
        }
    }

    void printClientInfo(bool init = false) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(clnt_sock, (struct sockaddr*)&addr, &addr_len) == 0) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(addr.sin_addr), ip, INET_ADDRSTRLEN);
            if (init)
                std::cout << "New Connection Established. Client IP: " << ip << ", Port: " << ntohs(addr.sin_port) << "\n";
            else
                std::cout << "Client IP: " << ip << ", Port: " << ntohs(addr.sin_port) << ":";
        }
        else {
            std::cerr << "Error getting client info" << std::endl;
        }
    }

    void handle() {
        try {
            char buffer[1024] = {};
            int bytes_received;
            currentPath = fs::current_path();
            std::cout << "\nWorking Path:" << currentPath << "\n\n";
            std::string abosolute_path(currentPath.string());
            abosolute_path = "P//" + abosolute_path;
            send_message(abosolute_path.c_str());
            while ((bytes_received = recv(clnt_sock, buffer, sizeof(buffer), 0)) > 0) {
                trimTrailingSpaces(buffer);
                printClientInfo();
                std::cout << buffer << std::endl;
                if (strcmp(buffer, LS_COMMAND) == 0) {
                    char* files = NULL;
                    std::cout << currentPath;
                    int file_name_length = list_files_in_directory(currentPath, &files);
                    if (file_name_length == 0)
                        send_message("Empty Directory");
                    else
                        send_message(files);
                    delete[] files;
                }
                else if (startsWith(buffer, DOWNLOAD_COMMAND)) {
                    char* files_ = std::strrchr(buffer, ' ');
                    files_ += 1;
                    if (files_ != nullptr) {
                        std::string filename((currentPath/files_).string());
                        std::ifstream file(filename, std::ios::binary | std::ios::ate);
                        if (file.is_open()) {
                            std::streampos fileSize = file.tellg(); // 获取文件大小，单位为字节
                            double fileSizeMB = static_cast<double>(fileSize) / (1024 * 1024); // 将字节转换为MB
                            double fileSizeBytes = static_cast<double>(fileSize);
                            std::cout << "\nFile size: " << fileSizeMB << " MB  " << fileSizeBytes <<"Bytes" << std::endl;
                            char filesizestr[100];
                            char filesizebytestr[100];
                            sprintf_s(filesizestr, "%.2f",fileSizeMB);
                            sprintf_s(filesizebytestr , "%d", (int)fileSizeBytes);
                            std::string filesizestring(filesizestr);
                            std::string filesizesbytestring(filesizebytestr);
                            file.seekg(0, std::ios::beg);
                            send_message(("SUCCESS_START_DOWNLOADING:" + filesizestring + "|" + filesizebytestr).c_str());
                            Sleep(100);
                            send_message("SUCCESS_RDY",1);
                            Sleep(100);
                            const std::size_t bufferSize = 1024;
                            char buffer[bufferSize];
                            while (file) {
                                file.read(buffer, bufferSize);
                                std::streamsize bytesRead = file.gcount();  // 获取实际读取的字节数
                                if (bytesRead > 0) {
                                    send(clnt_sock, buffer, bytesRead, 0);
                                }
                            }
                            file.close();
                            Sleep(100);
                            send_message("SUCCESS_RDY", 2);
                            Sleep(100);
                            send_message("SUCCESS_SEND_FINISH");
                        }
                        else {
                            send_message("FAIL_NOT_EXIST");
                        }
                    }
                    else send_message("FAIL_INVALID_COMMAND_FORMAT");
                }
                else if (startsWith(buffer, DELETE_COMMAND)) {
                    char* files_ = std::strchr(buffer, ' ');
                    files_ += 1;
                    std::cout << files_;
                    if (startsWith(files_, "-r")) {
                        files_ = std::strrchr(files_, ' ') + 1;
                        if (files_ != nullptr) {
                            std::string full_path = (currentPath / files_).string();
                            if (fs::is_directory(full_path)) {
                                if (fs::remove_all(full_path) > 0)
                                    send_message("SUCCESS_DELETED_DIR");
                                else
                                    send_message("FAIL_UNABLE_DELETE_DIR");
                            }
                            else
                                send_message("FAIL_NOT_A_DIR");
                        }
                        else send_message("FAIL_INVALID_COMMAND_FORMAT");
                    }
                    else {
                        if (files_ != nullptr) {
                            std::string full_path = (currentPath / files_).string();
                            if (std::remove(full_path.c_str()) != 0) {
                                send_message("FAIL_UNABLE_DELETE");
                            }
                            else {
                                send_message("SUCCESS_DELETED");
                            }
                        }
                        else send_message("FAIL_INVALID_COMMAND_FORMAT");
                    }
                }
                else if (startsWith(buffer, UPLOAD_COMMAND)) {
                    bool overwrite = false;
                    char* files_ = std::strchr(buffer, ' ');
                    files_ += 1;
                    std::cout << files_;
                    if (startsWith(files_, "-o")) {
                        files_ = std::strrchr(files_, ' ') + 1;
                        overwrite = true;
                    }
                    std::string r_file_name(files_);
                    if (fileExistsInDirectory(currentPath, files_) && !overwrite) {
                        send_message("FAIL_EXISTED_NO_OVERWRITE");
                        memset(buffer, 0, 1024);//处理结束清空缓冲区
                        continue;
                    }
                    r_file_name = (currentPath / files_).string();
                    send_message("SUCCESS_START_SEND");
                    std::vector<char> received_data;
                    int timeout = upload_timeout * 1000;
                    setsockopt(clnt_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
                    while (1) {
                        int recv_byte_num = recv(clnt_sock, buffer, sizeof(buffer), 0);
                        if (recv_byte_num == -1) {
                            send_message("FAIL_RECV_TIMEOUT");
                            break;
                        }
                        else if (recv_byte_num == 2) {
                            if (buffer[0] == end_bytearr_response_send[0] && buffer[1] == end_bytearr_response_send[1]) {
                                std::ofstream outfile(r_file_name.c_str(), std::ios::binary);
                                if (outfile.is_open()) {
                                    outfile.write(received_data.data(), received_data.size());
                                    outfile.close();
                                    std::cout << "Data saved" << std::endl;
                                    send_message("SUCCESS_FINISH_RECV");
                                    received_data.clear();
                                    break;
                                }
                                else {
                                    std::cerr << "Failed to open file for writing" << std::endl;
                                    send_message("FAIL_WRITING_FILE");
                                }
                                received_data.clear();
                            }
                            else if (buffer[0] == start_bytearr_response_send[0] && buffer[1] == start_bytearr_response_send[1]) {
                                printf("\n\nstart receiving file\n\n");
                                received_data.clear();
                            }
                        }
                        else {
                            received_data.insert(received_data.end(), buffer, buffer + recv_byte_num);
                        }
                    }
                    timeout = 0;
                    setsockopt(clnt_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
                }
                else if (startsWith(buffer, CD_COMMAND)) {
                    if (strcmp(buffer, CD_COMMAND) == 0) {
                        currentPath = fs::current_path();
                        std::cout << "\nBack to Working Path:" << currentPath << "\n\n";
                        std::string tmp_path(currentPath.string());
                        tmp_path = "P//" + tmp_path;
                        send_message(tmp_path.c_str());
                    }
                    else {
                        char* files_ = std::strrchr(buffer, ' ');
                        files_ += 1;
                        if (startsWith(files_, "/") && strlen(files_)>1) {
                            files_ += 1;
                            if (fs::exists(files_) && fs::is_directory(files_)) {
                                currentPath = files_;
                                std::cout << currentPath;
                                std::cout << "\nEnter absolute Path:" << currentPath << "\n\n";
                                std::string tmp_path(currentPath.string());
                                tmp_path = "P//" + tmp_path;
                                send_message(tmp_path.c_str());
                            }
                            else
                                send_message("FAIL_NO_DIR");
                        }
                        else if (strcmp(files_, "..") == 0) {
                            currentPath = currentPath.parent_path();
                            std::string new_path(currentPath.string());
                            new_path = "P//" + new_path;
                            send_message(new_path.c_str());
                        }
                        else if (directoryExists(currentPath, files_)) {
                            currentPath /= files_; // 将 dir 变量加上 subDirName
                            std::string new_path(currentPath.string());
                            new_path = "P//" + new_path;
                            send_message(new_path.c_str());
                        }
                        else send_message("FAIL_NO_DIR");
                    }
                }
                else if (startsWith(buffer, MKDIR_COMMAND)) {
                    char* files_ = std::strrchr(buffer, ' ');
                    files_ += 1;
                    if (files_ != nullptr) {
                        if (createDirectory(currentPath/files_)) {
                            send_message("SUCCESS_CREATED");
                        }
                        else
                            send_message("FAIL_UNABLE_CREATE_DIR");
                    }
                    else send_message("FAIL_INVALID_COMMAND_FORMAT");
                    }
                else
                    send_message("FAIL_INVALID_COMMAND");
                memset(buffer, 0, 1024);//处理结束清空缓冲区
            }

            closesocket(clnt_sock);
        }
        catch (const std::exception& e) {
            std::cerr << "Exception in client_handler: " << e.what() << std::endl;
        }
    }
};

class Server {
private:
    const wchar_t* ip;
    int port;
    SOCKET serv_sock;
    std::vector<std::thread> client_threads;
    std::atomic<bool> stop_server;

public:
    Server(const wchar_t* _ip, int _port) : ip(_ip), port(_port), serv_sock(INVALID_SOCKET), stop_server(false) {}

    bool initialize() {
        WSADATA wsadata;
        WORD w_req = MAKEWORD(2, 2);
        int err_code = WSAStartup(w_req, &wsadata);
        if (err_code != 0) {
            printf("Initialize failed.\n");
            return false;
        }
        else {
            printf("Initialized.\n");
        }
        if (LOBYTE(wsadata.wVersion) != 2 || HIBYTE(wsadata.wHighVersion) != 2) {
            printf("Version initialize failed.\n");
            WSACleanup();
            return false;
        }
        else {
            printf("All done\n");
            return true;
        }
    }

    void start_server() {
        if (!initialize()) {
            std::cerr << "Server initialization failed." << std::endl;
            return;
        }

        std::cout << "Server Starting" << std::endl;

        serv_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (serv_sock == INVALID_SOCKET) {
            std::cerr << "Socket creation failed." << std::endl;
            return;
        }

        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(port);

        int iResult = bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        if (iResult == SOCKET_ERROR) {
            std::cerr << "Bind failed: " << WSAGetLastError() << std::endl;
            return;
        }

        iResult = listen(serv_sock, 20);
        if (iResult == SOCKET_ERROR) {
            std::cerr << "Listen failed: " << WSAGetLastError() << std::endl;
            return;
        }
        std::cout << "Server Started" << std::endl;

        while (!stop_server) {
            struct sockaddr_in clnt_addr;
            socklen_t clnt_addr_size = sizeof(clnt_addr);
            SOCKET clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
            if (clnt_sock == INVALID_SOCKET) {
                std::cerr << "Accept failed or manually exited." << std::endl;
                continue;
            }
            client_threads.emplace_back([clnt_sock]() { ClientHandler handler(clnt_sock); handler.handle(); });
        }
    }

    void stop() {
        stop_server = true;

        for (auto& thread : client_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        if (serv_sock != INVALID_SOCKET) {
            closesocket(serv_sock);
            serv_sock = INVALID_SOCKET;
        }
        WSACleanup();
    }

    ~Server() {
        stop();
    }
};


class ClientProcessor {
private:
    SOCKET servers_sock;
    std::atomic<bool> stop_handler;
public:
    std::string file_size = "";
    int able_to_upload = 0;
    int able_to_download = 0;
    double last_progess = 0;
    fs::path download_file_path = "";
    fs::path currentPath = "command";//server path
    ClientProcessor(SOCKET _servers_sock) : servers_sock(_servers_sock), stop_handler(false) {}

    void handle() {
        char buffer[1024] = {};
        int bytes_received = 0;
        while ((bytes_received = recv(servers_sock, buffer, sizeof(buffer), 0)) > 0) {
            if (bytes_received == 2 && strncmp(buffer,start_cm_response_send,2) == 0) {
                while (true) {
                    bytes_received = recv(servers_sock, buffer, sizeof(buffer), 0);
                    if (bytes_received == 2 && strncmp(buffer, end_command_send, 2) == 0) {
                        break;
                    }
                    trimTrailingSpaces(buffer);
                    std::string message(buffer);
                    if (startsWith(buffer, "P//")) {
                        currentPath = buffer + 3;
                        std::cout << std::endl << currentPath.string() << ">";
                    }  //处理目录信息
                    else if (strncmp(message.c_str(), "SUCCESS_START_SEND", 18) == 0) { //处理开始发送
                        if (this->able_to_upload == 1) {
                            this->able_to_upload = 2;
                            printf("\n[-]Client:: Start Uploading\n");
                        }
                    }
                    else if (strncmp(message.c_str(), "FAIL_EXISTED_NO_OVERWRITE", 25) == 0) {
                        if (this->able_to_upload == 1) {
                            this->able_to_upload = 3;
                            printf("\n[-]Client:: error:file existed without overwrite option\n");
                        }
                    }
                    else if (strncmp(message.c_str(), "SUCCESS_START_DOWNLOADING", 21) == 0) {
                        std::size_t start_filestart = message.find(':');
                        std::size_t end_fileend = message.find('|');
                        std::string filesize_ = message.substr(start_filestart + 1, end_fileend - start_filestart - 1);
                        std::string filesizebytes_ = std::strrchr(message.c_str(), '|') + 1;
                        std::cout << "File Size: " << filesize_ << " MB";
                        std::cout << "File Size(Bytes): " << filesizebytes_ << " Bytes";
                        this->file_size = filesize_;
                    }
                    else {
                        message = "[+]Server:: " + message;
                        std::cout << message;
                        std::cout << std::endl << currentPath.string() << ">";
                    }
                }
            }
            else if (bytes_received == 2 && strncmp(buffer, start_bytearr_response_send, 2) == 0) {
                if (this->able_to_download == 1) {
                    std::vector<char> received_data;
                    int timeout = upload_timeout * 1000;
                    setsockopt(servers_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
                    size_t received_file_size = 0;
                    std::cout << std::endl;
                    while (1) {
                        int recv_byte_num = recv(servers_sock, buffer, sizeof(buffer), 0);
                        static double last_download_size = 0.f;
                        static unsigned int last_time = GetTickCount();
                        if (recv_byte_num == -1) {
                            printf("Download Fail timeout --Client");
                            break;
                        }
                        else if (recv_byte_num == 2) {
                            if (buffer[0] == end_bytearr_response_send[0] && buffer[1] == end_bytearr_response_send[1]) {
                                std::ofstream outfile(this->download_file_path.c_str(), std::ios::binary);
                                last_download_size = 0;
                                last_time = 0;
                                printProgressBar(1);
                                printf("\nConfirming...\n");
                                if (outfile.is_open()) {
                                    outfile.write(received_data.data(), received_data.size());
                                    outfile.close();
                                    std::cout << "\n[-]Client:: Data saved  "<< this->download_file_path <<"\n" << std::endl;
                                    received_data.clear();
                                    std::cout << this->currentPath.string() << ">";
                                    break;
                                }
                                else {
                                    std::cerr << "Failed to open file for writing" << std::endl;
                                    printf("[-]Client:: Download Fail Error Writing\n");
                                }
                                received_data.clear();
                            }
                            else if (buffer[0] == start_bytearr_response_send[0] && buffer[1] == start_bytearr_response_send[1]) {
                                printf("\n\n[-]Client:: start receiving file\n\n");
                                received_data.clear();
                            }
                        }
                        else {
                            received_file_size += recv_byte_num;
                            if (received_file_size / 1024 / 1024 / std::stod(file_size) - last_progess > 0.01) {
                                unsigned int timediff = GetTickCount() - last_time;
                                last_time = GetTickCount();
                                float speed = 0.f;
                                if (timediff > 0)
                                    speed = (received_file_size - last_download_size) / timediff / 1024 / 1024 * 1000;
                                last_download_size = received_file_size;
                                printProgressBar(received_file_size / 1024 / 1024 / std::stod(file_size), speed);
                                last_progess = received_file_size / 1024 / 1024 / std::stod(file_size);
                            }
                            received_data.insert(received_data.end(), buffer, buffer + recv_byte_num);
                        }
                    }
                    timeout = 0;
                    setsockopt(servers_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
                    this->able_to_download = 0;
                    received_file_size = 0;
                    this->last_progess = 0;
                }
            }
            memset(buffer, 0, 1024);
        }
        closesocket(servers_sock);
    }

    void stop() {
        stop_handler = true;
    }
};

class Client {
private:
    
    SOCKET servers_sock;
    std::atomic<bool> stop_client;
    std::thread input_thread;
    std::thread recv_thread;
    ClientProcessor* C_process; // Use pointer to manage lifetime

public:
    const char* ip;
    int port;
    bool initialize() {
        WSADATA wsadata;
        WORD w_req = MAKEWORD(2, 2);
        int err_code = WSAStartup(w_req, &wsadata);
        if (err_code != 0) {
            std::cerr << "Initialize failed." << std::endl;
            return false;
        }
        else {
            std::cout << "Initialized." << std::endl;
        }
        if (LOBYTE(wsadata.wVersion) != 2 || HIBYTE(wsadata.wHighVersion) != 2) {
            std::cerr << "Version initialize failed." << std::endl;
            WSACleanup();
            return false;
        }
        else {
            std::cout << "All done" << std::endl;
            return true;
        }
    }
    Client(const char* _ip, int _port) : ip(_ip), port(_port), servers_sock(INVALID_SOCKET), stop_client(false), C_process(nullptr) {
        initialize();
    }

    ~Client() {
        stop();
    }

    bool createProcessor() {
        servers_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (servers_sock == INVALID_SOCKET) {
            std::cerr << "Socket creation failed." << std::endl;
            return false;
        }

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        inet_pton(AF_INET, ip, &(server_addr.sin_addr));
        server_addr.sin_port = htons(port);

        if (connect(servers_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
            std::cerr << "Connect failed with error: " << WSAGetLastError() << std::endl;
            closesocket(servers_sock);
            return false;
        }
        C_process = new ClientProcessor(servers_sock);
        recv_thread = std::thread(&ClientProcessor::handle, C_process);
        recv_thread.detach();
        printf("Launched thread\n");
        return true;
    }

    void start_client() {
        input_handler();
    }

    void stop() {
        stop_client = true;
        closesocket(servers_sock);
        WSACleanup();
    }

private:
    void input_handler() {
        while (!stop_client) {
            std::string message;
            //std::cout << C_process->currentPath.string() << ">";
            std::getline(std::cin, message);
            if (message == "exit") {
                stop_client = true;
                break;
            }
            else if (startsWith(message.c_str(), "upload")) {
                double uploaded_size = 0.f;
                float upload_progess = 0.f;
                double total_upload_size = 0.f;

                char* files_ = new char[sizeof(message)];
                memcpy(files_, message.c_str(),sizeof(message));
                files_ = std::strrchr(files_, ' ') + 1;
                if (files_ != nullptr) {
                    std::string filename((fs::current_path() / files_).string());
                    std::ifstream file(filename, std::ios::binary | std::ios::ate);
                    if (file.is_open()) {

                        std::streampos fileSize = file.tellg(); // 获取文件大小，单位为字节
                        total_upload_size = static_cast<double>(fileSize) / (1024 * 1024); // 将字节转换为MB
                        std::cout << "\n[-] Upload file size: " << total_upload_size << " MB" << std::endl;
                        file.seekg(0, std::ios::beg);

                        C_process->able_to_upload = 1;
                        send(servers_sock, message.c_str(), message.size(), 0);
                        unsigned __int64 start_time = GetTickCount64();
                        while (C_process->able_to_upload == 1) {
                            if (GetTickCount64() - start_time > 5000) {
                                printf("Waiting upload response timeout.");
                                C_process->able_to_upload = 0;
                            }
                            Sleep(100);
                        }
                        if (C_process->able_to_upload == 2) {

                            int iResult = send(servers_sock, start_bytearr_response_send, 2, 0);
                            if (iResult == SOCKET_ERROR) {
                                throw std::runtime_error("Send failed: " + std::to_string(WSAGetLastError()));
                            }
                            Sleep(100);
                            const std::size_t bufferSize = 1024;
                            char buffer[bufferSize];
                            while (file) {
                                static unsigned __int64 last_time = GetTickCount();
                                static double last_upload_tsize = 0;
                                file.read(buffer, bufferSize);
                                std::streamsize bytesRead = file.gcount();  // 获取实际读取的字节数
                                if (bytesRead > 0) {
                                    send(servers_sock, buffer, bytesRead, 0);
                                }
                                uploaded_size += bytesRead;
                                if (uploaded_size / 1024 / 1024 / total_upload_size - upload_progess > 0.01) {
                                    unsigned int time_diff = GetTickCount() - last_time;
                                    last_time = GetTickCount();
                                    float speed = 0;
                                    if (time_diff > 0) {
                                        speed = (uploaded_size - last_upload_tsize) / time_diff / 1024 / 1024 * 1000;
                                        last_upload_tsize = uploaded_size;
                                    }
                                    printProgressBar(uploaded_size / 1024 / 1024 / total_upload_size, speed);
                                    upload_progess = uploaded_size / 1024 / 1024 / total_upload_size;
                                }
                            }
                            file.close();
                            printProgressBar(1);//完成传输
                            printf("\nConfirming...\n");
                            Sleep(100);
                            iResult = send(servers_sock, end_bytearr_response_send, 2, 0);
                            if (iResult == SOCKET_ERROR) {
                                throw std::runtime_error("Send failed: " + std::to_string(WSAGetLastError()));
                            }
                            C_process->able_to_upload = 0;
                        }
                        else std::cout << C_process->currentPath.string() << ">";
                    }
                    else std::cout << C_process->currentPath.string() << ">";
                }
                else std::cout << C_process->currentPath.string() << ">";
                continue;
            }
            else if (startsWith(message.c_str(), "get")) {
                char* files_ = new char[sizeof(message)];
                memcpy(files_, message.c_str(), sizeof(message));
                files_ = std::strchr(files_, ' ') + 1;
                if (startsWith(files_, "-o")) {
                    files_ = std::strrchr(files_, ' ') + 1;
                    C_process->download_file_path = fs::current_path() / files_;
                    if (fileExistsInDirectory(fs::current_path(), files_)) {
                        printf("[-]Ignore Existed File and Save with .L\n");
                        std::string new_file_name(files_);
                        new_file_name += ".L";
                        C_process->download_file_path.replace_filename(new_file_name);
                    }
                    C_process->able_to_download = 1;
                    send(servers_sock, message.c_str(), message.size(), 0);
                }
                else {
                    if (fileExistsInDirectory(fs::current_path(), files_)) {
                        printf("[-] Client:: File exists in local\n");
                        std::cout << C_process->currentPath.string() << ">";
                    }
                    else if (files_ != nullptr) {
                        C_process->download_file_path = fs::current_path() / files_;
                        C_process->able_to_download = 1;
                        send(servers_sock, message.c_str(), message.size(), 0);
                    }
                }
                continue;
            }
            else
                send(servers_sock, message.c_str(), message.size(), 0);
        }
    }
};

