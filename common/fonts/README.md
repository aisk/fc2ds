# Fonts

Run `./fetch.sh` once. It downloads [Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font) (12px, proportional, BDF) into `fusion-pixel/`, which is git-ignored. The font is licensed under the SIL Open Font License 1.1 (`fusion-pixel/OFL.txt`).

At build time `common/tools/mkfont.py` keeps only the glyphs that the game's text actually uses. The `ja` variant is used for Japanese and `zh_hans` for Chinese and English, since some kanji/hanzi have different shapes in each.
