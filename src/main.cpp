#include <iostream>
#include <boost/asio.hpp>
#include <cstdlib>

// Declaração da função run_server que será definida em async_echo_server.cpp
int run_server(int port);

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: data_server <port>\n";
        return 1;
    }

    int port = std::atoi(argv[1]);
    return run_server(port);
}
