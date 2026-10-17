#include "daedalus.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>

namespace g3 {

namespace {

enum Tok { T_END, T_ID, T_INT, T_FLOAT, T_STR, T_OP };

struct Token {
	Tok kind = T_END;
	std::string text; // identifiers in upper case
	i32 i = 0;
	float f = 0;
	int line = 0;
};

[[noreturn]] void fail(int line, const std::string& what) {
	throw std::runtime_error("line " + std::to_string(line) + ": " + what);
}

std::vector<Token> lex(const std::string& src) {
	std::vector<Token> out;
	int line = 1;
	size_t i = 0;
	const size_t n = src.size();
	while (i < n) {
		const char c = src[i];
		if (c == '\n') {
			++line;
			++i;
		} else if (std::isspace(static_cast<unsigned char>(c))) {
			++i;
		} else if (c == '/' && i + 1 < n && src[i + 1] == '/') {
			while (i < n && src[i] != '\n') ++i;
		} else if (c == '/' && i + 1 < n && src[i + 1] == '*') {
			i += 2;
			while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
				if (src[i] == '\n') ++line;
				++i;
			}
			i += 2;
		} else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
			Token t;
			t.kind = T_ID;
			t.line = line;
			// (fix195: and '@', as Ikarus' _@ / _@s have it)
			while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_' || src[i] == '^' || src[i] == '@'))
				t.text.push_back(char(std::toupper(static_cast<unsigned char>(src[i++]))));
			out.push_back(t);
		} else if (std::isdigit(static_cast<unsigned char>(c))) {
			Token t;
			t.line = line;
			size_t j = i;
			while (j < n && std::isdigit(static_cast<unsigned char>(src[j]))) ++j;
			if (j + 1 < n && src[j] == '.' && std::isdigit(static_cast<unsigned char>(src[j + 1]))) {
				++j;
				while (j < n && std::isdigit(static_cast<unsigned char>(src[j]))) ++j;
				t.kind = T_FLOAT;
				t.f = std::strtof(src.substr(i, j - i).c_str(), nullptr);
			} else {
				t.kind = T_INT;
				// Decimal whatever the leading zeros; wraps like the original's atoi.
				t.i = i32(u32(std::strtoull(src.substr(i, j - i).c_str(), nullptr, 10)));
			}
			i = j;
			out.push_back(t);
		} else if (c == '"') {
			Token t;
			t.kind = T_STR;
			t.line = line;
			++i;
			while (i < n && src[i] != '"') {
				if (src[i] == '\n') fail(line, "a string runs past the end of its line");
				t.text.push_back(src[i++]);
			}
			if (i >= n) fail(line, "a string without an end");
			++i;
			out.push_back(t);
		} else {
			Token t;
			t.kind = T_OP;
			t.line = line;
			static const char* const two[] = {"==", "!=", "<=", ">=", "<<", ">>", "&&", "||", "+=", "-=", "*=", "/="};
			t.text = std::string(1, c);
			for (const char* op : two)
				if (i + 1 < n && c == op[0] && src[i + 1] == op[1]) t.text = op;
			if (!std::strchr("+-*/%<>=!&|~(){}[];,.", c)) fail(line, std::string("unexpected character '") + c + "'");
			i += t.text.size();
			out.push_back(t);
		}
	}
	Token end;
	end.line = line;
	out.push_back(end);
	return out;
}

struct Expr {
	enum Kind { INT, FLOAT, STR, NAME, CALL, UNARY, BINARY } kind = INT;
	int line = 0;
	i32 i = 0;
	float f = 0;
	std::string text;                  // STR: the text; NAME, CALL: the name; UNARY, BINARY: the operator
	std::string object;                // NAME: the instance of "object.member"
	std::unique_ptr<Expr> index;       // NAME: of an array
	std::vector<std::unique_ptr<Expr>> args; // CALL: arguments; UNARY, BINARY: operands
};
using ExprPtr = std::unique_ptr<Expr>;

struct Body {
	int symbol = -1;   // function, prototype or instance
	size_t begin = 0;  // token after '{'
	int cls = -1;      // class whose members are in scope
	int call = -1;     // prototype to run first
};

class Compiler {
public:
	Compiler(const std::string& src, SrcScript& out) : _t(lex(src)), _s(out) {}

