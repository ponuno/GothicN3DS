// The script of the built-in test scene: a handful of characters and
// creatures put into the two synthetic worlds the way Gothic's own scripts
// populate theirs - the same classes, the same engine functions, the same
// idioms (prototypes, daily routines through TA_ wrappers, start-up
// functions named after the world).
#include "daedalus.h"
#include "importers.h"

namespace g3 {

const char* syntheticScriptSource() {
	return R"DAEDALUS(
// ---------------------------------------------------------------- constants
const int ATR_HITPOINTS     = 0;
const int ATR_HITPOINTS_MAX = 1;
const int ATR_MANA          = 2;
const int ATR_MANA_MAX      = 3;
const int ATR_STRENGTH      = 4;
const int ATR_DEXTERITY     = 5;
const int ATR_INDEX_MAX     = 8;
const int PROT_INDEX_MAX    = 8;
const int DAM_INDEX_MAX     = 8;
const int MAX_MISSIONS      = 5;

const int GIL_NONE = 0;
const int GIL_GRD  = 2;
const int GIL_VLK  = 3;
const int GIL_SCAVENGER = 20;
const int GIL_MOLERAT   = 21;

const int NPCTYPE_MAIN  = 1;
const int NPCTYPE_GUARD = 2;
const int NPC_FLAG_IMMORTAL = 1 << 1;

const int ITEM_KAT_ARMOR = 1 << 4;
// The inventory (Constants.d of the original).
const int ITEM_KAT_NONE    = 1 << 0;
const int ITEM_KAT_MUN     = 1 << 3;
const int ITEM_KAT_FOOD    = 1 << 5;
const int ITEM_KAT_DOCS    = 1 << 6;
const int ITEM_KAT_POTIONS = 1 << 7;
const int ITEM_KAT_RUNE    = 1 << 9;
const int ITEM_KAT_MAGIC   = 1 << 31;
const int ITEM_RING   = 1 << 11;
const int ITEM_BOW    = 1 << 19;
const int ITEM_MULTI  = 1 << 21;
const int ITEM_AMULET = 1 << 22;
const int PROT_EDGE = 2;
const int PROT_FIRE = 3;
const int DEFAULT = 0;
const int TRUE = 1;
const int FALSE = 0;

const int NPC_RUN  = 0;
const int NPC_WALK = 1;
const int LOOP_CONTINUE = 0;
const int LOOP_END      = 1;
const int BS_FLAG_INTERRUPTABLE = 32768;
const int BS_FLAG_FREEHANDS     = 65536;
const int BS_MAX = 31;
const int BS_STAND = 98304;       // 0, interruptable, with free hands
const int BS_SIT = 65547;         // 11, with free hands
const int BS_LIE = 12;
const int BS_MOBINTERACT = 15;
const int BS_MOBINTERACT_INTERRUPT = 32784; // 16, interruptable
// Which kinds of things one sits on and lies in (the engine reads these).
const string MOB_SIT = "BENCH,CHAIR,GROUND,THRONE";
const string MOB_LIE = "BED,BEDHIGH,BEDLOW";
const int PERC_ASSESSPLAYER = 1;
const int PERC_ASSESSENEMY  = 2;
const int PERC_ASSESSWARN   = 20;
const int PERC_ASSESSTALK   = 11;
const int ATT_HOSTILE  = 0;
const int ATT_ANGRY    = 1;
const int ATT_NEUTRAL  = 2;
const int ATT_FRIENDLY = 3;
const int AIV_SEENPLAYER = 4;

const int AIV_WASDEFEATED = 3;
const int AIV_PARTYMEMBER = 12;
const int AIV_INVINCIBLE = 13;
const int AIV_IMPORTANT = 14;

// Fighting (the original's Fight.d, AI_Constants.d, Species.d - the parts of
// them the engine reads, and the states that use them).
const int GIL_HUMAN = 1;
const int GIL_SEPERATOR_HUM = 16;
const int GIL_MAX = 24;
const int PERC_ASSESSFIGHTER = 3;
const int PERC_ASSESSMURDER = 6;
const int PERC_ASSESSDEFEAT = 7;
const int PERC_ASSESSDAMAGE = 8;
const int PERC_ASSESSOTHERSDAMAGE = 9;
const int PERC_ASSESSTHREAT = 10;
const int PERC_ASSESSFIGHTSOUND = 13;
const int PERC_DRAWWEAPON = 24;
const int BS_RUN = 3;
const int BS_STUMBLE = 21;
const int BS_UNCONSCIOUS = 22;
const int BS_DEAD = 23;
const int BS_HIT = 32794;          // 26, interruptable
const int BS_PARADE = 27;
const int FMODE_NONE = 0;
const int FMODE_FIST = 1;
const int FMODE_MELEE = 2;
const int FMODE_MAGIC = 7;
const int ITEM_KAT_NF = 1 << 1;
const int ITEM_KAT_FF = 1 << 2;
const int ITEM_SWD = 1 << 14;
const int ITEM_2HD_SWD = 1 << 16;
const int ITEM_2HD_AXE = 1 << 17;
const int DAM_BLUNT = 2;
const int DAM_FIRE = 8;
// Magic (Spells_Params.d of the original, the spells the scene has).
const int MAX_SPELL = 18;
const int SPL_LIGHT = 0;
const int SPL_FIREBALL = 1;
const int SPL_HEAL = 4;
const int SPL_FIREBOLT = 17;
const int SPL_DONTINVEST = 0;
const int SPL_RECEIVEINVEST = 1;
const int SPL_SENDCAST = 2;
const int SPL_SENDSTOP = 3;
const int SPL_NEXTLEVEL = 4;
const int SPL_SENDCAST_FIREBOLT = 1;
const int SPL_SENDCAST_FIREBALL = 5;
const int SPL_DAMAGE_FIREBOLT = 30;
const int SPL_DAMAGE_FIREBALL = 30;
const int SPL_HEALING_HP_PER_MP = 10;
const int SPELL_GOOD = 0;
const int SPELL_NEUTRAL = 1;
const int SPELL_BAD = 2;
const int TARGET_COLLECT_NONE = 0;
const int TARGET_COLLECT_CASTER = 1;
const int TARGET_COLLECT_FOCUS = 2;
const int TARGET_COLLECT_ALL = 3;
const int TARGET_COLLECT_ALL_FALLBACK_NONE = 4;
const int TARGET_COLLECT_ALL_FALLBACK_CASTER = 5;
const int TARGET_COLLECT_FOCUS_FALLBACK_NONE = 6;
const int TARGET_COLLECT_FOCUS_FALLBACK_CASTER = 7;
const int DAM_EDGE = 4;
const int DAM_INDEX_BLUNT = 1;
const int DAM_INDEX_EDGE = 2;
const int DAM_CRITICAL_MULTIPLIER = 2;
const int HAI_TIME_UNCONSCIOUS = 20;
const int FAI_HUMAN_STRONG = 3;
const int FAI_SCAVENGER = 15;
const int FAI_MOLERAT = 17;

const string NAME_Guard  = "Guard";
const string NAME_Digger = "Digger";

// ---------------------------------------------------------------- classes
class C_NPC {
	var int    id;
	var string name[5];
	var string slot;
	var int    npctype;
	var int    flags;
	var int    attribute[ATR_INDEX_MAX];
	var int    protection[PROT_INDEX_MAX];
	var int    damage[DAM_INDEX_MAX];
	var int    damagetype;
	var int    guild, level;
	var func   mission[MAX_MISSIONS];
	var int    fight_tactic;
	var int    weapon;
	var int    voice;
	var int    voicepitch;
	var int    bodymass;
	var func   daily_routine;
	var func   start_aistate;
	var string spawnpoint;
	var int    spawndelay;
	var int    senses;
	var int    senses_range;
	var int    aivar[50];
	var string wp;
	var int    exp;
	var int    exp_next;
	var int    lp;
};

class C_ITEM {
	var int    id;
	var string name, nameid;
	var int    hp, hp_max;
	var int    mainflag, flags;
	var int    weight, value;
	var int    damagetype;
	var int    damagetotal;
	var int    damage[DAM_INDEX_MAX];
	var int    wear;
	var int    protection[PROT_INDEX_MAX];
	var int    nutrition;
	var int    cond_atr[3];
	var int    cond_value[3];
	var int    change_atr[3];
	var int    change_value[3];
	var func   magic;
	var func   on_equip;
	var func   on_unequip;
	var func   on_state[4];
	var func   owner;
	var int    ownerguild;
	var int    disguiseguild;
	var string visual;
	var string visual_change;
	var int    visual_skin;
	var string scemename;
	var int    material;
	var int    munition;
	var int    spell;
	var int    range;
	var int    mag_circle;
	var string description;
	var string text[6];
	var int    count[6];
};

class C_Spell {
	var float time_per_mana;
	var int   damage_per_level;
	var int   damageType;
	var int   spellType;
	var int   canTurnDuringInvest;
	var int   canChangeTargetDuringInvest;
	var int   isMultiEffect;
	var int   targetCollectAlgo;
	var int   targetCollectType;
	var int   targetCollectRange;
	var int   targetCollectAzi;
	var int   targetCollectElev;
};

class CFx_Base {
	var string visName_S;
	var string emTrjMode_S;
	var float  emFXLifeSpan;
};

class C_ParticleFXEmitKey {
	var string visName_S;
	var float  visSizeScale;
};

class C_INFO {
	var int    npc;
	var int    nr;
	var int    important;
	var func   condition;
	var func   information;
	var string description;
	var int    trade;
	var int    permanent;
};

class C_GILVALUES {
	var int fight_range_base[GIL_MAX];
	var int fight_range_fist[GIL_MAX];
	var int fight_range_1ha[GIL_MAX];
	var int fight_range_2ha[GIL_MAX];
};

class C_ParticleFX {
	var float  ppsValue;
	var string ppsScaleKeys_S;
	var int    ppsIsLooping;
	var int    ppsIsSmooth;
	var float  ppsFPS;
	var string ppsCreateEm_S;
	var float  ppsCreateEmDelay;
	var string shpType_S;
	var string shpFOR_S;
	var string shpOffsetVec_S;
	var string shpDistribType_S;
	var float  shpDistribWalkSpeed;
	var int    shpIsVolume;
	var string shpDim_S;
	var string shpMesh_S;
	var int    shpMeshRender_B;
	var string shpScaleKeys_S;
	var int    shpScaleIsLooping;
	var int    shpScaleIsSmooth;
	var float  shpScaleFPS;
	var string dirMode_S;
	var string dirFOR_S;
	var string dirModeTargetFOR_S;
	var string dirModeTargetPos_S;
	var float  dirAngleHead;
	var float  dirAngleHeadVar;
	var float  dirAngleElev;
	var float  dirAngleElevVar;
	var float  velAvg;
	var float  velVar;
	var float  lspPartAvg;
	var float  lspPartVar;
	var string flyGravity_S;
	var int    flyCollDet_B;
	var string visName_S;
	var string visOrientation_S;
	var int    visTexIsQuadPoly;
	var float  visTexAniFPS;
	var int    visTexAniIsLooping;
	var string visTexColorStart_S;
	var string visTexColorEnd_S;
	var string visSizeStart_S;
	var float  visSizeEndScale;
	var string visAlphaFunc_S;
	var float  visAlphaStart;
	var float  visAlphaEnd;
	var float  trlFadeSpeed;
	var string trlTexture_S;
	var float  trlWidth;
	var float  mrkFadeSpeed;
	var string mrkTexture_S;
	var float  mrkSize;
};

instance SYNTH_FIRE (C_ParticleFX) {
	ppsValue = 30.0;
	ppsIsLooping = 1;
	shpType_S = "CIRCLE";
	shpDim_S = "12";
	dirMode_S = "DIR";
	dirAngleElev = 90.0;
	dirAngleElevVar = 10.0;
	velAvg = 0.03;
	velVar = 0.01;
	lspPartAvg = 900.0;
	lspPartVar = 200.0;
	flyGravity_S = "0 0.00003 0";
	visName_S = "SYNTH_SPARK.TGA";
	visTexColorStart_S = "255 255 255";
	visTexColorEnd_S = "255 80 0";
	visSizeStart_S = "30 30";
	visSizeEndScale = 0.3;
	visAlphaFunc_S = "ADD";
	visAlphaStart = 255.0;
	visAlphaEnd = 0.0;
};

instance SYNTH_SMOKE (C_ParticleFX) {
	ppsValue = 6.0;
	ppsIsLooping = 1;
	shpType_S = "POINT";
	dirMode_S = "DIR";
	dirAngleElev = 90.0;
	velAvg = 0.02;
	lspPartAvg = 2500.0;
	lspPartVar = 500.0;
	visName_S = "SYNTH_SMOKE.TGA";
	visTexColorStart_S = "255 255 255";
	visTexColorEnd_S = "255 255 255";
	visSizeStart_S = "30 30";
	visSizeEndScale = 3.0;
	visAlphaFunc_S = "BLEND";
	visAlphaStart = 140.0;
	visAlphaEnd = 0.0;
};

instance self, other (C_NPC);
instance victim (C_NPC);
instance hero (C_NPC);
instance item (C_ITEM);

// ---------------------------------------------------------------- the engine
extern func void   Snd_Play(var string s0);
extern func void   Snd_Play3D(var C_NPC n, var string s1);
extern func void   Wld_InsertNpc(var int npcInstance, var string spawnPoint);
extern func void   Wld_InsertItem(var int itemInstance, var string spawnPoint);
extern func void   Wld_SetTime(var int hour, var int minute);
extern func int    Wld_IsTime(var int hour1, var int min1, var int hour2, var int min2);
extern func void   Mdl_SetVisual(var C_NPC n, var string s);
extern func void   Mdl_SetVisualBody(var C_NPC n, var string bodyMesh, var int bodyTex, var int skinColor,
                                     var string headMesh, var int headTex, var int teethTex, var int armorInst);
extern func void   Mdl_SetModelScale(var C_NPC n, var float x, var float y, var float z);
extern func void   Mdl_SetModelFatness(var C_NPC n, var float fatness);
extern func void   Mdl_ApplyOverlayMds(var C_NPC n, var string s);
extern func void   TA_Min(var C_NPC n, var int start_h, var int start_m, var int stop_h, var int stop_m,
                          var func state, var string waypoint);
extern func C_NPC  Hlp_GetNpc(var int npcInstance);
extern func int    Hlp_GetInstanceID(var C_NPC n);
extern func int    Hlp_IsValidNpc(var C_NPC n);
extern func int    Hlp_Random(var int bound);
extern func int    Hlp_StrCmp(var string a, var string b);
extern func string ConcatStrings(var string a, var string b);
extern func string IntToString(var int x);
extern func float  IntToFloat(var int x);
extern func int    FloatToInt(var float x);
extern func int    Npc_IsPlayer(var C_NPC n);
extern func void   Npc_SetTalentSkill(var C_NPC n, var int talent, var int value);
extern func void   Npc_SetToFistMode(var C_NPC n);
extern func int    Npc_KnowsInfo(var C_NPC n, var int info);
extern func int    Npc_GetDistToNpc(var C_NPC a, var C_NPC b);
extern func void   CreateInvItem(var C_NPC n, var int itemInstance);
extern func void   CreateInvItems(var C_NPC n, var int itemInstance, var int amount);
extern func void   EquipItem(var C_NPC n, var int itemInstance);
extern func void   AI_Output(var C_NPC a, var C_NPC b, var string text);
extern func void   AI_StopProcessInfos(var C_NPC n);
extern func void   PrintDebug(var string text);
extern func void   AI_GotoWP(var C_NPC n, var string wp);
extern func void   AI_GotoFP(var C_NPC n, var string fp);
extern func void   AI_GotoNextFP(var C_NPC n, var string fp);
extern func void   AI_AlignToWP(var C_NPC n);
extern func void   AI_AlignToFP(var C_NPC n);
extern func void   AI_PlayAni(var C_NPC n, var string ani);
extern func void   AI_PlayAniBS(var C_NPC n, var string ani, var int bodyState);
extern func void   AI_Wait(var C_NPC n, var float seconds);
extern func void   AI_StandUp(var C_NPC n);
extern func void   AI_SetWalkmode(var C_NPC n, var int mode);
extern func void   AI_StartState(var C_NPC n, var func state, var int doFinalize, var string wp);
extern func void   AI_ContinueRoutine(var C_NPC n);
extern func void   AI_TurnToNpc(var C_NPC n, var C_NPC to);
extern func int    Npc_GetDistToWP(var C_NPC n, var string wp);
extern func int    Npc_IsOnFP(var C_NPC n, var string fp);
extern func int    Wld_IsFPAvailable(var C_NPC n, var string fp);
extern func int    Wld_IsNextFPAvailable(var C_NPC n, var string fp);
extern func int    Npc_GetStateTime(var C_NPC n);
extern func void   Npc_SetStateTime(var C_NPC n, var int seconds);
extern func int    Npc_GetBodyState(var C_NPC n);
extern func void   Npc_SetPercTime(var C_NPC n, var float seconds);
extern func void   Npc_PercEnable(var C_NPC n, var int perception, var func reaction);
extern func void   Npc_PercDisable(var C_NPC n, var int perception);
extern func void   Perc_SetRange(var int perception, var int range);
extern func void   Npc_SendPassivePerc(var C_NPC n, var int perception, var C_NPC victim, var C_NPC other);
extern func int    Npc_CanSeeNpcFreeLOS(var C_NPC a, var C_NPC b);
extern func void   AI_LookAtNpc(var C_NPC n, var C_NPC other);
extern func void   AI_StopLookAt(var C_NPC n);
extern func void   AI_GotoNpc(var C_NPC n, var C_NPC other);
extern func void   AI_OutputSVM(var C_NPC n, var C_NPC target, var string svm);
extern func void   Wld_SetGuildAttitude(var int guild1, var int attitude, var int guild2);
extern func int    Wld_GetGuildAttitude(var int guild1, var int guild2);
extern func void   Wld_ExchangeGuildAttitudes(var string table);
extern func int    Npc_GetAttitude(var C_NPC a, var C_NPC b);
extern func void   Npc_SetTempAttitude(var C_NPC n, var int attitude);
extern func void   Npc_SetTarget(var C_NPC n, var C_NPC target);
extern func int    Npc_GetTarget(var C_NPC n);
extern func int    Npc_IsDead(var C_NPC n);
extern func int    Npc_HasItems(var C_NPC n, var int item);
extern func int    Npc_IsInState(var C_NPC n, var func state);
extern func string Npc_GetNearestWP(var C_NPC n);
extern func int    AI_UseMob(var C_NPC n, var string scheme, var int targetState);
extern func int    Wld_IsMobAvailable(var C_NPC n, var string scheme);
extern func int    Wld_GetMobState(var C_NPC n, var string scheme);
extern func string Npc_GetDetectedMob(var C_NPC n);
extern func void   AI_Attack(var C_NPC n);
extern func void   AI_ReadyMeleeWeapon(var C_NPC n);
extern func void   AI_RemoveWeapon(var C_NPC n);
extern func void   AI_EquipBestMeleeWeapon(var C_NPC n);
extern func int    Npc_GetNextTarget(var C_NPC n);
extern func int    Npc_IsInFightMode(var C_NPC n, var int mode);
extern func int    Npc_HasEquippedMeleeWeapon(var C_NPC n);
// Talking.
extern func void   AI_ProcessInfos(var C_NPC n);
extern func int    InfoManager_HasFinished();
extern func void   Info_AddChoice(var int info, var string text, var func fn);
extern func void   Info_ClearChoices(var int info);
extern func int    Npc_CheckInfo(var C_NPC n, var int important);
extern func void   Npc_ExchangeRoutine(var C_NPC n, var string routine);
extern func void   Npc_ClearAIQueue(var C_NPC n);
extern func void   Npc_SetKnowsPlayer(var C_NPC n, var C_NPC player);
extern func void   Npc_RemoveInvItem(var C_NPC n, var int item);
extern func void   PrintScreen(var string msg, var int x, var int y, var string font, var int seconds);

// ---------------------------------------------------------------- helpers
var int Kapitel;
var int Guards_Greeted;
var int Camp_Population;

func void B_Scale(var C_NPC slf) {
	// Stronger characters are broader, as in the original.
	if (slf.attribute[ATR_STRENGTH] < 30)       { Mdl_SetModelScale(slf, 0.9, 1, 1); }
	else if (slf.attribute[ATR_STRENGTH] < 70)  { Mdl_SetModelScale(slf, 1, 1, 1); }
	else                                        { Mdl_SetModelScale(slf, 1.1, 1, 1.1); };
};

func void B_SetAttributes(var C_NPC slf, var int strength, var int hitpoints) {
	slf.attribute[ATR_STRENGTH]      = strength;
	slf.attribute[ATR_DEXTERITY]     = strength / 2 + 5;
	slf.attribute[ATR_HITPOINTS_MAX] = hitpoints;
	slf.attribute[ATR_HITPOINTS]     = hitpoints;
	slf.level = (strength + hitpoints / 10) / 8;
};

// The states of the daily routine, in the manner of the original's: each
// is a function that begins it, a "_Loop" called while it lasts and an
// "_End". They tell the engine what the character is to do.
func void ZS_Guard() {
	Npc_SetPercTime(self, 1);
	Npc_PercEnable(self, PERC_ASSESSDAMAGE, B_ReactToDamage);
	Npc_PercEnable(self, PERC_ASSESSPLAYER, B_AssessSC);
	Npc_PercEnable(self, PERC_ASSESSWARN, B_AssessWarn);
	Npc_PercEnable(self, PERC_ASSESSTALK, B_AssessTalk);
	AI_StandUp(self);
	AI_SetWalkmode(self, NPC_WALK);
	AI_GotoWP(self, self.wp);
	AI_AlignToWP(self);
	AI_PlayAni(self, "T_STAND_2_LGUARD");
};
func int ZS_Guard_Loop() {
	// The player has gone: the guard looks ahead again.
	if (self.aivar[AIV_SEENPLAYER] && (Npc_GetDistToNpc(self, hero) > 1200)) {
		self.aivar[AIV_SEENPLAYER] = FALSE;
		AI_StopLookAt(self);
	};
	if (Hlp_Random(100) < 10) { AI_PlayAni(self, "T_LGUARD_SCRATCH"); };
	AI_Wait(self, 1);
	return LOOP_CONTINUE;
};
func void ZS_Guard_End() {
	AI_PlayAni(self, "T_LGUARD_2_STAND");
};
// Perceptions, in the manner of the original's B_AssessSC: the guard who sees
// the player come near looks at him, asks what he wants, and tells the
// guards about of him.
func void B_AssessSC() {
	if (B_CheckForImportantInfo(self, other)) { return; };
	if (Npc_CanSeeNpcFreeLOS(self, other) && (Npc_GetDistToNpc(self, other) < 800)) {
		if (!self.aivar[AIV_SEENPLAYER]) {
			self.aivar[AIV_SEENPLAYER] = TRUE;
			AI_LookAtNpc(self, other);
			AI_OutputSVM(self, other, "$WHATDOYOUWANT");
			Npc_SendPassivePerc(self, PERC_ASSESSWARN, other, self);
		};
	};
};
func void B_AssessWarn() {
	AI_LookAtNpc(self, other);
};
func void InitPerceptions() {
	Perc_SetRange(PERC_ASSESSWARN, 3000);
};
// Talking, in the manner of the original's: the player's A makes the
// character hear PERC_ASSESSTALK (B_AssessTalk), which begins the state of
// talking; a character with an important info to tell begins it by itself.
func int B_CheckForImportantInfo(var C_NPC slf, var C_NPC oth) {
	if (oth.aivar[AIV_INVINCIBLE] == FALSE) {
		if (Npc_CheckInfo(slf, 1)) {
			hero.aivar[AIV_IMPORTANT] = TRUE;
			Npc_ClearAIQueue(slf);
			AI_StartState(slf, ZS_Talk, 0, "");
			return TRUE;
		};
	};
	return FALSE;
};
func void B_AssessTalk() {
	Npc_SetPercTime(self, 1);
	self.aivar[AIV_INVINCIBLE] = FALSE;
	hero.aivar[AIV_INVINCIBLE] = FALSE;
	hero.aivar[AIV_IMPORTANT] = FALSE;
	Npc_ClearAIQueue(self);
	AI_StartState(self, ZS_Talk, 1, "");
};
var int talks_begun;
func void ZS_Talk() {
	// Somebody is talking to the player already.
	if (other.aivar[AIV_INVINCIBLE] == TRUE) {
		AI_ContinueRoutine(self);
		return;
	};
	talks_begun += 1;
	self.aivar[AIV_INVINCIBLE] = TRUE;
	other.aivar[AIV_INVINCIBLE] = TRUE;
	AI_StandUp(self);
	AI_TurnToNpc(self, hero);
	if (!hero.aivar[AIV_IMPORTANT]) { AI_OutputSVM(self, hero, "$GREETINGS"); };
	Npc_SetKnowsPlayer(self, hero);
	AI_ProcessInfos(self);
};
func int ZS_Talk_Loop() {
	if (InfoManager_HasFinished()) {
		self.aivar[AIV_INVINCIBLE] = FALSE;
		other.aivar[AIV_INVINCIBLE] = FALSE;
		return LOOP_END;
	};
	return LOOP_CONTINUE;
};
func void ZS_Talk_End() {};
func void B_GiveXP(var int add_xp) {
	hero.exp = hero.exp + add_xp;
	PrintScreen(ConcatStrings("Experience gained: ", IntToString(add_xp)), -1, 55, "FONT_OLD_10_WHITE.TGA", 3);
	if (hero.exp >= hero.exp_next) {
		hero.level = hero.level + 1;
		hero.exp_next = hero.exp_next + 500 * (hero.level + 1);
		hero.lp = hero.lp + 10;
		PrintScreen("Level up!", -1, 50, "FONT_OLD_10_WHITE.TGA", 3);
	};
};
// Fighting, in the manner of the original's ZS_MM_Attack and ZS_Attack: the
// target is set, and the loop has the engine choose a move (AI_Attack) until
// the enemy is down or gone.
func void ZS_MM_Attack() {
	Npc_PercEnable(self, PERC_ASSESSDAMAGE, B_MM_ReactToDamage);
	AI_SetWalkmode(self, NPC_RUN);
};
func int ZS_MM_Attack_Loop() {
	Npc_GetTarget(self);
	if (Npc_IsDead(other) || Npc_IsInState(other, ZS_Unconscious)) { return LOOP_END; };
	if (Npc_GetDistToNpc(self, other) > 2500) { return LOOP_END; };
	AI_Attack(self);
	return LOOP_CONTINUE;
};
func void ZS_MM_Attack_End() {};
var int mm_enemy_seen;
func void B_MM_AssessEnemy() {
	mm_enemy_seen += 1;
	Npc_SetTarget(self, other);
	AI_StartState(self, ZS_MM_Attack, 0, "");
};
var int mm_hurt;
func void B_MM_ReactToDamage() {
	mm_hurt += 1;
	Npc_SetTarget(self, other);
	if (!Npc_IsInState(self, ZS_MM_Attack)) { AI_StartState(self, ZS_MM_Attack, 0, ""); };
};
var int attacks_begun;
func void ZS_Attack() {
	attacks_begun += 1;
	Npc_PercEnable(self, PERC_ASSESSDAMAGE, B_CombatReactToDamage);
	Npc_GetTarget(self);
	AI_StandUp(self);
	AI_EquipBestMeleeWeapon(self);
	AI_ReadyMeleeWeapon(self);
	AI_SetWalkmode(self, NPC_RUN);
};
func int ZS_Attack_Loop() {
	Npc_GetTarget(self);
	if (Npc_IsDead(other) || Npc_IsInState(other, ZS_Unconscious)) {
		if (!Npc_GetNextTarget(self)) { return LOOP_END; };
	};
	if (Npc_GetDistToNpc(self, other) > 1500) { return LOOP_END; };
	AI_Attack(self);
	return LOOP_CONTINUE;
};
func void ZS_Attack_End() {
	AI_RemoveWeapon(self);
};
func void B_CombatReactToDamage() {
	Npc_SetTarget(self, other);
};
var int damage_noticed;
func void B_ReactToDamage() {
	damage_noticed += 1;
	Npc_SetTarget(self, other);
	AI_StartState(self, ZS_Attack, 0, "");
};
// Down: knocked out for a while, or dead for good.
var int knocked_out;
func void ZS_Unconscious() {
	knocked_out += 1;
};
func int ZS_Unconscious_Loop() {
	if (Npc_GetStateTime(self) > HAI_TIME_UNCONSCIOUS) { return LOOP_END; };
	AI_Wait(self, 1);
	return LOOP_CONTINUE;
};
func void ZS_Unconscious_End() {
	AI_StandUp(self);
};
var int deaths;
func void ZS_Dead() {
	deaths += 1;
};

func void ZS_Stand() {
	Npc_PercEnable(self, PERC_ASSESSDAMAGE, B_ReactToDamage);
	Npc_PercEnable(self, PERC_ASSESSTALK, B_AssessTalk);
	AI_StandUp(self);
	AI_SetWalkmode(self, NPC_WALK);
	AI_GotoWP(self, self.wp);
	AI_AlignToWP(self);
};
func int ZS_Stand_Loop() {
	if (Npc_GetStateTime(self) > 15) {
		Npc_SetStateTime(self, 0);
		AI_PlayAni(self, "T_SEARCH");
	};
	AI_Wait(self, 1);
	return LOOP_CONTINUE;
};
func void ZS_Stand_End() {};

func void ZS_SitAround() {
	AI_StandUp(self);
	AI_SetWalkmode(self, NPC_WALK);
	if (Npc_GetDistToWP(self, self.wp) > 300) { AI_GotoWP(self, self.wp); };
};
func int ZS_SitAround_Loop() {
	// On a bench if there is a free one about, else on the ground by the fire.
	if (Npc_GetBodyState(self) != BS_SIT) {
		if (Wld_IsMobAvailable(self, "BENCH")) {
			AI_UseMob(self, "BENCH", 1);
		} else if (Wld_IsFPAvailable(self, "SIT")) {
			AI_GotoFP(self, "SIT");
			AI_AlignToFP(self);
			AI_PlayAniBS(self, "T_STAND_2_SIT", BS_SIT);
		};
	};
	AI_Wait(self, 1);
	return LOOP_CONTINUE;
};
func void ZS_SitAround_End() {
	AI_UseMob(self, "BENCH", -1);
	AI_StandUp(self);
};

func void ZS_Smith() {
	AI_StandUp(self);
	AI_SetWalkmode(self, NPC_RUN);
	AI_GotoWP(self, self.wp);
	AI_AlignToWP(self);
	AI_UseMob(self, "BSANVIL", 1);
};
func int ZS_Smith_Loop() {
	if (Hlp_Random(100) < 20) { AI_PlayAni(self, "T_SEARCH"); };
	AI_Wait(self, 2);
	return LOOP_CONTINUE;
};
func void ZS_Smith_End() {
	AI_UseMob(self, "BSANVIL", -1);
};

// The original's body-state check, formula and all.
func int C_BodyStateContains(var C_NPC slf, var int bodystate) {
	if ((Npc_GetBodyState(slf) & (BS_MAX | BS_FLAG_INTERRUPTABLE | BS_FLAG_FREEHANDS)) == (bodystate & (BS_MAX | BS_FLAG_INTERRUPTABLE | BS_FLAG_FREEHANDS))) {
		return TRUE;
	};
	return FALSE;
};

// To bed - written as the original's ZS_Sleep.d is, tricks and all: two
// AI_UseMob in a row for the two kinds of bed (the second is to be passed
// over once the first has begun), a loop that gets up and goes to sit
// somewhere if the body is not "busy with a thing" three seconds in, and
// three ways out of bed at the end.
func void ZS_Sleep() {
	AI_SetWalkmode(self, NPC_WALK);
	if (!C_BodyStateContains(self, BS_MOBINTERACT)) {
		if (Hlp_StrCmp(Npc_GetNearestWP(self), self.wp) == 0) {
			AI_GotoWP(self, self.wp);
		};
	};
	AI_StartState(self, ZS_SleepBed, 1, "");
};
func void ZS_SleepBed() {
	if (Wld_IsMobAvailable(self, "BED")) {
		AI_UseMob(self, "BED", 1);
	} else if (Wld_IsMobAvailable(self, "BEDHIGH") || Wld_IsMobAvailable(self, "BEDLOW")) {
		AI_UseMob(self, "BEDHIGH", 1); // (the original: further UseMobs are ignored while one is going on)
		AI_UseMob(self, "BEDLOW", 1);
	} else {
		AI_StartState(self, ZS_Stand, 1, "");
	};
};
func int ZS_SleepBed_Loop() {
	// (The original's workaround for an AI_UseMob that failed after all.)
	if (!C_BodyStateContains(self, BS_MOBINTERACT) && (Npc_GetStateTime(self) > 3)) {
		AI_StartState(self, ZS_SitAround, 1, "");
	};
	AI_Wait(self, 1);
	return LOOP_CONTINUE;
};
func void ZS_SleepBed_End() {
	AI_UseMob(self, "BEDHIGH", -1);
	AI_UseMob(self, "BEDLOW", -1);
	AI_UseMob(self, "BED", -1);
};

// Creatures have no routine: they start in a state that picks, by the hour,
// what they do for a while.
func void ZS_MM_AllScheduler() {
	Npc_SetPercTime(self, 1);
	if (Wld_IsTime(22, 0, 6, 0)) { AI_StartState(self, ZS_MM_Rtn_Sleep, 1, ""); }
	else                         { AI_StartState(self, ZS_MM_Rtn_Roam, 1, ""); };
};
func void ZS_MM_Rtn_Sleep() {
	AI_SetWalkmode(self, NPC_WALK);
	AI_PlayAni(self, "T_STAND_2_SLEEP");
};
func int ZS_MM_Rtn_Sleep_Loop() {
	if (!Wld_IsTime(22, 0, 6, 0)) { return LOOP_END; };
	AI_Wait(self, 1);
	return LOOP_CONTINUE;
};
func void ZS_MM_Rtn_Sleep_End() {
	AI_StandUp(self);
};
func void ZS_MM_Rtn_Roam() {
	Npc_PercEnable(self, PERC_ASSESSENEMY, B_MM_AssessEnemy);
	Npc_PercEnable(self, PERC_ASSESSDAMAGE, B_MM_ReactToDamage);
	AI_SetWalkmode(self, NPC_WALK);
};
func int ZS_MM_Rtn_Roam_Loop() {
	if (Wld_IsTime(22, 0, 6, 0)) { return LOOP_END; };
	if (Npc_GetStateTime(self) > 4) {
		Npc_SetStateTime(self, 0);
		if (Wld_IsNextFPAvailable(self, "FP_ROAM")) {
			AI_GotoNextFP(self, "FP_ROAM");
			AI_PlayAni(self, "T_WARN");
		};
	};
	AI_Wait(self, 0.5);
	return LOOP_CONTINUE;
};
func void ZS_MM_Rtn_Roam_End() {};

func void TA_Guard(var int start_h, var int start_m, var int stop_h, var int stop_m, var string waypoint) {
	TA_Min(self, start_h, start_m, stop_h, stop_m, ZS_Guard, waypoint);
};
func void TA_Stand(var int start_h, var int start_m, var int stop_h, var int stop_m, var string waypoint) {
	TA_Min(self, start_h, start_m, stop_h, stop_m, ZS_Stand, waypoint);
};
func void TA_Sleep(var int start_h, var int start_m, var int stop_h, var int stop_m, var string waypoint) {
	TA_Min(self, start_h, start_m, stop_h, stop_m, ZS_Sleep, waypoint);
};
func void TA_SitAround(var int start_h, var int start_m, var int stop_h, var int stop_m, var string waypoint) {
	TA_Min(self, start_h, start_m, stop_h, stop_m, ZS_SitAround, waypoint);
};
func void TA_Smith(var int start_h, var int start_m, var int stop_h, var int stop_m, var string waypoint) {
	TA_Min(self, start_h, start_m, stop_h, stop_m, ZS_Smith, waypoint);
};

// ---------------------------------------------------------------- items
prototype Armor_Default (C_ITEM) {
	mainflag = ITEM_KAT_ARMOR;
	material = 3;
	visual_skin = 0;
};

instance GRD_ARMOR_M (Armor_Default) {
	name = "Guard's armour";
	protection[1] = 55;
	protection[2] = 10;
	value = 1650;
	wear = 1;
	visual = "grdm.3ds";
	visual_change = "Hum_GRDM_ARMOR.asc";
	description = name;
	text[1] = "Protection against weapons:";
	count[1] = protection[1];
};

instance VLK_ARMOR_L (Armor_Default) {
	name = "Digger's trousers";
	protection[1] = 10;
	value = 250;
	wear = 1;
	visual = "vlkl.3ds";
	visual_change = "Hum_VLKL_ARMOR.asc";
	visual_skin = 1;
	description = name;
};

instance ItMw_Sword (C_ITEM) {
	name = "Sword";
	mainflag = ITEM_KAT_NF;
	flags = ITEM_SWD;
	damagetype = DAM_EDGE;
	damagetotal = 30;
	range = 100;
	value = 100 + damagetotal * 3;
	visual = "ItMw_1H_Sword_01.3DS";
	description = name;
};

instance ItMw_Club (C_ITEM) {
	name = "Club";
	mainflag = ITEM_KAT_NF;
	flags = ITEM_SWD;
	damagetype = DAM_BLUNT;
	damagetotal = 12;
	range = 80;
	value = 20;
	visual = "ItMw_1H_Club_01.3DS";
	description = name;
};

// The inventory's things: of every kind, with what using and wearing them
// does, and the info box's lines the way the original writes them.
extern func void   Npc_ChangeAttribute(var C_NPC n, var int atr, var int value);
// The diary.
extern func void   Log_CreateTopic(var string name, var int section);
extern func void   Log_SetTopicStatus(var string name, var int status);
extern func void   Log_AddEntry(var string topic, var string entry);
extern func void   IntroduceChapter(var string title, var string subtitle, var string texture, var string sound, var int waittime);
const int LOG_MISSION = 0;
const int LOG_NOTE = 1;
const int LOG_RUNNING = 1;
const int LOG_SUCCESS = 2;
const string TOPIC_Camp = "The camp";

const string NAME_Value = "Value";
const string NAME_Bonus_HP = "Heals";
const string NAME_Bonus_Mana = "Mana";

instance ItFo_Potion_Health_01 (C_ITEM) {
	name = "Potion";
	mainflag = ITEM_KAT_POTIONS;
	flags = ITEM_MULTI;
	value = 25;
	visual = "ItFo_Potion_Health_01.3ds";
	scemename = "POTIONFAST";
	on_state[0] = UseHealthPotion;
	description = "Essence of Healing";
	text[1] = NAME_Bonus_HP;    count[1] = 50;
	text[5] = NAME_Value;       count[5] = value;
};
func void UseHealthPotion() { Npc_ChangeAttribute(self, ATR_HITPOINTS, 50); };

instance ItFo_Potion_Mana_01 (C_ITEM) {
	name = "Potion";
	mainflag = ITEM_KAT_POTIONS;
	flags = ITEM_MULTI;
	value = 25;
	visual = "ItFo_Potion_Mana_01.3ds";
	scemename = "POTIONFAST";
	on_state[0] = UseManaPotion;
	description = "Essence of Magic Energy";
	text[1] = NAME_Bonus_Mana;  count[1] = 30;
	text[5] = NAME_Value;       count[5] = value;
};
func void UseManaPotion() { Npc_ChangeAttribute(self, ATR_MANA, 30); };

instance ItFoApple (C_ITEM) {
	name = "Apple";
	mainflag = ITEM_KAT_FOOD;
	flags = ITEM_MULTI;
	value = 4;
	visual = "ItFo_Apple_01.3ds";
	scemename = "FOOD";
	on_state[0] = UseApple;
	description = name;
	text[1] = NAME_Bonus_HP;    count[1] = 8;
	text[5] = NAME_Value;       count[5] = value;
};
func void UseApple() { Npc_ChangeAttribute(self, ATR_HITPOINTS, 8); };

instance ItFoMutton (C_ITEM) {
	name = "Fried meat";
	mainflag = ITEM_KAT_FOOD;
	flags = ITEM_MULTI;
	value = 8;
	visual = "ItFo_Mutton_01.3ds";
	scemename = "MEAT";
	on_state[0] = UseMutton;
	description = name;
};
func void UseMutton() { Npc_ChangeAttribute(self, ATR_HITPOINTS, 15); };

instance ItFoBeer (C_ITEM) {
	name = "Beer";
	mainflag = ITEM_KAT_FOOD;
	flags = ITEM_MULTI;
	value = 10;
	visual = "ItFo_Beer_01.3ds";
	scemename = "POTION";
	on_state[0] = UseBeer;
	description = name;
};
func void UseBeer() { Npc_ChangeAttribute(self, ATR_HITPOINTS, 3); };

instance ItMw_2H_Sword_Heavy (C_ITEM) {
	name = "Heavy two-hander";
	mainflag = ITEM_KAT_NF;
	flags = ITEM_2HD_SWD;
	damagetype = DAM_EDGE;
	damagetotal = 28; // (below the sword: the faults menu's "best weapon" stays the sword)
	range = 140;
	cond_atr[2] = ATR_STRENGTH;
	cond_value[2] = 60;
	value = 900;
	visual = "ItMw_2H_Sword_Heavy_01.3DS";
	description = name;
};

instance ItRw_Bow_Small (C_ITEM) {
	name = "Short bow";
	mainflag = ITEM_KAT_FF;
	flags = ITEM_BOW;
	damagetype = DAM_EDGE;
	damagetotal = 20;
	value = 80;
	visual = "ItRw_Bow_Small_01.3DS";
	description = name;
};

instance ItAmArrow (C_ITEM) {
	name = "Arrow";
	mainflag = ITEM_KAT_MUN;
	flags = ITEM_MULTI;
	value = 1;
	visual = "ItAm_Arrow_01.3ds";
	description = name;
};

instance ItMi_Ring_Fire (C_ITEM) {
	name = "Ring";
	mainflag = ITEM_KAT_MAGIC;
	flags = ITEM_RING;
	value = 200;
	visual = "ItMi_Ring_01.3ds";
	on_equip = Equip_Ring_Fire;
	on_unequip = UnEquip_Ring_Fire;
	description = "Ring of Flame Protection";
	text[2] = "Protection against fire";  count[2] = 3;
	text[5] = NAME_Value;                 count[5] = value;
};
func void Equip_Ring_Fire() { self.protection[PROT_FIRE] += 3; };
func void UnEquip_Ring_Fire() { self.protection[PROT_FIRE] -= 3; };

instance ItMi_Amulet_Edge (C_ITEM) {
	name = "Amulet";
	mainflag = ITEM_KAT_MAGIC;
	flags = ITEM_AMULET;
	value = 400;
	visual = "ItMi_Amulet_01.3ds";
	on_equip = Equip_Amulet_Edge;
	on_unequip = UnEquip_Amulet_Edge;
	description = "Amulet of Stone Skin";
};
func void Equip_Amulet_Edge() { self.protection[PROT_EDGE] += 5; };
func void UnEquip_Amulet_Edge() { self.protection[PROT_EDGE] -= 5; };

instance ItArScrollFirebolt (C_ITEM) {
	name = "Scroll";
	mainflag = ITEM_KAT_RUNE;
	flags = ITEM_MULTI;
	value = 50;
	visual = "ItAr_Scroll_01.3ds";
	spell = SPL_FIREBOLT;
	description = "Fire Bolt";
};

instance ItArRuneFirebolt (C_ITEM) {
	name = "Rune";
	mainflag = ITEM_KAT_RUNE;
	value = 300;
	visual = "ItAr_Rune_23.3ds";
	spell = SPL_FIREBOLT;
	mag_circle = 1;
	description = "Fire Bolt";
};

instance ItArRuneFireball (C_ITEM) {
	name = "Rune";
	mainflag = ITEM_KAT_RUNE;
	value = 600;
	visual = "ItAr_Rune_24.3ds";
	spell = SPL_FIREBALL;
	mag_circle = 3;
	description = "Fireball";
};

instance ItArRuneHeal (C_ITEM) {
	name = "Rune";
	mainflag = ITEM_KAT_RUNE;
	value = 300;
	visual = "ItAr_Rune_25.3ds";
	spell = SPL_HEAL;
	mag_circle = 1;
	description = "Heal";
};

// Magic (Magic/*.d of the original): the arrays and instances of the spells, the functions the engine
// asks how a spell goes (a point of mana at a time), the visual effects.
extern func int Npc_GetActiveSpell(var C_NPC n);
extern func int Npc_GetActiveSpellLevel(var C_NPC n);

const string spellFXInstanceNames[MAX_SPELL] = {
	"Light", "Fireball", "Transform", "Fear", "Heal", "Lightning", "Demon", "Skeleton", "Forget", "WindFist",
	"Telekinesis", "Charm", "Sleep", "Pyrokinesis", "MassDeath", "Control", "DestroyUndead", "Firebolt"
};
const string spellFXAniLetters[MAX_SPELL] = {
	"SLE", "FIB", "TRF", "FEA", "HEA", "XXX", "SUM", "SUM", "XXX", "WND",
	"TEL", "SLE", "SLE", "PYR", "FEA", "CON", "FIB", "FBT"
};

instance Spell_Firebolt (C_Spell) {
	time_per_mana = 500;
	damage_per_level = SPL_DAMAGE_FIREBOLT;
	spellType = SPELL_BAD;
	damageType = DAM_FIRE;
	targetCollectAlgo = TARGET_COLLECT_FOCUS_FALLBACK_NONE;
	targetCollectType = 1;
};
instance Spell_Fireball (C_Spell) {
	time_per_mana = 750;
	damage_per_level = SPL_DAMAGE_FIREBALL;
	spellType = SPELL_BAD;
	damageType = DAM_FIRE;
	targetCollectAlgo = TARGET_COLLECT_FOCUS_FALLBACK_NONE;
	targetCollectType = 1;
};
instance Spell_Heal (C_Spell) {
	time_per_mana = 500;
	spellType = SPELL_GOOD;
	targetCollectAlgo = TARGET_COLLECT_CASTER;
};

func int Spell_ProcessMana(var int manaInvested) {
	if (Npc_GetActiveSpell(self) == SPL_FIREBOLT) {
		if (manaInvested >= SPL_SENDCAST_FIREBOLT) { return SPL_SENDCAST; };
		return SPL_RECEIVEINVEST;
	};
	if (Npc_GetActiveSpell(self) == SPL_FIREBALL) {
		if ((manaInvested == (SPL_SENDCAST_FIREBALL * 3) / 10) || (manaInvested == (SPL_SENDCAST_FIREBALL * 5) / 10)) { return SPL_NEXTLEVEL; };
		if (manaInvested >= SPL_SENDCAST_FIREBALL) { return SPL_SENDCAST; };
		return SPL_RECEIVEINVEST;
	};
	if (Npc_GetActiveSpell(self) == SPL_HEAL) {
		if (self.attribute[ATR_HITPOINTS] < self.attribute[ATR_HITPOINTS_MAX]) {
			Npc_ChangeAttribute(self, ATR_HITPOINTS, SPL_HEALING_HP_PER_MP);
			return SPL_NEXTLEVEL;
		};
		return SPL_SENDSTOP;
	};
	return SPL_SENDSTOP;
};

func int Spell_ProcessMana_Release(var int manaInvested) {
	if (Npc_GetActiveSpell(self) == SPL_FIREBALL) { return SPL_SENDCAST; };
	return SPL_SENDSTOP;
};

instance spellFX_Firebolt (CFx_Base) {
	visName_S = "SYNTH_FIRE";
	emTrjMode_S = "TARGET";
};
instance spellFX_Firebolt_KEY_CAST (C_ParticleFXEmitKey) { visName_S = "SYNTH_SMOKE"; };
instance spellFX_Firebolt_KEY_COLLIDE (C_ParticleFXEmitKey) { visName_S = "SYNTH_SMOKE"; };
instance spellFX_Fireball (CFx_Base) {
	visName_S = "SYNTH_FIRE";
	emTrjMode_S = "TARGET";
};
instance spellFX_Fireball_KEY_COLLIDE (C_ParticleFXEmitKey) { visName_S = "SYNTH_SMOKE"; };

instance ItWr_Letter (C_ITEM) {
	name = "Letter";
	mainflag = ITEM_KAT_DOCS;
	value = 0;
	visual = "ItWr_Letter_01.3ds";
	description = "A letter to the mages";
};

instance ItMiNugget (C_ITEM) {
	name = "Ore";
	mainflag = ITEM_KAT_NONE;
	flags = ITEM_MULTI;
	value = 1;
	visual = "ItMi_Nugget_01.3ds";
	description = name;
};

// The chests' keys and lock picks (the scene's chests name them).
instance ItKeLockpick (C_ITEM) {
	name = "Lockpick";
	mainflag = ITEM_KAT_NONE;
	flags = ITEM_MULTI;
	value = 10;
	visual = "ItKe_Lockpick_01.3ds";
	description = name;
};

instance ItKe_CampChest (C_ITEM) {
	name = "Key";
	mainflag = ITEM_KAT_NONE;
	value = 5;
	visual = "ItKe_Key_01.3ds";
	description = "Key to the small chest";
};

// ---------------------------------------------------------------- fighting
func void SndTest() {
	Snd_Play("SFX_TEST_STEP");
};

func void SndTest3D() {
	Snd_Play3D(hero, "SFX_TEST_STEP");
};

instance Gil_Values (C_GILVALUES) {
	fight_range_base[GIL_HUMAN] = 60;
	fight_range_fist[GIL_HUMAN] = 50;
	fight_range_base[GIL_SCAVENGER] = 75;
	fight_range_fist[GIL_SCAVENGER] = 80;
	fight_range_base[GIL_MOLERAT] = 70;
	fight_range_fist[GIL_MOLERAT] = 70;
};
// (The fight AI tables are a script of their own, as in the game: syntheticFightScriptSource.)

// ---------------------------------------------------------------- characters
prototype Npc_Default (C_NPC) {
	attribute[ATR_STRENGTH]      = 10;
	attribute[ATR_DEXTERITY]     = 10;
	attribute[ATR_HITPOINTS_MAX] = 40;
	attribute[ATR_HITPOINTS]     = 40;
	senses_range = 2000;
	// (No damage type, as in the game: a person hits blunt, the engine says.)
};

instance PC_Hero (Npc_Default) {
	name = "I";
	npctype = NPCTYPE_MAIN;
	guild = GIL_NONE;
	level = 0;
	voice = 15;
	id = 0;
	exp_next = 500;
	B_SetAttributes(self, 10, 40);
	attribute[ATR_MANA_MAX] = 20;
	attribute[ATR_MANA]     = 10;
	Mdl_SetVisual(self, "HUMANS.MDS");
	//            body mesh          body tex  skin  head mesh        head tex  teeth  armour
	Mdl_SetVisualBody(self, "hum_body_Naked0", 4, 1, "Hum_Head_Pony", 9, 0, -1);
	// (The test scene's hero brings a few things along to try the inventory with.)
	CreateInvItems(self, ItFo_Potion_Health_01, 3);
	CreateInvItems(self, ItFoApple, 2);
};

instance GRD_200_Thorus (Npc_Default) {
	name = "Thorus";
	npctype = NPCTYPE_MAIN;
	guild = GIL_GRD;
	voice = 9;
	id = 200;
	flags = NPC_FLAG_IMMORTAL;
	B_SetAttributes(self, 100, 400);
	fight_tactic = FAI_HUMAN_STRONG;
	Mdl_SetVisual(self, "HUMANS.MDS");
	Mdl_ApplyOverlayMds(self, "Humans_Militia.mds");
	Mdl_SetVisualBody(self, "hum_body_Naked0", 0, 1, "Hum_Head_Fighter", 12, 0, GRD_ARMOR_M);
	B_Scale(self);
	Mdl_SetModelFatness(self, 0);
	Npc_SetTalentSkill(self, 1, 2);
	EquipItem(self, ItMw_Sword);
	daily_routine = Rtn_start_200;
};
func void Rtn_start_200() {
	TA_Guard(06, 00, 23, 00, "CAMP_GATE_GUARD_L");
	TA_Guard(23, 00, 06, 00, "CAMP_GATE_GUARD_L");
};

instance GRD_201_Jackal (Npc_Default) {
	name = NAME_Guard;
	npctype = NPCTYPE_GUARD;
	guild = GIL_GRD;
	voice = 7;
	id = 201;
	B_SetAttributes(self, 60, 220);
	fight_tactic = FAI_HUMAN_STRONG;
	Mdl_SetVisual(self, "HUMANS.MDS");
	Mdl_SetVisualBody(self, "hum_body_Naked0", 0, 2, "Hum_Head_Bald", 4, 1, GRD_ARMOR_M);
	B_Scale(self);
	Mdl_SetModelFatness(self, 1);
	EquipItem(self, ItMw_Sword);
	daily_routine = Rtn_start_201;
};
func void Rtn_start_201() {
	TA_Guard(07, 30, 20, 00, "CAMP_GATE_GUARD_R");
	TA_SitAround(20, 00, 23, 30, "CAMP_FIRE");
	TA_Sleep(23, 30, 07, 30, "CAMP_HUT_1");
};

instance VLK_500_Digger (Npc_Default) {
	name = NAME_Digger;
	guild = GIL_VLK;
	voice = 2;
	id = 500;
	B_SetAttributes(self, 15, 70);
	fight_tactic = FAI_HUMAN_STRONG;
	Mdl_SetVisual(self, "HUMANS.MDS");
	Mdl_SetVisualBody(self, "hum_body_Naked0", 3, 0, "Hum_Head_Thief", 70, 2, VLK_ARMOR_L);
	B_Scale(self);
	Mdl_SetModelFatness(self, -1);
	CreateInvItems(self, ItMw_Sword, 1);
	daily_routine = Rtn_start_500;
};
func void Rtn_start_500() {
	TA_Smith(08, 00, 18, 00, "CAMP_SMITH");
	TA_SitAround(18, 00, 22, 00, "CAMP_FIRE");
	TA_Sleep(22, 00, 08, 00, "CAMP_HUT_2");
};

instance VLK_501_Digger (Npc_Default) {
	name = NAME_Digger;
	guild = GIL_VLK;
	voice = 3;
	id = 501;
	B_SetAttributes(self, 20, 90);
	Mdl_SetVisual(self, "HUMANS.MDS");
	Mdl_SetVisualBody(self, "hum_body_Naked0", 3, 1, "Hum_Head_Pony", 9, 2, -1);
	B_Scale(self);
	daily_routine = Rtn_start_501;
};
func void Rtn_start_501() {
	TA_Stand(00, 00, 12, 00, "ROAD_1");
	TA_Stand(12, 00, 24, 00, "CAMP_FIRE");
};

instance GRD_210_CaveGuard (Npc_Default) {
	name = NAME_Guard;
	npctype = NPCTYPE_GUARD;
	guild = GIL_GRD;
	id = 210;
	B_SetAttributes(self, 50, 180);
	Mdl_SetVisual(self, "HUMANS.MDS");
	Mdl_SetVisualBody(self, "hum_body_Naked0", 0, 1, "Hum_Head_Fighter", 12, 0, GRD_ARMOR_M);
	daily_routine = Rtn_start_210;
};
func void Rtn_start_210() {
	TA_Guard(00, 00, 24, 00, "CAVE_GUARD");
};

// ---------------------------------------------------------------- creatures
prototype Mst_Default_Scavenger (C_NPC) {
	name = "Scavenger";
	guild = GIL_SCAVENGER;
	aivar[AIV_WASDEFEATED] = FALSE;
	level = 3;
	attribute[ATR_STRENGTH]      = 10;
	attribute[ATR_HITPOINTS_MAX] = 46;
	attribute[ATR_HITPOINTS]     = 46;
	protection[DAM_INDEX_BLUNT]  = 5;
	protection[DAM_INDEX_EDGE]   = 5;
	damagetype = DAM_EDGE;
	fight_tactic = FAI_SCAVENGER;
	senses_range = 2500;
	start_aistate = ZS_MM_AllScheduler;
};
func void Set_Scavenger_Visuals() {
	Mdl_SetVisual(self, "Scavenger.mds");
	Mdl_SetVisualBody(self, "Sca_Body", DEFAULT, DEFAULT, "", DEFAULT, DEFAULT, -1);
};
instance Scavenger (Mst_Default_Scavenger) {
	Set_Scavenger_Visuals();
	Npc_SetToFistMode(self);
};
instance YScavenger (Mst_Default_Scavenger) {
	name = "Young scavenger";
	level = 1;
	Set_Scavenger_Visuals();
	Mdl_SetModelScale(self, 0.8, 0.8, 0.8);
	Npc_SetToFistMode(self);
};
instance Molerat (C_NPC) {
	name = "Molerat";
	guild = GIL_MOLERAT;
	level = 3;
	attribute[ATR_HITPOINTS_MAX] = 30;
	attribute[ATR_HITPOINTS]     = 30;
	start_aistate = ZS_MM_AllScheduler;
	Mdl_SetVisual(self, "Molerat.mds");
	Mdl_SetVisualBody(self, "Mol_Body", DEFAULT, DEFAULT, "", DEFAULT, DEFAULT, -1);
};

// ---------------------------------------------------------------- dialogues
const string DIALOG_ENDE = "End";
const string DIALOG_BACK = "Back";
const string TOPIC_Delivery = "A favour for Thorus";
const int LOG_FAILED = 3;
var int MIS_Delivery;

instance DIA_Thorus_Hello (C_INFO) {
	npc = GRD_200_Thorus;
	nr = 1;
	condition = DIA_Thorus_Hello_Condition;
	information = DIA_Thorus_Hello_Info;
	important = TRUE;
	description = "Who are you?";
};
func int DIA_Thorus_Hello_Condition() {
	if (Kapitel == 1 && !Npc_KnowsInfo(other, DIA_Thorus_Hello) && Npc_GetDistToNpc(self, other) < 500) { return TRUE; };
	return FALSE;
};
func void DIA_Thorus_Hello_Info() {
	AI_Output(self, other, "DIA_Thorus_Hello_09_00");
	AI_Output(other, self, ConcatStrings("DIA_Thorus_Hello_15_", IntToString(self.id + other.level)));
	Guards_Greeted += 1;
};

instance DIA_Thorus_Job (C_INFO) {
	npc = GRD_200_Thorus;
	nr = 2;
	condition = DIA_Thorus_Job_Condition;
	information = DIA_Thorus_Job_Info;
	description = "What do you do here?";
};
func int DIA_Thorus_Job_Condition() {
	return Npc_KnowsInfo(other, DIA_Thorus_Hello);
};
func void DIA_Thorus_Job_Info() {
	AI_Output(other, self, "DIA_Thorus_Job_15_00");
	AI_Output(self, other, "DIA_Thorus_Job_09_01");
	AI_Output(self, other, "DIA_Thorus_Job_09_02");
};

// A task with a choice: the answer is asked for, and the diary keeps it.
instance DIA_Thorus_Delivery (C_INFO) {
	npc = GRD_200_Thorus;
	nr = 3;
	condition = DIA_Thorus_Delivery_Condition;
	information = DIA_Thorus_Delivery_Info;
	description = "Is there anything I can do for you?";
};
func int DIA_Thorus_Delivery_Condition() {
	return Npc_KnowsInfo(other, DIA_Thorus_Hello) && MIS_Delivery == 0;
};
func void DIA_Thorus_Delivery_Info() {
	AI_Output(other, self, "DIA_Thorus_Delivery_15_00");
	AI_Output(self, other, "DIA_Thorus_Delivery_09_01");
	Info_ClearChoices(DIA_Thorus_Delivery);
	Info_AddChoice(DIA_Thorus_Delivery, "I will bring you an apple.", DIA_Thorus_Delivery_Yes);
	Info_AddChoice(DIA_Thorus_Delivery, "Not now.", DIA_Thorus_Delivery_No);
};
func void DIA_Thorus_Delivery_Yes() {
	AI_Output(other, self, "DIA_Thorus_Delivery_Yes_15_00");
	AI_Output(self, other, "DIA_Thorus_Delivery_Yes_09_01");
	MIS_Delivery = LOG_RUNNING;
	Log_CreateTopic(TOPIC_Delivery, LOG_MISSION);
	Log_SetTopicStatus(TOPIC_Delivery, LOG_RUNNING);
	Log_AddEntry(TOPIC_Delivery, "Thorus wants an apple. I am to bring it to him at the gate.");
	Info_ClearChoices(DIA_Thorus_Delivery);
};
func void DIA_Thorus_Delivery_No() {
	AI_Output(other, self, "DIA_Thorus_Delivery_No_15_00");
	Info_ClearChoices(DIA_Thorus_Delivery);
};

instance DIA_Thorus_DeliveryDone (C_INFO) {
	npc = GRD_200_Thorus;
	nr = 4;
	condition = DIA_Thorus_DeliveryDone_Condition;
	information = DIA_Thorus_DeliveryDone_Info;
	description = "Here is your apple.";
};
func int DIA_Thorus_DeliveryDone_Condition() {
	return MIS_Delivery == LOG_RUNNING && Npc_HasItems(hero, ItFoApple) >= 1;
};
func void DIA_Thorus_DeliveryDone_Info() {
	AI_Output(other, self, "DIA_Thorus_DeliveryDone_15_00");
	Npc_RemoveInvItem(hero, ItFoApple);
	AI_Output(self, other, "DIA_Thorus_DeliveryDone_09_01");
	MIS_Delivery = LOG_SUCCESS;
	Log_SetTopicStatus(TOPIC_Delivery, LOG_SUCCESS);
	Log_AddEntry(TOPIC_Delivery, "Thorus has had his apple and thanked me for it.");
	CreateInvItems(hero, ItFo_Potion_Health_01, 1);
	B_GiveXP(150);
	// The story goes on: a chapter ends, the next begins.
	Kapitel = 2;
	Log_AddEntry(TOPIC_Delivery, "Chapter 2 has begun: the camp knows my name now.");
	IntroduceChapter("Chapter 2", "A Name in the Camp", "chapter2.tga", "chapter_02.wav", 6000);
	// The favour is done: he goes to sit at the fire, as his new routine says.
	Npc_ExchangeRoutine(self, "rest");
};
func void Rtn_rest_200() {
	TA_Stand(00, 00, 12, 00, "CAMP_FIRE");
	TA_Stand(12, 00, 24, 00, "CAMP_FIRE");
};

instance DIA_Thorus_EXIT (C_INFO) {
	npc = GRD_200_Thorus;
	nr = 999;
	condition = DIA_Thorus_EXIT_Condition;
	information = DIA_Thorus_EXIT_Info;
	permanent = TRUE;
	description = DIALOG_ENDE;
};
func int DIA_Thorus_EXIT_Condition() {
	return TRUE;
};
func void DIA_Thorus_EXIT_Info() {
	AI_StopProcessInfos(self);
};

instance DIA_Jackal_Hello (C_INFO) {
	npc = GRD_201_Jackal;
	nr = 1;
	condition = DIA_Jackal_Hello_Condition;
	information = DIA_Jackal_Hello_Info;
	permanent = TRUE;
	description = "How are things?";
};
func int DIA_Jackal_Hello_Condition() {
	return TRUE;
};
func void DIA_Jackal_Hello_Info() {
	AI_Output(other, self, "DIA_Jackal_Hello_15_00");
	AI_Output(self, other, "DIA_Jackal_Hello_07_01");
};
instance DIA_Jackal_EXIT (C_INFO) {
	npc = GRD_201_Jackal;
	nr = 999;
	condition = DIA_Jackal_EXIT_Condition;
	information = DIA_Jackal_EXIT_Info;
	permanent = TRUE;
	description = DIALOG_ENDE;
};
func int DIA_Jackal_EXIT_Condition() {
	return TRUE;
};
func void DIA_Jackal_EXIT_Info() {
	AI_StopProcessInfos(self);
};

instance DIA_Digger_Camp (C_INFO) {
	npc = VLK_500_Digger;
	nr = 1;
	condition = DIA_Digger_Camp_Condition;
	information = DIA_Digger_Camp_Info;
	description = "Tell me about the camp.";
};
func int DIA_Digger_Camp_Condition() {
	return TRUE;
};
func void DIA_Digger_Camp_Info() {
	AI_Output(other, self, "DIA_Digger_Camp_15_00");
	AI_Output(self, other, "DIA_Digger_Camp_02_01");
	Log_AddEntry(TOPIC_Camp, "A digger told me the camp is safe as long as one keeps away from the guards' fire after dark.");
};
instance DIA_Digger_Work (C_INFO) {
	npc = VLK_500_Digger;
	nr = 2;
	condition = DIA_Digger_Work_Condition;
	information = DIA_Digger_Work_Info;
	permanent = TRUE;
	description = "How is work?";
};
func int DIA_Digger_Work_Condition() {
	return self.aivar[AIV_PARTYMEMBER] == FALSE && (hero.attribute[ATR_STRENGTH] >= 10 || Hlp_Random(3) == 1);
};
func void DIA_Digger_Work_Info() {
	AI_Output(other, self, "DIA_Digger_Work_15_00");
	if (Hlp_StrCmp(self.name, NAME_Digger)) { self.aivar[AIV_PARTYMEMBER] = TRUE; };
};
instance DIA_Digger_EXIT (C_INFO) {
	npc = VLK_500_Digger;
	nr = 999;
	condition = DIA_Digger_EXIT_Condition;
	information = DIA_Digger_EXIT_Info;
	permanent = TRUE;
	description = DIALOG_ENDE;
};
func int DIA_Digger_EXIT_Condition() {
	return TRUE;
};
func void DIA_Digger_EXIT_Info() {
	AI_StopProcessInfos(self);
};

// ---------------------------------------------------------------- start-up
func void STARTUP_SUB_CAMP() {
	Wld_InsertNpc(GRD_200_Thorus, "CAMP_GATE_GUARD_L");
	Wld_InsertNpc(GRD_201_Jackal, "CAMP_GATE_GUARD_R");
	Wld_InsertNpc(VLK_500_Digger, "CAMP_SQUARE");
	Wld_InsertNpc(VLK_501_Digger, "CAMP_SQUARE");
	Camp_Population = 4;
};
func void STARTUP_SUB_WILDERNESS() {
	Wld_InsertNpc(Scavenger,  "FOREST_SCAVENGER_1");
	Wld_InsertNpc(Scavenger,  "FOREST_SCAVENGER_2");
	Wld_InsertNpc(YScavenger, "FOREST_SCAVENGER_3");
	Wld_InsertNpc(Molerat,    "FOREST_MOLERAT_1");
	Wld_InsertItem(ItMw_Sword, "CAMP_SMITH");
};
// What the guilds think of one another: the humans of a square of the
// table, the creatures one by one (as the original's B_InitGuildAttitudes
// and B_InitMonsterAttitudes).
const int GIL_ATTITUDES[16] = {
	ATT_NEUTRAL, ATT_NEUTRAL, ATT_NEUTRAL, ATT_NEUTRAL,
	ATT_NEUTRAL, ATT_FRIENDLY, ATT_FRIENDLY, ATT_FRIENDLY,
	ATT_NEUTRAL, ATT_FRIENDLY, ATT_FRIENDLY, ATT_FRIENDLY,
	ATT_NEUTRAL, ATT_FRIENDLY, ATT_FRIENDLY, ATT_FRIENDLY
};
func void B_InitGuildAttitudes() {
	Wld_ExchangeGuildAttitudes("GIL_ATTITUDES");
	Wld_SetGuildAttitude(GIL_SCAVENGER, ATT_NEUTRAL, GIL_MOLERAT);
	Wld_SetGuildAttitude(GIL_MOLERAT, ATT_NEUTRAL, GIL_SCAVENGER);
	Wld_SetGuildAttitude(GIL_SCAVENGER, ATT_NEUTRAL, GIL_SCAVENGER);
	Wld_SetGuildAttitude(GIL_MOLERAT, ATT_NEUTRAL, GIL_MOLERAT);
	Wld_SetGuildAttitude(GIL_SCAVENGER, ATT_HOSTILE, GIL_NONE);
};
func void STARTUP_WORLD() {
	Kapitel = 1;
	B_InitGuildAttitudes();
	STARTUP_SUB_CAMP();
	STARTUP_SUB_WILDERNESS();
	Wld_SetTime(8, 0);
	// The diary's first topic, as the game's prologue writes one.
	Log_CreateTopic(TOPIC_Camp, LOG_MISSION);
	Log_SetTopicStatus(TOPIC_Camp, LOG_RUNNING);
	Log_AddEntry(TOPIC_Camp, "The guard at the gate said I should look round the camp first.");
	IntroduceChapter("Chapter 1", "The Colony", "chapter1.tga", "chapter_01.wav", 6000);
};
func void INIT_WORLD() {
	var C_NPC thorus;
	thorus = Hlp_GetNpc(GRD_200_Thorus);
	if (Hlp_IsValidNpc(thorus)) {
		thorus.aivar[AIV_WASDEFEATED] = FALSE;
		PrintDebug(ConcatStrings("the gate is kept by ", thorus.name));
	};
};
func void STARTUP_CAVE() {
	Wld_InsertNpc(GRD_210_CaveGuard, "CAVE_GUARD");
	Wld_InsertNpc(Molerat, "CAVE_MOLERAT_1");
	Wld_InsertNpc(Molerat, "CAVE_MOLERAT_2");
};
func void INIT_CAVE() {
};
)DAEDALUS";
}

