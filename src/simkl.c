#include "simkl.h"
#include "simklauth.h"
#include "nuvem.h"
#include "rede.h"
#include "js.h"
#include "jsw.h"
#include "catalogo.h"
#include "vistoep.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>

// Declarado extern como nos outros consumidores (contalib.c, ctxmenu.c): o
// catalogo.h nao exporta esta funcao.
extern void cat_historico_definir_id(const char *imdb, const char *tipo, int visto);

#define SMK_BASE "https://api.simkl.com"

// Only "completed" marks: "watching"/"hold"/"dropped"/"plantowatch" say the
// title is unfinished, and without episode detail marking the title would
// lie. Episode granularity comes from the per-episode endpoints later;
// title-level import stays conservative on purpose.
int simkl_status_visto(const char *status) {
  return status && *status && !strcasecmp(status, "completed");
}

// SIMKL ids carry the imdb id with the "tt" prefix; normalize defensively so
// a bare number still matches the catalog instead of silently missing.
void simkl_normalizar_imdb(const char *id, char *dst, size_t tam) {
  size_t i = 0, k = 0;
  if (!dst || !tam) return;
  dst[0] = 0;
  if (!id) return;
  while (id[i] == ' ' || id[i] == '\t') i++;
  if ((id[i] == 't' || id[i] == 'T') && (id[i + 1] == 't' || id[i + 1] == 'T')) {
    snprintf(dst, tam, "tt%s", id + i + 2);
    return;
  }
  { int soDigitos = 1;
    size_t j;
    for (j = i; id[j]; j++)
      if (id[j] < '0' || id[j] > '9') { soDigitos = 0; break; }
    if (soDigitos && j > i) {
      snprintf(dst, tam, "tt%s", id + i);
      return;
    } }
  while (id[i] && k + 1 < tam) dst[k++] = id[i++];
  dst[k] = 0;
}

// Authed GET, same wire shape as the official Kodi addon: simkl-api-key
// header + Bearer token + the client_id/app-name/app-version query the PIN
// flow already uses, plus the descriptive User-Agent SIMKL mandates (without
// it the server answers 403). Returns the body (free it) or NULL.
static char *baixarAutenticado(const char *caminho, int *status) {
  char completo[600], cid[200], nome[120], chave[400], aut[400];
  const char *cab[5];
  if (status) *status = 0;
  nuvem_url_escapar(nuvem_simkl_cliente(), cid, sizeof cid);
  nuvem_url_escapar(nuvem_simkl_app()[0] ? nuvem_simkl_app() : "nuvio",
                    nome, sizeof nome);
  snprintf(completo, sizeof completo,
           "%s%s?client_id=%s&app-name=%s&app-version=1.0.1",
           SMK_BASE, caminho, cid, nome);
  snprintf(chave, sizeof chave, "simkl-api-key: %s", nuvem_simkl_cliente());
  snprintf(aut, sizeof aut, "Authorization: Bearer %s", simklauth_token());
  cab[0] = "Accept: application/vnd.api+json";
  cab[1] = chave;
  cab[2] = aut;
  cab[3] = "User-Agent: nuvio/1.0.1";
  cab[4] = NULL;
  return rede_baixar_st(completo, 25, cab, status);
}

// One list response into the history mirror. Returns marks applied.
// Shape (verified against MALSync's live client): {"shows":[{...}]}, i.e. an
// OBJECT keyed by type — not a bare array. Each item:
// {"show":{...,"ids":{"imdb":"tt..."}},"status":"completed",...} — the list
// status lives at depth 1 of each item (the nested media object has its own
// "status" for airing state, which js_texto would find first;
// js_texto_raiz_em reads depth 1 only, which is why it exists). A root array
// still works as fallback.
static int importar(const char *corpo, const char *chaveLista,
                    const char *chaveMidia, const char *tipo) {
  const char *arr, *p;
  int n = 0, sondado = 0;
  if (!corpo) return 0;
  arr = js_array(corpo, NULL, chaveLista);
  if (!arr) arr = js_raiz_array(corpo);
  if (!arr) {
    printf("[simkl] %s: sem array (http ok, forma desconhecida)\n", tipo);
    fflush(stdout);
    return 0;
  }
  for (p = arr; p; p = js_prox(js_fim(p))) {
    const char *f = js_fim(p);
    // Media objects carry poster/fanart URLs: 2 KB truncates the big ones
    // and js_bruto then refuses the whole item. 4 KB on a worker stack.
    char bruto[4096], status[24] = "", imdb[24] = "", norm[24] = "";
    if (!js_bruto(p, f, chaveMidia, bruto, sizeof bruto)) continue;
    if (!sondado) {
      sondado = 1;
      printf("[simkl] amostra %s: midia=1 status=%d imdb=%d\n", tipo,
             js_texto_raiz_em(p, f, "status", status, sizeof status) == 0,
             js_texto(bruto, bruto + strlen(bruto), "imdb", imdb, sizeof imdb) == 0);
      fflush(stdout);
      status[0] = 0;
      imdb[0] = 0;
    }
    if (!js_texto_raiz_em(p, f, "status", status, sizeof status)) continue;
    if (!js_texto(bruto, bruto + strlen(bruto), "imdb", imdb, sizeof imdb)) continue;
    simkl_normalizar_imdb(imdb, norm, sizeof norm);
    if (!norm[0] || !simkl_status_visto(status)) continue;
    cat_historico_definir_id(norm, tipo, 1);
    n++;
  }
  return n;
}

