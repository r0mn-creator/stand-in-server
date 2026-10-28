# Stand-In Server

Stand-In lets a small computer anywhere in your home feel like it is plugged straight into
your big PC: its screens, keyboard, mouse, pen, controllers and USB devices.

This is the part that runs on the big PC. It creates virtual monitors that match the
client's screens, streams them, and takes input back.

Stand-In has three parts:
[server](https://github.com/r0mn-creator/stand-in-server) (runs on the PC),
[desktop client](https://github.com/r0mn-creator/stand-in-client) (Linux, Windows, macOS) and
[Android client](https://github.com/r0mn-creator/stand-in-android).

**Status:** Beta 2 (0.2.0). Expect rough edges.

## What it adds

- Virtual monitors matching each client's screens
- Automatic bitrate: lowers quality when the network struggles and raises it again when
  it recovers, remembered per client and network
- USB device forwarding (USB/IP) from Stand-In clients
- Works with stock Moonlight clients too; Stand-In extras switch on only for Stand-In clients

## Building

Same steps as Sunshine/Apollo: see `docs/building.md`. Clone with `--recursive`.

## Credits and licence

Stand-In is free software under the GNU GPL v3 (see `LICENSE`). It is a new project
built on the work of others:

- [Moonlight](https://moonlight-stream.org) (moonlight-qt, moonlight-android, moonlight-common-c)
- [Artemis](https://github.com/ClassicOldSong/moonlight-android) by ClassicOldSong
- [Apollo](https://github.com/ClassicOldSong/Apollo) by ClassicOldSong, itself built on
  [Sunshine](https://github.com/LizardByte/Sunshine) by LizardByte
- [Inter](https://rsms.me/inter/) typeface by Rasmus Andersson (SIL OFL)

The original project's README is kept as `README.upstream.md`.
