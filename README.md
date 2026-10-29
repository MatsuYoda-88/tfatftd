# tfatftd.xyz

**The Fast and the Furious: Tokyo Drift** (PS2, 2006) running in your web browser.

It is not an emulator: the game's PS2 code was statically recompiled to WebAssembly (built on
[ps2-web-recomp](https://github.com/snuri00)) and its graphics are drawn with WebGPU.

## Play

1. Open the site in desktop Chrome or Edge (or Chrome on Android / Safari on iOS 26+).
2. **Game data → Load ISO**: pick your own USA disc image (SLUS-21449). It's copied into your browser once and never leaves your device.
3. Press **Play**.

Saves live in your browser. The **Saves** tab makes encrypted backup files and imports/exports PCSX2 `.ps2` memory cards.

## HD textures

Got a PCSX2 texture pack for SLUS-21449? **Settings → HD texture pack → Choose folder** and point it at the folder holding `replacements` (desktop Chrome/Edge).

## Notes

Fan project. Not affiliated with or endorsed by Universal, Bandai Namco or Eutechnyx. No game files are hosted here.
`coi-serviceworker.js` is by Guido Zuidhof (MIT).
