// The scripts' part of a bake: the script image, who the scripts put into
// each world, and the models those characters need.
#pragma once
#include "bake.h"

#include <functional>
#include <string>
#include <vector>

namespace g3 {

// Adds "script/gothic", "meta/looks" and the inhabitants' models to `pak`.
// `line` receives the report. Never throws for anything the scripts do: what
// cannot be used is reported and left out.
void bakePopulation(std::vector<BakeWorld>& worlds, const BakeScripts& scripts, const BakeOptions& opt, PakWriter& pak,
                    const std::function<void(const std::string&)>& line, BakeResult& result);

} // namespace g3
