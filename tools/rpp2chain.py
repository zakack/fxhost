#!/usr/bin/env python3
"""Convert the FX chain of a REAPER project track into an fxhost chain.json.

VST3 plugins keep their REAPER state (written as <id>.reaper next to the config;
fxhost imports it on first start). VST2 entries are swapped for the VST3 build of
the same plugin when one exists under ~/.vst3; their state can't be carried over.

    rpp2chain.py ~/carla_loadouts/fx.RPP [-o ~/.config/fxhost/chain.json] [--track 1]
"""
import argparse
import base64
import json
import os
import re
import sys
from pathlib import Path

# JACK port names as PipeWire's JACK layer shows them: "<node.description>:<port>",
# not the "<node.name>:<port>" form that pw-link prints.
SCARLETT = "Scarlett 2i2 3rd Gen Direct Scarlett 2i2 USB"
DEFAULT_MIC = f"{SCARLETT}:capture_FL"
DEFAULT_OUTS = [f"{SCARLETT}:playback_{{ch}}", "FX Mic:input_{ch}"]


def find_bundle(filename, roots):
    want = filename.lower()
    for root in roots:
        for dirpath, dirnames, _ in os.walk(root, followlinks=True):
            for d in dirnames:
                if d.lower() == want:
                    return str(Path(dirpath) / d)
            # don't descend into bundles
            dirnames[:] = [d for d in dirnames if not d.lower().endswith(".vst3")]
    return None


def slug(name, taken):
    base = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "plugin"
    s, i = base, 2
    while s in taken:
        s, i = f"{base}-{i}", i + 1
    taken.add(s)
    return s


def parse_tracks(text):
    """Yield (track_name, [fx dicts]) for every track with an FXCHAIN."""
    lines = text.splitlines()
    i, track_name = 0, None
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("<TRACK"):
            track_name = None
        elif line.startswith("NAME ") and track_name is None:
            track_name = line[5:].strip().strip('"')
        elif line == "<FXCHAIN":
            fx, bypass, depth = [], False, 1
            i += 1
            while i < len(lines) and depth > 0:
                l = lines[i].strip()
                if l.startswith("BYPASS "):
                    bypass = l.split()[1] == "1"
                elif l.startswith("<VST "):
                    m = re.match(r'<VST "([^"]*)" "([^"]*)" \S+ "[^"]*" (\S+)', l)
                    body = []
                    i += 1
                    while not lines[i].strip().startswith(">"):
                        body.append(lines[i].strip())
                        i += 1
                    if m:
                        fx.append({"label": m.group(1), "file": m.group(2), "ident": m.group(3),
                                   "body": body, "bypass": bypass})
                elif l.startswith("<"):
                    depth += 1
                elif l.startswith(">"):
                    depth -= 1
                i += 1
            yield track_name or "", fx
            continue
        i += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rpp")
    ap.add_argument("-o", "--output", default="~/.config/fxhost/chain.json")
    ap.add_argument("--track", type=int, default=1, help="1-based index among tracks that have FX")
    ap.add_argument("--mic", default=DEFAULT_MIC, help="source port fed to both inputs")
    ap.add_argument("--client-name", default="fx-chain")
    args = ap.parse_args()

    tracks = list(parse_tracks(Path(args.rpp).expanduser().read_text(errors="replace")))
    if not 1 <= args.track <= len(tracks):
        sys.exit(f"no FX track #{args.track} (found {len(tracks)})")
    _, fxlist = tracks[args.track - 1]

    out = Path(args.output).expanduser()
    state_dir = out.parent / "state"
    state_dir.mkdir(parents=True, exist_ok=True)
    roots = [Path("~/.vst3").expanduser(), Path("/usr/lib/vst3"), Path("/usr/local/lib/vst3")]

    plugins, taken = [], set()
    for fx in fxlist:
        kind, _, rest = fx["label"].partition(": ")
        name = re.sub(r"\s*\([^)]*\)\s*$", "", rest)  # drop " (Vendor)"
        entry = {"id": slug(name, taken), "name": name, "bypass": fx["bypass"]}

        if kind == "VST3":
            path = find_bundle(fx["file"], roots)
            uid = re.search(r"\{([0-9A-Fa-f]{32})\}", fx["ident"])
            if uid:
                entry["uid"] = uid.group(1).upper()
            body = fx["body"]
            if len(body) > 2:
                blob = base64.b64decode("".join(body[1:-1]))
                (state_dir / f"{entry['id']}.reaper").write_bytes(blob)
        elif kind == "VST":
            stem = re.sub(r"\.(so|dll)$", "", fx["file"], flags=re.I)
            path = find_bundle(stem + ".vst3", roots)
            entry["name"] = ""  # VST3 class names differ; take the bundle's only plugin
            entry["note"] = f"was VST2 '{rest}' in REAPER; state not imported"
            print(f"  ! {name}: using VST3 build, re-pick your preset once", file=sys.stderr)
        else:
            print(f"  ! skipping unsupported {fx['label']}", file=sys.stderr)
            continue

        if not path:
            print(f"  ! {name}: no VST3 bundle '{fx['file']}' found, skipping", file=sys.stderr)
            continue
        entry["path"] = path
        plugins.append(entry)
        print(f"  {entry['id']:<24} {path}{'  [bypassed]' if entry['bypass'] else ''}", file=sys.stderr)

    outs = {}
    for ch in ("FL", "FR"):
        outs[f"out_{ch}"] = [t.format(ch=ch) for t in DEFAULT_OUTS]
    config = {
        "client_name": args.client_name,
        "max_block": 2048,
        "connect": {"in_FL": [args.mic], "in_FR": [args.mic], **outs},
        "plugins": plugins,
    }
    out.write_text(json.dumps(config, indent=2) + "\n")
    print(f"wrote {out} ({len(plugins)} plugins)", file=sys.stderr)


if __name__ == "__main__":
    main()
