
#include "esminiLib.hpp"
#include <thread>  // For std::this_thread::sleep_for
#include <chrono>  // For std::chrono::seconds
#include <iostream>
#include <iostream>
#include <string>
#include <arpa/inet.h>        // For socket functions
#include <unistd.h>           // For close()
#include <nlohmann/json.hpp>  // For JSON handling

using json = nlohmann::json;

#define PORT 8080

bool recv_all(int sockfd, void *buffer, size_t length)
{
    size_t total_received = 0;
    char  *buf            = static_cast<char *>(buffer);

    while (total_received < length)
    {
        ssize_t received = recv(sockfd, buf + total_received, length - total_received, 0);
        if (received <= 0)
        {
            if (received == 0)
            {
                std::cerr << "[EsminiIPCServer] Connection closed by peer." << std::endl;
            }
            else
            {
                std::cerr << "[EsminiIPCServer] Failed to receive data. Error: " << strerror(errno) << std::endl;
            }
            return false;
        }
        total_received += received;
    }
    return true;
}

bool recv_json(int sockfd, json &j)
{
    uint32_t message_length_network;

    // Receive message length
    std::cout << "[EsminiIPCServer] Waiting to receive message length..." << std::endl;
    if (!recv_all(sockfd, &message_length_network, sizeof(message_length_network)))
    {
        std::cerr << "[EsminiIPCServer] Failed to receive message length." << std::endl;
        return false;
    }

    uint32_t message_length = ntohl(message_length_network);
    std::cout << "[EsminiIPCServer] Received message length: " << message_length << " bytes" << std::endl;

    if (message_length == 0 || message_length > 1048576)
    {  // 1MB limit
        std::cerr << "[EsminiIPCServer] Received message length is invalid or too large." << std::endl;
        return false;
    }

    std::vector<char> buffer(message_length);
    std::cout << "[EsminiIPCServer] Waiting to receive message body..." << std::endl;
    if (!recv_all(sockfd, buffer.data(), message_length))
    {
        std::cerr << "[EsminiIPCServer] Failed to receive message body." << std::endl;
        return false;
    }

    std::string message(buffer.begin(), buffer.end());
    std::cout << "[EsminiIPCServer] Received raw message: " << message << std::endl;

    try
    {
        j = json::parse(message);
    }
    catch (const json::parse_error &e)
    {
        std::cerr << "[EsminiIPCServer] JSON parsing error: " << e.what() << std::endl;
        return false;
    }

    return true;
}

void handle_init(const json &j)
{
    json        init_args     = j["args"];
    std::string xosc_path_str = init_args["xosc_path"];
    SE_Init(xosc_path_str.c_str(), init_args["disable_ctrls"], init_args["use_viewer"], init_args["threads"], init_args["record"]);
}

void handle_send_pos(const json &j)
{
    json        object_pos_args = j["args"];
    std::string object_id       = object_pos_args["object_id"];

    SE_ReportObjectPosXYH(SE_GetIdByName(object_id.c_str()),
                          object_pos_args["timestamp"],
                          object_pos_args["x"],
                          object_pos_args["y"],
                          object_pos_args["h"]);
}

void handle_step(const json &j)
{
    SE_Step();
}

void handle_close(const json &j)
{
    SE_Close();
}

bool handle_client(int client_sockfd)
{
    json received_json;
    while (true)
    {
        std::cout << "[EsminiIPCServer] Waiting to receive a JSON message..." << std::endl;
        if (!recv_json(client_sockfd, received_json))
        {
            std::cerr << "[EsminiIPCServer] Failed to receive or parse JSON message. Closing connection." << std::endl;
            break;
        }

        std::string function = received_json.value("function", "");
        std::cout << "[EsminiIPCServer] Received function: " << function << std::endl;

        if (function == "SE_Init")
        {
            std::cout << "[EsminiIPCServer] Handling Init message." << std::endl;
            handle_init(received_json);
        }
        else if (function == "SE_ReportObjectPosXYH")
        {
            std::cout << "[EsminiIPCServer] Handling SendPos message." << std::endl;
            handle_send_pos(received_json);
        }
        else if (function == "SE_Step")
        {
            std::cout << "[EsminiIPCServer] Handling Step message." << std::endl;
            handle_step(received_json);
        }
        else if (function == "SE_Close")
        {
            std::cout << "[EsminiIPCServer] Handling Close message." << std::endl;
            handle_close(received_json);
            break;
        }
        else
        {
            std::cerr << "[EsminiIPCServer] Unknown function received: " << function << std::endl;
        }
    }
    close(client_sockfd);
    return true;
}

int main()
{
    int server_sockfd = -1, client_sockfd = -1;

    server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sockfd == -1)
    {
        std::cerr << "[EsminiIPCServer] Socket creation failed. Error: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0)
    {
        std::cerr << "[EsminiIPCServer] setsockopt failed. Error: " << strerror(errno) << std::endl;
        close(server_sockfd);
        return EXIT_FAILURE;
    }

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(PORT);

    if (bind(server_sockfd, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
    {
        std::cerr << "[EsminiIPCServer] Binding failed. Error: " << strerror(errno) << std::endl;
        close(server_sockfd);
        return EXIT_FAILURE;
    }

    if (listen(server_sockfd, 5) < 0)
    {
        std::cerr << "[EsminiIPCServer] Listening failed. Error: " << strerror(errno) << std::endl;
        close(server_sockfd);
        return EXIT_FAILURE;
    }

    std::cout << "[EsminiIPCServer] Server listening on port " << PORT << std::endl;

    bool handle_client_complete = false;
    while (true)
    {
        sockaddr_in client_addr;
        socklen_t   client_addr_len = sizeof(client_addr);
        client_sockfd               = accept(server_sockfd, reinterpret_cast<sockaddr *>(&client_addr), &client_addr_len);
        if (client_sockfd < 0)
        {
            std::cerr << "[EsminiIPCServer] Accept failed. Error: " << strerror(errno) << std::endl;
            continue;
        }

        std::cout << "[EsminiIPCServer] Accepted connection from " << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port)
                  << std::endl;

        handle_client_complete = handle_client(client_sockfd);

        if (handle_client_complete)
        {
            break;
        }
    }

    close(server_sockfd);
    return EXIT_SUCCESS;
}
