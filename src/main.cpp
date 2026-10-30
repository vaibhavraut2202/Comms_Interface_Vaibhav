#include "http_server.hpp"

#include <charconv>
#include <csignal>
#include <iostream>
#include <string>

#include "command_queue.hpp"
#include "database.hpp"
#include "device_config.hpp"
#include "dispatcher.hpp"
#include "result_store.hpp"
#include "worker.hpp"
#include "../generated/DeviceBinding.nsmap"

namespace
{
volatile std::sig_atomic_t shutdownRequested = 0;

void requestShutdown(int)
{
    shutdownRequested = 1;
}

void printUsage(const char* executable)
{
    std::cout << "Usage: " << executable
              << " [--listen-address IPv4] [--port PORT]"
              << " [--camera-endpoint URL]\n";
}
}

int main(int argc, char* argv[])
{
    HttpServerConfig serverConfig;
    std::string cameraEndpoint = DeviceRegistry::defaultEndpoint;

    for (int i = 1; i < argc; ++i)
    {
        const std::string option = argv[i];
        if (option == "--help")
        {
            printUsage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc)
        {
            std::cerr << "Missing value for " << option << std::endl;
            printUsage(argv[0]);
            return 2;
        }

        const std::string value = argv[++i];
        if (option == "--listen-address")
        {
            serverConfig.listenAddress = value;
        }
        else if (option == "--port")
        {
            unsigned int port = 0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), port);
            if (parsed.ec != std::errc{}
                || parsed.ptr != value.data() + value.size()
                || port == 0 || port > 65535)
            {
                std::cerr << "Invalid port: " << value << std::endl;
                return 2;
            }
            serverConfig.port = static_cast<unsigned short>(port);
        }
        else if (option == "--camera-endpoint")
        {
            if (value.rfind("http://", 0) != 0)
            {
                std::cerr << "Camera endpoint must be an http:// URL" << std::endl;
                return 2;
            }
            cameraEndpoint = value;
        }
        else
        {
            std::cerr << "Unknown option: " << option << std::endl;
            printUsage(argv[0]);
            return 2;
        }
    }

    std::signal(SIGINT, requestShutdown);
    std::signal(SIGTERM, requestShutdown);
#ifdef SIGBREAK
    std::signal(SIGBREAK, requestShutdown);
#endif

    try
    {
        WinsockRuntime winsock;
        DeviceRegistry devices(cameraEndpoint);
        Database database;
        database.initialize();
        CommandQueue commandQueue;
        ResultStore resultStore;
        Dispatcher dispatcher(database);
        Worker worker(commandQueue, dispatcher, resultStore);
        HttpServer server(serverConfig, devices, commandQueue, resultStore);

        worker.start();
        const int serverResult = server.run(shutdownRequested);
        worker.stop();
        worker.join();
        return serverResult;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[Comms Interface] Fatal error: " << error.what() << std::endl;
        return 1;
    }
}
