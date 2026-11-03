#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../generated/soapDeviceBindingProxy.h"

class WsDiscovery
{
private:
    static constexpr std::size_t maximumUniqueDevices = 8;

#ifdef _WIN32
    using SocketHandle = SOCKET;
    using SocketLength = int;
    static constexpr SocketHandle invalidSocket = INVALID_SOCKET;
    static constexpr int socketError = SOCKET_ERROR;
#else
    using SocketHandle = int;
    using SocketLength = socklen_t;
    static constexpr SocketHandle invalidSocket = -1;
    static constexpr int socketError = -1;
#endif

    static constexpr const char* discoveryNamespace =
        "http://schemas.xmlsoap.org/ws/2005/04/discovery";
    static constexpr const char* discovery11Namespace =
        "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01";
    static constexpr const char* addressingNamespace =
        "http://schemas.xmlsoap.org/ws/2004/08/addressing";
    static constexpr const char* addressing2005Namespace =
        "http://www.w3.org/2005/08/addressing";
    static constexpr const char* soap11Namespace =
        "http://schemas.xmlsoap.org/soap/envelope/";
    static constexpr const char* soap12Namespace =
        "http://www.w3.org/2003/05/soap-envelope";

    static bool isNamespace(const char* actual, const char* expected)
    {
        return actual && std::string(actual) == expected;
    }

