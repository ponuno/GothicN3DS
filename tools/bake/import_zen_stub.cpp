// Used when the project is configured with -DG3DS_WITH_ZENKIT=OFF.
#include "importers.h"

#include <stdexcept>

namespace g3 {

void importZenWorld(const ZenImportOptions&, SrcScene&) {
	throw std::runtime_error("this g3ds-bake was built without ZenKit; reconfigure with -DG3DS_WITH_ZENKIT=ON");
}

} // namespace g3
