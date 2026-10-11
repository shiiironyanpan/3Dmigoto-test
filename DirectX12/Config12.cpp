// 3DMigoto DX12 - d3dx.ini model implementation (portable)
#include "Config12.h"
#include "Dump12.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace mig12 {

void (*g_log_cb)(const char *) = nullptr;

void mlog(const char *fmt, ...)
{
	if (!g_log_cb)
		return;
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_log_cb(buf);
}

// =============================================================================
// Expressions
// =============================================================================

enum {
	BI_TIME = 1, BI_RT_W, BI_RT_H, BI_WIN_W, BI_WIN_H, BI_HUNTING, BI_ANALYSE,
	BI_VERTEX_COUNT, BI_INDEX_COUNT, BI_INSTANCE_COUNT, BI_FIRST_VERTEX, BI_FIRST_INDEX,
	BI_FIRST_INSTANCE, BI_CURSOR_SHOWING
};

double Expr::Eval(const EvalEnv &env) const
{
	switch (kind) {
	case CONSTANT:
		return c;
	case VAR:
		return var ? var->v : 0;
	case BUILTIN: {
		const DrawCtx *d = env.draw;
		switch (builtin) {
		case BI_TIME: return env.time;
		case BI_RT_W: return env.rt_width;
		case BI_RT_H: return env.rt_height;
		case BI_WIN_W: return env.window_width;
		case BI_WIN_H: return env.window_height;
		case BI_HUNTING: return env.hunting;
		case BI_ANALYSE: return env.analyse_frame ? 1 : 0;
		case BI_VERTEX_COUNT: return d ? d->vertex_count : 0;
		case BI_INDEX_COUNT: return d ? d->index_count : 0;
		case BI_INSTANCE_COUNT: return d ? d->instance_count : 0;
		case BI_FIRST_VERTEX: return d ? d->first_vertex : 0;
		case BI_FIRST_INDEX: return d ? d->first_index : 0;
		case BI_FIRST_INSTANCE: return d ? d->first_instance : 0;
		case BI_CURSOR_SHOWING: return 0;
		}
		return 0;
	}
	case UNARY: {
		double x = a->Eval(env);
		return op == NOT ? (x == 0 ? 1 : 0) : -x;
	}
	case BINARY: {
		double x = a->Eval(env);
		if (op == AND)
			return (x != 0 && b->Eval(env) != 0) ? 1 : 0;
		if (op == OR)
			return (x != 0 || b->Eval(env) != 0) ? 1 : 0;
		double y = b->Eval(env);
		switch (op) {
		case ADD: return x + y;
		case SUB: return x - y;
		case MUL: return x * y;
		case DIV: return y != 0 ? x / y : 0;
		case IDIV: return y != 0 ? (double)(long long)(x / y) : 0;
		case MOD: return y != 0 ? x - y * (double)(long long)(x / y) : 0;
		case LT: return x < y;
		case LE: return x <= y;
		case GT: return x > y;
		case GE: return x >= y;
		case EQ: return x == y;
		case NE: return x != y;
		default: return 0;
		}
	}
	}
	return 0;
}

namespace {

struct ExprParser {
	const std::string &s;
	const VarResolver &res;
	size_t i = 0;
	std::string err;

	ExprParser(const std::string &str, const VarResolver &r) : s(str), res(r) {}

	void ws()
	{
		while (i < s.size() && isspace((unsigned char)s[i]))
			i++;
	}
	bool at(const char *tok)
	{
		ws();
		return s.compare(i, strlen(tok), tok) == 0;
	}
	static std::unique_ptr<Expr> bin(Expr::Op op, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r)
	{
		if (!l || !r)
			return nullptr;
		std::unique_ptr<Expr> e(new Expr());
		e->kind = Expr::BINARY;
		e->op = op;
		e->a = std::move(l);
		e->b = std::move(r);
		return e;
	}

