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
#include "lang.h"
#include "readahead.h"
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
	if (_speechHook.stop) _speechHook.stop(_speechHook.user);
}

void Npcs::tickSpeech(float dt) {
	if (_overlayBarrier > 0.0f) _overlayBarrier = max(0.0f, _overlayBarrier - dt);
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
		float patience = 8.0f;
		if (s.call == WAIT_FOR_ACTOR) {
			// fix199: the player's AI_WaitTillEnd: his next lines wait for what `speaker` was given before it.
			const NpcActor* a = s.speaker >= 0 ? actorByNpc(u32(s.speaker)) : nullptr;
			const bool done = !a || a->actionSerial > s.hash || (a->actionSerial == s.hash && a->action.kind == NPC_ACT_NONE) ||
			                  // (not for its own line further on, nor for something that waits for the player)
			                  a->action.kind == NPC_ACT_OUTPUT || a->action.kind == NPC_ACT_WAIT_END;
			if (!done) {
				_speechWait += dt;
				if (_speechWait < 30.0f) break;
			}
			_speechHead = u8((_speechHead + 1) % SPEECH);
			--_speechCount;
			_speechWait = 0;
			_speechDone = s.id + 1;
			continue;
		}
		if (s.speaker >= 0 && u32(s.speaker) < _game.npcCount() && !_game.npc(u32(s.speaker)).hero) {
			// The character says it when its queue has got to the line.
			const NpcActor* a = actorByNpc(u32(s.speaker));
			if (!a)
				what = DROP;
			else if (a->action.kind == NPC_ACT_OUTPUT && a->action.i == i32(s.id))
				what = PLAY;
			else if (a->action.kind == NPC_ACT_NONE && !a->queueCount)
				what = DROP; // (its queue was emptied: nobody will say it)
			else {
				what = WAIT;
				// fix199: while the line is still in its queue it is waited for longer (it walks somewhere first,
				// plays an animation); 8 s dropped lines of long talks.
				for (u32 k = 0; k < a->queueCount; ++k) {
					const NpcAction& q = a->queue[(a->queueHead + k) % NpcActor::QUEUE];
					if (q.kind == NPC_ACT_OUTPUT && q.i == i32(s.id)) patience = 30.0f;
				}
			}
		}
		if (what == WAIT) {
			_speechWait += dt;
			if (_speechWait < patience) break;
			what = DROP;
		}
		_speechHead = u8((_speechHead + 1) % SPEECH);
		--_speechCount;
		_speechWait = 0;
		if (what == DROP) {
			_speechDone = s.id + 1;
			continue;
		}
		if (s.call >= 0) {
			// fix196: the player's AI_Function, in its place among the lines.
			_speechDone = s.id + 1;
			if (s.speaker >= 0 && u32(s.speaker) < _game.npcCount()) _game.ikarus().callTicket(_game.npc(u32(s.speaker)).object, s.call);
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
		// With a recording the line lasts as long as it does (and a breath more).
		if (_speechHook.play) {
			const bool hero = s.speaker >= 0 && u32(s.speaker) < _game.npcCount() && _game.npc(u32(s.speaker)).hero;
			const float voice = _speechHook.play(_speechHook.user, s.hash, s.speaker, hero);
			if (voice > 0.0f) total = voice + 0.3f;
		}
		_say.active = true;
		_say.id = s.id;
		_say.speaker = s.speaker;
		_say.time = 0;
		_say.total = total;
	}
}

// Whether this speaker's words belong to the player's talk (shown and spoken in turn): the player's character
// and the one it is talking to. Anybody else speaks in the world.
bool Npcs::talkLineOf(int speaker) const {
	if (speaker < 0 || u32(speaker) >= _game.npcCount()) return false;
	if (_game.npc(u32(speaker)).hero) return true;
	return _talk.processing && _talk.npc == speaker;
}

