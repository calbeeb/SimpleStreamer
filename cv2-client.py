import socket
import struct
import sys
import threading
import time
import cv2
import numpy as np

if len(sys.argv) > 1:
    SWITCH_IP = sys.argv[1]
else:
    raise ValueError("Please provide the console's IP address (shown in stream-conf.nro)")
PORT = 8080
HDR_FMT = "<II"
HDR_SIZE = struct.calcsize(HDR_FMT)
STREAM_MAGIC = 0x5357324A  # "SW2J"
MAX_FRAME_SIZE = 4 * 1024 * 1024  # 4 MB


def recv_into_exact(sock, view, n):
    pos = 0
    while pos < n:
        nbytes = sock.recv_into(view[pos:n], n - pos)
        if nbytes == 0:
            raise ConnectionResetError("Connection closed by Switch")
        pos += nbytes


class StreamReceiver:
    def __init__(self, ip, port):
        self.ip = ip
        self.port = port
        self.running = True
        self.connected = False
        self.attempt = 0

        self.lock = threading.Lock()
        self.frame_event = threading.Event()

        # Pre-allocated triple buffers to avoid 2 MB allocations every frame
        self.bufs = [bytearray(MAX_FRAME_SIZE) for _ in range(3)]
        self.views = [memoryview(b) for b in self.bufs]
        self.write_idx = 0
        self.ready_idx = 1
        self.read_idx = 2
        self.ready_size = 0
        self.frame_seq = 0

        self.net_frames = 0
        self.net_bytes = 0
        self.last_frame_size = 0

        self.thread = threading.Thread(target=self._worker, daemon=True)
        self.thread.start()

    def _worker(self):
        hdr_buf = bytearray(HDR_SIZE)
        hdr_view = memoryview(hdr_buf)

        while self.running:
            sock = None
            self.attempt = 0
            while self.running and sock is None:
                self.attempt += 1
                try:
                    if self.attempt == 1:
                        print(f"Connecting to Switch 2 ({self.ip}:{self.port})...", end="", flush=True)
                    else:
                        print(".", end="", flush=True)

                    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
                    s.settimeout(2.0)
                    s.connect((self.ip, self.port))
                    s.settimeout(10.0)
                    sock = s
                    self.connected = True
                    print(f" Connected! [Attempt {self.attempt}]", flush=True)
                except Exception:
                    time.sleep(0.4)

            if not self.running or sock is None:
                break

            try:
                while self.running:
                    recv_into_exact(sock, hdr_view, HDR_SIZE)
                    magic, frame_size = struct.unpack(HDR_FMT, hdr_buf)
                    if magic != STREAM_MAGIC or frame_size == 0 or frame_size > MAX_FRAME_SIZE:
                        print(f"\n[Invalid header: magic=0x{magic:08x}, size={frame_size}]", flush=True)
                        break

                    w_idx = self.write_idx
                    recv_into_exact(sock, self.views[w_idx], frame_size)

                    with self.lock:
                        self.write_idx, self.ready_idx = self.ready_idx, w_idx
                        self.ready_size = frame_size
                        self.frame_seq += 1
                        self.net_frames += 1
                        self.net_bytes += frame_size
                        self.last_frame_size = frame_size
                    self.frame_event.set()
            except (ConnectionResetError, ConnectionError, OSError, socket.timeout) as e:
                if self.running:
                    print(f"\n[Connection interrupted: {e}. Reconnecting...]", flush=True)
            finally:
                self.connected = False
                self.frame_event.set()
                try:
                    sock.close()
                except Exception:
                    pass

    def get_latest_frame(self, last_seq):
        with self.lock:
            if self.frame_seq == last_seq or self.ready_size == 0:
                return None, 0, last_seq
            self.read_idx, self.ready_idx = self.ready_idx, self.read_idx
            size = self.ready_size
            seq = self.frame_seq
            buf = self.views[self.read_idx][:size]
        return buf, size, seq

    def pop_stats(self):
        with self.lock:
            nf = self.net_frames
            nb = self.net_bytes
            lfs = self.last_frame_size
            self.net_frames = 0
            self.net_bytes = 0
        return nf, nb, lfs

    def stop(self):
        self.running = False
        self.frame_event.set()


