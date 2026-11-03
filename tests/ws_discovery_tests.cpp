#include "../src/database.hpp"
#include "../src/dispatcher.hpp"
#include "../src/result_store.hpp"
#include "../src/ws_discovery.hpp"
#include "../generated/DeviceBinding.nsmap"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("Unable to read fixture: " + path.string());
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::string columnText(sqlite3_stmt* statement, int column)
{
    const auto* value = reinterpret_cast<const char*>(
        sqlite3_column_text(statement, column));
    return value
        ? std::string(value, static_cast<std::size_t>(
              sqlite3_column_bytes(statement, column)))
        : std::string();
}

void testProbeMatchParsing()
{
    std::vector<DiscoveredDevice> devices;
    std::string error;
    const std::string fixture = readFile(
        "tests/fixtures/probe_matches_multiple.xml");
    const bool parsed = WsDiscovery::parseProbeMatches(fixture, devices, error);
    require(parsed,
        "A valid ProbeMatches fixture was rejected: " + error);
    require(devices.size() == 2, "Duplicate UUIDs were not merged");
    require(devices[0].endpointReference
                == "urn:uuid:11111111-2222-3333-4444-555555555555",
        "The complete EndpointReference UUID was not preserved");
    require(devices[0].types.size() == 2,
        "Types from duplicate ProbeMatches were not preserved");
    require(devices[0].scopes.size() == 2,
        "Scopes from duplicate ProbeMatches were not preserved");
    require(devices[0].xAddrs.size() == 3,
        "Multiple XAddrs or duplicate merging failed");
    require(devices[0].metadataVersion == 5,
        "The latest metadata version was not retained");
    require(devices[1].endpointReference
                == "urn:uuid:aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        "A second discovered device was not parsed");

    devices.clear();
    error.clear();
    require(WsDiscovery::parseProbeMatches(
                fixture, devices, error, "192.168.1.25"),
        "ProbeMatches parsing failed with a UDP source address: " + error);
    require(devices[0].responseSourceAddresses
                == std::vector<std::string>{"192.168.1.25"},
        "The UDP response source was not associated with the discovered device");

    devices.clear();
    error.clear();
    const std::string malformed = readFile(
        "tests/fixtures/probe_matches_malformed.xml");
    require(!WsDiscovery::parseProbeMatches(malformed, devices, error),
        "Malformed XML was accepted");
    require(devices.empty(), "Malformed XML produced device records");
    require(!error.empty(), "Malformed XML did not provide an error");

    devices.clear();
    error.clear();
    require(WsDiscovery::parseProbeMatches(
                fixture, devices, error, {}, 1),
        "ProbeMatches parsing failed with a one-device limit: " + error);
    require(devices.size() == 1,
        "The unique-device limit was not enforced");
    require(!error.empty(),
        "Exceeding the unique-device limit was not reported");

    std::ostringstream manyDevices;
    manyDevices
        << "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
        << "xmlns:a=\"http://www.w3.org/2005/08/addressing\" "
        << "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
        << "<s:Body><d:ProbeMatches>";
    for (int index = 0; index < 10; ++index)
    {
        manyDevices << "<d:ProbeMatch><a:EndpointReference><a:Address>"
                    << "urn:uuid:device-" << index
                    << "</a:Address></a:EndpointReference></d:ProbeMatch>";
    }
    manyDevices << "</d:ProbeMatches></s:Body></s:Envelope>";
    devices.clear();
    error.clear();
    require(WsDiscovery::parseProbeMatches(
                manyDevices.str(), devices, error),
        "Parsing a response above the device limit failed: " + error);
    require(devices.size() == WsDiscovery::maximumDevicesPerOperation,
        "The configured maximum of eight unique devices was not enforced");
}

void testXAddrValidation()
{
    const std::vector<std::string> sourceAddresses{"192.168.1.25"};
    std::string rejectionReason;
    require(WsDiscovery::isAcceptableXAddr(
                "http://192.168.1.25/onvif/device_service",
                sourceAddresses, rejectionReason),
        "A valid RFC1918 HTTP XAddr was rejected: " + rejectionReason);
    require(WsDiscovery::isAcceptableXAddr(
                "https://10.20.30.40:443/onvif/device_service",
                {"10.20.30.40"}, rejectionReason),
        "A valid RFC1918 HTTPS XAddr was rejected: " + rejectionReason);
    require(WsDiscovery::isAcceptableXAddr(
                "http://172.20.30.40:8000/onvif/device_service",
                {"172.20.30.40"}, rejectionReason),
        "An allowed HTTP port was rejected: " + rejectionReason);
    require(WsDiscovery::isAcceptableXAddr(
                "http://172.16.10.38:8080/",
                {"172.16.10.38"}, rejectionReason),
        "A private IPv4 XAddr on port 8080 was rejected: " + rejectionReason);

    const std::vector<std::pair<std::string, std::string>> rejectedUrls = {
        {"http://8.8.8.8/onvif/device_service", "public IPv4"},
        {"http://127.0.0.1/onvif/device_service", "loopback IPv4"},
        {"http://169.254.1.20/onvif/device_service", "link-local IPv4"},
        {"http://239.1.2.3/onvif/device_service", "multicast IPv4"},
        {"http://255.255.255.255/onvif/device_service", "broadcast IPv4"},
        {"ftp://192.168.1.25/onvif/device_service", "unsupported scheme"},
        {"http://camera.local/onvif/device_service", "DNS hostname"},
        {"http:///onvif/device_service", "missing hostname"}
    };
    for (const auto& testCase : rejectedUrls)
    {
        rejectionReason.clear();
        require(!WsDiscovery::isAcceptableXAddr(
                    testCase.first, sourceAddresses, rejectionReason),
            "Rejected XAddr policy accepted " + testCase.second);
        require(!rejectionReason.empty(),
            "Rejected XAddr did not return a reason for " + testCase.second);
    }

    const std::string credentialUrl = std::string("http://")
        + "user" + ":" + "password" + "@192.168.1.25/onvif/device_service";
    rejectionReason.clear();
    require(!WsDiscovery::isAcceptableXAddr(
                credentialUrl, sourceAddresses, rejectionReason)
                && rejectionReason == "URL credentials are not allowed",
        "An XAddr containing embedded credentials was not rejected explicitly");

    rejectionReason.clear();
    require(!WsDiscovery::isAcceptableXAddr(
                "http://192.168.1.26/onvif/device_service",
                sourceAddresses, rejectionReason),
        "An XAddr not matching the UDP response source was accepted");

    rejectionReason.clear();
    require(!WsDiscovery::isAcceptableXAddr(
                "http://172.16.10.38:8080/",
                {"172.16.10.39"}, rejectionReason),
        "A port-8080 XAddr not matching the UDP response source was accepted");

    DiscoveredDevice device;
    device.endpointReference = "urn:uuid:test-device";
    device.responseSourceAddresses = sourceAddresses;
    device.xAddrs = {
        "http://8.8.8.8/onvif/device_service",
        "http://192.168.1.25/onvif/device_service",
        "http://192.168.1.25:8000/onvif/alternate",
        "https://192.168.1.25:443/onvif/third"
    };
    std::vector<std::string> rejectionReasons;
    const std::vector<std::string> selected =
        WsDiscovery::acceptableXAddrs(
            device, WsDiscovery::maximumXAddrAttemptsPerDevice,
            rejectionReasons);
    require(selected.size() == 2,
        "The maximum valid XAddr attempt count was not enforced");
    require(selected[0] == device.xAddrs[1]
                && selected[1] == device.xAddrs[2],
        "A rejected first XAddr prevented later valid addresses from selection");
    require(!rejectionReasons.empty(),
        "An invalid XAddr was not reported during selection");
    require(device.xAddrs.size() == 4,
        "XAddr selection modified the preserved discovery metadata");
}

void testDiscoverySoapTargetAllowlist()
{
    const std::optional<std::string> targetIp{"172.16.10.38"};
    const std::string piEndpoint = "http://172.16.10.38:8080/";
    const std::string otherEndpoint = "http://172.16.10.39:8080/";
    int soapCalls = 0;
    const auto testSoapCall = [&soapCalls]
    {
        ++soapCalls;
    };

    require(!Dispatcher::runEnrichmentIfAllowed(
                otherEndpoint, targetIp, testSoapCall),
        "A non-allowlisted device passed the SOAP enrichment gate");
    require(soapCalls == 0,
        "A non-allowlisted device reached SOAP enrichment");
    require(Dispatcher::runEnrichmentIfAllowed(
                piEndpoint, targetIp, testSoapCall),
        "The allowlisted Pi was rejected by the SOAP enrichment gate");
    require(soapCalls == 1,
        "The Pi was not the only SOAP enrichment target");
    require(Dispatcher::shouldEnrichEndpoint(
                otherEndpoint, std::nullopt),
        "Unrestricted production enrichment behavior changed");
}

void testEnrichmentDeadline()
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline{};
    require(Dispatcher::hasTimeForInformationAttempt(
                deadline, deadline - std::chrono::seconds(9)),
        "A SOAP attempt was rejected with exactly its timeout budget remaining");
    require(!Dispatcher::hasTimeForInformationAttempt(
                deadline, deadline - std::chrono::milliseconds(8999)),
        "A SOAP attempt was allowed without its full timeout budget");
    require(!Dispatcher::hasTimeForInformationAttempt(deadline, deadline),
        "A SOAP attempt was allowed after the enrichment deadline");
}

