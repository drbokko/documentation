import time
import threading
import signal

# --- Detector control imports ---
from DEigerClient import DEigerClient

# --- Stream receiver imports ---
import cbor2
from dectris.compression import decompress
import numpy as np

############################################################
# Configuration (no saving; live view only)
############################################################
configuration = {
    # Detector IP and Initialization
    "DCU_IP": '172.31.1.1',
    "force_initialization": False,

    # Data acquisition parameters
    "thresholds": [15000],  # Energy thresholds [eV], 1..4 values supported
    "number_of_images": 1,       # Number of images to capture
    "number_of_triggers": 1000,       # Number of trigger to send
    "exposure_time": 1/500.0,          # Exposure time per image [s]
    "sleep_time": 0.2,             # Delay between frames [s]

    # Live viewer performance knobs
    "live_every": 1,               # Show every Nth frame in live view
    "stats_subsample": 8,          # Subsample factor for median/contrast
    "max_fps": 20.0,               # Cap redraw rate (frames/sec)
}



############################################################
# CBOR/DECTRIS helpers
############################################################

def decode_multi_dim_array(tag, column_major):
    dimensions, contents = tag.value
    if isinstance(contents, list):
        array = np.empty((len(contents),), dtype=object)
        array[:] = contents
    elif isinstance(contents, (np.ndarray, np.generic)):
        array = contents
    else:
        raise cbor2.CBORDecodeValueError("expected array or typed array")
    return array.reshape(dimensions, order="F" if column_major else "C")


def decode_typed_array(tag, dtype):
    if not isinstance(tag.value, bytes):
        raise cbor2.CBORDecodeValueError("expected byte string in typed array")
    return np.frombuffer(tag.value, dtype=dtype)


def decode_dectris_compression(tag):
    algorithm, elem_size, encoded = tag.value
    return decompress(encoded, algorithm, elem_size=elem_size)


tag_decoders = {
    40: lambda tag: decode_multi_dim_array(tag, column_major=False),
    64: lambda tag: decode_typed_array(tag, dtype="u1"),
    65: lambda tag: decode_typed_array(tag, dtype=">u2"),
    66: lambda tag: decode_typed_array(tag, dtype=">u4"),
    67: lambda tag: decode_typed_array(tag, dtype=">u8"),
    68: lambda tag: decode_typed_array(tag, dtype="u1"),
    69: lambda tag: decode_typed_array(tag, dtype="<u2"),
    70: lambda tag: decode_typed_array(tag, dtype="<u4"),
    71: lambda tag: decode_typed_array(tag, dtype="<u8"),
    72: lambda tag: decode_typed_array(tag, dtype="i1"),
    73: lambda tag: decode_typed_array(tag, dtype=">i2"),
    74: lambda tag: decode_typed_array(tag, dtype=">i4"),
    75: lambda tag: decode_typed_array(tag, dtype=">i8"),
    77: lambda tag: decode_typed_array(tag, dtype="<i2"),
    78: lambda tag: decode_typed_array(tag, dtype="<i4"),
    79: lambda tag: decode_typed_array(tag, dtype="<i8"),
    80: lambda tag: decode_typed_array(tag, dtype=">f2"),
    81: lambda tag: decode_typed_array(tag, dtype=">f4"),
    82: lambda tag: decode_typed_array(tag, dtype=">f8"),
    83: lambda tag: decode_typed_array(tag, dtype=">f16"),
    84: lambda tag: decode_typed_array(tag, dtype="<f2"),
    85: lambda tag: decode_typed_array(tag, dtype="<f4"),
    86: lambda tag: decode_typed_array(tag, dtype="<f8"),
    87: lambda tag: decode_typed_array(tag, dtype="<f16"),
    1040: lambda tag: decode_multi_dim_array(tag, column_major=True),
    56500: lambda tag: decode_dectris_compression(tag),
}


def tag_hook(decoder, tag):
    tag_decoder = tag_decoders.get(tag.tag)
    return tag_decoder(tag) if tag_decoder else tag


############################################################
# Stream Receiver (thread) — live view only, first threshold only
############################################################

