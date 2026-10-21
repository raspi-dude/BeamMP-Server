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

Put a TLS reverse proxy (Caddy, nginx, Traefik) in front: game servers should
talk HTTPS, because the pairing code is a bearer secret.

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

`POST /v1/relay/send` — game to Discord:

```json
{ "code": "ABCDEFGHJK", "name": "Player1", "message": "hello" }
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

- Bot messages, webhooks and messages without a guild are ignored, which stops
  the `game -> Discord -> game` loop.
- Mention parsing is disabled on send, so player text cannot ping `@everyone`.
- Messages are capped at 400 characters and newlines are flattened, matching the
  game's 500-byte chat packet limit.
- Each pairing buffers the last 200 messages in RAM; long-poll clients are woken
  directly on publish.