static pthread_t fio;
static int fioVivo, puxado;
// Ultimo scrobble enviado (dedup de start repetido). Zera junto do puxado,
// porque os dois sao estado DA SESSAO com um vinculo: trocar de perfil sem
// zerar faria o primeiro play do perfil novo sumir como "repetido".
static char ultAcao[8];
static char ultImdb[16];
static int ultT, ultE;

static void *fioPuxar(void *u) {
  int stF = 0, stS = 0, nF = 0, nS = 0;
  char *corpo;
  (void)u;
  corpo = baixarAutenticado("/sync/movies", &stF);
  if (corpo) { nF = importar(corpo, "movies", "movie", "movie"); free(corpo); }
  corpo = baixarAutenticado("/sync/shows", &stS);
  if (corpo) { nS = importar(corpo, "shows", "show", "series"); free(corpo); }
  // Anime lists are tvdb-centric and the catalog keys on imdb: skipped in
  // phase 1 rather than matched wrong.
  if (stF == 401 || stS == 401) {
    printf("[simkl] token recusado (HTTP 401): vincule de novo em Ajustes\n");
    fflush(stdout);
  }
  printf("[simkl] assistidos: %d filmes, %d series (http %d/%d)\n",
         nF, nS, stF, stS);
  fflush(stdout);
  fioVivo = 0;
  return NULL;
}

void simkl_puxar(void) {
  if (puxado || fioVivo) return;
  if (simklauth_estado() != SMK_LIGADO || !simklauth_token()[0]) return;
  if (!nuvem_simkl_cliente()[0]) return;
  puxado = 1;
  if (pthread_create(&fio, NULL, fioPuxar, NULL) != 0) { puxado = 0; return; }
  pthread_detach(fio);
  fioVivo = 1;
  printf("[simkl] puxando assistidos…\n");
  fflush(stdout);
}

void simkl_esquecer(void) {
  puxado = 0;
  ultAcao[0] = 0;
  ultImdb[0] = 0;
  ultT = ultE = 0;
}

// POST helper shared by the write threads: authed like everything else,
// Content-Type comes from rede_postar itself. Returns the HTTP status
// (0 = transport failure).
static char *chamarSimkl(const char *caminho, const char *corpo, int *st,
                         int timeout) {
  char url[600], cid[200], nome[120], chave[400], aut[400];
  const char *cab[5];
  if (st) *st = 0;
  nuvem_url_escapar(nuvem_simkl_cliente(), cid, sizeof cid);
  nuvem_url_escapar(nuvem_simkl_app()[0] ? nuvem_simkl_app() : "nuvio",
                    nome, sizeof nome);
  snprintf(url, sizeof url,
           "%s%s?client_id=%s&app-name=%s&app-version=1.0.1",
           SMK_BASE, caminho, cid, nome);
  snprintf(chave, sizeof chave, "simkl-api-key: %s", nuvem_simkl_cliente());
  snprintf(aut, sizeof aut, "Authorization: Bearer %s", simklauth_token());
  cab[0] = "Accept: application/vnd.api+json";
  cab[1] = chave;
  cab[2] = aut;
  cab[3] = "User-Agent: nuvio/1.0.1";
  cab[4] = NULL;
  return rede_postar_st(url, timeout, cab, corpo, st);
}
static int postarSimkl(const char *caminho, const char *corpo) {
  char *resp;
  int st = 0;
  resp = chamarSimkl(caminho, corpo, &st, 25);
  if (resp) free(resp);
  return st;
}

