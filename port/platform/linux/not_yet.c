/* The parts of the port that are not on Linux yet, as inert versions of their Windows files:
   the updater (hle/update.c: the releases are Windows builds), the start window (hle/splash.c),
   texture packs (gs/texpack.c, for the native renderer), and the network (net/ws_winhttp.c,
   WinHTTP): online races report that they can't connect. */
#include <stdlib.h>
#include <string.h>

#include "../../gs/texpack.h"
#include "../../gs/font_sharp.h"
#include "../../hle/splash.h"
#include "../../net/ws_winhttp.h"

void update_save_environment(void) {}
void update_at_start(void) {}
int update_after_game(int code) { return code; }
void update_request(void) {}

void splash_show(void) {}
void splash_step(int percent, const char *text) { (void)percent, (void)text; }
void splash_close(void) {}

int texpack_available(void) { return 0; }
TexPackEntry *texpack_find(const Gs *g, unsigned c) { (void)g; (void)c; return NULL; }
void texpack_field(uint64_t field) { (void)field; }
void texpack_report(void) {}
int font_sharp_on(void) { return 0; }
int font_sharp_match(uint64_t tex0) { (void)tex0; return 0; }

struct WsConn { int unused; };

WsConn *ws_start(const char *url, const WsHandler *h)
{
    (void)url;
    WsConn *ws = calloc(1, sizeof *ws);
    /* connecting failed (code -1); a URL to retry is not followed */
    if (h && h->on_close) h->on_close(h->ctx, -1, "online races are not on Linux yet", 1);
    return ws;
}

void ws_send(WsConn *ws, int binary, const void *data, size_t n) { (void)ws, (void)binary, (void)data, (void)n; }
void ws_stop(WsConn *ws) { free(ws); }

int http_get(const char *url, char *out, size_t cap)
{
    (void)url;
    if (cap) out[0] = 0;
    return -1;
}