	std::unique_ptr<Expr> parseOr()
	{
		auto l = parseAnd();
		while (l && at("||")) {
			i += 2;
			l = bin(Expr::OR, std::move(l), parseAnd());
		}
		return l;
	}
	std::unique_ptr<Expr> parseAnd()
	{
		auto l = parseCmp();
		while (l && at("&&")) {
			i += 2;
			l = bin(Expr::AND, std::move(l), parseCmp());
		}
		return l;
	}
	std::unique_ptr<Expr> parseCmp()
	{
		auto l = parseAdd();
		while (l) {
			Expr::Op op;
			size_t n = 2;
			if (at("=="))
				op = Expr::EQ;
			else if (at("!="))
				op = Expr::NE;
			else if (at("<="))
				op = Expr::LE;
			else if (at(">="))
				op = Expr::GE;
			else if (at("<")) {
				op = Expr::LT;
				n = 1;
			} else if (at(">")) {
				op = Expr::GT;
				n = 1;
			} else
				break;
			i += n;
			l = bin(op, std::move(l), parseAdd());
		}
		return l;
	}
	std::unique_ptr<Expr> parseAdd()
	{
		auto l = parseMul();
		while (l) {
			if (at("+")) {
				i++;
				l = bin(Expr::ADD, std::move(l), parseMul());
			} else if (at("-")) {
				i++;
				l = bin(Expr::SUB, std::move(l), parseMul());
			} else
				break;
		}
		return l;
	}
	std::unique_ptr<Expr> parseMul()
	{
		auto l = parseUnary();
		while (l) {
			if (at("//")) {
				i += 2;
				l = bin(Expr::IDIV, std::move(l), parseUnary());
			} else if (at("*")) {
				i++;
				l = bin(Expr::MUL, std::move(l), parseUnary());
			} else if (at("/")) {
				i++;
				l = bin(Expr::DIV, std::move(l), parseUnary());
			} else if (at("%")) {
				i++;
				l = bin(Expr::MOD, std::move(l), parseUnary());
			} else
				break;
		}
		return l;
	}
	std::unique_ptr<Expr> parseUnary()
	{
		ws();
		if (at("!") && !at("!=")) {
			i++;
			auto x = parseUnary();
			if (!x)
				return nullptr;
			std::unique_ptr<Expr> e(new Expr());
			e->kind = Expr::UNARY;
			e->op = Expr::NOT;
			e->a = std::move(x);
			return e;
		}
		if (at("-")) {
			i++;
			auto x = parseUnary();
			if (!x)
				return nullptr;
			std::unique_ptr<Expr> e(new Expr());
			e->kind = Expr::UNARY;
			e->op = Expr::NEG;
			e->a = std::move(x);
			return e;
		}
		if (at("+")) {
			i++;
			return parseUnary();
		}
		return parsePrimary();
	}
	std::unique_ptr<Expr> parsePrimary()
	{
		ws();
		if (i >= s.size()) {
			err = "unexpected end of expression";
			return nullptr;
		}
		if (s[i] == '(') {
			i++;
			auto e = parseOr();
			ws();
			if (!e || i >= s.size() || s[i] != ')') {
				err = "missing )";
				return nullptr;
			}
			i++;
			return e;
		}
		if (s[i] == '$') {
			size_t b = ++i;
			while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '\\' || s[i] == '.'))
				i++;
			// don't swallow a trailing '.' that isn't part of the name
			while (i > b && s[i - 1] == '.')
				i--;
			std::string name = s.substr(b, i - b);
			Var *v = res(name);
			if (!v) {
				err = "unknown variable $" + name;
				return nullptr;
			}
			std::unique_ptr<Expr> e(new Expr());
			e->kind = Expr::VAR;
			e->var = v;
			return e;
		}
		if (isdigit((unsigned char)s[i]) || s[i] == '.') {
			char *end = nullptr;
			double v = strtod(s.c_str() + i, &end);
			if (end == s.c_str() + i) {
				err = "bad number";
				return nullptr;
			}
			i = end - s.c_str();
			std::unique_ptr<Expr> e(new Expr());
			e->kind = Expr::CONSTANT;
			e->c = v;
			return e;
		}
		if (isalpha((unsigned char)s[i]) || s[i] == '_') {
			size_t b = i;
			while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_'))
				i++;
			std::string id = lower(s.substr(b, i - b));
			std::unique_ptr<Expr> e(new Expr());
			if (id == "true" || id == "false") {
				e->kind = Expr::CONSTANT;
				e->c = id == "true" ? 1 : 0;
				return e;
			}
			static const std::map<std::string, int> names = {
				{ "time", BI_TIME }, { "rt_width", BI_RT_W }, { "rt_height", BI_RT_H },
				{ "window_width", BI_WIN_W }, { "window_height", BI_WIN_H },
				{ "hunting", BI_HUNTING }, { "analyse_frame", BI_ANALYSE },
				{ "vertex_count", BI_VERTEX_COUNT }, { "index_count", BI_INDEX_COUNT },
				{ "instance_count", BI_INSTANCE_COUNT }, { "first_vertex", BI_FIRST_VERTEX },
				{ "first_index", BI_FIRST_INDEX }, { "first_instance", BI_FIRST_INSTANCE },
				{ "cursor_showing", BI_CURSOR_SHOWING },
			};
			auto it = names.find(id);
			if (it == names.end()) {
				err = "unknown identifier '" + id + "'";
				return nullptr;
			}
			e->kind = Expr::BUILTIN;
			e->builtin = it->second;
			return e;
		}
		err = std::string("unexpected '") + s[i] + "'";
		return nullptr;
	}
};

} // namespace

std::unique_ptr<Expr> ParseExpr(const std::string &text, const VarResolver &resolve, std::string *err)
{
	ExprParser p(text, resolve);
	auto e = p.parseOr();
	p.ws();
	if (e && p.i < text.size()) {
		p.err = "trailing characters: " + text.substr(p.i);
		e.reset();
	}
	if (!e && err)
		*err = p.err.empty() ? "parse error" : p.err;
	return e;
}

// =============================================================================
// Fuzzy matching, overrides
// =============================================================================

bool FuzzyMatch::matches(uint32_t v) const
{
	switch (op) {
	case ALWAYS: return true;
	case EQ: return v == val;
	case NE: return v != val;
	case LT: return v < val;
	case LE: return v <= val;
	case GT: return v > val;
	case GE: return v >= val;
	}
	return true;
}

bool FuzzyMatch::parse(const std::string &in)
{
	std::string s = trim(in);
	if (s.empty() || s == "*") {
		op = ALWAYS;
		return true;
	}
	size_t n = 0;
	if (s.compare(0, 2, "==") == 0) { op = EQ; n = 2; }
	else if (s.compare(0, 2, "!=") == 0) { op = NE; n = 2; }
	else if (s.compare(0, 2, "<=") == 0) { op = LE; n = 2; }
	else if (s.compare(0, 2, ">=") == 0) { op = GE; n = 2; }
	else if (s[0] == '<') { op = LT; n = 1; }
	else if (s[0] == '>') { op = GT; n = 1; }
	else { op = EQ; n = 0; }
	char *end = nullptr;
	std::string num = trim(s.substr(n));
	val = (uint32_t)strtoul(num.c_str(), &end, 0);
	return end && end != num.c_str();
}

bool TextureOverride::matches_draw(const DrawCtx &d) const
{
	if (!has_draw_match)
		return true;
	return m_first_vertex.matches(d.first_vertex) && m_first_index.matches(d.first_index) &&
	       m_first_instance.matches(d.first_instance) && m_vertex_count.matches(d.vertex_count) &&
	       m_index_count.matches(d.index_count) && m_instance_count.matches(d.instance_count);
}

// =============================================================================
// Custom resources
// =============================================================================

bool CustomResource::Load()
{
	if (loaded)
		return !load_failed;
	loaded = true;
	if (filename.empty())
		return true; // inline data (or nothing)
	std::string f = filename;
	for (auto &c : f)
		if (c == '\\')
			c = '/';
	std::string path = dir.empty() ? f : (dir + "/" + f);
	std::ifstream in(fs::u8path(path), std::ios::binary);
	if (!in) {
		mlog("  ERROR: Unable to open resource file %s (%s)\n", filename.c_str(), path.c_str());
		load_failed = true;
		return false;
	}
	data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	mlog("  Loaded %s: %u bytes\n", filename.c_str(), (unsigned)data.size());
	return true;
}

// =============================================================================
// Commands
// =============================================================================

void CommandList::Run(ExecState &st) const
{
	if (st.depth > 32)
		return;
	st.depth++;
	int mask = st.post ? PHASE_POST : PHASE_PRE;
	for (auto &c : cmds)
		if (c->phase & mask)
			c->Run(st);
	st.depth--;
}