// The fight AI, compiled on its own as the game's Fight.src is (FIGHT.DAT):
// per tactic, the moves a fighter picks from in each situation.
const char* syntheticFightScriptSource() {
	return R"DAEDALUS(
const int MOVE_RUN = 1;
const int MOVE_RUNBACK = 2;
const int MOVE_JUMPBACK = 3;
const int MOVE_TURN = 4;
const int MOVE_STRAFE = 5;
const int MOVE_ATTACK = 6;
const int MOVE_SIDEATTACK = 7;
const int MOVE_FRONTATTACK = 8;
const int MOVE_TRIPLEATTACK = 9;
const int MOVE_WHIRLATTACK = 10;
const int MOVE_MASTERATTACK = 11;
const int MOVE_TURNTOHIT = 15;
const int MOVE_PARADE = 17;
const int MOVE_STANDUP = 18;
const int MOVE_WAIT = 19;
const int MAX_MOVE = 6;
const int MAX_FIGHTAI = 30;

class C_FightAI {
	var int move[MAX_MOVE];
};

instance FA_ENEMY_PREHIT_3 (C_FightAI) { move[0] = MOVE_PARADE; move[1] = MOVE_JUMPBACK; move[2] = MOVE_SIDEATTACK; move[3] = MOVE_WAIT; };
instance FA_MY_W_FOCUS_3 (C_FightAI) { move[0] = MOVE_ATTACK; move[1] = MOVE_TRIPLEATTACK; move[2] = MOVE_FRONTATTACK; move[3] = MOVE_STRAFE; };
instance FA_MY_W_NOFOCUS_3 (C_FightAI) { move[0] = MOVE_TURN; };
instance FA_MY_W_RUNTO_3 (C_FightAI) { move[0] = MOVE_TURN; };
instance FA_MY_W_STRAFE_3 (C_FightAI) { move[0] = MOVE_TURN; };
instance FA_MY_G_FOCUS_3 (C_FightAI) { move[0] = MOVE_RUN; };
instance FA_MY_G_FK_NOFOCUS_3 (C_FightAI) { move[0] = MOVE_RUN; };
instance FA_ENEMY_PREHIT_15 (C_FightAI) { move[0] = MOVE_JUMPBACK; move[1] = MOVE_ATTACK; };
instance FA_MY_W_FOCUS_15 (C_FightAI) { move[0] = MOVE_ATTACK; move[1] = MOVE_ATTACK; move[2] = MOVE_WAIT; };
instance FA_MY_W_NOFOCUS_15 (C_FightAI) { move[0] = MOVE_TURN; };
instance FA_MY_W_STRAFE_15 (C_FightAI) { move[0] = MOVE_JUMPBACK; move[1] = MOVE_TURN; };
instance FA_MY_G_FOCUS_15 (C_FightAI) { move[0] = MOVE_RUN; };
instance FA_MY_G_FK_NOFOCUS_15 (C_FightAI) { move[0] = MOVE_RUN; };

)DAEDALUS";
}

