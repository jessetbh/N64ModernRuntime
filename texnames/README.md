# texnames

Matches texture pack files named for other N64 renderers, for rt64: Rice Video / GLideN64 (`name#crc#fmt#siz[#palcrc]_suffix`)
and Jabo Direct3D8 (`<32 hex digits>`), computed at runtime so a community pack folder loads as it is (aki-roadmap#93).

rt64 is MIT and only knows the interface in its `src/common/rt64_texture_names.h`. This module implements it. It is a
separate part of the program, licensed with the rest of this repository (GPL-3.0, see `../COPYING`), because of where
its logic comes from:

- `src/wcw_rice.*` and the Rice key in `src/wcw_provider.cpp` follow GLideN64 (`src/GLideNHQ/TxUtil.cpp`, `src/Textures.cpp`,
  `src/GLideNHQ/TxHiResLoader.cpp`, by Sergey Lipskiy, Hiroshi Morii and contributors, GPL), which cites Rice Video
  (Rice, mupen64plus-video-rice, GPL v2 or later).
- `src/wcw_jabo.*` and the Jabo key follow what Gadge64's No Mercy rt64 fork found about Jabo's names (the plugin is closed
  source); MD5 is RFC 1321.

The application installs it with `RT64::setTextureNameProvider(&wcw_texnames::provider())`. Here, recompui does.
The tests in `tests/` build and run before the module links, like the other wcw suites.
