#!/usr/bin/env python3
"""
Shared Switch 2 USB Bulk transport for cv2-client.py and ffplay-client.py (--usb).
- Zero external Python dependencies on Windows (uses built-in ctypes + winusb.dll + setupapi.dll).
- Falls back to pyusb on Linux/macOS.
"""

import ctypes
import struct
import sys

USB_VID = 0x1209
USB_PID = 0xBEEB

USB_CMD_START = 0x54525453  # "STRT"
USB_CMD_STOP = 0x504F5453   # "STOP"
USB_FLAG_VIDEO = 1 << 0
USB_FLAG_AUDIO = 1 << 1

MAX_USB_READ_CHUNK = 1024 * 1024  # 1 MB per WinUsb_ReadPipe call


class UsbNotFoundError(RuntimeError):
    pass


class UsbDriverError(RuntimeError):
    pass


if sys.platform == "win32":
    from ctypes import wintypes

    class GUID(ctypes.Structure):
        _fields_ = [
            ("Data1", wintypes.DWORD),
            ("Data2", wintypes.WORD),
            ("Data3", wintypes.WORD),
            ("Data4", ctypes.c_ubyte * 8),
        ]

        @classmethod
        def from_str(cls, s: str) -> "GUID":
            s = s.strip().strip("{}")
            parts = s.split("-")
            d1 = int(parts[0], 16)
            d2 = int(parts[1], 16)
            d3 = int(parts[2], 16)
            d4_bytes = bytes.fromhex(parts[3] + parts[4])
            return cls(d1, d2, d3, (ctypes.c_ubyte * 8)(*d4_bytes))

    class SP_DEVINFO_DATA(ctypes.Structure):
        _fields_ = [
            ("cbSize", wintypes.DWORD),
            ("ClassGuid", GUID),
            ("DevInst", wintypes.DWORD),
            ("Reserved", ctypes.c_void_p),
        ]

    class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
        _fields_ = [
            ("cbSize", wintypes.DWORD),
            ("InterfaceClassGuid", GUID),
            ("Flags", wintypes.DWORD),
            ("Reserved", ctypes.c_void_p),
        ]

    class USB_INTERFACE_DESCRIPTOR(ctypes.Structure):
        _fields_ = [
            ("bLength", ctypes.c_ubyte),
            ("bDescriptorType", ctypes.c_ubyte),
            ("bInterfaceNumber", ctypes.c_ubyte),
            ("bAlternateSetting", ctypes.c_ubyte),
            ("bNumEndpoints", ctypes.c_ubyte),
            ("bInterfaceClass", ctypes.c_ubyte),
            ("bInterfaceSubClass", ctypes.c_ubyte),
            ("bInterfaceProtocol", ctypes.c_ubyte),
            ("iInterface", ctypes.c_ubyte),
        ]

    class WINUSB_PIPE_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("PipeType", ctypes.c_int),
            ("PipeId", ctypes.c_ubyte),
            ("MaximumPacketSize", ctypes.c_ushort),
            ("Interval", ctypes.c_ubyte),
        ]

    _setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    _winusb = ctypes.WinDLL("winusb", use_last_error=True)

    _HDEVINFO = ctypes.c_void_p
    _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    _DIGCF_PRESENT = 0x00000002
    _DIGCF_ALLCLASSES = 0x00000004
    _DIGCF_DEVICEINTERFACE = 0x00000010
    _SPDRP_SERVICE = 0x00000004
    _DICS_FLAG_GLOBAL = 0x00000001
    _DIREG_DEV = 0x00000001
    _KEY_READ = 0x20019

    _GENERIC_READ = 0x80000000
    _GENERIC_WRITE = 0x40000000
    _FILE_SHARE_READ = 0x00000001
    _FILE_SHARE_WRITE = 0x00000002
    _OPEN_EXISTING = 3
    _FILE_ATTRIBUTE_NORMAL = 0x00000080
    _FILE_FLAG_OVERLAPPED = 0x40000000
    _PIPE_TRANSFER_TIMEOUT = 0x03

    _setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD]
    _setupapi.SetupDiGetClassDevsW.restype = _HDEVINFO
    _setupapi.SetupDiEnumDeviceInfo.argtypes = [_HDEVINFO, wintypes.DWORD, ctypes.POINTER(SP_DEVINFO_DATA)]
    _setupapi.SetupDiEnumDeviceInfo.restype = wintypes.BOOL
    _setupapi.SetupDiGetDeviceInstanceIdW.argtypes = [
        _HDEVINFO, ctypes.POINTER(SP_DEVINFO_DATA), wintypes.LPWSTR, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)
    ]
    _setupapi.SetupDiGetDeviceInstanceIdW.restype = wintypes.BOOL
    _setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
        _HDEVINFO, ctypes.POINTER(SP_DEVINFO_DATA), ctypes.POINTER(GUID),
        wintypes.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)
    ]
    _setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
    _setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
        _HDEVINFO, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p,
        wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(SP_DEVINFO_DATA)
    ]
    _setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
    _setupapi.SetupDiGetDeviceRegistryPropertyW.argtypes = [
        _HDEVINFO, ctypes.POINTER(SP_DEVINFO_DATA), wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)
    ]
    _setupapi.SetupDiGetDeviceRegistryPropertyW.restype = wintypes.BOOL
    _setupapi.SetupDiOpenDevRegKey.argtypes = [
        _HDEVINFO, ctypes.POINTER(SP_DEVINFO_DATA), wintypes.DWORD,
        wintypes.DWORD, wintypes.DWORD, wintypes.DWORD
    ]
    _setupapi.SetupDiOpenDevRegKey.restype = wintypes.HKEY
    _setupapi.SetupDiDestroyDeviceInfoList.argtypes = [_HDEVINFO]
    _setupapi.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL

    _advapi32.RegQueryValueExW.argtypes = [
        wintypes.HKEY, wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD),
        ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)
    ]
    _advapi32.RegQueryValueExW.restype = wintypes.LONG
    _advapi32.RegCloseKey.argtypes = [wintypes.HKEY]
    _advapi32.RegCloseKey.restype = wintypes.LONG

    _kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE
    ]
    _kernel32.CreateFileW.restype = wintypes.HANDLE
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel32.CloseHandle.restype = wintypes.BOOL

    _winusb.WinUsb_Initialize.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
    _winusb.WinUsb_Initialize.restype = wintypes.BOOL
    _winusb.WinUsb_Free.argtypes = [ctypes.c_void_p]
    _winusb.WinUsb_Free.restype = wintypes.BOOL
    _winusb.WinUsb_QueryInterfaceSettings.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.POINTER(USB_INTERFACE_DESCRIPTOR)]
    _winusb.WinUsb_QueryInterfaceSettings.restype = wintypes.BOOL
    _winusb.WinUsb_QueryPipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_ubyte, ctypes.POINTER(WINUSB_PIPE_INFORMATION)]
    _winusb.WinUsb_QueryPipe.restype = wintypes.BOOL
    _winusb.WinUsb_SetPipePolicy.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, wintypes.ULONG, wintypes.ULONG, ctypes.c_void_p]
    _winusb.WinUsb_SetPipePolicy.restype = wintypes.BOOL
    _winusb.WinUsb_ResetPipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte]
    _winusb.WinUsb_ResetPipe.restype = wintypes.BOOL
    _winusb.WinUsb_FlushPipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte]
    _winusb.WinUsb_FlushPipe.restype = wintypes.BOOL
    _winusb.WinUsb_AbortPipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte]
    _winusb.WinUsb_AbortPipe.restype = wintypes.BOOL
    _winusb.WinUsb_ReadPipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG), ctypes.c_void_p]
    _winusb.WinUsb_ReadPipe.restype = wintypes.BOOL
    _winusb.WinUsb_WritePipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG), ctypes.c_void_p]
    _winusb.WinUsb_WritePipe.restype = wintypes.BOOL

    _GUID_DEVINTERFACE_USB_DEVICE = GUID.from_str("{A5DCBF10-6530-11D2-901F-00C04FB951ED}")

    def _read_dev_reg_str(hkey, name):
        val_type = wintypes.DWORD(0)
        buf_size = wintypes.DWORD(0)
        rc = _advapi32.RegQueryValueExW(hkey, name, None, ctypes.byref(val_type), None, ctypes.byref(buf_size))
        if rc != 0 or buf_size.value == 0:
            return []
        buf = ctypes.create_string_buffer(buf_size.value)
        rc = _advapi32.RegQueryValueExW(hkey, name, None, ctypes.byref(val_type), buf, ctypes.byref(buf_size))
        if rc != 0:
            return []
        raw_str = buf.raw.decode("utf-16-le", errors="ignore")
        return [s for s in raw_str.split("\x00") if s]

    def _enum_interface_paths(guid: GUID, vid: int, pid: int):
        needle = f"vid_{vid:04x}&pid_{pid:04x}"
        hdev = _setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, _DIGCF_PRESENT | _DIGCF_DEVICEINTERFACE)
        if hdev == _INVALID_HANDLE_VALUE:
            return []
        results = []
        try:
            idx = 0
            while True:
                iface = SP_DEVICE_INTERFACE_DATA()
                iface.cbSize = ctypes.sizeof(iface)
                if not _setupapi.SetupDiEnumDeviceInterfaces(hdev, None, ctypes.byref(guid), idx, ctypes.byref(iface)):
                    break
                idx += 1

                req_size = wintypes.DWORD(0)
                _setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(iface), None, 0, ctypes.byref(req_size), None)
                if req_size.value == 0:
                    continue

                buf = ctypes.create_string_buffer(req_size.value)
                cb = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
                struct.pack_into("<I", buf, 0, cb)
                devinfo = SP_DEVINFO_DATA()
                devinfo.cbSize = ctypes.sizeof(devinfo)
                if not _setupapi.SetupDiGetDeviceInterfaceDetailW(
                    hdev, ctypes.byref(iface), buf, req_size, None, ctypes.byref(devinfo)
                ):
                    continue

                path = buf.raw[4:].decode("utf-16-le", errors="ignore").split("\x00", 1)[0]
                if needle not in path.lower():
                    continue

                svc_buf = ctypes.create_unicode_buffer(256)
                svc_name = ""
                if _setupapi.SetupDiGetDeviceRegistryPropertyW(
                    hdev, ctypes.byref(devinfo), _SPDRP_SERVICE, None, svc_buf, ctypes.sizeof(svc_buf), None
                ):
                    svc_name = svc_buf.value

                guids = []
                hkey = _setupapi.SetupDiOpenDevRegKey(hdev, ctypes.byref(devinfo), _DICS_FLAG_GLOBAL, 0, _DIREG_DEV, _KEY_READ)
                if hkey and hkey != _INVALID_HANDLE_VALUE:
                    try:
                        guids.extend(_read_dev_reg_str(hkey, "DeviceInterfaceGUIDs"))
                        guids.extend(_read_dev_reg_str(hkey, "DeviceInterfaceGUID"))
                    finally:
                        _advapi32.RegCloseKey(hkey)

                results.append((path, svc_name, guids))
        finally:
            _setupapi.SetupDiDestroyDeviceInfoList(hdev)
        return results

    def _has_pnp_usb_device(vid: int, pid: int):
        needle = f"vid_{vid:04x}&pid_{pid:04x}"
        hdev = _setupapi.SetupDiGetClassDevsW(None, "USB", None, _DIGCF_ALLCLASSES | _DIGCF_PRESENT)
        if hdev == _INVALID_HANDLE_VALUE:
            return None
        try:
            idx = 0
            while True:
                devinfo = SP_DEVINFO_DATA()
                devinfo.cbSize = ctypes.sizeof(devinfo)
                if not _setupapi.SetupDiEnumDeviceInfo(hdev, idx, ctypes.byref(devinfo)):
                    break
                idx += 1
                inst_buf = ctypes.create_unicode_buffer(512)
                if not _setupapi.SetupDiGetDeviceInstanceIdW(hdev, ctypes.byref(devinfo), inst_buf, 512, None):
                    continue
                if needle in inst_buf.value.lower():
                    svc_buf = ctypes.create_unicode_buffer(256)
                    svc = ""
                    if _setupapi.SetupDiGetDeviceRegistryPropertyW(
                        hdev, ctypes.byref(devinfo), _SPDRP_SERVICE, None, svc_buf, ctypes.sizeof(svc_buf), None
                    ):
                        svc = svc_buf.value
                    return (inst_buf.value, svc)
        finally:
            _setupapi.SetupDiDestroyDeviceInfoList(hdev)
        return None

    class WinUsbStream:
        def __init__(self, vid=USB_VID, pid=USB_PID, timeout_ms=3000):
            self.hfile = None
            self.hwinusb = ctypes.c_void_p(None)
            self.ep_in = None
            self.ep_out = None
            self.max_pkt_in = 0
            self.max_pkt_out = 0

            found = _enum_interface_paths(_GUID_DEVINTERFACE_USB_DEVICE, vid, pid)
            if not found:
                pnp = _has_pnp_usb_device(vid, pid)
                if pnp is not None:
                    _, svc = pnp
                    raise UsbDriverError(
                        f"Switch 2 USB ({vid:04X}:{pid:04X}) detected, but driver is '{svc or 'NONE'}' instead of WinUSB. "
                        f"Bind WinUSB in Zadig (Options -> List All Devices -> Switch2 Streamer)."
                    )
                raise UsbNotFoundError(f"Switch 2 USB ({vid:04X}:{pid:04X}) not connected on bottom USB-C port.")

            usb_path, svc_name, guids = found[0]
            if svc_name.upper() != "WINUSB":
                raise UsbDriverError(
                    f"Switch 2 USB ({vid:04X}:{pid:04X}) driver is '{svc_name or 'NONE'}' instead of WinUSB. "
                    f"Bind WinUSB in Zadig."
                )

            open_paths = []
            for g in guids:
                try:
                    for p, _, _ in _enum_interface_paths(GUID.from_str(g), vid, pid):
                        open_paths.append(p)
                except Exception:
                    pass
            open_paths.append(usb_path)

            last_err = 0
            for path in open_paths:
                hf = _kernel32.CreateFileW(
                    path,
                    _GENERIC_READ | _GENERIC_WRITE,
                    _FILE_SHARE_READ | _FILE_SHARE_WRITE,
                    None,
                    _OPEN_EXISTING,
                    _FILE_ATTRIBUTE_NORMAL | _FILE_FLAG_OVERLAPPED,
                    None,
                )
                if hf == _INVALID_HANDLE_VALUE:
                    last_err = ctypes.get_last_error()
                    continue
                hw = ctypes.c_void_p(None)
                if _winusb.WinUsb_Initialize(hf, ctypes.byref(hw)):
                    self.hfile = hf
                    self.hwinusb = hw
                    break
                last_err = ctypes.get_last_error()
                _kernel32.CloseHandle(hf)

            if not self.hwinusb.value:
                raise UsbNotFoundError(f"WinUsb_Initialize failed (Win32 err={last_err})")

            idesc = USB_INTERFACE_DESCRIPTOR()
            if not _winusb.WinUsb_QueryInterfaceSettings(self.hwinusb, 0, ctypes.byref(idesc)):
                self.close(send_stop=False)
                raise UsbNotFoundError("WinUsb_QueryInterfaceSettings failed")

            for i in range(idesc.bNumEndpoints):
                pinfo = WINUSB_PIPE_INFORMATION()
                if _winusb.WinUsb_QueryPipe(self.hwinusb, 0, i, ctypes.byref(pinfo)):
                    tval = wintypes.ULONG(timeout_ms)
                    _winusb.WinUsb_SetPipePolicy(self.hwinusb, pinfo.PipeId, _PIPE_TRANSFER_TIMEOUT, 4, ctypes.byref(tval))
                    if pinfo.PipeId & 0x80:
                        self.ep_in = pinfo.PipeId
                        self.max_pkt_in = pinfo.MaximumPacketSize
                    else:
                        self.ep_out = pinfo.PipeId
                        self.max_pkt_out = pinfo.MaximumPacketSize

            if self.ep_in is None or self.ep_out is None:
                self.close(send_stop=False)
                raise UsbNotFoundError("Missing Bulk IN/OUT endpoints")

            _winusb.WinUsb_ResetPipe(self.hwinusb, self.ep_in)
            _winusb.WinUsb_FlushPipe(self.hwinusb, self.ep_in)
            _winusb.WinUsb_ResetPipe(self.hwinusb, self.ep_out)

        def send_ctrl(self, magic: int, arg: int = 0):
            data = struct.pack("<II", magic, arg)
            written = wintypes.ULONG(0)
            buf = ctypes.create_string_buffer(data, len(data))
            if not _winusb.WinUsb_WritePipe(self.hwinusb, self.ep_out, buf, len(data), ctypes.byref(written), None):
                raise ConnectionError(f"WinUsb_WritePipe failed (err={ctypes.get_last_error()})")

        def start_stream(self, want_video=True, want_audio=True):
            flags = (USB_FLAG_VIDEO if want_video else 0) | (USB_FLAG_AUDIO if want_audio else 0)
            self.send_ctrl(USB_CMD_START, flags)

        def read_into_exact(self, buf: bytearray, n: int):
            pos = 0
            while pos < n:
                chunk = min(n - pos, MAX_USB_READ_CHUNK)
                arr = (ctypes.c_ubyte * chunk).from_buffer(buf, pos)
                got = wintypes.ULONG(0)
                if not _winusb.WinUsb_ReadPipe(self.hwinusb, self.ep_in, arr, chunk, ctypes.byref(got), None):
                    raise ConnectionError(f"WinUsb_ReadPipe failed (err={ctypes.get_last_error()})")
                if got.value == 0:
                    raise ConnectionResetError("USB pipe returned 0 bytes")
                pos += got.value

        def close(self, send_stop=True):
            if self.hwinusb and self.hwinusb.value:
                if send_stop and self.ep_out is not None:
                    try:
                        self.send_ctrl(USB_CMD_STOP, 0)
                    except Exception:
                        pass
                if self.ep_in is not None:
                    try:
                        _winusb.WinUsb_AbortPipe(self.hwinusb, self.ep_in)
                    except Exception:
                        pass
                _winusb.WinUsb_Free(self.hwinusb)
                self.hwinusb = ctypes.c_void_p(None)
            if self.hfile and self.hfile != _INVALID_HANDLE_VALUE:
                _kernel32.CloseHandle(self.hfile)
                self.hfile = None


