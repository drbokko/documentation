# Stream V2 Examples

## C

`stream2.c` and `stream2.h` implement a stream V2 parser using [tinycbor]. [dectris-compression] is used to decompress image channel data.

The code requires compiler support for half-float conversions. Any C compiler supporting C11 extension ISO/IEC TS 18661-3 will work. Otherwise, x86-64 intrinsics for SSE2 and F16C are required. If the code does not work with your compiler, please let us know.

#### Library Structure

The examples use a shared helper library (`stream2_helpers`) that provides common functionality:

| Module | Description |
|--------|-------------|
| `stream2_common.h/c` | Platform compatibility (Windows/POSIX clock, signal handling) |
| `stream2_stats.h/c` | Statistics tracking and reporting |
| `stream2_decompress.h/c` | Decompression helpers |
| `stream2_image_buffer.h/c` | Image buffering with zero-copy support |
| `stream2_tiff.h/c` | TIFF file writing (single and multi-threaded) |

#### Example Programs

| Program | Description |
|---------|-------------|
| `example` | Basic example that dumps received Stream V2 messages to stdout. Good starting point to understand the protocol. |
| `check_stream2` | Simple stream checker that counts images and total bytes received. Minimal overhead. |
| `stream2_buffer` | Receives stream data over ZMQ and buffers images in memory (up to a configurable limit). Reports throughput statistics. Uses zero-copy where possible. Does **not** decompress or save files. |
| `stream2_buffer_decode` | Like `stream2_buffer`, but also decompresses the image data and reports compression statistics (ratio, compressed vs decompressed bytes). Does **not** save files. |
| `stream2_buffer_tiff` | Like `stream2_buffer_decode`, but writes buffered images to TIFF files on disk using **multi-threaded** parallel writing. Thread count configurable via `STREAM2_TIFF_THREADS` env var. |
| `stream2_writer_single_threaded` | Simpler **single-threaded** TIFF writer. Receives stream data, buffers images, prints first image metadata, and writes TIFF files sequentially. |
| `stream2_bifurcator` | **Stream relay/forwarder** for dual-NIC setups. Receives on one interface, buffers in RAM, and re-broadcasts raw messages on another interface without modification. |

All buffer-based programs support the `STREAM2_BUFFER_GB` environment variable to set the memory buffer limit (default: 20 GB).
Tested on a DGX Spark with the ConnectX7 and on a high performance Xeon EDGE server with a ConnectX 6.

#### Stream Bifurcator Usage

The bifurcator is designed for machines with two fast network interfaces, acting as a relay that buffers data locally while forwarding to downstream consumers.

```sh
# Receive from 192.168.1.100:31001, publish on 192.168.2.1:31002
./stream2_bifurcator 192.168.1.100 192.168.2.1 31002
```

Downstream clients should connect using ZMQ_PULL sockets (matching the Stream V2 protocol). Compatible with all Stream V2 client programs like `stream2_buffer_tiff`, `stream2_buffer`, etc. The bifurcator uses PUSH sockets with a timeout, so if receivers can't keep up, sends will timeout (but messages are still buffered locally).

#### Performance Tuning for ConnectX-6/7 NICs

For high-performance Mellanox/NVIDIA ConnectX NICs, the bifurcator supports several optimizations:

```sh
# Optimal settings for ConnectX-6/7 at 100Gbps+
STREAM2_RCVBUF_MB=512 \
STREAM2_SNDBUF_MB=512 \
STREAM2_BUSY_POLL_US=50 \
STREAM2_CPU_AFFINITY=0 \
STREAM2_IO_THREADS=4 \
STREAM2_REALTIME=1 \
./stream2_bifurcator 192.168.1.100 192.168.2.1 31002
```

| Environment Variable | Description | Default |
|---------------------|-------------|---------|
| `STREAM2_RCVBUF_MB` | Receive socket buffer size in MB | 256 |
| `STREAM2_SNDBUF_MB` | Send socket buffer size in MB | 256 |
| `STREAM2_BUSY_POLL_US` | Busy-poll timeout in microseconds (0=disabled) | 0 |
| `STREAM2_CPU_AFFINITY` | Pin process to specific CPU core | disabled |
| `STREAM2_IO_THREADS` | Number of ZMQ I/O threads | 2 |
| `STREAM2_REALTIME` | Enable realtime scheduler priority (requires root) | 0 |

**System tuning for ConnectX NICs:**

```sh
# Increase socket buffer limits (as root)
sysctl -w net.core.rmem_max=536870912
sysctl -w net.core.wmem_max=536870912
sysctl -w net.core.rmem_default=536870912
sysctl -w net.core.wmem_default=536870912

# Set NIC IRQ affinity to match CPU affinity
# Check: cat /proc/interrupts | grep mlx5
# Set: echo <cpu_mask> > /proc/irq/<irq_num>/smp_affinity

# Disable IRQ balancer for dedicated NICs
systemctl stop irqbalance
```

#### Building - Linux

To get started, make sure that the submodules are initialized recursively:

```sh
git submodule update --init --recursive
```

Building the examples requires libzmq. By default, CMake will try to locate and use an installed version of libzmq. To download and build libzmq from source, set the CMake variable `BUILD_LIBZMQ` to `YES`.

Let `documentation/` be the location where this repository is cloned recursively. Build and run the example with:

```sh
mkdir examples_build
cd examples_build
cmake ../documentation/stream_v2/examples -DCMAKE_BUILD_TYPE=Debug -DBUILD_LIBZMQ=YES
cmake --build .
./example
```

#### Building - Windows

```
$vs="C:\Program Files\Microsoft Visual Studio\18\Community"
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch x86 -HostArch x86
cd documentation\stream_v2\examples
cmake . -DCMAKE_BUILD_TYPE=Debug -DBUILD_LIBZMQ=YES
cmake --build .
```

## Python

`client.py` demonstrates how to receive and decode stream V2 data using Python 3. Fields of type `MultiDimArray` and `TypedArray` are represented as `numpy` arrays.

```sh
pip install cbor2 dectris-compression~=0.3.0 numpy pyzmq
python client.py
```

[dectris-compression]: https://github.com/dectris/compression
[tinycbor]: https://github.com/intel/tinycbor
