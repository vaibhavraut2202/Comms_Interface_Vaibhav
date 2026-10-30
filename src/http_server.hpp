#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "command.hpp"
#include "command_queue.hpp"
#include "device_config.hpp"
#include "json_utils.hpp"
#include "result_store.hpp"

class WinsockRuntime
{
public:
    WinsockRuntime()
    {
        WSADATA data;
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0)
        {
            throw std::runtime_error("WSAStartup failed with error "
                + std::to_string(result));
        }
    }

    ~WinsockRuntime()
    {
        WSACleanup();
    }

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

struct HttpServerConfig
{
    std::string listenAddress = "127.0.0.1";
    unsigned short port = 8090;
};

class HttpServer
{
private:
    struct HttpRequest
    {
        std::string method;
        std::string target;
        std::string version;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    struct HttpResponse
    {
        int status = 200;
        std::string body;
        std::string additionalHeaders;
    };

    static constexpr std::size_t maximumHeaderSize = 16 * 1024;
    static constexpr std::size_t maximumBodySize = 16 * 1024;
    static std::atomic<std::uint64_t> nextRequestId;

    HttpServerConfig config;
    const DeviceRegistry& devices;
    CommandQueue& commandQueue;
    ResultStore& resultStore;

    static std::string lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch)
            {
                return static_cast<char>(std::tolower(ch));
            });
        return value;
    }

    static std::string trim(const std::string& value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    static HttpResponse errorResponse(int status, const std::string& category,
                                      const std::string& message)
    {
        HttpResponse response;
        response.status = status;
        response.body = "{\"error\":{\"category\":\"" + jsonEscape(category)
            + "\",\"message\":\"" + jsonEscape(message) + "\"}}";
        return response;
    }

    static std::string statusText(int status)
    {
        switch (status)
        {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Error";
        }
    }

    static bool receiveRequest(SOCKET client, HttpRequest& request,
                               int& errorStatus, std::string& errorMessage)
    {
        std::string received;
        std::size_t headerEnd = std::string::npos;
        char buffer[4096];

        while (headerEnd == std::string::npos)
        {
            const int count = recv(client, buffer, sizeof(buffer), 0);
            if (count == 0)
            {
                errorMessage = "Client disconnected before sending complete headers";
                return false;
            }
            if (count == SOCKET_ERROR)
            {
                errorMessage = "Failed while receiving HTTP headers (Winsock "
                    + std::to_string(WSAGetLastError()) + ")";
                return false;
            }
            received.append(buffer, static_cast<std::size_t>(count));
            headerEnd = received.find("\r\n\r\n");
            if (headerEnd == std::string::npos && received.size() > maximumHeaderSize)
            {
                errorStatus = 431;
                errorMessage = "HTTP headers exceed the size limit";
                return false;
            }
        }
        if (headerEnd > maximumHeaderSize)
        {
            errorStatus = 431;
            errorMessage = "HTTP headers exceed the size limit";
            return false;
        }

        std::istringstream headers(received.substr(0, headerEnd));
        std::string line;
        if (!std::getline(headers, line))
        {
            errorStatus = 400;
            errorMessage = "Missing HTTP request line";
            return false;
        }
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        std::istringstream requestLine(line);
        std::string extra;
        if (!(requestLine >> request.method >> request.target >> request.version)
            || (requestLine >> extra)
            || (request.version != "HTTP/1.1" && request.version != "HTTP/1.0"))
        {
            errorStatus = 400;
            errorMessage = "Malformed HTTP request line";
            return false;
        }

        while (std::getline(headers, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0)
            {
                errorStatus = 400;
                errorMessage = "Malformed HTTP header";
                return false;
            }
            const std::string name = lower(trim(line.substr(0, colon)));
            const std::string value = trim(line.substr(colon + 1));
            if (!request.headers.emplace(name, value).second)
            {
                errorStatus = 400;
                errorMessage = "Duplicate HTTP header: " + name;
                return false;
            }
        }

        if (request.headers.find("transfer-encoding") != request.headers.end())
        {
            errorStatus = 400;
            errorMessage = "Transfer-Encoding is not supported";
            return false;
        }

        std::size_t contentLength = 0;
        const auto lengthHeader = request.headers.find("content-length");
        if (lengthHeader != request.headers.end())
        {
            const std::string& text = lengthHeader->second;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                                contentLength);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            {
                errorStatus = 400;
                errorMessage = "Invalid Content-Length";
                return false;
            }
        }
        if (contentLength > maximumBodySize)
        {
            errorStatus = 413;
            errorMessage = "Request body exceeds the size limit";
            return false;
        }

        const std::size_t bodyStart = headerEnd + 4;
        while (received.size() - bodyStart < contentLength)
        {
            const int count = recv(client, buffer, sizeof(buffer), 0);
            if (count == 0)
            {
                errorStatus = 400;
                errorMessage = "Client disconnected before sending the full request body";
                return false;
            }
            if (count == SOCKET_ERROR)
            {
                errorStatus = 400;
                errorMessage = "Failed while receiving HTTP request body";
                return false;
            }
            received.append(buffer, static_cast<std::size_t>(count));
        }
        request.body = received.substr(bodyStart, contentLength);
        return true;
    }

    static bool sendResponse(SOCKET client, const HttpResponse& response)
    {
        std::ostringstream wire;
        wire << "HTTP/1.1 " << response.status << ' ' << statusText(response.status)
             << "\r\nContent-Type: application/json; charset=utf-8\r\n"
             << "Content-Length: " << response.body.size()
             << "\r\nConnection: close\r\n"
             << response.additionalHeaders << "\r\n"
             << response.body;
        const std::string message = wire.str();
        std::size_t sent = 0;
        while (sent < message.size())
        {
            const int count = send(client, message.data() + sent,
                static_cast<int>(message.size() - sent), 0);
            if (count == SOCKET_ERROR || count == 0)
            {
                std::cerr << "[HTTP] Client disconnected while receiving response "
                          << "(Winsock " << WSAGetLastError() << ")" << std::endl;
                return false;
            }
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    static std::string makeRequestId()
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return "req-" + std::to_string(now) + "-"
            + std::to_string(nextRequestId.fetch_add(1));
    }

    static std::string formatTime(std::chrono::system_clock::time_point time)
    {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            time.time_since_epoch());
        const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
        std::tm utc{};
        if (gmtime_s(&utc, &seconds) != 0)
        {
            throw std::runtime_error("Unable to format request timestamp");
        }
        char timestamp[32];
        if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
        {
            throw std::runtime_error("Unable to format request timestamp");
        }
        auto fraction = milliseconds.count() % 1000;
        if (fraction < 0)
        {
            fraction += 1000;
        }
        std::ostringstream result;
        result << timestamp << '.' << (fraction / 100) << ((fraction / 10) % 10)
               << (fraction % 10) << 'Z';
        return result.str();
    }

    static std::string serializeRecord(const CommandRecord& record)
    {
        std::ostringstream json;
        json << "{\"request_id\":\"" << jsonEscape(record.requestId)
             << "\",\"camera_id\":\"" << jsonEscape(record.cameraId)
             << "\",\"operation\":\"" << jsonEscape(record.operation)
             << "\",\"status\":\"" << statusName(record.status)
             << "\",\"created_at\":\"" << formatTime(record.createdAt) << '"';
        if (record.completedAt)
        {
            json << ",\"completed_at\":\"" << formatTime(*record.completedAt) << '"';
        }
        else
        {
            json << ",\"completed_at\":null";
        }
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
            const DeviceInformation& info = *record.deviceInformation;
            json << ",\"data\":{\"manufacturer\":\"" << jsonEscape(info.manufacturer)
                 << "\",\"model\":\"" << jsonEscape(info.model)
                 << "\",\"firmware_version\":\"" << jsonEscape(info.firmwareVersion)
                 << "\",\"serial_number\":\"" << jsonEscape(info.serialNumber)
                 << "\",\"hardware_id\":\"" << jsonEscape(info.hardwareId) << "\"}";
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

    static std::optional<std::string> queryParameter(const std::string& query,
                                                      const std::string& wanted)
    {
        std::optional<std::string> result;
        std::size_t start = 0;
        while (start <= query.size())
        {
            const std::size_t end = query.find('&', start);
            const std::string part = query.substr(start,
                end == std::string::npos ? std::string::npos : end - start);
            const std::size_t equal = part.find('=');
            if (equal == std::string::npos)
            {
                return std::nullopt;
            }
            const std::string key = part.substr(0, equal);
            if (key == wanted)
            {
                if (result)
                {
                    return std::nullopt;
                }
                std::string value;
                for (std::size_t i = equal + 1; i < part.size(); ++i)
                {
                    if (part[i] == '+')
                    {
                        value.push_back(' ');
                    }
                    else if (part[i] == '%')
                    {
                        if (i + 2 >= part.size())
                        {
                            return std::nullopt;
                        }
                        const auto hexValue = [](char ch) -> int
                        {
                            if (ch >= '0' && ch <= '9') return ch - '0';
                            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                            return -1;
                        };
                        const int high = hexValue(part[i + 1]);
                        const int lowValue = hexValue(part[i + 2]);
                        if (high < 0 || lowValue < 0)
                        {
                            return std::nullopt;
                        }
                        value.push_back(static_cast<char>((high << 4) | lowValue));
                        i += 2;
                    }
                    else
                    {
                        value.push_back(part[i]);
                    }
                }
                result = std::move(value);
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
        return result;
    }

    HttpResponse handlePostCommand(const HttpRequest& request)
    {
        const auto contentType = request.headers.find("content-type");
        const std::string mediaType = contentType == request.headers.end()
            ? std::string{}
            : lower(trim(contentType->second.substr(
                0, contentType->second.find(';'))));
        if (mediaType != "application/json")
        {
            return errorResponse(415, "UNSUPPORTED_MEDIA_TYPE",
                                 "Content-Type must be application/json");
        }
        if (request.headers.find("content-length") == request.headers.end())
        {
            return errorResponse(411, "LENGTH_REQUIRED",
                                 "POST requests require Content-Length");
        }

        std::map<std::string, std::optional<std::string>> fields;
        std::string parseError;
        JsonObjectParser parser(request.body);
        if (!parser.parse(fields, parseError))
        {
            return errorResponse(400, "MALFORMED_JSON", parseError);
        }
        const auto cameraField = fields.find("camera_id");
        const auto operationField = fields.find("operation");
        const bool pairedDevicesOperation =
            operationField != fields.end()
            && operationField->second
            && trim(*operationField->second) == "GetCurrentPairedDevices";
        std::string cameraId;
        std::string operation;

        if (pairedDevicesOperation)
        {
            operation = trim(*operationField->second);
            if (cameraField != fields.end() && !cameraField->second)
            {
                return errorResponse(400, "INVALID_FIELD_TYPE",
                                     "camera_id must be a JSON string");
            }
            if (cameraField != fields.end())
            {
                cameraId = trim(*cameraField->second);
                if (cameraId.empty())
                {
                    return errorResponse(400, "INVALID_FIELD",
                                         "camera_id must not be empty");
                }
            }
        }
        else
        {
            if (cameraField == fields.end() || operationField == fields.end())
            {
                return errorResponse(400, "MISSING_FIELD",
                                     "Required fields are camera_id and operation");
            }
            if (!cameraField->second || !operationField->second)
            {
                return errorResponse(400, "INVALID_FIELD_TYPE",
                                     "camera_id and operation must be JSON strings");
            }
            cameraId = trim(*cameraField->second);
            operation = trim(*operationField->second);
            if (cameraId.empty() || operation.empty())
            {
                return errorResponse(400, "INVALID_FIELD",
                                     "camera_id and operation must not be empty");
            }
        }

        const bool recognized = operation == "GetDeviceInformation"
            || operation == "GetProfiles" || operation == "GetPTZCapabilities"
            || operation == "GetCurrentPairedDevices";
        if (!recognized)
        {
            return errorResponse(422, "UNKNOWN_OPERATION",
                                 "Unrecognized operation: " + operation);
        }
        if (operation == "GetProfiles" || operation == "GetPTZCapabilities")
        {
            return errorResponse(501, "UNSUPPORTED_OPERATION",
                operation + " is recognized but not implemented");
        }

        std::string endpoint;
        if (operation != "GetCurrentPairedDevices")
        {
            const auto device = devices.find(cameraId);
            if (!device)
            {
                return errorResponse(404, "UNKNOWN_CAMERA",
                                     "No configured camera has ID " + cameraId);
            }
            endpoint = device->endpoint;
        }

        Command command{makeRequestId(), cameraId, operation, endpoint};
        resultStore.addQueued(command);
        if (!commandQueue.push(command))
        {
            resultStore.complete(command.requestId, OperationResult::failure(
                "SERVER_SHUTDOWN", "Application is shutting down"));
            return errorResponse(503, "SERVER_SHUTDOWN",
                                 "Application is no longer accepting commands");
        }

        HttpResponse response;
        response.status = 202;
        response.body = "{\"request_id\":\"" + jsonEscape(command.requestId)
            + "\",\"status\":\"QUEUED\"}";
        return response;
    }

    HttpResponse handleGetResult(const HttpRequest& request)
    {
        const std::size_t question = request.target.find('?');
        if (question == std::string::npos)
        {
            return errorResponse(400, "MISSING_REQUEST_ID",
                                 "Use /result?id=<request_id>");
        }
        const auto requestId = queryParameter(request.target.substr(question + 1), "id");
        if (!requestId || requestId->empty())
        {
            return errorResponse(400, "INVALID_REQUEST_ID",
                                 "A single non-empty id query parameter is required");
        }
        const auto record = resultStore.find(*requestId);
        if (!record)
        {
            return errorResponse(404, "UNKNOWN_REQUEST_ID",
                                 "No request exists with ID " + *requestId);
        }

        HttpResponse response;
        response.body = serializeRecord(*record);
        return response;
    }

    HttpResponse handleRequest(const HttpRequest& request)
    {
        const std::size_t question = request.target.find('?');
        const std::string path = request.target.substr(0, question);
        if (request.method == "POST" && path == "/command")
        {
            return handlePostCommand(request);
        }
        if (request.method == "GET" && path == "/result")
        {
            return handleGetResult(request);
        }
        if ((path == "/command" && request.method == "GET")
            || (path == "/result" && request.method == "POST"))
        {
            HttpResponse response = errorResponse(
                405, "METHOD_NOT_ALLOWED", "HTTP method is not allowed for this route");
            response.additionalHeaders = path == "/command"
                ? "Allow: POST\r\n"
                : "Allow: GET\r\n";
            return response;
        }
        if (request.method != "GET" && request.method != "POST")
        {
            HttpResponse response = errorResponse(
                405, "METHOD_NOT_ALLOWED", "Only GET and POST are supported");
            response.additionalHeaders = "Allow: GET, POST\r\n";
            return response;
        }
        return errorResponse(404, "NOT_FOUND", "Route not found");
    }

    void handleClient(SOCKET client)
    {
        const DWORD timeoutMilliseconds = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeoutMilliseconds),
            sizeof(timeoutMilliseconds));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeoutMilliseconds),
            sizeof(timeoutMilliseconds));

        HttpRequest request;
        int errorStatus = 400;
        std::string errorMessage;
        if (!receiveRequest(client, request, errorStatus, errorMessage))
        {
            if (!errorMessage.empty())
            {
                std::cerr << "[HTTP] " << errorMessage << std::endl;
            }
            if (errorMessage.rfind("Client disconnected", 0) != 0)
            {
                sendResponse(client, errorResponse(
                    errorStatus, "INVALID_HTTP_REQUEST", errorMessage));
            }
            return;
        }

        try
        {
            sendResponse(client, handleRequest(request));
        }
        catch (const std::exception& error)
        {
            std::cerr << "[HTTP] Internal request handling error: "
                      << error.what() << std::endl;
            sendResponse(client, errorResponse(
                500, "INTERNAL_ERROR", "Internal request processing error"));
        }
    }

