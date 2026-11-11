#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gfx.h"

struct GfxShader { int kind, count; GfxShaderRes resources[32]; };
int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **out) {
    *out = s->resources;
    return kind == s->kind ? s->count : 0;
}
const uint32_t *gfx_shader_spirv(const GfxShader *s, uint64_t *n) { (void)s; *n = 0; return NULL; }
uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo out[32]) { (void)s; (void)out; return 0; }

int main(void) {
    GfxShaderAnswers a = {0}, out[GFX_PREP_ALTERNATIVES];
    GfxShader s = {.kind = GFX_RES_UBO, .count = 1, .resources = {{.slot = 4}}};
    assert(gfx_shader_guess_alternatives(&s, GFX_STAGE_VS, &a, out) == 0);
    s.resources[0].slot = 7;
    assert(gfx_shader_guess_alternatives(&s, GFX_STAGE_VS, &a, out) == 1);
    assert(out[0].mvprev && !a.mvprev);
    assert(gfx_shader_guess_alternatives(&s, GFX_STAGE_VS, &out[0], out) == 0);
    assert(gfx_shader_guess_alternatives(&s, GFX_STAGE_FS, &a, out) == 0);
    s = (GfxShader){.kind = GFX_RES_TEX, .count = 2,
                   .resources = {{.slot = 0, .type = 3}, {.slot = 1, .type = 3}}};
    a.tex_type[0] = a.tex_type[1] = GFX_TEX_2D;
    assert(gfx_shader_guess_alternatives(&s, GFX_STAGE_CS, &a, out) == 3);
    assert(out[0].tex_type[0] == GFX_TEX_2D_ARRAY && out[0].tex_type[1] == GFX_TEX_2D);
    assert(out[1].tex_type[0] == GFX_TEX_2D && out[1].tex_type[1] == GFX_TEX_2D_ARRAY);
    assert(out[2].tex_type[0] == GFX_TEX_2D_ARRAY && out[2].tex_type[1] == GFX_TEX_2D_ARRAY);
    s.resources[0].pool = 1;
    s.resources[1].type = 0x203; // MSAA is not a flat/array-layer alternative
    assert(gfx_shader_guess_alternatives(&s, GFX_STAGE_CS, &a, out) == 0);
    s.resources[0].pool = 0;
    s.resources[0].count = 32;
    memset(a.tex_type, GFX_TEX_2D, sizeof a.tex_type);
    assert(gfx_shader_guess_alternatives(&s, GFX_STAGE_CS, &a, out) == GFX_PREP_ALTERNATIVES);
    for (int i = 0; i < 32; i++) assert(out[32].tex_type[i] == GFX_TEX_2D_ARRAY);
    s.kind = GFX_RES_IMG;
    assert(gfx_shader_guess_alternatives(&s, GFX_STAGE_CS, &a, out) == GFX_PREP_ALTERNATIVES);
    for (int i = 0; i < 32; i++) {
        assert(out[32].img_type[i] == a.img_type[i]);
        assert(out[32].img_format[i] == a.img_format[i]);
    }
    puts("Shader preparation pattern checks passed");
    return 0;
}
