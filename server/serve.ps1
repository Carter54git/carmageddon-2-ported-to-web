param([int]$Port = 8080, [string]$Root = '')
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Root -eq '') {
  $site = Join-Path (Split-Path -Parent $here) 'site'
  if (Test-Path (Join-Path $site 'index.html')) { $Root = $site } else { $Root = $here }
}
$root = [IO.Path]::GetFullPath($Root)

$src = @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

public class Carma2Client {
    TcpClient tcp;
    NetworkStream stream;
    bool closed;
    public string Node;
    public string Room;

    public Carma2Client(TcpClient c, NetworkStream s, string node, string room) {
        tcp = c;
        stream = s;
        Node = node;
        Room = room;
    }

    public void Send(int op, byte[] data) {
        lock (this) {
            if (closed) return;
            try {
                int n = data.Length;
                byte[] h;
                if (n < 126) {
                    h = new byte[2];
                    h[1] = (byte)n;
                } else if (n < 65536) {
                    h = new byte[4];
                    h[1] = 126;
                    h[2] = (byte)(n >> 8);
                    h[3] = (byte)(n & 255);
                } else {
                    h = new byte[10];
                    h[1] = 127;
                    long ln = n;
                    for (int i = 0; i < 8; i++) h[9 - i] = (byte)((ln >> (8 * i)) & 255);
                }
                h[0] = (byte)(0x80 | op);
                byte[] f = new byte[h.Length + n];
                Buffer.BlockCopy(h, 0, f, 0, h.Length);
                Buffer.BlockCopy(data, 0, f, h.Length, n);
                stream.Write(f, 0, f.Length);
            } catch (Exception) {
                closed = true;
                try { tcp.Close(); } catch (Exception) { }
            }
        }
    }

    public void Close() {
        lock (this) {
            closed = true;
            try { tcp.Close(); } catch (Exception) { }
        }
    }
}

public class Carma2Server {
    static string root;
    static Dictionary<string, Dictionary<string, Carma2Client>> rooms = new Dictionary<string, Dictionary<string, Carma2Client>>();
    static object roomLock = new object();
    static Random rnd = new Random();

    public static void Start(string dir, int port) {
        root = Path.GetFullPath(dir);
        if (!root.EndsWith(Path.DirectorySeparatorChar.ToString())) root = root + Path.DirectorySeparatorChar;
        TcpListener listener = new TcpListener(IPAddress.Any, port);
        listener.Start();
        Thread t = new Thread(new ParameterizedThreadStart(AcceptLoop));
        t.IsBackground = true;
        t.Start(listener);
    }

    static void AcceptLoop(object o) {
        TcpListener listener = (TcpListener)o;
        while (true) {
            TcpClient c = null;
            try {
                c = listener.AcceptTcpClient();
            } catch (Exception) {
                Thread.Sleep(50);
                continue;
            }
            Thread t = new Thread(new ParameterizedThreadStart(Handle));
            t.IsBackground = true;
            t.Start(c);
        }
    }

    static void Log(string s) {
        try { Console.WriteLine(DateTime.Now.ToString("HH:mm:ss") + "  " + s); } catch (Exception) { }
    }

    static string ReadHeaders(NetworkStream s) {
        StringBuilder sb = new StringBuilder();
        while (sb.Length < 16384) {
            int b = s.ReadByte();
            if (b < 0) return null;
            sb.Append((char)b);
            int n = sb.Length;
            if (n >= 4 && sb[n - 4] == '\r' && sb[n - 3] == '\n' && sb[n - 2] == '\r' && sb[n - 1] == '\n') return sb.ToString();
        }
        return null;
    }

    static void Handle(object o) {
        TcpClient c = (TcpClient)o;
        try {
            c.NoDelay = true;
            NetworkStream s = c.GetStream();
            string head = ReadHeaders(s);
            if (head != null) {
                string[] lines = head.Split(new string[] { "\r\n" }, StringSplitOptions.None);
                string[] req = lines[0].Split(' ');
                if (req.Length >= 2) {
                    Dictionary<string, string> h = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                    for (int i = 1; i < lines.Length; i++) {
                        int k = lines[i].IndexOf(':');
                        if (k > 0) h[lines[i].Substring(0, k).Trim()] = lines[i].Substring(k + 1).Trim();
                    }
                    string target = req[1];
                    string path = target;
                    string query = "";
                    int q = target.IndexOf('?');
                    if (q >= 0) {
                        path = target.Substring(0, q);
                        query = target.Substring(q + 1);
                    }
                    string upgrade;
                    if (path == "/net" && h.TryGetValue("Upgrade", out upgrade) && upgrade.ToLowerInvariant() == "websocket") {
                        Relay(c, s, h, query);
                    } else {
                        ServeFile(c, s, req[0], Uri.UnescapeDataString(path));
                    }
                }
            }
        } catch (Exception) {
        }
        try { c.Close(); } catch (Exception) { }
    }

