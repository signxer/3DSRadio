# Third-party notices

The search input and visual language in this release take inspiration from
[ClouDS-Music-FA](https://github.com/Epic0522/ClouDS-Music-FA), released under
the MIT License. Its pinyin lookup design is adapted from the MIT-licensed
Fishason/DSSH project. The bundled `romfs/pinyin_dict.bin` is the dictionary
asset distributed by ClouDS-Music-FA and retains its upstream dictionary
licensing and attribution.

The MP3 decoder uses minimp3, dedicated to the public domain / CC0. OGG/Vorbis
decoding uses stb_vorbis, released into the public domain by its author.

AAC support is an optional link to FAAD2 (`libfaad`), distributed under the
GNU GPL version 2 or later. FAAD2 is not required for MP3/OGG-only builds. A
binary linked with FAAD2 must be distributed in accordance with the FAAD2 GPL
terms, including the corresponding source and license obligations.

This file is provided to make the reused input and decoder components easy to
audit. The application source remains MIT unless a distribution chooses to
apply the compatible GPL terms required by its linked FAAD2 binary.