	void run() {
		// So that no function starts at address 0, the "address" of everything
		// that has no code.
		_s.code.push_back(OP_RSR);
		while (peek().kind != T_END) declaration();
		for (const Body& b : _bodies) body(b);
		for (const auto& [at, symbol] : _calls) {
			const SrcSymbol& f = _s.symbols[symbol];
			std::memcpy(_s.code.data() + at, &f.address, 4);
		}
	}

private:
	// ---- tokens ----
	const Token& peek(size_t ahead = 0) const { return _t[std::min(_at + ahead, _t.size() - 1)]; }
	Token next() {
		Token t = peek();
		if (_at < _t.size() - 1) ++_at;
		return t;
	}
	bool isOp(const char* op, size_t ahead = 0) const { return peek(ahead).kind == T_OP && peek(ahead).text == op; }
	bool isWord(const char* w, size_t ahead = 0) const { return peek(ahead).kind == T_ID && peek(ahead).text == w; }
	bool acceptOp(const char* op) {
		if (!isOp(op)) return false;
		next();
		return true;
	}
	void expectOp(const char* op) {
		if (!acceptOp(op)) fail(peek().line, std::string("'") + op + "' expected");
	}
	std::string ident() {
		if (peek().kind != T_ID) fail(peek().line, "a name expected");
		return next().text;
	}

	// ---- symbols ----
	int find(const std::string& name) const {
		const auto it = _byName.find(name);
		return it == _byName.end() ? -1 : it->second;
	}
	int add(const SrcSymbol& s, int line) {
		if (!s.name.empty() && _byName.count(s.name)) fail(line, "'" + s.name + "' is declared twice");
		_s.symbols.push_back(s);
		const int index = int(_s.symbols.size()) - 1;
		if (!s.name.empty()) _byName[s.name] = index;
		return index;
	}
	// A name as code in function `scope` sees it.
	int resolve(const std::string& name, int line, bool must = true) const {
		if (_scope >= 0) {
			const int local = find(_s.symbols[_scope].name + "." + name);
			if (local >= 0) return local;
		}
		if (_class >= 0) {
			const int member = find(_s.symbols[_class].name + "." + name);
			if (member >= 0) return member;
		}
		const int global = find(name);
		if (global < 0 && must) fail(line, "'" + name + "' is not declared");
		return global;
	}
	int classOf(int symbol) const {
		for (int hops = 0; symbol >= 0 && hops < 16; ++hops) {
			if (_s.symbols[symbol].type == SCRIPT_CLASS) return symbol;
			symbol = _s.symbols[symbol].parent;
		}
		return -1;
	}

	struct TypeRef {
		ScriptType type = SCRIPT_VOID;
		int cls = -1;
	};
	TypeRef typeName() {
		const int line = peek().line;
		const std::string t = ident();
		if (t == "VOID") return {SCRIPT_VOID, -1};
		if (t == "INT") return {SCRIPT_INT, -1};
		if (t == "FLOAT") return {SCRIPT_FLOAT, -1};
		if (t == "STRING") return {SCRIPT_STRING, -1};
		if (t == "FUNC") return {SCRIPT_FUNC, -1};
		const int cls = find(t);
		if (cls < 0 || _s.symbols[cls].type != SCRIPT_CLASS) fail(line, "'" + t + "' is not a type");
		return {SCRIPT_INSTANCE, cls};
	}

	// ---- constant expressions ----
	i32 constant(const Expr& e) const {
		switch (e.kind) {
		case Expr::INT:
			return e.i;
		case Expr::NAME: {
			const int sym = resolve(e.text, e.line);
			const SrcSymbol& s = _s.symbols[sym];
			if (s.type != SCRIPT_INT || !s.isConst) fail(e.line, "'" + e.text + "' is not an integer constant");
			const u32 k = e.index ? u32(constant(*e.index)) : 0u;
			if (k >= s.ints.size()) fail(e.line, "'" + e.text + "' has no such element");
			return s.ints[k];
		}
		case Expr::UNARY: {
			const u32 a = u32(constant(*e.args[0]));
			if (e.text == "-") return i32(0u - a);
			if (e.text == "!") return !a;
			if (e.text == "~") return i32(~a);
			return i32(a);
		}
		case Expr::BINARY: {
			const i32 a = constant(*e.args[0]), b = constant(*e.args[1]);
			const std::string& o = e.text;
			if (o == "+") return i32(u32(a) + u32(b));
			if (o == "-") return i32(u32(a) - u32(b));
			if (o == "*") return i32(u32(a) * u32(b));
			if (o == "/" || o == "%") {
				if (b == 0 || b == -1) fail(e.line, "a constant divided by 0 or -1");
				return o == "/" ? a / b : a % b;
			}
			if (o == "<<") return i32(u32(a) << (u32(b) & 31u));
			if (o == ">>") return a >> (u32(b) & 31u);
			if (o == "|") return a | b;
			if (o == "&") return a & b;
			if (o == "<") return a < b;
			if (o == ">") return a > b;
			if (o == "<=") return a <= b;
			if (o == ">=") return a >= b;
			if (o == "==") return a == b;
			if (o == "!=") return a != b;
			if (o == "&&") return a && b;
			if (o == "||") return a || b;
			break;
		}
		default:
			break;
		}
		fail(e.line, "not a constant expression");
	}

