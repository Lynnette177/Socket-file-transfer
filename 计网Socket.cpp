#include "server.h"
//#define SERVER
const char* CONFIG_FILE = "./config.ini";
const char* SECTION = "Network";
const char* KEY_IP = "IP";
const char* KEY_PORT = "Port";
const int BUFFER_SIZE = 256;
void readConfig(std::string& ip, std::string& port) {
    char ipBuffer[BUFFER_SIZE] = { 0 };
    char portBuffer[BUFFER_SIZE] = { 0 };

    GetPrivateProfileStringA(SECTION, KEY_IP, "", ipBuffer, BUFFER_SIZE, CONFIG_FILE);
    GetPrivateProfileStringA(SECTION, KEY_PORT, "", portBuffer, BUFFER_SIZE, CONFIG_FILE);

    ip = ipBuffer;
    port = portBuffer;
}

void writeConfig(const std::string& ip, const std::string& port) {
    WritePrivateProfileStringA(SECTION, KEY_IP, ip.c_str(), CONFIG_FILE);
    WritePrivateProfileStringA(SECTION, KEY_PORT, port.c_str(), CONFIG_FILE);
}

int main() {
    const wchar_t* Server_Address = L"127.0.0.1";
    int Server_Port = 5005;
#ifdef SERVER
    Server server(Server_Address, Server_Port);
    std::thread serverThread(&Server::start_server, &server);
    serverThread.detach(); //线程在后台运行
    while (!GetAsyncKeyState(VK_F1)) {}
    server.stop();
    return 0;
#else
    std::string ip, port;

    readConfig(ip, port);

    if (ip.empty() || port.empty() || GetAsyncKeyState(VK_CONTROL)) {
        std::cout << "Please enter IP address: ";
        std::getline(std::cin, ip);

        std::cout << "Please enter port number: ";
        std::getline(std::cin, port);

        writeConfig(ip, port);
    }
    else {
        std::cout << "IP: " << ip << "\nPort: " << port << std::endl;
    }
    Client client(ip.c_str(),std::stoi(port));
    while (!client.createProcessor()) {
        std::cout << "Please enter IP address: ";
        std::getline(std::cin, ip);

        std::cout << "Please enter port number: ";
        std::getline(std::cin, port);

        writeConfig(ip, port);
        std::cout << "IP: " << ip << "\nPort: " << port << std::endl << "Trying to establish connection..." << std::endl;
        client.ip = ip.c_str();
        client.port = std::stoi(port);
    }
    client.start_client();
    return 0;
#endif // SERVER
}
