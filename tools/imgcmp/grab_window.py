"""Screen captures of the game window at a fixed rate, whatever renders it (bridge or native backend; P4.5 A/B).

Usage: py -I tools/imgcmp/grab_window.py --out <dir> [--start 30] [--seconds 20] [--fps 20] [--scale 2]
Waits for SecondSonPC.exe's visible top-level window, then from --start seconds after the process started grabs its
client area (PrintWindow with PW_RENDERFULLCONTENT: the window's own content even when other windows cover it, as
tools
un_phase0.ps1's screenshot) every 1/fps s for --seconds,
downscaled by --scale, as <out>/grab_<ms since process start>.png. The process is never touched.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import queue
import threading
import time

from PIL import Image

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi


def find_process(name='SecondSonPC.exe'):
    arr = (wt.DWORD * 4096)()
    needed = wt.DWORD()
    psapi.EnumProcesses(arr, ctypes.sizeof(arr), ctypes.byref(needed))
    for pid in arr[: needed.value // 4]:
        h = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            continue
        buf = ctypes.create_unicode_buffer(1024)
        size = wt.DWORD(1024)
        ok = kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size))
        created = None
        if ok and buf.value.lower().endswith(name.lower()):
            ft = (wt.FILETIME * 4)()
            kernel32.GetProcessTimes(h, ctypes.byref(ft[0]), ctypes.byref(ft[1]), ctypes.byref(ft[2]), ctypes.byref(ft[3]))
            created = ((ft[0].dwHighDateTime << 32) | ft[0].dwLowDateTime) / 1e7 - 11644473600.0
        kernel32.CloseHandle(h)
        if created is not None:
            return pid, created
    return None, None


def find_window(pid):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            r = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            if r.right - r.left >= 320 and r.bottom - r.top >= 200:
                found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


gdi32 = ctypes.windll.gdi32
# 64-bit handles: the default int return type would truncate them
user32.GetDC.restype = wt.HDC
user32.GetDC.argtypes = [wt.HWND]
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT]
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wt.DWORD), ('biWidth', wt.LONG), ('biHeight', wt.LONG), ('biPlanes', wt.WORD),
                ('biBitCount', wt.WORD), ('biCompression', wt.DWORD), ('biSizeImage', wt.DWORD),
                ('biXPelsPerMeter', wt.LONG), ('biYPelsPerMeter', wt.LONG), ('biClrUsed', wt.DWORD),
                ('biClrImportant', wt.DWORD)]


def print_window(hwnd):
    """The window's client area as an RGB image (PrintWindow, PW_CLIENTONLY | PW_RENDERFULLCONTENT)."""
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc_win = user32.GetDC(hwnd)
    hdc = gdi32.CreateCompatibleDC(hdc_win)
    bmp = gdi32.CreateCompatibleBitmap(hdc_win, w, h)
    old = gdi32.SelectObject(hdc, bmp)
    user32.PrintWindow(hwnd, hdc, 1 | 2)
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth, bi.biHeight, bi.biPlanes, bi.biBitCount = w, -h, 1, 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi32.SelectObject(hdc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(hdc)
    user32.ReleaseDC(hwnd, hdc_win)
    return Image.frombuffer('RGB', (w, h), buf, 'raw', 'BGRX', 0, 1)


def client_bbox(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return (pt.x, pt.y, pt.x + r.right, pt.y + r.bottom)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--start', type=float, default=30.0)
    ap.add_argument('--seconds', type=float, default=20.0)
    ap.add_argument('--fps', type=float, default=20.0)
    ap.add_argument('--scale', type=int, default=2)
    ap.add_argument('--wait', type=float, default=120.0, help='seconds to wait for the process')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    t_wait = time.time()
    pid = None
    while pid is None and time.time() - t_wait < a.wait:
        pid, created = find_process()
        if pid is None:
            time.sleep(0.2)
    if pid is None:
        print('no SecondSonPC.exe')
        return
    while time.time() < created + a.start:
        time.sleep(0.05)
    hwnd = find_window(pid)
    if hwnd is None:
        print('no window')
        return
    q = queue.Queue()

    def writer():
        while True:
            item = q.get()
            if item is None:
                return
            im, path = item
            if a.scale > 1:
                im = im.resize((im.width // a.scale, im.height // a.scale))
            im.save(path)

    th = threading.Thread(target=writer)
    th.start()
    n = 0
    period = 1.0 / a.fps
    t_end = created + a.start + a.seconds
    nxt = time.time()
    while time.time() < t_end:
        im = print_window(hwnd)
        ms = int((time.time() - created) * 1000)
        q.put((im, os.path.join(a.out, f'grab_{ms:07d}.png')))
        n += 1
        nxt += period
        delay = nxt - time.time()
        if delay > 0:
            time.sleep(delay)
    q.put(None)
    th.join()
    print(f'{n} grabs of pid {pid} window {hwnd} -> {a.out}')


if __name__ == '__main__':
    main()