namespace {

struct AssignCmd : Command {
	Var *v = nullptr;
	std::unique_ptr<Expr> e;
	void Run(ExecState &st) override { v->v = e->Eval(st.env); }
};

struct IfCmd : Command {
	struct Branch {
		std::unique_ptr<Expr> cond; // null for else
		std::vector<CmdPtr> body;
	};
	std::vector<Branch> branches;
	void Run(ExecState &st) override
	{
		int mask = st.post ? PHASE_POST : PHASE_PRE;
		for (auto &b : branches) {
			if (!b.cond || b.cond->Eval(st.env) != 0) {
				for (auto &c : b.body)
					if (c->phase & mask)
						c->Run(st);
				return;
			}
		}
	}
};

struct RunCmd : Command {
	CommandList *target = nullptr;
	void Run(ExecState &st) override
	{
		if (target)
			target->Run(st);
	}
};

struct SkipCmd : Command {
	void Run(ExecState &st) override { st.skip_original = true; }
};

struct SetIBCmd : Command {
	CustomResource *res = nullptr;
	void Run(ExecState &st) override
	{
		if (st.engine)
			st.engine->SetIndexBuffer(res ? res->Resolve() : nullptr);
	}
};

struct SetVBCmd : Command {
	int slot = 0;
	CustomResource *res = nullptr;
	void Run(ExecState &st) override
	{
		if (st.engine)
			st.engine->SetVertexBuffer(slot, res ? res->Resolve() : nullptr);
	}
};

struct AliasCmd : Command {
	CustomResource *dst = nullptr, *src = nullptr;
	void Run(ExecState &) override { dst->alias = src; }
};

struct CheckTOCmd : Command {
	std::string target;
	void Run(ExecState &st) override
	{
		if (st.check_texture_override)
			st.check_texture_override(st, target);
	}
};

struct NoopCmd : Command {
	void Run(ExecState &) override {}
};

struct DrawCmd : Command {
	enum Kind { DRAW, DRAWINDEXED, DRAWINSTANCED, DRAWINDEXEDINSTANCED } kind = DRAW;
	bool from_caller = false;
	std::vector<std::unique_ptr<Expr>> args;
	void Run(ExecState &st) override
	{
		if (!st.engine)
			return;
		if (from_caller) {
			st.engine->DrawFromCaller();
			return;
		}
		auto u = [&](size_t i) { return (uint32_t)(long long)args[i]->Eval(st.env); };
		auto s = [&](size_t i) { return (int)(long long)args[i]->Eval(st.env); };
		switch (kind) {
		case DRAW: st.engine->Draw(u(0), u(1)); break;
		case DRAWINDEXED: st.engine->DrawIndexed(u(0), u(1), s(2)); break;
		case DRAWINSTANCED: st.engine->DrawInstanced(u(0), u(1), u(2), u(3)); break;
		case DRAWINDEXEDINSTANCED: st.engine->DrawIndexedInstanced(u(0), u(1), u(2), s(3), u(4)); break;
		}
	}
};

std::vector<std::string> split(const std::string &s, char sep)
{
	std::vector<std::string> out;
	std::string cur;
	for (char c : s) {
		if (c == sep) {
			out.push_back(trim(cur));
			cur.clear();
		} else
			cur += c;
	}
	out.push_back(trim(cur));
	return out;
}

std::vector<std::string> split_ws(const std::string &s)
{
	std::istringstream in(s);
	std::vector<std::string> out;
	std::string t;
	while (in >> t)
		out.push_back(t);
	return out;
}

bool starts_with_word(const std::string &lraw, const char *w)
{
	size_t n = strlen(w);
	if (lraw.compare(0, n, w) != 0)
		return false;
	return lraw.size() == n || isspace((unsigned char)lraw[n]) || lraw[n] == '(';
}

} // namespace

// =============================================================================
// Keys
// =============================================================================

int parse_vk(const std::string &in)
{
	std::string n = lower(trim(in));
	if (n.compare(0, 3, "vk_") == 0)
		n = n.substr(3);
	if (n.empty())
		return -1;
	if (n.size() == 1) {
		char c = n[0];
		if (c >= 'a' && c <= 'z')
			return 'A' + (c - 'a');
		if (c >= '0' && c <= '9')
			return c;
	}
	if (n.compare(0, 2, "0x") == 0)
		return (int)strtol(n.c_str(), nullptr, 16);
	if (n[0] == 'f' && n.size() <= 3 && isdigit((unsigned char)n[1])) {
		int f = atoi(n.c_str() + 1);
		if (f >= 1 && f <= 24)
			return 0x70 + f - 1;
	}
	if (n.compare(0, 6, "numpad") == 0 && n.size() == 7 && isdigit((unsigned char)n[6]))
		return 0x60 + (n[6] - '0');
	static const std::map<std::string, int> m = {
		{ "lbutton", 1 }, { "rbutton", 2 }, { "mbutton", 4 }, { "xbutton1", 5 }, { "xbutton2", 6 },
		{ "back", 8 }, { "backspace", 8 }, { "tab", 9 }, { "return", 13 }, { "enter", 13 },
		{ "shift", 0x10 }, { "control", 0x11 }, { "ctrl", 0x11 }, { "menu", 0x12 }, { "alt", 0x12 },
		{ "pause", 0x13 }, { "capital", 0x14 }, { "caps", 0x14 }, { "capslock", 0x14 },
		{ "escape", 0x1b }, { "esc", 0x1b }, { "space", 0x20 },
		{ "prior", 0x21 }, { "pgup", 0x21 }, { "pageup", 0x21 }, { "next", 0x22 }, { "pgdn", 0x22 },
		{ "pagedown", 0x22 }, { "end", 0x23 }, { "home", 0x24 }, { "left", 0x25 }, { "up", 0x26 },
		{ "right", 0x27 }, { "down", 0x28 }, { "snapshot", 0x2c }, { "insert", 0x2d }, { "delete", 0x2e },
		{ "multiply", 0x6a }, { "add", 0x6b }, { "subtract", 0x6d }, { "decimal", 0x6e }, { "divide", 0x6f },
		{ "numlock", 0x90 }, { "scroll", 0x91 }, { "lshift", 0xa0 }, { "rshift", 0xa1 },
		{ "lcontrol", 0xa2 }, { "rcontrol", 0xa3 }, { "lmenu", 0xa4 }, { "rmenu", 0xa5 },
		{ "oem_1", 0xba }, { "oem_plus", 0xbb }, { "oem_comma", 0xbc }, { "oem_minus", 0xbd },
		{ "oem_period", 0xbe }, { "oem_2", 0xbf }, { "oem_3", 0xc0 }, { "oem_4", 0xdb },
		{ "oem_5", 0xdc }, { "oem_6", 0xdd }, { "oem_7", 0xde },
	};
	auto it = m.find(n);
	return it == m.end() ? -1 : it->second;
}

