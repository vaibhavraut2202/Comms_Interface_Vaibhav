#pragma once

#include <mqtt/async_client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "json_utils.hpp"
#include "request_submission.hpp"
#include "result_store.hpp"

struct MqttClientConfig
{
    std::string host = "127.0.0.1";
    unsigned short port = 1883;
    std::string clientId = "comms-interface";
    std::string commandTopic = "comms/command";
    std::string ackTopic = "comms/ack";
    std::string responseTopic = "comms/response";
    std::string statusTopic = "comms/status";
    int qos = 1;
    int reconnectMinimumSeconds = 1;
    int reconnectMaximumSeconds = 30;
};

class MqttCallbackHandler final : public mqtt::callback,
                                 public mqtt::iaction_listener
{
private:
    template <typename... Args>
    using Handler = std::function<void(Args...)>;

    std::mutex mutex;
    std::condition_variable callbacksChanged;
    std::size_t activeCallbacks = 0;
    bool closing = false;
    Handler<const std::string&> connectedHandler;
    Handler<const std::string&> connectionLostHandler;
    Handler<mqtt::const_message_ptr> messageArrivedHandler;
    Handler<mqtt::delivery_token_ptr> deliveryCompleteHandler;
    Handler<const mqtt::token&> successHandler;
    Handler<const mqtt::token&> failureHandler;

    template <typename HandlerType, typename... Args>
    void dispatch(HandlerType MqttCallbackHandler::* handlerMember,
                  Args&&... args) noexcept
    {
        try
        {
            HandlerType handler;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (closing)
                {
                    return;
                }
                handler = this->*handlerMember;
                if (!handler)
                {
                    return;
                }
                ++activeCallbacks;
            }

            struct CallbackCompletion
            {
                MqttCallbackHandler& owner;
                ~CallbackCompletion()
                {
                    std::lock_guard<std::mutex> lock(owner.mutex);
                    --owner.activeCallbacks;
                    if (owner.activeCallbacks == 0)
                    {
                        owner.callbacksChanged.notify_all();
                    }
                }
            } completion{*this};

            try
            {
                handler(std::forward<Args>(args)...);
            }
            catch (const std::exception& error)
            {
                std::cerr << "[MQTT] Callback failed: " << error.what()
                          << std::endl;
            }
            catch (...)
            {
                std::cerr << "[MQTT] Callback failed: unknown exception"
                          << std::endl;
            }
        }
        catch (const std::exception& error)
        {
            std::cerr << "[MQTT] Unable to dispatch callback: " << error.what()
                      << std::endl;
        }
        catch (...)
        {
            std::cerr << "[MQTT] Unable to dispatch callback: unknown exception"
                      << std::endl;
        }
    }

public:
    void setHandlers(Handler<const std::string&> connected,
                     Handler<const std::string&> connectionLost,
                     Handler<mqtt::const_message_ptr> messageArrived,
                     Handler<mqtt::delivery_token_ptr> deliveryComplete,
                     Handler<const mqtt::token&> success,
                     Handler<const mqtt::token&> failure)
    {
        std::lock_guard<std::mutex> lock(mutex);
        connectedHandler = std::move(connected);
        connectionLostHandler = std::move(connectionLost);
        messageArrivedHandler = std::move(messageArrived);
        deliveryCompleteHandler = std::move(deliveryComplete);
        successHandler = std::move(success);
        failureHandler = std::move(failure);
    }

    void quiesce()
    {
        std::unique_lock<std::mutex> lock(mutex);
        closing = true;
        callbacksChanged.wait(lock, [this]()
        {
            return activeCallbacks == 0;
        });
        connectedHandler = {};
        connectionLostHandler = {};
        messageArrivedHandler = {};
        deliveryCompleteHandler = {};
        successHandler = {};
        failureHandler = {};
    }

    void connected(const std::string& cause) override
    {
        dispatch(&MqttCallbackHandler::connectedHandler, cause);
    }

    void connection_lost(const std::string& cause) override
    {
        dispatch(&MqttCallbackHandler::connectionLostHandler, cause);
    }

    void message_arrived(mqtt::const_message_ptr message) override
    {
        dispatch(&MqttCallbackHandler::messageArrivedHandler, std::move(message));
    }

    void delivery_complete(mqtt::delivery_token_ptr token) override
    {
        dispatch(&MqttCallbackHandler::deliveryCompleteHandler, std::move(token));
    }

    void on_success(const mqtt::token& token) override
    {
        dispatch(&MqttCallbackHandler::successHandler, token);
    }

    void on_failure(const mqtt::token& token) override
    {
        dispatch(&MqttCallbackHandler::failureHandler, token);
    }
};

