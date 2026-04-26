# libdatachannel Demo

This repo contains two small C++ demos built on top of `libdatachannel`:

- `libdatachannel_demo`: a single-process demo where two `rtc::PeerConnection` objects exchange SDP and ICE in memory
- `producer` / `consumer`: a two-process demo where signaling happens over WebSocket and the actual payload moves over a WebRTC `DataChannel`

The `producer` / `consumer` flow is the main demo for running across two different devices.

## What The Producer/Consumer Demo Does

`Producer`:

- creates an `rtc::PeerConnection`
- starts a WebSocket signaling server
- accepts exactly one WebSocket client
- creates a `DataChannel` named `producer-demo`
- sends a few example messages over the `DataChannel`

`Consumer`:

- creates its own `rtc::PeerConnection`
- connects to the producer's WebSocket signaling server
- receives the incoming `DataChannel` in `onDataChannel(...)`
- prints messages received over WebRTC

WebSockets are used only for signaling. The demo messages themselves are sent over the WebRTC data channel.

## Signaling Protocol

Both sides exchange JSON messages over WebSocket in this format:

```json
{
  "command": "localDescription" | "localCandidate",
  "description": "string form of rtc::Description or rtc::Candidate"
}
```

Examples:

```json
{
  "command": "localDescription",
  "description": "v=0\r\no=- 123456789 1 IN IP4 0.0.0.0\r\n..."
}
```

```json
{
  "command": "localCandidate",
  "description": "a=candidate:1 1 UDP 2122260223 192.168.1.20 56032 typ host"
}
```

When a side receives:

- `localDescription`: it parses the SDP and calls `setRemoteDescription(...)`
- `localCandidate`: it parses the ICE candidate and calls `addRemoteCandidate(...)`

If ICE candidates arrive before the remote description is set, they are queued and applied later.

## Requirements

- C++17 compiler
- CMake
- OpenSSL discoverable by CMake

On macOS with Homebrew:

```bash
brew install cmake openssl@3
```

On Ubuntu / Debian:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libssl-dev
```

On Fedora:

```bash
sudo dnf install -y gcc-c++ cmake make pkgconf-pkg-config openssl-devel
```

On Arch Linux:

```bash
sudo pacman -S --needed base-devel cmake pkgconf openssl
```

## Build

```bash
cmake -S . -B build
cmake --build build
```

If OpenSSL is not found automatically:

```bash
cmake -S . -B build -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build
```

On Linux, OpenSSL is usually found automatically from the system packages above. If it is not, check that the development package is installed:

- Ubuntu / Debian: `libssl-dev`
- Fedora: `openssl-devel`
- Arch Linux: `openssl`

## Run

### Cross-device Producer/Consumer Demo

On the producer machine:

```bash
./scripts/run_producer.sh 8080 0.0.0.0
```

Arguments:

- first argument: WebSocket server port, default `8080`
- second argument: bind address, default `0.0.0.0`

On the consumer machine:

```bash
./scripts/run_consumer.sh ws://PRODUCER_HOST_OR_IP:8080/
```

Example:

```bash
./scripts/run_consumer.sh ws://192.168.1.50:8080/
```

### Single-process Demo

```bash
./build/libdatachannel_demo
```

## Expected Output

Producer:

```text
producer websocket server listening on ws://0.0.0.0:8080/
producer websocket client connected
producer generated local description
producer peer state: connecting
producer peer state: connected
producer data channel open (producer-demo)
```

Consumer:

```text
consumer websocket connected to ws://PRODUCER_HOST_OR_IP:8080/
consumer generated local description
consumer accepted data channel: producer-demo
consumer data channel open (producer-demo)
consumer received on data channel: hello from producer
consumer received on data channel: second message from producer
consumer received on data channel: third message from producer
```

## Files

- [src/Producer.hpp](src/Producer.hpp) / [src/Producer.cpp](src/Producer.cpp): producer role and WebSocket server signaling
- [src/Consumer.hpp](src/Consumer.hpp) / [src/Consumer.cpp](src/Consumer.cpp): consumer role and WebSocket client signaling
- [src/SignalingProtocol.hpp](src/SignalingProtocol.hpp) / [src/SignalingProtocol.cpp](src/SignalingProtocol.cpp): JSON signaling schema and parsing helpers
- [src/producer_main.cpp](src/producer_main.cpp): producer executable entrypoint
- [src/consumer_main.cpp](src/consumer_main.cpp): consumer executable entrypoint
- [src/main.cpp](src/main.cpp): original single-process demo

## Current Limitations

- The producer accepts only one signaling WebSocket client.
- The demo uses a public STUN server but does not use TURN.
- Across restrictive NATs or firewalls, peer-to-peer connectivity may fail.
- The signaling JSON schema only carries `command` and `description`.
  SDP type is inferred from local signaling state rather than sent explicitly.
  ICE `mid` is not sent separately.
- The demo is intentionally simple and sends hardcoded example messages once the data channel opens.

If you want to extend this next, the most useful improvements would be:

- add explicit SDP `type` and ICE `mid` fields to the signaling protocol
- add TURN support
- add reconnection and disconnect handling
- replace the hardcoded data-channel messages with application-level messaging