class StreamReceiver(threading.Thread):
    def __init__(self, ip: str, live_every: int = 5, stats_subsample: int = 8, max_fps: float = 20.0):
        super().__init__(daemon=True)
        self.ip = ip
        self.endpoint = f"tcp://{ip}:31001"
        self.live_every = max(1, int(live_every))
        self.stats_subsample = max(1, int(stats_subsample))
        self.max_dt = 1.0 / max(1.0, float(max_fps))
        self.stop_event = threading.Event()
        self.finished_event = threading.Event()

        # runtime fields
        self.image_num = 0
        self.count_time = None
        self.thresholds = []
        self.channel = 'threshold_1'  # plot only first threshold

        # live UI cache
        self._fig = None
        self._ax = None
        self._im = None
        self._last_draw = 0.0

    def _setup_live(self):
        import matplotlib.pyplot as plt  # lazy import
        plt.ion()
        fig, ax = plt.subplots(1, 1, figsize=(12, 5))
        ax.set_title("Threshold 1")
        ax.set_xticks([]); ax.set_yticks([])
        # placeholder image; real shape set on first frame
        im = ax.imshow(np.zeros((10, 10), dtype=np.uint16), cmap='gray', vmin=0, vmax=1, animated=False)
        self._fig, self._ax, self._im = fig, ax, im

    def stop(self):
        self.stop_event.set()

    def run(self):
        import zmq

        context = zmq.Context()
        socket = context.socket(zmq.PULL)
        socket.setsockopt(zmq.RCVTIMEO, 1000)   # check stop_event regularly
        socket.setsockopt(zmq.RCVHWM, 10)       # limit backlog so UI stays responsive

        try:
            socket.connect(self.endpoint)
            print(f"[STREAM] Connected PULL {self.endpoint}")
            self._setup_live()

            start_time = None

            while not self.stop_event.is_set():
                try:
                    raw = socket.recv()
                except zmq.Again:
                    continue  # timeout: loop and check stop_event

                message = cbor2.loads(raw, tag_hook=tag_hook)
                mtype = message.get('type')

                if mtype == 'start':
                    start_time = time.time()
                    print(f"[STREAM] start")
                    self.image_num = 0
                    self.count_time = message.get('count_time')
                    th = message.get('threshold_energy', {})
                    self.thresholds = [th.get('threshold_1')]

                elif mtype == 'image':
                    self.image_num += 1
                    img_id = int(message.get('image_id', self.image_num))
                    print(f"[STREAM] {img_id} image received")
                    draw_this = (img_id % self.live_every == 0)

                    # Select first threshold only
                    data = message['data'].get(self.channel)
                    if data is None:
                        # fallback: pick the first available channel if not present
                        first_key = next(iter(message['data']))
                        data = message['data'][first_key]
                    data = np.asarray(data)

                    if self._im and draw_this:
                        # Update artist in-place (fast path)
                        ss = self.stats_subsample
                        sample = data[::ss, ::ss]
                        median_counts = float(np.median(sample))

                        if self._im.get_array().shape != data.shape:
                            self._im.set_data(np.zeros_like(data))
                        self._im.set_data(data)

                        # contrast window around median; guard against zero span
                        lo = median_counts * 0.8
                        hi = max(lo + 1.0, median_counts * 1.2)
                        self._im.set_clim(lo, hi)

                        now = time.time()
                        if now - self._last_draw >= self.max_dt:
                            # Update title with current image index
                            if self._ax:
                                self._ax.set_title(f"Image {img_id} - {median_counts/1e3:.1f} kcps (th={self.thresholds} eV)")
                            self._fig.canvas.draw_idle()
                            self._fig.canvas.flush_events()
                            self._last_draw = now

                elif mtype == 'end':
                    elapsed = (time.time() - start_time) if start_time else 0
                    print(f"[STREAM] {self.image_num} images in {elapsed:.1f}s (live view only)")
                    break

        except Exception as e:
            print(f"[STREAM] Error: {e}")
        finally:
            try:
                socket.close(0)
            except Exception:
                pass
            context.term()
            self.finished_event.set()
            print("[STREAM] Receiver stopped")