class MqttClient
{
private:
    MqttClientConfig config;
    RequestSubmissionService& requestSubmission;
    mqtt::connect_options connectOptions;
    std::thread connectionThread;
    std::mutex stateMutex;
    std::condition_variable stateChanged;
    std::mutex incomingMutex;
    std::mutex pendingMutex;
    std::mutex stopMutex;
    std::condition_variable pendingChanged;
    std::vector<mqtt::delivery_token_ptr> pendingPublishes;
    bool started = false;
    bool stopping = false;
    bool stopped = false;
    bool isConnected = false;
    bool acceptingCommands = true;
    MqttCallbackHandler callbackHandler;
    mqtt::async_client client;

    static bool isPublishableTopic(const std::string& topic)
    {
        return !topic.empty() && topic.size() <= 65535
            && topic.find_first_of("+#") == std::string::npos;
    }

    static std::string brokerUri(const MqttClientConfig& clientConfig)
    {
        return "tcp://" + clientConfig.host + ":"
            + std::to_string(clientConfig.port);
    }

    static mqtt::connect_options makeConnectOptions(
        const MqttClientConfig& clientConfig)
    {
        mqtt::connect_options options;
        options.set_mqtt_version(MQTTVERSION_3_1_1);
        options.set_clean_session(false);
        options.set_keep_alive_interval(20);
        options.set_connect_timeout(5);
        options.set_automatic_reconnect(false);
        options.set_will_message(mqtt::message::create(
            clientConfig.statusTopic, "{\"status\":\"OFFLINE\"}",
            clientConfig.qos, true));
        return options;
    }

    static std::string serializeAcknowledgement(const SubmissionResult& result)
    {
        std::ostringstream json;
        json << "{\"request_id\":\"" << jsonEscape(result.correlationId)
             << "\",\"internal_request_id\":\"" << jsonEscape(result.requestId)
             << "\",\"status\":\"";
        if (result.duplicate)
        {
            json << (result.accepted ? "DUPLICATE" : "REJECTED");
        }
        else
        {
            json << (result.accepted ? "QUEUED" : "REJECTED");
        }
        json << "\",\"message\":\"";
        if (result.accepted && result.duplicate)
        {
            json << "Duplicate delivery; the original request remains authoritative";
        }
        else if (result.accepted)
        {
            json << "Command accepted";
        }
        else
        {
            json << jsonEscape(result.errorMessage);
        }
        json << '"';
        if (!result.accepted)
        {
            json << ",\"error\":{\"category\":\""
                 << jsonEscape(result.errorCategory)
                 << "\",\"message\":\"" << jsonEscape(result.errorMessage)
                 << "\"}";
        }
        json << '}';
        return json.str();
    }

