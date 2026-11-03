# fxhost: notes for agents

Headless JUCE 9 / libjack VST3 effect-chain host running as the `fxhost` systemd user
service. It carries the user's **live** mic chain (Scarlett → headphones + `fx-mic`).
`README.md` is the source of truth for usage, config keys, state files and known issues.
This file covers what the code and README don't tell you.

## Building

- The user runs the build: `cmake --build ~/.local/src/fxhost/build`. Claude Code's auto
  mode refuses to compile the cloned JUCE tree, so make the edits, then ask the user to
  build. Only `Source/Main.cpp` recompiles.
- Install = `install -Dm755 build/fxhost_artefacts/Release/fxhost ~/.local/bin/fxhost`, then
  `systemctl --user restart fxhost`. A restart briefly interrupts the user's mic, so mention
  it when you do it.
- clangd's "file not found" diagnostics on `Main.cpp` are noise: there's no
  `compile_commands.json`.
- No git repo.

## Invariants in `Source/Main.cpp`

Each of these was learned from a crash. Keep them when editing.

- **The audio thread is real-time.** `process()`/`processSlot()` allocate nothing and block
  on nothing. The plugin callback lock is taken with `ScopedTryLock`, and the plugin is
  skipped if the lock is busy. Scratch buffers are preallocated (32 channels × `max_block`),
  and channel counts are read **live** each block, because a plugin can change its layout at
  any time, for example on preset load.
- **Every bus stays enabled.** For a disabled bus, JUCE passes null channel pointers, and
  yabridge `memcpy`s from them (segfault). `configureBuses()` enables sidechains, and
  `processSlot()` feeds them silence and passes a plugin through if any bus is still disabled.
- **Load order:** `restoreState()` → `configureBuses()` → `prepareToPlay()`. A restored state
  can turn sidechains back on.
- **Shutdown is quiesced.** `cleanUp()` calls `waitForAudioIdle()` before and after
  `jack_deactivate()`, because pipewire-jack's deactivate doesn't wait for an in-flight
  cycle. Deactivating a plugin mid-`process()` corrupts yabridge's IPC (abort). Then it
  saves state and releases plugins, and `shutdown()` ends with `std::_Exit`: normal
  exit-time teardown jumps to a null pointer once JUCE has unloaded the yabridge libraries.
- **VST3 get/set state runs on the message thread** (JUCE asserts it). Control-socket
  commands are marshalled there with `MessageManager::callAsync`.
- **Shell plugins load by class ID.** `fillIdsFromTuid()` reproduces JUCE's
  `getHashForRange`/`getNormalisedTUID` (`JUCE/modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h`),
  and `name` must equal the VST3 class name exactly. Re-check both after a JUCE upgrade.
  Scanning a WaveShell (~715 classes) instead takes many minutes.
- **REAPER VST3 chunk layout** (`convertReaperVst3Chunk`): `u32 compSize`, `u32` (always 1),
  component bytes, `u64 ctrlSize`, controller bytes. These are the decoded base64 lines of a
  `<VST` block minus the first (header) and last (program name) lines. Verified against
  FabFilter and Waves chunks.

## Environment facts

- **JACK port names** under PipeWire are `<node.description>:<port>`, e.g. `FX Mic:input_FL`.
  `tools/jack-ports.py` lists them; `pw-link` names fail silently in `connect`.
- **The unit uses `KillMode=mixed`.** With the default, a stop SIGTERMs the yabridge/Wine
  plugin hosts too, and the audio thread aborts on a reset socket.
- **yabridge must stay on the official Arch `yabridge` 5.1.1.** `yabridge-wine10-git`
  (r3086) crashes with a Wine stack overflow in `createInstance`. The stale 5.0.5 copy was
  moved to `~/.local/share/yabridge.old-5.0.5`; a `~/.local/share/yabridge` directory
  overrides the system libraries.
- **Editor input only works at the monitor's top-left** (see README, Known issues). Treat it
  as a known yabridge/Wine limitation unless the user asks to work on it.

## Testing without touching the live service

- **Probe instance:** use a config copy with its own `client_name` (e.g. `fx-chain-test`),
  `"connect": {}`, a copied `state_dir`, and, while the service runs, its own `socket`.
  `fxhost ctl` only talks to the default socket, so testing `ctl` means stopping the
  service first. Keep probe files in the scratchpad.
- **Audio check:** `pw-play`/`pw-record` with `-P '{ node.autoconnect = false, node.name = … }'`,
  then `pw-link` them to the probe's `in_*`/`out_*` ports. `--target` won't attach to a JACK
  client node, and silently plays to the default sink and records the mic instead.
  Compare output RMS against input, bypassed vs. active.
- **Diagnostics:**
  - yabridge's banner (versions, prefix, config file) and `YABRIDGE_DEBUG_LEVEL=2` call
    traces only show when the probe's stderr goes to a file; under systemd they don't reach
    the journal.
  - Crashes: `coredumpctl info <pid>`.
  - Hangs: `gdb`/`eu-stack` attach is blocked (ptrace scope), so use a debug-traced probe.
- **Shell:** commands run in zsh, where `$var` doesn't word-split (use arrays). `pkill -f`
  and `pgrep -f` patterns also match your own shell's command line; target PIDs or use
  `systemctl --user`.
