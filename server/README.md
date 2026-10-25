# Carmageddon II Web — server

The server is needed **only for network games**. Single player works on any static host without it.

The server does two things:

1. **Relay** at `/net`: a WebSocket through which the players' browsers exchange the game's network packets. Players connected to the same relay and the same *room* see each other's games in the Network Game menu.
2. **Serving the site** (optional): the same process can serve the `site` folder, so a single server is enough.

## Files

| File | Purpose |
|------|------------|
| `relay.js` | Node.js 16+ server, no dependencies. The main option for hosting. |
| `package.json` | For platforms that run `npm start`. |
| `relay.py` | The same in Python 3.8+, also without dependencies. |
| `serve.ps1`, `serve.cmd` | Windows server with nothing to install (LAN games from a home PC). |
| `Dockerfile` | Image with `relay.js`. |
| `carma2-relay.service` | Example systemd service. |
| `nginx.conf.example` | Example nginx config: HTTPS, serving the site and proxying `/net`. |

## relay.js options

Command-line switches or environment variables:

| Switch | Variable | Default | Effect |
|------|------------|--------------|------------|
| `--port` | `CARMA2_PORT` or `PORT` | `8080` | Port |
| `--host` | `CARMA2_HOST` | `0.0.0.0` | Listen address (`127.0.0.1` when behind nginx) |
| `--static` | `CARMA2_STATIC` | not set | Site folder to serve. Without it only the relay runs. |
| `--max-rooms` | `CARMA2_MAX_ROOMS` | `200` | Room limit |
| `--max-players` | `CARMA2_MAX_PLAYERS` | `16` | Players per room limit |

Check: open `http://address:8080/` in a browser. Without `--static` the server answers with "Carmageddon II relay is running".

## Option 1. One server with Node.js (simplest)

On a VPS (Ubuntu/Debian):

```bash
sudo apt install nodejs
sudo mkdir -p /opt/carma2
# copy the site and server folders to /opt/carma2
node /opt/carma2/server/relay.js --port 8080 --static /opt/carma2/site
```

The game opens at `http://SERVER-IP:8080/`, network play works right away.

To start the server automatically and restart it on failure:

```bash
sudo cp /opt/carma2/server/carma2-relay.service /etc/systemd/system/
sudo nano /etc/systemd/system/carma2-relay.service
sudo systemctl daemon-reload
sudo systemctl enable --now carma2-relay
journalctl -u carma2-relay -f
```

In `nano`, add `--static /opt/carma2/site` to the `ExecStart` line if the relay should serve the site. If nginx serves the site (option 2), leave it as it is.

## Option 2. nginx + HTTPS (recommended for a public site)

When the site is opened over `https://`, the browser only allows `wss://` connections, so the relay is put behind nginx with a certificate:

1. Copy `site` to `/opt/carma2/site` and `server` to `/opt/carma2/server`.
2. Run the relay as a service (see option 1). It listens on `127.0.0.1:8080`.
3. Take `nginx.conf.example`, replace `carma.example.com` with your domain and put it into `/etc/nginx/sites-available/`. Enable it and get a certificate: `sudo certbot --nginx -d your.domain`.
4. `sudo nginx -t && sudo systemctl reload nginx`.

nginx serves the site and passes `/net` to the relay. No changes to `config.js` are needed: the game connects to `wss://your.domain/net` by itself.

## Option 3. Site on a static host, relay elsewhere

The site (`site` folder) can be put on any static host: regular web hosting, Cloudflare Pages, Netlify, GitHub Pages, etc. The game data is split into 20 MB parts to fit file size limits.

The relay runs anywhere with Node.js and WebSocket support: a VPS, Render, Railway, Fly.io, etc. For platforms that run `npm start` and set `PORT` themselves, uploading the `server` folder is enough.

Then set the relay address in `site/config.js`:

```js
window.CARMA2_RELAY = 'wss://relay.example.com/net';
```

If the site is opened over `https://`, the relay address must start with `wss://`.

## Option 4. Docker

```bash
cd server
docker build -t carma2-relay .
docker run -d --restart=always -p 8080:8080 carma2-relay
docker run -d --restart=always -p 8080:8080 -v /opt/carma2/site:/site:ro -e CARMA2_STATIC=/site carma2-relay
```

The first `docker run` starts the relay only, the second one the relay plus the site. Pick one.

## Option 5. Home PC with Windows (LAN)

Keep the `site` and `server` folders side by side and run `server\serve.cmd`. It finds `..\site` automatically, opens the game in the browser and prints the addresses for other computers on the network (e.g. `http://192.168.1.10:8080/`). If Windows asks about the firewall, allow access on private networks.

The script can serve another site folder: `powershell -ExecutionPolicy Bypass -File serve.ps1 8080 D:\path\to\site`.

To play over the internet from a home PC, forward port 8080 on your router to this computer and give your friends your external IP.

## How to play a network game

1. All players open the same site (or sites with the same `CARMA2_RELAY`).
2. In the game: race selection menu, **Network Game** at the top.
3. One player chooses the game type and track and presses **Host**.
4. The others see the game in the **Join a Game** list, select it and press **Join**.
5. When everybody is ready, the host presses **Start**.

The driver name is changed in the **Driver** field of the same screen.

### Rooms

By default all players of one relay are in the room `lan` and see each other's games. To play as a separate group, add `?room=name` to the site address, e.g. `https://your.domain/?room=friday`. Only players with the same room are visible.

Other URL parameters: `?name=Name` — default network name, `?relay=wss://...` — use another relay for this session, `?netlog` — log network packets to the browser console.

## Protocol (for developers)

WebSocket `GET /net?room=ROOM&node=XXXXXXXXXXXX`, where `node` is 12 hex digits, the player's address (IPX node). Messages are binary:

* client → server: `[6 bytes: destination node][payload]`; `ff:ff:ff:ff:ff:ff` = everybody in the room;
* server → client: `[6 bytes: source node][payload]`;
* text message `peers N` — number of players in the room.

The server knows nothing about the game: it only forwards packets within a room and stores no data.