	// ---- expressions ----
	ExprPtr primary() {
		auto e = std::make_unique<Expr>();
		e->line = peek().line;
		if (acceptOp("(")) {
			e = expression();
			expectOp(")");
			return e;
		}
		for (const char* op : {"-", "!", "~", "+"})
			if (isOp(op)) {
				next();
				e->kind = Expr::UNARY;
				e->text = op;
				e->args.push_back(primary());
				// A negative number is a number.
				Expr& a = *e->args[0];
				if (e->text == "-" && a.kind == Expr::INT) {
					a.i = i32(0u - u32(a.i));
					return std::move(e->args[0]);
				}
				if (e->text == "-" && a.kind == Expr::FLOAT) {
					a.f = -a.f;
					return std::move(e->args[0]);
				}
				return e;
			}
		const Token t = next();
		switch (t.kind) {
		case T_INT:
			e->kind = Expr::INT;
			e->i = t.i;
			return e;
		case T_FLOAT:
			e->kind = Expr::FLOAT;
			e->f = t.f;
			return e;
		case T_STR:
			e->kind = Expr::STR;
			e->text = t.text;
			return e;
		case T_ID:
			break;
		default:
			fail(t.line, "an expression expected");
		}
		e->text = t.text;
		if (acceptOp("(")) {
			e->kind = Expr::CALL;
			if (!acceptOp(")")) {
				do e->args.push_back(expression());
				while (acceptOp(","));
				expectOp(")");
			}
			return e;
		}
		e->kind = Expr::NAME;
		if (acceptOp(".")) {
			e->object = e->text;
			e->text = ident();
		}
		if (acceptOp("[")) {
			e->index = expression();
			expectOp("]");
		}
		return e;
	}

