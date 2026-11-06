/*
 * D3D11 render back end for the NV2A pushbuffer executor (nv2a_backend.h).
 *
 * The executor decodes the game's pushbuffer, runs vertex programs / fixed
 * function transform on the CPU, clips, and hands triangles in surface pixels
 * to draw(). This back end fills them on the GPU: one render target per guest
 * colour surface, a depth buffer per zeta surface, textures decoded once and
 * cached by content hash. At flip() every surface drawn since the last flip is
 * read back into guest memory, so the existing framebuffer window (title bar,
 * keyboard, fullscreen, RECOMP_D3D_PRESENT) works unchanged.
 *
 * RECOMP_GPU=0 keeps the CPU rasteriser. Not passed on: register combiners
 * and texture stages 1-3 (see nv2a_backend.h).
 */
#define COBJMACROS
#include <windows.h>
extern unsigned long long g_pb_scan_ticks; extern unsigned g_pb_scan_segs;
extern unsigned long long g_pb_t_batch, g_pb_t_xf, g_pb_t_vsh, g_pb_t_clip, g_pb_t_flush; extern unsigned g_pb_n_xfverts;
static unsigned long long gb_qf(void) { static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f); return (unsigned long long)f.QuadPart; }
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "kernel/nv2a_backend.h"
#include "mvc2_debug.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define MAX_RT   8
#define MAX_TEX  768
#define VB_BYTES (8u * 1024u * 1024u)

typedef struct { uint32_t va, w, h, bpp, pitch, dirty, used;
                 ID3D11Texture2D *tex, *stage; ID3D11RenderTargetView *rtv; ID3D11ShaderResourceView *srv; uint32_t sampled;
                 /* deferred readback: the copy queued at the last flip (pstage, one of stage/stage2) and where it goes */
                 ID3D11Texture2D *stage2, *pstage; int pend; uint32_t pva, pw, ph, ppitch, pbpp;
                 /* F1 options: mtex is the multisampled surface drawn to (tex then holds its resolve); down is the
                  * guest-sized copy written back to guest memory when the render scale is above 1 */
                 ID3D11Texture2D *mtex, *down; ID3D11RenderTargetView *down_rtv; uint32_t msdirty, shown_dirty; } Rt;
typedef struct { uint32_t va, w, h;
                 ID3D11Texture2D *tex; ID3D11DepthStencilView *dsv; } Ds;
typedef struct { uint32_t offset, w, h, color, pitch; uint64_t hash; uint32_t used;
                 uint64_t thash, sig; uint32_t vframe; /* texture-bytes hash, sample signature, s_frame+1 of the last full verify (get_tex fast path) */
                 ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; } TexEnt;
typedef struct { uint32_t key[12]; void *obj; } StateEnt;

typedef struct { float sw, sh, tw, th; uint32_t textured, afunc; float aref; uint32_t pad; } Cb;

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11InputLayout *s_il;
#define CB_RING 1024
#define VB_RING 3
static ID3D11Buffer *s_vb, *s_cb;                /* current vertex buffer / first constant buffer */
static ID3D11Buffer *s_vbr[VB_RING], *s_cbr[CB_RING];  /* rings: no buffer is DISCARDed twice within a ring cycle (Intel drivers corrupt queued draws otherwise) */
static unsigned s_vb_i, s_cb_i;
static unsigned s_vendor, s_flush_mode, s_flush_tex; /* RECOMP_GPU_FLUSH_TEX=1: flush before and after every texture (re)creation */        /* RECOMP_GPU_FLUSH: 0 none, 1 P8 512-wide glyph draws, 2 all textured draws, 3 every draw; default 3 on Intel (0x8086), 0 elsewhere */
static unsigned long s_defer_n; static struct { IUnknown *o; uint32_t f; } s_grave[4096];
static ID3D11RasterizerState *s_rs;
static ID3D11Texture2D *s_white;
static ID3D11ShaderResourceView *s_white_srv;
static uint32_t s_vb_off;
/* F1 options, requested from the window's thread and applied at a flip. */
static volatile int s_want_scale = 1, s_want_msaa = 1, s_want_aniso = 1;
static int s_scale = 1, s_msaa = 1, s_aniso = 1, s_resets;
static ID3D11VertexShader *s_bl_vs;
static ID3D11PixelShader *s_bl_ps;
static ID3D11Buffer *s_bl_cb;
static ID3D11SamplerState *s_bl_smp;
static Rt s_rt[MAX_RT];
static Ds s_ds[4];
static TexEnt s_tx[MAX_TEX];
static StateEnt s_bs[96], s_dss[96], s_ss[48];
static int s_nbs, s_ndss, s_nss;
static int s_failed, s_inited, s_noalpha;
static uint32_t s_frame, s_tick;
static uint32_t st_th_fast, st_th_full, st_th_stale;
static uint32_t st_rtsamples, st_draws, st_tris, st_culled, st_decodes, st_clears, st_hash_ns_us, st_flips;
static int s_cull_on = 1, s_verbose, s_trace, s_rb_all, s_trace_all, s_trace_lines, s_rb_sync;
static unsigned s_ta_lo, s_ta_hi;
static int s_ta_sig;
static uint64_t s_ta_seen[512];
static int s_ta_n;
static int ta_new(uint64_t sig)
{
    int i;
    if (!s_ta_sig) return 1;
    for (i = 0; i < s_ta_n; i++) if (s_ta_seen[i] == sig) return 0;
    if (s_ta_n < 512) s_ta_seen[s_ta_n++] = sig;
    return 1;
}

/* Copy between render targets of different sizes: a box over the source pixels each output pixel covers. */
static const char s_bl_hlsl[] =
"Texture2D t : register(t0); SamplerState s : register(s0);\n"
"cbuffer C : register(b0) { float4 src; };\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint i : SV_VertexID) { V o; float2 q = float2((i << 1) & 2, i & 2);\n"
"  o.uv = q; o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"float4 ps(V i) : SV_Target {\n"
"  float2 r = max(src.zw, 1.0); int nx = (int)min(ceil(r.x), 8.0), ny = (int)min(ceil(r.y), 8.0);\n"
"  float4 sum = 0; float2 c = i.uv * src.xy;\n"
"  for (int y = 0; y < ny; y++) for (int x = 0; x < nx; x++) {\n"
"    float2 o = ((float2(x, y) + 0.5) / float2(nx, ny) - 0.5) * r; sum += t.Sample(s, (c + o) / src.xy); }\n"
"  return sum / (nx * ny); }\n";

static const char s_hlsl[] =
"cbuffer C : register(b0) { float2 surf; float2 tdim; uint textured; uint afunc; float aref; uint pad; };\n"
"Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
"struct VI { float4 p : POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
"struct VO { float4 pos : SV_Position; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
"VO vs(VI i) {\n"
"  VO o; float w = 1.0 / max(i.p.w, 1e-20);\n"
"  float nx = i.p.x / surf.x * 2.0 - 1.0; float ny = 1.0 - i.p.y / surf.y * 2.0;\n"
"  float nz = saturate(i.p.z / 16777215.0);\n"
"  o.pos = float4(nx * w, ny * w, nz * w, w); o.c = i.c; o.uv = i.uv; return o; }\n"
"float4 ps(VO i) : SV_Target {\n"
"  float4 c = i.c;\n"
"  if (textured != 0) { float4 t = t0.Sample(s0, i.uv / tdim); if ((pad & 5) != 0) t.a = 1.0; if ((pad & 2) != 0) t.rgb = float3(1.0, 1.0, 1.0); c *= t; }\n"
"  float a = c.a * 255.0; float r = aref;\n"
"  bool ok = true;\n"
"  if (afunc == 0x200) ok = false; else if (afunc == 0x201) ok = a < r - 0.5;\n"
"  else if (afunc == 0x202) ok = abs(a - r) < 0.5; else if (afunc == 0x203) ok = a < r + 0.5;\n"
"  else if (afunc == 0x204) ok = a > r + 0.5; else if (afunc == 0x205) ok = abs(a - r) >= 0.5;\n"
"  else if (afunc == 0x206) ok = a > r - 0.5;\n"
"  if (!ok) discard;\n"
"  return c; }\n";

#define SAFE_REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)
/* Texture-cache entries replaced mid-frame are released two flips later, not while draws that sampled them are still queued. */
static uint32_t s_frame_for_grave;
static void gb_defer_rel(void *o) { if (!o) return; if (s_defer_n >= sizeof(s_grave)/sizeof(s_grave[0])) { IUnknown_Release(s_grave[0].o); memmove(&s_grave[0], &s_grave[1], (s_defer_n - 1) * sizeof(s_grave[0])); s_defer_n--; } s_grave[s_defer_n].o = (IUnknown *)o; s_grave[s_defer_n].f = s_frame_for_grave; s_defer_n++; }
#define DEFER_REL(p) do { if (p) { gb_defer_rel(p); (p) = NULL; } } while (0)

static int gb_init(void)
{
    D3D_FEATURE_LEVEL fl;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D11_INPUT_ELEMENT_DESC el[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    uint32_t white = 0xFFFFFFFFu;
    HRESULT hr;

    { const char *ad = getenv("RECOMP_GPU_ADAPTER");   /* "warp" = Microsoft software rasteriser, for A/B against a vendor driver */
      int warp = ad && (ad[0] == 'w' || ad[0] == 'W');
      const char *fls = getenv("RECOMP_GPU_FL");   /* "11_0" / "10_1" / "10_0": cap the feature level (driver A/B) */
      D3D_FEATURE_LEVEL want[1]; UINT nwant = 0;
      if (fls && !strcmp(fls, "11_0")) { want[0] = D3D_FEATURE_LEVEL_11_0; nwant = 1; }
      else if (fls && !strcmp(fls, "10_1")) { want[0] = D3D_FEATURE_LEVEL_10_1; nwant = 1; }
      else if (fls && !strcmp(fls, "10_0")) { want[0] = D3D_FEATURE_LEVEL_10_0; nwant = 1; }
      hr = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                             nwant ? want : NULL, nwant, D3D11_SDK_VERSION, &s_dev, &fl, &s_ctx); }
    if (FAILED(hr)) { fprintf(stderr, "[GPUBE] D3D11CreateDevice failed 0x%08lX\n", (unsigned long)hr); return 0; }
    hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "gb", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { fprintf(stderr, "[GPUBE] vs: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "gb", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { fprintf(stderr, "[GPUBE] ps: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    ID3D11Device_CreateInputLayout(s_dev, el, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &s_il);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (unsigned vi = 0; vi < VB_RING; vi++) ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vbr[vi]); s_vb = s_vbr[0];
    bd.ByteWidth = sizeof(Cb); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (unsigned ci = 0; ci < CB_RING; ci++) ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cbr[ci]); s_cb = s_cbr[0];
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.ScissorEnable = FALSE;
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);
    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = &white; sd.SysMemPitch = 4; sd.SysMemSlicePitch = 4;
    ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &s_white);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_white, NULL, &s_white_srv);
    SAFE_REL(vsb); SAFE_REL(psb);
    if (!s_vs || !s_ps || !s_il || !s_vb || !s_cb || !s_rs || !s_white_srv) return 0;
    if (SUCCEEDED(D3DCompile(s_bl_hlsl, sizeof s_bl_hlsl - 1, "bl", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err))
        && SUCCEEDED(D3DCompile(s_bl_hlsl, sizeof s_bl_hlsl - 1, "bl", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        D3D11_SAMPLER_DESC sm;
        ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_bl_vs);
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_bl_ps);
        bd.ByteWidth = 16;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_bl_cb);
        memset(&sm, 0, sizeof sm);
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_dev, &sm, &s_bl_smp);
    }
    SAFE_REL(vsb); SAFE_REL(psb);
    if (!s_bl_vs || !s_bl_ps || !s_bl_cb || !s_bl_smp) { fprintf(stderr, "[GPUBE] copy shader failed\n"); return 0; }
    {
        IDXGIDevice *dd = NULL; IDXGIAdapter *ad = NULL; DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
            s_vendor = desc.VendorId; { const char *fm = getenv("RECOMP_GPU_FLUSH"); s_flush_mode = fm ? (unsigned)atoi(fm) : (s_vendor == 0x8086 ? 3u : 0u); { const char *ft = getenv("RECOMP_GPU_FLUSH_TEX"); s_flush_tex = ft ? (unsigned)atoi(ft) : 0u; } }
            fprintf(stderr, "[GPUBE] vendor 0x%04X flush_mode %u flush_tex %u\n", s_vendor, s_flush_mode, s_flush_tex);
            fprintf(stderr, "[GPUBE] D3D11 back end up on '%ls' (feature level 0x%X)\n", desc.Description, (unsigned)fl);
        SAFE_REL(ad); SAFE_REL(dd);
    }
    return 1;
}

