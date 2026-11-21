import array
import os
import socket
import struct
import subprocess
import sys
import threading
import time

try:
    import numpy as np
except ImportError:
    np = None

args = [a for a in sys.argv[1:] if not a.startswith("--")]
NO_AUDIO = "--no-audio" in sys.argv[1:]

if len(args) > 0:
    SWITCH_IP = args[0]
else:
    raise ValueError("Please provide the console's IP address (shown in stream-conf.nro)")
PORT = 8080
AUDIO_PORT = 8081
HDR_FMT = "<II"
HDR_SIZE = struct.calcsize(HDR_FMT)
STREAM_MAGIC = 0x5357324A  # "SW2J"
AUDIO_MAGIC = 0x53573241   # "SW2A"
MAX_FRAME_SIZE = 4 * 1024 * 1024
MAX_AUDIO_PKT = 65536
VOLUME_STEP = 5


def recv_into_exact(sock, view, n):
    pos = 0
    while pos < n:
        nbytes = sock.recv_into(view[pos:n], n - pos)
        if nbytes == 0:
            raise ConnectionResetError("Connection closed by Switch")
        pos += nbytes


class AudioReceiver:
    def __init__(self, ip, port):
        self.ip = ip
        self.port = port
        self.running = True
        self.muted = False
        self.volume = 100
        self.proc = None
        self._proc_lock = threading.Lock()
        self.thread = threading.Thread(target=self._worker, daemon=True)
        self.thread.start()

    def adjust_volume(self, delta):
        self.volume = max(0, min(100, self.volume + delta))
        return self.volume

    def _scale_pcm(self, view, pkt_size, vol):
        if np is not None:
            samples = np.frombuffer(view[:pkt_size], dtype=np.int16)
            return ((samples.astype(np.int32) * vol) // 100).astype(np.int16).tobytes()
        samples = array.array("h")
        samples.frombytes(view[:pkt_size])
        for i in range(len(samples)):
            samples[i] = (samples[i] * vol) // 100
        return samples.tobytes()

    def _start_player(self):
        with self._proc_lock:
            if self.proc is not None and self.proc.poll() is None:
                return self.proc
            try:
                self.proc = subprocess.Popen(
                    ["ffplay", "-nodisp", "-loglevel", "error",
                     "-f", "s16le", "-ar", "48000", "-ac", "2",
                     "-fflags", "nobuffer", "-flags", "low_delay",
                     "-probesize", "32", "-analyzeduration", "0", "-i", "-"],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                return self.proc
            except FileNotFoundError:
                print("\n[Audio] ffplay not found in PATH; continuing with video only.", flush=True)
                self.running = False
                return None

    def _stop_player(self):
        with self._proc_lock:
            p = self.proc
            self.proc = None
        if p is not None:
            try:
                if p.stdin:
                    p.stdin.close()
            except Exception:
                pass
            if p.poll() is None:
                try:
                    p.terminate()
                except Exception:
                    pass

    def _worker(self):
        hdr_buf = bytearray(HDR_SIZE)
        hdr_view = memoryview(hdr_buf)
        pcm_buf = bytearray(MAX_AUDIO_PKT)
        pcm_view = memoryview(pcm_buf)
        silence = bytes(MAX_AUDIO_PKT)

        while self.running:
            sock = None
            while self.running and sock is None:
                try:
                    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
                    s.settimeout(2.0)
                    s.connect((self.ip, self.port))
                    s.settimeout(5.0)
                    sock = s
                    print(f"\n[Audio] Connected to {self.ip}:{self.port} (48kHz stereo PCM)", flush=True)
                except Exception:
                    time.sleep(0.5)

            if not self.running or sock is None:
                break

            player = self._start_player()
            if player is None:
                try:
                    sock.close()
                except Exception:
                    pass
                break

            try:
                while self.running and player.poll() is None:
                    recv_into_exact(sock, hdr_view, HDR_SIZE)
                    magic, pkt_size = struct.unpack(HDR_FMT, hdr_buf)
                    if magic != AUDIO_MAGIC or pkt_size == 0 or pkt_size > MAX_AUDIO_PKT:
                        break
                    recv_into_exact(sock, pcm_view, pkt_size)
                    vol = self.volume
                    if self.muted or vol <= 0:
                        out = silence[:pkt_size]
                    elif vol >= 100:
                        out = pcm_view[:pkt_size]
                    else:
                        out = self._scale_pcm(pcm_view, pkt_size, vol)
                    player.stdin.write(out)
                    player.stdin.flush()
            except (ConnectionResetError, ConnectionError, BrokenPipeError, OSError, socket.timeout):
                pass
            finally:
                self._stop_player()
                try:
                    sock.close()
                except Exception:
                    pass

    def stop(self):
        self.running = False
        self._stop_player()


def start_terminal_audio_keys(audio_rx, stop_event):
    if audio_rx is None or not sys.stdin.isatty():
        return None

    def _print_vol():
        suffix = " (MUTED)" if audio_rx.muted else ""
        print(f"\n[Audio] Vol: {audio_rx.volume}%{suffix}", flush=True)

    def _worker():
        if os.name == "nt":
            import msvcrt
            while not stop_event.is_set():
                if not msvcrt.kbhit():
                    time.sleep(0.03)
                    continue
                ch = msvcrt.getwch()
                if ch in ("\x00", "\xe0"):
                    code = msvcrt.getwch()
                    if code == "H":  # Up arrow
                        audio_rx.adjust_volume(VOLUME_STEP)
                        _print_vol()
                    elif code == "P":  # Down arrow
                        audio_rx.adjust_volume(-VOLUME_STEP)
                        _print_vol()
                elif ch in ("m", "M"):
                    audio_rx.muted = not audio_rx.muted
                    _print_vol()
                elif ch == "\x03":  # Ctrl+C
                    stop_event.set()
                    break
        else:
            import select
            import termios
            import tty
            fd = sys.stdin.fileno()
            old = termios.tcgetattr(fd)
            try:
                tty.setcbreak(fd)
                while not stop_event.is_set():
                    r, _, _ = select.select([sys.stdin], [], [], 0.05)
                    if not r:
                        continue
                    ch = sys.stdin.read(1)
                    if ch == "\x1b":
                        r2, _, _ = select.select([sys.stdin], [], [], 0.02)
                        if r2 and sys.stdin.read(1) == "[":
                            r3, _, _ = select.select([sys.stdin], [], [], 0.02)
                            if r3:
                                code = sys.stdin.read(1)
                                if code == "A":  # Up arrow
                                    audio_rx.adjust_volume(VOLUME_STEP)
                                    _print_vol()
                                elif code == "B":  # Down arrow
                                    audio_rx.adjust_volume(-VOLUME_STEP)
                                    _print_vol()
                    elif ch in ("m", "M"):
                        audio_rx.muted = not audio_rx.muted
                        _print_vol()
            finally:
                termios.tcsetattr(fd, termios.TCSADRAIN, old)

    t = threading.Thread(target=_worker, daemon=True)
    t.start()
    return t


def main():
    print("==================================================", flush=True)
    print("     Switch 2 Live Stream Client (FFplay)         ", flush=True)
    print("==================================================", flush=True)
    print(f"Target Switch IP: {SWITCH_IP}:{PORT} (Audio: {'OFF' if NO_AUDIO else f'port {AUDIO_PORT}'})")
    print("Controls: Close ffplay window or Ctrl+C to quit | Terminal [Up/Down] Volume +-5% | [M] Mute\n", flush=True)

    ffplay = None
    audio_rx = None if NO_AUDIO else AudioReceiver(SWITCH_IP, AUDIO_PORT)
    key_stop = threading.Event()
    start_terminal_audio_keys(audio_rx, key_stop)
    running = True
    hdr_buf = bytearray(HDR_SIZE)
    hdr_view = memoryview(hdr_buf)
    frame_buf = bytearray(MAX_FRAME_SIZE)
    frame_view = memoryview(frame_buf)

    try:
        while running and not key_stop.is_set():
            sock = None
            attempt = 0

            # Reconnection Loop
            while running and not key_stop.is_set() and sock is None:
                attempt += 1
                try:
                    if attempt == 1:
                        print(f"Connecting to Switch 2 ({SWITCH_IP}:{PORT})...", end="", flush=True)
                    else:
                        print(".", end="", flush=True)

                    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
                    s.settimeout(2.0)
                    s.connect((SWITCH_IP, PORT))
                    s.settimeout(10.0)
                    sock = s
                    print(f" Connected! [Attempt {attempt}]", flush=True)
                except Exception:
                    time.sleep(0.5)

            if not running or sock is None:
                break

            # Launch ffplay if not already running
            if ffplay is None or ffplay.poll() is not None:
                ffplay = subprocess.Popen(
                    ["ffplay", "-f", "mjpeg", "-fflags", "nobuffer", "-flags", "low_delay",
                     "-framedrop", "-probesize", "32", "-analyzeduration", "0", "-vf", "setpts=0",
                     "-window_title", "Switch 2 Stream (ffplay)", "-i", "-"],
                    stdin=subprocess.PIPE
                )

            stat_time = time.time()
            ok_frames = 0
            total_bytes = 0

            try:
                while running and not key_stop.is_set() and ffplay.poll() is None:
                    try:
                        recv_into_exact(sock, hdr_view, HDR_SIZE)
                    except (ConnectionResetError, ConnectionError, OSError, socket.timeout) as e:
                        print(f"\n[Connection interrupted: {e}. Reconnecting...]", flush=True)
                        break

                    magic, frame_size = struct.unpack(HDR_FMT, hdr_buf)
                    if magic != STREAM_MAGIC or frame_size == 0 or frame_size > MAX_FRAME_SIZE:
                        continue

                    try:
                        recv_into_exact(sock, frame_view, frame_size)
                    except (ConnectionResetError, ConnectionError, OSError, socket.timeout) as e:
                        print(f"\n[Connection interrupted during payload: {e}. Reconnecting...]", flush=True)
                        break

                    try:
                        ffplay.stdin.write(frame_view[:frame_size])
                        ffplay.stdin.flush()
                        ok_frames += 1
                        total_bytes += frame_size
                    except (BrokenPipeError, IOError):
                        print("\nffplay window closed by user.", flush=True)
                        running = False
                        break

                    now = time.time()
                    elapsed = now - stat_time
                    if elapsed >= 1.0:
                        fps = ok_frames / elapsed
                        mbps = (total_bytes * 8) / (elapsed * 1_000_000)
                        print(f"PC Receiver: {fps:.1f} FPS | Frame: {frame_size / 1024:.1f} KB | Bitrate: {mbps:.1f} Mbps", flush=True)
                        ok_frames = 0
                        total_bytes = 0
                        stat_time = now
            finally:
                if sock:
                    try:
                        sock.close()
                    except Exception:
                        pass
    except KeyboardInterrupt:
        print("\nInterrupted by user, shutting down...", flush=True)
    finally:
        key_stop.set()
        if audio_rx is not None:
            audio_rx.stop()
        if ffplay and ffplay.poll() is None:
            try:
                ffplay.terminate()
            except Exception:
                pass
        print("Stream client closed.", flush=True)


if __name__ == "__main__":
    main()