int simkl_corpo_lista(char *dst, size_t tam, const char *titulo,
                      const char *imdb, int serie, int adicionar) {
  Jsw w;
  if (!dst || !tam) return 0;
  dst[0] = 0;
  if (!titulo || !titulo[0] || !imdb || !imdb[0]) return 0;
  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  if (!serie) {
    jsw_chave(&w, "movies"); jsw_arr_ini(&w); jsw_obj_ini(&w);
    jsw_cs(&w, "title", titulo);
    jsw_chave(&w, "ids"); jsw_obj_ini(&w);
    jsw_cs(&w, "imdb", imdb);
    jsw_obj_fim(&w);
    if (adicionar) jsw_cs(&w, "to", "plantowatch");
    jsw_obj_fim(&w); jsw_arr_fim(&w);
  } else {
    jsw_chave(&w, "shows"); jsw_arr_ini(&w); jsw_obj_ini(&w);
    jsw_cs(&w, "title", titulo);
    jsw_chave(&w, "ids"); jsw_obj_ini(&w);
    jsw_cs(&w, "imdb", imdb);
    jsw_obj_fim(&w);
    if (adicionar) jsw_cs(&w, "to", "plantowatch");
    jsw_obj_fim(&w); jsw_arr_fim(&w);
  }
  // The web client always sends both keys, one of them empty.
  if (!serie) { jsw_chave(&w, "shows"); jsw_arr_ini(&w); jsw_arr_fim(&w); }
  else        { jsw_chave(&w, "movies"); jsw_arr_ini(&w); jsw_arr_fim(&w); }
  jsw_obj_fim(&w);
  if (w.erro) { jsw_livre(&w); return 0; }
  snprintf(dst, tam, "%s", jsw_texto_final(&w));
  jsw_livre(&w);
  return dst[0] != 0;
}

struct ListaPed { char imdb[16]; char tipo[8]; int adicionar; };

static void *fioLista(void *u) {
  struct ListaPed *p = u;
  char corpo[1024];
  int st = 0;
  // POST 1/sec per the dashboard rules: user-paced presses never hit it.
  if (!p) return NULL;
  { int idx = cat_indice_por_imdb(p->imdb);
    const CatItem *ci = idx >= 0 ? cat_item(idx) : NULL;
    int serie = !strcmp(p->tipo, "series");
    if (!ci || !ci->titulo[0] ||
        !simkl_corpo_lista(corpo, sizeof corpo, ci->titulo, p->imdb,
                           serie, p->adicionar)) {
      printf("[simkl] lista recusada local: %s\n", p->imdb);
      fflush(stdout);
      free(p);
      return NULL;
    }
    st = postarSimkl(p->adicionar ? "/sync/add-to-list" : "/sync/history/remove",
                     corpo);
  }
  printf("[simkl] lista %s %s: http %d\n",
         p->imdb, p->adicionar ? "guardar" : "tirar", st);
  fflush(stdout);
  free(p);
  return NULL;
}

int simkl_lista(const char *imdb, const char *tipo, int adicionar) {
  struct ListaPed *p;
  pthread_t f;
  if (!imdb || !imdb[0]) return 0;
  if (simklauth_estado() != SMK_LIGADO || !simklauth_token()[0]) return 0;
  if (!nuvem_simkl_cliente()[0]) return 0;
  p = malloc(sizeof *p);
  if (!p) return 0;
  snprintf(p->imdb, sizeof p->imdb, "%s", imdb);
  snprintf(p->tipo, sizeof p->tipo, "%s", tipo ? tipo : "");
  p->adicionar = adicionar ? 1 : 0;
  if (pthread_create(&f, NULL, fioLista, p) != 0) { free(p); return 0; }
  pthread_detach(f);
  return 1;
}

// POST /sync/history[/remove]. Shapes verified against SIMKL's official Kodi
// addon (mark_as_watched/mark_as_unwatched): shows carry seasons+episodes,
// movies carry title+ids. jsw escapes titles; imdb uniquely identifies.
int simkl_corpo_historico(char *dst, size_t tam, const char *titulo,
                          const char *imdb, int serie, int temporada,
                          int episodio) {
  Jsw w;
  if (!dst || !tam) return 0;
  dst[0] = 0;
  if (!titulo || !titulo[0] || !imdb || !imdb[0]) return 0;
  if (serie && (temporada <= 0 || episodio <= 0)) return 0;
  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  if (!serie) {
    jsw_chave(&w, "movies"); jsw_arr_ini(&w); jsw_obj_ini(&w);
    jsw_cs(&w, "title", titulo);
    jsw_chave(&w, "ids"); jsw_obj_ini(&w);
    jsw_cs(&w, "imdb", imdb);
    jsw_obj_fim(&w); jsw_obj_fim(&w); jsw_arr_fim(&w);
  } else {
    jsw_chave(&w, "shows"); jsw_arr_ini(&w); jsw_obj_ini(&w);
    jsw_cs(&w, "title", titulo);
    jsw_chave(&w, "ids"); jsw_obj_ini(&w);
    jsw_cs(&w, "imdb", imdb);
    jsw_obj_fim(&w);
    jsw_chave(&w, "seasons"); jsw_arr_ini(&w); jsw_obj_ini(&w);
    jsw_ci(&w, "number", temporada);
    jsw_chave(&w, "episodes"); jsw_arr_ini(&w); jsw_obj_ini(&w);
    jsw_ci(&w, "number", episodio);
    jsw_obj_fim(&w); jsw_arr_fim(&w); jsw_obj_fim(&w); jsw_arr_fim(&w);
    jsw_obj_fim(&w); jsw_arr_fim(&w);
  }
  jsw_obj_fim(&w);
  if (w.erro) { jsw_livre(&w); return 0; }
  snprintf(dst, tam, "%s", jsw_texto_final(&w));
  jsw_livre(&w);
  return dst[0] != 0;
}