// The words of the built-in scene's talks, by the names the script gives them.
void buildSyntheticDialogue(SrcDialogue& dialogue) {
	dialogue = SrcDialogue();
	dialogue.source = "the built-in script";
	dialogue.lines = {
	    {"DIA_Thorus_Hello_09_00", "Halt. I am Thorus, I keep this gate. Nobody gets into the camp without my say."},
	    {"DIA_Thorus_Hello_15_200", "I only wanted to know who stands in my way."},
	    {"DIA_Thorus_Job_15_00", "What do you do here?"},
	    {"DIA_Thorus_Job_09_01", "I watch the gate. The diggers dig, the guards guard, and I see to it that it stays so."},
	    {"DIA_Thorus_Job_09_02", "Cause no trouble and you will have none."},
	    {"DIA_Thorus_Delivery_15_00", "Is there anything I can do for you?"},
	    {"DIA_Thorus_Delivery_09_01", "As it happens, there is. I have a hunger for an apple, and nobody here will fetch me one."},
	    {"DIA_Thorus_Delivery_Yes_15_00", "I will bring you an apple."},
	    {"DIA_Thorus_Delivery_Yes_09_01", "Good. Bring it here to the gate, and I will not forget it."},
	    {"DIA_Thorus_Delivery_No_15_00", "Not now."},
	    {"DIA_Thorus_DeliveryDone_15_00", "Here is your apple."},
	    {"DIA_Thorus_DeliveryDone_09_01", "Ah, at last. Take this for your trouble. I think I will sit down by the fire a while."},
	    {"DIA_Jackal_Hello_15_00", "How are things?"},
	    {"DIA_Jackal_Hello_07_01", "Quiet. That is how I like them."},
	    {"DIA_Digger_Camp_15_00", "Tell me about the camp."},
	    {"DIA_Digger_Camp_02_01", "Keep away from the guards' fire after dark and you will live here well enough."},
	    {"DIA_Digger_Work_15_00", "How is work?"},
	};
}

