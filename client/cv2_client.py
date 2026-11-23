#!/usr/bin/env python3
"""
CLI usage:
    python cv2_client.py <SWITCH_IP> [--no-audio]
    python cv2_client.py --usb [--no-audio]
"""

import socket
import struct
import subprocess
import sys
import threading
import time
import cv2
import numpy as np

from usb_transport import UsbDriverError, UsbNotFoundError, open_usb_stream

DEFAULT_VIDEO_PORT = 8080
DEFAULT_AUDIO_PORT = 8081
HDR_FMT = "<II"
HDR_SIZE = struct.calcsize(HDR_FMT)
STREAM_MAGIC = 0x5357324A  # "SW2J"
AUDIO_MAGIC = 0x53573241   # "SW2A"
MAX_FRAME_SIZE = 4 * 1024 * 1024  # 4 MB
MAX_AUDIO_PKT = 65536
VOLUME_STEP = 5

# cv2.waitKeyEx() codes for Up/Down arrows across Windows (Win32), Linux (X11/Qt), macOS
KEY_UP_CODES = (2490368, 65362, 63232, 0x1000013)
KEY_DOWN_CODES = (2621440, 65364, 63233, 0x1000015)


def _is_usb_host(host) -> bool:
    return isinstance(host, str) and host.strip().upper() == "USB"


def _clamp_volume(volume: int) -> int:
    return max(0, min(100, int(volume)))


def recv_into_exact(sock, view, n: int) -> None:
    pos = 0
    while pos < n:
        nbytes = sock.recv_into(view[pos:n], n - pos)
        if nbytes == 0:
            raise ConnectionResetError("Connection closed by Switch")
        pos += nbytes


