# dRPC5 — Discord Rich Presence for PS5 via Direct Gateway

A PlayStation 5 homebrew payload that connects to discord gateway and publishes rich presence

> [!WARNING]
> This project uses a self-bot style approach to update RPC status. It **is**
> against Discord's TOS, but is very rarely enforced. Use at your own risk.

> [!NOTE]
> I am not responsible for any damages or consequences directly caused by this
> software, including but not limited to Discord account termination, PS5 system
> software corruption, data loss, or anything else arising from its use.

---
## TLDR
A homebrew payload for the Playstation 5 that uses a User Token to publish Rich Presence over discords Gateway.

---

## What it does

* Connects straight to Discord's gateway from the console and publishes Rich
  Presence, no PC host/ bridge is needed.
* Identifies as a PS5 console session, so the discord account reports as active on
  console (see [Console identity](#console-identity)).
* Works out what's running by walking the console's own process list
  (`sysctl KERN_PROC_PROC`) rather than asking PSN what you're playing, then
  fetches box art for that title from PSN.
* Manually set activity text, assets and timestamps when you don't want
  auto-detection.
* The console-side WebUI is avaiable at `http://localhost:8642`.
  * The PC-Side WebUI is available at `http://<consoleIP>:8642/pc.html`, any attempt to navigate to the console-side WebUI from a non-localhost IP (not the console) will be re-directed to pc.html.
* Sign-in by user token, email + password, or QR code via Discord Remote Auth
  (`wss://remote-auth-gateway.discord.gg`). `/pc.html` is reachable from another 8642
  device on the network for this.
   * **Work in progress:** the QR code and email + password flows are not reliable
  yet and **WILL** fail partway through. Pasting a user token is the only working
  option for now.

## Console identity

This was probably the hardest part of recent development.

On a stock PS5, when logging in with Discord and trusting the application, your discord profile reports that you are on console.
To achive this on a non-stock PS5 (normal discord integration not available) we need to do a bit of work.
If use [lanyard](https://github.com/Phineas/lanyard) to get our Discord Status on our profile, the reponse looks a bit like:
```json
  {
  "data": {
    "kv": {},
      ...
      },
      ...
      },
      ...
      },
      ...
      },
      ...
    },
    "activities": [
      {
      ...
      },
      {
      ...
      },
      {
      ...
      }
    ],
    "discord_status": "online",
    "active_on_discord_web": true,
    "active_on_discord_desktop": false,
    "active_on_discord_mobile": false,
    "active_on_discord_embedded": false,
    /* The above, 'active_on_discord_embedded' is what we want to be true */
    "active_on_discord_vr": false,
    "listening_to_spotify": false,
    "spotify": null
  },
  "success": true
  }
```
Now in order for us to flip that `active_on_discord_embedded` flag to `true` we need to reverse how the stock PS5 talks to the gateway.
On PS5 there exists a file known as a **self**, what that stands for? No clue, but I digress, to figure out the stock PS5 behavior, we need to
decrypt that **self** file, using a tool such as [ps5-self-pager](https://github.com/idlesauce/ps5-self-pager) we can do just that, after running
that tool we get the raw executable the PS5 uses for Discord-related stuff, located in the dump at `/system_ex/common_ex/lib/Sce.Vsh.DiscordAccessor.dll.sprx`

Reverse engineering that file results in the properties it sends to the gateway:
```json
"properties": {
  "os": "Playstation",
  "browser": "PS5 GameBase",
  "version": "1.00"
}
```
If we now send that information through to the websocket that we open to communicate with Discord, puts the user whos token is saved in the `console` bucket, which then sets the `active_on_discord_embedded` flag to true if you send a request to lanyard.

## Prebuilt binaries

Prebuilt payloads are on the
[releases page](https://github.com/pLabs5/dRPC5/releases). Each release is two
files:

| File | |
|---|---|
| `dRPC5.elf` | the payload. Tile package, web UI and CA bundle are all inside it |
| `dRPC5.elf.sha256` | checksum, in `sha256sum` output format |

Verify, then [deploy](#deploying):

```sh
sha256sum -c dRPC5.elf.sha256
```

No Payload SDK, `third_party/` or `dist/` needed. Deploying needs one small
host program (`make send-payload`) and a C compiler — nothing else, and no
cross-toolchain. Unsigned, for your own console.

<details>
<summary>Updating</summary>

The tile installs once, guarded by `/user/appmeta/DRPC00001`, so a new payload
updates the code but leaves the old icon. Force a reinstall with an elf that deletes the above mentioned path or via FTP. 

</details>

## Building

Requires the [Payload SDK](https://github.com/ps5-payload-dev/sdk) and a host C
compiler. Everything else, including the tile-packaging dependency, is in the
repository:
  ```sh
  payldSDK=$(mktemp -d)
  payloadSDK=$(mktemp -d)
  dRPC5=$(mktemp -d)
  git clone https://github.com/ps5-payload-dev/sdk.git "$payldSDK"
  cd "$payldSDK" 
  (install build deps for Payload SDK: Clang 18, LDD 18, LLVM / LLVM-config, meson, pyelftool )
  # Makes the PS5 Payload SDK used for Building
  make clean && make DESTDIR="$payloadSDK"
  git clone https://github.com/pLabs5/dRPC5.git "$dRPC5" 
  cd "$dRPC5"
  export PS5_PAYLOAD_SDK="$payloadSDK"
  make clean && make
  ```

</details>

Produces `dRPC5.elf` and `dist/drpc5-tile.pkg`. Host build tools are compiled
with `HOSTCC` (default `cc`), never the PS5 cross-compiler — override with
`make HOSTCC=clang` if you prefer.

The two host tools are plain C with no dependencies beyond libc:

- `tools/embed.c` turns the CA bundle, web assets and tile package into C arrays
- `tools/mk_tile_pkg.c` `dlopen`s `tools/lib/libprosperopkg.so` to build the tile

<details>
<summary>Host toolchain requirements</summary>

`libprosperopkg.so` is a .NET assembly, so it needs a .NET runtime that matches
the OpenSSL build it was compiled against (3.5.x). On NixOS the Makefile locates
a matching `openssl-3.5` store path and sets `LD_LIBRARY_PATH` for you. The
tools are loaded at runtime with `dlopen`, so there is no link-time dependency
and no .NET SDK is needed to build dRPC5 itself.

</details>

<details>
<summary>NixOS: <code>No usable version of libssl was found</code></summary>

The bundled `libprosperopkg` build picks an OpenSSL directory by glob, and on
NixOS that happily matches `openssl-*-dev/lib`, which contains headers but no
`libssl.so`. The Makefile filters candidate directories for an actual
`libssl.so*`; if you hit this on another distro, that's the line to look at.
</details>

## Deploying

Build the sender once — it replaces the SDK's `prospero-deploy`, which was just
a shell wrapper around `socat`, so no networking tool is needed:

```sh
make send-payload
```

Then send the payload to the console's ELF loader over TCP:

```sh
make deploy PS5_HOST=<ps5-ip> PS5_PORT=9021
```

`tools/send-payload` takes the same arguments (`-h`, `-p`, `-i`), also reads
`PS5_HOST` and `PS5_PORT`, and runs standalone too:

```sh
tools/send-payload -h 192.168.1.159 -p 9021 dRPC5.elf
```

## Network access

The server listens on every interface at port `8642`, but only the PC page is
reachable from outside the console:

| Reachable on the LAN | Everything else → `403 local only` |
|---|---|
| `GET /pc.html`, `/token`, `/qrcode.js` | `GET /`, `/index.html`, `/callback` → redirect to `/pc.html` |
| `GET /api/status` (read-only, powers the PC page's pills) | `/api/presence`, `/api/icon`, `/api/frame` |
| `POST /api/token` | `POST /api/config`, `/api/token/delete`, `/api/discord` |
| | `POST /api/ra/start`, `/api/ra/send`, `/api/ra/close` |
| | `GET /api/ra/poll` |

So a PC on your network can sign the console in, and nothing else. Holding the
console's own config, presence and Remote Auth endpoints to `127.0.0.1` is what
keeps anyone else on the network from rewriting your settings or reading your
activity.

## Configuration

`/data/drpc5/config.ini` on the console, `key=value`, one per line. Everything
is optional.

### Discord identity

| Key | Default | Notes |
|---|---|---|
| `gw_os` | `Playstation` | `properties.os`; drives the client-status bucket |
| `gw_browser` | `PS5 GameBase` | `properties.browser` |
| `gw_version` | `1.00` | `properties.version` — Sony-private, not public API |
| `gw_device` | `PS5` | `properties.device` |

Setting `gw_os` to something else (`Android`, `Windows`, …) moves the session to
that bucket instead. This is the knob to turn if console detection regresses
after a Discord change.

### Timing

All intervals are milliseconds. Values are clamped to a safe range, and a value
that is not a number falls back to the default, so a typo cannot stall or spin
the gateway loop. Changes take effect on the next poll — no restart needed.

| Key | Default | Range | Notes |
|---|---|---|---|
| `poll_ms` | `10000` | 1000–300000 | How often to re-read the foreground title and push a presence update. Lower reacts faster and costs more CPU and more gateway traffic; 10s is a good balance. |
| `hb_min_ms` | `5000` | 1000–60000 | Floor applied to the gateway's own `heartbeat_interval`. Raise it if the console's clock is unreliable, lower it to follow the server more closely. |
| `psn_retry_ms` | `30000` | 1000–3600000 | Backoff before retrying a PSN title or artwork lookup that failed. |
| `resync_ms` | `600000` | 60000–86400000 | How often to re-sync the clock offset used for activity timestamps. |
| `reidentify_ms` | `150` | 0–10000 | Delay before re-identifying with the gateway after it asks us to. |

### Activity source

| Key | Default | Notes |
|---|---|---|
| `enabled` | `1` | Master switch |
| `mode` | `auto` | `auto` follows the console; set `manual` to use the fields below |
| `status` | `online` | Status text |
| `src_game` | `1` | Derive activity from the running title. **The only source flag currently implemented.** |
| `show_artwork` | `1` | Fetch artwork for the activity |
| `show_platform` | `1` | Show platform in the activity body |
| `platform` | `ps5` | `activity.platform`; only emitted for game activities |
| `app_id` | — | Discord application ID for the activity |

Four further keys are written to the config file but **are not read by the code**
and currently do nothing:

| Key | Default | Intended meaning |
|---|---|---|
| `src_media` | `1` | Derive activity from media playback |
| `src_app` | `0` | Derive activity from the foreground app |
| `src_idle` | `1` | Publish when nothing is running |
| `media_line` | — | Override shown for media activity |

They are kept so existing configs load without error and so the intended feature
set stays visible, but do not expect media, foreground-app, or idle detection to
work. Media playback in particular is not yet supported — activity falls back to
the foreground-title behaviour above.

### Manual activity

Only used when `mode` is `manual`.

| Key | Default | Notes |
|---|---|---|
| `type` | `0` | Activity type (`0` playing, `2` listening, …) |
| `name` | — | Activity name |
| `details` | — | First line |
| `state` | — | Second line |
| `asset_key` | — | Large image asset key |
| `asset_text` | — | Large image hover text |
| `asset_small_key` | — | Small image asset key |
| `asset_small_text` | — | Small image hover text |
| `start_timestamp` | — | Start time |
| `end_timestamp` | — | End time |

### Token

The Discord token is read from `/data/drpc5/token`, or pasted through the web UI
at `http://<ps5-ip>:8642/pc.html`. Both `config.ini` and `token` are gitignored.

The QR code and email + password sign-in options in that UI are work in
progress; the token field is the reliable route.


## License

dRPC5 is licensed under the **GNU Affero General Public License v3.0 only**
(`AGPL-3.0-only`). See [LICENSE](LICENSE).

Not legal advice, and this does not affect your obligations under Discord's or
PlayStation's terms — see the warnings at the top.

### Bundled third-party code

Each component retains its own license.

| Component | Version | License | File |
|---|---|---|---|
| libcurl | 8.5.0 | curl license | [LICENSES/curl-LICENSE](third_party/LICENSES/curl-LICENSE) |
| Mbed TLS | 3.6.2 | Apache-2.0 | [LICENSES/mbedtls-LICENSE](third_party/LICENSES/mbedtls-LICENSE) |
| CA root bundle (`cacert.pem`) | 121 certs, 2026-09-25 | MPL-2.0 | [LICENSES/cacert-LICENSE](third_party/LICENSES/cacert-LICENSE) |
| LibProsperoPkg (`tools/lib/`) | 2.0.0 | GPL-3.0 | [tools/lib/LICENSE](tools/lib/LICENSE) |

libcurl, Mbed TLS and the CA bundle live in `third_party/` and are compiled into
the payload. LibProsperoPkg is a host-only build dependency: it is used by
`tools/mk_tile_pkg.c` to build the tile package and is never linked into or
shipped with `dRPC5.elf`. It is committed so a fork can build without extra
setup. Verify it with:

```sh
cd tools/lib && sha256sum -c SHA256SUMS
```

**Mbed TLS is used here under the Apache-2.0 option**, not under GPL-2.0-or-later.
Mbed TLS is dual-licensed and downstream users may pick either; this
choice is defined explicitly so there is no confusion about which terms apply to
the version under `third_party/mbedtls`

All four are compatible with `AGPL-3.0-only`. The MPL-2.0 bundle stays MPL-2.0;
the Apache-2.0, curl-licensed and GPL-3.0 components stay under those terms.
`AGPL-3.0-only` (rather than `-or-later`) means forks must use exactly this
license version.

## Legal

I, foxinwinter/ pLabs5 and any and all contributers are not affiliated with, associated with, sponsored by,
endorsed by, or otherwise established with Sony Interactive Entertainment,
PlayStation, Discord, or any of their other companies or works.

Just because a expict mention above isn't present does **NOT** mean
I, foxinwinter/ pLabs5, or any contributer is affiliated, associated, sponsered by, enorsed by, or otherwise established 
with any entity unless explicitlly mentioned.

This software is provided "as is", without warranty of any kind, express or
implied. Use of this software is at your own risk.

You are solely responsible for complying with Discord's Terms of Service,
PlayStation's terms, and any applicable law.

## Disclaimer of AI Generated Content

The used logo for the repo, favicon, and installed tile logo **ARE** AI-generated