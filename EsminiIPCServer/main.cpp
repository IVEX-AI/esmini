
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
                std::cerr << "Connection closed by peer." << std::endl;
            }
            else
            {
                std::cerr << "Failed to receive data. Error: " << strerror(errno) << std::endl;
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
    std::cout << "Waiting to receive message length..." << std::endl;
    if (!recv_all(sockfd, &message_length_network, sizeof(message_length_network)))
    {
        std::cerr << "Failed to receive message length." << std::endl;
        return false;
    }

    uint32_t message_length = ntohl(message_length_network);
    std::cout << "Received message length: " << message_length << " bytes" << std::endl;

    if (message_length == 0 || message_length > 1048576)
    {  // 1MB limit for debugging
        std::cerr << "Received message length is invalid or too large." << std::endl;
        return false;
    }

    std::vector<char> buffer(message_length);
    std::cout << "Waiting to receive message body..." << std::endl;
    if (!recv_all(sockfd, buffer.data(), message_length))
    {
        std::cerr << "Failed to receive message body." << std::endl;
        return false;
    }

    std::string message(buffer.begin(), buffer.end());
    std::cout << "Received raw message: " << message << std::endl;

    try
    {
        j = json::parse(message);
    }
    catch (const json::parse_error &e)
    {
        std::cerr << "JSON parsing error: " << e.what() << std::endl;
        return false;
    }

    return true;
}

void handle_init(const json &j)
{
    std::cout << "Handling Init: " << j.dump(4) << std::endl;
    json        init_args     = j["args"];
    std::string xosc_path_str = init_args["xosc_path"];
    std::cout << "AAAAAAAA " << xosc_path_str << std::endl;
    SE_Init(xosc_path_str.c_str(), init_args["disable_ctrls"], init_args["use_viewer"], init_args["threads"], init_args["record"]);
}

void handle_send_pos(const json &j)
{
    std::cout << "Handling SendPos: " << j.dump(4) << std::endl;
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
    std::cout << "Handling Step: " << j.dump(4) << std::endl;
    SE_Step();
}

void handle_close(const json &j)
{
    std::cout << "Handling Close: " << j.dump(4) << std::endl;
    SE_Close();
}

void handle_client(int client_sockfd)
{
    json received_json;
    while (true)
    {
        std::cout << "Waiting to receive a JSON message..." << std::endl;
        if (!recv_json(client_sockfd, received_json))
        {
            std::cerr << "Failed to receive or parse JSON message. Closing connection." << std::endl;
            break;
        }

        std::string function = received_json.value("function", "");
        std::cout << "Received function: " << function << std::endl;

        if (function == "SE_Init")
        {
            std::cout << "Handling Init message." << std::endl;
            handle_init(received_json);
        }
        else if (function == "SE_ReportObjectPosXYH")
        {
            std::cout << "Handling SendPos message." << std::endl;
            handle_send_pos(received_json);
        }
        else if (function == "SE_Step")
        {
            std::cout << "Handling Step message." << std::endl;
            handle_step(received_json);
        }
        else if (function == "SE_Close")
        {
            std::cout << "Handling Close message." << std::endl;
            handle_close(received_json);
        }
        else
        {
            std::cerr << "Unknown function received: " << function << std::endl;
        }
    }
    close(client_sockfd);
}