    static string Mime(string file) {
        string e = Path.GetExtension(file).ToLowerInvariant();
        switch (e) {
            case ".html": return "text/html; charset=utf-8";
            case ".js": return "text/javascript";
            case ".wasm": return "application/wasm";
            case ".json": return "application/json";
            case ".css": return "text/css";
            case ".png": return "image/png";
            case ".ico": return "image/x-icon";
            case ".ogg": return "audio/ogg";
            case ".mp3": return "audio/mpeg";
            case ".wav": return "audio/wav";
            case ".txt": return "text/plain; charset=utf-8";
            default: return "application/octet-stream";
        }
    }

    static void ServeFile(TcpClient c, NetworkStream s, string method, string path) {
        string rel = path.TrimStart('/');
        if (rel.Length == 0) rel = "index.html";
        string full = null;
        try {
            full = Path.GetFullPath(Path.Combine(root, rel.Replace('/', Path.DirectorySeparatorChar)));
        } catch (Exception) {
            full = null;
        }
        if (full == null || !full.StartsWith(root, StringComparison.OrdinalIgnoreCase) || !File.Exists(full)) {
            byte[] nf = Encoding.ASCII.GetBytes("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s.Write(nf, 0, nf.Length);
        } else {
            using (FileStream f = new FileStream(full, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
                string hdr = "HTTP/1.1 200 OK\r\nContent-Type: " + Mime(full) + "\r\nContent-Length: " + f.Length.ToString() +
                             "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
                byte[] hb = Encoding.ASCII.GetBytes(hdr);
                s.Write(hb, 0, hb.Length);
                if (method != "HEAD") {
                    byte[] buf = new byte[65536];
                    int r;
                    while ((r = f.Read(buf, 0, buf.Length)) > 0) s.Write(buf, 0, r);
                }
            }
        }
        s.Flush();
        try { c.Client.Shutdown(SocketShutdown.Send); } catch (Exception) { }
    }

    static bool ReadFull(NetworkStream s, byte[] buf, int off, int n) {
        while (n > 0) {
            int r = s.Read(buf, off, n);
            if (r <= 0) return false;
            off += r;
            n -= r;
        }
        return true;
    }

    static bool IsHex(string v) {
        foreach (char ch in v) {
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
        }
        return true;
    }

    static string Hex(byte[] b, int off, int n) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) sb.Append(b[off + i].ToString("x2"));
        return sb.ToString();
    }

    static byte[] Unhex(string v) {
        byte[] b = new byte[v.Length / 2];
        for (int i = 0; i < b.Length; i++) b[i] = Convert.ToByte(v.Substring(i * 2, 2), 16);
        return b;
    }

    static void Announce(string room) {
        List<Carma2Client> all = new List<Carma2Client>();
        lock (roomLock) {
            Dictionary<string, Carma2Client> peers;
            if (rooms.TryGetValue(room, out peers)) all.AddRange(peers.Values);
        }
        byte[] msg = Encoding.ASCII.GetBytes("peers " + all.Count.ToString());
        foreach (Carma2Client p in all) p.Send(1, msg);
    }

