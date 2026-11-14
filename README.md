# Darktamer

A private fork of [darktable](https://github.com/darktable-org/darktable) —
the open source photography workflow application and non-destructive raw
developer. Everything not listed below behaves exactly like upstream; this
README only documents where the fork diverges.

Upstream is synced **one-way** (upstream master → this fork, regular merges).
Nothing is contributed back, and issues exist only on this repository.

## What is different from upstream

### Native CIRAW decode (rawspeed cut for the supported window)

Canon RAW files are decoded by a native CIRAW pipeline instead of rawspeed:

- v9 → v8 fallback for the CR3 window, with a background warmup decodes gate
- byte-quota thumbnail cache and an all-level prewarm on entry
- two-class foreground/background gating and a view-aware pause, so imports
  and lighttable browsing stay responsive while warmups run in the background
- the import chain is wired through the same decode path

### yiyin framing module

A new `framing` module (`src/iop/borders.c` fork lineage) with three modes:

- **basic** — the legacy border
- **blur** — the yiyin backdrop: solid or blurred backdrop, rounded corners,
  drop shadow, and up to three picture info rows (`{Make}`, `{Model}`,
  `{Lens}`, `{FocalLength}`, `{FNumber}`, `{ExposureTime}`, `{ISO}`, ...),
  each with its own pattern and size, one shared font and opacity
- **film strip** — black bands with light-box holes and an edge marking

Layout is computed on the whole picture rather than the viewport, so panning
and zooming never clip the frame; CPU and OpenCL render identically. Defaults
live in preferences → processing → framing module.

### MCP agent server

The running GUI can serve an MCP (Model Context Protocol) server on a unix
socket, so an external agent can drive the app:

- start with `darktable --mcp`, connect with `darktable-mcp --bridge`
- tools: import/list images, open into darkroom, `apply_stack` edits live on
  the canvas, export, set ratings and color labels, steer the lighttable
  collection and read the selection, save/apply styles, render and stats
- CHAR array parameters are accepted by name (framing's text, font, caption)
- preferences → **AI** tab hosts the agent server settings with a copyable
  client configuration, and the server writes `mcp-client-config.json` on
  startup

### macOS packaging

- ships as `Darktamer.app` (ad-hoc signed, arm64 dmg on the releases page)
- keeps its own config and cache (`~/.config/darktamer`), so it never touches
  an official darktable install
- bundle binaries, libraries and the data tree are fully self-contained
  (no homebrew or build-tree paths)

## Releases

Grab the latest dmg from the
[releases page](https://github.com/kamasylvia/darktamer/releases). On first
launch macOS will refuse an ad-hoc signed app until you allow it
(right click → Open, or
`xattr -dr com.apple.quarantine /Applications/Darktamer.app`).

## Building

Same as upstream (`build.sh` or plain cmake); the macOS bundle recipe this
fork ships with lives in the working notes. `USE_MCP=ON` (default) builds the
MCP server and bridge.

## License

Same as upstream darktable: GPL-3.0. See [LICENSE](LICENSE).