def main():
    print("==================================================", flush=True)
    print("   Switch 2 Async Live Stream Client (Multi-Thr)  ", flush=True)
    print("==================================================", flush=True)
    print(f"Target Switch IP: {SWITCH_IP}:{PORT}")
    print("Controls: [ESC / Q] Quit | [H] Toggle Fast Half-Scale Decode\n", flush=True)

    win_name = "Switch 2 Stream"
    cv2.namedWindow(win_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(win_name, 1280, 720)

    receiver = StreamReceiver(SWITCH_IP, PORT)
    last_img = None
    last_seq = 0
    half_scale = False

    stat_time = time.time()
    disp_frames = 0

    try:
        while receiver.running:
            receiver.frame_event.wait(timeout=0.05)
            receiver.frame_event.clear()

            if not receiver.connected:
                if last_img is not None:
                    display = last_img.copy()
                    h, w = display.shape[:2]
                    overlay = display.copy()
                    cv2.rectangle(overlay, (0, 0), (w, 60), (20, 20, 20), -1)
                    cv2.addWeighted(overlay, 0.75, display, 0.25, 0, display)
                    cv2.putText(display, f"Connecting to Switch 2 ({SWITCH_IP}:{PORT})...", (20, 40),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.85, (0, 215, 255), 2, cv2.LINE_AA)
                    cv2.imshow(win_name, display)
                else:
                    blank = np.zeros((720, 1280, 3), dtype=np.uint8)
                    cv2.putText(blank, f"Waiting for Switch 2 at {SWITCH_IP}:{PORT}...", (160, 360),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.95, (220, 220, 220), 2, cv2.LINE_AA)
                    cv2.imshow(win_name, blank)

                key = cv2.waitKey(100) & 0xFF
                if key in (27, ord('q'), ord('Q')):
                    break
                continue

            jpeg_view, frame_size, new_seq = receiver.get_latest_frame(last_seq)
            if jpeg_view is not None:
                last_seq = new_seq
                arr = np.frombuffer(jpeg_view, dtype=np.uint8)
                flag = cv2.IMREAD_REDUCED_COLOR_2 if half_scale else cv2.IMREAD_COLOR
                img = cv2.imdecode(arr, flag)
                if img is not None:
                    last_img = img
                    cv2.imshow(win_name, img)
                    disp_frames += 1

            now = time.time()
            elapsed = now - stat_time
            if elapsed >= 1.0:
                net_f, net_b, last_fs = receiver.pop_stats()
                net_fps = net_f / elapsed
                disp_fps = disp_frames / elapsed
                mbps = (net_b * 8) / (elapsed * 1_000_000)
                res_str = f"{last_img.shape[1]}x{last_img.shape[0]}" if last_img is not None else ""
                title = f"Switch 2 Stream - {res_str} @ Net {net_fps:.1f} / Disp {disp_fps:.1f} FPS ({last_fs / 1024:.0f} KB, {mbps:.1f} Mbps)"
                cv2.setWindowTitle(win_name, title)
                print(f"PC Receiver: Net {net_fps:.1f} FPS | Disp {disp_fps:.1f} FPS | Frame: {last_fs / 1024:.1f} KB | Bitrate: {mbps:.1f} Mbps | Res: {res_str}", flush=True)
                disp_frames = 0
                stat_time = now

            key = cv2.waitKey(1) & 0xFF
            if key in (27, ord('q'), ord('Q')):
                print("\nQuit key pressed, closing stream.", flush=True)
                break
            elif key in (ord('h'), ord('H')):
                half_scale = not half_scale
                print(f"\n[Fast Half-Scale Decode: {'ON (960x540)' if half_scale else 'OFF (1920x1080)'}]", flush=True)
    finally:
        receiver.stop()
        cv2.destroyAllWindows()
        print("Stream viewer closed.", flush=True)


if __name__ == "__main__":
    main()
