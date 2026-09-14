// SIMKL import helpers. Only the pure functions are tested here — the pull
// itself needs a linked account and the network, and reports to [simkl] so a
// TV run shows exactly what it parsed.
//
//   cc tests/simkl.c src/simkl.c src/js.c -Isrc -o /tmp/t-simkl && /tmp/t-simkl
#include "simkl.h"
#include "simklauth.h"
#include "catalogo.h"
#include "perfil.h"
#include <stdio.h>
#include <string.h>

// Stubs so the module links without the world: the tested functions never
// touch the network, the catalog or the auth state (same pattern as
// tests/syncprog.c). js_* come from the real src/js.c, linked below.
SmkEstado simklauth_estado(void) { return SMK_PARADO; }
const char *simklauth_token(void) { return ""; }
const char *nuvem_simkl_cliente(void) { return ""; }
const char *nuvem_simkl_app(void) { return ""; }
void nuvem_url_escapar(const char *v, char *d, unsigned t) {
  unsigned k = 0;
  if (!d || !t) return;
  d[0] = 0;
  if (!v) return;
  while (v[k] && k + 1 < t) { d[k] = v[k]; k++; }
  d[k] = 0;
}
char *rede_baixar_st(const char *u, int t, const char *const *c, int *st) {
  (void)u; (void)t; (void)c; if (st) *st = 0; return NULL;
}
void cat_historico_definir_id(const char *a, const char *b, int c) {
  (void)a; (void)b; (void)c;
}
const CatItem *cat_item(int i) { (void)i; return NULL; }
int cat_indice_por_imdb(const char *id) { (void)id; return -1; }
char *rede_postar_st(const char *u, int t, const char *const *c,
                     const char *b, int *st) {
  (void)u; (void)t; (void)c; (void)b; if (st) *st = 0; return NULL;
}
// Decoration is network; the pure parse below never reaches it.
int cinemeta_enfeitar_lote(CatItem *saida, int n) {
  (void)saida; return n;
}

static int falhas;
static void ok(const char *oque, int cond) {
  if (!cond) { printf("FALHOU: %s\n", oque); falhas++; }
}

