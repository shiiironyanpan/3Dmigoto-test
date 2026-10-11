// 3DMigoto DX12 - d3dx.ini model: settings, variables, expressions, command
// lists, [TextureOverride]/[ShaderOverride]/[Resource]/[Key*] sections.
//
// Everything in this header is portable C++ (no Windows / D3D12 types) so the
// ini semantics can be unit tested; the D3D12 glue implements IEngine.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Ini12.h"

namespace mig12 {

// ---- logging bridge (the glue installs a sink that writes d3d11_log.txt-style output)
extern void (*g_log_cb)(const char *);
void mlog(const char *fmt, ...);

// ---- variables / expressions ---------------------------------------------------

struct Var {
	std::string name;
	double v = 0;
	bool global = false;
};

struct DrawCtx {
	bool indexed = false;
	uint32_t vertex_count = 0, index_count = 0, instance_count = 1;
	uint32_t first_vertex = 0, first_index = 0, first_instance = 0;
	int base_vertex = 0;
};

struct EvalEnv {
	double time = 0;
	double rt_width = 0, rt_height = 0, window_width = 0, window_height = 0;
	int hunting = 0;
	bool analyse_frame = false;
	const DrawCtx *draw = nullptr;
};

struct Expr {
	enum Kind { CONSTANT, VAR, BUILTIN, UNARY, BINARY } kind = CONSTANT;
	enum Op { ADD, SUB, MUL, DIV, IDIV, MOD, LT, LE, GT, GE, EQ, NE, AND, OR, NOT, NEG } op = ADD;
	double c = 0;
	Var *var = nullptr;
	int builtin = 0;
	std::unique_ptr<Expr> a, b;

	double Eval(const EvalEnv &env) const;
};

typedef std::function<Var *(const std::string &name_after_dollar)> VarResolver;
std::unique_ptr<Expr> ParseExpr(const std::string &text, const VarResolver &resolve, std::string *err);

// ---- custom resources ([ResourceXXX]) -----------------------------------------

struct CustomResource {
	std::string name;
	std::string type; // lower case: "buffer", ...
	std::string filename;
	std::string dir; // directory used to resolve filename
	uint32_t stride = 0;
	int format = 0; // DXGI_FORMAT numeric value
	std::vector<uint8_t> data;
	bool loaded = false;
	bool load_failed = false;
	CustomResource *alias = nullptr; // set by "Resource = ref OtherResource"

	// Owned by the D3D12 glue
	void *gpu = nullptr;
	uint64_t gpu_va = 0;
	uint32_t gpu_size = 0;

