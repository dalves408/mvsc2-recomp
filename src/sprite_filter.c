/*
 * The F1 sprite filter. The renderer draws a fight's character sprites into their own layer at the game's own resolution
 * (see gpu_backend.c), where a pose is whole and its tiles no longer show. That layer goes through one of the filters
 * below, then optional smoothing and sharpening, and over the picture at the render scale.
 *
 * OmniScale: Copyright (c) 2015-2016 Lior Halphon.
 * ScaleFX: Copyright (c) 2016 Sp00kyFox.
 * xBR level 2: Copyright (C) 2011-2022 Hyllian.
 *
 * Each of those is under the MIT license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sprite_filter.h"
#include "sprite_filter_hlsl.h"

enum { PS_OMNI, PS_XBR0, PS_XBR1, PS_SFX0, PS_SFX1, PS_SFX2, PS_SFX3, PS_SFX4, PS_SMOOTH, PS_SHARPEN, PS_COPY, PS_COUNT };
enum { T_A, T_B, T_C, T_D, T_E, T_OUT, T_OUT2, T_COUNT };

typedef struct { ID3D11Texture2D *tex; ID3D11RenderTargetView *rtv; ID3D11ShaderResourceView *srv; uint32_t w, h; DXGI_FORMAT fmt; } Tg;

static ID3D11Device *s_d;
static ID3D11DeviceContext *s_c;
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps[PS_COUNT];
static const char *const s_src[PS_COUNT] = { s_sf_omni, s_sf_xbr0, s_sf_xbr1, s_sf_sfx0, s_sf_sfx1, s_sf_sfx2, s_sf_sfx3, s_sf_sfx4, s_sf_smooth, s_sf_sharpen, s_sf_copy };
static ID3D11Buffer *s_cb;
static ID3D11SamplerState *s_point, *s_linear;
static ID3D11BlendState *s_premult;
static ID3D11RasterizerState *s_rs;
static Tg s_t[T_COUNT];
static int s_failed;

static const char s_vs_src[] =
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint i : SV_VertexID) { V o; float2 q = float2((i << 1) & 2, i & 2);\n"
"  o.uv = q; o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";

static ID3D11PixelShader *pixel_shader(int id)
{
    if (!s_ps[id] && !s_failed) {
        ID3DBlob *code = NULL, *err = NULL;
        if (FAILED(D3DCompile(s_src[id], strlen(s_src[id]), "sprite_filter", NULL, NULL, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err))) {
            fprintf(stderr, "[SPRITEFILTER] shader %d: %s\n", id, err ? (char *)ID3D10Blob_GetBufferPointer(err) : "failed");
            s_failed = 1;
        } else {
            ID3D11Device_CreatePixelShader(s_d, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &s_ps[id]);
        }
        if (code) ID3D10Blob_Release(code);
        if (err) ID3D10Blob_Release(err);
    }
    return s_ps[id];
}

int sprite_filter_init(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    ID3DBlob *code = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sm;
    D3D11_BLEND_DESC bl;
    D3D11_RASTERIZER_DESC rd;
    if (s_vs) return 1;
    if (s_failed) return 0;
    s_d = dev; s_c = ctx;
    s_failed = 1;
    if (FAILED(D3DCompile(s_vs_src, sizeof s_vs_src - 1, "sprite_filter", NULL, NULL, "vs", "vs_4_0", 0, 0, &code, NULL))) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &s_vs);
    ID3D10Blob_Release(code);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_cb);
    memset(&sm, 0, sizeof sm);
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &sm, &s_point);
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(dev, &sm, &s_linear);
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &bl, &s_premult);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &s_rs);
    s_failed = !(s_vs && s_cb && s_point && s_linear && s_premult && s_rs);
    return !s_failed;
}

static Tg *target(int id, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    Tg *t = &s_t[id];
    if (t->tex && (t->w != w || t->h != h || t->fmt != fmt)) {
        ID3D11ShaderResourceView_Release(t->srv);
        ID3D11RenderTargetView_Release(t->rtv);
        ID3D11Texture2D_Release(t->tex);
        memset(t, 0, sizeof *t);
    }
    if (!t->tex) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_d, &td, NULL, &t->tex))) { memset(t, 0, sizeof *t); return NULL; }
        ID3D11Device_CreateRenderTargetView(s_d, (ID3D11Resource *)t->tex, NULL, &t->rtv);
        ID3D11Device_CreateShaderResourceView(s_d, (ID3D11Resource *)t->tex, NULL, &t->srv);
        t->w = w; t->h = h; t->fmt = fmt;
    }
    return t;
}

/* One full-screen pass. a: t0, b: t1. src size goes in src.xy, 1/size in src.zw; output size in dst.xy, 1/width in dst.z. */
static void pass(int ps, ID3D11RenderTargetView *rtv, uint32_t dw, uint32_t dh, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b,
                 uint32_t sw, uint32_t sh, int blend, ID3D11SamplerState *smp, float extra, float radius)
{
    ID3D11ShaderResourceView *views[2], *none[2] = { NULL, NULL };
    ID3D11PixelShader *code = pixel_shader(ps);
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    if (!code) return;
    views[0] = a; views[1] = b;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_c, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float *c = (float *)m.pData;
        c[0] = (float)sw; c[1] = (float)sh;
        c[2] = ps == PS_SMOOTH ? radius : 1.0f / (float)sw; c[3] = 1.0f / (float)sh;
        c[4] = (float)dw; c[5] = (float)dh; c[6] = 1.0f / (float)dw; c[7] = extra;
        ID3D11DeviceContext_Unmap(s_c, (ID3D11Resource *)s_cb, 0);
    }
    ID3D11DeviceContext_PSSetShaderResources(s_c, 0, 2, none);
    ID3D11DeviceContext_OMSetRenderTargets(s_c, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_c, blend ? s_premult : NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_c, NULL, 0);
    ID3D11DeviceContext_RSSetState(s_c, s_rs);
    vp.TopLeftX = vp.TopLeftY = 0; vp.Width = (float)dw; vp.Height = (float)dh; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_c, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(s_c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_c, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_c, code, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_c, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_c, 0, 2, views);
    ID3D11DeviceContext_PSSetSamplers(s_c, 0, 1, &smp);
    ID3D11DeviceContext_Draw(s_c, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_c, 0, 2, none);
    ID3D11DeviceContext_OMSetRenderTargets(s_c, 0, NULL, NULL);
}