    static std::string serializeResponse(const CommandRecord& record)
    {
        std::ostringstream json;
        const std::string& correlationId = record.mqttCorrelationId
            ? *record.mqttCorrelationId
            : record.requestId;
        json << "{\"request_id\":\"" << jsonEscape(correlationId)
             << "\",\"internal_request_id\":\"" << jsonEscape(record.requestId)
             << "\",\"camera_id\":\"" << jsonEscape(record.cameraId)
             << "\",\"operation\":\"" << jsonEscape(record.operation)
             << "\",\"status\":\"" << statusName(record.status) << '"';
        if (record.pairedDevices)
        {
            json << ",\"data\":[";
            for (std::size_t i = 0; i < record.pairedDevices->size(); ++i)
            {
                if (i != 0)
                {
                    json << ',';
                }
                const PairedDevice& device = (*record.pairedDevices)[i];
                json << "{\"camera_id\":\"" << jsonEscape(device.cameraId)
                     << "\",\"ip_address\":\"" << jsonEscape(device.ipAddress)
                     << "\"}";
            }
            json << ']';
        }
        else if (record.deviceInformation)
        {
            const DeviceInformation& information = *record.deviceInformation;
            json << ",\"data\":{\"manufacturer\":\""
                 << jsonEscape(information.manufacturer)
                 << "\",\"model\":\"" << jsonEscape(information.model)
                 << "\",\"firmware_version\":\""
                 << jsonEscape(information.firmwareVersion)
                 << "\",\"serial_number\":\""
                 << jsonEscape(information.serialNumber)
                 << "\",\"hardware_id\":\""
                 << jsonEscape(information.hardwareId) << "\"}";
        }
        else if (record.discoverySummary)
        {
            const DiscoverySummary& summary = *record.discoverySummary;
            json << ",\"data\":{\"discovered_count\":"
                 << summary.discoveredCount
                 << ",\"information_success_count\":"
                 << summary.informationSuccessCount
                 << ",\"information_failure_count\":"
                 << summary.informationFailureCount << '}';
        }
        else
        {
            json << ",\"data\":null";
        }
        if (record.status == RequestStatus::failed)
        {
            json << ",\"error\":{\"category\":\""
                 << jsonEscape(record.errorCategory) << "\",\"message\":\""
                 << jsonEscape(record.errorMessage) << "\"}";
        }
        else
        {
            json << ",\"error\":null";
        }
        json << '}';
        return json.str();
    }

    void removePendingPublish(int messageId)
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingPublishes.erase(
            std::remove_if(pendingPublishes.begin(), pendingPublishes.end(),
                [messageId](const mqtt::delivery_token_ptr& pending)
                {
                    return pending->get_message_id() == messageId;
                }),
            pendingPublishes.end());
        pendingChanged.notify_all();
    }

    void publish(const std::string& topic, const std::string& payload,
                 bool retained)
    {
        if (!client.is_connected())
        {
            std::cerr << "[MQTT] Cannot publish to " << topic
                      << ": broker is disconnected; message is not persisted"
                      << std::endl;
            return;
        }

        try
        {
            const auto message = mqtt::message::create(
                topic, payload, config.qos, retained);
            const mqtt::delivery_token_ptr token =
                client.publish(message, nullptr, callbackHandler);
            if (!token)
            {
                std::cerr << "[MQTT] Publish to " << topic
                          << " returned no delivery token" << std::endl;
                return;
            }
            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                if (!token->is_complete())
                {
                    pendingPublishes.push_back(token);
                }
            }
        }
        catch (const std::exception& error)
        {
            std::cerr << "[MQTT] Unable to publish to " << topic
                      << ": " << error.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "[MQTT] Unable to publish to " << topic
                      << ": unknown exception" << std::endl;
        }
    }

    void connectionLoop()
    {
        int retrySeconds = config.reconnectMinimumSeconds;
        while (true)
        {
            {
                std::unique_lock<std::mutex> lock(stateMutex);
                stateChanged.wait(lock, [this]()
                {
                    return stopping || !isConnected;
                });
                if (stopping)
                {
                    return;
                }
            }

            try
            {
                const mqtt::token_ptr token = client.connect(connectOptions);
                token->wait();
                if (client.is_connected())
                {
                    retrySeconds = config.reconnectMinimumSeconds;
                    std::unique_lock<std::mutex> lock(stateMutex);
                    stateChanged.wait_for(lock, std::chrono::seconds(2),
                        [this]()
                        {
                            return stopping || isConnected;
                        });
                    continue;
                }
                std::cerr << "[MQTT] Broker connection attempt did not connect"
                          << std::endl;
            }
            catch (const std::exception& error)
            {
                std::cerr << "[MQTT] Broker connection failed: "
                          << error.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[MQTT] Broker connection failed: unknown exception"
                          << std::endl;
            }

            std::unique_lock<std::mutex> lock(stateMutex);
            if (stateChanged.wait_for(lock,
                    std::chrono::seconds(retrySeconds),
                    [this]() { return stopping; }))
            {
                return;
            }
            retrySeconds = std::min(
                retrySeconds * 2, config.reconnectMaximumSeconds);
        }
    }

    void waitForPendingPublishes()
    {
        std::unique_lock<std::mutex> lock(pendingMutex);
        if (!pendingChanged.wait_for(lock, std::chrono::seconds(5), [this]()
            {
                return pendingPublishes.empty();
            }))
        {
            std::cerr << "[MQTT] Shutdown timed out waiting for "
                      << pendingPublishes.size() << " publish acknowledgement(s)"
                      << std::endl;
        }
    }