int main(void) {
  // Only finished titles mark: anything in progress must not pretend done.
  ok("completed marca",      simkl_status_visto("completed"));
  ok("COMPLETED marca",      simkl_status_visto("COMPLETED"));
  ok("watching nao marca",   !simkl_status_visto("watching"));
  ok("hold nao marca",       !simkl_status_visto("hold"));
  ok("dropped nao marca",    !simkl_status_visto("dropped"));
  ok("plantowatch nao marca", !simkl_status_visto("plantowatch"));
  ok("vazio nao marca",      !simkl_status_visto(""));
  ok("nulo nao marca",       !simkl_status_visto(NULL));

  // Catalog keys on imdb with the tt prefix either way.
  { char d[24];
    simkl_normalizar_imdb("tt0903747", d, sizeof d);
    ok("tt passa intacto", !strcmp(d, "tt0903747"));
    simkl_normalizar_imdb("0903747", d, sizeof d);
    ok("numero ganha tt", !strcmp(d, "tt0903747"));
    simkl_normalizar_imdb("TT0903747", d, sizeof d);
    ok("TT maiusculo normaliza", !strcmp(d, "tt0903747"));
    simkl_normalizar_imdb("", d, sizeof d);
    ok("vazio da vazio", !d[0]);
    simkl_normalizar_imdb(NULL, d, sizeof d);
    ok("nulo da vazio", !d[0]); }

  // History bodies, byte-exact (shapes verified against SIMKL's Kodi addon).
  { char b[1024];
    ok("filme monta", simkl_corpo_historico(b, sizeof b, "Dune", "tt1160419",
                                            0, 0, 0));
    ok("filme exato", !strcmp(b, "{\"movies\":[{\"title\":\"Dune\","
                                 "\"ids\":{\"imdb\":\"tt1160419\"}}]}"));
    ok("serie monta", simkl_corpo_historico(b, sizeof b, "Breaking Bad",
                                            "tt0903747", 1, 2, 5));
    ok("serie exata", !strcmp(b, "{\"shows\":[{\"title\":\"Breaking Bad\","
                                 "\"ids\":{\"imdb\":\"tt0903747\"},"
                                 "\"seasons\":[{\"number\":2,\"episodes\":"
                                 "[{\"number\":5}]}]}]}"));
    ok("serie sem episodio recusa",
       !simkl_corpo_historico(b, sizeof b, "Breaking Bad", "tt0903747",
                              1, 0, 0));
    ok("sem titulo recusa",
       !simkl_corpo_historico(b, sizeof b, "", "tt1160419", 0, 0, 0));
    ok("sem imdb recusa",
       !simkl_corpo_historico(b, sizeof b, "Dune", "", 0, 0, 0)); }

  // Watchlist bodies, byte-exact (web client shapes: "to" on add only,
  // both keys always present, one empty).
  { char b[1024];
    ok("lista filme monta", simkl_corpo_lista(b, sizeof b, "Dune",
                                              "tt1160419", 0, 1));
    ok("lista filme exata", !strcmp(b, "{\"movies\":[{\"title\":\"Dune\","
                                       "\"ids\":{\"imdb\":\"tt1160419\"},"
                                       "\"to\":\"plantowatch\"}],\"shows\":[]}"));
    ok("lista filme tirar", simkl_corpo_lista(b, sizeof b, "Dune",
                                              "tt1160419", 0, 0));
    ok("lista filme tirar exata", !strcmp(b, "{\"movies\":[{\"title\":\"Dune\","
                                              "\"ids\":{\"imdb\":\"tt1160419\"}}],"
                                              "\"shows\":[]}"));
    ok("lista serie monta", simkl_corpo_lista(b, sizeof b, "Breaking Bad",
                                              "tt0903747", 1, 1));
    ok("lista serie exata", !strcmp(b, "{\"shows\":[{\"title\":\"Breaking Bad\","
                                       "\"ids\":{\"imdb\":\"tt0903747\"},"
                                       "\"to\":\"plantowatch\"}],\"movies\":[]}"));
    ok("sem titulo recusa",
       !simkl_corpo_lista(b, sizeof b, "", "tt1160419", 0, 1)); }

  // Playback parse: bare-array sessions into CatItems (no decoration — the
  // caller decorates). Anime skips (tvdb-keyed), imdb-less skips.
  { static CatItem saida[8];
    const char *corpo =
      "[{\"id\":1,\"show\":{\"title\":\"Breaking Bad\","
      "\"ids\":{\"simkl\":123,\"imdb\":\"tt0903747\"}},"
      "\"episode\":{\"season\":2,\"number\":5,\"title\":\"Breakage\"},"
      "\"progress\":45.5,\"paused_at\":\"2026-09-01T10:00:00Z\"},"
      "{\"id\":2,\"movie\":{\"title\":\"Dune\","
      "\"ids\":{\"imdb\":\"tt1160419\"}},"
      "\"progress\":30.0,\"paused_at\":\"2026-09-02T10:00:00Z\"},"
      "{\"id\":3,\"anime\":{\"title\":\"X\"},"
      "\"episode\":{\"season\":1,\"number\":3},\"progress\":10.0},"
      "{\"id\":4,\"show\":{\"title\":\"NoId\"},"
      "\"episode\":{\"season\":1,\"number\":1},\"progress\":50.0}]";
    int n = simkl_continuar_ler(corpo, saida, 8);
    ok("playback le 2", n == 2);
    ok("serie tipo", !strcmp(saida[0].tipo, "series"));
    ok("serie imdb composto", !strcmp(saida[0].imdb, "tt0903747:2:5"));
    ok("serie ep", saida[0].temporada == 2 && saida[0].episodio == 5);
    ok("serie titulo", !strcmp(saida[0].titulo, "Breaking Bad"));
    ok("serie nome ep", !strcmp(saida[0].nomeEpisodio, "Breakage"));
    ok("serie progresso", saida[0].progresso == 45);
    ok("serie instante", saida[0].retomadoMs > 0);
    ok("filme tipo", !strcmp(saida[1].tipo, "movie"));
    ok("filme imdb", !strcmp(saida[1].imdb, "tt1160419"));
    ok("filme progresso", saida[1].progresso == 30);
    ok("filme instante", saida[1].retomadoMs > 0);
    ok("nulo recusa", simkl_continuar_ler(NULL, saida, 8) == 0); }

  // Profile parse: shapes from api.simkl.org (settings + user stats).
  { static PerfilDados pd;
    const char *cfg =
      "{\"user\":{\"name\":\"jane_doe\","
      "\"avatar\":\"https://simkl.in/avatars/12/abc/user_100.jpg\"},"
      "\"account\":{\"id\":12345,\"timezone\":\"Europe/Lisbon\","
      "\"type\":\"free\"}}";
    const char *stats =
      "{\"user\":{\"name\":\"jane_doe\"},\"total_mins\":78230,"
      "\"movies\":{\"total_mins\":18000,"
      "\"plantowatch\":{\"mins\":0,\"count\":12},"
      "\"completed\":{\"mins\":18000,\"count\":200},"
      "\"dropped\":{\"mins\":0,\"count\":1}},"
      "\"tv\":{\"total_mins\":35000,"
      "\"watching\":{\"watched_episodes_count\":23,\"count\":4},"
      "\"completed\":{\"watched_episodes_count\":100,\"count\":5}},"
      "\"anime\":{\"watching\":{\"watched_episodes_count\":10,"
      "\"count\":2}},"
      "\"watched_last_week\":{\"total_mins\":320}}";
    ok("perfil monta", simkl_perfil_ler(cfg, stats, &pd));
    ok("perfil nome", !strcmp(pd.nome, "jane_doe"));
    ok("perfil avatar", !strcmp(pd.avatar, "https://simkl.in/avatars/12/abc/user_100.jpg"));
    ok("perfil periodo", !strcmp(pd.periodo, "Todo o período"));
    ok("perfil minutos", pd.minutos == 78230);
    ok("perfil filmes", pd.filmes == 200);
    ok("perfil episodios", pd.episodios == 133);
    ok("perfil assistindo", pd.assistindo == 6);
    ok("perfil lista", pd.naLista == 12);
    // Sem settings, a identidade sai do stats; sem stats, recusa.
    memset(&pd, 0, sizeof pd);
    ok("perfil sem cfg", simkl_perfil_ler(NULL, stats, &pd));
    ok("perfil nome do stats", !strcmp(pd.nome, "jane_doe"));
    ok("perfil sem stats recusa", !simkl_perfil_ler(cfg, NULL, &pd));
    ok("perfil nulo recusa", !simkl_perfil_ler(cfg, stats, NULL)); }

  // Scrobble bodies, byte-exact (guides/scrobble: progress first, movie has
  // title+ids, show adds episode with season+number).
  { char b[1024];
    ok("scrobble filme monta", simkl_corpo_scrobble(b, sizeof b, "Dune",
                                                    "tt1160419", 0, 0, 0, 12.5));
    ok("scrobble filme exato", !strcmp(b, "{\"progress\":12.50,\"movie\":"
                                        "{\"title\":\"Dune\","
                                        "\"ids\":{\"imdb\":\"tt1160419\"}}}"));
    ok("scrobble serie monta", simkl_corpo_scrobble(b, sizeof b, "Breaking Bad",
                                                    "tt0903747", 1, 2, 5, 42.0));
    ok("scrobble serie exata", !strcmp(b, "{\"progress\":42.00,\"show\":"
                                          "{\"title\":\"Breaking Bad\","
                                          "\"ids\":{\"imdb\":\"tt0903747\"}},"
                                          "\"episode\":{\"season\":2,"
                                          "\"number\":5}}"));
    ok("scrobble serie sem episodio recusa",
       !simkl_corpo_scrobble(b, sizeof b, "Breaking Bad", "tt0903747",
                             1, 0, 0, 10.0));
    ok("scrobble sem titulo recusa",
       !simkl_corpo_scrobble(b, sizeof b, "", "tt1160419", 0, 0, 0, 10.0));
    ok("scrobble teto 100", simkl_corpo_scrobble(b, sizeof b, "Dune",
                                                 "tt1160419", 0, 0, 0, 150.0) &&
                            strstr(b, "\"progress\":100.00") != NULL);
    ok("scrobble piso 0", simkl_corpo_scrobble(b, sizeof b, "Dune",
                                               "tt1160419", 0, 0, 0, -5.0) &&
                          strstr(b, "\"progress\":0.00") != NULL); }

  if (falhas) { printf("%d falha(s)\n", falhas); return 1; }
  printf("simkl ok\n");
  return 0;
}
