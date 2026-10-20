# Atrium app ↔ Dashboard protocol

**Status: draft v0, for review by the dashboard developer.** Nothing is implemented on either side
yet. Field names follow what the Atrium app already sends its own window (`src/app.rs`
`snapshot()`), so both UIs show the same data.

## Shape

- The **Atrium app** (Rust, always running while VR is in use) is the server. It owns all state:
  library, headsets, runtime, accounts. The dashboard keeps no state of its own.
- The **dashboard** (Unity, OpenXR) is a client. It connects, receives a full **state** snapshot
  after every change, and sends **commands** back. It never touches files, Steam, Meta or the
  runtime directly.
- One transport: **WebSocket over localhost**, text frames, one JSON object per frame (UTF-8).

## Connecting

1. The app listens on `127.0.0.1` only, on a random free port.
2. It writes the connection details to a file only the user can read:
   - Windows: `%APPDATA%\Atrium\dashboard.json`
   - Linux: `~/.config/atrium/dashboard.json` (mode 0600)

   ```json
   { "protocol": 1, "url": "ws://127.0.0.1:49731/dashboard", "token": "c2b1…(32 random bytes, hex)" }
   ```

3. The dashboard reads that file and connects to `url` with the header
   `Authorization: Bearer <token>`.
   - Unity's `ClientWebSocket` supports headers. If that's awkward, `?token=<token>` is accepted
     too.
4. The app checks:
   - **The token.** Anything local, including web pages in a browser, can open a socket to
     localhost, so the token is what keeps other programs out.
   - **No `Origin` header.** Browsers always send one; the dashboard doesn't.

   On failure it closes with code 4401.
5. The token changes every time the app starts, so re-read the file on each (re)connect.
6. If the file is missing or the connection drops, retry every 2 s. The app may not be running
   yet.

## Messages: app → dashboard

Every message has `"type"`.

### `hello` (first message after connecting)

```json
{ "type": "hello", "protocol": 1, "app": "Atrium", "version": "0.1.0" }
```

If `protocol` is higher than the dashboard understands, show "Update the dashboard" and stop.
Minor additions (new fields, new message types) don't bump `protocol`; ignore what you don't know.

### `state` (right after `hello`, then after every change)

A full snapshot, not a diff. Redraw from it.

```json
{
  "type": "state",
  "games": [
    {
      "id": "meta:1369078409873402",
      "source": "meta",
      "name": "Echo VR",
      "installed": true,
      "owned": true,
      "vr": true,
      "vr_only": true,
      "last_played": 1792499950000,
      "size": 9123456789,
      "art": { "cover": "/art/meta%3A1369078409873402/cover", "hero": "/art/…/hero", "wide": null },
      "start": "Through Atrium OVR (OpenXR)"
    }
  ],
  "launching": null,
  "headsets": [
    { "name": "Meta Quest 3", "family": "quest", "vendor": "Meta", "via": "Atrium streaming",
      "active": true, "battery": 0.82 }
  ],
  "controllers": [ { "name": "Left Touch", "via": "Atrium streaming", "battery": 0.6 } ],
  "runtime": { "registered": true, "backend": "Atrium streaming" },
  "accounts": { "steam": "lucas", "meta": ["lucas"] },
  "vr_only": true
}
```

| Field | Meaning |
| --- | --- |
| `games[].id` | Stable id: `steam:<appid>`, `meta:<appid>`, `local:<id>`. Use it in commands. |
| `games[].source` | `steam`, `meta` or `local`. |
| `games[].installed` | `false` = owned but not installed; `launch` opens its store page on the PC. |
| `games[].last_played` | ms since epoch, `0` = never. Sort "Recent" by this. |
| `games[].art.*` | Paths on the art endpoint (below) or `null`. `cover` is square, `hero`/`wide` are landscape. |
| `games[].start` | How it will start, for display only. |
| `launching` | Name of the game being started, or `null`. Disable Play while it's set. |
| `headsets[].family` | Icon key: `rift`, `rifts`, `quest`, `index`, `vive`, `frame`, `wmr`, `pimax`, `beyond`, `generic`. Icons are our own art. |
| `*.battery` | 0 to 1, or absent when unknown. |
| `vr_only` | The user's "VR only" filter in the app; the dashboard may honour it or offer its own. |

### `notice`

A short message for the user, the same ones the app shows as toasts.

```json
{ "type": "notice", "text": "Starting Echo VR" }
```

### `show` / `hide`

The user asked to open or close the dashboard, for example from the Atrium app or a controller
shortcut handled by the runtime.

```json
{ "type": "show" }
```

## Messages: dashboard → app

Commands have `"cmd"`. The names are the same as the Atrium app's own page uses.

| Command | Fields | Does |
| --- | --- | --- |
| `launch` | `id` | Start the game (or open its store page if not installed). |
| `refresh` | | Re-check Steam/Meta libraries. |
| `setVrOnly` | `on` (bool) | Change the VR-only filter. |
| `quitGame` | | Close the running game. *(new; app side to add)* |
| `recenter` | | Recenter the playspace. *(new; runtime side to add)* |
| `ping` | | App answers `{"type":"pong"}`. |

```json
{ "cmd": "launch", "id": "meta:1369078409873402" }
```

Unknown commands are ignored. Account sign-in, adding games and settings stay in the desktop app
(they need a keyboard or browser).

## Art endpoint

Same port, plain HTTP, same token:

```
GET http://127.0.0.1:<port>/art/<game id, URL-encoded>/<cover|hero|wide>
Authorization: Bearer <token>
```

It returns the image (JPEG or PNG; check `Content-Type`), or 404. This saves the dashboard from
reading Meta's or Steam's folders itself. Cache per id + kind for the session.

## Running as an overlay

- The dashboard is an ordinary **OpenXR** app (Unity OpenXR plugin only; no Meta or SteamVR
  plugins).
- On the **Atrium Runtime** (Monado-based: streamed Quest/Pico, Linux) it should enable
  `XR_EXTX_overlay` so it draws over the running game. Check the extension at startup.
- Where the runtime doesn't offer it (the Rift on Meta's backend today), it can only run as a
  normal app, i.e. instead of a game, not over one.
- The app starts the dashboard process and tells it to `show`/`hide`. The dashboard should idle
  cheaply while hidden.

## Open questions for the dashboard side

1. Is a WebSocket fine for Unity here, or would a named pipe be easier? (The JSON stays the
   same either way.)
2. Which commands beyond these does the first version need?
3. Does the dashboard want headset/controller battery and connection quality (latency, bitrate)
   for streamed headsets? The app can add them.
4. Where should controller shortcuts to open the dashboard live: in the runtime (system button)
   or in the dashboard?