	CustomResource *Resolve()
	{
		CustomResource *r = this;
		for (int i = 0; i < 16 && r->alias; i++)
			r = r->alias;
		return r;
	}
	bool Load(); // reads filename (relative to dir) if not inline data
};

// ---- command lists -----------------------------------------------------------

struct IEngine {
	virtual ~IEngine() {}
	virtual void SetIndexBuffer(CustomResource *r) = 0; // nullptr => unbind
	virtual void SetVertexBuffer(int slot, CustomResource *r) = 0;
	virtual void DrawFromCaller() = 0;
	virtual void Draw(uint32_t vertex_count, uint32_t start) = 0;
	virtual void DrawIndexed(uint32_t index_count, uint32_t start, int base) = 0;
	virtual void DrawInstanced(uint32_t vc, uint32_t ic, uint32_t sv, uint32_t si) = 0;
	virtual void DrawIndexedInstanced(uint32_t ixc, uint32_t ic, uint32_t si, int bv, uint32_t sinst) = 0;
};

struct ExecState {
	IEngine *engine = nullptr;
	EvalEnv env;
	bool post = false;
	bool skip_original = false;
	int depth = 0;
	// "checktextureoverride = ib / vbN" support, implemented by the glue
	std::function<void(ExecState &, const std::string &target)> check_texture_override;
};

enum { PHASE_PRE = 1, PHASE_POST = 2, PHASE_BOTH = 3 };

struct Command {
	int phase = PHASE_PRE;
	std::string ini_line;
	virtual ~Command() {}
	virtual void Run(ExecState &st) = 0;
};
typedef std::shared_ptr<Command> CmdPtr;

struct CommandList {
	std::string name;
	std::vector<CmdPtr> cmds;
	void Run(ExecState &st) const;
};

// ---- overrides ------------------------------------------------------------------

struct FuzzyMatch {
	enum Op { ALWAYS, EQ, NE, LT, LE, GT, GE } op = ALWAYS;
	uint32_t val = 0;
	bool matches(uint32_t v) const;
	bool parse(const std::string &s);
};

struct TextureOverride {
	std::string name;
	uint32_t hash = 0;
	bool has_hash = false;
	bool has_draw_match = false;
	int priority = 0;
	FuzzyMatch m_first_vertex, m_first_index, m_first_instance, m_vertex_count, m_index_count, m_instance_count;
	int override_vertex_count = -1;
	int override_byte_stride = -1;
	CommandList cl;
	bool matches_draw(const DrawCtx &d) const;
};

struct ShaderOverride {
	std::string name;
	uint64_t hash = 0;
	bool has_hash = false;
	CommandList cl;
};

// ---- keys -----------------------------------------------------------------------

typedef std::function<bool(int vk)> KeyDownFn;

struct KeyTok {
	int vk = 0;
	bool negate = false;
};

struct KeyCombo {
	std::vector<KeyTok> toks;
	bool no_modifiers = false;
	bool valid = false;
	bool prev = false;
	bool Matches(const KeyDownFn &down) const;
	bool Pressed(const KeyDownFn &down); // edge triggered
	std::string text;
};

int parse_vk(const std::string &name);
bool parse_key_combo(const std::string &text, KeyCombo *out);

struct KeySection {
	std::string name;
	KeyCombo key, back;
	std::string type; // activate (default), cycle, toggle, hold
	std::vector<Var *> vars;
	std::vector<std::vector<double>> values;
	int index = -1;
	bool toggled = false;
	bool held = false;
	CommandList cl;
};

// ---- the whole configuration -----------------------------------------------------

struct Settings {
	int hunting = 0; // 0 off, 1 on, 2 soft-disabled
	std::string marking_mode = "skip";
	bool marking_clipboard = true;
	bool marking_dump_shaders = true;
	// DX12 specific
	int shadow_budget_mb = 1536;
	int shadow_max_copy_mb = 256;
	bool export_shaders = false; // write every created shader to ShaderCache
	bool overlay = true;
	bool log_enabled = true;
	bool debug_log = false;
	bool log_pso = false; // log PSO creation with shader hashes
	std::string analyse_options;
	bool fa_dump_vb = true, fa_dump_ib = true, fa_dump_shaders = true, fa_dedupe = true;
	// Bisect switches ([DX12] section) to turn individual hook groups off when diagnosing a crash
	bool hook_present = true;       // swap chain proxy (frame boundary, input, hunting, frame analysis)
	bool hook_resources = true;     // resource creation / Map / Unmap tracking and shadow copies
	bool hook_release = true;       // IUnknown::Release hook on resources (frees tracking data)
	bool hook_pipeline = true;      // pipeline state creation (shader hashes / replacement / layouts)
	bool hook_commandlists = true;  // command list hooks (draws, bindings, copies)
	int hash_sample_threshold_kb = 4096; // views bigger than this are hashed from samples, not fully
};

class Config {
public:
	Settings settings;
	std::string ini_dir;

	std::map<std::string, std::unique_ptr<Var>> vars;
	std::map<std::string, std::unique_ptr<CustomResource>> resources;
	std::map<std::string, std::unique_ptr<CommandList>> command_lists;
	std::vector<std::unique_ptr<TextureOverride>> texture_override_store;
	std::unordered_map<uint32_t, std::vector<TextureOverride *>> texture_overrides;
	std::vector<std::unique_ptr<ShaderOverride>> shader_override_store;
	std::unordered_map<uint64_t, std::vector<ShaderOverride *>> shader_overrides;
	std::vector<std::unique_ptr<KeySection>> keys;
	CommandList present;
	std::map<std::string, KeyCombo> hunt_keys;
	std::vector<std::string> warnings;

	bool Load(const std::string &ini_path);
	bool LoadText(const std::string &text, const std::string &dir = "."); // tests

	// Runs [Key*] sections; returns true if anything fired
	bool PollKeys(const KeyDownFn &down, ExecState &st);

	// Draw-time helpers
	const std::vector<TextureOverride *> *FindTextureOverrides(uint32_t hash) const;
	const std::vector<ShaderOverride *> *FindShaderOverrides(uint64_t hash) const;
	bool HasTextureOverrides() const { return !texture_overrides.empty(); }

	Var *GetVar(const std::string &key, bool create);

private:
	bool Build(IniFile &ini);
	std::string ns_prefix_key(const std::string &lname, const std::string &ns) const;

	struct Lookup {
		std::map<std::string, CommandList *> cl;
		std::map<std::string, CustomResource *> res;
	};
	Lookup lk_;

	CommandList *FindCL(const std::string &ref, const std::string &ns);
	CustomResource *FindRes(const std::string &ref, const std::string &ns);
	Var *ResolveVar(const std::string &name, const std::string &ns, bool create);
	void ParseCommands(CommandList &cl, const IniSection &sec);
	void ParseSettings(const IniSection &sec);
	void ParseKeyDefaults();
	void warn(const std::string &s);

	friend struct ParserAccess;
};

} // namespace mig12
