# libdatachannel C++ Demo

This project builds a tiny single-process libdatachannel example in C++.

It creates two `rtc::PeerConnection` instances in the same executable, forwards SDP and ICE candidates in memory, opens a `DataChannel` from the offerer to the answerer, exchanges one text message in each direction, and exits.

## Requirements

- A C++17 compiler
- CMake
- OpenSSL discoverable by CMake

On macOS with Homebrew:

```bash
brew install cmake openssl@3
```

## Build

```bash
cmake -S . -B build
cmake --build build
```

If CMake cannot find OpenSSL automatically, configure with:

```bash
cmake -S . -B build -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
```

## Run

```bash
./build/libdatachannel_demo
```

Expected output is similar to:

```text
offerer produced local description
answerer produced local description
answerer accepted data channel: demo
offerer channel open (demo)
answerer channel open (demo)
offerer received: hello from answerer
answerer received: hello from offerer
Demo completed successfully
```