static int gb_ready(void)
{
    if (s_failed) return 0;
    if (!s_inited) {
        s_inited = 1;
        s_verbose = getenv("RECOMP_GPU_STATS") != NULL; s_trace = getenv("RECOMP_GPU_TRACE") != NULL; { const char *ta = getenv("RECOMP_GPU_TRACEALL"); s_trace_all = ta != NULL; if (ta && sscanf(ta, "%u-%u", &s_ta_lo, &s_ta_hi) != 2) { s_ta_lo = 0; s_ta_hi = 0xFFFFFFFFu; s_ta_sig = (ta[0] == (char)115); } } s_rb_all = getenv("RECOMP_GPU_SKIPRB") == NULL; /* default: read back every dirty RT (skipping sampled RTs stalled the game) */ s_rb_sync = getenv("RECOMP_GPU_SYNC_RB") != NULL;
        { const char *c = getenv("RECOMP_GPU_CULL"); if (c && c[0] == '0') s_cull_on = 0; }
        if (!gb_init()) { s_failed = 1; fprintf(stderr, "[GPUBE] init failed\n"); return 0; }
    }
    return 1;
}

/* ---- render targets ---------------------------------------------------- */

/* The most samples (up to the wanted number) the card supports for both colour and depth. */
static int msaa_supported(int want)
{
    int n;
    for (n = want; n > 1; n >>= 1) {
        UINT qc = 0, qd = 0;
        ID3D11Device_CheckMultisampleQualityLevels(s_dev, DXGI_FORMAT_B8G8R8A8_UNORM, (UINT)n, &qc);
        ID3D11Device_CheckMultisampleQualityLevels(s_dev, DXGI_FORMAT_D24_UNORM_S8_UINT, (UINT)n, &qd);
        if (qc > 0 && qd > 0) break;
    }
    return n < 1 ? 1 : n;
}

/* Draw srv into rtv. Every draw sets all the state it needs, so nothing is restored but the bindings. */
static void blit(ID3D11ShaderResourceView *srv, ID3D11RenderTargetView *rtv, uint32_t dw, uint32_t dh, uint32_t sw, uint32_t sh)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *nosrv = NULL;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_bl_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float *c = (float *)m.pData;
        c[0] = (float)sw; c[1] = (float)sh; c[2] = (float)sw / (float)dw; c[3] = (float)sh / (float)dh;
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_bl_cb, 0);
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &nosrv);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    vp.TopLeftX = vp.TopLeftY = 0; vp.Width = (float)dw; vp.Height = (float)dh; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_bl_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_bl_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_bl_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_bl_smp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &nosrv);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
}

static void rt_resolve(Rt *rt)
{
    if (rt->mtex && rt->msdirty) {
        ID3D11DeviceContext_ResolveSubresource(s_ctx, (ID3D11Resource *)rt->tex, 0, (ID3D11Resource *)rt->mtex, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        rt->msdirty = 0;
    }
}

/* What a staging copy reads: the guest-sized image of this target. */
static ID3D11Resource *rt_guest_image(Rt *rt)
{
    rt_resolve(rt);
    if (!rt->down) return (ID3D11Resource *)rt->tex;
    blit(rt->srv, rt->down_rtv, rt->w, rt->h, rt->w * (uint32_t)s_scale, rt->h * (uint32_t)s_scale);
    return (ID3D11Resource *)rt->down;
}

static void rt_release(Rt *r)
{
    SAFE_REL(r->rtv); SAFE_REL(r->srv); SAFE_REL(r->tex); SAFE_REL(r->stage); SAFE_REL(r->stage2);
    SAFE_REL(r->mtex); SAFE_REL(r->down); SAFE_REL(r->down_rtv);
    r->pend = 0; r->pstage = NULL; r->msdirty = 0; r->shown_dirty = 0; r->dirty = 0;
}

/* After a change of size, a target starts from what the game last saw in guest memory, not from nothing:
 * some are drawn once and sampled for a long time. */
static void rt_seed_from_guest(Rt *rt, const Nv2aSurface *s)
{
    extern int tex_range_ok_public(uint32_t va, uint32_t n);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = s->bytes_per_pixel, y, x;
    uint32_t *px;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *t = NULL;
    ID3D11ShaderResourceView *v = NULL;
    if ((bpp != 4 && bpp != 2) || s->pitch < s->width * bpp || !tex_range_ok_public(s->color_va, s->pitch * s->height)) return;
    px = (uint32_t *)malloc((size_t)s->width * s->height * 4);
    if (!px) return;
    for (y = 0; y < s->height; y++) {
        const uint8_t *row = mem + s->color_va + (size_t)y * s->pitch;
        if (bpp == 4) memcpy(px + (size_t)y * s->width, row, (size_t)s->width * 4);
        else for (x = 0; x < s->width; x++) {
            uint32_t c = ((const uint16_t *)row)[x];
            px[(size_t)y * s->width + x] = 0xFF000000u | (((c >> 11) & 31) * 255 / 31) << 16 | (((c >> 5) & 63) * 255 / 63) << 8 | ((c & 31) * 255 / 31);
        }
    }
    memset(&td, 0, sizeof td);
    td.Width = s->width; td.Height = s->height; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = px; sd.SysMemPitch = s->width * 4; sd.SysMemSlicePitch = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &t)) &&
        SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &v))) {
        blit(v, rt->rtv, s->width * (uint32_t)s_scale, s->height * (uint32_t)s_scale, s->width, s->height);
        rt->msdirty = rt->mtex != NULL;
    }
    SAFE_REL(v); SAFE_REL(t);
    free(px);
}

static Rt *get_rt(const Nv2aSurface *s)
{
    int i, free_i = -1, lru = 0;
    D3D11_TEXTURE2D_DESC td;
    uint32_t w = s->width, h = s->height;

    for (i = 0; i < MAX_RT; i++) {
        if (s_rt[i].tex && s_rt[i].va == s->color_va && s_rt[i].w == w && s_rt[i].h == h) {
            s_rt[i].used = s_tick; s_rt[i].pitch = s->pitch; s_rt[i].bpp = s->bytes_per_pixel;
            return &s_rt[i];
        }
        if (!s_rt[i].tex && free_i < 0) free_i = i;
        if (s_rt[i].tex && s_rt[i].used < s_rt[lru].used) lru = i;
    }
    if (free_i < 0) {
        free_i = lru;
        rt_release(&s_rt[free_i]);
    }
    s_rt[free_i].pend = 0; s_rt[free_i].pstage = NULL;
    memset(&td, 0, sizeof td);
    td.Width = w * (uint32_t)s_scale; td.Height = h * (uint32_t)s_scale; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rt[free_i].tex))) { rt_release(&s_rt[free_i]); return NULL; }
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_rt[free_i].tex, NULL, &s_rt[free_i].srv);
    if (s_msaa > 1) {
        td.SampleDesc.Count = (UINT)s_msaa; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rt[free_i].mtex))) { rt_release(&s_rt[free_i]); return NULL; }
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_rt[free_i].mtex, NULL, &s_rt[free_i].rtv);
        td.SampleDesc.Count = 1;
    } else {
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_rt[free_i].tex, NULL, &s_rt[free_i].rtv);
    }
    td.Width = w; td.Height = h;
    if (s_scale > 1) {
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rt[free_i].down))) { rt_release(&s_rt[free_i]); return NULL; }
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_rt[free_i].down, NULL, &s_rt[free_i].down_rtv);
    }
    s_rt[free_i].sampled = 0;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rt[free_i].stage);
    if (!s_rb_sync) ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rt[free_i].stage2);
    s_rt[free_i].va = s->color_va; s_rt[free_i].w = w; s_rt[free_i].h = h;
    s_rt[free_i].pitch = s->pitch; s_rt[free_i].bpp = s->bytes_per_pixel; s_rt[free_i].used = s_tick; s_rt[free_i].dirty = 0;
    s_rt[free_i].msdirty = 0; s_rt[free_i].shown_dirty = 0;
    if (s_resets) rt_seed_from_guest(&s_rt[free_i], s);
    if (s_verbose || getenv("RECOMP_GPU_LOGRT"))
        fprintf(stderr, "[GPUBE] render target va=%08X %ux%u bpp=%u pitch=%u aa=%ux%u\n", s->color_va, w, h,
                s->bytes_per_pixel, s->pitch, s->aa_sx, s->aa_sy);
    return &s_rt[free_i];
}

static Ds *get_ds(const Nv2aSurface *s, uint32_t zva)
{
    int i, slot = -1;
    D3D11_TEXTURE2D_DESC td;
    if (!zva) return NULL;
    for (i = 0; i < 4; i++)
        if (s_ds[i].tex && s_ds[i].va == zva && s_ds[i].w == s->width && s_ds[i].h == s->height) return &s_ds[i];
    for (i = 0; i < 4; i++) if (!s_ds[i].tex) { slot = i; break; }
    if (slot < 0) { slot = (int)(s_tick & 3); SAFE_REL(s_ds[slot].dsv); SAFE_REL(s_ds[slot].tex); }
    memset(&td, 0, sizeof td);
    td.Width = s->width * (uint32_t)s_scale; td.Height = s->height * (uint32_t)s_scale; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.SampleDesc.Count = (UINT)s_msaa; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ds[slot].tex))) return NULL;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)s_ds[slot].tex, NULL, &s_ds[slot].dsv);
    s_ds[slot].va = zva; s_ds[slot].w = s->width; s_ds[slot].h = s->height;
    return &s_ds[slot];
}

/* ---- state objects ------------------------------------------------------ */

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return alpha ? D3D11_BLEND_BLEND_FACTOR : D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    }
    return D3D11_BLEND_ONE;
}

static D3D11_BLEND_OP blend_op(uint32_t e)
{
    switch (e) {
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    }
    return D3D11_BLEND_OP_ADD;
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    }
    return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP sten_op(uint32_t o)
{
    switch (o) {
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    }
    return D3D11_STENCIL_OP_KEEP;
}

static void *find_state(StateEnt *tab, int *n, int max, const uint32_t *key, int nk)
{
    int i;
    for (i = 0; i < *n; i++)
        if (!memcmp(tab[i].key, key, nk * 4)) return tab[i].obj;
    (void)max;
    return NULL;
}

