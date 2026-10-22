# Raspi Host Discord relay

Central service that bridges many BeamMP servers to Discord through **one** bot
connection. Game servers never hold a Discord bot token: they only know a
pairing code for one channel, which is issued by `/pair` in Discord.

```
 Discord ──(Gateway WebSocket)── bot ──┐
                                       ├─ RelayCore ──(HTTP long-poll)── game servers
 Discord <──(REST send)──────── channel ┘
```

## Why central instead of a bot per server

- One Gateway connection and one token for the whole hosting box. Discord allows
  only one Gateway session per token/shard, so a per-server bot would need a
  token per customer.
- Rate limits stay in one place: bots share buckets per token, webhooks and
  polling per customer would multiply requests from a single IP.
- Customers only copy a short code into `RaspiConfig.toml`.

## Setup

1. Create an application at <https://discord.com/developers/applications>, add a
   bot, and copy its token.
2. In the Bot settings, enable **MESSAGE CONTENT INTENT**. That is the only
   privileged intent this bot uses; presence and server-members intents can stay
   **off**. Without it, login fails with `Used disallowed intents`.
3. Invite the bot with this URL (`client_id` = your application id):

   ```
   https://discord.com/oauth2/authorize?client_id=YOUR_APP_ID&permissions=68608&scope=bot%20applications.commands
   ```

   That is the whole permission set. Add `274877906944` to the number if you
   also want to pair threads (`68608 | Send Messages in Threads` =
   `274877975552`). `bot` must be checked in the invite; without
   `applications.commands` the slash commands never appear.
4. Run the relay:

```bash
cd relay
npm install
npm start
```

### Bot permissions

| Permission | Why |
|---|---|
| View Channels | See the paired channel |
| Send Messages | Relay game chat into it |
| Read Message History (optional) | Not required by the code; harmless |
| Send Messages in Threads (optional) | Only if a thread is paired |

Nothing else is needed — no Administrator, no Manage Messages, no Manage
Channels, no Manage Server, no webhooks, no voice. **Manage Server is checked on
the person running `/pair`, not granted to the bot**: the bot reads the
interaction's `memberPermissions` and never needs the permission itself. The bot
never deletes messages, so leave `Manage Messages` off (that also keeps its
blast radius small if the token leaks).

Granting `Administrator` works but is unnecessary: it lets a leaked token nuke
the server.

The token is read from `DISCORD_TOKEN` if set, otherwise from `relay/src/token.txt`
(one line, just the token). `token.txt` is git-ignored — keep it that way, and
`chmod 600` it on a shared box.

Environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `DISCORD_TOKEN` | `src/token.txt` | Bot token |
| `PORT` | `8099` | HTTP port for game servers |
| `HOST` | `0.0.0.0` | Bind address |
| `DATA_FILE` | `data/pairings.json` | Persisted pairings (hashes only, no codes) |
| `DISCORD_GUILD_ID` | unset | Register slash commands in one guild (instant, for testing) |
| `RELAY_PUBLIC_URL` | `config.json` | Overrides `publicUrl` |
| `RELAY_ALLOWED_IPS` | `config.json` | Overrides `allowedIps`, comma separated |
| `RELAY_TRUST_PROXY` | `config.json` | Overrides `trustProxy` (`true`/`false`); needed behind any reverse proxy |

## Configuration (`relay/config.json`)

```json
{
  "http": { "host": "0.0.0.0", "port": 8099 },
  "publicUrl": "https://relay.example.com",
  "allowedIps": ["203.0.113.7", "198.51.100.0/24"],
  "trustProxy": false,
  "rateLimit": {
    "perPairing": { "windowSeconds": 10, "maxMessages": 30 },
    "perIp": { "windowSeconds": 60, "maxRequests": 600 }
  },
  "dataFile": "data/pairings.json"
}
```

| Key | Meaning |
|---|---|
| `publicUrl` | The URL `/pair` prints for `RelayUrl`. Without it the reply contains a placeholder. |
| `allowedIps` | IPs or CIDR blocks allowed to use the relay. **Empty means any IP may connect** and the relay logs a warning at startup. IPv4-mapped addresses (`::ffff:1.2.3.4`) and `::1` are normalised, so list plain IPv4. |
| `trustProxy` | Honour `X-Forwarded-For` / `X-Real-IP`. Only enable behind a proxy you control — otherwise a client can claim any IP and walk past `allowedIps`. Only the **rightmost** `X-Forwarded-For` entry is used, i.e. the one the proxy appended, so a client-supplied header cannot spoof the address. |
| `rateLimit.perPairing` | Game → Discord messages per pairing code. Sliding window, so no boundary bursts. |
| `rateLimit.perIp` | All relay requests per client IP; this is what a long-poll budget is drawn from. |
| `dataFile` | Where pairings are persisted (hashes only). |

A malformed `allowedIps` entry (a hostname, or `/64` on an IPv4 address) aborts startup with a clear message instead of silently locking everyone out.

### What a blocked server sees

The relay answers with a machine-readable error and a human sentence, and the
game server prints the same thing in its console (`discord` command shows it as
`last error`):

| Status | Error | Meaning |
|---|---|---|
| 403 | `ip_not_allowed` | The game server's IP is not in `allowedIps`. |
| 429 | `rate_limited` | Over the per-pairing or per-IP limit; `retryAfter` says how long. |
| 401 | `invalid pairing code` | Code unknown or revoked; run `/pair` again. |
| 502 | `discord_send_failed` | The bot cannot post in that channel (missing, or no permission). |

