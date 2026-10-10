// The pictures of the inventory: each thing's model drawn small, as the
// console's Gothic shows its things (g3ds/item_view.h says how it is turned).
#pragma once
#include "model_src.h"

#include <vector>

namespace g3 {

// IconHeader followed by `size` x `size` RGBA8 pixels (rows from the top), or
// nothing if the model has no triangles. The model's first node is taken to
// be where all of it hangs (a thing has one).
std::vector<u8> renderItemIcon(const SrcModel& model, int size);

} // namespace g3
