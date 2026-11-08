// Texture pack names from other N64 renderers, for rt64 (aki-roadmap#93). GPL; see ../README.md.
//
// The application installs this at startup: RT64::setTextureNameProvider(&wcw_texnames::provider()).

#pragma once

#include "common/rt64_texture_names.h"

namespace wcw_texnames {
    const RT64::TextureNameProvider &provider();
};
