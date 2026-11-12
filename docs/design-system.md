# Fulcrum native launcher design contract

Fulcrum uses three first-party native UI stacks: SwiftUI (macOS), WinUI 3 (Windows), GTK4 (Linux). Visual parity means a consistent interaction grammar, density, hierarchy, typography and accent, not replacing native windows with WebViews. See [cross-platform-interaction.md](cross-platform-interaction.md) for the behavioral contract.

This file is the value baseline for the v2 design language (`design/DESIGN.md` records semantics and decisions; `design/prototype/shared.css` mirrors these values 1:1 in the prototype). Values below override the pre-v2 numbers; hosts converge onto them slice by slice.

## Shared launcher tokens

| Token | Baseline | Implementation |
| --- | --- | --- |
| Panel size | 680 wide; height grows with results, 240–640, capped at 12 visible rows | Resize per result count on all hosts; the settings “Maximum results” upper bound agrees with the visible-row cap |
| Search typography | 19 medium | SwiftUI / XAML / GTK CSS |
| Row title | 13 medium | Unified: Windows was 13.5/600, GTK 13/600 — converge to 13 medium |
| Row subtitle | 11.5 | Mute tier; GTK was 11 — converge to 11.5 |
| Badge / Enter chip | 10 semibold / 11 semibold | Trailing pill |
| Hint bar / status line | 11 | Faint tier |
| AI and detail body | 12.5 | Regular; Windows was 13, GTK used monospace — converge to 12.5 |
| Ink | `#1d1d1f` light / `#f2f2f2` dark | Primary text |
| Mute | ink at 66% | Row subtitles, hints |
| Faint | ink at 62% light / 52% dark (≥ 4.5:1) | Status line, weakest text |
| Accent (blocks) | `#D97706`, both themes | Fill only: selection, icon tile, switch, caret, spinners |
| Accent text/icons | `#9A4B06` light / `#F0A64B` dark (each ≥ 4.5:1) | Badges, Enter chip, headings, selected sidebar item, tile icons — raw `#D97706` is never used on text or glyphs |
| Selected row | amber 14% | SwiftUI 0.14, XAML #24, GTK alpha 0.14; in the actions layer selection becomes a 1.5 px inset ring instead of a fill |
| Hover row | neutral 5% light / 7% dark | Was amber 8% — hover is neutral so it never competes with the amber selection |
| Icon tile | amber 12%, 28 pt, radius 6 | Leading icon inside the tile |
| Badge fill | amber 14% | Accent-text glyph inside |
| Hit highlight | accent-text, weight 650 | Color + weight double cue on matched characters; no background fill |
| Danger | `#d02f26` light / `#ff5449` dark | Errors and destructive actions only |
| Focus ring | 2 px accent-text, inset −2 px | All hosts; focus and selection are two different visuals |
| Result row min height | 44 | Keep subtitles legible |
| Row / panel corner radius | 8 / 12 | Inset rounded selection; 6 for tiles |
| Settings window | 720 × 520 on all hosts | Windows was 820 × 580 — converge to 720 × 520 |
| Launcher backdrop | Native blur where available; >= 98% opaque GTK fallback | Windows Acrylic, AppKit vibrancy, Linux CSS RGBA |

Native settings forms retain platform conventions; windowing, blur and tray/menu carriers may vary. Background **legibility** does not: a Linux session without compositor blur must not reveal sharp text or icons behind the result list. Use a >= 98% opaque launcher surface there (light and dark), and verify the actual contrast in front of a busy wallpaper and overlapping application text. Acrylic and vibrancy can remain native on Windows/macOS, but should also favor clear results over translucency. Do not sacrifice keyboard focus, legible contrast, accessibility identifiers or the shared Esc/actions/i18n contract.

## Verification

Run `raco test tests/`, `raco rivet schema check rivet-schema.json --json` and `raco rivet acceptance --json` on all three platforms against packaged products. Check light/dark themes, scaling, selection/hover contrast (accent-text must hold ≥ 4.5:1 in both themes), localized hints and accessibility. On Linux, additionally exercise X11 and Wayland host activation.

Design-state parity is self-checked against `design/prototype/audit.html` — the prototype's state matrix (hit highlight, IME composition, focus order, offline / rate-limit / AI running·done·401·429, sensitive clipboard, over-limit CJK, row-cap states) is the reference for what hosts must be able to show; new states enter that matrix before implementation.