public:
    MqttClient(const MqttClientConfig& clientConfig,
               RequestSubmissionService& submission)
        : config(clientConfig),
          requestSubmission(submission),
          connectOptions(makeConnectOptions(clientConfig)),
          client(brokerUri(clientConfig), clientConfig.clientId)
    {
        if (config.host.empty() || config.port == 0 || config.clientId.empty()
            || !isPublishableTopic(config.commandTopic)
            || !isPublishableTopic(config.ackTopic)
            || !isPublishableTopic(config.responseTopic)
            || !isPublishableTopic(config.statusTopic)
            || config.commandTopic == config.ackTopic
            || config.commandTopic == config.responseTopic
            || config.commandTopic == config.statusTopic
            || config.ackTopic == config.responseTopic
            || config.ackTopic == config.statusTopic
            || config.responseTopic == config.statusTopic
            || config.qos < 0 || config.qos > 2
            || config.reconnectMinimumSeconds < 1
            || config.reconnectMaximumSeconds < config.reconnectMinimumSeconds)
        {
            throw std::invalid_argument("Invalid MQTT client configuration");
        }
        callbackHandler.setHandlers(
            [this](const std::string& cause) { connected(cause); },
            [this](const std::string& cause) { connection_lost(cause); },
            [this](mqtt::const_message_ptr message)
            {
                message_arrived(std::move(message));
            },
            [this](mqtt::delivery_token_ptr token)
            {
                delivery_complete(std::move(token));
            },
            [this](const mqtt::token& token) { on_success(token); },
            [this](const mqtt::token& token) { on_failure(token); });
        try
        {
            client.set_callback(callbackHandler);
        }
        catch (...)
        {
            callbackHandler.quiesce();
            throw;
        }
    }

    ~MqttClient()
    {
        stop();
    }

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    void start()
    {
        std::lock_guard<std::mutex> stopLock(stopMutex);
        if (stopped)
        {
            throw std::logic_error("MQTT client has already been stopped");
        }
        std::lock_guard<std::mutex> lock(stateMutex);
        if (started)
        {
            throw std::logic_error("MQTT client has already been started");
        }
        connectionThread = std::thread(&MqttClient::connectionLoop, this);
        started = true;
    }

    void stopAccepting()
    {
        {
            std::lock_guard<std::mutex> lock(incomingMutex);
            acceptingCommands = false;
        }
        if (client.is_connected())
        {
            try
            {
                const mqtt::token_ptr token = client.unsubscribe(
                    config.commandTopic, nullptr, callbackHandler);
                if (!token->wait_for(std::chrono::seconds(2)))
                {
                    std::cerr << "[MQTT] Timed out waiting to unsubscribe from "
                              << config.commandTopic << std::endl;
                }
            }
            catch (const std::exception& error)
            {
                std::cerr << "[MQTT] Unable to unsubscribe during shutdown: "
                          << error.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[MQTT] Unable to unsubscribe during shutdown: "
                          << "unknown exception" << std::endl;
            }
        }
    }

    void stop()
    {
        std::unique_lock<std::mutex> stopLock(stopMutex);
        if (stopped)
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            stopping = true;
        }
        stateChanged.notify_all();
        if (connectionThread.joinable())
        {
            connectionThread.join();
        }

        try
        {
            stopAccepting();
            if (client.is_connected())
            {
                publish(config.statusTopic, "{\"status\":\"OFFLINE\"}", true);
                waitForPendingPublishes();
                try
                {
                    const mqtt::token_ptr token = client.disconnect(2000);
                    if (!token->wait_for(std::chrono::seconds(3)))
                    {
                        std::cerr << "[MQTT] Timed out waiting for broker disconnect"
                                  << std::endl;
                    }
                }
                catch (const std::exception& error)
                {
                    std::cerr << "[MQTT] Disconnect failed: "
                              << error.what() << std::endl;
                }
                catch (...)
                {
                    std::cerr << "[MQTT] Disconnect failed: unknown exception"
                              << std::endl;
                }
            }
            else
            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                if (!pendingPublishes.empty())
                {
                    std::cerr << "[MQTT] " << pendingPublishes.size()
                              << " publish acknowledgement(s) were not received"
                              << std::endl;
                }
            }
        }
        catch (const std::exception& error)
        {
            std::cerr << "[MQTT] Shutdown cleanup failed: " << error.what()
                      << std::endl;
        }
        catch (...)
        {
            std::cerr << "[MQTT] Shutdown cleanup failed: unknown exception"
                      << std::endl;
        }

        try
        {
            client.disable_callbacks();
        }
        catch (const std::exception& error)
        {
            std::cerr << "[MQTT] Unable to disable callbacks: " << error.what()
                      << std::endl;
        }
        catch (...)
        {
            std::cerr << "[MQTT] Unable to disable callbacks: unknown exception"
                      << std::endl;
        }
        callbackHandler.quiesce();
        stopped = true;
    }

    void commandCompleted(const CommandRecord& record)
    {
        if (!record.mqttCorrelationId)
        {
            return;
        }
        publish(config.responseTopic, serializeResponse(record), false);
    }

    void connected(const std::string& cause)
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (stopping)
            {
                return;
            }
            isConnected = true;
        }
        stateChanged.notify_all();
        std::cout << "[MQTT] Connected to broker " << brokerUri(config);
        if (!cause.empty())
        {
            std::cout << ": " << cause;
        }
        std::cout << std::endl;

        publish(config.statusTopic, "{\"status\":\"ONLINE\"}", true);
        try
        {
            client.subscribe(
                config.commandTopic, config.qos, nullptr, callbackHandler);
        }
        catch (const std::exception& error)
        {
            std::cerr << "[MQTT] Unable to subscribe to "
                      << config.commandTopic << ": " << error.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "[MQTT] Unable to subscribe to "
                      << config.commandTopic << ": unknown exception"
                      << std::endl;
        }
    }

    void connection_lost(const std::string& cause)
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            isConnected = false;
        }
        stateChanged.notify_all();
        std::cerr << "[MQTT] Broker connection lost";
        if (!cause.empty())
        {
            std::cerr << ": " << cause;
        }
        std::cerr << std::endl;
    }

    void message_arrived(mqtt::const_message_ptr message)
    {
        std::lock_guard<std::mutex> lock(incomingMutex);
        if (!acceptingCommands)
        {
            std::cerr << "[MQTT] Ignoring command received during shutdown"
                      << std::endl;
            return;
        }
        if (message->get_topic() != config.commandTopic)
        {
            return;
        }

        try
        {
            const SubmissionResult submitted =
                requestSubmission.submitMqtt(message->get_payload_str());
            publish(config.ackTopic, serializeAcknowledgement(submitted), false);
            if (!submitted.accepted)
            {
                std::cerr << "[MQTT] Command was not queued: "
                          << submitted.errorCategory << ": "
                          << submitted.errorMessage << std::endl;
            }
        }
        catch (const std::exception& error)
        {
            std::cerr << "[MQTT] Unable to submit command: "
                      << error.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "[MQTT] Unable to submit command: unknown exception"
                      << std::endl;
        }
    }

    void delivery_complete(mqtt::delivery_token_ptr token)
    {
        removePendingPublish(token->get_message_id());
    }

    void on_success(const mqtt::token& token)
    {
        removePendingPublish(token.get_message_id());
    }

    void on_failure(const mqtt::token& token)
    {
        std::cerr << "[MQTT] Asynchronous action failed (code "
                  << token.get_return_code() << ')' << std::endl;
        removePendingPublish(token.get_message_id());
    }
};