static ID3D11BlendState *get_blend(const Nv2aRenderState *r)
{
    uint32_t key[12] = { r->blend_enable ? 1 : 0, r->blend_src, r->blend_dst, r->blend_eq, r->color_mask };
    ID3D11BlendState *b = (ID3D11BlendState *)find_state(s_bs, &s_nbs, 96, key, 12);
    D3D11_BLEND_DESC d;
    if (b) return b;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = r->blend_enable ? TRUE : FALSE;
    d.RenderTarget[0].SrcBlend = blend_factor(r->blend_src, 0);
    d.RenderTarget[0].DestBlend = blend_factor(r->blend_dst, 0);
    d.RenderTarget[0].BlendOp = blend_op(r->blend_eq);
    d.RenderTarget[0].SrcBlendAlpha = blend_factor(r->blend_src, 1);
    d.RenderTarget[0].DestBlendAlpha = blend_factor(r->blend_dst, 1);
    d.RenderTarget[0].BlendOpAlpha = blend_op(r->blend_eq);
    d.RenderTarget[0].RenderTargetWriteMask =
        (uint8_t)(((r->color_mask & 0x00010000) ? 1 : 0) | ((r->color_mask & 0x100) ? 2 : 0) |
                  ((r->color_mask & 1) ? 4 : 0) | ((r->color_mask & 0x01000000) ? 8 : 0));
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &b))) return NULL;
    if (s_nbs < 96) { memcpy(s_bs[s_nbs].key, key, sizeof key); s_bs[s_nbs].obj = b; s_nbs++; }
    return b;
}

static ID3D11DepthStencilState *get_dss(const Nv2aRenderState *r, int have_ds)
{
    uint32_t key[12] = { have_ds, r->depth_test_enable ? 1 : 0, r->depth_func, r->depth_write ? 1 : 0,
                         r->stencil_enable ? 1 : 0, r->stencil_func, r->stencil_func_mask & 0xFF,
                         r->stencil_write_mask & 0xFF, r->stencil_fail, r->stencil_zfail, r->stencil_zpass,
                         (r->blend_enable && r->blend_src == 0x302 && r->blend_dst == 0x303) ? 1u : 0u };
    ID3D11DepthStencilState *d = (ID3D11DepthStencilState *)find_state(s_dss, &s_ndss, 96, key, 12);
    D3D11_DEPTH_STENCIL_DESC dd;
    D3D11_DEPTH_STENCILOP_DESC op;
    if (d) return d;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = (have_ds && r->depth_test_enable) ? TRUE : FALSE;
    dd.DepthWriteMask = r->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    {   /* RECOMP_GPU_BLEND_NOZWRITE=1 (experiment): alpha-blended draws (SRC_ALPHA / INV_SRC_ALPHA) do not write depth,
         * as the Dreamcast's translucent list never does. Ryu's Shinku Hadoken: 12 overlapping segment sprites. */
        static int nz = -1;
        if (nz < 0) nz = getenv("RECOMP_GPU_BLEND_NOZWRITE") != NULL;
        if (nz && r->blend_enable && r->blend_src == 0x302 && r->blend_dst == 0x303) dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    }
    dd.DepthFunc = cmp_func(r->depth_func);
    dd.StencilEnable = (have_ds && r->stencil_enable) ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)(r->stencil_func_mask & 0xFF);
    dd.StencilWriteMask = (UINT8)(r->stencil_write_mask & 0xFF);
    op.StencilFailOp = sten_op(r->stencil_fail);
    op.StencilDepthFailOp = sten_op(r->stencil_zfail);
    op.StencilPassOp = sten_op(r->stencil_zpass);
    op.StencilFunc = cmp_func(r->stencil_func);
    dd.FrontFace = op; dd.BackFace = op;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &dd, &d))) return NULL;
    if (s_ndss < 96) { memcpy(s_dss[s_ndss].key, key, sizeof key); s_dss[s_ndss].obj = d; s_ndss++; }
    return d;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_mode(uint32_t m)
{
    switch (m) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    case 5: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}

static ID3D11SamplerState *get_sampler(const Nv2aTexture *t)
{
    uint32_t minf = (t->filter >> 16) & 0xFF, magf = (t->filter >> 24) & 0xF;
    uint32_t key[12] = { minf == 1 ? 1u : 0u, magf == 1 ? 1u : 0u, t->addr_u, t->addr_v, (minf != 1 && magf != 1) ? (uint32_t)s_aniso : 1u };
    ID3D11SamplerState *s = (ID3D11SamplerState *)find_state(s_ss, &s_nss, 48, key, 12);
    D3D11_SAMPLER_DESC d;
    if (s) return s;
    memset(&d, 0, sizeof d);
    if (key[0] && key[1]) d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    else if (key[0]) d.Filter = D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    else if (key[1]) d.Filter = D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    else d.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    d.AddressU = addr_mode(t->addr_u); d.AddressV = addr_mode(t->addr_v); d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX; d.MaxAnisotropy = 1; d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (key[4] > 1) { d.Filter = D3D11_FILTER_ANISOTROPIC; d.MaxAnisotropy = key[4]; }
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &s))) return NULL;
    if (s_nss < 48) { memcpy(s_ss[s_nss].key, key, sizeof key); s_ss[s_nss].obj = s; s_nss++; }
    return s;
}

/* ---- textures ----------------------------------------------------------- */

static uint32_t tex_bytes(const Nv2aTexture *t)
{
    uint32_t px = t->width * t->height;
    switch (t->color) {
    case 0x0C: return px / 2 < 8 ? 8 : px / 2;                 /* DXT1 */
    case 0x0E: case 0x0F: return px < 16 ? 16 : px;            /* DXT3/5 */
    case 0x00: case 0x01: case 0x0B: case 0x19: return px;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1A: case 0x1B: return px * 2;
    }
    return px * 4;
}

static uint64_t hash_bytes(const uint8_t *p, uint32_t n)
{
    /* Four independent multiply lanes: the old single FNV-style chain ran at ~2 GB/s and was
     * ~10% of the executor thread; this is limited by memory bandwidth instead. */
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    uint64_t a = 0x243F6A8885A308D3ull, b = 0x13198A2E03707344ull, c = 0xA4093822299F31D0ull, d = 0x082EFA98EC4E6C89ull;
    const uint64_t *w = (const uint64_t *)p;
    uint32_t i, q = n / 32;
    for (i = 0; i < q; i++, w += 4) {
        a = (a ^ w[0]) * K; a ^= a >> 29;
        b = (b ^ w[1]) * K; b ^= b >> 29;
        c = (c ^ w[2]) * K; c ^= c >> 29;
        d = (d ^ w[3]) * K; d ^= d >> 29;
    }
    for (i = q * 32; i < n; i++) { a = (a ^ p[i]) * K; a ^= a >> 29; }
    a ^= (b << 17) | (b >> 47); a *= K;
    a ^= (c << 31) | (c >> 33); a *= K;
    a ^= (d << 7) | (d >> 57); a *= K;
    return a ^ (a >> 32);
}
static unsigned st_dec_us, st_crt_us;
static unsigned qpc_us(void);
/* Cheap content signature. Textures up to 32 KB (portraits, small P8 sheets: changed mid-frame in practice) get the full hash;
 * bigger ones: n/1024 spread words (64..512) + the last word, i.e. one word per KB, so any rewritten run >= 1 KB is seen. */
static uint64_t sample_sig(const uint8_t *p, uint32_t n)
{
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    const uint64_t *w = (const uint64_t *)p;
    uint32_t nw = n / 8, ns, step, i;
    uint64_t a = 0x452821E638D01377ull;
    if (n <= 32768) return hash_bytes(p, n);
    ns = n / 1024; if (ns < 64) ns = 64; if (ns > 512) ns = 512;
    step = nw / ns;
    for (i = 0; i < ns; i++) { a = (a ^ w[(size_t)i * step]) * K; a ^= a >> 29; }
    a = (a ^ w[nw - 1]) * K; a ^= a >> 29;
    return a;
}
/* A texture read must land in mapped guest memory. A garbage offset (seen once during a stage load: 0xEED36461) faults here,
 * where the real GPU would only sample rubbish for a frame. Windows: 64 MB of RAM, its contiguous and tiled views. */
static int tex_range_ok(uint32_t va, uint32_t n)
{
    static const uint32_t base[3] = { 0x00000000u, 0x80000000u, 0xF0000000u };
    const uint32_t size = 0x04000000u;
    int i;
    for (i = 0; i < 3; i++)
        if (va - base[i] < size && n <= size - (va - base[i])) return 1;
    return 0;
}
int tex_range_ok_public(uint32_t va, uint32_t n) { return tex_range_ok(va, n); }
static int tex_ok(const Nv2aTexture *t)
{
    static LONG nlog;
    if (t->width && t->width <= 4096 && t->height && t->height <= 4096 && tex_range_ok(t->offset, tex_bytes(t))
        && (t->color != 0x0B || !t->palette || tex_range_ok(t->palette, 1024))) return 1;
    if (InterlockedIncrement(&nlog) <= 20)
        fprintf(stderr, "[GPUBE] bad texture skipped: offset %08X palette %08X %ux%u fmt %02X\n", t->offset, t->palette, t->width, t->height, t->color);
    return 0;
}
static int s_gen_mips;
static ID3D11ShaderResourceView *get_tex_slow(const Nv2aTexture *t)
{
    int i, free_i = -1, lru = 0;
    uint64_t h, th;
    uint32_t n = tex_bytes(t);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    LARGE_INTEGER a, b, f;

    if (!tex_ok(t)) return s_white_srv;
    s_noalpha = (t->color == 0x1E || t->color == 0x1F || t->color == 0x07) ? 1 : 0;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].srv && s_rt[i].va == t->offset && s_rt[i].w == t->width && s_rt[i].h == t->height) {
            s_rt[i].sampled = s_frame + 1; st_rtsamples++;
            rt_resolve(&s_rt[i]);
            return s_rt[i].srv;
        }
    QueryPerformanceCounter(&a);
    th = hash_bytes(mem + t->offset, n);
    if (t->color == 0x0B && t->palette) h = th ^ (hash_bytes(mem + t->palette, 1024) * 31u);
    else h = th;
    st_th_full++;
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    st_hash_ns_us += (uint32_t)((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart);
    for (i = 0; i < MAX_TEX; i++) {
        TexEnt *e = &s_tx[i];
        if (!e->tex) { if (free_i < 0) free_i = i; continue; }
        if (e->offset == t->offset && e->w == t->width && e->h == t->height && e->color == t->color && e->pitch == t->pitch) {
            e->used = s_tick;
            { static int nc = -1; if (nc < 0) nc = getenv("RECOMP_GPU_NOTEXCACHE") ? 1 : 0; /* A/B switch: re-upload every draw */
              if (!nc && e->hash == h) { e->thash = th; e->sig = sample_sig(mem + t->offset, n); e->vframe = s_frame + 1; return e->srv; } }
            free_i = i;                    /* same slot, new content */
            DEFER_REL(e->srv); DEFER_REL(e->tex);
            goto decode;
        }
        if (e->used < s_tx[lru].used) lru = i;
    }
    if (free_i < 0) { free_i = lru; DEFER_REL(s_tx[free_i].srv); DEFER_REL(s_tx[free_i].tex); }
decode:
    {
        uint32_t *argb = (uint32_t *)malloc((size_t)t->width * t->height * 4);
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd;
        TexEnt *e = &s_tx[free_i];
        if (!argb) return s_white_srv;
        unsigned q0 = qpc_us(), q1;
        if (!argb) return s_white_srv;
        if (!nv2a_backend_decode_texture(t, argb)) { fprintf(stderr, "[GPUTR] decode FAILED fmt=%02X %ux%u\n", t->color, t->width, t->height); free(argb); e->tex = NULL; return s_white_srv; }
        q1 = qpc_us(); st_dec_us += q1 - q0;
        memset(&td, 0, sizeof td);
        td.Width = t->width; td.Height = t->height; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = argb; sd.SysMemPitch = t->width * 4; sd.SysMemSlicePitch = 0;
        if (s_flush_tex) ID3D11DeviceContext_Flush(s_ctx);
        if (s_aniso > 1 && t->width > 1 && t->height > 1) {   /* the anisotropic filter needs mip levels to work from */
            td.MipLevels = 0; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags |= D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex))) {
                ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, 0, NULL, argb, t->width * 4, 0);
                s_gen_mips = 1;
            }
        }
        if (!e->tex && FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &e->tex))) { fprintf(stderr, "[GPUBE] CreateTexture2D failed %ux%u fmt=%02X\n", t->width, t->height, t->color); free(argb); e->tex = NULL; return s_white_srv; }
        { static int tl = -1; static unsigned nlog;
          if (tl < 0) tl = getenv("RECOMP_GPU_TEXLOG") ? 1 : 0;
          if (tl && nlog < 400) { nlog++;
              fprintf(stderr, "[TEXLOG] f%u new tex off=%08X %ux%u fmt=%02X pitch=%u pal=%08X hash=%016llX", s_frame, t->offset, t->width, t->height, t->color, t->pitch, t->palette, (unsigned long long)h);
              if (t->color == 0x0B && t->palette) { const uint32_t *pp = (const uint32_t *)(mem + t->palette); unsigned a0 = 0, aw = 0, k; for (k = 0; k < 256; k++) { if ((pp[k] >> 24) == 0) a0++; if (pp[k] == 0xFFFFFFFFu) aw++; } fprintf(stderr, " pal: alpha0=%u white=%u e0=%08X e1=%08X e255=%08X palhash=%016llX", a0, aw, pp[0], pp[1], pp[255], (unsigned long long)hash_bytes(mem + t->palette, 1024)); }
              { unsigned a0 = 0, aw = 0; size_t k, np = (size_t)t->width * t->height; for (k = 0; k < np; k++) { if ((argb[k] >> 24) == 0) a0++; else if (argb[k] == 0xFFFFFFFFu) aw++; } fprintf(stderr, " decoded: alpha0=%u%% white=%u%%\n", (unsigned)(a0 * 100ull / np), (unsigned)(aw * 100ull / np)); }
          } }
        free(argb);
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
        if (s_gen_mips) { s_gen_mips = 0; ID3D11DeviceContext_GenerateMips(s_ctx, e->srv); }
        if (s_flush_tex) ID3D11DeviceContext_Flush(s_ctx);
        e->offset = t->offset; e->w = t->width; e->h = t->height; e->color = t->color; e->pitch = t->pitch;
        e->hash = h; e->used = s_tick;
        e->thash = th; e->sig = sample_sig(mem + t->offset, n); e->vframe = s_frame + 1;
        st_crt_us += qpc_us() - q1;
        st_decodes++;
        return e->srv ? e->srv : s_white_srv;
    }
}

