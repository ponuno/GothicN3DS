// Used when the project is configured with -DG3DS_WITH_ZENKIT=OFF.
#include "g3ds/model.h"
#include "importers.h"
#include "script_check.h"

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
bool importZenCamera(std::shared_ptr<const ZenGame>, CameraSet&, std::vector<std::string>&) { unavailable(); }
LookSource zenLookSource(std::shared_ptr<const ZenGame>) { unavailable(); }
bool importZenScript(std::shared_ptr<const ZenGame>, SrcScript&, std::vector<u8>&, std::vector<std::string>&) { unavailable(); }
bool importZenFightScript(std::shared_ptr<const ZenGame>, SrcScript&, std::vector<std::string>&) { unavailable(); }
bool importZenDialogue(std::shared_ptr<const ZenGame>, SrcDialogue&, std::vector<std::string>&) { unavailable(); }

ScriptCheckResult checkScript(const std::vector<u8>&, const SrcScript&, const std::vector<u8>&, const ScriptCheckOptions&) {
	ScriptCheckResult r;
	r.report = "scripts: not checked against the reference interpreter: this build has none\n";
	return r;
}

} // namespace g3
