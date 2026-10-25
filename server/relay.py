import asyncio, base64, hashlib, mimetypes, os, struct, sys, urllib.parse

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("PORT", "8080"))
ROOT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else (os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "site") if os.path.isfile(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "site", "index.html")) else os.path.dirname(os.path.abspath(__file__))))
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
BCAST = b"\xff" * 6
MIME = {".html": "text/html; charset=utf-8", ".js": "text/javascript", ".wasm": "application/wasm",
        ".data": "application/octet-stream", ".json": "application/json", ".css": "text/css",
        ".png": "image/png", ".ogg": "audio/ogg", ".mp3": "audio/mpeg", ".wav": "audio/wav"}

rooms = {}


class Client:
    def __init__(self, w, room, node):
        self.w, self.room, self.node = w, room, node

    def send(self, opcode, data):
        n = len(data)
        if n < 126:
            hdr = struct.pack("!BB", 0x80 | opcode, n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x80 | opcode, 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x80 | opcode, 127, n)
        try:
            self.w.write(hdr + data)
        except Exception:
            pass


def announce(room):
    peers = rooms.get(room, {})
    msg = ("peers %d" % len(peers)).encode()
    for c in list(peers.values()):
        c.send(1, msg)


async def websocket(r, w, headers, query):
    key = headers.get("sec-websocket-key", "")
    acc = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()
    w.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
    room = query.get("room", ["lan"])[0][:64] or "lan"
    try:
        node = bytes.fromhex(query.get("node", [""])[0])
    except ValueError:
        node = b""
    if len(node) != 6:
        node = os.urandom(6)
    me = Client(w, room, node)
    peers = rooms.setdefault(room, {})
    old = peers.get(node)
    if old:
        try:
            old.w.close()
        except Exception:
            pass
    peers[node] = me
    print("net: %s joined room '%s' (%d players)" % (node.hex(), room, len(peers)))
    announce(room)
    try:
        while True:
            b0, b1 = await r.readexactly(2)
            op, n = b0 & 15, b1 & 127
            if n == 126:
                n = struct.unpack("!H", await r.readexactly(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", await r.readexactly(8))[0]
            mask = await r.readexactly(4) if b1 & 128 else b"\0\0\0\0"
            data = bytearray(await r.readexactly(n))
            for i in range(n):
                data[i] ^= mask[i & 3]
            if op == 8:
                break
            if op == 9:
                me.send(10, bytes(data))
                continue
            if op != 2 or n < 6:
                continue
            dst, payload = bytes(data[:6]), bytes(data[6:])
            out = node + payload
            if dst == BCAST:
                for c in list(peers.values()):
                    if c is not me:
                        c.send(2, out)
            else:
                c = peers.get(dst)
                if c:
                    c.send(2, out)
    except (asyncio.IncompleteReadError, ConnectionError, OSError):
        pass
    finally:
        if peers.get(node) is me:
            del peers[node]
        print("net: %s left room '%s' (%d players)" % (node.hex(), room, len(peers)))
        announce(room)
        try:
            w.close()
        except Exception:
            pass


async def handle(r, w):
    try:
        line = await r.readline()
        parts = line.decode("latin1").split()
        if len(parts) < 2:
            w.close()
            return
        headers = {}
        while True:
            h = await r.readline()
            if h in (b"\r\n", b"\n", b""):
                break
            k, _, v = h.decode("latin1").partition(":")
            headers[k.strip().lower()] = v.strip()
        url = urllib.parse.urlsplit(parts[1])
        if url.path == "/net" and headers.get("upgrade", "").lower() == "websocket":
            await websocket(r, w, headers, urllib.parse.parse_qs(url.query))
            return
        path = urllib.parse.unquote(url.path).lstrip("/") or "index.html"
        full = os.path.abspath(os.path.join(ROOT, path))
        if not full.startswith(ROOT) or not os.path.isfile(full):
            w.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        else:
            ext = os.path.splitext(full)[1].lower()
            ctype = MIME.get(ext) or mimetypes.guess_type(full)[0] or "application/octet-stream"
            size = os.path.getsize(full)
            head = parts[0] == "HEAD"
            cache = "public, max-age=2592000, immutable" if full[-4:-3] == "." and full[-3:].isdigit() else "no-cache"
            w.write(("HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: %s\r\n"
                     "Connection: close\r\n\r\n" % (ctype, size, cache)).encode())
            if not head:
                with open(full, "rb") as f:
                    while True:
                        chunk = f.read(1 << 20)
                        if not chunk:
                            break
                        w.write(chunk)
                        await w.drain()
        await w.drain()
        w.close()
    except Exception:
        try:
            w.close()
        except Exception:
            pass


async def main():
    srv = await asyncio.start_server(handle, "0.0.0.0", PORT)
    print("Carmageddon II web: http://localhost:%d/  (network relay at /net, Ctrl+C to stop)" % PORT)
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