bool parse_key_combo(const std::string &text, KeyCombo *out)
{
	KeyCombo k;
	k.text = text;
	for (auto &tok : split_ws(text)) {
		std::string l = lower(tok);
		if (l == "no_modifiers") {
			k.no_modifiers = true;
			continue;
		}
		bool neg = false;
		if (l.compare(0, 3, "no_") == 0) {
			neg = true;
			l = l.substr(3);
		}
		int vk = parse_vk(l);
		if (vk < 0)
			return false;
		KeyTok t;
		t.vk = vk;
		t.negate = neg;
		k.toks.push_back(t);
	}
	k.valid = !k.toks.empty();
	*out = k;
	return k.valid;
}

bool KeyCombo::Matches(const KeyDownFn &down) const
{
	if (!valid || toks.empty())
		return false;
	bool want_pos[3] = { false, false, false };
	for (auto &t : toks) {
		bool d = down(t.vk);
		if (t.negate ? d : !d)
			return false;
		if (!t.negate) {
			if (t.vk == 0x10 || t.vk == 0xa0 || t.vk == 0xa1) want_pos[0] = true;
			if (t.vk == 0x11 || t.vk == 0xa2 || t.vk == 0xa3) want_pos[1] = true;
			if (t.vk == 0x12 || t.vk == 0xa4 || t.vk == 0xa5) want_pos[2] = true;
		}
	}
	if (no_modifiers) {
		static const int mods[3] = { 0x10, 0x11, 0x12 };
		for (int i = 0; i < 3; i++)
			if (!want_pos[i] && down(mods[i]))
				return false;
	}
	return true;
}

bool KeyCombo::Pressed(const KeyDownFn &down)
{
	bool now = Matches(down);
	bool fired = now && !prev;
	prev = now;
	return fired;
}

// =============================================================================
// Config
// =============================================================================

void Config::warn(const std::string &s)
{
	warnings.push_back(s);
	mlog("WARNING: %s\n", s.c_str());
}

std::string Config::ns_prefix_key(const std::string &lname, const std::string &ns) const
{
	return ns.empty() ? lname : (lname + "\x01" + ns);
}

Var *Config::GetVar(const std::string &key, bool create)
{
	auto it = vars.find(key);
	if (it != vars.end())
		return it->second.get();
	if (!create)
		return nullptr;
	std::unique_ptr<Var> v(new Var());
	v->name = key;
	Var *p = v.get();
	vars[key] = std::move(v);
	return p;
}

Var *Config::ResolveVar(const std::string &name, const std::string &ns, bool create)
{
	std::string n = lower(name);
	if (!n.empty() && n[0] == '\\')
		return GetVar(n, create);
	if (!ns.empty()) {
		Var *v = GetVar("\\" + ns + "\\" + n, false);
		if (v)
			return v;
	}
	Var *v = GetVar(n, false);
	if (v)
		return v;
	if (!create)
		return nullptr;
	return GetVar(ns.empty() ? n : ("\\" + ns + "\\" + n), true);
}

CommandList *Config::FindCL(const std::string &ref, const std::string &ns)
{
	std::string l = lower(trim(ref));
	if (!ns.empty()) {
		auto it = lk_.cl.find(l + "\x01" + ns);
		if (it != lk_.cl.end())
			return it->second;
	}
	auto it = lk_.cl.find(l);
	return it == lk_.cl.end() ? nullptr : it->second;
}

CustomResource *Config::FindRes(const std::string &ref, const std::string &ns)
{
	std::string l = lower(trim(ref));
	if (!ns.empty()) {
		auto it = lk_.res.find(l + "\x01" + ns);
		if (it != lk_.res.end())
			return it->second;
	}
	auto it = lk_.res.find(l);
	return it == lk_.res.end() ? nullptr : it->second;
}

static const char *kSettingKeys[] = {
	"hash", "match_first_vertex", "match_first_index", "match_first_instance", "match_vertex_count",
	"match_index_count", "match_instance_count", "match_priority", "override_vertex_count",
	"override_byte_stride", "filter_index", "expand_region_copy", "deny_cpu_read", "width", "height",
	"format", "width_multiply", "height_multiply", "iterations", "stereomode", "allow_duplicate_hash",
	"partner", "model", "depth_filter", "disable_scissor", "fake_o0", "type", "key", "back", "delay",
	"release_transition", "release_delay", "transition", "release_transition_type", "transition_type",
	"smart", "wrap", "convergence", "separation", "match_type", "match_usage", "match_bind_flags",
	"match_cpu_access_flags", "match_misc_flags", "match_byte_width", "match_stride", "match_width",
	"match_height", "match_depth", "match_mips", "match_array", "match_msaa", "match_msaa_quality",
	"match_format", nullptr
};

static bool is_setting_key(const std::string &lk)
{
	for (int i = 0; kSettingKeys[i]; i++)
		if (lk == kSettingKeys[i])
			return true;
	return false;
}