############################################################
# Main
############################################################

if __name__ == '__main__':
    # Connect to Detector
    c = DEigerClient.DEigerClient(host=configuration["DCU_IP"])
    print(f"Detector status:\t\t\t{c.detectorStatus('state')['value']}")

    if configuration["force_initialization"] or c.detectorStatus('state')['value'] != 'idle':
        print("Initializing detector...")
        c.sendDetectorCommand("initialize")

    print(f"Detector high_voltage status:\t\t{c.detectorStatus('high_voltage/state')['value']}")
    print(f"Detector temperature:\t\t\t{c.detectorStatus('temperature')['value']:.1f} deg")
    print(f"Detector humidity:\t\t\t{c.detectorStatus('humidity')['value']:.1f} %")

    # Configure Detector
    c.setDetectorConfig("countrate_correction_applied", False)
    c.setDetectorConfig('retrigger', False)
    c.setDetectorConfig('counting_mode', 'normal')
    c.setDetectorConfig("flatfield_correction_applied", False)
    c.setDetectorConfig('virtual_pixel_correction_applied', True)
    c.setDetectorConfig('mask_to_zero', True)  # pixels marked in the pixel_mask will be set to zero
    c.setDetectorConfig("test_image_mode", "")
    # c.setDetectorConfig("test_image_value", 100)

    c.setDetectorConfig("photon_energy", 10000)  # example value for monochromatic

    # only one threshold (device has 2 thresholds total; we enable only the first and disable the second)
    c.setDetectorConfig("threshold/1/mode", 'enabled')
    c.setDetectorConfig("threshold/1/energy", configuration["thresholds"][0])
    c.setDetectorConfig("threshold/2/mode", 'disabled')

    # Set Acquisition Parameters
    c.setDetectorConfig("count_time", configuration["exposure_time"]) 
    c.setDetectorConfig('frame_time', configuration["exposure_time"] + configuration["sleep_time"]) 
    c.setDetectorConfig("nimages", configuration["number_of_images"]) 
    c.setDetectorConfig("ntrigger", configuration["number_of_triggers"])  
    c.setDetectorConfig('auto_summation', False)

    print(f'count_time= {c.detectorConfig("count_time")["value"]}')
    print(f'frame_time= {c.detectorConfig("frame_time")["value"]}')

    # Configure Interfaces (saving disabled)
    c.setMonitorConfig('mode', 'disabled')  # monitor explicitly OFF  
    c.setFileWriterConfig('mode', 'disabled')              # ensure filewriter is OFF
    c.setStreamConfig('mode', 'enabled') 
    c.setStreamConfig('format', 'cbor') 
    c.setStreamConfig('header_detail', 'all') 

    # Start Stream Receiver (live view only)
    stream_thread = StreamReceiver(
        ip=configuration["DCU_IP"],
        live_every=configuration["live_every"],
        stats_subsample=configuration["stats_subsample"],
        max_fps=configuration["max_fps"],
    )
    stream_thread.start()

    # Graceful shutdown on CTRL-C
    def handle_sigint(sig, frame):
        print("\n[MAIN] Caught interrupt. Stopping...")
        if stream_thread:
            stream_thread.stop()
        raise SystemExit

    signal.signal(signal.SIGINT, handle_sigint)

    # Run Acquisition
    try:
        print("Acquiring data...")
        print("Arming")
        c.sendDetectorCommand('arm')
        for trigger in range(configuration['number_of_triggers']):
            print(f'trigger #{trigger}')
            c.sendDetectorCommand('trigger')
        print("disarming")
        c.sendDetectorCommand('disarm')  # writes data internally and disarms; filewriter is disabled

    finally:
        # Stop stream thread
        if stream_thread:
            print("Stopping stream receiver...")
            stream_thread.stop()
            stream_thread.finished_event.wait(timeout=5.0)
            print("disarming")
            c.sendDetectorCommand('disarm')  # writes data internally and disarms; filewriter is disabled

    print("All done.")
