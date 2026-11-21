#!/usr/bin/env python3
# Перелік вікон через ext-foreign-toplevel-list-v1 (сирий протокол Wayland, без залежностей):
# рядки «identifier<TAB>app_id<TAB>title». identifier потрібен для `grim -T` (знімок вікна, навіть під іншими).
import os, socket, struct, sys


def main():
    path = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), os.environ.get("WAYLAND_DISPLAY", "wayland-0"))
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    s.settimeout(1.0)
    nid = [2]

    def new():
        i = nid[0]; nid[0] += 1; return i

    def wstr(t):
        b = t.encode() + b"\0"
        return struct.pack("<I", len(b)) + b + b"\0" * ((4 - len(b) % 4) % 4)

    def send(obj, op, body=b""):
        s.sendall(struct.pack("<II", obj, ((8 + len(body)) << 16) | op) + body)

    def rstr(b, o):
        n = struct.unpack_from("<I", b, o)[0]
        t = b[o + 4:o + 4 + n - 1].decode(errors="replace")
        return t, o + 4 + n + ((4 - n % 4) % 4)

    reg = new(); send(1, 1, struct.pack("<I", reg))
    buf = b""; lst = None; handles = {}; cb_done = set()

    def roundtrip():
        nonlocal buf, lst
        cb = new(); send(1, 0, struct.pack("<I", cb))
        while cb not in cb_done:
            while len(buf) < 8 or len(buf) < (struct.unpack_from("<I", buf, 4)[0] >> 16):
                d = s.recv(65536)
                if not d: return
                buf += d
            obj, so = struct.unpack_from("<II", buf, 0); size = so >> 16; op = so & 0xffff
            body = buf[8:size]; buf = buf[size:]
            if obj == cb: cb_done.add(cb)
            elif obj == reg and op == 0:
                name = struct.unpack_from("<I", body, 0)[0]
                iface, o = rstr(body, 4)
                if iface == "ext_foreign_toplevel_list_v1" and lst is None:
                    lst = new()
                    send(reg, 0, struct.pack("<I", name) + wstr(iface) + struct.pack("<II", 1, lst))
            elif obj == lst and op == 0:
                handles[struct.unpack_from("<I", body, 0)[0]] = ["", "", ""]
            elif obj in handles and op in (2, 3, 4):
                t, _ = rstr(body, 0)
                handles[obj][{4: 0, 3: 1, 2: 2}[op]] = t

    roundtrip(); roundtrip(); roundtrip()
    for h in handles.values():
        sys.stdout.write("\t".join(x.replace("\t", " ").replace("\n", " ") for x in h) + "\n")


if __name__ == "__main__":
    main()
