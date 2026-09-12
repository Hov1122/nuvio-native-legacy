#ifndef NV_TRAILER_H
#define NV_TRAILER_H

// YouTube trailer playback.
//
// The port renders everything on an SDL canvas and has no video element for
// YouTube, so on Tizen the trailer opens as a fullscreen iframe overlay owned
// by tools/tizen-shell.html (window.__nvTrailerAbrir/Fechar), the same way
// the web app plays trailers in an iframe. On targets without that overlay
// the call logs the watch URL and reports "not played" instead of pretending.

// 11 chars of [A-Za-z0-9_-]. Guards the shell against javascript: and other
// non-ids before anything crosses into JS.
int trailer_id_valido(const char *yt);
// Opens the trailer. 1 when the overlay took it, 0 otherwise.
int trailer_abrir(const char *yt);
void trailer_fechar(void);
int trailer_aberto(void);

#endif
