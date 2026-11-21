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

from usb_transport import UsbDriverError, UsbNotFoundError, open_usb_stream

args = [a for a in sys.argv[1:] if not a.startswith("--")]
NO_AUDIO = "--no-audio" in sys.argv[1:]
USE_USB = "--usb" in sys.argv[1:]

if USE_USB:
    SWITCH_IP = None
elif len(args) > 0:
    SWITCH_IP = args[0]
else:
    raise ValueError("Provide the console's IP address (e.g. 'python ffplay-client.py 192.168.x.x') or pass '--usb'")

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


class AudioPlayer:
    def __init__(self):
        self.muted = False
        self.volume = 100
        self.proc = None
        self.missing_ffplay = False
        self._lock = threading.Lock()
        self._silence = bytes(MAX_AUDIO_PKT)

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

    def _ensure_proc(self):
        with self._lock:
            if self.missing_ffplay:
                return None
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
                self.missing_ffplay = True
                return None

    def feed(self, pcm_view, pkt_size):
        p = self._ensure_proc()
        if p is None:
            return False
        vol = self.volume
        if self.muted or vol <= 0:
            out = self._silence[:pkt_size]
        elif vol >= 100:
            out = pcm_view[:pkt_size]
        else:
            out = self._scale_pcm(pcm_view, pkt_size, vol)
        try:
            p.stdin.write(out)
            p.stdin.flush()
            return True
        except (BrokenPipeError, OSError):
            self.stop()
            return False

    def stop(self):
        with self._lock:
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


class TcpAudioReceiver:
    def __init__(self, ip, port, audio_player):
        self.ip = ip
        self.port = port
        self.audio_player = audio_player
        self.running = True
        self.thread = threading.Thread(target=self._worker, daemon=True)
        self.thread.start()

    def _worker(self):
        hdr_buf = bytearray(HDR_SIZE)
        hdr_view = memoryview(hdr_buf)
        pcm_buf = bytearray(MAX_AUDIO_PKT)
        pcm_view = memoryview(pcm_buf)

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

            try:
                while self.running:
                    recv_into_exact(sock, hdr_view, HDR_SIZE)
                    magic, pkt_size = struct.unpack(HDR_FMT, hdr_buf)
                    if magic != AUDIO_MAGIC or pkt_size == 0 or pkt_size > MAX_AUDIO_PKT:
                        break
                    recv_into_exact(sock, pcm_view, pkt_size)
                    if not self.audio_player.feed(pcm_view, pkt_size):
                        if self.audio_player.missing_ffplay:
                            self.running = False
                        break
            except (ConnectionResetError, ConnectionError, BrokenPipeError, OSError, socket.timeout):
                pass
            finally:
                self.audio_player.stop()
                try:
                    sock.close()
                except Exception:
                    pass

    def stop(self):
        self.running = False
        self.audio_player.stop()


def start_terminal_audio_keys(audio_player, stop_event):
    if audio_player is None or not sys.stdin.isatty():
        return None

    def _print_vol():
        suffix = " (MUTED)" if audio_player.muted else ""
        print(f"\n[Audio] Vol: {audio_player.volume}%{suffix}", flush=True)

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
                        audio_player.adjust_volume(VOLUME_STEP)
                        _print_vol()
                    elif code == "P":  # Down arrow
                        audio_player.adjust_volume(-VOLUME_STEP)
                        _print_vol()
                elif ch in ("m", "M"):
                    audio_player.muted = not audio_player.muted
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
                                    audio_player.adjust_volume(VOLUME_STEP)
                                    _print_vol()
                                elif code == "B":  # Down arrow
                                    audio_player.adjust_volume(-VOLUME_STEP)
                                    _print_vol()
                    elif ch in ("m", "M"):
                        audio_player.muted = not audio_player.muted
                        _print_vol()
            finally:
                termios.tcsetattr(fd, termios.TCSADRAIN, old)

    t = threading.Thread(target=_worker, daemon=True)
    t.start()
    return t


def launch_ffplay(title):
    return subprocess.Popen(
        ["ffplay", "-f", "mjpeg", "-fflags", "nobuffer", "-flags", "low_delay",
         "-framedrop", "-probesize", "32", "-analyzeduration", "0", "-vf", "setpts=0",
         "-window_title", title, "-i", "-"],
        stdin=subprocess.PIPE
    )


