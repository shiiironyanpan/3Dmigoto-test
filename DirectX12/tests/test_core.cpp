#include "../Config12.h"
#include "../Dump12.h"
#include "../Hash12.h"
#include <cassert>
#include <cstdio>
using namespace mig12;

struct MockEngine : IEngine {
	std::vector<std::string> log;
	void SetIndexBuffer(CustomResource *r) override { log.push_back("ib=" + (r ? r->name : "null") + (r ? "(" + std::to_string(r->data.size()) + ")" : "")); }
	void SetVertexBuffer(int s, CustomResource *r) override { log.push_back("vb" + std::to_string(s) + "=" + (r ? r->name : "null")); }
	void DrawFromCaller() override { log.push_back("from_caller"); }
	void Draw(uint32_t v, uint32_t s) override { log.push_back("draw " + std::to_string(v) + "," + std::to_string(s)); }
	void DrawIndexed(uint32_t c, uint32_t s, int b) override { log.push_back("drawindexed " + std::to_string(c) + "," + std::to_string(s) + "," + std::to_string(b)); }
	void DrawInstanced(uint32_t, uint32_t, uint32_t, uint32_t) override {}
	void DrawIndexedInstanced(uint32_t, uint32_t, uint32_t, int, uint32_t) override {}
};

static void logsink(const char *s) { fputs(s, stdout); }

int main()
{
	g_log_cb = logsink;
	// hashes: crc32c("123456789") == 0xE3069283
	assert(crc32c(0, "123456789", 9) == 0xE3069283u);
	assert(crc32c_sw(0, "123456789", 9) == 0xE3069283u);
	assert(fnv64("a", 1) == 0x61);

	Config cfg;
	bool ok = cfg.Load("ini/d3dx.ini");
	assert(ok);
	printf("hunting=%d marking=%s clipboard=%d keys=%zu hunt_keys=%zu\n", cfg.settings.hunting,
	       cfg.settings.marking_mode.c_str(), cfg.settings.marking_clipboard, cfg.keys.size(), cfg.hunt_keys.size());
	assert(cfg.settings.hunting == 1);
	assert(cfg.hunt_keys.count("next_pixelshader"));
	assert(cfg.hunt_keys.count("analyse_frame"));
	assert(cfg.keys.size() == 1);
	assert(cfg.texture_overrides.size() == 4);

	// namespaced variable
	Var *sv = cfg.GetVar("\\mods\\foo\\foo.ini\\swapvar", false);
	assert(sv);

	// IB override list has 2 entries (IB skip + part0)
	auto *ibs = cfg.FindTextureOverrides(0xcafebabe);
	assert(ibs && ibs->size() == 2);

	for (auto &kv : cfg.resources) kv.second->Load();
	MockEngine eng;
	ExecState st;
	st.engine = &eng;
	DrawCtx d;
	d.indexed = true;
	d.first_index = 0;
	d.index_count = 100;
	st.env.draw = &d;
	for (TextureOverride *to : *ibs) {
		if (!to->matches_draw(d))
			continue;
		st.post = false;
		to->cl.Run(st);
	}
	assert(st.skip_original);
	for (auto &l : eng.log)
		printf("  engine: %s\n", l.c_str());
	for (auto &l : eng.log) fprintf(stderr, "LOG: %s\n", l.c_str());
	assert(eng.log.size() == 2);
	assert(eng.log[0].find("ib=ResourceFooPart0IB(12)") == 0);
	assert(eng.log[1] == "drawindexed 3,0,0");

	// draw with first_index = 3 must not match the part override
	DrawCtx d2 = d;
	d2.first_index = 3;
	int matched = 0;
	for (TextureOverride *to : *ibs)
		if (to->matches_draw(d2)) matched++;
	assert(matched == 1);

	// key cycle: smart start at 0 -> first press goes to 1
	KeyCombo *kc = &cfg.keys[0]->key;
	bool down = false;
	KeyDownFn fn = [&](int vk) { return vk == 0x28 && down; };
	cfg.PollKeys(fn, st);
	down = true;
	cfg.PollKeys(fn, st);
	printf("swapvar after press = %g\n", sv->v);
	assert(sv->v == 1);
	down = false; cfg.PollKeys(fn, st);
	down = true; cfg.PollKeys(fn, st);
	assert(sv->v == 2);
	(void)kc;

	// with swapvar 1 the else-if branch runs
	sv->v = 1;
	MockEngine eng2; st.engine = &eng2; st.skip_original = false;
	for (TextureOverride *to : *ibs) if (to->matches_draw(d)) to->cl.Run(st);
	assert(eng2.log.size() == 2 && eng2.log[1] == "drawindexed 6,3,0");

	// post phase: [Present] 'post $active = 0'
	Var *act = cfg.GetVar("active", false);
	assert(act);
	act->v = 1;
	st.post = true; cfg.present.Run(st);
	assert(act->v == 0);

	// blend override draws N
	MockEngine eng3; st.engine = &eng3; st.post = false;
	auto *bl = cfg.FindTextureOverrides(0xdeadbeef);
	assert(bl && bl->size() == 1);
	(*bl)[0]->cl.Run(st);
	assert(eng3.log.size() == 2 && eng3.log[1] == "draw 1234,0");
	auto *vl = cfg.FindTextureOverrides(0x11112222);
	assert(vl && (*vl)[0]->override_vertex_count == 1234 && (*vl)[0]->override_byte_stride == 40);

	// resource loading
	for (auto &kv : cfg.resources) { assert(kv.second->Load()); }

	// dump formats
	std::vector<IAElement> lay;
	IAElement e; e.semantic = "POSITION"; e.format = 6; e.slot = 0; e.aligned_offset = 0; lay.push_back(e);
	IAElement e2; e2.semantic = "TEXCOORD"; e2.semantic_index = 1; e2.format = 16; e2.slot = 0; e2.aligned_offset = IA_APPEND; lay.push_back(e2);
	float verts[10] = {1, 2, 3, 0.5f, 0.25f, 4, 5, 6, 0.75f, 1.0f};
	std::string vt = dump_vb_txt((uint8_t *)verts, sizeof(verts), 0, 20, 0, 0, 2, lay, "trianglelist", 0, 0);
	printf("%s\n", vt.c_str());
	assert(vt.find("vb0[1]+000 POSITION: 4, 5, 6") != std::string::npos);
	assert(vt.find("vb0[0]+012 TEXCOORD1: 0.5, 0.25") != std::string::npos);
	uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
	std::string it = dump_ib_txt((uint8_t *)idx, sizeof(idx), 57, 0, 0, 6, "trianglelist");
	printf("%s\n", it.c_str());
	assert(it.find("\n0 1 2\n2 1 3\n") != std::string::npos);
	assert(max_index_plus_one((uint8_t *)idx, sizeof(idx), 57, 0, 6) == 4);
	printf("ALL OK\n");
	return 0;
}
