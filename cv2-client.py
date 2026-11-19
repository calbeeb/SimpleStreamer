import socket
import struct
import sys
import time
import cv2
import numpy as np

SWITCH_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.8.205"
PORT = 8080
HDR_FMT = "<II"
HDR_SIZE = struct.calcsize(HDR_FMT)
STREAM_MAGIC = 0x5357324A  # "SW2J"

def recv_exact(sock, n):
    buf = bytearray(n)
    view = memoryview(buf)
    pos = 0
    while pos < n:
        nbytes = sock.recv_into(view[pos:], n - pos)
        if nbytes == 0:
            raise ConnectionResetError("Connection closed by Switch")
        pos += nbytes
    return bytes(buf)

def main():
    print(f"==================================================", flush=True)
    print(f"       Switch 2 Live Stream Client (TCP)          ", flush=True)
    print(f"==================================================", flush=True)
    print(f"Target Switch IP: {SWITCH_IP}:{PORT}")
    print(f"Controls: Press 'ESC' or 'q' in the stream window to quit.\n", flush=True)

    win_name = "Switch 2 Stream"
    cv2.namedWindow(win_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(win_name, 1280, 720)

    last_img = None
    running = True

    while running:
        sock = None
        attempt = 0

        # Connection / Auto-Reconnect Loop
        while running and sock is None:
            attempt += 1
            try:
                if attempt == 1:
                    print(f"Connecting to Switch 2 ({SWITCH_IP}:{PORT})...", end="", flush=True)
                else:
                    print(".", end="", flush=True)

                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                s.settimeout(2.0)
                s.connect((SWITCH_IP, PORT))
                s.settimeout(10.0)  # 10s read timeout
                sock = s
                print(f" Connected! [Attempt {attempt}]", flush=True)
            except Exception:
                # Render reconnect overlay over previous frame or a blank placeholder
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

                key = cv2.waitKey(500) & 0xFF
                if key in (27, ord('q'), ord('Q')):
                    running = False
                    break

        if not running or sock is None:
            break

        # Video Streaming Loop
        stat_time = time.time()
        ok_frames = 0
        total_bytes = 0

        try:
            while running:
                try:
                    hdr = recv_exact(sock, HDR_SIZE)
                except (ConnectionResetError, ConnectionError, OSError, socket.timeout) as e:
                    print(f"\n[Connection interrupted: {e}. Transitioning/reconnecting...]", flush=True)
                    break

                magic, frame_size = struct.unpack(HDR_FMT, hdr)
                if magic != STREAM_MAGIC or frame_size == 0 or frame_size > 10 * 1024 * 1024:
                    print(f"\n[Invalid header: magic=0x{magic:08x}, size={frame_size}]", flush=True)
                    break

                try:
                    jpeg_data = recv_exact(sock, frame_size)
                except (ConnectionResetError, ConnectionError, OSError, socket.timeout) as e:
                    print(f"\n[Connection interrupted during frame payload: {e}. Reconnecting...]", flush=True)
                    break

                total_bytes += frame_size
                arr = np.frombuffer(jpeg_data, dtype=np.uint8)
                img = cv2.imdecode(arr, cv2.IMREAD_COLOR)

                if img is not None:
                    last_img = img
                    cv2.imshow(win_name, img)
                    ok_frames += 1

                now = time.time()
                elapsed = now - stat_time
                if elapsed >= 1.0:
                    fps = ok_frames / elapsed
                    mbps = (total_bytes * 8) / (elapsed * 1_000_000)
                    res_str = f"{last_img.shape[1]}x{last_img.shape[0]}" if last_img is not None else ""
                    title = f"Switch 2 Stream - {res_str} @ {fps:.1f} FPS ({frame_size / 1024:.0f} KB/f, {mbps:.1f} Mbps)"
                    cv2.setWindowTitle(win_name, title)
                    print(f"PC Receiver: {fps:.1f} FPS | Frame: {frame_size / 1024:.1f} KB | Bitrate: {mbps:.1f} Mbps | Res: {res_str}", flush=True)
                    ok_frames = 0
                    total_bytes = 0
                    stat_time = now

                key = cv2.waitKey(1) & 0xFF
                if key in (27, ord('q'), ord('Q')):
                    print("\nQuit key pressed, closing stream.", flush=True)
                    running = False
                    break
        finally:
            if sock:
                try:
                    sock.close()
                except Exception:
                    pass

    cv2.destroyAllWindows()
    print("Stream viewer closed.", flush=True)

if __name__ == "__main__":
    main()