    static std::string lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch)
            {
                return static_cast<char>(std::tolower(ch));
            });
        return value;
    }

    static bool isOneOf(const char* actual, const char* first,
                        const char* second)
    {
        return isNamespace(actual, first) || isNamespace(actual, second);
    }

    static bool hasLocalName(const soap_dom_element* element,
                             const char* expected)
    {
        if (!element || !element->name)
        {
            return false;
        }
        std::string name(element->name);
        const std::size_t separator = name.rfind(':');
        if (separator != std::string::npos)
        {
            name.erase(0, separator + 1);
        }
        return name == expected;
    }

    static soap_dom_element* child(soap_dom_element& parent,
                                   const char* elementName,
                                   const char* namespaceOne,
                                   const char* namespaceTwo = nullptr)
    {
        for (soap_dom_element* element = parent.elts;
             element;
             element = element->next)
        {
            if (hasLocalName(element, elementName)
                && (isNamespace(element->nstr, namespaceOne)
                    || (namespaceTwo
                        && isNamespace(element->nstr, namespaceTwo))))
            {
                return element;
            }
        }
        return nullptr;
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

    static std::string text(const soap_dom_element* element)
    {
        return element && element->text ? trim(element->text) : std::string();
    }

    static std::vector<std::string> splitWhitespace(const std::string& value)
    {
        std::istringstream input(value);
        std::vector<std::string> values;
        std::string token;
        while (input >> token)
        {
            values.push_back(std::move(token));
        }
        return values;
    }

    static void appendUnique(std::vector<std::string>& destination,
                             const std::vector<std::string>& additions)
    {
        for (const std::string& addition : additions)
        {
            if (std::find(destination.begin(), destination.end(), addition)
                == destination.end())
            {
                destination.push_back(addition);
            }
        }
    }

    static bool mergeDevice(std::vector<DiscoveredDevice>& devices,
                            DiscoveredDevice device,
                            std::size_t maximumDevices)
    {
        const auto existing = std::find_if(
            devices.begin(), devices.end(),
            [&device](const DiscoveredDevice& candidate)
            {
                return candidate.endpointReference == device.endpointReference;
            });
        if (existing == devices.end())
        {
            if (devices.size() >= maximumDevices)
            {
                return false;
            }
            devices.push_back(std::move(device));
            return true;
        }

        appendUnique(existing->types, device.types);
        appendUnique(existing->scopes, device.scopes);
        appendUnique(existing->xAddrs, device.xAddrs);
        appendUnique(existing->responseSourceAddresses,
                     device.responseSourceAddresses);
        existing->metadataVersion = std::max(
            existing->metadataVersion, device.metadataVersion);
        return true;
    }

    static bool isPrivateIpv4(const in_addr& address)
    {
        const std::uint32_t hostAddress = ntohl(address.s_addr);
        const std::uint8_t first = static_cast<std::uint8_t>(
            (hostAddress >> 24) & 0xFF);
        const std::uint8_t second = static_cast<std::uint8_t>(
            (hostAddress >> 16) & 0xFF);
        return first == 10
            || (first == 172 && second >= 16 && second <= 31)
            || (first == 192 && second == 168);
    }

    static bool parseXAddr(const std::string& xAddr,
                           const std::vector<std::string>& responseSources,
                           std::string& rejectionReason)
    {
        rejectionReason.clear();
        if (xAddr.empty())
        {
            rejectionReason = "URL is empty";
            return false;
        }
        for (const unsigned char ch : xAddr)
        {
            if (ch <= 0x20 || ch >= 0x7F || ch == '\\')
            {
                rejectionReason = "URL contains whitespace or invalid characters";
                return false;
            }
        }

        const std::size_t schemeEnd = xAddr.find("://");
        if (schemeEnd == std::string::npos)
        {
            rejectionReason = "URL must include an HTTP or HTTPS scheme";
            return false;
        }
        const std::string scheme = lowercase(xAddr.substr(0, schemeEnd));
        if (scheme != "http" && scheme != "https")
        {
            rejectionReason = "URL scheme is not HTTP or HTTPS";
            return false;
        }

        const std::size_t authorityStart = schemeEnd + 3;
        const std::size_t authorityEnd = xAddr.find_first_of("/?#", authorityStart);
        const std::string authority = xAddr.substr(
            authorityStart,
            authorityEnd == std::string::npos
                ? std::string::npos
                : authorityEnd - authorityStart);
        if (authority.empty())
        {
            rejectionReason = "URL has no host";
            return false;
        }
        if (authority.find('@') != std::string::npos)
        {
            rejectionReason = "URL credentials are not allowed";
            return false;
        }
        if (authorityEnd != std::string::npos
            && xAddr[authorityEnd] == '#')
        {
            rejectionReason = "URL fragments are not allowed";
            return false;
        }

        const std::size_t colon = authority.find(':');
        const std::string host = authority.substr(0, colon);
        if (host.empty())
        {
            rejectionReason = "URL has no host";
            return false;
        }

        unsigned int port = scheme == "http" ? 80U : 443U;
        if (colon != std::string::npos)
        {
            if (authority.find(':', colon + 1) != std::string::npos)
            {
                rejectionReason = "Only IPv4 URL hosts are supported";
                return false;
            }
            const std::string portText = authority.substr(colon + 1);
            if (portText.empty())
            {
                rejectionReason = "URL port is empty";
                return false;
            }
            const auto parsedPort = std::from_chars(
                portText.data(), portText.data() + portText.size(), port);
            if (parsedPort.ec != std::errc{}
                || parsedPort.ptr != portText.data() + portText.size()
                || port == 0 || port > 65535)
            {
                rejectionReason = "URL port is invalid";
                return false;
            }
        }

        const bool allowedPort = scheme == "http"
            ? port == 80 || port == 8000 || port == 8080
            : port == 443;
        if (!allowedPort)
        {
            rejectionReason = "URL port is not allowed for its scheme";
            return false;
        }

        in_addr address{};
        if (inet_pton(AF_INET, host.c_str(), &address) != 1)
        {
            rejectionReason = "Host must be a numeric IPv4 address";
            return false;
        }
        if (!isPrivateIpv4(address))
        {
            rejectionReason = "Destination is not an RFC1918 private IPv4 address";
            return false;
        }

        bool matchesResponseSource = false;
        for (const std::string& responseSource : responseSources)
        {
            in_addr sourceAddress{};
            if (inet_pton(AF_INET, responseSource.c_str(), &sourceAddress) == 1
                && sourceAddress.s_addr == address.s_addr)
            {
                matchesResponseSource = true;
                break;
            }
        }
        if (!matchesResponseSource)
        {
            rejectionReason =
                "Destination does not match a WS-Discovery response source";
            return false;
        }

        return true;
    }

    static std::string ipv4String(const in_addr& address)
    {
        std::array<char, INET_ADDRSTRLEN> buffer{};
#ifdef _WIN32
        if (!InetNtopA(AF_INET, &address, buffer.data(),
                       static_cast<DWORD>(buffer.size())))
#else
        if (!inet_ntop(AF_INET, &address, buffer.data(), buffer.size()))
#endif
        {
            return {};
        }
        return buffer.data();
    }

    static std::string probeMessage()
    {
        static std::atomic<std::uint64_t> nextId{0};
        const std::uint64_t id =
            static_cast<std::uint64_t>(
                std::chrono::system_clock::now().time_since_epoch().count())
            ^ ++nextId;
        std::ostringstream messageId;
        messageId << "urn:uuid:00000000-0000-4000-8000-"
                  << std::hex << std::setw(12) << std::setfill('0')
                  << (id & 0xFFFFFFFFFFFFULL);

        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
            "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
            "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
            "<e:Header><w:MessageID>" + messageId.str()
            + "</w:MessageID><w:To>"
            "urn:schemas-xmlsoap-org:ws:2005:04:discovery"
            "</w:To><w:Action>"
            "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe"
            "</w:Action><w:ReplyTo><w:Address>"
            "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
            "</w:Address></w:ReplyTo></e:Header><e:Body><d:Probe>"
            "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
            "</d:Probe></e:Body></e:Envelope>";
    }

public:
    static constexpr std::size_t maximumDevicesPerOperation =
        maximumUniqueDevices;
    static constexpr std::size_t maximumXAddrAttemptsPerDevice = 2;

    static bool isPrivateIpv4Address(const std::string& value)
    {
        in_addr address{};
        return inet_pton(AF_INET, value.c_str(), &address) == 1
            && isPrivateIpv4(address);
    }

    static bool xAddrTargetsIpv4(const std::string& xAddr,
                                 const std::string& targetIp)
    {
        const std::size_t schemeEnd = xAddr.find("://");
        if (schemeEnd == std::string::npos)
        {
            return false;
        }

        const std::size_t authorityStart = schemeEnd + 3;
        const std::size_t authorityEnd = xAddr.find_first_of(
            "/?#", authorityStart);
        const std::string authority = xAddr.substr(
            authorityStart,
            authorityEnd == std::string::npos
                ? std::string::npos
                : authorityEnd - authorityStart);
        if (authority.empty() || authority.find('@') != std::string::npos)
        {
            return false;
        }

        const std::size_t colon = authority.find(':');
        if (colon != std::string::npos
            && authority.find(':', colon + 1) != std::string::npos)
        {
            return false;
        }
        return authority.substr(0, colon) == targetIp;
    }

    static bool isAcceptableXAddr(
        const std::string& xAddr,
        const std::vector<std::string>& responseSources,
        std::string& rejectionReason)
    {
        return parseXAddr(xAddr, responseSources, rejectionReason);
    }

    static std::vector<std::string> acceptableXAddrs(
        const DiscoveredDevice& device,
        std::size_t maximumAttempts,
        std::vector<std::string>& rejectionReasons)
    {
        std::vector<std::string> accepted;
        rejectionReasons.clear();
        for (const std::string& xAddr : device.xAddrs)
        {
            std::string reason;
            if (!parseXAddr(xAddr, device.responseSourceAddresses, reason))
            {
                rejectionReasons.push_back(std::move(reason));
                continue;
            }
            if (accepted.size() == maximumAttempts)
            {
                break;
            }
            accepted.push_back(xAddr);
        }
        return accepted;
    }

    static bool parseProbeMatches(const std::string& xml,
                                  std::vector<DiscoveredDevice>& devices,
                                  std::string& errorMessage,
                                  const std::string& responseSourceAddress = {},
                                  std::size_t maximumDevices =
                                      maximumUniqueDevices)
    {
        devices.clear();
        errorMessage.clear();

        soap* context = soap_new1(SOAP_DOM_TREE | SOAP_C_UTFSTRING);
        if (!context)
        {
            errorMessage = "Unable to allocate gSOAP XML parser";
            return false;
        }
        std::unique_ptr<soap, decltype(&soap_free)> soapContext(
            context, &soap_free);
        soap_dom_element envelope(context);
        std::istringstream input(xml);
        input >> envelope;
        if (context->error != SOAP_OK)
        {
            errorMessage = "Malformed WS-Discovery XML (gSOAP error "
                + std::to_string(context->error) + ")";
            return false;
        }
        if (!hasLocalName(&envelope, "Envelope")
            || !isOneOf(envelope.nstr, soap11Namespace, soap12Namespace))
        {
            errorMessage = "WS-Discovery response is not a SOAP Envelope (found {"
                + std::string(envelope.nstr ? envelope.nstr : "")
                + "}" + (envelope.name ? envelope.name : "") + ")";
            return false;
        }

        soap_dom_element* body = child(
            envelope, "Body", soap11Namespace, soap12Namespace);
        if (!body)
        {
            errorMessage = "WS-Discovery response has no SOAP Body";
            return false;
        }
        soap_dom_element* matches = child(
            *body, "ProbeMatches", discoveryNamespace, discovery11Namespace);
        if (!matches)
        {
            errorMessage = "SOAP Body has no WS-Discovery ProbeMatches";
            return false;
        }

        for (soap_dom_element* match = matches->elts;
             match;
             match = match->next)
        {
            if (!hasLocalName(match, "ProbeMatch")
                || !isOneOf(match->nstr, discoveryNamespace,
                            discovery11Namespace))
            {
                continue;
            }

            soap_dom_element* endpointReference = child(
                *match, "EndpointReference", addressingNamespace,
                addressing2005Namespace);
            soap_dom_element* address = endpointReference
                ? child(*endpointReference, "Address", addressingNamespace,
                        addressing2005Namespace)
                : nullptr;
            DiscoveredDevice device;
            device.endpointReference = text(address);
            if (device.endpointReference.empty())
            {
                if (errorMessage.empty())
                {
                    errorMessage = "Ignoring ProbeMatch without an EndpointReference";
                }
                continue;
            }
            if (!responseSourceAddress.empty())
            {
                device.responseSourceAddresses.push_back(
                    responseSourceAddress);
            }

            if (soap_dom_element* types = child(
                    *match, "Types", discoveryNamespace, discovery11Namespace))
            {
                device.types = splitWhitespace(text(types));
            }
            if (soap_dom_element* scopes = child(
                    *match, "Scopes", discoveryNamespace, discovery11Namespace))
            {
                device.scopes = splitWhitespace(text(scopes));
            }
            if (soap_dom_element* xAddrs = child(
                    *match, "XAddrs", discoveryNamespace, discovery11Namespace))
            {
                device.xAddrs = splitWhitespace(text(xAddrs));
            }
            if (soap_dom_element* metadataVersion = child(
                    *match, "MetadataVersion", discoveryNamespace,
                    discovery11Namespace))
            {
                const std::string value = text(metadataVersion);
                std::uint32_t parsedVersion = 0;
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), parsedVersion);
                if (parsed.ec != std::errc{}
                    || parsed.ptr != value.data() + value.size())
                {
                    if (errorMessage.empty())
                    {
                        errorMessage =
                            "Ignoring ProbeMatch with invalid MetadataVersion";
                    }
                    continue;
                }
                device.metadataVersion = parsedVersion;
            }
            if (!mergeDevice(devices, std::move(device), maximumDevices)
                && errorMessage.empty())
            {
                errorMessage =
                    "Maximum unique device count reached; additional devices ignored";
            }
        }

        return true;
    }

    std::vector<DiscoveredDevice> discover(
        std::chrono::milliseconds responseWindow =
            std::chrono::milliseconds(3000)) const
    {
        if (responseWindow.count() <= 0
            || responseWindow > std::chrono::seconds(30))
        {
            throw std::invalid_argument(
                "WS-Discovery response window must be between 1 ms and 30 s");
        }

        const SocketHandle socketHandle = ::socket(
            AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socketHandle == invalidSocket)
        {
            throw std::runtime_error(
                "Unable to create WS-Discovery UDP socket (socket error "
                + std::to_string(socketErrorCode()) + ")");
        }
        struct SocketGuard
        {
            SocketHandle value;
            ~SocketGuard()
            {
                if (value != invalidSocket)
                {
#ifdef _WIN32
                    closesocket(value);
#else
                    ::close(value);
#endif
                }
            }
        } socketGuard{socketHandle};

        sockaddr_in localAddress{};
        localAddress.sin_family = AF_INET;
        localAddress.sin_addr.s_addr = htonl(INADDR_ANY);
        localAddress.sin_port = htons(0);
        if (::bind(socketHandle, reinterpret_cast<sockaddr*>(&localAddress),
                   sizeof(localAddress)) == socketError)
        {
            throw std::runtime_error(
                "Unable to bind WS-Discovery UDP socket (socket error "
                + std::to_string(socketErrorCode()) + ")");
        }

#ifdef _WIN32
        const DWORD multicastTtl = 1;
#else
        const unsigned char multicastTtl = 1;
#endif
        if (setsockopt(socketHandle, IPPROTO_IP, IP_MULTICAST_TTL,
                       reinterpret_cast<const char*>(&multicastTtl),
                       sizeof(multicastTtl)) == socketError)
        {
            throw std::runtime_error(
                "Unable to configure WS-Discovery multicast TTL (socket error "
                + std::to_string(socketErrorCode()) + ")");
        }

        sockaddr_in multicastAddress{};
        multicastAddress.sin_family = AF_INET;
        multicastAddress.sin_port = htons(3702);
        if (inet_pton(AF_INET, "239.255.255.250",
                      &multicastAddress.sin_addr) != 1)
        {
            throw std::runtime_error(
                "Unable to configure WS-Discovery multicast address");
        }

        const std::string probe = probeMessage();
        const int sent = sendto(
            socketHandle, probe.data(), static_cast<int>(probe.size()), 0,
            reinterpret_cast<const sockaddr*>(&multicastAddress),
            sizeof(multicastAddress));
        if (sent == socketError
            || static_cast<std::size_t>(sent) != probe.size())
        {
            throw std::runtime_error(
                "Unable to send WS-Discovery Probe (socket error "
                + std::to_string(socketErrorCode()) + ")");
        }

        const auto deadline = std::chrono::steady_clock::now() + responseWindow;
        std::vector<DiscoveredDevice> devices;
        std::array<char, 65536> buffer{};
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::microseconds>(
                    deadline - std::chrono::steady_clock::now());
            timeval timeout{};
            timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
            timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socketHandle, &readable);
            const int ready = select(selectDescriptor(socketHandle),
                &readable, nullptr, nullptr, &timeout);
            if (ready == 0)
            {
                break;
            }
            if (ready == socketError)
            {
                throw std::runtime_error(
                    "WS-Discovery receive wait failed (socket error "
                    + std::to_string(socketErrorCode()) + ")");
            }

            sockaddr_in sender{};
            SocketLength senderLength = sizeof(sender);
            const int received = recvfrom(
                socketHandle, buffer.data(), static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&sender), &senderLength);
            if (received == socketError)
            {
                throw std::runtime_error(
                    "Unable to receive WS-Discovery response (socket error "
                    + std::to_string(socketErrorCode()) + ")");
            }

            std::vector<DiscoveredDevice> responseDevices;
            std::string parseError;
            const std::string response(buffer.data(),
                static_cast<std::size_t>(received));
            const std::string responseSourceAddress =
                ipv4String(sender.sin_addr);
            if (responseSourceAddress.empty()
                || !parseProbeMatches(response, responseDevices, parseError,
                                      responseSourceAddress,
                                      std::numeric_limits<std::size_t>::max()))
            {
                std::cerr << "[WS-Discovery] Ignoring response: "
                          << (parseError.empty()
                                  ? "unable to determine IPv4 source address"
                                  : parseError)
                          << std::endl;
                continue;
            }
            if (!parseError.empty())
            {
                std::cerr << "[WS-Discovery] " << parseError << std::endl;
            }
            for (DiscoveredDevice& device : responseDevices)
            {
                if (!mergeDevice(devices, std::move(device),
                                 maximumUniqueDevices))
                {
                    std::cerr << "[WS-Discovery] Maximum unique device count "
                              << "reached; ignoring additional devices"
                              << std::endl;
                }
            }
        }
        return devices;
    }

private:
    static int socketErrorCode()
    {
#ifdef _WIN32
        return WSAGetLastError();
#else
        return errno;
#endif
    }

    static int selectDescriptor(SocketHandle socketHandle)
    {
#ifdef _WIN32
        (void)socketHandle;
        return 0;
#else
        return socketHandle + 1;
#endif
    }
};