void Npcs::queueTalkLine(ScriptInstance* speakerInst, u32 hash) {
	const int speaker = npcFor(speakerInst);
	if (speaker < 0 || !_cfg.alive) return;
	const bool hero = _game.npc(u32(speaker)).hero;
	NpcActor* a = hero ? nullptr : actorFor(speakerInst);
	if (!hero && !a) return; // out of sight, unheard
	if (_speechCount >= SPEECH) {
		++_stats.queueFull;
		return;
	}
	const u32 id = _speechNext++;
	if (a) {
		NpcAction act;
		act.kind = NPC_ACT_OUTPUT;
		act.i = i32(id);
		if (!push(a, act)) {
			--_speechNext;
			return;
		}
	}
	_speech[(_speechHead + _speechCount) % SPEECH] = SpeechStep {hash, speaker, id, -1};
	++_speechCount;
	prefetchLine(hash, false);
}

// fix204: the lines of an info are queued all at once, seconds before each is said: their words and recordings are
// read from the card now, off the frame (the words through the read-ahead, the recording by the voices' thread).
void Npcs::prefetchLine(u32 hash, bool world) {
	if (!hash) return;
	if (_ahead && _ahead->running() && _ahead->enabled()) _texts.prefetch(hash, *_ahead);
	if (_speechHook.prefetch) _speechHook.prefetch(_speechHook.user, hash, world);
}

// fix196: LeGo's AI_Function on the player's character, who has no queue: a step of the words' queue, run when the
// lines before it have been said (outside a talk at once).
void Npcs::queueHeroCall(ScriptInstance* heroInst, i32 ticket) {
	const int who = npcFor(heroInst);
	if (who < 0 || !_talk.processing || _speechCount >= SPEECH) {
		_game.ikarus().callTicket(heroInst, ticket);
		return;
	}
	SpeechStep st {0, who, _speechNext++, ticket};
	_speech[(_speechHead + _speechCount) % SPEECH] = st;
	++_speechCount;
}

// fix199: AI_WaitTillEnd(player, other) in a talk: the player's next lines wait until `other` has done what it was
// given so far (a step of the words' queue).
void Npcs::queueHeroWait(int other) {
	const NpcActor* a = other >= 0 ? actorByNpc(u32(other)) : nullptr;
	if (!a || !_talk.processing || _speechCount >= SPEECH) return;
	// The action that ends what it has now: the last of its queue (the serial it will get) or the current one.
	const u32 last = a->actionSerial + a->queueCount;
	if (last == a->actionSerial && a->action.kind == NPC_ACT_NONE) return;
	SpeechStep st {last, other, _speechNext++, WAIT_FOR_ACTOR};
	_speech[(_speechHead + _speechCount) % SPEECH] = st;
	++_speechCount;
}

// A line said where its speaker stands, outside the player's talk (OpenGothic: GameScript::GlobalOutput). It sounds
// from the speaker, shows no subtitle, and the speaker is busy as long as the recording lasts. `svm`: the phrase
// ("$WHATDOYOUWANT"), else `hash` is the line.
bool Npcs::queueWorldLine(ScriptInstance* speakerInst, int target, u32 hash, const char* svm) {
	if (!_cfg.alive) return false;
	NpcActor* a = actorFor(speakerInst);
	if (!a) return false; // out of sight, unheard
	NpcAction act;
	act.kind = NPC_ACT_SAY;
	act.i = target;
	act.hash = hash;
	act.order = _sayOrder++;
	if (svm) copyText(act.text, sizeof(act.text), svm);
	if (!push(a, act)) return false;
	if (!svm) prefetchLine(hash, true); // (an SVM phrase is named only when said)
	return true;
}

void Npcs::xOutput(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const u32 hash = hashName(vm.popString());
	const int target = self.npcFor(vm.popInstance());
	ScriptInstance* const speakerInst = vm.popInstance();
	if (self.talkLineOf(self.npcFor(speakerInst))) self.queueTalkLine(speakerInst, hash);
	else self.queueWorldLine(speakerInst, target, hash, nullptr);
}

// ---- the talk ----

