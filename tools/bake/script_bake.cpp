#include "script_src.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

namespace g3 {

namespace {

template <class T> void append(std::vector<u8>& out, const T& v) {
	const u8* p = reinterpret_cast<const u8*>(&v);
	out.insert(out.end(), p, p + sizeof(T));
}

void pad4(std::vector<u8>& out) {
	while (out.size() % 4) out.push_back(0);
}

u32 elementCount(const SrcSymbol& s) { return s.count ? s.count : 1u; }

} // namespace

int SrcScript::find(const std::string& name) const {
	const u32 h = hashName(name.c_str());
	for (size_t i = 0; i < symbols.size(); ++i)
		if (hashName(symbols[i].name.c_str()) == h && symbols[i].name.size() == name.size()) return int(i);
	return -1;
}

std::vector<u8> bakeScript(const SrcScript& script, ScriptBakeResult& result) {
	const u32 n = u32(script.symbols.size());
	if (n == 0) throw std::runtime_error("the script has no symbols");
	if (n >= (1u << 24)) throw std::runtime_error("the script has too many symbols");
	const u32 codeSize = u32(script.code.size());

	std::vector<ScriptSymbol> symbols(n);
	std::vector<ScriptFunction> functions;
	std::vector<i32> data;
	std::vector<u32> stringOffsets;
	std::string stringPool;
	std::map<std::string, u32> pooled;
	std::map<u32, u32> byHash;
	u32 instanceSlots = 0, externals = 0, badAddresses = 0, badParents = 0, relaid = 0, members = 0;
	std::string clashes;

	// Classes are laid out here, not as the file has them: the layout only has
	// to agree with itself, and this way no file can make members overlap.
	// Whatever else is odd about a symbol is put right and counted, not
	// refused: a file the game itself runs must not fail here over a detail.
	std::vector<u32> classSize(n, 0);
	std::vector<char> isMember(n, 0);
	u32 odd = 0;
	std::string oddOnes;
	const auto oddity = [&](const SrcSymbol& s, const char* what) {
		if (++odd <= 4) oddOnes += (oddOnes.empty() ? "" : ", ") + s.name + " (" + what + ")";
	};
	for (u32 i = 0; i < n; ++i) {
		const SrcSymbol& s = script.symbols[i];
		if (!s.member) continue;
		if (s.parent < 0 || u32(s.parent) >= n || script.symbols[s.parent].type != SCRIPT_CLASS) {
			oddity(s, "a member of no class");
			continue;
		}
		isMember[i] = 1;
		++members;
	}

	for (u32 i = 0; i < n; ++i) {
		SrcSymbol s = script.symbols[i];
		s.member = isMember[i] != 0;
		ScriptSymbol& o = symbols[i];
		if (s.type > SCRIPT_INSTANCE) throw std::runtime_error("the symbol '" + s.name + "' has an unknown type");
		if (s.count > 0xFFF) {
			oddity(s, "too many elements");
			s.count = 0xFFF;
		}
		if (s.type == SCRIPT_FUNC && !s.member && (s.external || s.isConst) && s.count >= n - i) {
			oddity(s, "parameters past the table");
			s.count = 0;
		}
		o.nameHash = s.name.empty() ? 0 : hashName(s.name.c_str());
		if (o.nameHash) {
			const auto [it, fresh] = byHash.emplace(o.nameHash, i);
			if (!fresh) {
				// Two names, one hash: the first keeps it, the second cannot be
				// looked up by name (scripts refer to symbols by number).
				++result.hashClashes;
				if (result.hashClashes <= 4)
					clashes += (clashes.empty() ? "" : ", ") + s.name + " = " + script.symbols[it->second].name;
				o.nameHash = 0;
			}
		}
		o.type = u8(s.type);
		o.flags = u8((s.isConst ? SCRIPTF_CONST : 0) | (s.hasReturn ? SCRIPTF_RETURN : 0) | (s.member ? SCRIPTF_MEMBER : 0) |
		             (s.external ? SCRIPTF_EXTERNAL : 0));
		o.count = u16(s.count);
		o.parent = s.parent;
		if (o.parent < -1 || o.parent >= i32(n)) {
			o.parent = -1;
			++badParents;
		}
		o.value = 0;
		o.extra = 0;

		if (s.member) {
			u32& size = classSize[s.parent];
			const u32 bytes = elementCount(s) * (s.type == SCRIPT_STRING ? SCRIPT_STRING_SIZE : 4u);
			if (size + bytes > (1u << 16)) throw std::runtime_error("the class of '" + s.name + "' is too large");
			if (s.type == SCRIPT_INSTANCE) o.type = u8(SCRIPT_INT); // a member that refers to an object is its number
			o.value = size;
			if (size != s.memberOffset) ++relaid;
			size += bytes;
			continue;
		}
		switch (s.type) {
		case SCRIPT_INT:
			o.value = u32(data.size());
			for (u32 k = 0; k < elementCount(s); ++k) data.push_back(k < s.ints.size() ? s.ints[k] : 0);
			break;
		case SCRIPT_FLOAT:
			o.value = u32(data.size());
			for (u32 k = 0; k < elementCount(s); ++k) {
				const float f = k < s.floats.size() ? s.floats[k] : 0.0f;
				i32 bits;
				std::memcpy(&bits, &f, 4);
				data.push_back(bits);
			}
			break;
		case SCRIPT_STRING:
			o.value = u32(stringOffsets.size());
			for (u32 k = 0; k < elementCount(s); ++k) {
				const std::string text = k < s.strings.size() ? s.strings[k] : std::string();
				if (text.empty()) {
					stringOffsets.push_back(SCRIPT_NONE);
					continue;
				}
				const auto [it, fresh] = pooled.emplace(text, u32(stringPool.size()));
				if (fresh) {
					stringPool += text;
					stringPool.push_back('\0');
				}
				stringOffsets.push_back(it->second);
			}
			break;
		case SCRIPT_CLASS:
			++result.classes;
			break; // its size is known once all members have been seen
		case SCRIPT_FUNC:
			o.extra = u32(s.returnType);
			if (s.external) {
				o.flags |= SCRIPTF_CONST;
				o.value = externals++;
			} else if (s.isConst) {
				if (s.address < codeSize) {
					o.value = s.address;
					functions.push_back({s.address, i});
				} else {
					o.value = SCRIPT_NONE;
					++badAddresses;
				}
			} else {
				// A variable that holds a function.
				o.count = 0;
				o.value = u32(data.size());
				data.push_back(s.ints.empty() ? 0 : s.ints[0]);
			}
			break;
		case SCRIPT_PROTOTYPE:
			if (s.address < codeSize) {
				o.value = s.address;
				functions.push_back({s.address, i});
			} else {
				o.value = SCRIPT_NONE;
				++badAddresses;
			}
			break;
		case SCRIPT_INSTANCE:
			o.extra = instanceSlots++;
			o.value = SCRIPT_NONE;
			if (s.isConst) {
				++result.instances;
				if (s.address < codeSize) {
					o.value = s.address;
					functions.push_back({s.address, i});
				} else {
					++badAddresses;
				}
			}
			break;
		default:
			break;
		}
	}
	for (u32 i = 0; i < n; ++i)
		if (script.symbols[i].type == SCRIPT_CLASS && !script.symbols[i].member) symbols[i].value = classSize[i];

	// One symbol per address. Where the file has several, a function comes
	// before a prototype before an instance, an earlier symbol before a later.
	std::stable_sort(functions.begin(), functions.end(), [&](const ScriptFunction& a, const ScriptFunction& b) {
		if (a.address != b.address) return a.address < b.address;
		const auto rank = [&](u32 sym) {
			const ScriptType t = script.symbols[sym].type;
			return t == SCRIPT_FUNC ? 0 : t == SCRIPT_PROTOTYPE ? 1 : 2;
		};
		return rank(a.symbol) < rank(b.symbol);
	});
	u32 shared = 0;
	{
		std::vector<ScriptFunction> unique;
		for (const ScriptFunction& f : functions) {
			if (!unique.empty() && unique.back().address == f.address) {
				++shared;
				continue;
			}
			unique.push_back(f);
		}
		functions.swap(unique);
	}

	// States: every function that has a "_LOOP" or an "_END" to its name.
	std::vector<ScriptState> states;
	{
		const auto plain = [&](u32 i) {
			const SrcSymbol& s = script.symbols[i];
			return s.type == SCRIPT_FUNC && s.isConst && !s.external && !isMember[i] && symbols[i].value != SCRIPT_NONE;
		};
		const auto companion = [&](const std::string& name) {
			const auto it = byHash.find(hashName(name.c_str()));
			return it != byHash.end() && plain(it->second) ? it->second : SCRIPT_NONE;
		};
		for (u32 i = 0; i < n; ++i) {
			if (!plain(i) || script.symbols[i].name.empty() || symbols[i].nameHash == 0) continue;
			const u32 loop = companion(script.symbols[i].name + "_LOOP"), end = companion(script.symbols[i].name + "_END");
			if (loop != SCRIPT_NONE || end != SCRIPT_NONE) {
				states.push_back({i, loop, end});
				result.stateNames.push_back(script.symbols[i].name);
			}
		}
	}

	ScriptHeader h {};
	h.magic = SCRIPT_MAGIC;
	h.version = SCRIPT_VERSION;
	h.symbolCount = n;
	h.functionCount = u32(functions.size());
	h.dataCount = u32(data.size());
	h.stringCount = u32(stringOffsets.size());
	h.stringBytes = u32(stringPool.size());
	h.codeSize = codeSize;
	h.instanceSlots = instanceSlots;
	h.externalCount = externals;
	h.stateCount = u32(states.size());
	if (externals >= (1u << 16)) throw std::runtime_error("the script has too many externals");

	std::vector<u8> out;
	append(out, h);
	for (const ScriptSymbol& s : symbols) append(out, s);
	for (const ScriptFunction& f : functions) append(out, f);
	for (const ScriptState& st : states) append(out, st);
	for (i32 v : data) append(out, v);
	for (u32 v : stringOffsets) append(out, v);
	out.insert(out.end(), stringPool.begin(), stringPool.end());
	pad4(out);
	out.insert(out.end(), script.code.begin(), script.code.end());
	pad4(out);

	result.symbols = n;
	result.functions = u32(functions.size());
	result.externals = externals;
	char buf[400];
	std::snprintf(buf, sizeof(buf),
	              "scripts: %s: %u symbols, %u functions and instances (%u instances), %u externals, %u states, %u classes "
	              "with %u members, %.0f KB of code, %.0f KB of text; image %.2f MB\n",
	              script.source.empty() ? "?" : script.source.c_str(), n, result.functions, result.instances, externals,
	              h.stateCount, result.classes, members, double(codeSize) / 1024.0, double(stringPool.size()) / 1024.0,
	              double(out.size()) / 1048576.0);
	result.report += buf;
	if (result.hashClashes) {
		std::snprintf(buf, sizeof(buf), "scripts: %u names share a hash with an earlier one and cannot be found by name: %s%s\n",
		              result.hashClashes, clashes.c_str(), result.hashClashes > 4 ? ", ..." : "");
		result.report += buf;
	}
	if (badAddresses || badParents || shared) {
		std::snprintf(buf, sizeof(buf),
		              "scripts: %u symbols with code outside the file, %u with a parent outside the table, %u sharing an "
		              "address with another\n",
		              badAddresses, badParents, shared);
		result.report += buf;
	}
	if (odd) {
		std::snprintf(buf, sizeof(buf), "scripts: %u symbols put right: %s%s\n", odd, oddOnes.c_str(), odd > 4 ? ", ..." : "");
		result.report += buf;
	}
	if (relaid) {
		std::snprintf(buf, sizeof(buf), "scripts: %u of %u class members are laid out differently than the file says (harmless)\n",
		              relaid, members);
		result.report += buf;
	}
	for (const std::string& note : script.notes) result.report += "scripts: " + note + "\n";
	return out;
}

// ---- the game's own file format ----
//
//   u8 version; u32 symbolCount; u32 sortTable[symbolCount];
//   per symbol: u32 named; name '\n'; u32 offsetOrSizeOrReturnType;
//               u32 count:12, type:4, flags:6; u32 fileInfo[5];
//               content by type (not for members); i32 parent
//   u32 codeSize; u8 code[codeSize]

namespace {

constexpr u32 DAT_CONST = 1, DAT_RETURN = 2, DAT_MEMBER = 4, DAT_EXTERNAL = 8;

struct DatReader {
	const std::vector<u8>& d;
	size_t at = 0;
	void need(size_t n) const {
		if (d.size() - at < n) throw std::runtime_error("the script file is cut short");
	}
	u32 u() {
		need(4);
		u32 v;
		std::memcpy(&v, d.data() + at, 4);
		at += 4;
		return v;
	}
	std::string line() {
		std::string s;
		for (;;) {
			need(1);
			const char c = char(d[at++]);
			if (c == '\n') break;
			s.push_back(c);
		}
		return s;
	}
};

} // namespace

std::vector<u8> writeScriptDat(const SrcScript& script) {
	std::vector<u8> out;
	out.push_back(50);
	const u32 n = u32(script.symbols.size());
	append(out, n);
	// The table of symbols in the order of their names.
	std::vector<u32> order(n);
	for (u32 i = 0; i < n; ++i) order[i] = i;
	std::stable_sort(order.begin(), order.end(),
	                 [&](u32 a, u32 b) { return script.symbols[a].name < script.symbols[b].name; });
	for (u32 i : order) append(out, i);
	for (const SrcSymbol& s : script.symbols) {
		append(out, u32(s.name.empty() ? 0 : 1));
		if (!s.name.empty()) {
			out.insert(out.end(), s.name.begin(), s.name.end());
			out.push_back('\n');
		}
		u32 vary = 0;
		if (s.member)
			vary = s.memberOffset;
		else if (s.type == SCRIPT_CLASS)
			vary = s.classSize;
		else if (s.type == SCRIPT_FUNC)
			vary = u32(s.returnType);
		append(out, vary);
		const u32 flags = (s.isConst ? DAT_CONST : 0u) | (s.hasReturn ? DAT_RETURN : 0u) | (s.member ? DAT_MEMBER : 0u) |
		                  (s.external ? DAT_EXTERNAL : 0u);
		append(out, u32((s.count & 0xFFFu) | (u32(s.type) << 12) | (flags << 16)));
		for (int k = 0; k < 5; ++k) append(out, u32(0));
		if (!s.member) {
			switch (s.type) {
			case SCRIPT_FLOAT:
				for (u32 k = 0; k < s.count; ++k) append(out, k < s.floats.size() ? s.floats[k] : 0.0f);
				break;
			case SCRIPT_INT:
				for (u32 k = 0; k < s.count; ++k) append(out, k < s.ints.size() ? s.ints[k] : i32(0));
				break;
			case SCRIPT_STRING:
				for (u32 k = 0; k < s.count; ++k) {
					if (k < s.strings.size()) out.insert(out.end(), s.strings[k].begin(), s.strings[k].end());
					out.push_back('\n');
				}
				break;
			case SCRIPT_CLASS:
				append(out, u32(0));
				break;
			case SCRIPT_INSTANCE:
			case SCRIPT_FUNC:
			case SCRIPT_PROTOTYPE:
				append(out, s.address);
				break;
			default:
				break;
			}
		}
		append(out, s.parent);
	}
	append(out, u32(script.code.size()));
	out.insert(out.end(), script.code.begin(), script.code.end());
	return out;
}

void readScriptDat(const std::vector<u8>& file, SrcScript& script) {
	DatReader r {file};
	r.need(5);
	r.at = 1;
	const u32 n = r.u();
	if (n == 0 || n > (1u << 22) || u64(n) * 4 > file.size()) throw std::runtime_error("this is not a compiled script file");
	r.need(size_t(n) * 4);
	r.at += size_t(n) * 4;
	script.symbols.assign(n, SrcSymbol());
	for (u32 i = 0; i < n; ++i) {
		SrcSymbol& s = script.symbols[i];
		if (r.u() != 0) {
			s.name = r.line();
			// Names the compiler made up start with character 255.
			for (char& c : s.name) c = char(std::toupper(static_cast<unsigned char>(c)));
		}
		const u32 vary = r.u();
		const u32 props = r.u();
		s.count = props & 0xFFFu;
		const u32 type = (props >> 12) & 0xFu;
		const u32 flags = (props >> 16) & 0x3Fu;
		if (type > SCRIPT_INSTANCE) throw std::runtime_error("the symbol '" + s.name + "' has an unknown type");
		s.type = ScriptType(type);
		s.isConst = flags & DAT_CONST;
		s.hasReturn = flags & DAT_RETURN;
		s.member = flags & DAT_MEMBER;
		s.external = flags & DAT_EXTERNAL;
		if (s.member)
			s.memberOffset = vary;
		else if (s.type == SCRIPT_CLASS)
			s.classSize = vary;
		else if (s.type == SCRIPT_FUNC)
			s.returnType = vary <= SCRIPT_INSTANCE ? ScriptType(vary) : SCRIPT_VOID;
		r.need(20);
		r.at += 20;
		if (!s.member) {
			switch (s.type) {
			case SCRIPT_FLOAT:
				for (u32 k = 0; k < s.count; ++k) {
					const u32 bits = r.u();
					float f;
					std::memcpy(&f, &bits, 4);
					s.floats.push_back(f);
				}
				break;
			case SCRIPT_INT:
				for (u32 k = 0; k < s.count; ++k) s.ints.push_back(i32(r.u()));
				break;
			case SCRIPT_STRING:
				for (u32 k = 0; k < s.count; ++k) s.strings.push_back(r.line());
				break;
			case SCRIPT_CLASS:
				(void)r.u();
				break;
			case SCRIPT_INSTANCE:
			case SCRIPT_FUNC:
			case SCRIPT_PROTOTYPE:
				s.address = r.u();
				break;
			default:
				break;
			}
		}
		s.parent = i32(r.u());
	}
	const u32 codeSize = r.u();
	r.need(codeSize);
	script.code.assign(file.begin() + long(r.at), file.begin() + long(r.at + codeSize));
}

} // namespace g3
