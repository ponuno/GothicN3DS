// Talks. The player's A at a character makes it hear PERC_ASSESSTALK, and its
// scripts do the rest (B_AssessTalk -> AI_StartState(ZS_Talk) -> the greeting
// -> AI_ProcessInfos). From there the engine keeps the book of the talk, as
// the original's info manager does:
//  * the infos the character has (C_INFO, game_dialog.cpp) that can be told
//    now make the menu; the ones marked important are told first, by
//    themselves;
//  * an info that is chosen is run: its function says what is said
//    (AI_Output - the words come from the archive, dialog_text.h) and what
//    is offered next (Info_AddChoice);
//  * the talk ends when the scripts say so (AI_StopProcessInfos), and
//    ZS_Talk_Loop finds that out through InfoManager_HasFinished.
//
// Words. Every AI_Output of the player's character or a character in sight
// is a step of one queue that is said one after the other, whoever speaks, so
// that a reply never comes before the question. A character's own step waits
// until its queue has got as far as the line (its earlier actions are done
// first); the queue goes on from there.
#include "npcs.h"
#include "ui.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

void copyText(char* to, size_t size, const char* from) {
	size_t n = from ? strlen(from) : 0;
	if (n >= size) n = size - 1;
	if (n) memcpy(to, from, n);
	to[n] = 0;
}

u32 utf8Length(const char* s) {
	u32 n = 0;
	while (utf8Next(s)) ++n;
	return n;
}

} // namespace

NpcActor* Npcs::actorByNpc(u32 npc) {
	for (NpcActor& a : _actors)
		if (a.used && a.npc == npc) return &a;
	return nullptr;
}

// ---- the words ----

bool Npcs::speechPending(u32 id) const {
	if (_say.active && _say.id == id) return true;
	for (u32 k = 0; k < _speechCount; ++k)
		if (_speech[(_speechHead + k) % SPEECH].id == id) return true;
	return false;
}

void Npcs::flushSpeech() {
	_speechHead = _speechCount = 0;
	_speechDone = _speechNext;
	_say.active = false;
	_sayText[0] = 0;
	_speechWait = 0;
}

void Npcs::tickSpeech(float dt) {
	if (_say.active) {
		_say.time += dt;
		if (_say.time >= _say.total) {
			_speechDone = _say.id + 1;
			_say.active = false;
			_sayText[0] = 0;
		}
	}
	while (!_say.active && _speechCount) {
		const SpeechStep s = _speech[_speechHead];
		enum { PLAY, WAIT, DROP } what = PLAY;
		if (s.speaker >= 0 && u32(s.speaker) < _game.npcCount() && !_game.npc(u32(s.speaker)).hero) {
			// The character says it when its queue has got to the line.
			const NpcActor* a = actorByNpc(u32(s.speaker));
			if (!a)
				what = DROP;
			else if (a->action.kind == NPC_ACT_OUTPUT && a->action.i == i32(s.id))
				what = PLAY;
			else if (a->action.kind == NPC_ACT_NONE && !a->queueCount)
				what = DROP; // (its queue was emptied: nobody will say it)
			else
				what = WAIT;
		}
		if (what == WAIT) {
			_speechWait += dt;
			if (_speechWait < 8.0f) break;
			what = DROP;
		}
		_speechHead = u8((_speechHead + 1) % SPEECH);
		--_speechCount;
		_speechWait = 0;
		if (what == DROP) {
			_speechDone = s.id + 1;
			continue;
		}
		++_stats.lines;
		const char* raw = _texts.find(s.hash);
		_sayText[0] = 0;
		float total = 0.5f;
		if (raw && *raw) {
			scriptText(raw, _sayText, sizeof(_sayText));
			// No voices: the time it takes to read it.
			total = clamp(1.2f + 0.055f * float(utf8Length(_sayText)), 1.8f, 9.0f);
		} else {
			++_stats.linesMissing;
		}
		_say.active = true;
		_say.id = s.id;
		_say.speaker = s.speaker;
		_say.time = 0;
		_say.total = total;
	}
}