struct MarcarPed {
  char imdb[16];
  char tipo[8];
  int temporada, episodio, marcar;
};

static void *fioMarcar(void *u) {
  struct MarcarPed *p = u;
  char corpo[1024];
  char url[600], cid[200], nome[120], chave[400], aut[400];
  const char *cab[5];
  char *resp;
  int st = 0;
  // POST 1/sec per the dashboard rules: user-paced presses never hit it,
  // and this thread is one press. No throttle needed until scrobbling.
  if (!p) return NULL;
  { int idx = cat_indice_por_imdb(p->imdb);
    const CatItem *ci = idx >= 0 ? cat_item(idx) : NULL;
    int serie = !strcmp(p->tipo, "series");
    if (!ci || !ci->titulo[0] ||
        !simkl_corpo_historico(corpo, sizeof corpo, ci->titulo, p->imdb,
                               serie, p->temporada, p->episodio)) {
      printf("[simkl] marcar recusado local: %s\n", p->imdb);
      fflush(stdout);
      free(p);
      return NULL;
    } }
  nuvem_url_escapar(nuvem_simkl_cliente(), cid, sizeof cid);
  nuvem_url_escapar(nuvem_simkl_app()[0] ? nuvem_simkl_app() : "nuvio",
                    nome, sizeof nome);
  snprintf(url, sizeof url,
           "%s/sync/history%s?client_id=%s&app-name=%s&app-version=1.0.1",
           SMK_BASE, p->marcar ? "/" : "/remove", cid, nome);
  snprintf(chave, sizeof chave, "simkl-api-key: %s", nuvem_simkl_cliente());
  snprintf(aut, sizeof aut, "Authorization: Bearer %s", simklauth_token());
  cab[0] = "Accept: application/vnd.api+json";
  cab[1] = chave;
  cab[2] = aut;
  cab[3] = "User-Agent: nuvio/1.0.1";
  cab[4] = NULL;
  resp = rede_postar_st(url, 25, cab, corpo, &st);
  if (resp) free(resp);
  printf("[simkl] marcar %s %s: http %d\n", p->imdb, p->marcar ? "visto" : "desmarcar", st);
  fflush(stdout);
  free(p);
  return NULL;
}

int simkl_marcar(const char *imdb, const char *tipo, int temporada,
                 int episodio, int marcar) {
  struct MarcarPed *p;
  pthread_t f;
  if (!imdb || !imdb[0]) return 0;
  if (simklauth_estado() != SMK_LIGADO || !simklauth_token()[0]) return 0;
  if (!nuvem_simkl_cliente()[0]) return 0;
  p = malloc(sizeof *p);
  if (!p) return 0;
  snprintf(p->imdb, sizeof p->imdb, "%s", imdb);
  snprintf(p->tipo, sizeof p->tipo, "%s", tipo ? tipo : "");
  p->temporada = temporada;
  p->episodio = episodio;
  p->marcar = marcar ? 1 : 0;
  if (pthread_create(&f, NULL, fioMarcar, p) != 0) { free(p); return 0; }
  pthread_detach(f);
  return 1;
}

int simkl_ligado(void) {
  return simklauth_estado() == SMK_LIGADO && simklauth_token()[0] &&
         nuvem_simkl_cliente()[0];
}