void Config::ParseCommands(CommandList &cl, const IniSection &sec)
{
	const std::string &ns = sec.ns;
	VarResolver resolver = [&](const std::string &name) -> Var * { return ResolveVar(name, ns, true); };

	// Stack of body vectors; ifs tracks the active IfCmd for else/endif handling
	std::vector<std::vector<CmdPtr> *> body_stack;
	std::vector<IfCmd *> if_stack;
	body_stack.push_back(&cl.cmds);

	auto add = [&](CmdPtr c, int phase, const IniLine &l) {
		c->phase = phase;
		c->ini_line = "[" + sec.name + "] " + l.raw;
		body_stack.back()->push_back(c);
	};

	for (const IniLine &l : sec.lines) {
		std::string raw = l.raw;
		std::string lraw = lower(raw);

		// ---- flow control --------------------------------------------------
		if (starts_with_word(lraw, "if")) {
			std::string cond = trim(raw.substr(2));
			std::string err;
			auto e = ParseExpr(cond, resolver, &err);
			std::shared_ptr<IfCmd> ic(new IfCmd());
			ic->phase = PHASE_BOTH;
			ic->ini_line = "[" + sec.name + "] " + raw;
			IfCmd::Branch br;
			br.cond = std::move(e);
			if (!br.cond) {
				warn("[" + sec.name + "] bad if condition '" + cond + "': " + err);
				br.cond.reset(new Expr()); // constant 0
			}
			ic->branches.push_back(std::move(br));
			body_stack.back()->push_back(ic);
			if_stack.push_back(ic.get());
			body_stack.push_back(&ic->branches.back().body);
			continue;
		}
		if (starts_with_word(lraw, "elif") || lraw.compare(0, 7, "else if") == 0) {
			if (if_stack.empty()) {
				warn("[" + sec.name + "] elif without if");
				continue;
			}
			size_t off = lraw.compare(0, 4, "elif") == 0 ? 4 : 7;
			std::string cond = trim(raw.substr(off));
			std::string err;
			auto e = ParseExpr(cond, resolver, &err);
			IfCmd::Branch br;
			br.cond = std::move(e);
			if (!br.cond) {
				warn("[" + sec.name + "] bad elif condition '" + cond + "': " + err);
				br.cond.reset(new Expr());
			}
			IfCmd *ic = if_stack.back();
			ic->branches.push_back(std::move(br));
			body_stack.back() = &ic->branches.back().body;
			continue;
		}
		if (lraw == "else") {
			if (if_stack.empty()) {
				warn("[" + sec.name + "] else without if");
				continue;
			}
			IfCmd *ic = if_stack.back();
			IfCmd::Branch br; // null condition = else
			ic->branches.push_back(std::move(br));
			body_stack.back() = &ic->branches.back().body;
			continue;
		}
		if (lraw == "endif") {
			if (if_stack.empty()) {
				warn("[" + sec.name + "] endif without if");
				continue;
			}
			if_stack.pop_back();
			body_stack.pop_back();
			continue;
		}

		// ---- normal line: strip pre/post/global/persist prefixes ---------
		std::string lhs = l.key;
		int phase = -1;
		for (;;) {
			std::string ll = lower(lhs);
			size_t sp = lhs.find_first_of(" \t");
			if (sp == std::string::npos)
				break;
			std::string first = ll.substr(0, sp);
			if (first == "pre") {
				phase = PHASE_PRE;
			} else if (first == "post") {
				phase = PHASE_POST;
			} else if (first == "global" || first == "persist" || first == "local") {
				// scope keywords - variables are treated as persistent globals
			} else
				break;
			lhs = trim(lhs.substr(sp));
		}
		std::string ll = lower(lhs);

		if (!l.has_eq) {
			if (ll == "else" || ll == "endif")
				continue;
			warn("[" + sec.name + "] ignoring line without '=': " + raw);
			continue;
		}
		if (is_setting_key(ll))
			continue; // handled by the section parser

		std::string val = l.val;

		if (ll == "run") {
			CommandList *t = FindCL(val, ns);
			std::shared_ptr<RunCmd> c(new RunCmd());
			if (!t) {
				std::string lv = lower(val);
				if (lv.compare(0, 18, "builtincommandlist") == 0)
					continue; // DX11 built-in command lists have no DX12 equivalent; silently ignored
				if (lv.compare(0, 12, "customshader") == 0 || lv.compare(0, 19, "builtincustomshader") == 0)
					warn("[" + sec.name + "] custom shaders are not supported in the DX12 build: " + raw);
				else
					warn("[" + sec.name + "] run target not found: " + val);
				continue;
			}
			c->target = t;
			add(c, phase < 0 ? PHASE_BOTH : phase, l);
		} else if (ll == "handling") {
			if (lower(val) == "skip") {
				add(std::make_shared<SkipCmd>(), phase < 0 ? PHASE_PRE : phase, l);
			} else
				warn("[" + sec.name + "] unsupported handling: " + val);
		} else if (ll == "draw" || ll == "drawindexed" || ll == "drawinstanced" || ll == "drawindexedinstanced") {
			std::shared_ptr<DrawCmd> c(new DrawCmd());
			size_t need = 0;
			if (ll == "draw") { c->kind = DrawCmd::DRAW; need = 2; }
			else if (ll == "drawindexed") { c->kind = DrawCmd::DRAWINDEXED; need = 3; }
			else if (ll == "drawinstanced") { c->kind = DrawCmd::DRAWINSTANCED; need = 4; }
			else { c->kind = DrawCmd::DRAWINDEXEDINSTANCED; need = 5; }
			std::string lv = lower(val);
			if (lv == "from_caller") {
				c->from_caller = true;
			} else {
				bool ok = true;
				for (auto &a : split(val, ',')) {
					std::string err;
					auto e = ParseExpr(a, resolver, &err);
					if (!e) {
						warn("[" + sec.name + "] bad draw argument '" + a + "': " + err);
						ok = false;
						break;
					}
					c->args.push_back(std::move(e));
				}
				if (!ok || c->args.size() != need) {
					if (ok)
						warn("[" + sec.name + "] wrong number of draw arguments: " + raw);
					continue;
				}
			}
			add(c, phase < 0 ? PHASE_PRE : phase, l);
		} else if (ll == "ib" || (ll.size() >= 3 && ll.compare(0, 2, "vb") == 0 && isdigit((unsigned char)ll[2]))) {
			std::vector<std::string> toks = split_ws(val);
			size_t k = 0;
			while (k < toks.size() && (lower(toks[k]) == "ref" || lower(toks[k]) == "copy" || lower(toks[k]) == "copy_desc"))
				k++;
			if (k >= toks.size()) {
				warn("[" + sec.name + "] missing resource: " + raw);
				continue;
			}
			std::string target = toks[k];
			CustomResource *r = nullptr;
			if (lower(target) != "null") {
				r = FindRes(target, ns);
				if (!r) {
					warn("[" + sec.name + "] resource not found: " + target);
					continue;
				}
			}
			if (ll == "ib") {
				std::shared_ptr<SetIBCmd> c(new SetIBCmd());
				c->res = r;
				add(c, phase < 0 ? PHASE_PRE : phase, l);
			} else {
				int slot = atoi(ll.c_str() + 2);
				if (slot > 15) {
					warn("[" + sec.name + "] vertex buffer slot out of range: " + raw);
					continue;
				}
				std::shared_ptr<SetVBCmd> c(new SetVBCmd());
				c->slot = slot;
				c->res = r;
				add(c, phase < 0 ? PHASE_PRE : phase, l);
			}
		} else if (ll == "checktextureoverride") {
			std::shared_ptr<CheckTOCmd> c(new CheckTOCmd());
			c->target = lower(trim(val));
			add(c, phase < 0 ? PHASE_BOTH : phase, l);
		} else if (!ll.empty() && ll[0] == '$') {
			std::shared_ptr<AssignCmd> c(new AssignCmd());
			c->v = ResolveVar(ll.substr(1), ns, true);
			std::string err;
			c->e = ParseExpr(val, resolver, &err);
			if (!c->e) {
				warn("[" + sec.name + "] bad expression '" + val + "': " + err);
				continue;
			}
			add(c, phase < 0 ? PHASE_PRE : phase, l);
		} else if (ll.compare(0, 8, "resource") == 0) {
			CustomResource *dst = FindRes(lhs, ns);
			std::vector<std::string> toks = split_ws(val);
			size_t k = 0;
			while (k < toks.size() && (lower(toks[k]) == "ref" || lower(toks[k]) == "copy" || lower(toks[k]) == "copy_desc"))
				k++;
			if (!dst || k >= toks.size()) {
				warn("[" + sec.name + "] cannot resolve resource assignment: " + raw);
				continue;
			}
			CustomResource *src = lower(toks[k]) == "null" ? nullptr : FindRes(toks[k], ns);
			if (!src && lower(toks[k]) != "null") {
				warn("[" + sec.name + "] resource not found: " + toks[k]);
				continue;
			}
			std::shared_ptr<AliasCmd> c(new AliasCmd());
			c->dst = dst;
			c->src = src;
			add(c, phase < 0 ? PHASE_PRE : phase, l);
		} else {
			// ps-t0 = ..., this = ..., o0 = ..., etc: texture / render target binding is
			// not implemented in this DX12 version yet.
			static std::set<std::string> warned;
			std::string wkey = sec.name + "|" + raw;
			if (warned.insert(wkey).second)
				mlog("NOTE: [%s] '%s' is not supported by the DX12 build yet (ignored)\n", sec.name.c_str(), raw.c_str());
			add(std::make_shared<NoopCmd>(), PHASE_PRE, l);
		}
	}

	if (!if_stack.empty())
		warn("[" + sec.name + "] missing endif");
}