void testDatabaseUpsert()
{
    const auto uniquePart = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const std::filesystem::path databasePath =
        std::filesystem::temp_directory_path()
        / ("comms-discovery-" + std::to_string(uniquePart) + ".db");
    {
        Database database(databasePath.string());
        database.initialize();
        database.upsertPairedDevice("CAM_TEST", "192.0.2.20");
        const std::vector<PairedDevice> pairedDevices =
            database.getPairedDevices();
        require(pairedDevices.size() == 1
                    && pairedDevices.front().cameraId == "CAM_TEST"
                    && pairedDevices.front().ipAddress == "192.0.2.20",
            "The existing paired_devices behavior changed");

        DiscoveredDevice device;
        device.endpointReference =
            "urn:uuid:11111111-2222-3333-4444-555555555555";
        device.types = {"dn:NetworkVideoTransmitter"};
        device.scopes = {"onvif://www.onvif.org/name/Initial"};
        device.xAddrs = {"http://192.0.2.10/onvif/device_service"};
        device.metadataVersion = 4;
        database.upsertDiscoveredDevice(device);

        DeviceInformation information;
        information.manufacturer = "Acme";
        information.model = "Camera 1";
        information.firmwareVersion = "1.2.3";
        information.serialNumber = "serial-1";
        information.hardwareId = "hardware-1";
        database.updateDiscoveredDeviceInformation(
            device.endpointReference, information);

        device.types.push_back("tds:Device");
        device.scopes = {"onvif://www.onvif.org/name/Updated"};
        device.xAddrs.push_back("http://192.0.2.10/onvif/alternate");
        device.metadataVersion = 5;
        database.upsertDiscoveredDevice(device);
    }

    sqlite3* connection = nullptr;
    require(sqlite3_open(databasePath.string().c_str(), &connection) == SQLITE_OK,
        "Unable to reopen the local test database");
    sqlite3_stmt* statement = nullptr;
    const int prepared = sqlite3_prepare_v2(connection,
        "SELECT COUNT(*), types_json, scopes_json, xaddrs_json, "
        "metadata_version, manufacturer, model, firmware_version, "
        "serial_number, hardware_id FROM discovered_devices "
        "WHERE endpoint_reference = ?",
        -1, &statement, nullptr);
    require(prepared == SQLITE_OK, "Unable to prepare database verification");
    const std::string id =
        "urn:uuid:11111111-2222-3333-4444-555555555555";
    sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    require(sqlite3_step(statement) == SQLITE_ROW,
        "UPSERT did not leave a discovered-device row");
    require(sqlite3_column_int(statement, 0) == 1,
        "UPSERT created duplicate UUID records");
    require(columnText(statement, 1)
                == "[\"dn:NetworkVideoTransmitter\",\"tds:Device\"]",
        "UPSERT did not update Types");
    require(columnText(statement, 2)
                == "[\"onvif://www.onvif.org/name/Updated\"]",
        "UPSERT did not update Scopes");
    require(columnText(statement, 3)
                == "[\"http://192.0.2.10/onvif/device_service\","
                   "\"http://192.0.2.10/onvif/alternate\"]",
        "UPSERT did not preserve all XAddrs");
    require(sqlite3_column_int(statement, 4) == 5,
        "UPSERT did not update MetadataVersion");
    for (int column = 5; column < 10; ++column)
    {
        require(sqlite3_column_type(statement, column) == SQLITE_NULL,
            "A rediscovery did not reset stale device information");
    }
    sqlite3_finalize(statement);
    sqlite3_close(connection);
    std::filesystem::remove(databasePath);
}

