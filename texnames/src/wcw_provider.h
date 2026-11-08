// How the provider's opaque key maps to the Rice and Jabo names (aki-roadmap#93), for the provider and its tests.

#pragma once

#include "common/rt64_texture_names.h"
#include "wcw_rice.h"

namespace wcw_texnames {
    WcwRice::TextureKey decodeKey(const RT64::TextureNameKey &key);
};
