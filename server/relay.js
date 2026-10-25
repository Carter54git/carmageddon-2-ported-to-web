'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const args = process.argv.slice(2);
function opt(name, def) {
  const i = args.indexOf('--' + name);
  if (i >= 0 && i + 1 < args.length) return args[i + 1];
  const env = process.env['CARMA2_' + name.toUpperCase()];
  return env !== undefined && env !== '' ? env : def;
}
const PORT = parseInt(opt('port', process.env.PORT || '8080'), 10);
const HOST = opt('host', '0.0.0.0');
const STATIC = opt('static', '');
const ROOT = STATIC ? path.resolve(STATIC) : '';
const MAX_ROOMS = parseInt(opt('max-rooms', '200'), 10);
const MAX_PLAYERS = parseInt(opt('max-players', '16'), 10);
const MAX_FRAME = 4096;
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const BCAST = 'ffffffffffff';

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.wasm': 'application/wasm',
  '.json': 'application/json', '.css': 'text/css', '.png': 'image/png', '.ico': 'image/x-icon',
  '.ogg': 'audio/ogg', '.mp3': 'audio/mpeg', '.wav': 'audio/wav', '.txt': 'text/plain; charset=utf-8'
};

const rooms = new Map();

function log(s) {
  console.log(new Date().toISOString().replace('T', ' ').slice(0, 19) + '  ' + s);
}

function frame(op, data) {
  const n = data.length;
  let h;
  if (n < 126) {
    h = Buffer.from([0x80 | op, n]);
  } else if (n < 65536) {
    h = Buffer.from([0x80 | op, 126, n >> 8, n & 255]);
  } else {
    h = Buffer.alloc(10);
    h[0] = 0x80 | op;
    h[1] = 127;
    h.writeUInt32BE(Math.floor(n / 4294967296), 2);
    h.writeUInt32BE(n >>> 0, 6);
  }
  return Buffer.concat([h, data]);
}

class Peer {
  constructor(sock, room, node) {
    this.sock = sock;
    this.room = room;
    this.node = node;
    this.nodeBytes = Buffer.from(node, 'hex');
    this.closed = false;
  }
  send(op, data) {
    if (this.closed) return;
    if (this.sock.writableLength > 1 << 20) return;
    try { this.sock.write(frame(op, data)); } catch (e) { this.close(); }
  }
  close() {
    if (this.closed) return;
    this.closed = true;
    try { this.sock.destroy(); } catch (e) { }
  }
}

function announce(room) {
  const peers = rooms.get(room);
  if (!peers) return;
  const msg = Buffer.from('peers ' + peers.size);
  for (const p of peers.values()) p.send(1, msg);
}

function leave(peer) {
  const peers = rooms.get(peer.room);
  if (!peers || peers.get(peer.node) !== peer) return;
  peers.delete(peer.node);
  if (peers.size === 0) rooms.delete(peer.room);
  log('player ' + peer.node + ' left room "' + peer.room + '", players: ' + (peers.size));
  announce(peer.room);
}

function relay(req, sock, head) {
  const key = req.headers['sec-websocket-key'];
  if (!key) { sock.destroy(); return; }
  const url = new URL(req.url, 'http://x');
  let room = (url.searchParams.get('room') || 'lan').slice(0, 64) || 'lan';
  let node = (url.searchParams.get('node') || '').toLowerCase();
  if (!/^[0-9a-f]{12}$/.test(node) || node === BCAST) {
    const b = crypto.randomBytes(6);
    b[0] = 2;
    node = b.toString('hex');
  }
  let peers = rooms.get(room);
  if (!peers) {
    if (rooms.size >= MAX_ROOMS) { sock.end('HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n'); return; }
    peers = new Map();
    rooms.set(room, peers);
  }
  if (!peers.has(node) && peers.size >= MAX_PLAYERS) { sock.end('HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n'); return; }
  const accept = crypto.createHash('sha1').update(key + GUID).digest('base64');
  sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  sock.setNoDelay(true);
  const me = new Peer(sock, room, node);
  const old = peers.get(node);
  peers.set(node, me);
  if (old) old.close();
  const who = req.headers['x-forwarded-for'] || sock.remoteAddress;
  log('player ' + node + ' (' + who + ') joined room "' + room + '", players: ' + peers.size);
  announce(room);

  let buf = head && head.length ? Buffer.from(head) : Buffer.alloc(0);
  sock.on('data', (d) => {
    buf = buf.length ? Buffer.concat([buf, d]) : d;
    for (;;) {
      if (buf.length < 2) return;
      const op = buf[0] & 15, masked = (buf[1] & 128) !== 0;
      let n = buf[1] & 127, off = 2;
      if (n === 126) { if (buf.length < 4) return; n = buf.readUInt16BE(2); off = 4; }
      else if (n === 127) { if (buf.length < 10) return; n = buf.readUInt32BE(6); off = 10; }
      if (n > MAX_FRAME) { me.close(); return; }
      const need = off + (masked ? 4 : 0) + n;
      if (buf.length < need) return;
      const data = Buffer.from(buf.subarray(off + (masked ? 4 : 0), need));
      if (masked) {
        const m = buf.subarray(off, off + 4);
        for (let i = 0; i < n; i++) data[i] ^= m[i & 3];
      }
      buf = buf.subarray(need);
      if (op === 8) { me.close(); return; }
      if (op === 9) { me.send(10, data); continue; }
      if (op !== 2 || n < 6) continue;
      const dst = data.subarray(0, 6).toString('hex');
      const out = Buffer.concat([me.nodeBytes, data.subarray(6)]);
      if (dst === BCAST) {
        for (const p of peers.values()) if (p !== me) p.send(2, out);
      } else {
        const p = peers.get(dst);
        if (p && p !== me) p.send(2, out);
      }
    }
  });
  sock.on('close', () => { me.closed = true; leave(me); });
  sock.on('error', () => { me.close(); });
}

function serveFile(req, res) {
  if (!ROOT) {
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('Carmageddon II relay is running. Players connect to ws(s)://this-host/net\n');
    return;
  }
  let rel;
  try { rel = decodeURIComponent(new URL(req.url, 'http://x').pathname); } catch (e) { res.writeHead(400); res.end(); return; }
  if (rel.endsWith('/')) rel += 'index.html';
  const full = path.join(ROOT, path.normalize(rel));
  if (full !== ROOT && !full.startsWith(ROOT + path.sep)) { res.writeHead(403); res.end(); return; }
  fs.stat(full, (err, st) => {
    if (err || !st.isFile()) { res.writeHead(404); res.end(); return; }
    const ext = path.extname(full).toLowerCase();
    const longCache = /\.data\.\d{3}$/.test(full);
    res.writeHead(200, {
      'Content-Type': MIME[ext] || 'application/octet-stream',
      'Content-Length': st.size,
      'Cache-Control': longCache ? 'public, max-age=2592000, immutable' : 'no-cache',
      'Last-Modified': st.mtime.toUTCString()
    });
    if (req.method === 'HEAD') { res.end(); return; }
    fs.createReadStream(full).pipe(res);
  });
}

const server = http.createServer(serveFile);
server.on('upgrade', (req, sock, head) => {
  const p = new URL(req.url, 'http://x').pathname;
  if (p === '/net' && String(req.headers.upgrade || '').toLowerCase() === 'websocket') relay(req, sock, head);
  else sock.destroy();
});
server.listen(PORT, HOST, () => {
  log('Carmageddon II relay listening on ' + HOST + ':' + PORT + ' (ws path /net)' + (ROOT ? ', serving files from ' + ROOT : ''));
});