// Session shapes, verified against the web client's reader
// (simklSyncService.js progressFromPlayback): each element carries ONE media
// block ("movie", "show" or "anime") with title+ids, an optional "episode"
// block with tvdb_season/season + tvdb_number/number (+title), "progress"
// 0-100, "paused_at" (fallback "watched_at") and the session "id".
// The tvdb_* keys win like in the web client; for the 99% of shows where the
// numberings agree this matches the TMDB-numbered catalog exactly.
int simkl_continuar_ler(const char *corpo, CatItem *saida, int max) {
  const char *p;
  int n = 0;
  if (!corpo || !saida || max <= 0) return 0;
  // The body is a bare array at the root; walk it by hand.
  p = strchr(corpo, '[');
  p = p ? p + 1 : NULL;
  while (p && *p && n < max) {
    const char *f;
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p != '{') break;
    f = js_fim(p);
    {
      CatItem *d = &saida[n];
      const char *ep = strstr(p, "\"episode\"");
      const char *anime = strstr(p, "\"anime\"");
      int serie = ep && ep < f;
      char imdb[24] = "", norm[24] = "";
      memset(d, 0, sizeof *d);
      // Anime sessions are tvdb-keyed and the catalog keys on imdb: skipped
      // like in the import (see importar), rather than matched wrong.
      if (anime && anime < f) { p = js_prox(f); continue; }
      d->progresso = (int)js_num(p, f, "progress", 0.0);
      // WHEN it was paused: the only field that lets montarContinuar compare
      // a SIMKL item against an account-progress one (see instanteDaConta in
      // descoberta.c). Without it every SIMKL item sorts after dated ones.
      { char quando[40] = "";
        if (!js_texto(p, f, "paused_at", quando, sizeof quando))
          js_texto(p, f, "watched_at", quando, sizeof quando);
        if (quando[0]) d->retomadoMs = js_ms_iso(quando); }
      // The media block holds title+ids; "imdb" searched over the whole item
      // could catch the episode's, which addons accept but which does not
      // identify the work.
      { const char *bloco = strstr(p, serie ? "\"show\"" : "\"movie\"");
        if (bloco && bloco < f) {
          const char *fb = js_fim(strchr(bloco, '{'));
          js_texto(bloco, fb, "title", d->titulo, sizeof d->titulo);
          js_texto(bloco, fb, "imdb", imdb, sizeof imdb);
        } }
      simkl_normalizar_imdb(imdb, norm, sizeof norm);
      if (!norm[0]) { p = js_prox(f); continue; }
      if (serie) {
        const char *fe = js_fim(strchr(ep, '{'));
        double s = js_num(ep, fe, "tvdb_season", -1.0);
        double e = js_num(ep, fe, "tvdb_number", -1.0);
        if (s < 0.0) s = js_num(ep, fe, "season", 0.0);
        if (e < 0.0) e = js_num(ep, fe, "number", 0.0);
        d->temporada = (int)s;
        d->episodio = (int)e;
        js_texto(ep, fe, "title", d->nomeEpisodio, sizeof d->nomeEpisodio);
        snprintf(d->imdb, sizeof d->imdb, "%s:%d:%d", norm,
                 d->temporada ? d->temporada : 1, d->episodio ? d->episodio : 1);
        snprintf(d->tipo, sizeof d->tipo, "series");
      } else {
        snprintf(d->imdb, sizeof d->imdb, "%s", norm);
        snprintf(d->tipo, sizeof d->tipo, "movie");
      }
      // No playback-record side table: SIMKL has no "delete this resume
      // session" call, so there is no id worth keeping. Removing a card is
      // local + account (see ctxmenu.c); the SIMKL session expires on its own.
      n++;
    }
    p = js_prox(f);
  }
  return n;
}

int simkl_continuar(CatItem *saida, int max) {
  int st = 0, n = 0;
  char *corpo;
  if (!saida || max <= 0) return 0;
  if (!simkl_ligado()) return 0;
  corpo = baixarAutenticado("/sync/playback", &st);
  if (!corpo) {
    printf("[simkl] playback sem resposta (http %d)\n", st);
    fflush(stdout);
    return 0;
  }
  if (st == 401) {
    printf("[simkl] token recusado (HTTP 401): vincule de novo em Ajustes\n");
    fflush(stdout);
    free(corpo);
    return 0;
  }
  n = simkl_continuar_ler(corpo, saida, max);
  free(corpo);
  // Mesma guarda do continuarLocal em descoberta.c: enfeite falhando em tudo
  // (rede do arranque) nao pode zerar candidatos bons. Os itens do Simkl ja
  // trazem titulo da API, entao basta mante-los — a arte vem no proximo ciclo.
#define SMK_CW_RESERVA 16
  { static CatItem reserva[SMK_CW_RESERVA];
    int nCheios = (max <= SMK_CW_RESERVA) ? n : 0;
    if (nCheios > 0) memcpy(reserva, saida, sizeof(CatItem) * (size_t)nCheios);
    n = cinemeta_enfeitar_lote(saida, n);
    if (n == 0 && nCheios > 0) {
      memcpy(saida, reserva, sizeof(CatItem) * (size_t)nCheios);
      n = nCheios;
      printf("[simkl] enfeite falhou em %d, mantidos sem arte\n", nCheios);
      fflush(stdout);
    } }
  printf("[simkl] %d em andamento\n", n);
  fflush(stdout);
  return n;
}