/* RECOMP_GPU_TEXDUMP=<dir>: while the RECOMP_FB_DUMP_FLIPS flag file exists, write every distinct texture
 * (offset + content + palette) a draw samples as <dir>\t<offset>_<pal>_<fmt>_<w>x<h>_<hash>.bmp, decoded the way the
 * renderer sees it. For texture bugs: compare against what the scene should show. Checked once per flip. */
static int s_texdump_on;
static void texdump_poll(void)
{
    static const char *dir = (const char *)-1, *flag;
    FILE *f;
    if (dir == (const char *)-1) { dir = getenv("RECOMP_GPU_TEXDUMP"); flag = getenv("RECOMP_FB_DUMP_FLIPS"); }
    if (!dir || !flag) return;
    f = fopen(flag, "rb");
    s_texdump_on = f != NULL;
    if (f) fclose(f);
}
static void texdump(const Nv2aTexture *t)
{
    static uint64_t seen[1024]; static unsigned nseen;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint64_t h = hash_bytes(mem + t->offset, tex_bytes(t));
    uint32_t *argb, k, hdr[13];
    char path[512];
    FILE *f;
    if (t->color == 0x0B && t->palette) h ^= hash_bytes(mem + t->palette, 1024) * 31u;
    for (k = 0; k < nseen; k++) if (seen[k] == h) return;
    if (nseen == 1024) return;
    seen[nseen++] = h;
    if (!(argb = (uint32_t *)malloc((size_t)t->width * t->height * 4))) return;
    if (!nv2a_backend_decode_texture(t, argb)) { free(argb); return; }
    snprintf(path, sizeof path, "%s\\t%08X_%08X_%02X_%ux%u_%08X.bmp", getenv("RECOMP_GPU_TEXDUMP"), t->offset, t->palette,
             t->color, t->width, t->height, (uint32_t)h);
    if ((f = fopen(path, "wb")) != NULL) {
        uint32_t img = t->width * t->height * 4;
        fputc('B', f); fputc('M', f);
        hdr[0] = 54 + img; hdr[1] = 0; hdr[2] = 54; hdr[3] = 40; hdr[4] = t->width; hdr[5] = (uint32_t)-(int32_t)t->height;
        hdr[6] = 1 | 32 << 16; hdr[7] = 0; hdr[8] = img; hdr[9] = hdr[10] = 2835; hdr[11] = hdr[12] = 0;
        fwrite(hdr, 4, 13, f);
        fwrite(argb, 4, (size_t)t->width * t->height, f);
        fclose(f);
    }
    free(argb);
}

/* Fast path (RECOMP_GPU_TEXHASH=0 turns it off = every draw hashes the full texture bytes, the old behaviour).
 * A texture whose bytes were fully hashed earlier in this same flip interval is re-checked with sample_sig() only
 * (the palette is still hashed in full: palette animation changes it between draws). RECOMP_GPU_TEXHASH_VERIFY=1
 * also computes the full hash on every fast hit and counts disagreements (thstale in [GPUBE]); the result is then exact. */
static ID3D11ShaderResourceView *get_tex(const Nv2aTexture *t)
{
    static int mode = -1, verify = 0;
    static uint16_t idx[256];
    uint32_t n, k;
    int i;
    const uint8_t *mem;
    TexEnt *e = NULL;
    if (!tex_ok(t)) return s_white_srv;
    if (mode < 0) { const char *m = getenv("RECOMP_GPU_TEXHASH"); mode = !(m && m[0] == '0'); verify = getenv("RECOMP_GPU_TEXHASH_VERIFY") != NULL; }
    if (s_texdump_on && t->width && t->height && t->width <= 4096 && t->height <= 4096) texdump(t);
    if (!mode || !t->width || !t->height || t->width > 4096 || t->height > 4096) return get_tex_slow(t);
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].srv && s_rt[i].va == t->offset && s_rt[i].w == t->width && s_rt[i].h == t->height) return get_tex_slow(t);
    k = ((t->offset >> 4) * 2654435761u ^ t->width * 40503u ^ t->height * 9973u ^ t->color) >> 8 & 255;
    {
        TexEnt *c = &s_tx[idx[k]];
        if (c->tex && c->offset == t->offset && c->w == t->width && c->h == t->height && c->color == t->color && c->pitch == t->pitch) e = c;
    }
    if (!e) {
        for (i = 0; i < MAX_TEX; i++) {
            TexEnt *c = &s_tx[i];
            if (c->tex && c->offset == t->offset && c->w == t->width && c->h == t->height && c->color == t->color && c->pitch == t->pitch) { e = c; idx[k] = (uint16_t)i; break; }
        }
    }
    if (!e || e->vframe != s_frame + 1 || !e->srv) {
        ID3D11ShaderResourceView *r = get_tex_slow(t);
        for (i = 0; i < MAX_TEX; i++) { /* slot may be new: refresh the index */
            TexEnt *c = &s_tx[i];
            if (c->tex && c->srv == r) { idx[k] = (uint16_t)i; break; }
        }
        return r;
    }
    n = tex_bytes(t); mem = (const uint8_t *)xbox_GetMemoryOffset();
    {
        uint64_t h = e->thash;
        if (t->color == 0x0B && t->palette) h ^= hash_bytes(mem + t->palette, 1024) * 31u;
        if (sample_sig(mem + t->offset, n) == e->sig && h == e->hash) {
            if (verify) {
                uint64_t fh = hash_bytes(mem + t->offset, n);
                if (t->color == 0x0B && t->palette) fh ^= hash_bytes(mem + t->palette, 1024) * 31u;
                if (fh != e->hash) { static unsigned nl; st_th_stale++; if (nl++ < 40) fprintf(stderr, "[TEXV] stale f%u off=%08X %ux%u fmt=%02X\n", s_frame, t->offset, t->width, t->height, t->color); e->vframe = 0; return get_tex_slow(t); }
            }
            e->used = s_tick; st_th_fast++;
            { static int nc = -1; if (nc < 0) nc = getenv("RECOMP_GPU_NOTEXCACHE") ? 1 : 0; if (nc) return get_tex_slow(t); }
            s_noalpha = (t->color == 0x1E || t->color == 0x1F || t->color == 0x07) ? 1 : 0;
            return e->srv;
        }
    }
    e->vframe = 0;
    return get_tex_slow(t);
}

/* ---- watchdog (debug): reports when the executor stops calling the back end --- */
static unsigned st_us_clear, st_us_draw, st_us_flip;
typedef struct { uint64_t vh, th; float x0, y0; uint32_t n, w, h, fmt, c0, st; } FdRec;
static FdRec fd_cur[256], fd_prev[256]; static unsigned fd_pn, fd_lines;
extern uint32_t g_dbg_begin, g_dbg_ilf, g_dbg_dpn, g_dbg_dp, g_dbg_trunc, g_dbg_batches, g_dbg_flush0, g_dbg_xffail, g_dbg_short;
static int s_fh = -1; static uint64_t fh_dh, fh_rth; static unsigned fh_nd, fh_nc;
static volatile LONG s_stage;          /* 0 idle/outside, 1 clear, 2 draw, 3 readback, 4 flip */
static volatile DWORD s_stage_tick;
uint32_t mvc2_gpu_flips(void) { return s_frame; } /* for the stall dumper in main.c */
static volatile LONG s_calls;
static void gb_mark(LONG st) { s_stage = st; s_stage_tick = GetTickCount(); s_calls++; }
static void gb_freeze_dump(int pass);
static DWORD WINAPI gb_watchdog(LPVOID p)
{
    LONG lastcalls = -1; int quiet = 0, dumps = 0; DWORD t0 = GetTickCount(); (void)p;
    for (;;) {
        Sleep(3000);
        if (s_calls == lastcalls) {
            fprintf(stderr, "[GPUBE] WATCHDOG: no backend call for >=3 s; stage=%ld (0 idle,1 clear,2 draw,3 readback,4 flip) for %lu ms; calls=%ld\n",
                    (long)s_stage, (unsigned long)(GetTickCount() - s_stage_tick), (long)s_calls);
            /* Stuck >= 6 s past boot (> 1000 backend calls, or up > 40 s, which catches a stall soon after the logos):
             * every thread's stack, twice 6 s apart, so a spin and a block look different. */
            if (++quiet >= 2 && (s_calls > 1000 || GetTickCount() - t0 > 40000) && dumps < 2 && (quiet % 2) == 0) gb_freeze_dump(++dumps);
        } else quiet = 0;
        lastcalls = s_calls;
    }
    return 0;
}
/* ---- Nv2aBackend -------------------------------------------------------- */