void testDatabaseDrivenCameraEndpoint()
{
    const auto uniquePart = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const std::filesystem::path databasePath =
        std::filesystem::temp_directory_path()
        / ("comms-endpoint-" + std::to_string(uniquePart) + ".db");

    sqlite3* setupConnection = nullptr;
    require(sqlite3_open(databasePath.string().c_str(), &setupConnection)
                == SQLITE_OK,
        "Unable to create the legacy-schema test database");
    char* setupError = nullptr;
    const int setupResult = sqlite3_exec(
        setupConnection,
        "CREATE TABLE paired_devices ("
        "camera_id TEXT PRIMARY KEY, ip_address TEXT NOT NULL);"
        "INSERT INTO paired_devices (camera_id, ip_address) "
        "VALUES ('CAM_TEST', '172.16.10.38');",
        nullptr, nullptr, &setupError);
    if (setupResult != SQLITE_OK)
    {
        const std::string message = setupError
            ? setupError
            : sqlite3_errmsg(setupConnection);
        sqlite3_free(setupError);
        sqlite3_close(setupConnection);
        throw std::runtime_error(
            "Unable to create the legacy paired_devices schema: " + message);
    }
    sqlite3_close(setupConnection);

    {
        Database database(databasePath.string());
        database.initialize();

        DiscoveredDevice discovered;
        discovered.endpointReference = "urn:uuid:camera-test";
        discovered.xAddrs = {"http://172.16.10.38:8080/"};
        database.upsertDiscoveredDevice(discovered);

        const auto initialIp =
            database.getPairedDeviceIpAddress("CAM_TEST");
        require(initialIp && *initialIp == "172.16.10.38",
            "The additive schema migration did not preserve the paired IP");

        DeviceConfig config;
        std::string resolvedIp;
        std::string errorCategory;
        std::string errorMessage;
        const auto endpoint = Dispatcher::resolveCameraEndpoint(
            database, config, "CAM_TEST", resolvedIp,
            errorCategory, errorMessage);
        require(endpoint && *endpoint == "http://172.16.10.38:8080/"
                    && resolvedIp == "172.16.10.38",
            "The camera endpoint was not resolved from its SQLite mapping");

        const auto missingEndpoint = Dispatcher::resolveCameraEndpoint(
            database, config, "CAM_MISSING", resolvedIp,
            errorCategory, errorMessage);
        require(!missingEndpoint && errorCategory == "CAMERA_NOT_PAIRED"
                    && !errorMessage.empty(),
            "A missing camera mapping did not fail explicitly");

        database.upsertPairedDevice("CAM_TEST", "not-an-ip");
        const auto invalidEndpoint = Dispatcher::resolveCameraEndpoint(
            database, config, "CAM_TEST", resolvedIp,
            errorCategory, errorMessage);
        require(!invalidEndpoint
                    && errorCategory == "ENDPOINT_RESOLUTION_ERROR"
                    && !errorMessage.empty(),
            "An invalid camera IP did not fail endpoint resolution");

        database.upsertPairedDevice("CAM_TEST", "172.16.10.38");
        database.associatePairedDevice(
            "CAM_TEST", "172.16.10.38", discovered.endpointReference);
        DeviceInformation information;
        information.manufacturer = "Vaibhav ONVIF Lab";
        information.model = "RaspberryPi-ONVIF";
        information.firmwareVersion = "1.0";
        information.serialNumber = "PI4-001";
        information.hardwareId = "RPI4";
        database.updatePairedDeviceInformation(
            "CAM_TEST", "172.16.10.38", information);

        const auto persistedIp =
            database.getPairedDeviceIpAddress("CAM_TEST");
        const auto persistedInformation =
            database.getPairedDeviceInformation("CAM_TEST");
        const auto persistedEndpointReference =
            database.getPairedDeviceEndpointReference("CAM_TEST");
        require(persistedIp && *persistedIp == "172.16.10.38",
            "Saving device information overwrote the paired IP mapping");
        require(persistedInformation
                    && persistedInformation->manufacturer
                        == information.manufacturer
                    && persistedInformation->model == information.model
                    && persistedInformation->firmwareVersion
                        == information.firmwareVersion
                    && persistedInformation->serialNumber
                        == information.serialNumber
                    && persistedInformation->hardwareId
                        == information.hardwareId,
            "Device information was not persisted against the paired camera");
        require(persistedEndpointReference
                    && *persistedEndpointReference
                        == discovered.endpointReference,
            "The discovered-device association was not retained");

        config.onvifPort = 0;
        const auto invalidPortEndpoint = Dispatcher::resolveCameraEndpoint(
            database, config, "CAM_TEST", resolvedIp,
            errorCategory, errorMessage);
        require(!invalidPortEndpoint
                    && errorCategory == "ENDPOINT_RESOLUTION_ERROR",
            "An invalid ONVIF port did not fail endpoint resolution");
    }
    std::filesystem::remove(databasePath);
}