struct LotePed {
  char imdb[16];
  char tipo[12];
  VistoPar pares[64];
  int n, marcar;
};

static void *fioLote(void *u) {
  struct LotePed *p = u;
  char titulo[160] = "", corpo[1024];
  int i, ok = 0;
  if (!p) return NULL;
  { int idx = cat_indice_por_imdb(p->imdb);
    const CatItem *ci = idx >= 0 ? cat_item(idx) : NULL;
    if (!ci || !ci->titulo[0]) {
      printf("[simkl] lote recusado local: %s\n", p->imdb);
      fflush(stdout);
      free(p);
      return NULL;
    }
    snprintf(titulo, sizeof titulo, "%s", ci->titulo); }
  for (i = 0; i < p->n; i++) {
    int serie = !strcmp(p->tipo, "series");
    int st;
    if (!simkl_corpo_historico(corpo, sizeof corpo, titulo, p->imdb, serie,
                               p->pares[i].temporada, p->pares[i].episodio))
      continue;
    st = postarSimkl(p->marcar ? "/sync/history" : "/sync/history/remove",
                     corpo);
    if (st >= 200 && st < 300) ok++;
    printf("[simkl] lote %s T%dE%d: http %d\n", p->imdb, p->pares[i].temporada,
           p->pares[i].episodio, st);
    fflush(stdout);
    // POST pacing per the dashboard rules: one user gesture becomes many
    // posts, and a burst is exactly what the throttle punishes. Detached
    // thread, nobody waits.
    if (i + 1 < p->n) {
      struct timespec espera = { 0, 500 * 1000 * 1000L };
      nanosleep(&espera, NULL);
    }
  }
  printf("[simkl] lote %s: %d de %d\n", p->imdb, ok, p->n);
  fflush(stdout);
  free(p);
  return NULL;
}

int simkl_marcar_lote(const char *imdb, const char *tipo,
                      const VistoPar *pares, int n, int marcar) {
  struct LotePed *p;
  pthread_t f;
  int i;
  if (!imdb || !imdb[0] || !pares || n <= 0) return 0;
  if (!simkl_ligado()) return 0;
  if (n > 64) n = 64;
  p = malloc(sizeof *p);
  if (!p) return 0;
  snprintf(p->imdb, sizeof p->imdb, "%s", imdb);
  snprintf(p->tipo, sizeof p->tipo, "%s", tipo ? tipo : "");
  for (i = 0; i < n; i++) p->pares[i] = pares[i];
  p->n = n;
  p->marcar = marcar ? 1 : 0;
  if (pthread_create(&f, NULL, fioLote, p) != 0) { free(p); return 0; }
  pthread_detach(f);
  return 1;
}

// --- perfil (POST /users/settings -> account.id, then POST /users/{id}/stats)
//
// Verified shapes (api.simkl.org): settings carries user{name,avatar} +
// account{id}; stats carries user{name,avatar}, total_mins,
// movies{completed{count},plantowatch{count}}, tv/anime per status
// {count, watched_episodes_count}, watched_last_week. No daily grid, no
// genres, no highlights — those sections stay honestly empty (see perfil.c).
// Docs warn stats is the most expensive call: it runs ONLY on explicit
// profile open (see carregarPerfil in app.c), never on a timer.

// Start of the {"chave":{...}} object, or NULL. Same hand-walk the Trakt
// reader used; keys are quoted so values can't false-match.
static const char *blocoDe(const char *p, const char *f, const char *chave) {
  char pat[40];
  const char *b;
  if (!p) return NULL;
  snprintf(pat, sizeof pat, "\"%s\"", chave);
  b = strstr(p, pat);
  if (!b || (f && b >= f)) return NULL;
  b = strchr(b, '{');
  if (!b || (f && b >= f)) return NULL;
  return b;
}