def main():
    target_desc = "USB (bottom USB-C, 1209:BEEB)" if USE_USB else f"{SWITCH_IP}:{PORT}"
    audio_desc = "OFF" if NO_AUDIO else ("USB muxed" if USE_USB else f"port {AUDIO_PORT}")
    print("==================================================", flush=True)
    print("     Switch 2 Live Stream Client (FFplay)         ", flush=True)
    print("==================================================", flush=True)
    print(f"Target: {target_desc} (Audio: {audio_desc})")
    print("Controls: Close ffplay window or Ctrl+C to quit | Terminal [Up/Down] Volume +-5% | [M] Mute\n", flush=True)

    ffplay = None
    audio_player = None if NO_AUDIO else AudioPlayer()
    tcp_audio_rx = None if (USE_USB or audio_player is None) else TcpAudioReceiver(SWITCH_IP, AUDIO_PORT, audio_player)
    key_stop = threading.Event()
    start_terminal_audio_keys(audio_player, key_stop)
    running = True
    hdr_buf = bytearray(HDR_SIZE)
    hdr_view = memoryview(hdr_buf)
    frame_buf = bytearray(MAX_FRAME_SIZE)
    frame_view = memoryview(frame_buf)
    pcm_buf = bytearray(MAX_AUDIO_PKT)
    pcm_view = memoryview(pcm_buf)

    try:
        while running and not key_stop.is_set():
            sock = None
            usb_dev = None
            attempt = 0
            last_driver_warn = 0.0

            # Reconnection Loop
            while running and not key_stop.is_set() and sock is None and usb_dev is None:
                attempt += 1
                if USE_USB:
                    try:
                        if attempt == 1:
                            print("Connecting to Switch 2 over USB (bottom USB-C)...", end="", flush=True)
                        else:
                            print(".", end="", flush=True)
                        usb_dev = open_usb_stream(want_video=True, want_audio=not NO_AUDIO, timeout_ms=3000)
                        print(f" Connected! (maxPkt={usb_dev.max_pkt_in}B) [Attempt {attempt}]", flush=True)
                    except UsbDriverError as e:
                        now = time.time()
                        if now - last_driver_warn >= 5.0:
                            print(f"\n[USB Driver] {e}", flush=True)
                            last_driver_warn = now
                        time.sleep(0.5)
                    except (UsbNotFoundError, Exception):
                        time.sleep(0.5)
                else:
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

            if not running or key_stop.is_set() or (sock is None and usb_dev is None):
                break

            if ffplay is None or ffplay.poll() is not None:
                title = "Switch 2 Stream (ffplay USB)" if USE_USB else "Switch 2 Stream (ffplay)"
                ffplay = launch_ffplay(title)

            stat_time = time.time()
            ok_frames = 0
            total_bytes = 0
            last_frame_size = 0

            try:
                while running and not key_stop.is_set() and ffplay.poll() is None:
                    try:
                        if USE_USB:
                            usb_dev.read_into_exact(hdr_buf, HDR_SIZE)
                        else:
                            recv_into_exact(sock, hdr_view, HDR_SIZE)
                    except (ConnectionResetError, ConnectionError, OSError, socket.timeout) as e:
                        print(f"\n[Connection interrupted: {e}. Reconnecting...]", flush=True)
                        break

                    magic, pkt_size = struct.unpack(HDR_FMT, hdr_buf)

                    if magic == AUDIO_MAGIC and USE_USB:
                        if pkt_size == 0 or pkt_size > MAX_AUDIO_PKT:
                            print(f"\n[USB invalid audio size={pkt_size}]", flush=True)
                            break
                        try:
                            usb_dev.read_into_exact(pcm_buf, pkt_size)
                        except (ConnectionResetError, ConnectionError, OSError) as e:
                            print(f"\n[USB interrupted during audio: {e}. Reconnecting...]", flush=True)
                            break
                        if audio_player is not None:
                            audio_player.feed(pcm_view, pkt_size)
                        continue

                    if magic != STREAM_MAGIC or pkt_size == 0 or pkt_size > MAX_FRAME_SIZE:
                        if USE_USB:
                            print(f"\n[USB sync lost: magic=0x{magic:08x}, size={pkt_size}]", flush=True)
                            break
                        continue

                    try:
                        if USE_USB:
                            usb_dev.read_into_exact(frame_buf, pkt_size)
                        else:
                            recv_into_exact(sock, frame_view, pkt_size)
                    except (ConnectionResetError, ConnectionError, OSError, socket.timeout) as e:
                        print(f"\n[Connection interrupted during payload: {e}. Reconnecting...]", flush=True)
                        break

                    try:
                        ffplay.stdin.write(frame_view[:pkt_size])
                        ffplay.stdin.flush()
                        ok_frames += 1
                        total_bytes += pkt_size
                        last_frame_size = pkt_size
                    except (BrokenPipeError, IOError):
                        print("\nffplay window closed by user.", flush=True)
                        running = False
                        break

                    now = time.time()
                    elapsed = now - stat_time
                    if elapsed >= 1.0:
                        fps = ok_frames / elapsed
                        mbps = (total_bytes * 8) / (elapsed * 1_000_000)
                        tag = "USB" if USE_USB else "Wi-Fi"
                        print(f"PC Receiver ({tag}): {fps:.1f} FPS | Frame: {last_frame_size / 1024:.1f} KB | Bitrate: {mbps:.1f} Mbps", flush=True)
                        ok_frames = 0
                        total_bytes = 0
                        stat_time = now
            finally:
                if sock:
                    try:
                        sock.close()
                    except Exception:
                        pass
                if usb_dev:
                    if audio_player is not None:
                        audio_player.stop()
                    try:
                        usb_dev.close(send_stop=not running or key_stop.is_set())
                    except Exception:
                        pass
    except KeyboardInterrupt:
        print("\nInterrupted by user, shutting down...", flush=True)
    finally:
        key_stop.set()
        if tcp_audio_rx is not None:
            tcp_audio_rx.stop()
        if audio_player is not None:
            audio_player.stop()
        if ffplay and ffplay.poll() is None:
            try:
                ffplay.terminate()
            except Exception:
                pass
        print("Stream client closed.", flush=True)


if __name__ == "__main__":
    main()