static void gb_clear_impl(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags, uint32_t argb, uint32_t zst)
{
    Rt *rt;
    Ds *ds;
    if (!gb_ready()) return;
    if (s_trace_all && s_frame >= s_ta_lo && s_frame <= s_ta_hi && s_trace_lines < 3000 && ta_new(0xC1EA0000ull ^ ((uint64_t)flags << 32) ^ argb ^ ((uint64_t)s->color_va << 8))) { s_trace_lines++; fprintf(stderr, "[GPUTA] f%u CLEAR va=%08X %ux%u flags=%X argb=%08X zst=%08X\n", s_frame, s->color_va, s->width, s->height, flags, argb, zst); }
    rt = get_rt(s);
    if (!rt) return;
    if (flags & 0xF0) {
        float c[4];
        c[0] = ((argb >> 16) & 255) / 255.0f; c[1] = ((argb >> 8) & 255) / 255.0f;
        c[2] = (argb & 255) / 255.0f; c[3] = ((argb >> 24) & 255) / 255.0f;
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rt->rtv, c);
        rt->dirty = 1; rt->msdirty = 1; rt->shown_dirty = 1;
    }
    if ((flags & 3) && (ds = get_ds(s, rs->zeta_va)) != NULL) {
        UINT f = 0;
        if (flags & 1) f |= D3D11_CLEAR_DEPTH;
        if (flags & 2) f |= D3D11_CLEAR_STENCIL;
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, ds->dsv, f, (float)(zst >> 8) / 16777215.0f, (UINT8)(zst & 0xFF));
    }
    st_clears++;
}

static int is_front(float area, uint32_t front_face)
{
    /* y grows downwards in surface pixels, so a positive area is clockwise. */
    return front_face == 0x901 ? area < 0.0f : area > 0.0f;
}

/* RECOMP_GPU_TEXTDRAW diagnostic: after a glyph draw, read the GPU texture copy and the rendered rectangle back and log statistics, so a
 * driver-specific sampling/blend/alpha-test failure shows up as numbers (e.g. the glyph rectangle coming out solid white). */
static void probe_after_draw(Rt *rt, ID3D11ShaderResourceView *srv, const Nv2aBatch *b)
{
    D3D11_MAPPED_SUBRESOURCE m;
    int x0 = 100000, y0 = 100000, x1 = -100000, y1 = -100000, x, y;
    uint32_t i;
    ID3D11Resource *res = NULL;
    for (i = 0; i < b->count && i < 6; i++) { int vx = (int)b->vertices[i].x, vy = (int)b->vertices[i].y; if (vx < x0) x0 = vx; if (vx > x1) x1 = vx; if (vy < y0) y0 = vy; if (vy > y1) y1 = vy; }
    if (srv && srv != s_white_srv) {
        static ID3D11Resource *seen[8]; static int nseen; int dup = 0, k;
        ID3D11ShaderResourceView_GetResource(srv, &res);
        for (k = 0; k < nseen; k++) if (seen[k] == res) dup = 1;
        if (res && !dup && nseen < 8) {
            D3D11_TEXTURE2D_DESC d; ID3D11Texture2D *st = NULL;
            seen[nseen++] = res;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &st))) {
                ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, res);
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
                    unsigned a0 = 0, aw = 0, other = 0; uint32_t n = d.Width * d.Height;
                    for (y = 0; y < (int)d.Height; y++) { const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
                        for (x = 0; x < (int)d.Width; x++) { uint32_t p = row[x]; if ((p >> 24) == 0) a0++; else if (p == 0xFFFFFFFFu) aw++; else other++; } }
                    fprintf(stderr, "[TEXPROBE] gpu texture copy %ux%u fmt=%u: alpha0=%u%% white=%u%% shaded=%u%%\n", d.Width, d.Height, (unsigned)d.Format, a0 * 100u / n, aw * 100u / n, other * 100u / n);
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
                }
                ID3D11Texture2D_Release(st);
            }
        }
        SAFE_REL(res);
    }
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)rt->stage, rt_guest_image(rt));
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)rt->stage, 0, D3D11_MAP_READ, 0, &m))) {
        unsigned nw = 0, n = 0, nz = 0; uint32_t first[4] = { 0, 0, 0, 0 };
        for (y = y0; y < y1; y++) for (x = x0; x < x1; x++) {
            uint32_t p = ((const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch))[x];
            if (n < 4) first[n] = p; n++; if ((p & 0xFFFFFF) == 0xFFFFFF) nw++; if (p == 0) nz++;
        }
        fprintf(stderr, "[TEXPROBE] rendered rect (%d,%d)-(%d,%d): %u px, white=%u (%u%%) zero=%u first=%08X %08X %08X %08X\n", x0, y0, x1, y1, n, nw, n ? nw * 100u / n : 0, nz, first[0], first[1], first[2], first[3]);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)rt->stage, 0);
    }
}