int main()
{
    const int server_port   = 8080;
    int       server_sockfd = -1, client_sockfd = -1;

    server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sockfd == -1)
    {
        std::cerr << "Socket creation failed. Error: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0)
    {
        std::cerr << "setsockopt failed. Error: " << strerror(errno) << std::endl;
        close(server_sockfd);
        return EXIT_FAILURE;
    }

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(server_port);

    if (bind(server_sockfd, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
    {
        std::cerr << "Binding failed. Error: " << strerror(errno) << std::endl;
        close(server_sockfd);
        return EXIT_FAILURE;
    }

    if (listen(server_sockfd, 5) < 0)
    {
        std::cerr << "Listening failed. Error: " << strerror(errno) << std::endl;
        close(server_sockfd);
        return EXIT_FAILURE;
    }

    std::cout << "Server listening on port " << server_port << std::endl;

    while (true)
    {
        sockaddr_in client_addr;
        socklen_t   client_addr_len = sizeof(client_addr);
        client_sockfd               = accept(server_sockfd, reinterpret_cast<sockaddr *>(&client_addr), &client_addr_len);
        if (client_sockfd < 0)
        {
            std::cerr << "Accept failed. Error: " << strerror(errno) << std::endl;
            continue;
        }

        std::cout << "Accepted connection from " << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port) << std::endl;

        handle_client(client_sockfd);
    }

    close(server_sockfd);
    return EXIT_SUCCESS;
}

// void handle_client(int client_sock)
// {
//     while (true)
//     {
//         int     message_length;
//         ssize_t received = recv(client_sock, &message_length, sizeof(message_length), 0);

//         if (received <= 0)
//         {
//             std::cerr << "Error receiving message length or client disconnected" << std::endl;
//             break;
//         }

//         message_length = ntohl(message_length);  // Convert from network to host byte order

//         char *buffer = new char[message_length + 1];
//         received     = recv(client_sock, buffer, message_length, 0);

//         if (received <= 0)
//         {
//             std::cerr << "Error receiving message body" << std::endl;
//             delete[] buffer;
//             break;
//         }

//         buffer[message_length] = '\0';  // Null-terminate the received string
//         std::string received_data(buffer);
//         delete[] buffer;

//         // Parse the JSON message
//         json j = json::parse(received_data);

//         // Determine the function type and call the appropriate handler
//         std::string function_name = j["function"];
//         if (function_name == "SE_Init")
//         {
//             handle_init(j);
//         }
//         else if (function_name == "SE_ReportObjectPosXYH")
//         {
//             handle_send_pos(j);
//         }
//         else if (function_name == "SE_Step")
//         {
//             handle_step(j);
//         }
//         else if (function_name == "SE_Close")
//         {
//             handle_close(j);
//         }
//         else
//         {
//             std::cerr << "Unknown function: " << function_name << std::endl;
//         }

//         if (function_name == "SE_Close")
//         {
//             break;  // Exit loop after handling "Close" function
//         }
//     }
// }

// int main()
// {
//     int                server_fd, new_socket;
//     struct sockaddr_in address;
//     int                addrlen = sizeof(address);

//     server_fd = socket(AF_INET, SOCK_STREAM, 0);
//     if (server_fd == 0)
//     {
//         std::cerr << "Socket failed" << std::endl;
//         return -1;
//     }

//     address.sin_family      = AF_INET;
//     address.sin_addr.s_addr = INADDR_ANY;
//     address.sin_port        = htons(8080);

//     if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
//     {
//         std::cerr << "Bind failed" << std::endl;
//         return -1;
//     }

//     if (listen(server_fd, 3) < 0)
//     {
//         std::cerr << "Listen failed" << std::endl;
//         return -1;
//     }

//     while (true)
//     {
//         std::cout << "Waiting for a connection..." << std::endl;
//         new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
//         if (new_socket < 0)
//         {
//             std::cerr << "Accept failed" << std::endl;
//             return -1;
//         }

//         handle_client(new_socket);
//         close(new_socket);
//     }

//     return 0;
// }

// // int main(int argc, char *argv[])
// // {
// //     SE_Init("../../resources/xosc/ivex_test.xosc", 1, 1, 0, 0);

// //     SE_ReportObjectPosXYH(SE_GetIdByName("Ego"), 0, 603.0f, 1091.0f, 7.0f);
// //     SE_Step();

// //     std::this_thread::sleep_for(std::chrono::seconds(1));

// //     SE_ReportObjectPosXYH(SE_GetIdByName("Ego"), 0, 623.0f, 1091.0f, 7.0f);
// //     SE_Step();
// //     std::this_thread::sleep_for(std::chrono::seconds(1));

// //     SE_Close();

// //     return 0;
// // }
