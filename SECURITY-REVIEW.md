# WS-Discovery Code Review

Review scope: read-only review of the ONVIF WS-Discovery feature and its
integration. No application or network tests were run as part of this review.

## Findings

### High — Windows multicast TTL option uses the wrong value size

**Location:** `src/ws_discovery.hpp`, `WsDiscovery::discover`, multicast TTL
configuration.

The code passes an `unsigned char` and `sizeof(unsigned char)` to
`setsockopt(IPPROTO_IP, IP_MULTICAST_TTL, ...)`. Winsock expects the option
value as a `DWORD`. The error path throws before the Probe is sent, so this
Windows-specific mismatch can prevent discovery from starting.

**Recommendation:** Use a Winsock `DWORD` value and its size on Windows. Keep
the POSIX option representation in the non-Windows branch if portability is
needed. Verify the option setup locally without sending multicast traffic.

### Medium — Unvalidated XAddrs cause outbound SOAP requests to arbitrary hosts

**Location:** `src/ws_discovery.hpp`, response receive and ProbeMatch parsing;
`src/dispatcher.hpp`, `Dispatcher::dispatch`; `src/onvif_client.hpp`,
`OnvifClient::getDeviceInformation`.

The UDP sender address is captured but not checked, and Probe responses are
not correlated with the outstanding Probe's `MessageID`. Each advertised
XAddr is passed directly to gSOAP. A host able to send a response to the
discovery socket can therefore influence the destination of an outbound SOAP
request when discovery is triggered. This is a potential SSRF path, especially
if HTTP is bound beyond loopback or the MQTT command channel is accessible to
untrusted clients.

**Recommendation:** Validate response correlation and sender, restrict XAddr
schemes and destinations to the intended local-device policy, and reject
addresses outside that policy before calling gSOAP.

### Medium — A discovery request has no overall enrichment-work budget

**Location:** `src/dispatcher.hpp`, `Dispatcher::dispatch`; `src/onvif_client.hpp`,
`OnvifClient::getDeviceInformation`.

The single Worker tries every XAddr for every discovered device sequentially.
Each ONVIF call configures three-second connect, send, and receive timeouts, but
there is no total operation deadline or cap on devices/endpoints processed.
Consequently one discovery can hold the only Worker for a long time and delay
unrelated HTTP and MQTT commands. A response containing many XAddrs makes this
more pronounced.

**Recommendation:** Preserve sequential processing but add a clear total
operation budget and limits for accepted devices/endpoints; report skipped work
in the summary or logs.

### Low — README build instructions contain a developer-specific gSOAP path

**Location:** `README.md`, application and test build instructions.

The documented `$gsoapRoot` is set to `C:\\Users\\vaibh\\...`, so the commands
will not work unchanged for other account names or gSOAP installation paths.

**Recommendation:** Document a user-configurable path or derive the path from
an environment variable.

## Review notes

- The Probe uses SOAP 1.2, WS-Addressing 2004/08, WS-Discovery 2005/04, and the
  ONVIF NetworkVideoTransmitter QName. Its `To`, `Action`, and anonymous
  `ReplyTo` values are consistent with that exchange.
- The destination is `239.255.255.250:3702`. The response loop uses a steady
  clock and `select` with the remaining deadline; it can receive multiple
  datagrams and has no receive-side infinite wait.
- gSOAP DOM parsing checks namespace URIs and local element names. It accepts
  SOAP 1.1/1.2, WS-Discovery 1.0/1.1, and both supported WS-Addressing
  namespaces. Duplicate EndpointReferences merge Types, Scopes, XAddrs, and
  retain the greatest MetadataVersion.
- A ProbeMatch with no EndpointReference is ignored; malformed XML is rejected
  per response. Missing XAddrs or MetadataVersion are not rejected: such a
  record can count as discovered while information collection fails or uses
  the default metadata version of zero.
- `discovered_devices` uses the full EndpointReference string as its primary
  key. Lists are bound as JSON text; SQL values use prepared statements. The
  database mutex protects each operation. `paired_devices` and its query path
  remain separate.
- Rediscovery deliberately clears the five information fields before a new
  enrichment attempt. Thus discovery metadata survives a failed attempt, but
  previously successful manufacturer/model/etc. values do not. This is
  documented and asserted by the current test; confirm that this freshness
  policy is desired.
- The Dispatcher supplies each discovered XAddr to the existing
  GetDeviceInformation call, tries later XAddrs after SOAP failures, and moves
  on to the next device. The result remains successful with counts for
  partial failures and zero devices.
- Socket and gSOAP contexts have scoped cleanup. The README includes
  `dom.cpp` in both manual build commands. No CMake build configuration was
  found.

## Test assessment

During the preceding implementation task, local C++17 syntax checks, an
application link, and synthetic parser/SQLite/ResultStore tests completed
successfully. Those tests cover multiple devices, duplicate UUID merging,
multiple XAddrs, malformed XML, SQLite UPSERT, paired-device preservation, and
summary storage.

The tests do not exercise Winsock option setup, actual UDP response collection,
sender/correlation validation, Dispatcher endpoint fallback, HTTP/MQTT summary
serialization, or full queue behavior. No application runtime, physical
multicast, camera, or Raspberry Pi test was performed.
