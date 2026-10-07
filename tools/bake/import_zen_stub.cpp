// Used when the project is configured with -DG3DS_WITH_ZENKIT=OFF.
#include "importers.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace g3 {

struct ZenGame {};

namespace {
[[noreturn]] void unavailable() {
	throw std::runtime_error("this g3ds-bake was built without ZenKit; reconfigure with -DG3DS_WITH_ZENKIT=ON");
}
} // namespace

std::shared_ptr<ZenGame> openZenGame(const std::string&, std::vector<std::string>&) { unavailable(); }
bool zenGameHasFile(const ZenGame&, const std::string&) { return false; }
void importZenWorld(std::shared_ptr<const ZenGame>, const ZenImportOptions&, SrcScene&) { unavailable(); }
void importZenMesh(const ZenImportOptions&, SrcScene&) { unavailable(); }
bool importZenActor(std::shared_ptr<const ZenGame>, const ZenActorOptions&, SrcModel&) { unavailable(); }

} // namespace g3