void sprite_filter_run(ID3D11ShaderResourceView *layer, uint32_t w, uint32_t h, ID3D11RenderTargetView *dst, uint32_t dw, uint32_t dh,
                       int filter, int smooth, int sharpen)
{
    const DXGI_FORMAT rgba8 = DXGI_FORMAT_R8G8B8A8_UNORM, flt = DXGI_FORMAT_R32G32B32A32_FLOAT;
    static const float k_smooth[3] = { 0, 0.45f, 0.9f }, k_sharp[3] = { 0, 0.35f, 0.85f };
    ID3D11ShaderResourceView *cur = layer;
    uint32_t cw = w, ch = h;
    Tg *out = target(T_OUT, dw, dh, rgba8);
    if (!out) return;

    if (filter == SPRITE_FILTER_OMNISCALE) {
        pass(PS_OMNI, out->rtv, dw, dh, layer, NULL, w, h, 0, s_point, 0, 0);
        cur = out->srv; cw = dw; ch = dh;
    } else if (filter == SPRITE_FILTER_XBR) {
        Tg *info = target(T_A, w, h, rgba8), *big = target(T_B, w * 3, h * 3, rgba8);
        if (!info || !big) return;
        pass(PS_XBR0, info->rtv, w, h, layer, NULL, w, h, 0, s_point, 0, 0);
        pass(PS_XBR1, big->rtv, w * 3, h * 3, info->srv, layer, w, h, 0, s_point, 0, 0);
        cur = big->srv; cw = w * 3; ch = h * 3;
    } else if (filter == SPRITE_FILTER_SCALEFX) {
        Tg *a = target(T_A, w, h, flt), *b = target(T_B, w, h, flt), *c = target(T_C, w, h, rgba8), *d = target(T_D, w, h, rgba8), *e = target(T_E, w * 3, h * 3, rgba8);
        if (!a || !b || !c || !d || !e) return;
        pass(PS_SFX0, a->rtv, w, h, layer, NULL, w, h, 0, s_point, 0, 0);
        pass(PS_SFX1, b->rtv, w, h, a->srv, NULL, w, h, 0, s_point, 0, 0);
        pass(PS_SFX2, c->rtv, w, h, b->srv, a->srv, w, h, 0, s_point, 0, 0);
        pass(PS_SFX3, d->rtv, w, h, c->srv, NULL, w, h, 0, s_point, 0, 0);
        pass(PS_SFX4, e->rtv, w * 3, h * 3, d->srv, layer, w, h, 0, s_point, 0, 0);
        cur = e->srv; cw = w * 3; ch = h * 3;
    }
    if (smooth > 0 && smooth < 3) {
        Tg *t = target(cur == out->srv ? T_OUT2 : T_OUT, cw, ch, rgba8);
        if (t) {
            float radius = (float)cw / (float)w * 0.5f;
            pass(PS_SMOOTH, t->rtv, cw, ch, cur, NULL, cw, ch, 0, s_point, k_smooth[smooth], radius < 1.0f ? 1.0f : radius);
            cur = t->srv;
        }
    }
    if (sharpen > 0 && sharpen < 3) {
        Tg *t = target(cur == s_t[T_OUT].srv ? T_OUT2 : T_OUT, cw, ch, rgba8);
        if (t) {
            pass(PS_SHARPEN, t->rtv, cw, ch, cur, NULL, cw, ch, 0, s_point, k_sharp[sharpen], 0);
            cur = t->srv;
        }
    }
    pass(PS_COPY, dst, dw, dh, cur, NULL, cw, ch, 1, cw == dw && ch == dh ? s_point : s_linear, 0, 0);
}
