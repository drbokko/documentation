# Stream V2 Examples

C examples and small Python tools for receiving and inspecting DECTRIS Stream V2 data over ZeroMQ.

## Layout

| Location | Contents |
|----------|----------|
| `src/` | Sources for the static libraries (`stream2`, `stream2_helpers`) |
| `*.c` in this directory | Executable programs |
| `third_party/` | tinycbor, compression stack, and other CMake subprojects |

## C

### Requirements

- **Parser and helpers** use [tinycbor] and [dectris-compression] (pulled in via CMake / `third_party`).
- **Half-precision**: either a C11 compiler with ISO/IEC TS 18661-3 half-float support, or x86-64 with SSE2 and F16C.

### Libraries

CMake builds static targets **`stream2`** (parser) and **`stream2_helpers`** (linked by all C examples):

| Module | Role |
|--------|------|
| `src/stream2` | Stream V2 CBOR message parser |
| `src/stream2_common` | Clock, signals, shared helpers |
| `src/stream2_stats` | Receive / throughput statistics |
| `src/stream2_decompress` | Decompress image channels |
| `src/stream2_image_buffer` | In-memory buffering, optional ZMQ zero-copy |
| `src/tiff_writer` | Write TIFFs from buffered images (single- or multi-threaded flush) |

### Programs

| Target | Description |
|--------|-------------|
| `stream2_dump` | Decode messages and print to stdout; useful to learn the protocol. |
| `stream2_buffer` | Receive and buffer images, print stats; does not decompress for storage or write files. |
| `stream2_buffer_decode` | Same as `stream2_buffer`, then a final pass to decompress and report compression ratios. |
| `DectrisStream2Receiver_linux` | Buffer, decompress, write TIFFs; `--threads` sets writer threads (default 10). |
| `stream2_bifurcator` | Relay: listen on one host/port, buffer, forward raw stream on another interface/port. |

**Windows-only targets** (when building on Windows): `DectrisStream2Demo_windows`, `start_stream_eigerclient`.

**Buffer cap:** programs that use the image buffer respect **`STREAM2_BUFFER_GB`** (default `20`).

**Linux receiver:** optional **`STREAM2_NET_IFACE`** selects the network interface for binding when applicable (see program help / source).

### Bifurcator usage

```sh
# Receive from 192.168.1.100:31001, publish on 192.168.2.1:31002
./stream2_bifurcator 192.168.1.100 192.168.2.1 31002
```

Downstream clients should use ZMQ `PULL` like other Stream V2 tools. The bifurcator uses `PUSH` with timeouts; slow consumers may see send timeouts while data stays buffered locally.

On shutdown it can flush buffered images to TIFF; thread count uses **`STREAM2_TIFF_THREADS`** (default `10`, overridable in code paths that call the multi-threaded writer).

### Bifurcator tuning (e.g. ConnectX NICs)

```sh
STREAM2_RCVBUF_MB=512 \
STREAM2_SNDBUF_MB=512 \
STREAM2_BUSY_POLL_US=50 \
STREAM2_CPU_AFFINITY=0 \
STREAM2_IO_THREADS=4 \
STREAM2_REALTIME=1 \
./stream2_bifurcator 192.168.1.100 192.168.2.1 31002
```

| Variable | Meaning | Default |
|----------|---------|---------|
| `STREAM2_RCVBUF_MB` / `STREAM2_SNDBUF_MB` | ZMQ socket buffers (MB) | 256 |
| `STREAM2_BUSY_POLL_US` | Busy-poll timeout (µs); `0` = off | 0 |
| `STREAM2_CPU_AFFINITY` | Pin process to a CPU core | off |
| `STREAM2_IO_THREADS` | ZMQ I/O threads | 2 |
| `STREAM2_REALTIME` | Realtime priority (Linux; often needs root) | 0 |

**Host sysctl (Linux, as root)** — raise socket buffer limits when pushing high throughput:

```sh
sysctl -w net.core.rmem_max=536870912
sysctl -w net.core.wmem_max=536870912
sysctl -w net.core.rmem_default=536870912
sysctl -w net.core.wmem_default=536870912
```

Match IRQ affinity to your CPU/NIC layout if you pin the process (`/proc/interrupts`, `smp_affinity`). For dedicated NICs, consider stopping `irqbalance`.

*Tested on DGX systems with ConnectX-7 and on Xeon servers with ConnectX-6.*

### Build — Linux

Initialize submodules, then configure (system libzmq or bundled build):

```sh
git submodule update --init --recursive
mkdir build && cd build
cmake /path/to/documentation/stream_v2/examples -DCMAKE_BUILD_TYPE=Debug -DBUILD_LIBZMQ=YES
cmake --build .
./stream2_dump YOUR_DCU_HOST
```

Set **`BUILD_LIBZMQ=YES`** to fetch and build ZeroMQ from source; otherwise CMake expects an installed `libzmq`.

### Build — Windows

Open a **Visual Studio** developer shell (adjust the path to match your install), then:

```powershell
cd documentation\stream_v2\examples
cmake . -DCMAKE_BUILD_TYPE=Debug -DBUILD_LIBZMQ=YES
cmake --build .
```

### Cleaning

- **Makefile / Ninja:** `cmake --build . --target clean` — removes build products, keeps the CMake cache.
- **Deep clean:** `cmake --build . --target clean-build` — also removes `bin/`, `lib/`, generated `CMakeFiles`, cache, `compile_commands.json`, and `_deps` under the build tree (full reconfigure next time).

## Python

### `client.py`

Minimal Stream V2 receiver using `cbor2`, `pyzmq`, `numpy`, and `dectris-compression`. `MultiDimArray` / `TypedArray` fields become NumPy arrays.

```sh
pip install cbor2 "dectris-compression~=0.3.0" numpy pyzmq
python client.py
```



[dectris-compression]: https://github.com/dectris/compression
[tinycbor]: https://github.com/intel/tinycbor