static void gb_draw_impl(const Nv2aSurface *s, const Nv2aBatch *b)
{
    Rt *rt;
    Ds *ds;
    const Nv2aRenderState *r = b->state;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    uint32_t i, kept = 0, need;
    Nv2aVertex *dst;
    Cb cb;
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState *smp = NULL;
    ID3D11RenderTargetView *rtv;
    float bf[4];
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *dss;
    UINT stride = sizeof(Nv2aVertex), off = 0;

    if (!gb_ready() || !b->count) return;
    if (s_fh < 0) s_fh = getenv("RECOMP_GPU_FRAMEHASH") != NULL;
    if (s_fh > 0) { uint32_t k; uint64_t h = fh_dh * 1099511628211ULL + b->count; for (k = 0; k < b->count; k++) { const Nv2aVertex *v = &b->vertices[k]; h = (h ^ (uint64_t)(int)(v->x * 16)) * 1099511628211ULL; h = (h ^ (uint64_t)(int)(v->y * 16)) * 1099511628211ULL; h = (h ^ v->diffuse) * 1099511628211ULL; h = (h ^ (uint64_t)(int)(v->u * 16)) * 1099511628211ULL; h = (h ^ (uint64_t)(int)(v->v * 16)) * 1099511628211ULL; } if (b->texture) { uint32_t n = tex_bytes(b->texture); h = (h ^ hash_bytes((const uint8_t *)xbox_GetMemoryOffset() + b->texture->offset, n)) * 1099511628211ULL; } h = (h ^ (r->blend_enable * 7u + r->blend_src * 131u + r->blend_dst * 977u + r->alpha_test_enable * 13u + r->depth_test_enable * 17u + r->color_mask)) * 1099511628211ULL; { uint64_t vh = 1469598103934665603ULL, th = 0; for (k = 0; k < b->count; k++) { const Nv2aVertex *v = &b->vertices[k]; vh = (vh ^ (uint64_t)(int)(v->x * 16)) * 1099511628211ULL; vh = (vh ^ (uint64_t)(int)(v->y * 16)) * 1099511628211ULL; vh = (vh ^ v->diffuse) * 1099511628211ULL; vh = (vh ^ (uint64_t)(int)(v->u * 16)) * 1099511628211ULL; vh = (vh ^ (uint64_t)(int)(v->v * 16)) * 1099511628211ULL; } if (b->texture) th = hash_bytes((const uint8_t *)xbox_GetMemoryOffset() + b->texture->offset, tex_bytes(b->texture)); if (fh_nd < 256) { FdRec *q = &fd_cur[fh_nd]; q->vh = vh; q->th = th; q->x0 = b->vertices[0].x; q->y0 = b->vertices[0].y; q->n = b->count; q->w = b->texture ? b->texture->width : 0; q->h = b->texture ? b->texture->height : 0; q->fmt = b->texture ? b->texture->color : 0; q->c0 = b->vertices[0].diffuse; q->st = r->blend_enable * 1000 + r->blend_src; } } fh_dh = h; fh_nd++; }
    { static int gr = -1; if (gr < 0) gr = getenv("RECOMP_GPU_GRAD") != NULL; if (gr && b->count == 12 && !b->texture && (s_frame % 10) == 0) fprintf(stderr, "[GRAD] f%u y0=%.1f c0=%08X y2=%.1f c2=%08X y8=%.1f c8=%08X z=%.0f rhw=%g bl=%u %X/%X\n", s_frame, b->vertices[0].y, b->vertices[0].diffuse, b->vertices[2].y, b->vertices[2].diffuse, b->vertices[8].y, b->vertices[8].diffuse, b->vertices[0].z, b->vertices[0].rhw, b->state->blend_enable, b->state->blend_src, b->state->blend_dst); }
    if (s_trace && (s_frame % 150) == 60) {
        const Nv2aVertex *v0 = &b->vertices[0];
        fprintf(stderr, "[GPUTR] f%u va=%08X n=%u tex=%s %ux%u fmt=%02X off=%08X pitch=%u lv=%u | v0 (%.1f,%.1f,z%.0f,rhw%g) c=%08X uv=(%.1f,%.1f) | bl=%u %X/%X/%X dt=%u/%X/%u at=%u/%X/%u cm=%08X cull=%u/%X/%X zeta=%08X st=%u\n",
                s_frame, s->color_va, b->count, b->texture ? "Y" : "n", b->texture ? b->texture->width : 0, b->texture ? b->texture->height : 0,
                b->texture ? b->texture->color : 0, b->texture ? b->texture->offset : 0, b->texture ? b->texture->pitch : 0, b->texture ? b->texture->levels : 0,
                v0->x, v0->y, v0->z, v0->rhw, v0->diffuse, v0->u, v0->v,
                b->state->blend_enable, b->state->blend_src, b->state->blend_dst, b->state->blend_eq,
                b->state->depth_test_enable, b->state->depth_func, b->state->depth_write,
                b->state->alpha_test_enable, b->state->alpha_func, b->state->alpha_ref, b->state->color_mask,
                b->state->cull_enable, b->state->cull_face, b->state->front_face, b->state->zeta_va, b->state->stencil_enable);
    }
    if (s_trace_all && s_frame >= s_ta_lo && s_frame <= s_ta_hi && s_trace_lines < 3000 &&
        ta_new(((uint64_t)(b->texture ? b->texture->color + 1 : 0) << 56) ^ ((uint64_t)(b->texture ? b->texture->width : 0) << 40) ^ ((uint64_t)b->count << 24) ^
               ((uint64_t)r->blend_enable << 23) ^ ((uint64_t)r->blend_src << 12) ^ ((uint64_t)r->blend_dst << 3) ^ ((uint64_t)r->depth_test_enable << 22) ^ ((uint64_t)r->alpha_test_enable << 21) ^ ((uint64_t)r->color_mask * 2654435761u) ^ ((uint64_t)b->vertices[0].diffuse * 40503u) ^ ((uint64_t)s->color_va << 4))) {
        uint32_t k;
        s_trace_lines++;
        fprintf(stderr, "[GPUTA] f%u DRAW va=%08X %ux%u n=%u tex=%s fmt=%02X %ux%u bl=%u %X/%X/%X dt=%u/%X/%u at=%u/%X/%u cm=%08X\n", s_frame, s->color_va, s->width, s->height, b->count,
                b->texture ? "Y" : "n", b->texture ? b->texture->color : 0, b->texture ? b->texture->width : 0, b->texture ? b->texture->height : 0,
                r->blend_enable, r->blend_src, r->blend_dst, r->blend_eq, r->depth_test_enable, r->depth_func, r->depth_write,
                r->alpha_test_enable, r->alpha_func, r->alpha_ref, r->color_mask);
        for (k = 0; k < b->count && k < (b->count == 12 ? 12 : 6); k++)
            fprintf(stderr, "[GPUTA]    v%u (%.1f,%.1f,z%.0f,rhw%g) c=%08X uv=(%.1f,%.1f)\n", k, b->vertices[k].x, b->vertices[k].y, b->vertices[k].z, b->vertices[k].rhw, b->vertices[k].diffuse, b->vertices[k].u, b->vertices[k].v);
    }
    rt = get_rt(s);
    if (!rt) return;
    ds = get_ds(s, r->zeta_va);
    need = b->count * (uint32_t)sizeof(Nv2aVertex);
    if (need > VB_BYTES) return;
    if (s_vb_off + need > VB_BYTES) { s_vb_off = 0; s_vb_i = (s_vb_i + 1) % VB_RING; s_vb = s_vbr[s_vb_i]; }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
               s_vb_off == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)))
        return;
    dst = (Nv2aVertex *)((uint8_t *)m.pData + s_vb_off);
    for (i = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertex *v = &b->vertices[i];
        float area;
        if (!(v[0].rhw > 0 && v[1].rhw > 0 && v[2].rhw > 0)) continue;
        area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[1].y - v[0].y) * (v[2].x - v[0].x);
        if (!(fabsf(area) > 0.0f) || !isfinite(area)) continue;
        if (s_cull_on && r->cull_enable) {
            int front = is_front(area, r->front_face);
            if ((r->cull_face == 0x405 && !front) || (r->cull_face == 0x404 && front) || r->cull_face == 0x408) {
                st_culled++;
                continue;
            }
        }
        dst[kept] = v[0]; dst[kept + 1] = v[1]; dst[kept + 2] = v[2];
        kept += 3;
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    if (!kept) return;

    memset(&cb, 0, sizeof cb);
    cb.sw = (float)s->width; cb.sh = (float)s->height;
    cb.textured = 0; cb.afunc = r->alpha_test_enable ? r->alpha_func : 0x207; cb.aref = (float)(r->alpha_ref & 0xFF);
    srv = s_white_srv;
    if (b->texture) {
        srv = get_tex(b->texture);
        smp = get_sampler(b->texture);
        cb.textured = (srv != s_white_srv) ? 1 : 0; cb.pad = (s_noalpha ? 1u : 0u) | ((b->texture->use_mask & 1) ? 0u : 2u) | ((b->texture->use_mask & 2) ? 0u : 4u);
        cb.tw = (float)b->texture->width; cb.th = (float)b->texture->height;
    }
    if (!smp) { Nv2aTexture dflt; memset(&dflt, 0, sizeof dflt); dflt.addr_u = dflt.addr_v = 3; smp = get_sampler(&dflt); }
    int td_probe = 0;
    { static int tdm = -1; static unsigned tdn, tdf, tdo;   /* RECOMP_GPU_TEXTDRAW=N: dump the first N draws that sample a P8 texture (glyph quads) */
      if (tdm < 0) { const char *e = getenv("RECOMP_GPU_TEXTDRAW"); tdm = e ? atoi(e) : 0; { const char *ef = getenv("RECOMP_GPU_TEXTDRAW_FROM"); tdf = ef ? (unsigned)atoi(ef) : 0; ef = getenv("RECOMP_GPU_TEXTDRAW_OFF"); tdo = ef ? (unsigned)strtoul(ef, NULL, 16) : 0; } }
      if (tdn < (unsigned)tdm && b->texture && b->texture->color == 0x0B && b->texture->width == 512 && s_frame >= tdf && (!tdo || tdo == b->texture->offset)) { uint32_t k, nv = b->count < 12 ? b->count : 12; tdn++; td_probe = 1;
          fprintf(stderr, "[TEXTDRAW] #%u f%u n=%u rt=%08X %ux%u tex off=%08X %ux%u pal=%08X filt=%08X au=%u av=%u use_mask=%u textured=%u white_srv=%d cb.pad=%u aref=%g afunc=%X | bl=%u %X/%X/%X at=%u/%X/%u dt=%u cm=%08X\n", tdn, s_frame, b->count, s->color_va, s->width, s->height,
                  b->texture->offset, b->texture->width, b->texture->height, b->texture->palette, (unsigned)b->texture->filter, (unsigned)b->texture->addr_u, (unsigned)b->texture->addr_v, (unsigned)b->texture->use_mask, (unsigned)cb.textured, srv == s_white_srv, cb.pad, cb.aref, cb.afunc,
                  r->blend_enable, r->blend_src, r->blend_dst, r->blend_eq, r->alpha_test_enable, r->alpha_func, r->alpha_ref, r->depth_test_enable, r->color_mask);
          for (k = 0; k < nv; k++) { const Nv2aVertex *v = &b->vertices[k]; fprintf(stderr, "[TEXTDRAW]    v%u (%.2f,%.2f,z%.0f,rhw%g) c=%08X uv=(%.3f,%.3f)\n", k, v->x, v->y, v->z, v->rhw, v->diffuse, v->u, v->v); } } }
    ID3D11Buffer *cbuf = s_cbr[s_cb_i++ % CB_RING];
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof cb);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)cbuf, 0);
    }
    bs = get_blend(r);
    dss = get_dss(r, ds != NULL);
    bf[0] = ((r->blend_color >> 16) & 255) / 255.0f; bf[1] = ((r->blend_color >> 8) & 255) / 255.0f;
    bf[2] = (r->blend_color & 255) / 255.0f; bf[3] = ((r->blend_color >> 24) & 255) / 255.0f;
    rtv = rt->rtv;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, ds ? ds->dsv : NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dss, r->stencil_ref & 0xFF);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    /* Pixel-centre shift, default +0.5 px (RECOMP_GPU_PIXOFS=<px> overrides, 0 = the old mapping). Without it the SCORE
     * ATTACK label's quad sampled atlas row 96, where the "Xbox" line's outline starts, and drew a black line under "SC";
     * the full-screen copy quad (0.5..640.5 onto texels 0..639) also only lands on texel centres with it (menu text measured
     * ~5% sharper). So the NV2A samples like D3D9 (centres on integers) for these pre-transformed draws. */
    { static float po = -1.0f; if (po < 0) { const char *e = getenv("RECOMP_GPU_PIXOFS"); po = e ? (float)atof(e) : 0.5f; if (po < 0) po = 0; }
      vp.TopLeftX = vp.TopLeftY = po * (float)s_scale; }
    vp.Width = (float)(s->width * (uint32_t)s_scale); vp.Height = (float)(s->height * (uint32_t)s_scale); vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_il);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &smp);
    ID3D11DeviceContext_Draw(s_ctx, kept, s_vb_off / (UINT)sizeof(Nv2aVertex));
    if (s_flush_mode && (s_flush_mode >= 3 || (cb.textured && (s_flush_mode >= 2 || (b->texture && b->texture->color == 0x0B && b->texture->width == 512))))) ID3D11DeviceContext_Flush(s_ctx);
    if (td_probe) probe_after_draw(rt, srv, b);
    s_vb_off += kept * (uint32_t)sizeof(Nv2aVertex);
    rt->dirty = 1; rt->msdirty = 1; rt->shown_dirty = 1;
    st_draws++; st_tris += kept / 3;
}

/* Map a staging copy and write it into guest memory at va (w x h, pitch, bpp 4 or 2). */
/* The largest render target written back to guest memory since the last flip: the frame the window shows. The debug
 * build's mark screenshots read it (MVC2_DEBUG_ON_FLIP); the NV2A scan-out register is never set in this emulation. */
static uint32_t s_shot_va, s_shot_w, s_shot_h, s_shot_pitch, s_shot_bpp;

static void rb_write(ID3D11Texture2D *st, uint32_t va, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t y, x;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) return;
    if (w * h >= s_shot_w * s_shot_h) { s_shot_va = va; s_shot_w = w; s_shot_h = h; s_shot_pitch = pitch; s_shot_bpp = bpp; }
    if (s_fh > 0) { uint32_t yy; uint64_t hh = 1469598103934665603ULL; for (yy = 0; yy < h; yy++) hh = (hh ^ hash_bytes((const uint8_t *)m.pData + (size_t)yy * m.RowPitch, w * 4)) * 1099511628211ULL; fh_rth ^= hh; }
    if (bpp == 4 && pitch >= w * 4) {
        for (y = 0; y < h; y++)
            memcpy(mem + va + (size_t)y * pitch, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
    } else if (bpp == 2 && pitch >= w * 2) {
        for (y = 0; y < h; y++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            uint16_t *d = (uint16_t *)(mem + va + (size_t)y * pitch);
            for (x = 0; x < w; x++) {
                uint32_t c = src[x];
                d[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            }
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
}

/* RECOMP_GPU_SYNC_RB=1: copy and map in the same flip (the CPU waits for the GPU to finish the frame). */
static void readback(Rt *rt)
{
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)rt->stage, rt_guest_image(rt));
    rb_write(rt->stage, rt->va, rt->w, rt->h, rt->pitch, rt->bpp);
    rt->dirty = 0;
}

/* Default, deferred by one flip: queue the copy now (into the staging texture the last copy did not use) and write it
 * to guest memory at the next flip, when the GPU has long finished. Mapping right after the copy made the pushbuffer
 * thread wait ~3.4 ms per flip for the GPU (~400 ms per 2 s in fights). The window shows the frame one flip later;
 * GPU-side reads of render targets use the live texture (the sampled-RT path), not guest memory. */
static void readback_start(Rt *rt)
{
    ID3D11Texture2D *st = rt->pstage == rt->stage ? rt->stage2 : rt->stage;
    if (!st) { readback(rt); return; }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, rt_guest_image(rt));
    rt->pstage = st; rt->pend = 1;
    rt->pva = rt->va; rt->pw = rt->w; rt->ph = rt->h; rt->ppitch = rt->pitch; rt->pbpp = rt->bpp;
    rt->dirty = 0;
}
static void readback_finish(Rt *rt)
{
    if (!rt->pend) return;
    rb_write(rt->pstage, rt->pva, rt->pw, rt->ph, rt->ppitch, rt->pbpp);
    rt->pend = 0;
}

/* ---- what the window shows ---------------------------------------------- */

/* The frame goes to the window's own device in a shared texture, at full render scale. The window thread presents it
 * on its own clock, so the display never holds up the game. If sharing fails, the window keeps showing guest memory. */
extern volatile uint32_t g_nv2a_flip_va;
extern void gpu_present_set_source(void *shared_handle, uint32_t width, uint32_t height);
extern void gpu_present_frame_ready(void);
extern void mvc2_overlay_flip_wait(void);
static ID3D11Texture2D *s_share;
static IDXGIKeyedMutex *s_share_mutex;
static uint32_t s_share_w, s_share_h;
static int s_share_failed, s_share_on;

static void share_close(void)
{
    if (s_share_on) { gpu_present_set_source(NULL, 0, 0); s_share_on = 0; }
    SAFE_REL(s_share_mutex); SAFE_REL(s_share);
    s_share_w = s_share_h = 0;
}