// The sounds, compiled on their own as the game's Sfx.src is (SFX.DAT), and the
// music themes likewise (MUSIC.DAT).
const char* syntheticSfxScriptSource() {
	return R"DAEDALUS(
class C_SFX {
	var string file;
	var int    pitchOff;
	var int    pitchVar;
	var int    vol;
	var int    loop;
	var int    loopStartOffset;
	var int    loopEndOffset;
	var float  reverbLevel;
	var string pfxName;
};
instance SFX_TEST_STEP (C_SFX) {
	file = "STEP.WAV";
	vol  = 100;
};

instance SFX_TEST_STEP_STONE (C_SFX) {
	file = "STEP_STONE.WAV";
	vol  = 127;
};

instance SFX_TEST_HUM (C_SFX) {
	file = "HUM.WAV";
	loop = 1;
};
)DAEDALUS";
}

const char* syntheticMusicScriptSource() {
	return R"DAEDALUS(
class C_MUSICTHEME {
	var string file;
	var float  vol;
	var int    loop;
	var float  reverbMix;
	var float  reverbTime;
	var int    transType;
	var int    transSubType;
};
instance TEST_DAY_STD (C_MUSICTHEME) {
	file = "TEST_DAY_STD.sgt";
	loop = 1;
};

instance TEST_NGT_STD (C_MUSICTHEME) {
	file = "TEST_NGT_STD.sgt";
	loop = 1;
};

instance TEST_FIG (C_MUSICTHEME) {
	file = "TEST_FIG.sgt";
	loop = 1;
};
)DAEDALUS";
}

void buildSyntheticSfxScript(SrcScript& script) {
	script = SrcScript();
	script.source = "the built-in sound script";
	compileDaedalus(syntheticSfxScriptSource(), script);
}

void buildSyntheticMusicScript(SrcScript& script) {
	script = SrcScript();
	script.source = "the built-in music script";
	compileDaedalus(syntheticMusicScriptSource(), script);
}

void buildSyntheticFightScript(SrcScript& script) {
	script = SrcScript();
	script.source = "the built-in fight script";
	compileDaedalus(syntheticFightScriptSource(), script);
}

void buildSyntheticScript(SrcScript& script) {
	script = SrcScript();
	script.source = "the built-in script";
	compileDaedalus(syntheticScriptSource(), script);
}

} // namespace g3
