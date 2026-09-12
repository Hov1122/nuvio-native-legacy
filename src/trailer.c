#include "trailer.h"
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Keep the bodies comma-free at top level: EM_JS is a variadic macro and a
// bare comma splits arguments (video_tizen.c documents a build this broke).
EM_JS(int, trailer_js_abrir, (const char *yt), {
  try {
    if (window.__nvTrailerAbrir) {
      window.__nvTrailerAbrir(UTF8ToString(yt));
      return 1;
    }
  } catch (e) {}
  return 0;
});
EM_JS(void, trailer_js_fechar, (void), {
  try {
    if (window.__nvTrailerFechar) window.__nvTrailerFechar();
  } catch (e) {}
});
#endif

static int aberto;

int trailer_id_valido(const char *yt) {
  size_t i;
  if (!yt) return 0;
  if (strlen(yt) != 11) return 0;
  for (i = 0; i < 11; i++) {
    char c = yt[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_') continue;
    return 0;
  }
  return 1;
}

int trailer_abrir(const char *yt) {
  if (!trailer_id_valido(yt)) return 0;
#ifdef __EMSCRIPTEN__
  // Called from the main loop (detail OK), which is the main browser thread,
  // so this EM_JS runs where the DOM lives — the same single-thread doorway
  // rule video_tizen.c follows for webapis.avplay.
  if (!trailer_js_abrir(yt)) {
    printf("[trailer] shell sem overlay: %s\n", yt);
    fflush(stdout);
    return 0;
  }
  aberto = 1;
  printf("[trailer] abrindo %s\n", yt);
  fflush(stdout);
  return 1;
#else
  printf("[trailer] sem overlay neste alvo: https://www.youtube.com/watch?v=%s\n",
         yt);
  fflush(stdout);
  return 0;
#endif
}

void trailer_fechar(void) {
#ifdef __EMSCRIPTEN__
  trailer_js_fechar();
#endif
  aberto = 0;
}

int trailer_aberto(void) { return aberto; }
