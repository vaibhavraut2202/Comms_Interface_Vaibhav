# Comms Interface

A C++ communication layer for camera commands.

## HTTP API

The application exposes an HTTP API bound to loopback by default
(`127.0.0.1:8090`); the bind address is configurable. The API has no
authentication, so keep it on loopback or a trusted network.
`POST /command` accepts JSON request bodies. Any request with a supported HTTP
content type and a complete, size-limited body receives HTTP `202` with a
request ID and `QUEUED` status when it is added to the command queue. The worker
then parses and validates the JSON and performs the requested operation.
Malformed or invalid business requests are therefore accepted first and later
appear as `FAILED` results when queried by request ID.

```json
{"camera_id":"CAM_007","operation":"GetDeviceInformation"}
```

`GetDeviceInformation` requires a valid `camera_id`. The Dispatcher reads that
camera's IP from the `paired_devices` table and combines it with the general
ONVIF port setting (8080 by default) to form the endpoint. A missing mapping or
invalid RFC1918 IPv4 address fails without sending SOAP. After a successful
response, the returned device information is saved on the same paired-device
row; the IP mapping is not changed. A database write failure makes the result
`FAILED` with a `DATABASE_ERROR`.

To list paired devices without contacting a camera, send:

```json
{"operation":"GetCurrentPairedDevices"}
```

`camera_id` is not required for this operation. Poll
`GET /result?id=<request_id>` for the current `QUEUED`, `PROCESSING`,
`SUCCESS`, or `FAILED` status. Device-information results keep their existing
response format. A successful paired-device result contains a `data` array of
`camera_id` and `ip_address` objects; when there are no records, it is `[]`.
Failures include an error category and message. Every camera ID used for
camera-specific ONVIF requests must already have an IP mapping in
`paired_devices`; discovery does not automatically pair devices. `GetProfiles`
and `GetPTZCapabilities` remain recognized but explicitly unsupported.

To discover ONVIF devices, send:

```json
{"operation":"DiscoverDevices"}
```

This operation does not require `camera_id`. The worker sends one WS-Discovery
Probe to `239.255.255.250:3702`, collects responses for up to three seconds,
stores discovery metadata in `discovered_devices`, then sequentially calls
GetDeviceInformation using each advertised XAddr until one succeeds. The result
reports `discovered_count`, `information_success_count`, and
`information_failure_count`. Discovery metadata is retained when information
collection fails; a later discovery refreshes the metadata and clears stale
device-information fields before trying again. Discovery remains separate from
pairing, and GetCurrentPairedDevices continues to return camera IDs and IPs.
Each operation retains at most eight unique devices, attempts at most two
acceptable XAddrs per device, and spends up to 60 seconds enriching information
after discovery. A SOAP XAddr must use
HTTP on port 80, 8000, or 8080, or HTTPS on port 443, and contain a numeric
RFC1918 IPv4 host matching the response's UDP source address. DNS names and
other destinations are rejected for SOAP use, but their advertised XAddrs
remain in the discovery record. This intentionally strict source-address
match can reject valid multi-homed-device or translated/NAT deployments where
the service address differs from the UDP response source. For a controlled
lab run, `--discovery-soap-target-ip PRIVATE_IPV4` limits SOAP enrichment to
that private IPv4 host while still recording metadata for every discovered
device. Without the option, discovery retains its existing multi-device
enrichment behavior.

The application creates `comms.db` in its current working directory at startup.
The database contains separate `paired_devices` and `discovered_devices`
tables, and their records persist across restarts. On startup, existing paired
tables are upgraded additively with nullable device-information and association
columns; rows and their IP mappings are preserved. Paired rows can retain the
EndpointReference of the explicitly associated discovered device; changing a
camera's IP through the paired-device upsert clears a stale association.

The generated gSOAP call uses bounded three-second connect, send, and receive
timeouts. Its connection attempt is also the availability check, so the HTTP
handler does not perform a duplicate network probe. No external JSON or HTTP
library is required: the project uses a small strict JSON parser for the
request shape and Winsock for HTTP.

Run the API with optional HTTP, general ONVIF-port, discovery-safety, and MQTT
configuration overrides:

```powershell
.\comms_interface.exe [--listen-address 127.0.0.1] [--port 8090] `
    [--onvif-port 8080] `
    [--discovery-soap-target-ip 172.16.10.38] `
    [--mqtt-host 127.0.0.1] [--mqtt-port 1883] `
    [--mqtt-client-id comms-interface] [--mqtt-qos 1]
```

## MQTT