void Npcs::xOutput(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const u32 hash = hashName(vm.popString());
	(void)vm.popInstance(); // (whom it is said to: the player's character or the one talked to)
	ScriptInstance* const speakerInst = vm.popInstance();
	const int speaker = self.npcFor(speakerInst);
	if (speaker < 0 || !self._cfg.alive) return;
	const bool hero = self._game.npc(u32(speaker)).hero;
	NpcActor* a = hero ? nullptr : self.actorFor(speakerInst);
	if (!hero && !a) return; // out of sight, unheard
	if (self._speechCount >= SPEECH) {
		++self._stats.queueFull;
		return;
	}
	const u32 id = self._speechNext++;
	if (a) {
		NpcAction act;
		act.kind = NPC_ACT_OUTPUT;
		act.i = i32(id);
		if (!self.push(a, act)) {
			--self._speechNext;
			return;
		}
	}
	self._speech[(self._speechHead + self._speechCount) % SPEECH] = SpeechStep {hash, speaker, id};
	++self._speechCount;
}

// ---- the talk ----

void Npcs::xProcessInfos(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_PROCESS_INFOS;
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xStopProcessInfos(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_STOP_INFOS;
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xInfoManagerHasFinished(ScriptVm& vm, void* user) { vm.pushInt(!static_cast<Npcs*>(user)->_talk.processing); }

void Npcs::bindDialog() {
	ScriptVm& vm = _game.vm();
	const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"AI_OUTPUT", xOutput},
	             {"AI_PROCESSINFOS", xProcessInfos},
	             {"AI_STOPPROCESSINFOS", xStopProcessInfos},
	             {"INFOMANAGER_HASFINISHED", xInfoManagerHasFinished}};
	for (const auto& e : table) vm.bind(e.name, e.fn, this);
	_percTalk = _game.constant("PERC_ASSESSTALK", -1);
	_fnTalk = _game.function("ZS_TALK");
}

bool Npcs::canTalk(u32 npc) const {
	if (!_cfg.alive || npc >= _game.npcCount() || npc == _game.heroIndex() || _game.npc(npc).down != DOWN_NONE) return false;
	for (const NpcActor& a : _actors)
		if (a.used && a.npc == npc)
			return a.ready && ((_percTalk >= 0 && _percTalk < Game::PERC_TYPES && a.perc[_percTalk]) || _game.hasInfos(npc));
	return false;
}

bool Npcs::talkTo(u32 npc) {
	if (talking() || !_cfg.alive || _game.heroIndex() == SCRIPT_NONE || npc >= _game.npcCount() || npc == _game.heroIndex()) return false;
	NpcActor* a = actorByNpc(npc);
	if (!a || !a->ready || _game.npc(npc).down != DOWN_NONE) return false;
	const int hero = int(_game.heroIndex());
	_talk = Talk();
	_talk.requested = int(npc);
	if (_percTalk >= 0 && _percTalk < Game::PERC_TYPES && a->perc[_percTalk] && a->state && a->started) {
		react(*a, a->perc[_percTalk], hero, -1);
	} else if (_fnTalk != SCRIPT_NONE) {
		// It does not listen for talk in what it is doing: the talk is begun for it.
		startState(*a, _fnTalk, true, "", false);
		a->other = hero;
	} else {
		_talk.requested = -1;
		return false;
	}
	refreshView();
	return true;
}

void Npcs::openTalk(NpcActor& a) {
	if (_talk.processing && _talk.npc == int(a.npc)) return;
	if (_talk.processing) talkAbort();
	_talk = Talk();
	_talk.processing = true;
	_talk.npc = int(a.npc);
	_talk.state = a.state;
	_talk.importantLeft = 8;
	++_stats.talks;
	_game.clearAllChoices();
	_menuDirty = true;
}

void Npcs::finishTalk() {
	_talk = Talk();
	_game.clearAllChoices();
	_menuDirty = true;
}

void Npcs::talkAbort() {
	if (!talking()) return;
	flushSpeech();
	finishTalk();
	refreshView();
}

void Npcs::buildMenu() {
	Talk& t = _talk;
	t.count = 0;
	if (t.menuInfo >= 0) {
		u32 ways[TALK_ITEMS];
		const u32 n = _game.choicesOf(u32(t.menuInfo), ways, TALK_ITEMS);
		for (u32 i = 0; i < n; ++i) {
			t.ref[i] = ways[i];
			t.isChoice[i] = true;
		}
		t.count = n;
		if (!n) t.menuInfo = -1;
	}
	if (!t.count) {
		u32 list[TALK_ITEMS];
		const u32 n = _game.infosFor(u32(t.npc), false, list, TALK_ITEMS);
		for (u32 i = 0; i < n; ++i) {
			t.ref[i] = list[i];
			t.isChoice[i] = false;
		}
		t.count = n;
	}
	_menuDirty = true;
}