void testDiscoverySummaryStorage()
{
    ResultStore results;
    Command command;
    command.requestId = "discovery-summary-test";
    command.operation = "DiscoverDevices";
    results.addQueued(command);
    require(results.markProcessing(command.requestId),
        "Discovery request did not enter PROCESSING");
    results.complete(command.requestId,
        OperationResult::discoverySuccess(DiscoverySummary{2, 1, 1}));

    const auto record = results.find(command.requestId);
    require(record && record->discoverySummary,
        "Discovery summary was not retained in the result store");
    require(record->discoverySummary->discoveredCount == 2
                && record->discoverySummary->informationSuccessCount == 1
                && record->discoverySummary->informationFailureCount == 1,
        "Discovery summary counts were not retained");
    require(!record->deviceInformation,
        "Discovery result was incorrectly stored as device information");
}
}

int main()
{
    try
    {
        testProbeMatchParsing();
        testXAddrValidation();
        testDiscoverySoapTargetAllowlist();
        testEnrichmentDeadline();
        testDatabaseUpsert();
        testDatabaseDrivenCameraEndpoint();
        testDiscoverySummaryStorage();
        std::cout << "WS-Discovery parser and database tests passed"
                  << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "WS-Discovery tests failed: " << error.what() << std::endl;
        return 1;
    }
}