MQTT and HTTP use the same `RequestSubmissionService`, request ID generator,
`CommandQueue`, `Worker`, validation, `Dispatcher`, and `ResultStore`. MQTT
messages are queued as raw payloads; the MQTT callback does not validate
business fields or execute operations. The worker emits one terminal result
notification after the ResultStore transition, and the MQTT client publishes
that result asynchronously.
If the broker is unavailable at startup or disconnects later, the MQTT client
retries with a configurable exponential delay while the HTTP server and worker
remain available.

The incoming command topic is `comms/command`. The outgoing topics are
`comms/ack`, `comms/response`, and `comms/status`. All are configurable with
the `--mqtt-*-topic` command-line options. The client defaults to
`127.0.0.1:1883`, MQTT 3.1.1, QoS 1, and client ID `comms-interface`.
Commands, acknowledgements, and responses are non-retained; status messages
are retained. The client publishes `ONLINE` after connecting and configures a
retained `OFFLINE` Last Will.

### Mosquitto broker

Mosquitto is already available on this development machine. On a new Windows
machine, install it with WinGet:

```powershell
winget install --id EclipseFoundation.Mosquitto --exact --source winget
```

For a local-only development broker, start it from the project root with the
included config. It explicitly binds port 1883 to loopback and allows
anonymous local development clients:

```powershell
& "C:\Program Files\Mosquitto\mosquitto.exe" `
    -c ".\config\mosquitto-local.conf" -v
```

Do not start a second broker if one is already listening on port 1883. Check
the listener before startup:

```powershell
Get-NetTCPConnection -LocalPort 1883 -State Listen
```

The `mosquitto`, `mosquitto_pub`, and `mosquitto_sub` executables are installed
under `C:\Program Files\Mosquitto`; add that directory to `PATH` or invoke them
by their full paths. In one PowerShell window subscribe:

```powershell
$mosquitto = "C:\Program Files\Mosquitto"
& "$mosquitto\mosquitto_sub.exe" -h 127.0.0.1 -p 1883 `
    -t "comms/#" -q 1 -v
```

In another window, publish a harmless database-read command:

```powershell
& "$mosquitto\mosquitto_pub.exe" -h 127.0.0.1 -p 1883 `
    -t "comms/command" -q 1 `
    -m '{"request_id":"REQ_001","operation":"GetCurrentPairedDevices"}'
```

The message gets an ACK with `request_id: "REQ_001"` and a generated
`internal_request_id`. The latter is the request ID accepted by the HTTP
`GET /result?id=...` endpoint. If `request_id` is missing, invalid, or the JSON
is malformed, the payload is still queued; the generated internal ID is also
used as the MQTT correlation ID.

To confirm queue-first validation without contacting a camera, also publish:

```powershell
& "$mosquitto\mosquitto_pub.exe" -h 127.0.0.1 -p 1883 `
    -t "comms/command" -q 1 `
    -m '{"request_id":"REQ_BAD","operation":"NotAnOperation"}'
```

It receives a `QUEUED` ACK followed by a `FAILED` response from the worker.

Example ACK:

```json
{"request_id":"REQ_001","internal_request_id":"req-...","status":"QUEUED","message":"Command accepted"}
```

Example final response:

```json
{"request_id":"REQ_001","internal_request_id":"req-...","camera_id":"","operation":"GetCurrentPairedDevices","status":"SUCCESS","data":[],"error":null}
```

Only the two implemented operations should be used for local tests. Do not send
`GetDeviceInformation` during these checks because it contacts the configured
camera.

### Paho MQTT C++ dependency and build

Paho C++ requires the Paho C library. The reproducible source build below uses
Paho C++ v1.6.0 and its Paho C v1.3.16 submodule, and installs static libraries
under the current user's profile. TLS is disabled for this loopback-only
development setup.

```powershell
$dependencyRoot = "$env:USERPROFILE\paho-mqtt"
git clone --branch v1.6.0 --depth 1 --recurse-submodules `
    https://github.com/eclipse-paho/paho.mqtt.cpp.git "$dependencyRoot\source"
cmake -S "$dependencyRoot\source" -B "$dependencyRoot\build" `
    -G "MinGW Makefiles" `
    -DCMAKE_INSTALL_PREFIX="$dependencyRoot\install" `
    -DPAHO_WITH_MQTT_C=ON -DPAHO_WITH_SSL=OFF `
    -DPAHO_BUILD_STATIC=ON -DPAHO_BUILD_SHARED=OFF `
    -DPAHO_BUILD_TESTS=OFF -DPAHO_BUILD_EXAMPLES=OFF `
    -DCMAKE_C_FLAGS=-Wno-error=incompatible-pointer-types
