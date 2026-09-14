#ifndef NV_SIMKL_H
#define NV_SIMKL_H
#include <stddef.h>
#include "catalogo.h"
#include "vistoep.h"
#include "perfil.h"

// Watched-history import from SIMKL: the read half of replacing Trakt.
//
// Phase 1 (this file): pull watched movies/shows and feed the same local
// mirrors the account pull feeds (cat_historico_definir_id). Endpoints follow
// the dashboard's Phase-1 initial sync (/sync/shows + /sync/movies,
// sequential, no date_from — all-items is the delta endpoint for later).
// Writes (mark-as-watched, watchlist) come next, reusing the authed client.
// Reads never touch the account, so a bad parse can only under-mark, never
// corrupt anything — and every step logs to [simkl] for on-TV verification,
// because authenticated SIMKL responses can't be fetched from here.
void simkl_puxar(void);    // one-shot per boot while linked; spawns a thread
void simkl_esquecer(void); // forget the one-shot (logout)

// Fire-and-forget mark write (eye/menu): 1 if linked and the post left,
// 0 if SIMKL can't take it (unlinked, no key, or series without episode —
// the show shape needs seasons+episodes and title-only is unverified, so it
// refuses loudly instead of sending a shape that might 400 or mis-mark).
// Callers always apply the local mirror themselves; a failed post only logs.
int simkl_marcar(const char *imdb, const char *tipo, int temporada,
                 int episodio, int marcar);

// Pure body builder for the above, unit-tested (exact JSON assertions).
// dst gets the object; returns 1, or 0 when it refuses (see simkl_marcar).
int simkl_corpo_historico(char *dst, size_t tam, const char *titulo,
                          const char *imdb, int serie, int temporada,
                          int episodio);

// Fire-and-forget watchlist write (+ button): 1 if linked and the post
// left, 0 otherwise. Add puts the title on Plan to Watch, remove takes it
// off via history/remove — the web client's exact semantics (there is no
// non-destructive un-list call). Callers always apply the local mirror
// themselves; a failed post only logs.
int simkl_lista(const char *imdb, const char *tipo, int adicionar);

// Pure helpers, unit-tested in tests/simkl.c.
int  simkl_status_visto(const char *status);  // "completed" (any case) -> 1
void simkl_normalizar_imdb(const char *id, char *dst, size_t tam);

// Pure list bodies for the above (exact JSON assertions in tests).
// adicionar=1: {"movies":[{"title":..,"ids":..,"to":"plantowatch"}],...}
// adicionar=0: same media without "to", like the web client.
int simkl_corpo_lista(char *dst, size_t tam, const char *titulo,
                      const char *imdb, int serie, int adicionar);

// 1 when SIMKL can answer reads/writes (linked, token present, client id
// configured) — the single gate every SIMKL path checks, so no caller
// reimplements the triple condition and none drifts.
int simkl_ligado(void);

// "Continue watching" from SIMKL: GET /sync/playback, same CatItem contract
// as the old Trakt leg it replaces (composite imdb for series, tipo,
// temporada/episodio, progresso 0-100, retomadoMs from paused_at/watched_at),
// decorated with Cinemeta art like every other row source. Anime sessions are
// skipped (tvdb-centric, same reason as the import). 0 when unlinked or the
// fetch fails; every outcome logs to [simkl].
int simkl_continuar(CatItem *saida, int max);

// Pure parse half of the above, unit-tested: bare-array sessions into CatItems
// (no decoration — the caller decorates). Returns items filled.
int simkl_continuar_ler(const char *corpo, CatItem *saida, int max);

// Batch episode mark (episode list "seen" toggle): one thread, sequential
// POSTs to /sync/history[/remove] reusing simkl_corpo_historico, so a whole
// season costs one thread instead of one per episode. 1 if linked and the
// batch left, 0 otherwise. Local mirrors are the caller's job, as everywhere.
int simkl_marcar_lote(const char *imdb, const char *tipo,
                      const VistoPar *pares, int n, int marcar);

// Profile snapshot for Perfil e Stats: POST /users/settings (account.id +
// identity) then POST /users/{id}/stats (totals). BLOCKS — call from a worker
// (carregarPerfil already is one). 1 on HTTP 200 with a parsed snapshot
// (possibly all zeros on a fresh account — honest, not failure), 0 on
// transport/auth failure. The docs flag stats as the most expensive call, so
// this runs ONLY on explicit profile open, never on a timer.
int simkl_perfil(PerfilDados *saida);

// Pure parse half of the above, unit-tested: settings body (may be NULL) +
// stats body into PerfilDados. Returns 1, or 0 without a usable stats body.
int simkl_perfil_ler(const char *corpoCfg, const char *corpoStats,
                     PerfilDados *saida);

// Scrobble (POST /scrobble/start|pause|stop): fire-and-forget, one thread per
// user gesture — play (start), pause, close/end (stop). 1 if linked and the
// post left (or a repeated start was skipped), 0 otherwise. Series without
// episode refuses, like simkl_marcar. Shapes follow guides/scrobble:
// {"progress":P,"movie":{...}} or {"progress":P,"show":{...},"episode":{...}}.
int simkl_scrobble(const char *acao, const char *imdb, const char *tipo,
                   int temporada, int episodio, double progresso);

// Pure body builder for the above, unit-tested (exact JSON assertions).
// dst gets the object with progress first (docs order); returns 1, or 0 when
// it refuses (no title/imdb, or series without episode).
int simkl_corpo_scrobble(char *dst, size_t tam, const char *titulo,
                         const char *imdb, int serie, int temporada,
                         int episodio, double progresso);

#endif
