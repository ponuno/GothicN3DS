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
const int DEFAULT = 0;
const int TRUE = 1;
const int FALSE = 0;

const int AIV_WASDEFEATED = 3;
const int AIV_PARTYMEMBER = 12;

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

instance self, other (C_NPC);
instance victim (C_NPC);
instance hero (C_NPC);
instance item (C_ITEM);

// ---------------------------------------------------------------- the engine
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

// The states of the daily routine.
func void ZS_Guard()        {};
func int  ZS_Guard_Loop()   { return 0; };
func void ZS_Guard_End()    {};
func void ZS_Stand()        {};
func void ZS_Sleep()        {};
func void ZS_SitAround()    {};
func void ZS_Smith()        {};
func void ZS_MM_AllScheduler() {};

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
	damagetotal = 30;
	value = 100 + damagetotal * 3;
	visual = "ItMw_1H_Sword_01.3DS";
	description = name;
};

// ---------------------------------------------------------------- characters
prototype Npc_Default (C_NPC) {
	attribute[ATR_STRENGTH]      = 10;
	attribute[ATR_DEXTERITY]     = 10;
	attribute[ATR_HITPOINTS_MAX] = 40;
	attribute[ATR_HITPOINTS]     = 40;
	senses_range = 2000;
	damagetype = 2;
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
	Mdl_SetVisual(self, "HUMANS.MDS");
	//            body mesh          body tex  skin  head mesh        head tex  teeth  armour
	Mdl_SetVisualBody(self, "hum_body_Naked0", 4, 1, "Hum_Head_Pony", 9, 0, -1);
};

instance GRD_200_Thorus (Npc_Default) {
	name = "Thorus";
	npctype = NPCTYPE_MAIN;
	guild = GIL_GRD;
	voice = 9;
	id = 200;
	flags = NPC_FLAG_IMMORTAL;
	B_SetAttributes(self, 100, 400);
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
	AI_StopProcessInfos(self);
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
func void STARTUP_WORLD() {
	Kapitel = 1;
	STARTUP_SUB_CAMP();
	STARTUP_SUB_WILDERNESS();
	Wld_SetTime(8, 0);
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

void buildSyntheticScript(SrcScript& script) {
	script = SrcScript();
	script.source = "the built-in script";
	compileDaedalus(syntheticScriptSource(), script);
}

} // namespace g3
