// Barter. A character whose C_INFO has TRADE set, once what the info says has been said (npc_dialog.cpp), has the
// bottom screen turn into the trade page - the chest page's two sides, the trader's wares and the pack, with
// prices. The rules are Gothic's own, as OpenGothic has them (Npc::buyItem / sellItem, Item::cost / sellCost,
// InventoryMenu in the state Trade):
//  * Buying costs the item's C_ITEM.VALUE for each; as many are bought as the purse allows. If it allows none,
//    the scripts' own complaint (player_trade_not_enough_gold) is called, else a line of ours says so.
//  * Selling pays the value times the scripts' TRADE_VALUE_MULTIPLIER (none in Gothic 1: the whole value),
//    rounded up. The money (ITMINUGGET in Gothic 1; TRADE_CURRENCY_INSTANCE in Gothic 2) is neither bought nor sold,
//    nor are the things the trader wears or the player has on.
//  * The trader's purse is what he carries (fix158): he pays from his own money and takes what the player pays for his
//    wares; with too little he buys fewer things, or none (Gothic 1's traders ran out of ore).
// The page is closed with B (or the page's back arrow); the talk then goes on from its menu.
#include "lang.h"
#include "app.h"

#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 NONE = SCRIPT_NONE;

} // namespace

bool App::traderStack(u32 npc, u32 k, u32& symbol, i32& count) const {
	Game& g = const_cast<App*>(this)->_npcs.game();
	if (npc >= g.npcCount()) return false;
	const GameNpc& t = g.npc(npc);
	const u32 money = g.tradeCurrency();
	u32 n = 0;
	for (u32 i = 0; i < g.itemStackCount(); ++i) {
		const GameItem& s = g.itemStack(i);
		if (s.npc != npc || s.count <= 0 || s.symbol == money) continue;
		i32 c = s.count;
		// (What he wears or holds is not for sale, one of each.)
		if (s.symbol == t.armorItem || s.symbol == t.melee || s.symbol == t.ranged) c -= 1;
		if (c <= 0) continue;
		if (n++ == k) {
			symbol = s.symbol;
			count = c;
			return true;
		}
	}
	return false;
}

void App::tradeMove(u32 symbol, i32 amount, bool sell) {
	const int hi = heroIndex();
	const int tr_ = _npcs.talkPartner();
	if (hi < 0 || tr_ < 0 || !_npcs.trading() || amount <= 0) return;
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(symbol);
	const u32 money = g.tradeCurrency();
	if (!it || money == NONE || symbol == money) return;
	const u32 hero = u32(hi), trader = u32(tr_);
	char name[48], buf[120];
	scriptText(g.text(it->description ? it->description : it->name), name, sizeof(name));
	if (!sell) {
		i32 avail = 0;
		u32 sym;
		i32 c;
		for (u32 k = 0; traderStack(trader, k, sym, c); ++k)
			if (sym == symbol) avail = c;
		if (avail <= 0) return;
		const i32 price = g.buyPrice(symbol);
		const i32 purse = g.itemCount(hero, money);
		i32 n = amount < avail ? amount : avail;
		if (price > 0 && n > purse / price) n = purse / price;
		if (n <= 0) {
			// The scripts may have their own complaint (Gothic 2 does); else the line is ours.
			const u32 fn = g.function("PLAYER_TRADE_NOT_ENOUGH_GOLD");
			if (fn != NONE) g.callAs(fn, hero, -1);
			else itemMessage(tr("Не хватает руды"), true);
			++_itemStats.refused;
			return;
		}
		g.giveItems(trader, symbol, -n);
		g.giveItems(hero, symbol, n);
		if (price > 0) {
			g.giveItems(hero, money, -price * n);
			g.giveItems(trader, money, price * n); // (the money goes to the trader)
		}
		if (n > 1) snprintf(buf, sizeof(buf), tr("Куплено: %s ×%ld (-%ld)"), name, static_cast<long>(n), static_cast<long>(price) * n);
		else snprintf(buf, sizeof(buf), tr("Куплено: %s (-%ld)"), name, static_cast<long>(price));
		itemMessage(buf, false);
		++_itemStats.pickedUp;
	} else {
		const GameNpc& h = g.npc(hero);
		const i32 have = g.itemCount(hero, symbol);
		const bool worn = symbol == h.melee || symbol == h.ranged || symbol == h.armorItem || symbol == _amulet || symbol == _rings[0] ||
		                  symbol == _rings[1];
		i32 n = amount < have ? amount : have;
		if (worn) n = have > 1 ? (n < have - 1 ? n : have - 1) : 0;
		if (_job.stage != JOB_NONE && _job.symbol == symbol) n = 0;
		if (n <= 0) {
			snprintf(buf, sizeof(buf), worn ? tr("Сначала снимите: %s") : tr("Нельзя: %s"), name);
			itemMessage(buf);
			return;
		}
		const i32 price = g.sellPrice(symbol);
		// He pays from his own purse: no more things than it covers.
		const i32 purse = g.itemCount(trader, money);
		if (price > 0 && n > purse / price) n = purse / price;
		if (n <= 0) {
			itemMessage(tr("У торговца нет руды"), true);
			++_itemStats.refused;
			return;
		}
		g.giveItems(hero, symbol, -n);
		g.giveItems(trader, symbol, n);
		long long pay = (long long)price * n;
		if (pay > 1000000000LL) pay = 1000000000LL;
		if (pay > 0) {
			g.giveItems(trader, money, -i32(pay));
			g.giveItems(hero, money, i32(pay));
		}
		if (n > 1) snprintf(buf, sizeof(buf), tr("Продано: %s ×%ld (+%ld)"), name, static_cast<long>(n), static_cast<long>(pay));
		else snprintf(buf, sizeof(buf), tr("Продано: %s (+%ld)"), name, static_cast<long>(pay));
		itemMessage(buf, false);
	}
	_invStamp = 0;
}