void Npcs::xProcessInfos(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	ScriptInstance* inst = vm.popInstance();
	const int who = self.npcFor(inst);
	if (who >= 0 && u32(who) == self._game.heroIndex()) {
		// The player's character with himself (a bed, a workbench: his own infos - OpenGothic's
		// GameScript::ai_processinfos on the player opens the dialogue with the player as both).
		self.openSelfTalk();
		return;
	}
	NpcAction act;
	act.kind = NPC_ACT_PROCESS_INFOS;
	self.push(self.actorFor(inst), act);
}

void Npcs::xStopProcessInfos(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	ScriptInstance* inst = vm.popInstance();
	const int who = self.npcFor(inst);
	if (who >= 0 && u32(who) == self._game.heroIndex()) {
		// (The player's character has no queue: the talk ends once what is said has been said.)
		if (self._talk.processing) self._talk.stop = true;
		return;
	}
	NpcAction act;
	act.kind = NPC_ACT_STOP_INFOS;
	self.push(self.actorFor(inst), act);
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
		if (a.used && a.npc == npc) {
			// (Not one in a fight: it has no ear for talk, nor does an enemy.)
			if (!a.ready || inFightState(a) || a.flee > 0.0f || a.refuseTalk > 0.0f || _game.npc(npc).weapon != WEAPON_NONE) return false;
			const u32 hero = _game.heroIndex();
			if (hero != SCRIPT_NONE && _game.attitude(npc, hero) == _game.constant("ATT_HOSTILE", 0) && _game.isMonster(_game.npc(npc))) return false;
			return (_percTalk >= 0 && _percTalk < Game::PERC_TYPES && a.perc[_percTalk]) || _game.hasInfos(npc);
		}
	return false;
}