static void publish_frame(void)
{
    uint32_t va = g_nv2a_flip_va;
    int i;
    Rt *rt = NULL;
    if (s_share_failed) return;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].va == va) { rt = &s_rt[i]; break; }
    if (!rt) { if (s_share_on) { gpu_present_set_source(NULL, 0, 0); s_share_on = 0; } return; }
    if (!rt->shown_dirty && s_share_on) return;
    {
        uint32_t w = rt->w * (uint32_t)s_scale, h = rt->h * (uint32_t)s_scale;
        if (w != s_share_w || h != s_share_h || !s_share) {
            D3D11_TEXTURE2D_DESC td;
            IDXGIResource *res = NULL;
            HANDLE handle = NULL;
            share_close();
            memset(&td, 0, sizeof td);
            td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_share))
                || FAILED(ID3D11Texture2D_QueryInterface(s_share, &IID_IDXGIKeyedMutex, (void **)&s_share_mutex))
                || FAILED(ID3D11Texture2D_QueryInterface(s_share, &IID_IDXGIResource, (void **)&res))
                || FAILED(IDXGIResource_GetSharedHandle(res, &handle))) {
                fprintf(stderr, "[GPUBE] shared frame texture failed: the window shows guest memory\n");
                SAFE_REL(res);
                share_close();
                s_share_failed = 1;
                return;
            }
            SAFE_REL(res);
            s_share_w = w; s_share_h = h;
            gpu_present_set_source(handle, w, h);
            s_share_on = 1;
        }
    }
    rt_resolve(rt);
    if (FAILED(IDXGIKeyedMutex_AcquireSync(s_share_mutex, 0, 4))) return;   /* the window has it: the next flip's frame */
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_share, (ID3D11Resource *)rt->tex);
    IDXGIKeyedMutex_ReleaseSync(s_share_mutex, 0);
    rt->shown_dirty = 0;
    gpu_present_frame_ready();
}

void mvc2_video_request(int scale, int msaa, int aniso)
{
    s_want_scale = scale; s_want_msaa = msaa; s_want_aniso = aniso;
}

/* Between frames, on the pushbuffer thread. */
static void video_apply(void)
{
    int scale = s_want_scale, msaa = msaa_supported(s_want_msaa), aniso = s_want_aniso, i;
    if (aniso != s_aniso) {
        for (i = 0; i < s_nss; i++) IUnknown_Release((IUnknown *)s_ss[i].obj);
        s_nss = 0;
        for (i = 0; i < MAX_TEX; i++) { DEFER_REL(s_tx[i].srv); DEFER_REL(s_tx[i].tex); s_tx[i].used = 0; }
        s_aniso = aniso;
    }
    if (scale != s_scale || msaa != s_msaa) {
        ID3D11RenderTargetView *none = NULL;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &none, NULL);
        for (i = 0; i < MAX_RT; i++) { rt_release(&s_rt[i]); s_rt[i].va = 0; }
        for (i = 0; i < 4; i++) { SAFE_REL(s_ds[i].dsv); SAFE_REL(s_ds[i].tex); s_ds[i].va = 0; }
        s_scale = scale; s_msaa = msaa;
        s_resets++;
        fprintf(stderr, "[GPUBE] video: render scale %dx, %dx MSAA, %dx anisotropic\n", s_scale, s_msaa, s_aniso);
    }
}

static void gb_flip_impl(void)
{
    int i;
    static unsigned long last;
    unsigned long now;
    if (!s_inited || s_failed) return;
    s_frame++; s_tick++; st_flips++;
    s_frame_for_grave = s_frame;
    texdump_poll();
    { unsigned long k = 0, keep = 0; for (; k < s_defer_n; k++) { if (s_grave[k].f + 2 <= s_frame) IUnknown_Release(s_grave[k].o); else s_grave[keep++] = s_grave[k]; } s_defer_n = keep; }
    gb_mark(3);
    if (!s_rb_sync)
        for (i = 0; i < MAX_RT; i++)
            if (s_rt[i].tex) readback_finish(&s_rt[i]);   /* last flip's copies */
    MVC2_DEBUG_ON_FLIP(s_shot_va, s_shot_w, s_shot_h, s_shot_pitch, s_shot_bpp);   /* debug flavour only: DEL mark key, due screenshots */
    s_shot_w = s_shot_h = 0;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].dirty) {
            if (s_rt[i].sampled + 2 >= s_frame && !s_rb_all) { s_rt[i].dirty = 0; continue; }
            if (s_rb_sync) readback(&s_rt[i]); else readback_start(&s_rt[i]);
        }
    gb_mark(4);
    gb_mark(0);
    if (s_fh > 0) { unsigned k, nn = fh_nd < 256 ? fh_nd : 256; if (nn != fd_pn && fd_lines < 4000) { fprintf(stderr, "[FD] f=%u ndraws %u -> %u\n", s_frame, fd_pn, nn); fd_lines++; if (nn + 1 == fd_pn || nn == fd_pn + 1) { const FdRec *A = nn > fd_pn ? fd_cur : fd_prev; const FdRec *B = nn > fd_pn ? fd_prev : fd_cur; unsigned m = nn > fd_pn ? nn : fd_pn, j; for (j = 0; j + 1 < m; j++) if (A[j].vh != B[j].vh) break; fprintf(stderr, "[FD]   extra/missing draw at idx %u of %u: n=%u pos=(%.1f,%.1f) tex=%ux%u fmt=%02X c0=%08X blend=%u vh=%016llX\n", j, m, A[j].n, A[j].x0, A[j].y0, A[j].w, A[j].h, A[j].fmt, A[j].c0, A[j].st, (unsigned long long)A[j].vh); } } else if (nn == fd_pn) { for (k = 0; k < nn && fd_lines < 4000; k++) if (fd_cur[k].vh != fd_prev[k].vh || fd_cur[k].th != fd_prev[k].th) { fprintf(stderr, "[FD] f=%u draw#%u/%u n=%u %s%s pos=(%.1f,%.1f) tex=%ux%u fmt=%02X c0=%08X blend=%u\n", s_frame, k, nn, fd_cur[k].n, fd_cur[k].vh != fd_prev[k].vh ? "VERT " : "", fd_cur[k].th != fd_prev[k].th ? "TEXDATA" : "", fd_cur[k].x0, fd_cur[k].y0, fd_cur[k].w, fd_cur[k].h, fd_cur[k].fmt, fd_cur[k].c0, fd_cur[k].st); fd_lines++; } } memcpy(fd_prev, fd_cur, sizeof(fd_cur)); fd_pn = nn; }
    if (s_fh > 0) { fprintf(stderr, "[FH] f=%u nd=%u exb=%u fl0=%u xff=%u sh=%u tr=%u bg=%u ilf=%u dpn=%u dp=%u dh=%016llX rth=%016llX\n", s_frame, fh_nd, g_dbg_batches, g_dbg_flush0, g_dbg_xffail, g_dbg_short, g_dbg_trunc, g_dbg_begin, g_dbg_ilf, g_dbg_dpn, g_dbg_dp, (unsigned long long)fh_dh, (unsigned long long)fh_rth); fh_dh = 0; fh_rth = 0; fh_nd = 0; g_dbg_batches = g_dbg_flush0 = g_dbg_xffail = g_dbg_short = g_dbg_begin = g_dbg_ilf = g_dbg_dpn = g_dbg_dp = 0; }
    now = GetTickCount();
    if (s_verbose && now - last >= 2000) {
        fprintf(stderr, "[GPUBE] frame=%u flips=%u draws=%u tris=%u culled=%u clears=%u texdecodes=%u texhash_us=%u dec_us=%u crt_us=%u rtsamples=%u us_draw=%u us_clear=%u us_flip=%u scan_ms=%u segs=%u thfast=%u thfull=%u thstale=%u\n",
                s_frame, st_flips, st_draws, st_tris, st_culled, st_clears, st_decodes, st_hash_ns_us, st_dec_us, st_crt_us, st_rtsamples, st_us_draw, st_us_clear, st_us_flip, (unsigned)(g_pb_scan_ticks * 1000ULL / gb_qf()), g_pb_scan_segs, st_th_fast, st_th_full, st_th_stale);
        /* Executor split, ms per window: batch = all of raster_batch; xf = vertex loop (fetch + vsh), vsh = interpreter alone;
         * clip = clip + triangle assembly; flush = hand-off incl. draw() (us_draw above). */
        fprintf(stderr, "[GPUSPLIT] scan_ms=%u batch_ms=%u xf_ms=%u vsh_ms=%u fetch_ms=%u clip_ms=%u flush_ms=%u xfverts=%u\n",
                (unsigned)(g_pb_scan_ticks * 1000ULL / gb_qf()), (unsigned)(g_pb_t_batch * 1000ULL / gb_qf()),
                (unsigned)(g_pb_t_xf * 1000ULL / gb_qf()), (unsigned)(g_pb_t_vsh * 1000ULL / gb_qf()),
                (unsigned)((g_pb_t_xf > g_pb_t_vsh ? g_pb_t_xf - g_pb_t_vsh : 0) * 1000ULL / gb_qf()),
                (unsigned)(g_pb_t_clip * 1000ULL / gb_qf()), (unsigned)(g_pb_t_flush * 1000ULL / gb_qf()), g_pb_n_xfverts);
        g_pb_scan_ticks = 0; g_pb_scan_segs = 0;
        g_pb_t_batch = g_pb_t_xf = g_pb_t_vsh = g_pb_t_clip = g_pb_t_flush = 0; g_pb_n_xfverts = 0;
        st_us_draw = st_us_clear = st_us_flip = 0;
        st_dec_us = st_crt_us = 0; st_draws = st_tris = st_culled = st_clears = st_decodes = st_hash_ns_us = st_flips = st_rtsamples = 0; st_th_fast = st_th_full = st_th_stale = 0;
        last = now;
    }
    publish_frame();
    mvc2_overlay_flip_wait();
    video_apply();
}

static unsigned qpc_us(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (unsigned)((t.QuadPart * 1000000ULL) / f.QuadPart); }
static void gb_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags, uint32_t argb, uint32_t zst) { unsigned t0 = qpc_us(); gb_mark(1); gb_clear_impl(s, rs, flags, argb, zst); gb_mark(0); st_us_clear += qpc_us() - t0; }
static void gb_draw(const Nv2aSurface *s, const Nv2aBatch *b) { unsigned t0 = qpc_us(); gb_mark(2); gb_draw_impl(s, b); gb_mark(0); st_us_draw += qpc_us() - t0; }
static void gb_flip(void) { unsigned t0 = qpc_us(); gb_mark(4); gb_flip_impl(); gb_mark(0); st_us_flip += qpc_us() - t0; }
static const Nv2aBackend s_backend = { gb_clear, gb_draw, gb_flip };


