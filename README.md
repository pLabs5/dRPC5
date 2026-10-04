# dRPC5 — Discord Rich Presence for PS5 via Direct Gateway

A PlayStation 5 homebrew payload that connects straight to Discord's gateway and
publishes Rich Presence, so the PS5 shows up as an active console session.

> [!WARNING]
> This project uses a self-bot style approach to update RPC status. It **is**
> against Discord's TOS, but is very rarely enforced. Use at your own risk.

> [!NOTE]
> I am not responsible for any damages or consequences directly caused by this
> software, including but not limited to Discord account termination, PS5 system
> software corruption, data loss, or anything else arising from its use.

---

## What it does

* Connects directly to Discord's gateway from the console and publishes Rich
  Presence (`op 3`). Nothing needs to be running on a PC.
* Identifies as a PS5 console session, so the account reports as active on
  console (see [Console identity](#console-identity)).
* Works out what's running by walking the console's own process list
  (`sysctl KERN_PROC_PROC`) rather than asking PSN what you're playing, then
  fetches box art for that title from PSN.
* Manual override for activity text, assets and timestamps when you don't want
  auto-detection.
* Web UI for configuration on the console itself at `http://localhost:8642/`.
  That page is **local-only** — non-local requests to `/` get redirected to
  `/pc.html`.
* Sign-in by user token, email + password, or QR code via Discord Remote Auth
  (`wss://remote-auth-gateway.discord.gg`). `/pc.html` is reachable from another
  device on the network for this.
  **Work in progress:** the QR code and email + password flows are not reliable
  yet and may fail partway through. Pasting a user token is the dependable
  option for now.
* Authenticated Discord REST proxy with hCaptcha header passthrough, so the
  browser can make token-gated calls the console would otherwise be blocked
  from.

## Console identity

This is the part that isn't obvious.

Discord buckets each gateway session into a *client status* — `desktop`,
`mobile`, `web`, plus undocumented `embedded` and `vr` keys. Presence services
such as [Lanyard](https://github.com/Phineas/lanyard) just probe for those keys,
so **the bucket is decided entirely by how you identify**, not by anything you
put in an activity.

A stock PS5 Discord install identifies with Sony-private values that Discord
maps onto the console bucket:

```json
"properties": {
  "os": "Playstation",
  "browser": "PS5 GameBase",
  "version": "1.00"
}
```

Note `properties.version` — that field isn't in Discord's public
`GatewayIdentifyProperties`, and `device` isn't used at all. Identifying this
way is what makes a session report as active on console.

Two things that are **not** the mechanism, worth recording so nobody repeats the
experiment:

* **Activity `platform`.** `"platform": "ps5"` on an activity is a genuine field
  (`ActivityPlatform` in `discord-api-types`) and does tag the activity as
  PlayStation, but it does *not* move the session into the `embedded` bucket.
* **`ActivityFlags.Embedded` (256).** Despite the name, this refers to Discord's
  *embedded activities* / Embedded App SDK, not consoles. Setting it does nothing
  here.

`src/gw/ws.c` reproduces the console identity, and all four values are
configurable so you can A/B without rebuilding — see `gw_os`, `gw_browser`,
`gw_version`, `gw_device` below.

## Prebuilt binaries

Prebuilt payloads are on the
[releases page](https://github.com/pawprnt/dRPC5/releases). Each release is two
files:

| File | |
|---|---|
| `dRPC5.elf` | the payload. Tile package, web UI and CA bundle are all inside it |
| `dRPC5.elf.sha256` | checksum, in `sha256sum` output format |

Verify, then [deploy](#deploying):

```sh
sha256sum -c dRPC5.elf.sha256
```

No Payload SDK, host compiler, `dist/` or `third_party/` needed. Unsigned, for
your own console.

<details>
<summary>Updating</summary>

The tile installs once, guarded by `/user/appmeta/DRPC00001`, so a new payload
updates the code but leaves the old icon. Force a reinstall with
`rm -rf /user/appmeta/DRPC00001`. The payload itself is never installed — it is
injected at deploy time, so redeploying is the whole upgrade.

</details>

## Building

Requires the [Payload SDK](https://github.com/ps5-payload-dev/sdk) and a host C
compiler. Everything else, including the tile-packaging dependency, is in the
repository:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make clean && make
```

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

The payload is sent to the console's ELF loader over TCP:

```sh
make deploy PS5_HOST=<ps5-ip> PS5_PORT=9021
```

`prospero-deploy` shells out to `socat`. If you don't have it installed:

```sh
nix shell nixpkgs#socat -c make deploy PS5_HOST=192.168.1.159 PS5_PORT=9021
```

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

## Credits

Console identity strings were recovered from the PS5's own
`Sce.Vsh.DiscordAccessor` on a decrypted firmware dump; the `#US` string heap
orders literals by first reference, which is what made the identify payload
reconstructable.

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
| Mbed TLS | 3.6.2 | Apache-2.0 **or** GPL-2.0-or-later | [LICENSES/mbedtls-LICENSE](third_party/LICENSES/mbedtls-LICENSE) |
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

**Mbed TLS is used here under the Apache-2.0 option**, not the GPL-2.0-or-later
option. Mbed TLS is dual-licensed and downstream users may pick either; this
election is stated explicitly so there is no ambiguity about which terms apply to
the vendored copy.

All four are compatible with `AGPL-3.0-only`. The MPL-2.0 bundle stays MPL-2.0;
the Apache-2.0, curl-licensed and GPL-3.0 components stay under those terms.
`AGPL-3.0-only` (rather than `-or-later`) means forks must use exactly this
license version.

## Legal

I, foxinwinter / pawprnt, am not affiliated with, associated with, sponsored by,
endorsed by, or otherwise established with Sony Interactive Entertainment,
PlayStation, Discord, or any of their other companies or works.

This software is provided "as is", without warranty of any kind, express or
implied. Use of this software is at your own risk.

You are solely responsible for complying with Discord's Terms of Service,
PlayStation's terms, and any applicable law. The Discord protocol details and
console identification values used here were obtained by reverse engineering a
firmware dump on a personally owned console, and are used for personal research
and interoperability.

## Disclaimer of AI Generated Content

The used logo for the repo, favicon, and installed Tile **ARE** AI-generated