bool Npcs::talkTo(u32 npc) {
	if (talking() || !_cfg.alive || _game.heroIndex() == SCRIPT_NONE || npc >= _game.npcCount() || npc == _game.heroIndex()) return false;
	NpcActor* a = actorByNpc(npc);
	if (!a || !a->ready || _game.npc(npc).down != DOWN_NONE) return false;
	const int hero = int(_game.heroIndex());
	_talk = Talk();
	_talk.requested = int(npc);
	// Whoever is on his way somewhere, waiting or in the middle of a gesture is stopped on the
	// spot: the state the talk begins would otherwise stand in the queue behind the walk (which
	// can be a long one) and the player would run after him in vain. It looks at the player.
	if (a->phase != NpcActor::IDLE || a->action.kind != NPC_ACT_NONE || a->queueCount) {
		abortActions(*a);
		++_stats.talksStopped;
	}
	a->lookAt = hero;
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

void Npcs::openSelfTalk() {
	const u32 hero = _game.heroIndex();
	if (hero == SCRIPT_NONE || _talk.processing) return;
	_talk = Talk();
	_talk.processing = true;
	_talk.self = true;
	_talk.npc = int(hero);
	_talk.importantLeft = 8;
	++_stats.talks;
	_game.clearAllChoices();
	_menuDirty = true;
	refreshView();
}

namespace {
// The engine's sleep menu: until when (hours), the last one is "not now".
const float kSleepHours[4] = {8.0f, 12.0f, 20.0f, 0.0f};
const char* sleepItem(u32 i) {
	switch (i) {
	case 0: return tr("Спать до утра");
	case 1: return tr("Спать до полудня");
	case 2: return tr("Спать до вечера");
	case 3: return tr("Спать до полуночи");
	default: return tr("Не спать");
	}
}
} // namespace

bool Npcs::openSleepMenu() {
	const u32 hero = _game.heroIndex();
	if (hero == SCRIPT_NONE || talking()) return false;
	_talk = Talk();
	_talk.processing = true;
	_talk.self = true;
	_talk.sleepMenu = true;
	_talk.npc = int(hero);
	_talk.choosing = true;
	_talk.count = 5;
	_menuDirty = true;
	refreshView();
	return true;
}

bool Npcs::takeSleep(float& hours) {
	if (_sleepTo < 0.0f) return false;
	hours = _sleepTo;
	_sleepTo = -1.0f;
	return true;
}

void Npcs::finishTalk() {
	// (ZS_Talk_Loop clears the flag itself, but a talk cut short never gets there: and with it left set every
	// fighter only waits - AI_Wait in ZS_Attack - while the player is hit.)
	if (_game.heroIndex() != SCRIPT_NONE) _game.clearTalkFlag(_game.heroIndex());
	if (_talk.npc >= 0) _game.clearTalkFlag(u32(_talk.npc));
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
	_talk.tradeNext = _game.info(info).trade;
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
	if (t.tradeNext) {
		// What the info said is said: now the wares (OpenGothic: DialogMenu::startTrade once the output is done).
		t.tradeNext = false;
		t.trading = true;
		++_stats.trades;
		_menuDirty = true;
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
	if (t.sleepMenu) {
		if (index < 4) _sleepTo = kSleepHours[index];
		finishTalk();
		refreshView();
		return true;
	}
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
	if (_say.active && _say.time > 0.25f) {
		_say.time = _say.total;
		if (_speechHook.stop) _speechHook.stop(_speechHook.user);
	}
}

void Npcs::tradeClose() {
	if (!_talk.processing || !_talk.trading) return;
	_talk.trading = false;
	_menuDirty = true;
	afterTold(); // (the menu again, or the end if the scripts said so)
	refreshView();
}

bool Npcs::talkBack() {
	if (!_talk.processing) return false;
	if (_talk.trading) {
		tradeClose();
		return true;
	}
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
	if (t.self) {
		// With himself: over when he is down, or when the scripts say so and all is said.
		const GameNpc* me = _game.hero();
		if (!me || me->down != DOWN_NONE) {
			talkAbort();
			return;
		}
		if (!t.choosing && !t.trading && !_say.active && !_speechCount) afterTold();
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
	if (!t.choosing && !t.trading && a->action.kind == NPC_ACT_NONE && !a->queueCount && !_say.active && !_speechCount) afterTold();
	refreshView();
}

void Npcs::refreshView() {
	TalkView& v = _talkView;
	const Talk& t = _talk;
	const bool active = talking();
	const bool choosing = t.processing && t.choosing;
	const bool saying = _say.active && _sayText[0];
	const bool heroSays = saying && _say.speaker >= 0 && _game.npc(u32(_say.speaker)).hero;
	const bool trading = t.processing && t.trading;
	bool changed = _menuDirty || v.active != active || v.choosing != choosing || v.saying != saying || v.heroSays != heroSays || v.trading != trading;
	if (saying && strcmp(v.line, _sayText) != 0) changed = true;
	if (!changed) return;
	v.active = active;
	v.choosing = choosing;
	v.saying = saying;
	v.trading = trading;
	v.heroSays = heroSays;
	copyText(v.line, sizeof(v.line), saying ? _sayText : "");
	const int partner = t.processing ? t.npc : t.requested;
	v.partner[0] = 0;
	if (partner >= 0 && active) scriptText(_game.nameOf(_game.npc(u32(partner))), v.partner, sizeof(v.partner));
	v.speaker[0] = 0;
	if (saying && _say.speaker >= 0) scriptText(_game.nameOf(_game.npc(u32(_say.speaker))), v.speaker, sizeof(v.speaker));
	v.count = choosing ? t.count : 0;
	for (u32 i = 0; i < v.count; ++i) {
		if (t.sleepMenu) {
			copyText(v.items[i], sizeof(v.items[i]), sleepItem(i));
			continue;
		}
		const char* text = t.isChoice[i] ? _game.choice(t.ref[i]).text : _game.infoDescription(t.ref[i]);
		scriptText(text, v.items[i], sizeof(v.items[i]));
	}
	if (t.sleepMenu && active) copyText(v.partner, sizeof(v.partner), tr("Сон"));
	_menuDirty = false;
	++v.version;
}

} // namespace g3
