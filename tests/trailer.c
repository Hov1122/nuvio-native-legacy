// The trailer id gate and the open/close state machine. The actual overlay
// lives in tools/tizen-shell.html, which no unit test can load — what is
// tested here is that garbage never crosses into JS and that the state never
// lies about being open.
//
//   cc tests/trailer.c src/trailer.c -Isrc -o /tmp/t-trailer && /tmp/t-trailer
#include "trailer.h"
#include <stdio.h>

static int falhas;
static void ok(const char *oque, int cond) {
  if (!cond) { printf("FALHOU: %s\n", oque); falhas++; }
}

int main(void) {
  ok("valido", trailer_id_valido("dQw4w9WgXcQ"));
  ok("hifen e underline", trailer_id_valido("aB3-_xY12-A"));
  ok("curto nao", !trailer_id_valido("abc"));
  ok("longo nao", !trailer_id_valido("dQw4w9WgXcQ1"));
  ok("nulo nao", !trailer_id_valido(NULL));
  ok("vazio nao", !trailer_id_valido(""));
  ok("espaco nao", !trailer_id_valido("dQw4w9WgXc Q"));
  ok("esquema nao", !trailer_id_valido("javascript:"));
  ok("barra nao", !trailer_id_valido("dQw/4w9WgXc"));

  // No desktop nao ha overlay: abrir informa e devolve 0, sem marcar aberto.
  ok("abrir sem overlay da 0", trailer_abrir("dQw4w9WgXcQ") == 0);
  ok("nao marca aberto", !trailer_aberto());
  ok("invalido nao abre", trailer_abrir("xyz") == 0);
  trailer_fechar();
  ok("fechado", !trailer_aberto());

  if (falhas) { printf("%d falha(s)\n", falhas); return 1; }
  printf("trailer ok\n");
  return 0;
}
