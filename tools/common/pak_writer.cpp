#include "pak_writer.h"

#include "lz4hc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace g3 {

void PakWriter::add(const std::string& name, AssetType type, const std::vector<u8>& data, bool compress) {
	Item it;
	it.name = name;
	it.hash = hashName(name.c_str());
	it.type = type;
	it.size = u32(data.size());
	for (const Item& other : _items) {
		if (other.hash == it.hash)
			throw std::runtime_error("pak: name hash collision between '" + other.name + "' and '" + name + "'");
	}

	it.codec = PAK_RAW;
	if (compress && data.size() > 64 && data.size() < size_t(LZ4_MAX_INPUT_SIZE)) {
		std::vector<u8> out(size_t(LZ4_compressBound(int(data.size()))));
		int n = LZ4_compress_HC(reinterpret_cast<const char*>(data.data()), reinterpret_cast<char*>(out.data()),
		                        int(data.size()), int(out.size()), LZ4HC_CLEVEL_MAX);
		// Only keep the compressed form if it actually pays for the decode.
		if (n > 0 && size_t(n) < data.size() - data.size() / 16) {
			out.resize(size_t(n));
			it.packed = std::move(out);
			it.codec = PAK_LZ4;
		}
	}
	if (it.codec == PAK_RAW) it.packed = data;
	_items.push_back(std::move(it));
}

void PakWriter::write(const std::string& path) const {
	std::vector<const Item*> sorted;
	for (const Item& it : _items) sorted.push_back(&it);
	std::sort(sorted.begin(), sorted.end(), [](const Item* a, const Item* b) { return a->hash < b->hash; });

	std::vector<PakEntry> table;
	std::vector<char> names;
	u64 offset = sizeof(PakHeader);
	for (const Item* it : sorted) {
		PakEntry e {};
		e.hash = it->hash;
		e.offset = u32(offset);
		e.packedSize = u32(it->packed.size());
		e.size = it->size;
		e.type = it->type;
		e.codec = it->codec;
		e.nameOffset = u32(names.size());
		names.insert(names.end(), it->name.begin(), it->name.end());
		names.push_back('\0');
		table.push_back(e);
		offset += (it->packed.size() + 3) & ~size_t(3); // keep blobs 4-byte aligned
		if (offset > 0xFFFFFFFFull) throw std::runtime_error("pak: archive exceeds 4 GB");
	}

	PakHeader h {};
	h.magic = PAK_MAGIC;
	h.version = PAK_VERSION;
	h.entryCount = u32(table.size());
	h.tableOffset = u32(offset);
	h.namesOffset = u32(offset + table.size() * sizeof(PakEntry));
	h.namesSize = u32(names.size());

	FILE* f = fopen(path.c_str(), "wb");
	if (!f) throw std::runtime_error("pak: cannot create '" + path + "'");
	bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
	static const u8 pad[4] = {0, 0, 0, 0};
	for (const Item* it : sorted) {
		if (!it->packed.empty()) ok = ok && fwrite(it->packed.data(), 1, it->packed.size(), f) == it->packed.size();
		size_t rem = (4 - (it->packed.size() & 3)) & 3;
		if (rem) ok = ok && fwrite(pad, 1, rem, f) == rem;
	}
	if (!table.empty()) ok = ok && fwrite(table.data(), sizeof(PakEntry), table.size(), f) == table.size();
	if (!names.empty()) ok = ok && fwrite(names.data(), 1, names.size(), f) == names.size();
	ok = (fclose(f) == 0) && ok;
	if (!ok) throw std::runtime_error("pak: write to '" + path + "' failed");
}

u64 PakWriter::packedBytes(AssetType type) const {
	u64 n = 0;
	for (const Item& it : _items)
		if (it.type == type) n += it.packed.size();
	return n;
}

u64 PakWriter::rawBytes(AssetType type) const {
	u64 n = 0;
	for (const Item& it : _items)
		if (it.type == type) n += it.size;
	return n;
}

} // namespace g3