class AudioPlayer:
    def __init__(self, volume: int = 100, muted: bool = False, verbose: bool = True):
        self.muted = bool(muted)
        self.volume = _clamp_volume(volume)
        self.verbose = verbose
        self.proc = None
        self.missing_ffplay = False
        self._lock = threading.Lock()
        self._silence = bytes(MAX_AUDIO_PKT)

    def set_volume(self, volume: int) -> int:
        self.volume = _clamp_volume(volume)
        return self.volume

    def adjust_volume(self, delta: int) -> int:
        self.volume = _clamp_volume(self.volume + delta)
        return self.volume

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
                if self.verbose:
                    print("\n[Audio] ffplay not found in PATH; continuing with video only.", flush=True)
                self.missing_ffplay = True
                return None

    def feed(self, pcm_view, pkt_size: int) -> bool:
        p = self._ensure_proc()
        if p is None:
            return False
        vol = self.volume
        if self.muted or vol <= 0:
            out = self._silence[:pkt_size]
        elif vol >= 100:
            out = pcm_view[:pkt_size]
        else:
            samples = np.frombuffer(pcm_view[:pkt_size], dtype=np.int16)
            out = ((samples.astype(np.int32) * vol) // 100).astype(np.int16).tobytes()
        try:
            p.stdin.write(out) # type: ignore
            p.stdin.flush() # type: ignore
            return True
        except (BrokenPipeError, OSError):
            self.stop()
            return False

    def stop(self) -> None:
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


class _TcpAudioReceiver:
    def __init__(self, ip: str, port: int, audio_player: AudioPlayer, verbose: bool = True):
        self.ip = ip
        self.port = port
        self.audio_player = audio_player
        self.verbose = verbose
        self.running = True
        self.connected = False
        self._sock_lock = threading.Lock()
        self._sock = None
        self.thread = threading.Thread(target=self._worker, daemon=True)
        self.thread.start()

    def _worker(self) -> None:
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
                    with self._sock_lock:
                        self._sock = s
                    self.connected = True
                    if self.verbose:
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
                self.connected = False
                self.audio_player.stop()
                with self._sock_lock:
                    s = self._sock
                    self._sock = None
                if s is not None:
                    try:
                        s.close()
                    except Exception:
                        pass

    def stop(self) -> None:
        self.running = False
        with self._sock_lock:
            s = self._sock
            self._sock = None
        if s is not None:
            try:
                s.close()
            except Exception:
                pass
        self.audio_player.stop()


class _BaseVideoReceiver:
    def __init__(self, verbose: bool = True):
        self.verbose = verbose
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

    def _publish_frame(self, w_idx: int, frame_size: int) -> None:
        with self.lock:
            self.write_idx, self.ready_idx = self.ready_idx, w_idx
            self.ready_size = frame_size
            self.frame_seq += 1
            self.net_frames += 1
            self.net_bytes += frame_size
            self.last_frame_size = frame_size
        self.frame_event.set()

    def get_latest_frame(self, last_seq: int):
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

    def stop(self) -> None:
        self.running = False
        self.frame_event.set()


class _TcpVideoReceiver(_BaseVideoReceiver):
    def __init__(self, ip: str, port: int, verbose: bool = True):
        super().__init__(verbose=verbose)
        self.ip = ip
        self.port = port
        self._sock_lock = threading.Lock()
        self._sock = None
        self.thread = threading.Thread(target=self._worker, daemon=True)
        self.thread.start()

    def _worker(self) -> None:
        hdr_buf = bytearray(HDR_SIZE)
        hdr_view = memoryview(hdr_buf)

        while self.running:
            sock = None
            self.attempt = 0
            while self.running and sock is None:
                self.attempt += 1
                try:
                    if self.verbose:
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
                    with self._sock_lock:
                        self._sock = s
                    self.connected = True
                    if self.verbose:
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
                        if self.verbose:
                            print(f"\n[Invalid header: magic=0x{magic:08x}, size={frame_size}]", flush=True)
                        break

                    w_idx = self.write_idx
                    recv_into_exact(sock, self.views[w_idx], frame_size)
                    self._publish_frame(w_idx, frame_size)
            except (ConnectionResetError, ConnectionError, OSError, socket.timeout) as e:
                if self.running and self.verbose:
                    print(f"\n[Connection interrupted: {e}. Reconnecting...]", flush=True)
            finally:
                self.connected = False
                self.frame_event.set()
                with self._sock_lock:
                    s = self._sock
                    self._sock = None
                if s is not None:
                    try:
                        s.close()
                    except Exception:
                        pass

    def stop(self) -> None:
        super().stop()
        with self._sock_lock:
            s = self._sock
            self._sock = None
        if s is not None:
            try:
                s.close()
            except Exception:
                pass


class _UsbStreamReceiver(_BaseVideoReceiver):
    def __init__(self, audio_player, verbose: bool = True):
        super().__init__(verbose=verbose)
        self.audio_player = audio_player
        self._dev_lock = threading.Lock()
        self._dev = None
        self.thread = threading.Thread(target=self._worker, daemon=True)
        self.thread.start()

    def _worker(self) -> None:
        hdr_buf = bytearray(HDR_SIZE)
        pcm_buf = bytearray(MAX_AUDIO_PKT)
        pcm_view = memoryview(pcm_buf)
        want_audio = self.audio_player is not None

        while self.running:
            dev = None
            self.attempt = 0
            last_driver_warn = 0.0
            while self.running and dev is None:
                self.attempt += 1
                try:
                    if self.verbose:
                        if self.attempt == 1:
                            print("Connecting to Switch 2 over USB (bottom USB-C)...", end="", flush=True)
                        else:
                            print(".", end="", flush=True)
                    dev = open_usb_stream(want_video=True, want_audio=want_audio, timeout_ms=3000)
                    with self._dev_lock:
                        self._dev = dev
                    self.connected = True
                    if self.verbose:
                        print(f" Connected! (maxPkt={dev.max_pkt_in}B) [Attempt {self.attempt}]", flush=True)
                except UsbDriverError as e:
                    now = time.time()
                    if self.verbose and now - last_driver_warn >= 5.0:
                        print(f"\n[USB Driver] {e}", flush=True)
                        last_driver_warn = now
                    time.sleep(0.5)
                except (UsbNotFoundError, Exception):
                    time.sleep(0.5)

            if not self.running or dev is None:
                break

            try:
                while self.running:
                    dev.read_into_exact(hdr_buf, HDR_SIZE)
                    magic, payload_size = struct.unpack(HDR_FMT, hdr_buf)

                    if magic == STREAM_MAGIC:
                        if payload_size == 0 or payload_size > MAX_FRAME_SIZE:
                            if self.verbose:
                                print(f"\n[USB invalid video size={payload_size}]", flush=True)
                            break
                        w_idx = self.write_idx
                        dev.read_into_exact(self.bufs[w_idx], payload_size)
                        self._publish_frame(w_idx, payload_size)
                    elif magic == AUDIO_MAGIC:
                        if payload_size == 0 or payload_size > MAX_AUDIO_PKT:
                            if self.verbose:
                                print(f"\n[USB invalid audio size={payload_size}]", flush=True)
                            break
                        dev.read_into_exact(pcm_buf, payload_size)
                        if self.audio_player is not None:
                            self.audio_player.feed(pcm_view, payload_size)
                    else:
                        if self.verbose:
                            print(f"\n[USB sync lost: magic=0x{magic:08x}, size={payload_size}]", flush=True)
                        break
            except (ConnectionResetError, ConnectionError, OSError) as e:
                if self.running and self.verbose:
                    print(f"\n[USB interrupted: {e}. Reconnecting...]", flush=True)
            finally:
                self.connected = False
                self.frame_event.set()
                if self.audio_player is not None:
                    self.audio_player.stop()
                with self._dev_lock:
                    d = self._dev
                    self._dev = None
                if d is not None:
                    try:
                        d.close(send_stop=not self.running)
                    except Exception:
                        pass

    def stop(self) -> None:
        super().stop()
        with self._dev_lock:
            d = self._dev
            self._dev = None
        if d is not None:
            try:
                d.close(send_stop=True)
            except Exception:
                pass


class Cv2Player:
    """OpenCV-based Switch 2 live video + audio player."""

    def __init__(
        self,
        host: str,
        audio: bool = True,
        volume: int = 100,
        muted: bool = False,
        half_scale: bool = False,
        port: int = DEFAULT_VIDEO_PORT,
        audio_port: int = DEFAULT_AUDIO_PORT,
        window_name: str = "Switch 2 Stream",
        window_size=(1920, 1080),
        verbose: bool = True,
    ):
        if not host or not isinstance(host, str):
            raise ValueError("host must be an IP address string or 'USB'")
        self.host = host.strip()
        self.use_usb = _is_usb_host(self.host)
        self.port = int(port)
        self.audio_port = int(audio_port)
        self.audio_enabled = bool(audio)
        self.half_scale = bool(half_scale)
        self.window_name = window_name
        self.window_size = tuple(window_size)
        self.verbose = bool(verbose)

        self._audio_player = (
            AudioPlayer(volume=_clamp_volume(volume), muted=bool(muted), verbose=self.verbose)
            if self.audio_enabled
            else None
        )

        self._stop_event = threading.Event()
        self._title_dirty = threading.Event()
        self._thread = None
        self._running = False
        self._receiver = None
        self._tcp_audio_rx = None
        self._state_lock = threading.Lock()
        self._stats = {
            "rx_fps": 0.0,
            "disp_fps": 0.0,
            "bitrate_mbps": 0.0,
            "frame_kb": 0.0,
            "resolution": "",
            "connected": False,
        }

    # --- Audio & Display Controls ---

    def _notify_title_change(self) -> None:
        self._title_dirty.set()
        rx = self._receiver
        if rx is not None:
            rx.frame_event.set()

    def mute(self, state: bool = True) -> bool:
        """Mute audio (pass False to unmute). Returns current mute state."""
        if self._audio_player is None:
            return True
        self._audio_player.muted = bool(state)
        self._notify_title_change()
        return self._audio_player.muted

    def unmute(self) -> bool:
        """Unmute audio. Returns current mute state (False)."""
        return self.mute(False)

    def toggleMute(self) -> bool:
        """Toggle mute on/off. Returns new mute state."""
        if self._audio_player is None:
            return True
        self._audio_player.muted = not self._audio_player.muted
        self._notify_title_change()
        return self._audio_player.muted

    def isMuted(self) -> bool:
        return True if self._audio_player is None else self._audio_player.muted

    def setVolume(self, volume: int) -> int:
        """Set audio volume (0-100). Returns clamped volume."""
        if self._audio_player is None:
            return 0
        vol = self._audio_player.set_volume(volume)
        self._notify_title_change()
        return vol

    def adjustVolume(self, delta: int) -> int:
        """Adjust audio volume by delta (clamped 0-100). Returns new volume."""
        if self._audio_player is None:
            return 0
        vol = self._audio_player.adjust_volume(delta)
        self._notify_title_change()
        return vol

    def getVolume(self) -> int:
        return 0 if self._audio_player is None else self._audio_player.volume

    def setHalfScale(self, enabled: bool) -> bool:
        self.half_scale = bool(enabled)
        return self.half_scale

    def toggleHalfScale(self) -> bool:
        self.half_scale = not self.half_scale
        return self.half_scale

    def isRunning(self) -> bool:
        return self._running

    def isConnected(self) -> bool:
        with self._state_lock:
            return bool(self._stats["connected"])

    def getStats(self) -> dict:
        with self._state_lock:
            st = dict(self._stats)
        st["volume"] = self.getVolume()
        st["muted"] = self.isMuted()
        st["audio"] = self.audio_enabled
        return st

    # --- Lifecycle Controls ---

    def start(self):
        """Start the player in a background thread and return immediately."""
        if self._thread is not None and self._thread.is_alive():
            return self
        self._stop_event.clear()
        self._running = True
        self._thread = threading.Thread(target=self.run, daemon=True)
        self._thread.start()
        return self

    def quit(self) -> None:
        """Stop the player cleanly and close all connections/windows."""
        self._stop_event.set()
        rx = self._receiver
        if rx is not None:
            rx.stop()
        ta = self._tcp_audio_rx
        if ta is not None:
            ta.stop()
        t = self._thread
        if t is not None and t.is_alive() and threading.current_thread() is not t:
            t.join(timeout=5.0)
        self._thread = None

    stop = quit

    def wait(self, timeout=None) -> None:
        """Wait for a background-started player thread to exit."""
        t = self._thread
        if t is not None and threading.current_thread() is not t:
            t.join(timeout=timeout)

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.quit()
        return False

    def _format_audio_title(self) -> str:
        if self._audio_player is None:
            return "Audio: OFF"
        if self._audio_player.muted:
            return f"Vol: {self._audio_player.volume}% (MUTED)"
        return f"Vol: {self._audio_player.volume}%"

    def run(self) -> None:
        self._stop_event.clear()
        self._running = True

        target_desc = "USB (bottom USB-C, 1209:BEEB)" if self.use_usb else f"{self.host}:{self.port}"
        audio_desc = "OFF" if not self.audio_enabled else ("USB muxed" if self.use_usb else f"port {self.audio_port}")
        if self.verbose:
            print("===================================================", flush=True)
            print("               SimpleStreamer Client               ", flush=True)
            print("===================================================", flush=True)
            print(f"Target: {target_desc} (Audio: {audio_desc})")
            print("Controls: [ESC / Q] Quit | [H] Half-Scale | [M] Mute | [Up/Down] Volume +-5%\n", flush=True)

        win_name = self.window_name
        cv2.namedWindow(win_name, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(win_name, self.window_size[0], self.window_size[1])

        if self.use_usb:
            receiver = _UsbStreamReceiver(self._audio_player, verbose=self.verbose)
            tcp_audio_rx = None
        else:
            receiver = _TcpVideoReceiver(self.host, self.port, verbose=self.verbose)
            tcp_audio_rx = (
                _TcpAudioReceiver(self.host, self.audio_port, self._audio_player, verbose=self.verbose)
                if self._audio_player is not None
                else None
            )

        self._receiver = receiver
        self._tcp_audio_rx = tcp_audio_rx

        last_img = None
        last_seq = 0
        mode_tag = "USB" if self.use_usb else "Wi-Fi"
        base_title = f"Switch 2 Stream ({mode_tag})"
        wait_label = "Switch 2 over USB (bottom port)" if self.use_usb else f"Switch 2 ({self.host}:{self.port})"

        def update_title():
            self._title_dirty.clear()
            cv2.setWindowTitle(win_name, f"{base_title} | {self._format_audio_title()}")

        def handle_key(key):
            if key == -1:
                return False
            if key in KEY_UP_CODES and self._audio_player is not None:
                self._audio_player.adjust_volume(VOLUME_STEP)
                update_title()
                return False
            if key in KEY_DOWN_CODES and self._audio_player is not None:
                self._audio_player.adjust_volume(-VOLUME_STEP)
                update_title()
                return False
            if 0 <= key < 256:
                if key in (27, ord("q"), ord("Q")):
                    if self.verbose:
                        print("\nQuit key pressed, closing stream.", flush=True)
                    return True
                if key in (ord("h"), ord("H")):
                    self.half_scale = not self.half_scale
                    if self.verbose:
                        print(f"\n[Fast Half-Scale Decode: {'ON (960x540)' if self.half_scale else 'OFF (1920x1080)'}]", flush=True)
                elif key in (ord("m"), ord("M")) and self._audio_player is not None:
                    self._audio_player.muted = not self._audio_player.muted
                    update_title()
            return False

        update_title()
        stat_time = time.time()
        disp_frames = 0

        try:
            while receiver.running and not self._stop_event.is_set():
                receiver.frame_event.wait(timeout=0.05)
                receiver.frame_event.clear()

                if self._stop_event.is_set():
                    break

                if self._title_dirty.is_set():
                    update_title()

                with self._state_lock:
                    self._stats["connected"] = receiver.connected

                if not receiver.connected:
                    if last_img is not None:
                        display = last_img.copy()
                        h, w = display.shape[:2]
                        overlay = display.copy()
                        cv2.rectangle(overlay, (0, 0), (w, 60), (20, 20, 20), -1)
                        cv2.addWeighted(overlay, 0.75, display, 0.25, 0, display)
                        cv2.putText(display, f"Connecting to {wait_label}...", (20, 40),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.85, (0, 215, 255), 2, cv2.LINE_AA)
                        cv2.imshow(win_name, display)
                    else:
                        blank = np.zeros((720, 1280, 3), dtype=np.uint8)
                        cv2.putText(blank, f"Waiting for {wait_label}...", (140, 360),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.95, (220, 220, 220), 2, cv2.LINE_AA)
                        cv2.imshow(win_name, blank)

                    if handle_key(cv2.waitKeyEx(50)):
                        break
                    if cv2.getWindowProperty(win_name, cv2.WND_PROP_VISIBLE) < 1:
                        break
                    continue

                jpeg_view, frame_size, new_seq = receiver.get_latest_frame(last_seq)
                if jpeg_view is not None:
                    last_seq = new_seq
                    arr = np.frombuffer(jpeg_view, dtype=np.uint8)
                    flag = cv2.IMREAD_REDUCED_COLOR_2 if self.half_scale else cv2.IMREAD_COLOR
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
                    with self._state_lock:
                        self._stats["rx_fps"] = net_fps
                        self._stats["disp_fps"] = disp_fps
                        self._stats["bitrate_mbps"] = mbps
                        self._stats["frame_kb"] = last_fs / 1024.0
                        self._stats["resolution"] = res_str
                    base_title = f"Switch 2 Stream ({mode_tag}) - {res_str} @ {net_fps:.1f} / Disp {disp_fps:.1f} FPS ({last_fs / 1024:.0f} KB, {mbps:.1f} Mbps)"
                    update_title()
                    if self.verbose:
                        print(f"PC Receiver ({mode_tag}): Rx {net_fps:.1f} FPS | Disp {disp_fps:.1f} FPS | Frame: {last_fs / 1024:.1f} KB | Bitrate: {mbps:.1f} Mbps | Res: {res_str}", flush=True)
                    disp_frames = 0
                    stat_time = now

                if handle_key(cv2.waitKeyEx(1)):
                    break
                if cv2.getWindowProperty(win_name, cv2.WND_PROP_VISIBLE) < 1:
                    break
        except KeyboardInterrupt:
            if self.verbose:
                print("\nInterrupted by user, shutting down...", flush=True)
        finally:
            self._stop_event.set()
            receiver.stop()
            if tcp_audio_rx is not None:
                tcp_audio_rx.stop()
            if self._audio_player is not None:
                self._audio_player.stop()
            self._receiver = None
            self._tcp_audio_rx = None
            self._running = False
            with self._state_lock:
                self._stats["connected"] = False
            try:
                cv2.destroyWindow(win_name)
                cv2.waitKey(1)
            except Exception:
                pass
            if self.verbose:
                print("Stream viewer closed.", flush=True)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    no_audio = "--no-audio" in sys.argv[1:]
    use_usb = "--usb" in sys.argv[1:]

    if use_usb:
        host = "USB"
    elif len(args) > 0:
        host = args[0]
    else:
        raise ValueError("Provide the console's IP address (e.g. 'python cv2_client.py 192.168.x.x') or pass '--usb'")

    player = Cv2Player(host=host, audio=not no_audio)
    player.run()


__all__ = ["Cv2Player", "AudioPlayer"]

if __name__ == "__main__":
    main()