public:
    HttpServer(const HttpServerConfig& serverConfig, const DeviceRegistry& deviceRegistry,
               CommandQueue& queue, ResultStore& results)
        : config(serverConfig),
          devices(deviceRegistry),
          commandQueue(queue),
          resultStore(results)
    {
    }

    int run(const volatile std::sig_atomic_t& shutdownRequested)
    {
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
        {
            std::cerr << "[HTTP] socket() failed with Winsock "
                      << WSAGetLastError() << std::endl;
            return 1;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config.port);
        if (inet_pton(AF_INET, config.listenAddress.c_str(), &address.sin_addr) != 1)
        {
            std::cerr << "[HTTP] Invalid IPv4 listen address: "
                      << config.listenAddress << std::endl;
            closesocket(listener);
            return 1;
        }
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address))
                == SOCKET_ERROR
            || listen(listener, SOMAXCONN) == SOCKET_ERROR)
        {
            std::cerr << "[HTTP] Unable to listen on " << config.listenAddress << ':'
                      << config.port << " (Winsock " << WSAGetLastError() << ')'
                      << std::endl;
            closesocket(listener);
            return 1;
        }

        std::cout << "[HTTP] Listening on http://" << config.listenAddress << ':'
                  << config.port << std::endl;
        while (!shutdownRequested)
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener, &readable);
            timeval timeout{};
            timeout.tv_usec = 500000;
            const int ready = select(0, &readable, nullptr, nullptr, &timeout);
            if (ready == 0)
            {
                continue;
            }
            if (ready == SOCKET_ERROR)
            {
                if (shutdownRequested)
                {
                    break;
                }
                std::cerr << "[HTTP] select() failed with Winsock "
                          << WSAGetLastError() << std::endl;
                closesocket(listener);
                return 1;
            }

            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
            {
                if (!shutdownRequested)
                {
                    std::cerr << "[HTTP] accept() failed with Winsock "
                              << WSAGetLastError() << std::endl;
                }
                continue;
            }
            handleClient(client);
            shutdown(client, SD_BOTH);
            closesocket(client);
        }

        closesocket(listener);
        std::cout << "[HTTP] Server stopping; queued commands will be drained"
                  << std::endl;
        return 0;
    }
};

inline std::atomic<std::uint64_t> HttpServer::nextRequestId{1};