/* ---- sampling profiler (debug): RECOMP_PROF="from-to" seconds since install; needs a PDB ---- */
#include <tlhelp32.h>
#include <psapi.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")
typedef struct { DWORD64 a, b; unsigned n; } ProfB;
static char g_prof_chain[8][1200]; static int g_prof_chain_n;
static DWORD WINAPI gb_prof_thread(LPVOID p)
{
    unsigned lo = 0, hi = 0; DWORD t0 = GetTickCount(); DWORD self = GetCurrentThreadId();
    enum { MAXT = 64, MAXA = 4096 };
    static DWORD tids[MAXT]; static unsigned nt = 0; static ProfB *bk[MAXT]; static unsigned nb[MAXT]; static unsigned tot[MAXT];
    unsigned i, last_enum = 0;
    sscanf((const char *)p, "%u-%u", &lo, &hi);
    while ((GetTickCount() - t0) / 1000 < lo) Sleep(100);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(GetCurrentProcess(), NULL, TRUE);
    { /* pick the 4 busiest threads over 2 s so sampling does not perturb the run */
        DWORD ids[256]; unsigned long long t1[256]; unsigned n = 0, k, j; HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te; te.dwSize = sizeof te;
        if (s != INVALID_HANDLE_VALUE) { if (Thread32First(s, &te)) do { if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self && n < 256) ids[n++] = te.th32ThreadID; } while (Thread32Next(s, &te)); CloseHandle(s); }
        for (k = 0; k < n; k++) { HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, ids[k]); FILETIME a, b, kt, ut; t1[k] = 0; if (h) { if (GetThreadTimes(h, &a, &b, &kt, &ut)) t1[k] = ((unsigned long long)kt.dwHighDateTime << 32 | kt.dwLowDateTime) + ((unsigned long long)ut.dwHighDateTime << 32 | ut.dwLowDateTime); CloseHandle(h); } }
        Sleep(2000);
        for (k = 0; k < n; k++) { HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, ids[k]); FILETIME a, b, kt, ut; unsigned long long v = 0; if (h) { if (GetThreadTimes(h, &a, &b, &kt, &ut)) v = ((unsigned long long)kt.dwHighDateTime << 32 | kt.dwLowDateTime) + ((unsigned long long)ut.dwHighDateTime << 32 | ut.dwLowDateTime); CloseHandle(h); } t1[k] = v - t1[k]; }
        for (j = 0; j < 4; j++) { unsigned bi = 0; unsigned long long bv = 0; for (k = 0; k < n; k++) if (t1[k] > bv) { bv = t1[k]; bi = k; } if (bv < 1000000ULL) break; fprintf(stderr, "[PROF] watching thread %lu cpu=%llu ms/2s\n", ids[bi], bv / 10000); tids[nt] = ids[bi]; bk[nt] = (ProfB *)calloc(MAXA, sizeof(ProfB)); nt++; t1[bi] = 0; }
    }
    while ((GetTickCount() - t0) / 1000 < hi) {
        if (0) {
            HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te; te.dwSize = sizeof te;
            if (s != INVALID_HANDLE_VALUE) { if (Thread32First(s, &te)) do { if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self) { unsigned k; for (k = 0; k < nt; k++) if (tids[k] == te.th32ThreadID) break; if (k == nt && nt < MAXT) { tids[nt] = te.th32ThreadID; bk[nt] = (ProfB *)calloc(MAXA, sizeof(ProfB)); nt++; } } } while (Thread32Next(s, &te)); CloseHandle(s); }
            last_enum = GetTickCount();
        }
        for (i = 0; i < nt; i++) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[i]);
            if (!h) continue;
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &c)) {
                    DWORD64 a = c.Rip, disp = 0; char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf; unsigned k;
                    memset(buf, 0, sizeof buf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
                    if (SymFromAddr(GetCurrentProcess(), a, &disp, si)) a = si->Address; else a &= ~0xFFULL;
                    { /* first caller frame inside the exe */
                      static DWORD64 exe_lo, exe_hi; CONTEXT u = c; int fr; DWORD64 caller = 0;
                      if (!exe_lo) { HMODULE m = GetModuleHandleA(NULL); MODULEINFO mi; exe_lo = (DWORD64)m; exe_hi = exe_lo + 0x4000000; (void)mi; }
                      { static char chainbuf[8][1200]; static int nchain = 0; static int dumped_at_end; (void)dumped_at_end;
                        if (nchain < 8 && si->Name[0] == (char)90 && !strcmp(si->Name, "ZwWriteFile") && (tot[i] % 37) == 0) { CONTEXT u2 = c; int f2; char *w = chainbuf[nchain]; w[0] = 0; for (f2 = 0; f2 < 14; f2++) { DWORD64 base2, ef2; PVOID hd2; PRUNTIME_FUNCTION rf2; char b3[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *s3 = (SYMBOL_INFO *)b3; DWORD64 d3 = 0; memset(b3, 0, sizeof b3); s3->SizeOfStruct = sizeof(SYMBOL_INFO); s3->MaxNameLen = 127; if (SymFromAddr(GetCurrentProcess(), u2.Rip, &d3, s3)) { strncat(w, s3->Name, 60); strcat(w, " < "); } else strcat(w, "? < "); rf2 = RtlLookupFunctionEntry(u2.Rip, &base2, NULL); if (!rf2) break; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base2, u2.Rip, rf2, &u2, &hd2, &ef2, NULL); } nchain++; g_prof_chain_n = nchain; strcpy(g_prof_chain[nchain - 1], w); }
                      }
                      __try { for (fr = 0; fr < 12; fr++) { DWORD64 base, hnd_data = 0; PVOID hd; DWORD64 ef; PRUNTIME_FUNCTION rf; if (u.Rip >= exe_lo && u.Rip < exe_hi) { caller = u.Rip; break; } rf = RtlLookupFunctionEntry(u.Rip, &base, NULL); if (!rf) break; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, rf, &u, &hd, &ef, NULL); (void)hnd_data; } } __except(EXCEPTION_EXECUTE_HANDLER) { }
                      if (caller && !(c.Rip >= exe_lo && c.Rip < exe_hi)) { char b2[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *s2 = (SYMBOL_INFO *)b2; DWORD64 d2 = 0; memset(b2, 0, sizeof b2); s2->SizeOfStruct = sizeof(SYMBOL_INFO); s2->MaxNameLen = 255; caller = SymFromAddr(GetCurrentProcess(), caller, &d2, s2) ? s2->Address : caller; } else caller = 0;
                      for (k = 0; k < nb[i]; k++) if (bk[i][k].a == a && bk[i][k].b == caller) break;
                      if (k == nb[i] && nb[i] < MAXA) { bk[i][k].a = a; bk[i][k].b = caller; nb[i]++; }
                    }
                    if (k < nb[i]) bk[i][k].n++;
                    tot[i]++;
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        Sleep(1);
    }
    for (i = 0; i < nt; i++) {
        unsigned r, k; if (tot[i] < 50) continue;
        fprintf(stderr, "[PROF] thread %lu samples=%u\n", tids[i], tot[i]);
        for (r = 0; r < 14; r++) {
            unsigned bi = 0, bn = 0; char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf; DWORD64 d = 0;
            for (k = 0; k < nb[i]; k++) if (bk[i][k].n > bn) { bn = bk[i][k].n; bi = k; }
            if (!bn) break;
            memset(buf, 0, sizeof buf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
            { char nm1[300]; strcpy(nm1, SymFromAddr(GetCurrentProcess(), bk[i][bi].a, &d, si) ? si->Name : "?"); memset(buf, 0, sizeof buf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255; fprintf(stderr, "[PROF]   %5.1f%% %s <- %s\n", 100.0 * bn / tot[i], nm1, bk[i][bi].b && SymFromAddr(GetCurrentProcess(), bk[i][bi].b, &d, si) ? si->Name : "-"); }
            bk[i][bi].n = 0;
        }
    }
    { int q; for (q = 0; q < g_prof_chain_n; q++) fprintf(stderr, "[PROF] WriteFile chain: %s\n", g_prof_chain[q]); }
    fflush(stderr);
    return 0;
}
/* Freeze dump (from the watchdog): suspend each thread in turn and print up to 24 symbolized frames. Needs a PDB. */
static void gb_freeze_dump(int pass)
{
    static int sym_ok; DWORD self = GetCurrentThreadId(); HANDLE s; THREADENTRY32 te; HANDLE proc = GetCurrentProcess();
    static DWORD sym_err;
    /* ERROR_INVALID_PARAMETER (87) = already initialised for this process (main.c's crash handler does it): usable. */
    if (!sym_ok) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); sym_ok = SymInitialize(proc, NULL, TRUE) ? 1 : -1; if (sym_ok < 0) { sym_err = GetLastError(); if (sym_err == ERROR_INVALID_PARAMETER) sym_ok = 1; } }
    /* exe base printed so a dump without symbols can still be resolved offline against the PDB */
    fprintf(stderr, "[FREEZE] dump %d: all threads (symbols %s, err %lu) exe base %p\n", pass, sym_ok > 0 ? "ok" : "unavailable", (unsigned long)sym_err, (void *)GetModuleHandleA(NULL));
    s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); te.dwSize = sizeof te;
    if (s == INVALID_HANDLE_VALUE) return;
    if (Thread32First(s, &te)) do {
        HANDLE h; CONTEXT c; char line[2400]; int fr, nfr = 0, fault = 0; DWORD64 rips[24];
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        line[0] = 0;
        /* Only unwind while suspended; symbol lookup allocates, so it waits until the thread runs again. */
        if (SuspendThread(h) != (DWORD)-1) {
            memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &c)) {
                __try {
                    for (; nfr < 24 && c.Rip; nfr++) {
                        DWORD64 base, ef; PVOID hd; PRUNTIME_FUNCTION rf;
                        rips[nfr] = c.Rip;
                        rf = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                        if (rf) RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, NULL);
                        else { c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8; }   /* leaf function */
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { fault = 1; }
            }
            ResumeThread(h);
        }
        CloseHandle(h);
        for (fr = 0; fr < nfr; fr++) {
            char b[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *si = (SYMBOL_INFO *)b; DWORD64 d = 0; char nm[160];
            memset(b, 0, sizeof b); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 127;
            if (sym_ok > 0 && SymFromAddr(proc, rips[fr], &d, si)) _snprintf(nm, sizeof nm, "%s+0x%llx", si->Name, (unsigned long long)d);
            else _snprintf(nm, sizeof nm, "%llx", (unsigned long long)rips[fr]);
            nm[sizeof nm - 1] = 0;
            if (strlen(line) + strlen(nm) + 4 < sizeof line) { strcat(line, nm); strcat(line, " < "); }
        }
        fprintf(stderr, "[FREEZE] tid %lu: %s%s\n", te.th32ThreadID, line, fault ? "(unwind fault)" : "");
    } while (Thread32Next(s, &te));
    CloseHandle(s);
    fflush(stderr);
}

void mvc2_gpu_backend_install(void)
{
    const char *e = getenv("RECOMP_GPU");
    if (e && e[0] == '0') { fprintf(stderr, "[GPUBE] disabled (RECOMP_GPU=0): CPU rasteriser\n"); return; }
    if (!gb_ready()) { fprintf(stderr, "[GPUBE] D3D11 init failed, using CPU rasteriser\n"); return; }
    nv2a_backend_register(&s_backend);
    { static char pe[64]; const char *pv = getenv("RECOMP_PROF"); if (pv) { strncpy(pe, pv, 63); CreateThread(NULL, 0, gb_prof_thread, pe, 0, NULL); } }
    if (getenv("RECOMP_GPU_WATCHDOG")) CreateThread(NULL, 0, gb_watchdog, NULL, 0, NULL);

    fprintf(stderr, "[GPUBE] D3D11 NV2A back end registered\n");
}






#ifdef MVC2_SAVE_STATES
/* After a save-state load, on the coordinator with the GPU executor parked:
 * drop deferred readbacks (they hold post-snapshot frames and would overwrite
 * restored guest RAM at the next flip) and force a full texture re-verify. */
void mvc2_gpu_state_after_load(void)
{
    int i;
    for (i = 0; i < MAX_RT; i++) { s_rt[i].pend = 0; s_rt[i].dirty = 0; }
    for (i = 0; i < MAX_TEX; i++) s_tx[i].vframe = 0;
    s_shot_w = s_shot_h = 0;
}
#endif
