# Virtio vsock

`UML::VsockDevice` is the host end of the guest's virtio-vsock device (Virtio
device 19). It carries a byte stream between a host application and a service
inside the guest, adbd on port 5555 for Andarium, with no listening socket on
the host and no network between them. It reuses the common vhost-user transport
and, like battery and console, builds on it alone.

The host only opens connections. The guest is CID 3 and the host CID 2, and the
device offers stream sockets alone with no optional feature. Every connection a
guest tries to open is refused with an RST, so no guest process can reach the
application.

## Layout

| File | Owns |
|---|---|
| `protocol.hpp` | the wire records, CIDs, ops and flow-control constants |
| `virtio_vsock_device.{hpp,cpp}` | the `VhostUserDevice`: packets, state machine, flow control |
| `connection_table.{hpp,cpp}` | per-connection state, the credit counters, the host port allocator |
| `host_events.{hpp,cpp}` | the kqueue the transport polls: readiness of the host ends, and the wake |
| `endpoint.{hpp,cpp}` | the thread-safe open API and the gateway it reaches the device through |
| `in_process_service.{hpp,cpp}` | the service on a thread named `uml-vsock`, and its endpoint |
| `forwarder.{hpp,cpp}` | macOS test tooling: loopback TCP listeners attached to guest ports |

## Rules the device keeps

- **Stream data on a disabled ring stays.** A transmit buffer is data the guest
  has handed over for good, so it stays on a ring the guest disabled to suspend
  and runs after the resume (`keepsRequestsOnDisabledRing()`).
- **A request is never deferred.** `processQueue` returns `complete` or
  `retry`, because disabling a ring with a deferred request pending resets the
  device and a reset ends every connection. Connections therefore survive both
  Android's autosuspend and the host's quiesce. The event queue always retries:
  the device sends no event.
- **Every guest-initiated request is answered with an RST**, and so is a packet
  for another CID, of another socket type, or for no connection.
- **The device is appended after every other Virtio device**, in the runtime and
  in the launcher, so that no other device's index moves: Android's vendor policy
  names the battery's sysfs path by its position.

## Connections

A connection is the pair (host port, guest port), with a bounded number open at
once. The host's end of each is a socket the device reads and writes without
blocking. Flow control follows Virtio 1.2, section 5.10.6.3, with credit
counters that wrap at 32 bits (`Credit` in `connection_table.hpp`). A device
reset, guest restart or service stop closes every host end, so the application
reads end of stream, and fails the opens in flight with `ECONNRESET`.

```cpp
uml::vsock::InProcessService started = uml::vsock::makeInProcessService();
// started.service goes to the guest as virtio_uml.device=<name>:19
int fd = started.endpoint->connect(5555, std::chrono::seconds(5));
```

`Endpoint::connect(port, timeout)` blocks up to the timeout and returns a
connected `SOCK_STREAM` descriptor the caller owns, or a negated errno
(`endpoint.hpp` lists them). `Endpoint::attach` does the same for a stream the
caller already has, and `begin`/`beginAttach` return the `Opening` for a caller
that must not block. The endpoint outlives the device and then answers
`-ENODEV`. The application's end is a plain socket: closing or half-closing it
is seen by the guest as the stream's end or half-close.

Wait for the application's end with `poll` or a kqueue before reading it, and
read only what they report. The device reports the guest's half-close as
`shutdown(SHUT_WR)` on its own end, which a blocking read can miss; the
[local stream socket rule](../../../docs/architecture.md#local-stream-sockets)
owns the contract.

## The forwarder

`macos` launchers take `--vsock-forward=[127.0.0.1:]HOSTPORT-GUESTPORT`,
repeatable. Each forward is a loopback TCP listener, and each accepted
connection is attached to the guest port, so a stock client of a guest service
reaches it: `adb connect 127.0.0.1:<port>` for adbd on 5555. Only a loopback
address is accepted. The device exists, appended last, only when a forward is
given.

## Tests

`tests/unit/vsock` drives the device through its queues with synthetic packets
and real socketpairs, no guest. `guest-vsock-stream` (integration) runs the same
device against a Linux guest's driver, including a suspend and resume in the
middle of a stream and a guest restart.