	ExprPtr binary(int level) {
		static const std::vector<std::vector<const char*>> levels = {
		    {"||"}, {"&&"}, {"|"}, {"&"}, {"==", "!="}, {"<", "<=", ">", ">="}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"}};
		if (level == int(levels.size())) return primary();
		ExprPtr left = binary(level + 1);
		for (;;) {
			const char* found = nullptr;
			for (const char* op : levels[level])
				if (isOp(op)) found = op;
			if (!found) return left;
			auto e = std::make_unique<Expr>();
			e->kind = Expr::BINARY;
			e->line = next().line;
			e->text = found;
			e->args.push_back(std::move(left));
			e->args.push_back(binary(level + 1));
			left = std::move(e);
		}
	}
	ExprPtr expression() { return binary(0); }

	// ---- declarations ----
	u32 arraySize() {
		if (!acceptOp("[")) return 1;
		const ExprPtr e = expression();
		expectOp("]");
		const i32 n = constant(*e);
		if (n < 1 || n > 4095) fail(e->line, "an array of that size cannot be");
		return u32(n);
	}

	// "var <type> a, b[3];" - with `prefix` the names of a function's locals or
	// a class' members.
	void variables(const std::string& prefix, int memberOf, std::vector<int>* made = nullptr) {
		const TypeRef type = typeName();
		do {
			const int line = peek().line;
			SrcSymbol s;
			s.name = prefix + ident();
			s.type = type.type;
			s.count = arraySize();
			s.parent = type.cls;
			if (memberOf >= 0) {
				s.member = true;
				s.parent = memberOf;
				SrcSymbol& cls = _s.symbols[memberOf];
				s.memberOffset = cls.classSize;
				cls.classSize += s.count * (s.type == SCRIPT_STRING ? SCRIPT_STRING_SIZE : 4u);
			} else if (s.type == SCRIPT_INT) {
				s.ints.assign(s.count, 0);
			} else if (s.type == SCRIPT_FLOAT) {
				s.floats.assign(s.count, 0.0f);
			} else if (s.type == SCRIPT_STRING) {
				s.strings.assign(s.count, std::string());
			} else if (s.type == SCRIPT_FUNC) {
				s.count = 1; // a variable that holds a function
			}
			const int index = add(s, line);
			if (made) made->push_back(index);
		} while (acceptOp(","));
	}

	void constantDecl(const std::string& prefix) {
		const TypeRef type = typeName();
		const int line = peek().line;
		SrcSymbol s;
		s.name = prefix + ident();
		s.type = type.type;
		s.isConst = true;
		s.count = arraySize();
		expectOp("=");
		const bool list = acceptOp("{");
		for (u32 k = 0; k < s.count; ++k) {
			if (k) expectOp(",");
			const ExprPtr e = expression();
			if (s.type == SCRIPT_INT) {
				s.ints.push_back(constant(*e));
			} else if (s.type == SCRIPT_FLOAT) {
				if (e->kind == Expr::FLOAT)
					s.floats.push_back(e->f);
				else
					s.floats.push_back(float(constant(*e)));
			} else if (s.type == SCRIPT_STRING) {
				if (e->kind != Expr::STR) fail(line, "a text expected");
				s.strings.push_back(e->text);
			} else {
				fail(line, "a constant of that type cannot be");
			}
		}
		if (list) expectOp("}");
		add(s, line);
	}

	// Skips a "{ ... }" whose '{' has been read, declaring the locals in it.
	void skipBody(const std::string& prefix) {
		int depth = 1;
		while (depth > 0) {
			if (peek().kind == T_END) fail(peek().line, "'}' missing");
			if (isOp("{")) {
				++depth;
			} else if (isOp("}")) {
				--depth;
			} else if (isWord("VAR") && !prefix.empty()) {
				next();
				variables(prefix, -1);
				continue;
			}
			next();
		}
	}

	void declaration() {
		const int line = peek().line;
		if (acceptOp(";")) return;
		bool external = false;
		if (isWord("EXTERN")) {
			next();
			external = true;
		}
		const std::string word = ident();
		if (word == "CONST" && !external) {
			constantDecl("");
		} else if (word == "VAR" && !external) {
			variables("", -1);
		} else if (word == "CLASS" && !external) {
			SrcSymbol s;
			s.name = ident();
			s.type = SCRIPT_CLASS;
			const int cls = add(s, line);
			expectOp("{");
			while (!acceptOp("}")) {
				if (ident() != "VAR") fail(peek().line, "'var' expected in a class");
				variables(_s.symbols[cls].name + ".", cls);
				expectOp(";");
			}
			// The number of members, as the game's compiler counts.
			_s.symbols[cls].count = u32(_s.symbols.size()) - 1 - u32(cls);
		} else if (word == "FUNC") {
			const TypeRef ret = typeName();
			SrcSymbol s;
			s.name = ident();
			s.type = SCRIPT_FUNC;
			s.isConst = true;
			s.external = external;
			s.hasReturn = ret.type != SCRIPT_VOID;
			s.returnType = ret.type;
			const int fn = add(s, line);
			if (external) _s.symbols[fn].address = 0x40000000u + u32(fn);
			expectOp("(");
			const std::string prefix = _s.symbols[fn].name + ".";
			if (!acceptOp(")")) {
				do {
					if (ident() != "VAR") fail(peek().line, "'var' expected before a parameter");
					const TypeRef type = typeName();
					SrcSymbol p;
					p.name = prefix + ident();
					p.type = type.type;
					p.count = 1;
					p.parent = type.cls;
					if (external) {
						// The game's own compiler gives the parameters of an engine
						// function a place in the table, but no room for a value.
						p.count = 0;
					} else {
						if (p.type == SCRIPT_INT) p.ints.assign(1, 0);
						if (p.type == SCRIPT_FLOAT) p.floats.assign(1, 0.0f);
						if (p.type == SCRIPT_STRING) p.strings.assign(1, std::string());
					}
					add(p, line);
					++_s.symbols[fn].count;
				} while (acceptOp(","));
				expectOp(")");
			}
			if (external) return;
			expectOp("{");
			_bodies.push_back({fn, _at, -1, -1});
			skipBody(prefix);
		} else if ((word == "PROTOTYPE" || word == "INSTANCE") && !external) {
			std::vector<std::string> names;
			do names.push_back(ident());
			while (acceptOp(","));
			expectOp("(");
			const std::string parentName = ident();
			expectOp(")");
			const int parent = find(parentName);
			if (parent < 0 || (_s.symbols[parent].type != SCRIPT_CLASS && _s.symbols[parent].type != SCRIPT_PROTOTYPE))
				fail(line, "'" + parentName + "' is neither a class nor a prototype");
			const bool hasBody = isOp("{");
			for (const std::string& name : names) {
				SrcSymbol s;
				s.name = name;
				s.type = word == "PROTOTYPE" ? SCRIPT_PROTOTYPE : SCRIPT_INSTANCE;
				s.parent = parent;
				s.isConst = hasBody;
				s.count = 0;
				const int sym = add(s, line);
				if (hasBody && name == names.back()) {
					next();
					_bodies.push_back({sym, _at, classOf(parent), _s.symbols[parent].type == SCRIPT_PROTOTYPE ? parent : -1});
					skipBody("");
				} else if (hasBody) {
					fail(line, "several instances cannot share one body here");
				}
			}
		} else {
			fail(line, "'" + word + "' cannot start a declaration");
		}
		acceptOp(";");
	}

	// ---- code ----
	void emit(u8 op) { _s.code.push_back(op); }
	void emit(u8 op, u32 operand) {
		_s.code.push_back(op);
		const u8* p = reinterpret_cast<const u8*>(&operand);
		_s.code.insert(_s.code.end(), p, p + 4);
	}
	u32 here() const { return u32(_s.code.size()); }
	void patch(u32 at, u32 value) { std::memcpy(_s.code.data() + at, &value, 4); }

	int literal(const std::string& text) {
		SrcSymbol s;
		s.name = "\xFF" + std::to_string(10000 + _literals++);
		s.type = SCRIPT_STRING;
		s.isConst = true;
		s.count = 1;
		s.strings.push_back(text);
		return add(s, 0);
	}

	// Pushes the variable a name stands for.
	void pushVariable(const Expr& e, int sym) {
		const SrcSymbol& s = _s.symbols[sym];
		if (!e.object.empty()) {
			const int object = resolve(e.object, e.line);
			if (_s.symbols[object].type != SCRIPT_INSTANCE) fail(e.line, "'" + e.object + "' is not an instance");
			emit(OP_GMOVI, u32(object));
		}
		const u32 index = e.index ? u32(constant(*e.index)) : 0u;
		if (index >= std::max<u32>(s.count, 1)) fail(e.line, "'" + e.text + "' has no element " + std::to_string(index));
		if (index > 255) fail(e.line, "an index beyond 255");
		if (index) {
			emit(OP_PUSHVV, u32(sym));
			_s.code.push_back(u8(index));
		} else {
			emit(s.type == SCRIPT_INSTANCE ? OP_PUSHVI : OP_PUSHV, u32(sym));
		}
	}

	int variableOf(const Expr& e) const {
		if (e.object.empty()) return resolve(e.text, e.line);
		const int object = resolve(e.object, e.line);
		const int cls = classOf(object);
		if (cls < 0) fail(e.line, "'" + e.object + "' has no members");
		const int member = find(_s.symbols[cls].name + "." + e.text);
		if (member < 0) fail(e.line, "'" + e.object + "' has no member '" + e.text + "'");
		return member;
	}

	ScriptType call(const Expr& e) {
		const int fn = resolve(e.text, e.line);
		const SrcSymbol f = _s.symbols[fn];
		if (f.type != SCRIPT_FUNC || !f.isConst) fail(e.line, "'" + e.text + "' is not a function");
		if (e.args.size() != f.count)
			fail(e.line, "'" + e.text + "' takes " + std::to_string(f.count) + " arguments, not " + std::to_string(e.args.size()));
		for (u32 k = 0; k < f.count; ++k) value(*e.args[k], _s.symbols[fn + 1 + k].type);
		if (f.external) {
			emit(OP_BE, u32(fn));
		} else {
			emit(OP_BL, 0);
			_calls.push_back({here() - 4, fn});
		}
		return f.hasReturn ? f.returnType : SCRIPT_VOID;
	}

	// Code that leaves a value of type `want` on the stack.
	void value(const Expr& e, ScriptType want) {
		switch (e.kind) {
		case Expr::INT:
			if (want == SCRIPT_FLOAT) {
				const float f = float(e.i);
				u32 bits;
				std::memcpy(&bits, &f, 4);
				emit(OP_PUSHI, bits);
			} else if (want == SCRIPT_INT || want == SCRIPT_FUNC) {
				emit(OP_PUSHI, u32(e.i));
			} else {
				fail(e.line, "a number where something else is expected");
			}
			return;
		case Expr::FLOAT: {
			if (want != SCRIPT_FLOAT) fail(e.line, "a fraction where something else is expected");
			u32 bits;
			std::memcpy(&bits, &e.f, 4);
			emit(OP_PUSHI, bits);
			return;
		}
		case Expr::STR:
			if (want != SCRIPT_STRING) fail(e.line, "a text where something else is expected");
			emit(OP_PUSHV, u32(literal(e.text)));
			return;
		case Expr::CALL: {
			const ScriptType got = call(e);
			if (got != want && !(got == SCRIPT_FUNC && want == SCRIPT_INT))
				fail(e.line, "'" + e.text + "' does not return what is expected here");
			return;
		}
		case Expr::NAME: {
			const int sym = variableOf(e);
			const SrcSymbol& s = _s.symbols[sym];
			// The name of a function, prototype or instance is its number.
			const bool isNumber = s.type == SCRIPT_PROTOTYPE || (s.type == SCRIPT_FUNC && s.isConst && !s.member) ||
			                      (s.type == SCRIPT_INSTANCE && want != SCRIPT_INSTANCE);
			if (isNumber) {
				if (want != SCRIPT_INT && want != SCRIPT_FUNC) fail(e.line, "'" + e.text + "' cannot be used here");
				emit(OP_PUSHI, u32(sym));
				return;
			}
			const ScriptType has = s.type == SCRIPT_FUNC ? SCRIPT_INT : s.type;
			const ScriptType wanted = want == SCRIPT_FUNC ? SCRIPT_INT : want;
			if (has != wanted) fail(e.line, "'" + e.text + "' is not of the type expected here");
			pushVariable(e, sym);
			return;
		}
		case Expr::UNARY:
			if (want != SCRIPT_INT) fail(e.line, "arithmetic where something else is expected");
			value(*e.args[0], SCRIPT_INT);
			emit(e.text == "-" ? OP_NEGATE : e.text == "!" ? OP_NOT : e.text == "~" ? OP_CMPL : OP_PLUS);
			return;
		case Expr::BINARY: {
			if (want != SCRIPT_INT) fail(e.line, "arithmetic where something else is expected");
			// The right operand first: the left one is taken off the stack first.
			value(*e.args[1], SCRIPT_INT);
			value(*e.args[0], SCRIPT_INT);
			static const std::map<std::string, u8> ops = {
			    {"+", OP_ADD},  {"-", OP_SUB},  {"*", OP_MUL}, {"/", OP_DIV}, {"%", OP_MOD},  {"|", OP_OR},
			    {"&", OP_ANDB}, {"<", OP_LT},   {">", OP_GT},  {"<=", OP_LTE}, {">=", OP_GTE}, {"==", OP_EQ},
			    {"!=", OP_NEQ}, {"<<", OP_LSL}, {">>", OP_LSR}, {"&&", OP_AND}, {"||", OP_ORR}};
			emit(ops.at(e.text));
			return;
		}
		}
	}

	void block() {
		while (!acceptOp("}")) statement();
	}

	void statement() {
		const int line = peek().line;
		if (acceptOp(";")) return;
		if (isWord("VAR")) {
			// Declared when the body was skipped.
			while (!isOp(";") && peek().kind != T_END) next();
			expectOp(";");
			return;
		}
		if (isWord("IF")) {
			next();
			std::vector<u32> ends;
			for (;;) {
				const ExprPtr cond = expression();
				value(*cond, SCRIPT_INT);
				emit(OP_BZ, 0);
				const u32 skip = here() - 4;
				expectOp("{");
				block();
				if (isWord("ELSE")) {
					next();
					emit(OP_B, 0);
					ends.push_back(here() - 4);
					patch(skip, here());
					if (isWord("IF")) {
						next();
						continue;
					}
					expectOp("{");
					block();
				} else {
					patch(skip, here());
				}
				break;
			}
			for (u32 at : ends) patch(at, here());
			acceptOp(";");
			return;
		}
		if (isWord("RETURN")) {
			next();
			const SrcSymbol& f = _s.symbols[_scope];
			if (!isOp(";")) {
				if (!f.hasReturn) fail(line, "a value returned from a function that has none");
				const ExprPtr e = expression();
				value(*e, f.returnType);
			}
			emit(OP_RSR);
			expectOp(";");
			return;
		}
		const ExprPtr target = primary();
		if (target->kind == Expr::CALL) {
			call(*target); // a result, if any, stays on the stack: the machine tidies up
			expectOp(";");
			return;
		}
		if (target->kind != Expr::NAME) fail(line, "a statement expected");
		if (acceptOp(";")) {
			// A name on its own (fix195): its value is pushed and left there, as the game's compiler does - Ikarus'
			// `end;`, `break;`, `continue;` and its "push a value for the next call" are this.
			const int sym = variableOf(*target);
			const SrcSymbol& s = _s.symbols[sym];
			if (s.type == SCRIPT_FUNC || s.type == SCRIPT_PROTOTYPE || (s.type == SCRIPT_INSTANCE && s.isConst && target->object.empty()))
				emit(OP_PUSHI, u32(sym));
			else
				pushVariable(*target, sym);
			return;
		}
		const Token op = next();
		if (op.kind != T_OP) fail(line, "an assignment expected");
		const int sym = variableOf(*target);
		const SrcSymbol s = _s.symbols[sym];
		const ExprPtr e = expression();
		if (op.text == "=") {
			value(*e, s.type);
			pushVariable(*target, sym);
			switch (s.type) {
			case SCRIPT_INT: emit(OP_MOVI); break;
			case SCRIPT_FLOAT: emit(OP_MOVF); break;
			case SCRIPT_STRING: emit(OP_MOVS); break;
			case SCRIPT_FUNC: emit(OP_MOVVF); break;
			case SCRIPT_INSTANCE: emit(OP_MOVVI); break;
			default: fail(line, "'" + target->text + "' cannot be assigned to");
			}
		} else if (op.text == "+=" || op.text == "-=" || op.text == "*=" || op.text == "/=") {
			if (s.type != SCRIPT_INT) fail(line, "'" + op.text + "' needs an integer");
			value(*e, SCRIPT_INT);
			pushVariable(*target, sym);
			emit(op.text == "+=" ? OP_ADDMOVI : op.text == "-=" ? OP_SUBMOVI : op.text == "*=" ? OP_MULMOVI : OP_DIVMOVI);
		} else {
			fail(line, "an assignment expected");
		}
		expectOp(";");
	}

	void body(const Body& b) {
		_at = b.begin;
		_scope = b.symbol;
		_class = b.cls;
		SrcSymbol& s = _s.symbols[b.symbol];
		s.address = here();
		if (s.type == SCRIPT_FUNC) {
			// Arguments arrive on the stack, the last one on top.
			for (u32 k = s.count; k > 0; --k) {
				const int p = b.symbol + int(k);
				switch (_s.symbols[p].type) {
				case SCRIPT_INT: emit(OP_PUSHV, u32(p)), emit(OP_MOVI); break;
				case SCRIPT_FLOAT: emit(OP_PUSHV, u32(p)), emit(OP_MOVF); break;
				case SCRIPT_STRING: emit(OP_PUSHV, u32(p)), emit(OP_MOVS); break;
				case SCRIPT_FUNC: emit(OP_PUSHV, u32(p)), emit(OP_MOVVF); break;
				default: emit(OP_PUSHVI, u32(p)), emit(OP_MOVVI); break;
				}
			}
		} else if (b.call >= 0) {
			emit(OP_BL, 0);
			_calls.push_back({here() - 4, b.call});
		}
		block();
		emit(OP_RSR);
		_scope = -1;
		_class = -1;
	}

	std::vector<Token> _t;
	size_t _at = 0;
	SrcScript& _s;
	std::map<std::string, int> _byName;
	std::vector<Body> _bodies;
	std::vector<std::pair<u32, int>> _calls; // operand position, function
	int _scope = -1, _class = -1;
	int _literals = 0;
};

} // namespace

void compileDaedalus(const std::string& source, SrcScript& out) {
	out.symbols.clear();
	out.code.clear();
	if (out.source.empty()) out.source = "built-in script";
	Compiler(source, out).run();
}

} // namespace g3
