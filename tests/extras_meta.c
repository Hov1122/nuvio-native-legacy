// Keyless /meta parse (extras_meta_cinemeta) against a trimmed fixture.
// Only the pure parse is tested here — fetch + threads need the network and
// report to [extras] so a TV run shows exactly what arrived.
//
//   cc tests/extras_meta.c src/extras.c src/js.c -Isrc -o /tmp/t-meta && /tmp/t-meta
#include "extras.h"
#include "catalogo.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Stubs so the module links without the world (same pattern as tests/simkl.c).
// The parse under test never touches the network, the catalog or the auth.
const CatItem *cat_item(int i) { (void)i; return NULL; }
int cat_indice_por_imdb(const char *id) { (void)id; return -1; }
int cat_similares(int a, int *b, int c) { (void)a; (void)b; (void)c; return 0; }
int vistoep_estado(const char *a, int b, int c) { (void)a; (void)b; (void)c; return -1; }
void vistoep_definir(const char *a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
int vistoep_contar(const char *a) { (void)a; return 0; }
int vistoep_conhecido(const char *a) { (void)a; return 0; }
int vistoep_n(void) { return 0; }
int trakt_ativo(void) { return 0; }
int trakt_cabecalhos(const char **a, char *b, size_t c, char *d, size_t e) {
  (void)a; (void)b; (void)c; (void)d; (void)e; return 0;
}
const char *ling_titulo_original(void) { return ""; }
int trailer_id_valido(const char *a) { (void)a; return 1; }
char *rede_baixar(const char *a, int b) { (void)a; (void)b; return NULL; }
char *rede_baixar_com(const char *a, int b, const char *const *c) {
  (void)a; (void)b; (void)c; return NULL;
}
char *rede_postar(const char *a, int b, const char *const *c, const char *d) {
  (void)a; (void)b; (void)c; (void)d; return NULL;
}
const char *desc_chave_tmdb(void) { return ""; }
const char *desc_tmdb_idioma(void) { return "en"; }
const char *ling_audio(void) { return ""; }
const char *ling_audio2(void) { return ""; }
int ling_casa(const char *a, const char *b) { (void)a; (void)b; return 0; }

static int falhas;
static void ok(const char *oque, int cond) {
  if (!cond) { printf("FALHOU: %s\n", oque); falhas++; }
}

static char *ler(const char *path) {
  FILE *f = fopen(path, "rb");
  char *b;
  long n;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  b = malloc((size_t)n + 1);
  if (!b) { fclose(f); return NULL; }
  if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
  else b[n] = 0;
  fclose(f);
  return b;
}

int main(void) {
  // Suite relativa a raiz (ver tests/simkl.sh).
  char *corpo = ler("tests/fixtures/meta_bb.json");
  if (!corpo) { printf("FALHOU: sem fixture\n"); return 1; }
  extras_pedir("tt0903747", 1, 0);   // arma o idPedido; fios nascem mortos
  extras_meta_cinemeta("tt0903747", corpo, 1);
  // Specials (temporada 0) ficam de fora; o resto agrupa por temporada.
  ok("2 temporadas", extras_n_temporadas() == 2);
  ok("T1 e T2", extras_temporada_numero(0) == 1 &&
     extras_temporada_numero(1) == 2);
  ok("T1 tem 2", extras_n_eps(0) == 2);
  ok("T1E1 = 77", extras_ep_numero(0, 0) == 1 && extras_ep_nota(0, 0) == 77);
  ok("T1E2 = 76", extras_ep_numero(0, 1) == 2 && extras_ep_nota(0, 1) == 76);
  ok("T2E2 sem nota", extras_ep_numero(1, 1) == 2 && extras_ep_nota(1, 1) == 0);
  ok("imdb 95", extras_nota(EX_IMDB) == 95);
  // Guarda de titulo: corpo de outra obra nao escreve por cima.
  extras_meta_cinemeta("tt9999999", corpo, 1);
  ok("stale nao escreve", extras_n_temporadas() == 2 &&
     extras_nota(EX_IMDB) == 95);
  free(corpo);
  // Temporada TMDB (/tv/{id}/season/{n}): pares (episodio, decimos), sem
  // voto fica de fora para nao zerar o que ja existe.
  { char *tm = ler("tests/fixtures/tmdb_s1.json");
    int eps[30], notas[30], n;
    if (!tm) { printf("FALHOU: sem fixture tmdb\n"); return 1; }
    n = extras_tmdb_temporada_ler(tm, eps, notas, 30);
    ok("tmdb 2 com voto", n == 2);
    ok("tmdb E1 = 74", n > 0 && eps[0] == 1 && notas[0] == 74);
    ok("tmdb E2 = 73", n > 1 && eps[1] == 2 && notas[1] == 73);
    ok("tmdb nulo recusa", extras_tmdb_temporada_ler(NULL, eps, notas, 30) == 0);
    free(tm); }
  if (falhas) { printf("%d falha(s)\n", falhas); return 1; }
  printf("extras meta ok\n");
  return 0;
}
