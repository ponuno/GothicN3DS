// fix222: script externals that were unbound (the VM swallowed them, answering 0). Only those whose answer matters to
// Gothic 1 live code get a meaning here (as OpenGothic's GameScript: npc_canseesource, floattostring, ...); the rest
// is listed on purpose so that an audit (test "g1 chapter") shows nothing missing by accident.
#include "game.h"
#include "npcs.h"

#include <math.h>
#include <stdio.h>

namespace g3 {

namespace {
// Called for the externals that have nothing to do here: arguments off the stack, the neutral answer back.
void inert(ScriptVm& vm, void*) {
	const u32 fn = vm.callee();
	const ScriptSymbol& s = vm.symbol(fn);
	for (u32 i = s.count; i > 0; --i) {
		const ScriptSymbol& p = vm.symbol(fn + i);
		if (p.type == SCRIPT_FLOAT) (void)vm.popFloat();
		else if (p.type == SCRIPT_INT || p.type == SCRIPT_FUNC) (void)vm.popInt();
		else if (p.type == SCRIPT_STRING) (void)vm.popString();
		else (void)vm.popInstance();
	}
	if (s.flags & SCRIPTF_RETURN) {
		switch (s.extra) {
		case SCRIPT_FLOAT: vm.pushFloat(0.0f); break;
		case SCRIPT_INT:
		case SCRIPT_FUNC: vm.pushInt(0); break;
		case SCRIPT_STRING: vm.pushString(""); break;
		case SCRIPT_INSTANCE: vm.pushInstance(nullptr); break;
		default: break;
		}
	}
}
} // namespace

// Snd_IsSourceNpc(self): the sound (PERC_ASSESSQUIETSOUND ...) came from a character - "other" of the perception.
void Npcs::xSndIsSourceNpc(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const NpcActor* a = self.actorFor(vm.popInstance());
	vm.pushInt(a && a->other >= 0);
}

// Snd_GetDistToSource(self): centimetres to it, 0 when there is none.
void Npcs::xSndDistToSource(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const NpcActor* a = self.actorFor(vm.popInstance());
	i32 d = 0;
	if (a && a->other >= 0) {
		const GameNpc& n = self._game.npc(a->npc);
		const GameNpc& o = self._game.npc(u32(a->other));
		const float dx = o.pos[0] - n.pos[0], dy = o.pos[1] - n.pos[1], dz = o.pos[2] - n.pos[2];
		d = i32(sqrtf(dx * dx + dy * dy + dz * dz));
	}
	vm.pushInt(d);
}

// Npc_CanSeeSource(self): the character that made the sound is in sight (OpenGothic: canSeeSource).
void Npcs::xCanSeeSource(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const NpcActor* a = self.actorFor(vm.popInstance());
	vm.pushInt(a && a->other >= 0 && self.sees(*a, u32(a->other), true));
}

// FloatToString(f) (OpenGothic gothic.cpp: floattostring).
void Npcs::xFloatToString(ScriptVm& vm, void*) {
	static char buf[32];
	snprintf(buf, sizeof(buf), "%g", double(vm.popFloat()));
	vm.pushString(buf);
}

// Npc_IsPlayerInMyRoom(self) / Npc_WasPlayerInMyRoom(self): the portal room of the character is the player's (former) one.
void Npcs::xPlayerInMyRoom(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int n = self.npcFor(vm.popInstance());
	vm.pushInt(n >= 0 && self._game.heroRoom() != 0 && self._game.npc(u32(n)).room == self._game.heroRoom());
}

void Npcs::bindExtras() {
	ScriptVm& vm = _game.vm();
	const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"SND_ISSOURCENPC", xSndIsSourceNpc},
	             {"SND_GETDISTTOSOURCE", xSndDistToSource},
	             {"NPC_CANSEESOURCE", xCanSeeSource},
	             {"FLOATTOSTRING", xFloatToString},
	             {"NPC_ISPLAYERINMYROOM", xPlayerInMyRoom},
	             {"NPC_WASPLAYERINMYROOM", xPlayerInMyRoom}};
	for (const auto& e : table) vm.bind(e.name, e.fn, this);
	// Nothing to do for these in this engine (no cutscene player, no dropped weapons animation, no mission log of
	// Gothic 1's own, ...): they answer 0 / nothing on purpose.
	static const char* const quiet[] = {
	    "SND_ISSOURCEITEM",   "TAL_CONFIGURE",       "TA_BEGINOVERLAY",    "TA_ENDOVERLAY",       "TA_REMOVEOVERLAY",
	    "MDL_STARTFACEANI",   "MDL_APPLYRANDOMFACEANI", "AI_POINTAT",      "AI_LOOKAT",           "AI_ASK",
	    "AI_ASKTEXT",         "AI_WAITFORQUESTION",  "AI_GOTOSOUND",       "AI_TURNTOSOUND",      "AI_WHIRLAROUNDTOSOURCE",
	    "AI_LOOKFORITEM",     "AI_DEFEND",           "AI_COMBATREACTTODAMAGE", "AI_PLAYCUTSCENE",  "AI_CANSEENPC",
	    "AI_TAKEMOB",         "AI_DROPMOB",          "NPC_ISWAYBLOCKED",   "NPC_ISAIMING",        "NPC_HASDETECTEDNPC",
	    "NPC_AREWESTRONGER", "NPC_OWNEDBYGUILD", "AI_DROPITEM", "APPLY_OPTIONS_AUDIO", "APPLY_OPTIONS_CONTROLS", "APPLY_OPTIONS_GAME", "APPLY_OPTIONS_PERFORMANCE", "APPLY_OPTIONS_VIDEO",    "NPC_HASTALENT",       "NPC_HASFIGHTTALENT", "NPC_HASOFFERED",      "NPC_ISVOICEACTIVE",
	    "NPC_SETTELEPORTPOS", "NPC_STARTITEMREACTMODULES", "NPC_GETCOMRADES", "NPC_CREATESPELL",   "NPC_LEARNSPELL",
	    "NPC_CHECKAVAILABLEMISSION", "NPC_CHECKOFFERMISSION", "NPC_CHECKRUNNINGMISSION", "NPC_OFFERMISSION",
	    "MIS_ADDMISSIONENTRY", "MIS_GETSTATUS",      "MIS_ONTIME",         "MIS_REMOVEMISSION",   "MIS_SETSTATUS",
	    "NPC_GIVEINFO",       "RTN_EXCHANGE",        "SETPERCENTDONE",     "UPDATE_CHOICEBOX",    "PRINTDIALOG",
	    "PRINTMULTI",         "DOC_FONT",            "DOC_MAPCOORDINATES", "DOC_OPEN",            "DOC_PRINT",
	    "WLD_ASSIGNROOMTONPC", "WLD_GETPLAYERPORTALOWNER", "WLD_GETFORMERPLAYERPORTALOWNER",
	    "WLD_INSERTNPCANDRESPAWN", "WLD_INSERTOBJECT", "HLP_CUTSCENEPLAYED", "TA_CS", "EXITGAME"};
	for (const char* name : quiet) {
		const u32 s = vm.find(name);
		// Never replace what another group has bound already.
		if (s != SCRIPT_NONE && !vm.isBound(s)) vm.bind(name, inert, this);
	}
}

} // namespace g3