int simkl_perfil_ler(const char *corpoCfg, const char *corpoStats,
                     PerfilDados *d) {
  const char *u;
  int filmes = 0, episodios = 0, assistindo = 0, naLista = 0;
  static const char *sts[] = { "watching", "completed", "hold", "plantowatch" };
  static const char *doms[] = { "tv", "anime" };
  unsigned a, b;
  if (!d) return 0;
  memset(d, 0, sizeof *d);
  if (!corpoStats || !corpoStats[0]) return 0;
  // Identity: settings first (always present for the authed user), stats as
  // fallback (omitted on brand-new accounts).
  u = (corpoCfg && corpoCfg[0]) ? blocoDe(corpoCfg, NULL, "user") : NULL;
  if (!u) u = blocoDe(corpoStats, NULL, "user");
  if (u) {
    const char *f = js_fim(u);
    js_texto(u, f, "name", d->nome, sizeof d->nome);
    js_texto(u, f, "avatar", d->avatar, sizeof d->avatar);
  }
  // All-time totals: the Simkl page behind this call is all-time, so the
  // periodo says so instead of borrowing a month label.
  snprintf(d->periodo, sizeof d->periodo, "Todo o período");
  d->minutos = (int)js_num(corpoStats, NULL, "total_mins", 0.0);
  { const char *m = blocoDe(corpoStats, NULL, "movies");
    if (m) {
      const char *f = js_fim(m), *c;
      c = blocoDe(m, f, "completed");
      if (c) filmes = (int)js_num(c, js_fim(c), "count", 0.0);
      c = blocoDe(m, f, "plantowatch");
      if (c) naLista += (int)js_num(c, js_fim(c), "count", 0.0);
    } }
  for (a = 0; a < sizeof doms / sizeof *doms; a++) {
    const char *o = blocoDe(corpoStats, NULL, doms[a]);
    const char *f = o ? js_fim(o) : NULL;
    if (!o) continue;
    for (b = 0; b < sizeof sts / sizeof *sts; b++) {
      const char *s = blocoDe(o, f, sts[b]);
      if (!s) continue;
      { const char *sf = js_fim(s);
        int cnt = (int)js_num(s, sf, "count", 0.0);
        int w = (int)js_num(s, sf, "watched_episodes_count", 0.0);
        episodios += w;
        if (!strcmp(sts[b], "watching")) assistindo += cnt;
        if (!strcmp(sts[b], "plantowatch")) naLista += cnt; }
    }
  }
  d->filmes = filmes;
  d->episodios = episodios;
  d->assistindo = assistindo;
  d->naLista = naLista;
  return 1;
}

int simkl_perfil(PerfilDados *saida) {
  int st = 0, ok = 0;
  long id = 0;
  char *cfg = NULL, *stats = NULL;
  if (!saida) return 0;
  if (!simkl_ligado()) return 0;
  cfg = chamarSimkl("/users/settings", "{}", &st, 25);
  if (!cfg || st < 200 || st >= 300) {
    if (st == 401)
      printf("[simkl] token recusado (HTTP 401): vincule de novo em Ajustes\n");
    else
      printf("[simkl] perfil sem resposta (settings http %d)\n", st);
    fflush(stdout);
    free(cfg);
    return 0;
  }
  { const char *ac = blocoDe(cfg, NULL, "account");
    if (ac) id = (long)js_num(ac, js_fim(ac), "id", 0.0); }
  if (id <= 0) {
    printf("[simkl] perfil sem account.id; abortando\n");
    fflush(stdout);
    free(cfg);
    return 0;
  }
  { char caminho[64];
    // Expensive by design (docs: computed live per request): 60 s, and only
    // ever fired from the profile screen, never on a timer.
    snprintf(caminho, sizeof caminho, "/users/%ld/stats", id);
    stats = chamarSimkl(caminho, "{}", &st, 60);
    if (!stats || st < 200 || st >= 300) {
      printf("[simkl] perfil sem resposta (stats http %d)\n", st);
      fflush(stdout);
      free(cfg);
      free(stats);
      return 0;
    } }
  ok = simkl_perfil_ler(cfg, stats, saida);
  printf("[simkl] perfil %s: %d min, %d filmes, %d eps, assistindo %d, lista %d\n",
         saida->nome[0] ? saida->nome : "(sem nome)", saida->minutos,
         saida->filmes, saida->episodios, saida->assistindo, saida->naLista);
  fflush(stdout);
  free(cfg);
  free(stats);
  return ok;
}

