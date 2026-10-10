#include "fight_ai.h"

#include "fight.h"
#include "script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace g3 {

std::vector<u8> bakeFightAi(const SrcScript& fight, std::string& report) {
	ScriptBakeResult sr;
	const std::vector<u8> image = bakeScript(fight, sr);
	u8* blob = static_cast<u8*>(std::malloc(image.size()));
	if (!blob) throw std::bad_alloc();
	std::memcpy(blob, image.data(), image.size());
	ScriptVm vm;
	if (!vm.adopt(blob, u32(image.size()))) throw std::runtime_error("the script machine refuses it");
	const u32 moveMember = vm.findMember("C_FIGHTAI", "MOVE");
	if (moveMember == SCRIPT_NONE || vm.symbol(moveMember).type != SCRIPT_INT)
		throw std::runtime_error("it has no class C_FIGHTAI with moves");
	// The scripts' numbers of the moves, as this script defines them.
	i32 numbers[FM_COUNT] = {};
	for (u32 m = 1; m < FM_COUNT; ++m) {
		const u32 sym = vm.find(kFightMoveNames[m]);
		const bool constant = sym != SCRIPT_NONE && vm.symbol(sym).type == SCRIPT_INT && !(vm.symbol(sym).flags & SCRIPTF_MEMBER);
		numbers[m] = constant ? vm.getInt(sym) : kFightMoveDefaults[m];
	}
	const auto engineMove = [&](i32 scriptMove) -> u8 {
		if (scriptMove == 0) return FM_NONE;
		for (u8 m = 1; m < FM_COUNT; ++m)
			if (numbers[m] == scriptMove) return m;
		return FM_NONE; // (a move the engine does not know: none)
	};
	// As many tactics as the script says it has (MAX_FIGHTAI), and at least
	// as many as it has tables for.
	u32 tactics = 0;
	{
		const u32 sym = vm.find("MAX_FIGHTAI");
		if (sym != SCRIPT_NONE && vm.symbol(sym).type == SCRIPT_INT) tactics = u32(std::max<i32>(vm.getInt(sym), 0));
		if (tactics > 256) tactics = 256;
	}
	for (u32 t = tactics; t < 256; ++t)
		for (u32 s = 0; s < FS_COUNT; ++s) {
			char name[48];
			std::snprintf(name, sizeof(name), "%s%u", kFightSituations[s], unsigned(t));
			if (vm.find(name) != SCRIPT_NONE) tactics = t + 1;
		}
	if (tactics == 0) throw std::runtime_error("it has no fight AI tables (FA_...)");
	std::vector<u8> out(sizeof(FightAiHeader) + size_t(tactics) * FS_COUNT * FIGHT_AI_MOVES, 0);
	const FightAiHeader h = {FIGHT_AI_MAGIC, FIGHT_AI_VERSION, tactics, FS_COUNT};
	std::memcpy(out.data(), &h, sizeof(h));
	u8* moves = out.data() + sizeof(h);
	const u32 count = std::min<u32>(vm.symbol(moveMember).count, FIGHT_AI_MOVES);
	u32 withTables = 0, tables = 0, unknown = 0;
	for (u32 t = 0; t < tactics; ++t) {
		bool any = false;
		for (u32 s = 0; s < FS_COUNT; ++s) {
			char name[48];
			std::snprintf(name, sizeof(name), "%s%u", kFightSituations[s], unsigned(t));
			const u32 sym = vm.find(name);
			if (sym == SCRIPT_NONE || vm.symbol(sym).type != SCRIPT_INSTANCE) continue;
			ScriptInstance* inst = vm.create(sym);
			if (!inst) continue;
			for (u32 k = 0; k < count; ++k) {
				const i32 raw = vm.getInt(moveMember, k, inst);
				const u8 m = engineMove(raw);
				if (raw != 0 && m == FM_NONE) ++unknown;
				moves[(size_t(t) * FS_COUNT + s) * FIGHT_AI_MOVES + k] = m;
			}
			vm.destroy(inst);
			++tables;
			any = true;
		}
		withTables += any;
	}
	char buf[200];
	std::snprintf(buf, sizeof(buf), "fight AI: %u tables for %u of %u tactics%s", unsigned(tables), unsigned(withTables), unsigned(tactics),
	              unknown ? " (some moves unknown to the engine: left out)" : "");
	report = buf;
	if (tables == 0) throw std::runtime_error("none of its fight AI tables could be read");
	return out;
}

} // namespace g3