def open_usb_stream(want_video=True, want_audio=True, timeout_ms=3000):
    if sys.platform == "win32":
        dev = WinUsbStream(USB_VID, USB_PID, timeout_ms=timeout_ms)
        dev.start_stream(want_video=want_video, want_audio=want_audio)
        return dev

    import usb.core
    import usb.util

    dev = usb.core.find(idVendor=USB_VID, idProduct=USB_PID)
    if dev is None:
        raise UsbNotFoundError(f"Switch 2 USB ({USB_VID:04X}:{USB_PID:04X}) not connected.")
    dev.set_configuration()
    cfg = dev.get_active_configuration()
    intf = cfg[(0, 0)]
    ep_out = usb.util.find_descriptor(
        intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT
    )
    ep_in = usb.util.find_descriptor(
        intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN
    )

    class PyUsbStream:
        def __init__(self):
            self.max_pkt_in = ep_in.wMaxPacketSize
            self.max_pkt_out = ep_out.wMaxPacketSize

        def send_ctrl(self, magic: int, arg: int = 0):
            ep_out.write(struct.pack("<II", magic, arg), timeout=timeout_ms)

        def start_stream(self, want_video=True, want_audio=True):
            flags = (USB_FLAG_VIDEO if want_video else 0) | (USB_FLAG_AUDIO if want_audio else 0)
            self.send_ctrl(USB_CMD_START, flags)

        def read_into_exact(self, buf: bytearray, n: int):
            mv = memoryview(buf)
            pos = 0
            while pos < n:
                chunk = min(n - pos, MAX_USB_READ_CHUNK)
                got = ep_in.read(mv[pos : pos + chunk], timeout=timeout_ms)
                if got == 0:
                    raise ConnectionResetError("USB pipe returned 0 bytes")
                pos += got

        def close(self, send_stop=True):
            if send_stop:
                try:
                    self.send_ctrl(USB_CMD_STOP, 0)
                except Exception:
                    pass
            usb.util.dispose_resources(dev)

    tr = PyUsbStream()
    tr.start_stream(want_video=want_video, want_audio=want_audio)
    return tr