void Config::ParseSettings(const IniSection &sec)
{
	for (const IniLine &l : sec.lines) {
		std::string k = lower(l.key), v = l.val;
		if (sec.lname == "hunting") {
			if (k == "hunting") settings.hunting = atoi(v.c_str());
			else if (k == "marking_mode") settings.marking_mode = lower(v);
			else if (k == "marking_actions") {
				std::string lv = lower(v);
				settings.marking_clipboard = lv.find("clipboard") != std::string::npos;
				settings.marking_dump_shaders = lv.find("hlsl") != std::string::npos || lv.find("asm") != std::string::npos || lv.find("dump") != std::string::npos;
			} else if (l.has_eq) {
				KeyCombo kc;
				if (parse_key_combo(v, &kc))
					hunt_keys[k] = kc;
			}
		} else if (sec.lname == "logging") {
			if (k == "calls" || k == "debug") settings.debug_log = settings.debug_log || atoi(v.c_str()) != 0;
		} else if (sec.lname == "dx12") {
			if (k == "shadow_budget_mb") settings.shadow_budget_mb = atoi(v.c_str());
			else if (k == "shadow_max_copy_mb") settings.shadow_max_copy_mb = atoi(v.c_str());
			else if (k == "export_shaders") settings.export_shaders = atoi(v.c_str()) != 0;
			else if (k == "overlay") settings.overlay = atoi(v.c_str()) != 0;
			else if (k == "log") settings.log_enabled = atoi(v.c_str()) != 0;
			else if (k == "analyse_options") settings.analyse_options = lower(v);
			else if (k == "fa_dedupe") settings.fa_dedupe = atoi(v.c_str()) != 0;
			else if (k == "hook_present") settings.hook_present = atoi(v.c_str()) != 0;
			else if (k == "hook_resources") settings.hook_resources = atoi(v.c_str()) != 0;
			else if (k == "hook_release") settings.hook_release = atoi(v.c_str()) != 0;
			else if (k == "hook_pipeline") settings.hook_pipeline = atoi(v.c_str()) != 0;
			else if (k == "hook_commandlists") settings.hook_commandlists = atoi(v.c_str()) != 0;
			else if (k == "hash_sample_threshold_kb") settings.hash_sample_threshold_kb = atoi(v.c_str());
			else if (k == "log_pso") settings.log_pso = atoi(v.c_str()) != 0;
		}
	}
	if (sec.lname == "hunting" && !settings.analyse_options.empty()) {}
}

bool Config::Load(const std::string &ini_path)
{
	IniFile ini;
	if (!ini.Load(ini_path)) {
		for (auto &m : ini.messages)
			warn(m);
		return false;
	}
	ini_dir = ini.root_dir;
	for (auto &m : ini.messages)
		warn(m);
	return Build(ini);
}

bool Config::LoadText(const std::string &text, const std::string &dir)
{
	IniFile ini;
	ini.LoadText(text, "", dir);
	ini_dir = dir;
	return Build(ini);
}

static bool starts(const std::string &s, const char *p)
{
	return s.compare(0, strlen(p), p) == 0;
}

