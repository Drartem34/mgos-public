#!/usr/bin/env python3
# Знімок одного вікна (навіть прозорого/під іншими) через hyprland_toplevel_export_v1 → PPM (P6).
# grim -T потребує ext-image-copy-capture, якого Hyprland 0.53 не має — тому сирий протокол, без залежностей.
# Виклик: toplevel_shot.py <адреса вікна 0x...> <out.ppm>; код 0 — успіх.
import mmap, os, socket, struct, sys

FMT = {0: (2, 1, 0), 1: (2, 1, 0), 0x34325241: (2, 1, 0), 0x34325258: (2, 1, 0),   # ARGB/XRGB: B G R A у пам'яті
       0x34324241: (0, 1, 2), 0x34324258: (0, 1, 2)}                                # ABGR/XBGR: R G B A


def main():
    addr, out = sys.argv[1], sys.argv[2]
    handle = int(addr, 16) & 0xFFFFFFFF
    path = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), os.environ.get("WAYLAND_DISPLAY", "wayland-0"))
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    s.settimeout(1.5)
    nid = [2]

    def new():
        i = nid[0]; nid[0] += 1; return i

    def wstr(t):
        b = t.encode() + b"\0"
        return struct.pack("<I", len(b)) + b + b"\0" * ((4 - len(b) % 4) % 4)

    def send(obj, op, body=b"", fd=None):
        msg = struct.pack("<II", obj, ((8 + len(body)) << 16) | op) + body
        if fd is None: s.sendall(msg)
        else: s.sendmsg([msg], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("i", fd))])

    def rstr(b, o):
        n = struct.unpack_from("<I", b, o)[0]
        return b[o + 4:o + 4 + n - 1].decode(errors="replace"), o + 4 + n + ((4 - n % 4) % 4)

    reg = new(); send(1, 1, struct.pack("<I", reg))
    buf = b""; st = {"shm": None, "exp": None, "fmt": None, "flags": 0, "done": False, "ready": False, "failed": False}
    frame = [None]; cbs = set()

    def pump(until):
        nonlocal buf
        while not until():
            while len(buf) < 8 or len(buf) < (struct.unpack_from("<I", buf, 4)[0] >> 16):
                d = s.recv(65536)
                if not d: return
                buf += d
            obj, so = struct.unpack_from("<II", buf, 0); size = so >> 16; op = so & 0xffff
            body = buf[8:size]; buf = buf[size:]
            if obj in cbs: cbs.discard(obj); st["cb"] = True
            elif obj == 1 and op == 0: st["failed"] = True   # wl_display.error
            elif obj == reg and op == 0:
                name = struct.unpack_from("<I", body, 0)[0]
                iface, o = rstr(body, 4)
                if iface == "wl_shm" and st["shm"] is None:
                    st["shm"] = new(); send(reg, 0, struct.pack("<I", name) + wstr(iface) + struct.pack("<II", 1, st["shm"]))
                elif iface == "hyprland_toplevel_export_manager_v1" and st["exp"] is None:
                    st["exp"] = new(); send(reg, 0, struct.pack("<I", name) + wstr(iface) + struct.pack("<II", 1, st["exp"]))
            elif obj == frame[0]:
                if op == 0 and st["fmt"] is None:
                    f, w, h, stride = struct.unpack_from("<IIII", body, 0)
                    if f in FMT: st["fmt"] = (f, w, h, stride)
                elif op == 2: st["flags"] = struct.unpack_from("<I", body, 0)[0]
                elif op == 3: st["ready"] = True
                elif op == 4: st["failed"] = True
                elif op == 6: st["done"] = True

    def roundtrip():
        st["cb"] = False
        cb = new(); cbs.add(cb); send(1, 0, struct.pack("<I", cb))
        pump(lambda: st["cb"] or st["failed"])

    roundtrip()
    if st["shm"] is None or st["exp"] is None: return 1
    frame[0] = new(); send(st["exp"], 0, struct.pack("<IiI", frame[0], 0, handle))
    pump(lambda: st["done"] or st["failed"])
    if st["failed"] or st["fmt"] is None: return 1
    f, w, h, stride = st["fmt"]
    size = stride * h
    fd = os.memfd_create("mgos-shot")
    os.ftruncate(fd, size)
    pool = new(); send(st["shm"], 0, struct.pack("<Ii", pool, size), fd)
    wb = new(); send(pool, 0, struct.pack("<IiiiiI", wb, 0, w, h, stride, f))
    send(frame[0], 0, struct.pack("<Ii", wb, 1))
    pump(lambda: st["ready"] or st["failed"])
    if not st["ready"]: return 1
    mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
    ri, gi, bi = FMT[f]
    rows = []
    for y in range(h):
        row = mm[y * stride:y * stride + w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3] = row[ri::4]; rgb[1::3] = row[gi::4]; rgb[2::3] = row[bi::4]
        rows.append(bytes(rgb))
    if st["flags"] & 1: rows.reverse()   # y_invert
    tmp = out + ".tmp"
    with open(tmp, "wb") as o:
        o.write(b"P6 %d %d 255\n" % (w, h)); o.write(b"".join(rows))
    os.replace(tmp, out)
    return 0


if __name__ == "__main__":
    try: sys.exit(main())
    except Exception: sys.exit(1)