void Npcs::runInfo(u32 info) {
	_game.clearChoices(info);
	_talk.menuInfo = int(info);
	_talk.choosing = false;
	++_stats.infosTold;
	_game.runInfo(info, u32(_talk.npc));
	_menuDirty = true;
}

// What was said and done is over: the next important info, or the menu - or the end.
void Npcs::afterTold() {
	Talk& t = _talk;
	if (t.stop) {
		finishTalk();
		return;
	}
	while (t.importantLeft > 0) {
		u32 next[1];
		if (_game.infosFor(u32(t.npc), true, next, 1) == 0) break;
		--t.importantLeft;
		runInfo(next[0]);
		return;
	}
	t.importantLeft = 0;
	buildMenu();
	if (!t.count) {
		finishTalk();
		return;
	}
	t.choosing = true;
}

bool Npcs::talkChoose(u32 index) {
	Talk& t = _talk;
	if (!t.processing || !t.choosing || index >= t.count) return false;
	const u32 ref = t.ref[index];
	if (!t.isChoice[index]) {
		runInfo(ref);
		return true;
	}
	t.choosing = false;
	const u32 fn = _game.choice(ref).fn;
	const u32 adds = _game.choiceAdds();
	++_stats.infosTold;
	_game.callAs(fn, u32(t.npc), int(_game.heroIndex()));
	// A way on that put up new ways takes the menu over.
	if (_game.choiceAdds() != adds) t.menuInfo = int(_game.lastChoiceInfo());
	_menuDirty = true;
	return true;
}

void Npcs::talkSkip() {
	if (_say.active && _say.time > 0.25f) _say.time = _say.total;
}

bool Npcs::talkBack() {
	if (!_talk.processing) return false;
	if (_talk.choosing && _talk.count) return talkChoose(_talk.count - 1);
	talkSkip();
	return true;
}

void Npcs::tickTalk(float dt) {
	tickSpeech(dt);
	Talk& t = _talk;
	if (!t.processing) {
		if (t.requested >= 0) {
			t.wait += dt;
			if (t.wait > 8.0f) {
				// It never began (the character is busy, or has no time for talk).
				++_stats.talksAborted;
				t.requested = -1;
			}
		}
		refreshView();
		return;
	}
	NpcActor* a = actorByNpc(u32(t.npc));
	const GameNpc& n = _game.npc(u32(t.npc));
	const GameNpc* hero = _game.hero();
	bool gone = !a || !a->ready || n.down != DOWN_NONE || a->state != t.state;
	if (!gone && hero) {
		const float dx = hero->pos[0] - n.pos[0], dz = hero->pos[2] - n.pos[2];
		gone = dx * dx + dz * dz > 1500.0f * 1500.0f;
	}
	if (gone) {
		// Hurt, called away, left behind: the talk is over.
		++_stats.talksAborted;
		talkAbort();
		return;
	}
	if (!t.choosing && a->action.kind == NPC_ACT_NONE && !a->queueCount && !_say.active && !_speechCount) afterTold();
	refreshView();
}

void Npcs::refreshView() {
	TalkView& v = _talkView;
	const Talk& t = _talk;
	const bool active = talking();
	const bool choosing = t.processing && t.choosing;
	const bool saying = _say.active && _sayText[0];
	const bool heroSays = saying && _say.speaker >= 0 && _game.npc(u32(_say.speaker)).hero;
	bool changed = _menuDirty || v.active != active || v.choosing != choosing || v.saying != saying || v.heroSays != heroSays;
	if (saying && strcmp(v.line, _sayText) != 0) changed = true;
	if (!changed) return;
	v.active = active;
	v.choosing = choosing;
	v.saying = saying;
	v.heroSays = heroSays;
	copyText(v.line, sizeof(v.line), saying ? _sayText : "");
	const int partner = t.processing ? t.npc : t.requested;
	v.partner[0] = 0;
	if (partner >= 0 && active) scriptText(_game.nameOf(_game.npc(u32(partner))), v.partner, sizeof(v.partner));
	v.speaker[0] = 0;
	if (saying && _say.speaker >= 0) scriptText(_game.nameOf(_game.npc(u32(_say.speaker))), v.speaker, sizeof(v.speaker));
	v.count = choosing ? t.count : 0;
	for (u32 i = 0; i < v.count; ++i) {
		const char* text = t.isChoice[i] ? _game.choice(t.ref[i]).text : _game.infoDescription(t.ref[i]);
		scriptText(text, v.items[i], sizeof(v.items[i]));
	}
	_menuDirty = false;
	++v.version;
}

} // namespace g3