bool Config::Build(IniFile &ini)
{
	vars.clear();
	resources.clear();
	command_lists.clear();
	texture_override_store.clear();
	texture_overrides.clear();
	shader_override_store.clear();
	shader_overrides.clear();
	keys.clear();
	present.cmds.clear();
	hunt_keys.clear();
	lk_ = Lookup();

	// ---- pass 0: global settings from the root ini ----------------------------
	for (auto &sec : ini.sections)
		if (sec.ns.empty())
			ParseSettings(sec);

	// ---- pass 1: declare variables, resources, command lists, overrides -------
	struct Pending {
		const IniSection *sec;
		CommandList *cl;
		TextureOverride *to;
		ShaderOverride *so;
		KeySection *ks;
	};
	std::vector<Pending> pending;
	static const char *types[] = { "commandlist", "resource", nullptr };

	auto register_alias = [&](const IniSection &sec, std::map<std::string, void *> *unused) { (void)unused; (void)sec; };
	(void)register_alias;

	for (auto &sec : ini.sections) {
		const std::string &ln = sec.lname;
		std::string key = ns_prefix_key(ln, sec.ns);

		auto alias_for = [&](const char *type) -> std::string {
			size_t n = strlen(type);
			std::string rest = ln.substr(n);
			if (!rest.empty() && rest[0] == '\\')
				rest = rest.substr(1);
			return std::string(type) + "\\" + sec.ns + "\\" + rest;
		};

		if (ln == "constants") {
			for (auto &l : sec.lines) {
				if (!l.has_eq)
					continue;
				std::string lhs = l.key;
				for (;;) {
					size_t sp = lhs.find_first_of(" \t");
					if (sp == std::string::npos)
						break;
					std::string f = lower(lhs.substr(0, sp));
					if (f == "global" || f == "persist" || f == "local")
						lhs = trim(lhs.substr(sp));
					else
						break;
				}
				if (lhs.empty() || lhs[0] != '$')
					continue;
				std::string name = lower(lhs.substr(1));
				std::string vkey = sec.ns.empty() ? name : ("\\" + sec.ns + "\\" + name);
				Var *v = GetVar(vkey, true);
				v->global = true;
				std::string err;
				auto e = ParseExpr(l.val, [&](const std::string &n) -> Var * { return ResolveVar(n, sec.ns, true); }, &err);
				if (e) {
					EvalEnv env;
					v->v = e->Eval(env);
				} else
					warn("[Constants] bad value for " + lhs + ": " + l.val);
			}
		} else if (starts(ln, "resource")) {
			std::unique_ptr<CustomResource> r(new CustomResource());
			r->name = sec.name;
			r->dir = sec.dir;
			for (auto &l : sec.lines) {
				std::string k = lower(l.key);
				if (k == "type") r->type = lower(l.val);
				else if (k == "filename") r->filename = l.val;
				else if (k == "stride") r->stride = (uint32_t)strtoul(l.val.c_str(), nullptr, 0);
				else if (k == "format") r->format = fmt_from_name(l.val);
				else if (k == "data") {
					std::string v = l.val;
					if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
						std::string str = v.substr(1, v.size() - 2);
						r->data.assign(str.begin(), str.end());
					} else {
						warn("[" + sec.name + "] only quoted string 'data' is supported");
					}
					r->loaded = r->filename.empty();
				}
			}
			CustomResource *p = r.get();
			resources[key] = std::move(r);
			lk_.res[key] = p;
			if (!sec.ns.empty())
				lk_.res[alias_for("resource")] = p;
		} else if (starts(ln, "commandlist")) {
			std::unique_ptr<CommandList> c(new CommandList());
			c->name = sec.name;
			CommandList *p = c.get();
			command_lists[key] = std::move(c);
			lk_.cl[key] = p;
			if (!sec.ns.empty())
				lk_.cl[alias_for("commandlist")] = p;
			pending.push_back({ &sec, p, nullptr, nullptr, nullptr });
		} else if (ln == "present") {
			pending.push_back({ &sec, &present, nullptr, nullptr, nullptr });
		} else if (starts(ln, "textureoverride")) {
			std::unique_ptr<TextureOverride> t(new TextureOverride());
			t->name = sec.name;
			bool bad = false;
			for (auto &l : sec.lines) {
				std::string k = lower(l.key);
				if (k == "hash") {
					char *end = nullptr;
					t->hash = (uint32_t)strtoul(l.val.c_str(), &end, 16);
					t->has_hash = end && end != l.val.c_str();
				} else if (k == "match_priority") {
					t->priority = atoi(l.val.c_str());
				} else if (k == "override_vertex_count") {
					t->override_vertex_count = atoi(l.val.c_str());
				} else if (k == "override_byte_stride") {
					t->override_byte_stride = atoi(l.val.c_str());
				} else if (k.compare(0, 6, "match_") == 0) {
					FuzzyMatch *fm = nullptr;
					if (k == "match_first_vertex") fm = &t->m_first_vertex;
					else if (k == "match_first_index") fm = &t->m_first_index;
					else if (k == "match_first_instance") fm = &t->m_first_instance;
					else if (k == "match_vertex_count") fm = &t->m_vertex_count;
					else if (k == "match_index_count") fm = &t->m_index_count;
					else if (k == "match_instance_count") fm = &t->m_instance_count;
					if (fm) {
						if (!fm->parse(l.val))
							warn("[" + sec.name + "] bad " + k + " value: " + l.val);
						t->has_draw_match = true;
					}
				} else if (k.compare(0, 3, "ps-") == 0 || k.compare(0, 3, "vs-") == 0 ||
				           k.compare(0, 3, "gs-") == 0 || k.compare(0, 3, "hs-") == 0 ||
				           k.compare(0, 3, "ds-") == 0 || k.compare(0, 3, "cs-") == 0) {
					// Texture replacement: ps-t0 = ResourceName, vs-t1 = ResourceName, etc.
					t->texture_replacements[k] = l.val;
				}
			}
			if (!t->has_hash) {
				// Without a hash it may be a fuzzy (desc based) override which we don't support
				bad = true;
				warn("[" + sec.name + "] TextureOverride has no hash (fuzzy matching is not supported)");
			}
			TextureOverride *p = t.get();
			texture_override_store.push_back(std::move(t));
			if (!bad)
				texture_overrides[p->hash].push_back(p);
			pending.push_back({ &sec, &p->cl, p, nullptr, nullptr });
		} else if (starts(ln, "shaderoverride")) {
			std::unique_ptr<ShaderOverride> s(new ShaderOverride());
			s->name = sec.name;
			for (auto &l : sec.lines) {
				if (lower(l.key) == "hash") {
					char *end = nullptr;
					s->hash = strtoull(l.val.c_str(), &end, 16);
					s->has_hash = end && end != l.val.c_str();
				}
			}
			ShaderOverride *p = s.get();
			shader_override_store.push_back(std::move(s));
			if (p->has_hash)
				shader_overrides[p->hash].push_back(p);
			pending.push_back({ &sec, &p->cl, nullptr, p, nullptr });
		} else if (starts(ln, "key")) {
			std::unique_ptr<KeySection> k(new KeySection());
			k->name = sec.name;
			KeySection *p = k.get();
			keys.push_back(std::move(k));
			pending.push_back({ &sec, &p->cl, nullptr, nullptr, p });
		} else if (starts(ln, "customshader")) {
			std::unique_ptr<CustomShader> cs(new CustomShader());
			cs->name = sec.name;
			for (auto &l : sec.lines) {
				std::string k = lower(l.key);
				if (k == "hash") {
					char *end = nullptr;
					cs->target_hash = strtoull(l.val.c_str(), &end, 16);
				} else if (k == "hlsl") {
					cs->hlsl = l.val;
				} else if (k == "entry_point") {
					cs->entry_point = l.val;
				} else if (k == "target") {
					cs->target = l.val;
				}
			}
			CustomShader *p = cs.get();
			custom_shader_store.push_back(std::move(cs));
			if (p->target_hash != 0)
				custom_shaders[p->target_hash] = p;
		} else if (starts(ln, "shaderregex")) {
			warn("[" + sec.name + "] is not supported by the DX12 build (ignored)");
		}
	}
	(void)types;

	// ---- pass 2: parse command lists --------------------------------------------
	for (auto &p : pending) {
		const IniSection &sec = *p.sec;
		if (p.ks) {
			KeySection *ks = p.ks;
			std::string type = "activate";
			std::vector<std::pair<std::string, std::string>> assigns;
			for (auto &l : sec.lines) {
				std::string k = lower(l.key);
				if (k == "key") {
					if (!parse_key_combo(l.val, &ks->key))
						warn("[" + sec.name + "] unsupported key binding: " + l.val);
				} else if (k == "back") {
					if (!parse_key_combo(l.val, &ks->back))
						warn("[" + sec.name + "] unsupported back binding: " + l.val);
				} else if (k == "type") {
					type = lower(l.val);
				}
			}
			ks->type = type;
			if (type == "cycle" || type == "toggle" || type == "hold") {
				for (auto &l : sec.lines) {
					std::string lhs = l.key;
					std::string ll = lower(lhs);
					if (!l.has_eq)
						continue;
					size_t sp = lhs.find_first_of(" \t");
					if (sp != std::string::npos) {
						std::string f = ll.substr(0, sp);
						if (f == "global" || f == "persist" || f == "local")
							lhs = trim(lhs.substr(sp));
					}
					if (lhs.empty() || lhs[0] != '$')
						continue;
					Var *v = ResolveVar(lhs.substr(1), sec.ns, true);
					std::vector<double> vals;
					for (auto &t : split(l.val, ','))
						vals.push_back(strtod(t.c_str(), nullptr));
					ks->vars.push_back(v);
					ks->values.push_back(vals);
				}
				// also allow "run = X" to be executed on key press
				ParseCommands(*p.cl, sec);
			} else {
				ParseCommands(*p.cl, sec);
			}
			continue;
		}
		ParseCommands(*p.cl, sec);
	}

	// ---- sort overrides by priority then name for deterministic order -----------
	for (auto &kv : texture_overrides)
		std::stable_sort(kv.second.begin(), kv.second.end(), [](TextureOverride *a, TextureOverride *b) {
			if (a->priority != b->priority)
				return a->priority < b->priority;
			return lower(a->name) < lower(b->name);
		});

	mlog("Config: %u texture overrides, %u shader overrides, %u resources, %u command lists, %u keys, %u variables\n",
	     (unsigned)texture_override_store.size(), (unsigned)shader_override_store.size(),
	     (unsigned)resources.size(), (unsigned)command_lists.size(), (unsigned)keys.size(), (unsigned)vars.size());
	return true;
}