cmake --build "$dependencyRoot\build" --target install --parallel 4
```

Paho's upstream Windows instructions say MinGW is not directly supported. The
source build was verified here with MinGW-w64 GCC 16.1 and CMake 4.4.1; the C
compiler flag above is needed because the upstream C submodule passes Windows
`DWORD*` values to functions declared with `int*`. GCC continues to report
those warnings. The static library was built with those warnings non-fatal;
this is an environment-specific workaround, not a claim that upstream formally
supports MinGW.

Build from the project root in Windows PowerShell with MinGW-w64 and the
installed gSOAP 2.8.135 runtime. SQLite 3.54.1's official amalgamation source
is included in `third_party\sqlite`; compiling the C source locally avoids a
SQLite DLL or MinGW SQLite import library:

```powershell
$dependencyRoot = "$env:USERPROFILE\paho-mqtt\install"
$gsoapRoot = "$env:USERPROFILE\gsoap_2.8.135\gsoap-2.8\gsoap"
gcc -std=c11 -O2 -c third_party\sqlite\sqlite3.c -o third_party\sqlite\sqlite3.o
g++ -std=c++17 -I"third_party\sqlite" -I"generated" `
    -I"$gsoapRoot" `
    -I"$dependencyRoot\include" -pthread `
    src\main.cpp generated\soapC.cpp generated\soapDeviceBindingProxy.cpp `
    "$gsoapRoot\dom.cpp" "$gsoapRoot\stdsoap2.cpp" `
    third_party\sqlite\sqlite3.o -L"$dependencyRoot\lib" `
    -lpaho-mqttpp3-static -lpaho-mqtt3a-static `
    -lws2_32 -lrpcrt4 -lcrypt32 `
    -o comms_interface.exe
```

The parser and database tests use synthetic SOAP XML and a temporary local
SQLite database only. They do not send WS-Discovery probes:

```powershell
$gsoapRoot = "$env:USERPROFILE\gsoap_2.8.135\gsoap-2.8\gsoap"
$testRoot = Join-Path $env:TEMP "comms-ws-discovery-tests"
New-Item -ItemType Directory -Force $testRoot | Out-Null
gcc -std=c11 -O2 -c third_party\sqlite\sqlite3.c -o "$testRoot\sqlite3.o"
g++ -std=c++17 -I"third_party\sqlite" -I"generated" -I"$gsoapRoot" `
    tests\ws_discovery_tests.cpp generated\soapC.cpp `
    "$gsoapRoot\dom.cpp" "$gsoapRoot\stdsoap2.cpp" `
    "$testRoot\sqlite3.o" -lws2_32 -o "$testRoot\ws_discovery_tests.exe"
& "$testRoot\ws_discovery_tests.exe"
```

To exercise both operations, start `.\comms_interface.exe` in one PowerShell
window. In a second PowerShell window, from the project directory, post each
request and poll its result:

```powershell
$baseUrl = "http://127.0.0.1:8090"

function Wait-ForCommandResult([string]$requestId) {
    do {
        $result = Invoke-RestMethod -Uri "$baseUrl/result?id=$requestId"
        if ($result.status -eq "QUEUED" -or $result.status -eq "PROCESSING") {
            Start-Sleep -Milliseconds 250
        }
    } while ($result.status -eq "QUEUED" -or $result.status -eq "PROCESSING")
    $result
}

$pairedRequest = Invoke-RestMethod -Method Post `
    -Uri "$baseUrl/command" `
    -ContentType "application/json" `
    -Body '{"operation":"GetCurrentPairedDevices"}'
Wait-ForCommandResult $pairedRequest.request_id | ConvertTo-Json -Depth 8
```

This local test does not contact the camera. `GetCurrentPairedDevices` reads the
database directly; its successful `data` value is an array of `camera_id` and
`ip_address` objects (or `[]` if no devices are stored). Test
`GetDeviceInformation` separately only when you intend to contact the configured
camera.

### Delivery and duplicate handling

QoS 1 provides at-least-once delivery, not exactly-once command execution.
Within one process lifetime, a valid MQTT `request_id` is used as an
idempotency key: an identical payload is not enqueued twice, and reusing that
ID with a different payload is rejected. This in-memory deduplication does not
survive an application restart. Invalid or missing IDs use a generated
correlation ID and cannot be deduplicated across separate publishes.

The Paho client publishes asynchronously and logs publish failures. There is
no durable outbound result outbox; a terminal MQTT response can be lost if the
broker is unavailable at completion time. The result remains available through
HTTP while the process is running if the internal request ID is known. A
persistent outbox and durable idempotency store are future work.

## Development Environment

- C++17
- MinGW-w64
- gSOAP 2.8.135
- SQLite 3.54.1 amalgamation (`sqlite3.c` and `sqlite3.h`)
- Eclipse Paho MQTT C++ v1.6.0 and bundled Paho C v1.3.16
