/* The game's font drawn sharp at high resolutions (font_sharp.c, finding 058). */
#ifndef PORT_GS_FONT_SHARP_H
#define PORT_GS_FONT_SHARP_H
#include <stdint.h>

/* Direct3D 11 objects (<d3d11.h> in font_sharp.c); font_sharp_on and font_sharp_match need none. */
struct ID3D11Device;
struct ID3D11ShaderResourceView;

/* DHD_FONT (dhd.ini "font"): sharp (the default) or original. Read once. */
int font_sharp_on(void);

/* Whether a context's TEX0 is the text engine's font texture: the one its text packets set
   (D_004F35B0, func_00210068 and func_00210250). Reads EE memory, writes none. */
int font_sharp_match(uint64_t tex0);

/* A high-resolution version of a decoded font texture (w x h GS colours, alpha 0x80 opaque), made
   in memory only: a new reference to its view, or NULL. key names the decoded texture; the last
   few are kept, so decoding the same font again costs nothing. */
struct ID3D11ShaderResourceView *font_sharp_view(struct ID3D11Device *device, const uint32_t *pixels, unsigned w,
                                                 unsigned h, uint64_t key);

#endif