    static void Relay(TcpClient c, NetworkStream s, Dictionary<string, string> h, string query) {
        string key;
        if (!h.TryGetValue("Sec-WebSocket-Key", out key)) return;
        string accept;
        using (SHA1 sha = SHA1.Create()) {
            accept = Convert.ToBase64String(sha.ComputeHash(Encoding.ASCII.GetBytes(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));
        }
        byte[] resp = Encoding.ASCII.GetBytes("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
        s.Write(resp, 0, resp.Length);

        string room = "lan";
        string node = null;
        foreach (string kv in query.Split('&')) {
            int e = kv.IndexOf('=');
            if (e < 0) continue;
            string k = kv.Substring(0, e);
            string v = Uri.UnescapeDataString(kv.Substring(e + 1).Replace('+', ' '));
            if (k == "room" && v.Length > 0) room = v.Length > 64 ? v.Substring(0, 64) : v;
            if (k == "node") node = v.ToLowerInvariant();
        }
        if (node == null || node.Length != 12 || !IsHex(node)) {
            byte[] rb = new byte[6];
            lock (rnd) rnd.NextBytes(rb);
            rb[0] = 2;
            node = Hex(rb, 0, 6);
        }
        byte[] nodeBytes = Unhex(node);
        Carma2Client me = new Carma2Client(c, s, node, room);
        Carma2Client old = null;
        Dictionary<string, Carma2Client> peers;
        int count;
        lock (roomLock) {
            if (!rooms.TryGetValue(room, out peers)) {
                peers = new Dictionary<string, Carma2Client>();
                rooms[room] = peers;
            }
            if (peers.TryGetValue(node, out old)) peers.Remove(node);
            peers[node] = me;
            count = peers.Count;
        }
        if (old != null) old.Close();
        string who = ((IPEndPoint)c.Client.RemoteEndPoint).Address.ToString();
        Log("player " + node + " (" + who + ") joined room '" + room + "', players: " + count.ToString());
        Announce(room);

        byte[] hdr = new byte[10];
        byte[] mask = new byte[4];
        try {
            while (true) {
                if (!ReadFull(s, hdr, 0, 2)) break;
                int op = hdr[0] & 15;
                bool masked = (hdr[1] & 128) != 0;
                long n = hdr[1] & 127;
                if (n == 126) {
                    if (!ReadFull(s, hdr, 2, 2)) break;
                    n = (hdr[2] << 8) | hdr[3];
                } else if (n == 127) {
                    if (!ReadFull(s, hdr, 2, 8)) break;
                    n = 0;
                    for (int i = 0; i < 8; i++) n = (n << 8) | hdr[2 + i];
                }
                if (n < 0 || n > (1 << 20)) break;
                if (masked && !ReadFull(s, mask, 0, 4)) break;
                byte[] data = new byte[(int)n];
                if (n > 0 && !ReadFull(s, data, 0, (int)n)) break;
                if (masked) {
                    for (int i = 0; i < data.Length; i++) data[i] = (byte)(data[i] ^ mask[i & 3]);
                }
                if (op == 8) break;
                if (op == 9) {
                    me.Send(10, data);
                    continue;
                }
                if (op != 2 || data.Length < 6) continue;
                bool bcast = true;
                for (int i = 0; i < 6; i++) {
                    if (data[i] != 0xff) bcast = false;
                }
                string dst = bcast ? null : Hex(data, 0, 6);
                byte[] outb = new byte[data.Length];
                Buffer.BlockCopy(nodeBytes, 0, outb, 0, 6);
                Buffer.BlockCopy(data, 6, outb, 6, data.Length - 6);
                List<Carma2Client> targets = new List<Carma2Client>();
                lock (roomLock) {
                    if (bcast) {
                        foreach (Carma2Client p in peers.Values) {
                            if (p != me) targets.Add(p);
                        }
                    } else {
                        Carma2Client p;
                        if (peers.TryGetValue(dst, out p) && p != me) targets.Add(p);
                    }
                }
                foreach (Carma2Client p in targets) p.Send(2, outb);
            }
        } catch (Exception) {
        }
        lock (roomLock) {
            Carma2Client cur;
            if (peers.TryGetValue(node, out cur) && cur == me) peers.Remove(node);
            count = peers.Count;
        }
        me.Close();
        Log("player " + node + " left room '" + room + "', players: " + count.ToString());
        Announce(room);
    }
}
'@

$started = $false
try {
  if (-not ('Carma2Server' -as [type])) { Add-Type -TypeDefinition $src -Language CSharp -IgnoreWarnings }
  [Carma2Server]::Start($root, $Port)
  $started = $true
} catch {
  Write-Host ("Network server could not start: " + $_.Exception.Message) -ForegroundColor Yellow
  Write-Host "Falling back to a local-only server (no network games)." -ForegroundColor Yellow
}

if ($started) {
  Write-Host ""
  Write-Host "Carmageddon II web is running." -ForegroundColor Green
  Write-Host "  On this computer:        http://localhost:$Port/"
  try {
    $ips = [System.Net.Dns]::GetHostAddresses([System.Net.Dns]::GetHostName()) | Where-Object { $_.AddressFamily -eq 'InterNetwork' -and -not $_.ToString().StartsWith('169.254.') }
    foreach ($ip in $ips) { Write-Host "  From other computers:    http://$($ip):$Port/" }
  } catch { }
  Write-Host "Network game: everybody opens one of these addresses, one player hosts, the others join."
  Write-Host "(If Windows asks about the firewall, allow access for private networks.)"
  Write-Host "Close this window to stop the server."
  Start-Process "http://localhost:$Port/index.html"
  while ($true) { Start-Sleep -Seconds 1 }
}

$mime = @{ '.html'='text/html; charset=utf-8'; '.js'='text/javascript'; '.wasm'='application/wasm'; '.data'='application/octet-stream';
           '.json'='application/json'; '.css'='text/css'; '.png'='image/png'; '.ico'='image/x-icon'; '.ogg'='audio/ogg'; '.mp3'='audio/mpeg'; '.wav'='audio/wav' }
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
Write-Host "Carmageddon II web: http://localhost:$Port/  (Ctrl+C to stop)"
Start-Process "http://localhost:$Port/index.html"
try {
  while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    $path = [Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath.TrimStart('/'))
    if ($path -eq '') { $path = 'index.html' }
    $file = Join-Path $root $path
    $res = $ctx.Response
    try {
      if ((Test-Path $file -PathType Leaf) -and ([IO.Path]::GetFullPath($file).StartsWith($root))) {
        $ext = [IO.Path]::GetExtension($file).ToLower()
        $res.ContentType = $(if ($mime.ContainsKey($ext)) { $mime[$ext] } else { 'application/octet-stream' })
        $res.Headers.Add('Cache-Control', 'no-cache')
        $fs = [IO.File]::OpenRead($file)
        $res.ContentLength64 = $fs.Length
        if ($ctx.Request.HttpMethod -ne 'HEAD') { $fs.CopyTo($res.OutputStream) }
        $fs.Close()
      } else { $res.StatusCode = 404 }
    } catch { } finally { try { $res.OutputStream.Close() } catch { } }
  }
} finally { $listener.Stop() }
