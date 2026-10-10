// The words people say, cut into blocks (g3ds/dialog.h).
#include "g3ds/dialog.h"
#include "script_src.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace g3 {

namespace {

constexpr size_t kBlockText = 24 * 1024;

template <class T> void append(std::vector<u8>& out, const T& v) {
	const u8* p = reinterpret_cast<const u8*>(&v);
	out.insert(out.end(), p, p + sizeof(T));
}

} // namespace

std::vector<DialogueAsset> bakeDialogue(const SrcDialogue& dialogue, std::string& report) {
	struct Entry {
		u32 hash;
		const std::string* text;
	};
	std::vector<Entry> entries;
	entries.reserve(dialogue.lines.size());
	for (const auto& line : dialogue.lines)
		if (!line.first.empty()) entries.push_back({hashName(line.first.c_str()), &line.second});
	// By hash; of lines with one hash (the same name twice) the first stays.
	std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
	size_t clashes = 0;
	entries.erase(std::unique(entries.begin(), entries.end(),
	                          [&](const Entry& a, const Entry& b) {
		                          if (a.hash != b.hash) return false;
		                          ++clashes;
		                          return true;
	                          }),
	              entries.end());

	std::vector<DialogueAsset> out;
	std::vector<u32> firsts;
	size_t textTotal = 0;
	for (size_t at = 0; at < entries.size();) {
		// A block: lines until its texts are about a block's worth.
		size_t end = at, bytes = 0;
		while (end < entries.size() && (end == at || bytes + entries[end].text->size() + 1 <= kBlockText)) {
			bytes += entries[end].text->size() + 1;
			++end;
		}
		DialogueAsset block;
		char name[32];
		std::snprintf(name, sizeof(name), "dialog/%zu", firsts.size());
		block.name = name;
		append(block.bytes, DialogBlockHeader {u32(end - at), u32(bytes)});
		u32 offset = 0;
		for (size_t i = at; i < end; ++i) {
			append(block.bytes, DialogLine {entries[i].hash, offset});
			offset += u32(entries[i].text->size() + 1);
		}
		for (size_t i = at; i < end; ++i) {
			block.bytes.insert(block.bytes.end(), entries[i].text->begin(), entries[i].text->end());
			block.bytes.push_back(0);
		}
		firsts.push_back(entries[at].hash);
		textTotal += bytes;
		out.push_back(std::move(block));
		at = end;
	}
	DialogueAsset index;
	index.name = "dialog/index";
	DialogIndexHeader h {};
	h.magic = DIALOG_MAGIC;
	h.version = DIALOG_VERSION;
	h.blockCount = u32(firsts.size());
	h.lineCount = u32(entries.size());
	append(index.bytes, h);
	for (u32 f : firsts) append(index.bytes, f);
	out.insert(out.begin(), std::move(index));

	char buf[256];
	std::snprintf(buf, sizeof(buf), "dialogue: %zu lines (%zu KB of text) from %s, in %zu blocks%s", entries.size(), textTotal / 1024,
	              dialogue.source.c_str(), firsts.size(),
	              clashes ? (std::string(", ") + std::to_string(clashes) + " names given twice (the first kept)").c_str() : "");
	report = buf;
	return out;
}

} // namespace g3
