// Builds a G3PK archive in memory and writes it out. Tool-side only (uses the STL).
#pragma once
#include "g3ds/pak.h"

#include <string>
#include <vector>

namespace g3 {

class PakWriter {
public:
	struct Item {
		std::string name;
		u32 hash;
		u16 type;
		u16 codec;
		u32 size;
		std::vector<u8> packed;
	};

	// Throws std::runtime_error on a duplicate name or a hash collision.
	void add(const std::string& name, AssetType type, const std::vector<u8>& data, bool compress = true);
	void write(const std::string& path) const;

	const std::vector<Item>& items() const { return _items; }
	u64 packedBytes(AssetType type) const;
	u64 rawBytes(AssetType type) const;

private:
	std::vector<Item> _items;
};

} // namespace g3