void App::tradeDecoratePack(InventoryView& v) {
	Game& g = _npcs.game();
	const u32 money = g.tradeCurrency();
	for (u32 i = 0; i < v.count; ++i) {
		ItemEntry& e = v.items[i];
		e.price = g.sellPrice(e.symbol);
		if (e.symbol == money) e.flags |= ITEMF_NOSELL;
	}
}

// The trade page as the bottom screen shows it: the trader's wares on the first side.
void App::tradeView(const GameNpc& hero) {
	InventoryView::Chest& v = _inv.chest;
	Game& g = _npcs.game();
	const int partner = _npcs.talkPartner();
	v.open = true;
	v.lock = false;
	v.trade = true;
	v.count = 0;
	v.length = 0;
	v.progress = v.picks = 0;
	v.text[0] = 0;
	v.name[0] = 0;
	v.currency[0] = 0;
	const u32 money = g.tradeCurrency();
	v.gold = money != NONE ? g.itemCount(u32(heroIndex()), money) : 0;
	if (const GameItemInfo* mi = money != NONE ? g.itemInfo(money) : nullptr)
		scriptText(g.text(mi->name ? mi->name : mi->description), v.currency, sizeof(v.currency));
	if (partner < 0) return;
	corpseName(u32(partner), v.name, sizeof(v.name));
	u32 sym;
	i32 cnt;
	for (u32 k = 0; v.count < InventoryView::CHEST_MAX && traderStack(u32(partner), k, sym, cnt); ++k) {
		ItemEntry& e = v.items[v.count];
		fillEntry(e, sym, cnt, hero, false);
		if (e.symbol != sym) continue;
		e.price = g.buyPrice(sym);
		++v.count;
	}
	// By kind, like the pack (the same order).
	for (u32 i = 1; i < v.count; ++i) {
		u32 k = i;
		while (k > 0 && (v.items[i].kind < v.items[k - 1].kind ||
		                 (v.items[i].kind == v.items[k - 1].kind && v.items[i].value > v.items[k - 1].value)))
			--k;
		if (k == i) continue;
		ItemEntry tmp;
		memcpy(&tmp, &v.items[i], sizeof(tmp));
		memmove(&v.items[k + 1], &v.items[k], sizeof(ItemEntry) * (i - k));
		memcpy(&v.items[k], &tmp, sizeof(tmp));
	}
}

} // namespace g3