// --- scrobble (POST /scrobble/start|pause|stop): o "Watching now" do Simkl.
//
// Shapes verified against the scrobble guide (api.simkl.org/guides/scrobble):
// movie carrega title+ids, show/anime carrega title+ids mais episode com
// season+number, e progress (0..100) vai junto. Sem heartbeat, sem polling:
// um POST por gesto do usuario (play, pausa, fim) — a regra de ouro do guia.
// start cria/substitui a sessao (binge troca sozinho); stop com >=80 marca
// visto; pause salva a posicao para retomada em outro aparelho.
int simkl_corpo_scrobble(char *dst, size_t tam, const char *titulo,
                         const char *imdb, int serie, int temporada,
                         int episodio, double progresso) {
  Jsw w;
  char pct[16], nSerie[16], nEp[16];
  long pi;
  if (!dst || !tam) return 0;
  dst[0] = 0;
  if (!titulo || !titulo[0] || !imdb || !imdb[0]) return 0;
  if (serie && (temporada <= 0 || episodio <= 0)) return 0;
  // Duas casas, como o guia aceita ("75.00" normaliza no servidor); %.17g
  // do jsw_num cuspiria "45.569999999999997" num progresso quebrado.
  if (!(progresso >= 0.0)) progresso = 0.0;
  if (progresso > 100.0) progresso = 100.0;
  pi = (long)(progresso * 100.0 + 0.5);
  snprintf(pct, sizeof pct, "%ld.%02ld", pi / 100, pi % 100);
  snprintf(nSerie, sizeof nSerie, "%d", temporada);
  snprintf(nEp, sizeof nEp, "%d", episodio);
  jsw_iniciar(&w);
  jsw_obj_ini(&w);
  jsw_chave(&w, "progress");
  jsw_bruto(&w, pct);
  jsw_chave(&w, serie ? "show" : "movie");
  jsw_obj_ini(&w);
  jsw_cs(&w, "title", titulo);
  jsw_chave(&w, "ids");
  jsw_obj_ini(&w);
  jsw_cs(&w, "imdb", imdb);
  jsw_obj_fim(&w);
  jsw_obj_fim(&w);
  if (serie) {
    jsw_chave(&w, "episode");
    jsw_obj_ini(&w);
    jsw_chave(&w, "season");
    jsw_bruto(&w, nSerie);
    jsw_chave(&w, "number");
    jsw_bruto(&w, nEp);
    jsw_obj_fim(&w);
  }
  jsw_obj_fim(&w);
  if (w.erro) { jsw_livre(&w); return 0; }
  snprintf(dst, tam, "%s", jsw_texto_final(&w));
  jsw_livre(&w);
  return dst[0] != 0;
}

struct ScrobPed {
  char acao[8];
  char imdb[16];
  char tipo[8];
  int temporada, episodio;
  double progresso;
};

static void *fioScrobble(void *u) {
  struct ScrobPed *p = u;
  char corpo[1024], caminho[32];
  int st = 0;
  if (!p) return NULL;
  { int idx = cat_indice_por_imdb(p->imdb);
    const CatItem *ci = idx >= 0 ? cat_item(idx) : NULL;
    int serie = !strcmp(p->tipo, "series");
    if (!ci || !ci->titulo[0] ||
        !simkl_corpo_scrobble(corpo, sizeof corpo, ci->titulo, p->imdb,
                              serie, p->temporada, p->episodio, p->progresso)) {
      printf("[simkl] scrobble %s recusado local: %s\n", p->acao, p->imdb);
      fflush(stdout);
      free(p);
      return NULL;
    } }
  snprintf(caminho, sizeof caminho, "/scrobble/%s", p->acao);
  st = postarSimkl(caminho, corpo);
  printf("[simkl] scrobble %s %s (%.0f%%): http %d\n",
         p->acao, p->imdb, p->progresso, st);
  fflush(stdout);
  free(p);
  return NULL;
}

// Ultimo scrobble enviado: um start repetido para o mesmo episodio sem
// pausa/stop no meio e a troca de fonte do proprio player (mesma sessao no
// servidor, que a substituiria de graca) — e a trava de 20 s do guia pune
// justamente a duplicata. pause/stop passam sempre: sao gestos distintos.
// (Declarados junto de fioVivo/puxado, acima: o esquecer precisa deles.)
int simkl_scrobble(const char *acao, const char *imdb, const char *tipo,
                   int temporada, int episodio, double progresso) {
  struct ScrobPed *p;
  pthread_t f;
  int serie;
  if (!acao || !imdb || !imdb[0]) return 0;
  if (strcmp(acao, "start") && strcmp(acao, "pause") && strcmp(acao, "stop"))
    return 0;
  if (!simkl_ligado()) return 0;
  serie = tipo && !strcmp(tipo, "series");
  if (serie && (temporada <= 0 || episodio <= 0)) return 0;
  if (!strcmp(acao, "start") && !strcmp(ultAcao, "start") &&
      !strcmp(ultImdb, imdb) && ultT == temporada && ultE == episodio) {
    printf("[simkl] scrobble start repetido: ignorado (%s)\n", imdb);
    fflush(stdout);
    return 1;
  }
  p = malloc(sizeof *p);
  if (!p) return 0;
  snprintf(p->acao, sizeof p->acao, "%s", acao);
  snprintf(p->imdb, sizeof p->imdb, "%s", imdb);
  snprintf(p->tipo, sizeof p->tipo, "%s", tipo ? tipo : "");
  p->temporada = temporada;
  p->episodio = episodio;
  p->progresso = progresso;
  if (pthread_create(&f, NULL, fioScrobble, p) != 0) { free(p); return 0; }
  pthread_detach(f);
  snprintf(ultAcao, sizeof ultAcao, "%s", acao);
  snprintf(ultImdb, sizeof ultImdb, "%s", imdb);
  ultT = temporada;
  ultE = episodio;
  return 1;
}
