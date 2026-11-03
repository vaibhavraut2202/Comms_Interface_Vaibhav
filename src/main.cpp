#include "http_server.hpp"

#include <charconv>
#include <csignal>
#include <iostream>
#include <optional>
#include <string>

#include "command_queue.hpp"
#include "database.hpp"
#include "device_config.hpp"
#include "dispatcher.hpp"
#include "mqtt_client.hpp"
#include "result_store.hpp"
#include "request_submission.hpp"
#include "worker.hpp"
#include "ws_discovery.hpp"
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
              << " [--onvif-port PORT] [--mqtt-host HOST]"
              << " [--discovery-soap-target-ip PRIVATE_IPV4]"
              << " [--mqtt-port PORT] [--mqtt-client-id ID]"
              << " [--mqtt-command-topic TOPIC] [--mqtt-ack-topic TOPIC]"
              << " [--mqtt-response-topic TOPIC] [--mqtt-status-topic TOPIC]"
              << " [--mqtt-qos 0|1|2]"
              << " [--mqtt-reconnect-min SECONDS]"
              << " [--mqtt-reconnect-max SECONDS]\n";
}

class MqttShutdownGuard
{
private:
    MqttClient& mqttClient;
    ResultStore& resultStore;

public:
    MqttShutdownGuard(MqttClient& client, ResultStore& results)
        : mqttClient(client), resultStore(results)
    {
    }

    ~MqttShutdownGuard() noexcept
    {
        mqttClient.stop();
        try
        {
            resultStore.setCompletionCallback({});
        }
        catch (const std::exception& error)
        {
            std::cerr << "[Comms Interface] Unable to clear MQTT completion "
                      << "callback: " << error.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "[Comms Interface] Unable to clear MQTT completion "
                      << "callback: unknown exception" << std::endl;
        }
    }
};
}

int main(int argc, char* argv[])
{
    HttpServerConfig serverConfig;
    MqttClientConfig mqttConfig;
    DeviceConfig deviceConfig;
    std::optional<std::string> discoverySoapTargetIp;

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
        else if (option == "--onvif-port")
        {
            unsigned int port = 0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), port);
            if (parsed.ec != std::errc{}
                || parsed.ptr != value.data() + value.size()
                || port == 0 || port > 65535)
            {
                std::cerr << "Invalid ONVIF port: " << value << std::endl;
                return 2;
            }
            deviceConfig.onvifPort = static_cast<unsigned short>(port);
        }
        else if (option == "--discovery-soap-target-ip")
        {
            if (!WsDiscovery::isPrivateIpv4Address(value))
            {
                std::cerr << "Discovery SOAP target must be an RFC1918 IPv4 "
                          << "address" << std::endl;
                return 2;
            }
            discoverySoapTargetIp = value;
        }
        else if (option == "--mqtt-host")
        {
            mqttConfig.host = value;
        }
        else if (option == "--mqtt-port")
        {
            unsigned int port = 0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), port);
            if (parsed.ec != std::errc{}
                || parsed.ptr != value.data() + value.size()
                || port == 0 || port > 65535)
            {
                std::cerr << "Invalid MQTT port: " << value << std::endl;
                return 2;
            }
            mqttConfig.port = static_cast<unsigned short>(port);
        }
        else if (option == "--mqtt-client-id")
        {
            mqttConfig.clientId = value;
        }
        else if (option == "--mqtt-command-topic")
        {
            mqttConfig.commandTopic = value;
        }
        else if (option == "--mqtt-ack-topic")
        {
            mqttConfig.ackTopic = value;
        }
        else if (option == "--mqtt-response-topic")
        {
            mqttConfig.responseTopic = value;
        }
        else if (option == "--mqtt-status-topic")
        {
            mqttConfig.statusTopic = value;
        }
        else if (option == "--mqtt-qos")
        {
            unsigned int qos = 0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), qos);
            if (parsed.ec != std::errc{}
                || parsed.ptr != value.data() + value.size()
                || qos > 2)
            {
                std::cerr << "Invalid MQTT QoS: " << value << std::endl;
                return 2;
            }
            mqttConfig.qos = static_cast<int>(qos);
        }
        else if (option == "--mqtt-reconnect-min"
                 || option == "--mqtt-reconnect-max")
        {
            int seconds = 0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), seconds);
            if (parsed.ec != std::errc{}
                || parsed.ptr != value.data() + value.size()
                || seconds < 1 || seconds > 300)
            {
                std::cerr << "Invalid MQTT reconnect interval: " << value
                          << std::endl;
                return 2;
            }
            if (option == "--mqtt-reconnect-min")
            {
                mqttConfig.reconnectMinimumSeconds = seconds;
            }
            else
            {
                mqttConfig.reconnectMaximumSeconds = seconds;
            }
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
        Database database;
        database.initialize();
        CommandQueue commandQueue;
        ResultStore resultStore;
        RequestSubmissionService requestSubmission(commandQueue, resultStore);
        Dispatcher dispatcher(database, deviceConfig, discoverySoapTargetIp);
        MqttClient mqttClient(mqttConfig, requestSubmission);
        resultStore.setCompletionCallback(
            [&mqttClient](const CommandRecord& record)
            {
                mqttClient.commandCompleted(record);
            });
        MqttShutdownGuard mqttShutdown(mqttClient, resultStore);
        Worker worker(commandQueue, dispatcher, resultStore);
        HttpServer server(serverConfig, requestSubmission, resultStore);

        worker.start();
        mqttClient.start();
        const int serverResult = server.run(shutdownRequested);
        mqttClient.stopAccepting();
        worker.stop();
        worker.join();
        mqttClient.stop();
        return serverResult;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[Comms Interface] Fatal error: " << error.what() << std::endl;
        return 1;
    }
}