const std::vector<TextureOverride *> *Config::FindTextureOverrides(uint32_t hash) const
{
	auto it = texture_overrides.find(hash);
	return it == texture_overrides.end() ? nullptr : &it->second;
}

const std::vector<ShaderOverride *> *Config::FindShaderOverrides(uint64_t hash) const
{
	auto it = shader_overrides.find(hash);
	return it == shader_overrides.end() ? nullptr : &it->second;
}

bool Config::PollKeys(const KeyDownFn &down, ExecState &st)
{
	bool fired_any = false;
	for (auto &kp : keys) {
		KeySection &ks = *kp;
		if (!ks.key.valid)
			continue;
		bool pressed = ks.key.Pressed(down);
		bool back = ks.back.valid && ks.back.Pressed(down);
		const std::string &t = ks.type;

		if (t == "cycle") {
			if (!pressed && !back)
				continue;
			if (ks.vars.empty() || ks.values.empty() || ks.values[0].empty())
				continue;
			int n = (int)ks.values[0].size();
			// "smart": resync with reality before advancing
			int cur = ks.index;
			auto matches = [&](int idx) {
				for (size_t i = 0; i < ks.vars.size(); i++) {
					const auto &vl = ks.values[i];
					if (vl.empty())
						continue;
					if (ks.vars[i]->v != vl[idx % vl.size()])
						return false;
				}
				return true;
			};
			if (cur < 0 || !matches(cur)) {
				for (int i = 0; i < n; i++)
					if (matches(i)) {
						cur = i;
						break;
					}
			}
			if (pressed) {
				cur = (cur < 0) ? 0 : (cur + 1) % n;
			} else {
				cur = (cur < 0) ? n - 1 : (cur + n - 1) % n;
			}
			ks.index = cur;
			for (size_t i = 0; i < ks.vars.size(); i++) {
				const auto &vl = ks.values[i];
				if (!vl.empty())
					ks.vars[i]->v = vl[cur % vl.size()];
			}
			fired_any = true;
			mlog("Key [%s] cycle -> %d\n", ks.name.c_str(), cur);
		} else if (t == "toggle") {
			if (!pressed)
				continue;
			ks.toggled = !ks.toggled;
			for (size_t i = 0; i < ks.vars.size(); i++) {
				const auto &vl = ks.values[i];
				if (vl.empty())
					continue;
				double on = vl[0], off = vl.size() > 1 ? vl[1] : 0;
				ks.vars[i]->v = ks.toggled ? on : off;
			}
			fired_any = true;
			mlog("Key [%s] toggle -> %d\n", ks.name.c_str(), ks.toggled ? 1 : 0);
		} else if (t == "hold") {
			bool now = ks.key.Matches(down);
			if (now != ks.held) {
				ks.held = now;
				for (size_t i = 0; i < ks.vars.size(); i++) {
					const auto &vl = ks.values[i];
					if (vl.empty())
						continue;
					ks.vars[i]->v = now ? vl[0] : (vl.size() > 1 ? vl[1] : 0);
				}
				fired_any = true;
			}
		} else {
			if (!pressed)
				continue;
			bool saved_post = st.post;
			st.post = false;
			ks.cl.Run(st);
			st.post = saved_post;
			fired_any = true;
			mlog("Key [%s] activated\n", ks.name.c_str());
		}
	}
	return fired_any;
}

} // namespace mig12
