/* The F1 sprite filter: see sprite_filter.c. */
#pragma once
#include <d3d11.h>
#include <stdint.h>

enum { SPRITE_FILTER_OFF, SPRITE_FILTER_XBR, SPRITE_FILTER_OMNISCALE, SPRITE_FILTER_SCALEFX };

int  sprite_filter_init(ID3D11Device *dev, ID3D11DeviceContext *ctx);
/* layer: premultiplied w x h. dst: the render target to composite over, dw x dh. smooth and sharpen: 0 off, 1 low, 2 high. */
void sprite_filter_run(ID3D11ShaderResourceView *layer, uint32_t w, uint32_t h, ID3D11RenderTargetView *dst, uint32_t dw, uint32_t dh,
                       int filter, int smooth, int sharpen);