Put a TLS reverse proxy (Caddy, nginx, Traefik) in front: game servers should
talk HTTPS, because the pairing code is a bearer secret.

### Reverse proxy (nginx, incl. Nginx Proxy Manager)

Two settings are needed, one on each side:

1. `relay/config.json`: `"trustProxy": true` (or `RELAY_TRUST_PROXY=true`). The
   startup log then prints `trust proxy headers: yes`. Without it the relay
   only ever sees the proxy's address, `127.0.0.1`, and the lock is meaningless.
2. The proxy must forward the peer address. nginx does this by default with
   `X-Real-IP` and `X-Forwarded-For`; Nginx Proxy Manager's built-in
   `proxy.conf` already sends both, so **no Advanced-tab config is required**.

Nginx Proxy Manager, add a Proxy Host:

| Field | Value |
|---|---|
| Domain Names | `relay.example.com` |
| Scheme | `http` |
| Forward Hostname/IP | `127.0.0.1` (NPM in Docker, relay on the host: use `host.docker.internal` or the Docker bridge IP) |
| Forward Port | `8099` |
| Block Common Exploits | on |
| Websockets Support | not needed (long-polling is plain HTTP) |

Then the SSL tab: request a Let's Encrypt certificate and turn on **Force SSL**.

Details that matter here:

- NPM appends with `$proxy_add_x_forwarded_for`, so a client-supplied
  `X-Forwarded-For` stays on the **left**. The relay reads the rightmost entry,
  which is why a forged header cannot claim an allowlisted IP. Setting
  `proxy_set_header X-Forwarded-For $remote_addr` in the Advanced tab does not
  work in NPM: the template's own headers live in the `location /` block, and
  server-level directives are not inherited once a location sets its own.
- nginx's default `proxy_read_timeout` is 60 s and the relay's longest poll is
  20 s, so long-polls survive untouched. No timeout tuning needed.
- **Never expose port 8099 publicly.** With `trustProxy: true` the header is the
  only source of the client IP, so anything that can reach the relay directly
  can forge it. Bind it to `127.0.0.1` (`"http": { "host": "127.0.0.1" }`) or
  firewall the port to the proxy only. Inside Docker, keep NPM and the relay on
  a private network for the same reason.
- Behind Cloudflare as well, NPM forwards Cloudflare's address rather than the
  player's. Add Cloudflare's ranges in the Advanced tab so `$remote_addr` is
  corrected first, otherwise every game server appears as one IP and the per-IP
  limit is shared by all of them:

  ```nginx
  set_real_ip_from 173.245.48.0/20;
  set_real_ip_from 103.21.244.0/22;
  # ...remaining ranges from https://www.cloudflare.com/ips/
  real_ip_header CF-Connecting-IP;
  ```

- Do not use NPM's Access Lists for the game-server lock: they answer with an
  HTML 403 before the relay is reached, so the server log cannot say why. The
  relay's own `allowedIps` returns `ip_not_allowed` with the IP it saw.

Verify from a machine that is *not* allowlisted — the 403 body echoes the
address the relay resolved, which should be that machine's public IP and not
`127.0.0.1`:

```bash
curl -s https://relay.example.com/v1/relay/send \
  -H 'content-type: application/json' \
  -d '{"code":"NOPE","name":"x","message":"y"}'
# {"ok":false,"error":"ip_not_allowed","message":"... Your IP (198.51.100.9) ..."}
```

## Pairing

In the Discord channel's server, someone with **Manage Server** runs:

```
/pair channel:#game-chat label:MyServer
```

The bot replies with a code. That code goes into the game server's config:

```toml
[Discord]
Enabled = true
RelayUrl = "https://relay.example.com"
PairingCode = "ABCDEFGHJK"
```

`/unpair channel:#game-chat` revokes the code; re-pairing rotates it.

## Protocol

Both endpoints are `POST` with a JSON body containing the `code`.

`POST /v1/relay/send` — game to Discord. Player chat:

```json
{ "code": "ABCDEFGHJK", "name": "Player1", "message": "hello" }
```

Or a system line (server started, a player joined, ...) which is posted without
a player prefix:

```json
{ "code": "ABCDEFGHJK", "system": true, "message": "**Server started**" }
```

`POST /v1/relay/poll` — Discord to game, long-polls up to `wait` seconds:

```json
{ "code": "ABCDEFGHJK", "cursor": 0, "wait": 20 }
```

Response:

```json
{ "ok": true, "cursor": 12, "messages": [{ "id": 12, "name": "SomeUser", "message": "hi" }] }
```

The client sends back the last `cursor` it saw, so messages are never replayed
(or missed) across reconnects. A poll with no new messages blocks until a
message arrives or the wait window expires, so an idle server costs no traffic.

## Behaviour notes

- Two threads on the game server: one sends, one long-polls. The poll blocks for
  the whole window, so sharing a thread meant chat waited behind it (this was
  the "server to Discord takes forever" bug).
- Every relayed Discord message is also printed in the server console as
  `[Discord] Name: message`, so server owners can follow Discord without
  alt-tabbing.
- Server start/stop and player join/leave are posted as system lines. Turn them
  off with `RelayEvents = false` in `RaspiConfig.toml`.
- Bot messages, webhooks and messages without a guild are ignored, which stops
  the `game -> Discord -> game` loop.
- Mention parsing is disabled on send, so player text cannot ping `@everyone`.
- Messages are capped at 400 characters and newlines are flattened, matching the
  game's 500-byte chat packet limit.
- Each pairing buffers the last 200 messages in RAM; long-poll clients are woken
  directly on publish.
