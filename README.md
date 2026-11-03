# fxhost

A small headless VST3 effect-chain host for JACK/PipeWire. It runs a fixed chain of
plugins between a capture port and any number of playback ports, with no DAW open.
You open plugin editors on demand from the command line.

It was built to replace an always-open REAPER project for a live mic chain of VST3s
plugins bridged with [yabridge](https://github.com/robbert-vdh/yabridge).

## Why not Carla, pedalboard, or another host?

Waves ships its plugins inside a single *WaveShell* bundle, which here holds about 715
plugins. JUCE-based hosts, and Carla's scanner, create every class in a bundle just to
list it, which takes several minutes for a WaveShell. fxhost skips the scan. Given a
plugin's display name and VST3 class ID, it builds the JUCE `PluginDescription` directly
and creates only that one class. REAPER's plugin cache (and `fx.RPP` files) already hold
both values, and `tools/rpp2chain.py` copies them over.

## Layout

| Path | What |
|---|---|
| `Source/Main.cpp` | The whole host: audio engine, plugin loading, state, editors, control socket |
| `tools/rpp2chain.py` | Converts a REAPER track's FX chain into `chain.json` and state blobs |
| `tools/jack-ports.py` | Lists JACK port names in the form `connect` needs |
| `fxhost.service` | systemd user unit |
| `JUCE/` | JUCE 9.0.5, cloned (not vendored in any VCS) |
| `build/` | CMake/Ninja build tree |

Installed files:

| Path | What |
|---|---|
| `~/.local/bin/fxhost` | The binary |
| `~/.config/systemd/user/fxhost.service` | The unit |
| `~/.config/fxhost/chain.json` | The chain |
| `~/.config/fxhost/state/` | Per-plugin state and `bypass.json` |

## Build and install

Requires CMake, Ninja, a C++20 compiler, `pipewire-jack` (for `jack.h`/`libjack`), and the
usual JUCE Linux deps (freetype2, fontconfig, libX11/Xrandr/Xinerama/Xcursor/Xext, alsa-lib).

```sh
cd ~/.local/src/fxhost
git clone --depth 1 --branch 9.0.5 https://github.com/juce-framework/JUCE.git JUCE   # first time only
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release                               # first time only
cmake --build build

install -Dm755 build/fxhost_artefacts/Release/fxhost ~/.local/bin/fxhost
install -Dm644 fxhost.service ~/.config/systemd/user/fxhost.service
systemctl --user daemon-reload
systemctl --user enable --now fxhost
```

After changing only `Main.cpp`, `cmake --build build` recompiles just that file.

## Usage

```sh
fxhost [--config PATH]        # run (default ~/.config/fxhost/chain.json)
fxhost ctl <command>          # talk to the running instance
```

| Command | Effect |
|---|---|
| `list` | The chain with indexes, ids, names, bypass and editor state |
| `show <ref>` / `show all` | Open a plugin editor |
| `hide <ref>` / `hide all` | Close an editor (saves state) |
| `bypass <ref> [on\|off]` | Toggle or set bypass (persisted) |
| `save` | Write all plugin state now |
| `status` | Sample rate, JACK buffer, DSP load, xruns since start, total plugin latency |
| `reconnect` | Recreate every configured connection |

`<ref>` resolves in this order, and the first match wins:

1. A chain index (`0`, `1`, …).
2. An exact id, ignoring case.
3. Text contained in the display name, ignoring case, taking the first match in chain order.

So `clarity` finds "Clarity Vx Pro Stereo", while `pro` finds whichever "Pro-…" comes
first. Use the index or the id when a name is ambiguous.

The service log is in `journalctl --user -u fxhost`.

## Configuration: `chain.json`

```json
{
  "client_name": "fx-chain",
  "max_block": 2048,
  "connect": {
    "in_FL":  ["Scarlett 2i2 USB:capture_FL"],
    "in_FR":  ["Scarlett 2i2 USB:capture_FL"],
    "out_FL": ["FX Mic:input_FL"],
    "out_FR": ["FX Mic:input_FR"]
  },
  "plugins": [
    { "id": "pro-q-4", "name": "Pro-Q 4", "uid": "ED57BD725C60467EA64DD2F400758B6F",
      "path": "/home/user/.vst3/yabridge/FabFilter/FabFilter Pro-Q 4.vst3", "bypass": false }
  ]
}
```

| Key | Default | Meaning |
|---|---|---|
| `client_name` | `fx-chain` | JACK client / PipeWire node name |
| `max_block` | `2048` | Largest block the plugins are prepared for. Longer JACK periods are split. |
| `state_dir` | `state` | Relative to the config file's directory |
| `socket` | `$XDG_RUNTIME_DIR/fxhost.sock` | Control socket. `fxhost ctl` always uses the default path, so change this only for test instances. |
| `connect` | — | Our port (`in_FL`, `in_FR`, `out_FL`, `out_FR`) → list of remote JACK ports |
| `plugins[].id` | `name` | Key for the state file and bypass entry. **Must be unique.** |
| `plugins[].name` | — | VST3 class name. Required with `uid`; empty means the bundle's first plugin. |
| `plugins[].uid` | — | 32-hex-digit VST3 class ID. Use it for shells (loads without scanning). |
| `plugins[].path` | — | The `.vst3` bundle (the yabridge one under `~/.vst3/yabridge`) |
| `plugins[].bypass` | `false` | Initial bypass. `state/bypass.json` overrides it once it exists. |

Connection port names are what PipeWire's JACK layer shows: `<node.description>:<port>`.
This is **not** the `<node.name>:<port>` form that `pw-link` prints. List the real names
with `tools/jack-ports.py [filter]` (`in` = a port you can connect *to*).

Connections are made at startup, and again whenever a port named in `connect` appears,
such as when the interface is plugged back in. Links you remove by hand stay removed until
then, or until `fxhost ctl reconnect`.

The chain is fixed while the host runs. To change it, edit the file and run
`systemctl --user restart fxhost`.

## Plugin state

- **On load:** `state/<id>.state` (JUCE's binary state) is restored if it exists.
  Otherwise `state/<id>.reaper` (a raw REAPER VST3 chunk from the importer) is converted
  and applied.
- **When saved:** when you close an editor, every 30 s while anything changed, on
  `ctl save`, and on a normal stop.
- **Bypass:** stored separately in `state/bypass.json`.

## Importing from REAPER

```sh
tools/rpp2chain.py ~/carla_loadouts/fx.RPP [-o ~/.config/fxhost/chain.json] [--track N] [--mic PORT]
```

- **Track:** it takes the Nth track that has FX (default 1), and keeps each FX's bypass flag.
- **Plugin paths:** they're resolved under `~/.vst3`.
- **VST3 entries:** these keep their class ID, and their state is written as `state/<id>.reaper`.
- **VST2 entries:** these are replaced by the VST3 build of the same plugin, if one exists.
  Their state can't be converted, so you re-pick their presets once.
- **Overwriting:** it overwrites `chain.json`. Existing `.state` files take priority over
  the new `.reaper` files, so delete a `.state` file to re-import that plugin's settings.
