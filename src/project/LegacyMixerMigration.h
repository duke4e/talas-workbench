#pragma once

#include "talas/project/ProjectDocument.h"

namespace talas::project::detail
{
// Project format 43 replaces the two-input talas.mixer with the Mixer,
// talas.mixerV2. Each input whose gain is not a plain unit gain keeps it on an
// inserted VCA, and references to that gain follow it there. The mixed
// signal leaves through leftOut, where audioOut's consumers now read it.
void migrateLegacyMixers(ProjectDocument& document);
}
