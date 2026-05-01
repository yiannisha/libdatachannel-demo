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
- starts a local GStreamer camera pipeline
- captures camera frames
- encodes them with `x264enc`
- packetizes them with `rtph264pay`
- pulls complete RTP packets from an `appsink`
- forwards those RTP packets directly into a send-only WebRTC H.264 track
- creates a `DataChannel` named `producer-demo`
- sends a few example messages over the `DataChannel`

`Consumer`:

- creates its own `rtc::PeerConnection`
- connects to the producer's WebSocket signaling server
- receives the incoming H.264 video track with `onTrack(...)`
- logs RTP packets received on that track
- mirrors received RTP packets to a local UDP debug probe on `127.0.0.1:5004`
- receives the incoming `DataChannel` in `onDataChannel(...)`
- prints messages received over WebRTC

WebSockets are used only for signaling. The demo messages themselves are sent over the WebRTC data channel.

The current video path is:

- laptop camera -> GStreamer -> H.264 encode -> RTP packetize -> `appsink` -> `rtc::Track`

There is also a minimal browser viewer in [browser-viewer/index.html](browser-viewer/index.html) that can connect directly to the producer's WebSocket signaling server and render the remote WebRTC video track in a browser.

## Signaling Protocol

Both sides exchange JSON messages over WebSocket in this format:

```json
{
  "command": "localDescription" | "localCandidate",
  "description": "string form of SDP or ICE candidate",
  "type": "offer" | "answer",
  "mid": "video" | "0" | "data"
}
```

`type` is used for SDP messages and `mid` is used for ICE candidate messages. The native code still accepts older messages without those fields, but the browser viewer uses them explicitly.

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

The same signaling flow is used for both:

- the WebRTC data channel
- the producer's H.264 video track

No separate media signaling path is needed. The media track is negotiated through the same SDP and ICE exchange.

## Requirements

- C++17 compiler
- CMake
- OpenSSL discoverable by CMake
- GStreamer development packages, including `gstreamer-app-1.0`

On macOS with Homebrew:

```bash
brew install cmake openssl@3 gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly
```

On Ubuntu / Debian:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  pkg-config \
  libssl-dev \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly
```

On Fedora:

```bash
sudo dnf install -y \
  gcc-c++ \
  cmake \
  make \
  pkgconf-pkg-config \
  openssl-devel \
  gstreamer1-devel \
  gstreamer1-plugins-base-devel \
  gstreamer1-plugins-base \
  gstreamer1-plugins-good
```

On Fedora, the exact packages providing `x264enc` can depend on your enabled repositories. You need the GStreamer plugin set that includes:

- `x264enc`
- `h264parse`
- `rtph264pay`

On Arch Linux:

```bash
sudo pacman -S --needed \
  base-devel \
  cmake \
  pkgconf \
  openssl \
  gstreamer \
  gst-plugins-base \
  gst-plugins-good \
  gst-plugins-bad \
  gst-plugins-ugly
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

GStreamer is discovered via `pkg-config`. If configure fails, verify these modules are available:

- `gstreamer-1.0`
- `gstreamer-app-1.0`

At runtime, the producer also requires these GStreamer elements:

- `x264enc`
- `h264parse`
- `rtph264pay`

## Run

### Cross-device Producer/Consumer Demo

On the producer machine:

```bash
./scripts/run_producer.sh 8080 0.0.0.0
```

When the producer starts, it also starts this GStreamer pipeline:

```text
<platform camera source> !
videoconvert !
video/x-raw,format=I420,width=640,height=480,framerate=30/1 !
x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=4000 !
h264parse config-interval=-1 !
rtph264pay pt=96 ssrc=42 mtu=1200 config-interval=-1 aggregate-mode=zero-latency !
appsink
```

On macOS the source is `avfvideosrc`. On Linux it defaults to `autovideosrc`.

So you should see RTP packet logs even before a consumer connects, and once the WebRTC track opens those packets are forwarded into the peer connection.

Arguments:

- first argument: WebSocket server port, default `8080`
- second argument: bind address, default `0.0.0.0`
- third argument: video pipeline profile, default `default`

Supported producer pipeline profiles:

- `default`: current cross-platform camera pipeline using `avfvideosrc` on macOS or `autovideosrc` on Linux
- `zed-appsink`: simple ZED camera path based on `zedsrc`, matching the plain `../gst-test/zed-gstreamer/scripts/linux/simple-fps_rendering.sh` source side and then feeding the existing encode/RTP/appsink chain
- `zed-two-stream-appsink`: ZED two-output path that demuxes the camera feed into separate left and auxiliary RTP appsinks and publishes both as independent WebRTC video tracks

Example:

```bash
./scripts/run_producer.sh 8080 0.0.0.0 zed-appsink
```

Two-stream example:

```bash
./scripts/run_producer.sh 8080 0.0.0.0 zed-two-stream-appsink
```

On the consumer machine:

```bash
./scripts/run_consumer.sh ws://PRODUCER_HOST_OR_IP:8080/
```

Example:

```bash
./scripts/run_consumer.sh ws://192.168.1.50:8080/
```

### Browser Viewer

You can also view the producer's WebRTC video directly in a browser:

1. Start the producer.
2. Open [browser-viewer/index.html](browser-viewer/index.html) in a browser, or serve the repo over HTTP:

```bash
python3 -m http.server 8000
```

Then open:

```text
http://127.0.0.1:8000/browser-viewer/
```

3. Enter the producer WebSocket URL, for example:

```text
ws://127.0.0.1:8080/
```

4. Click `Connect`.

The page will:

- receive the producer's SDP offer
- create and send an SDP answer
- exchange ICE candidates
- attach each remote WebRTC video track to its own `<video>` tile
- log the incoming data channel messages

With `zed-two-stream-appsink`, the browser viewer renders two remote video tiles, one per outbound producer track.

To visualize the RTP mirrored by the consumer on the same machine, run:

```bash
gst-launch-1.0 -v \
  udpsrc port=5004 caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96" \
  ! rtpjitterbuffer latency=30 drop-on-latency=true \
  ! rtph264depay \
  ! h264parse \
  ! avdec_h264 \
  ! videoconvert \
  ! autovideosink sync=false
```

The consumer forwards raw RTP packets exactly as received from the WebRTC video track to that local UDP port.

For a live packet-level plot instead of a decoded video preview, run:

```bash
./scripts/run_rtp_plot.sh
```

That script listens to the same `127.0.0.1:5004` RTP mirror and opens a rolling matplotlib view with:

- packets per second
- bitrate in kbps
- RTP sequence deltas, where values above `1` indicate gaps and values below `1` indicate reordering or duplicates
- packet inter-arrival time in milliseconds

If `matplotlib` is missing, install it with:

```bash
python3 -m pip install matplotlib
```

### Single-process Demo

```bash
./build/libdatachannel_demo
```

## Expected Output

Producer:

```text
video pipeline started: avfvideosrc ! ... ! x264enc ... ! h264parse ... ! rtph264pay ... ! appsink
producer websocket server listening on ws://0.0.0.0:8080/
video pipeline state changed: NULL -> READY
video pipeline state changed: READY -> PAUSED
video pipeline produced RTP packet #1 bytes=779 payloadType=96 seq=3591 timestamp=3669289752 ssrc=42 trackOpen=false forwarded=buffered-or-skipped ...
video pipeline state changed: PAUSED -> PLAYING
producer websocket client connected
producer generated local description
producer peer state: connecting
producer video track open (video)
producer peer state: connected
producer data channel open (producer-demo)
```

Consumer:

```text
consumer UDP debug probe forwarding RTP to 127.0.0.1:5004
consumer websocket connected to ws://PRODUCER_HOST_OR_IP:8080/
consumer accepted track: mid=video direction=recvonly
consumer generated local description
consumer track open: video
consumer received RTP packet #1 on track video bytes=120 payloadType=96 seq=4534 timestamp=3669434654 ssrc=42
consumer accepted data channel: producer-demo
consumer data channel open (producer-demo)
consumer received on data channel: hello from producer
consumer received on data channel: second message from producer
consumer received on data channel: third message from producer
```

## Files

- [src/Producer.hpp](src/Producer.hpp) / [src/Producer.cpp](src/Producer.cpp): producer role and WebSocket server signaling
- [src/Consumer.hpp](src/Consumer.hpp) / [src/Consumer.cpp](src/Consumer.cpp): consumer role and WebSocket client signaling
- [src/VideoPipeline.hpp](src/VideoPipeline.hpp) / [src/VideoPipeline.cpp](src/VideoPipeline.cpp): GStreamer camera pipeline that captures frames into an `appsink`
- [src/SignalingProtocol.hpp](src/SignalingProtocol.hpp) / [src/SignalingProtocol.cpp](src/SignalingProtocol.cpp): JSON signaling schema and parsing helpers
- [src/producer_main.cpp](src/producer_main.cpp): producer executable entrypoint
- [src/consumer_main.cpp](src/consumer_main.cpp): consumer executable entrypoint
- [browser-viewer/index.html](browser-viewer/index.html): minimal browser-based WebRTC viewer
- [src/main.cpp](src/main.cpp): original single-process demo

## Current Limitations

- The producer accepts only one signaling WebSocket client.
- The demo uses a public STUN server but does not use TURN.
- Across restrictive NATs or firewalls, peer-to-peer connectivity may fail.
- The producer currently forwards RTP packets directly from GStreamer into the WebRTC track.
  This verifies H.264 RTP transport through libdatachannel, but it is still a low-level RTP path.
  There is not yet a receiver-side decoder or player in this repo.
- The consumer's UDP debug probe is local only.
  It mirrors packets to `127.0.0.1:5004` for debugging and local visualization.
- The signaling JSON schema only carries `command` and `description`.
  SDP type is inferred from local signaling state rather than sent explicitly.
  ICE `mid` is not sent separately.
- The demo is intentionally simple and sends hardcoded example messages once the data channel opens.
- On macOS, the terminal or executable may need camera permission the first time you run the producer.

If you want to extend this next, the most useful improvements would be:

- add explicit SDP `type` and ICE `mid` fields to the signaling protocol
- add TURN support
- add reconnection and disconnect handling
- replace the hardcoded data-channel messages with application-level messaging
