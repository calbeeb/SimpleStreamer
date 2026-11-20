import socket
import struct
import subprocess
import sys
import time

if len(sys.argv) > 1:
    SWITCH_IP = sys.argv[1]
else:
    raise ValueError("Please provide the console's IP address (shown in stream-conf.nro)")
PORT = 8080
HDR_FMT = "<II"
HDR_SIZE = struct.calcsize(HDR_FMT)
STREAM_MAGIC = 0x5357324A  # "SW2J"
MAX_FRAME_SIZE = 4 * 1024 * 1024


def recv_into_exact(sock, view, n):
    pos = 0
    while pos < n:
        nbytes = sock.recv_into(view[pos:n], n - pos)
        if nbytes == 0:
            raise ConnectionResetError("Connection closed by Switch")
        pos += nbytes


def main():
    print("==================================================", flush=True)
    print("     Switch 2 Live Stream Client (FFplay)         ", flush=True)
    print("==================================================", flush=True)
    print(f"Target Switch IP: {SWITCH_IP}:{PORT}")
    print("Controls: Close ffplay window or press Ctrl+C to quit.\n", flush=True)

    ffplay = None
    running = True
    hdr_buf = bytearray(HDR_SIZE)
    hdr_view = memoryview(hdr_buf)
    frame_buf = bytearray(MAX_FRAME_SIZE)
    frame_view = memoryview(frame_buf)

    try:
        while running:
            sock = None
            attempt = 0

            # Reconnection Loop
            while running and sock is None:
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
                while running and ffplay.poll() is None:
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
        if ffplay and ffplay.poll() is None:
            try:
                ffplay.terminate()
            except Exception:
                pass
        print("Stream client closed.", flush=True)


if __name__ == "__main__":
    main()
