// O que as ABAS da tela de titulo mostram alem do elenco: nota do IMDb,
// episodios com nota, titulos relacionados e o mapa de vistos.
//
// Tudo isto ja existe no app web (renderExternalRatingsRow, o painel de notas
// por episodio e a aba "Mais como este"). As fontes aqui sao sem chave: o
// /meta do Cinemeta (nota, episodios, trailers), o catalogo local
// (relacionados) e a conta Nuvio + marcas locais (vistos via vistoep).
// Comentarios ficaram sem fonte keyless e a secao segue oculta.
#ifndef NV_EXTRAS_H
#define NV_EXTRAS_H

#define EX_COMENT_MAX 8
#define EX_REL_MAX   12

// Pede tudo de um titulo. Nao bloqueia: dispara um fio. Repetir com o mesmo
// `imdb` nao refaz o pedido. `serie` decide entre /shows e /movies.
// `tmdbId` e o id do titulo no TMDB (0 quando nao se sabe). Serve so a COLECAO,
// que o TMDB expoe apenas por id proprio — nao ha caminho por IMDb.
void extras_pedir(const char *imdb, int serie, long tmdbId);

// Nota e episodios do corpo /meta do Cinemeta, sem rede nova: imdbRating
// ("9.5") alimenta a fonte EX_IMDB e o videos[] com season/number/rating
// alimenta a grade de temporadas. Idempotente e com guarda de titulo — pode
// ser chamada pelo fetch proprio e pelo hook de desc_episodios com o mesmo
// corpo. E o que acende nota, episodios e retomada sem Trakt e sem chave.
void extras_meta_cinemeta(const char *imdbBase, const char *corpo, int serie);

// Pares (numero, decimos) do corpo de /tv/{id}/season/{n} do TMDB, so com
// voto (vote_average 0 fica de fora). Publica para teste (fixture em tests).
int extras_tmdb_temporada_ler(const char *corpo, int *epOut, int *notaOut,
                              int max);

// FONTES DE NOTA, na ordem em que o web as lista (renderExternalRatingsRow,
// metaDetailsScreen.js:3410). Todas menos IMDb vem do mdbList, que precisa da
// chave: pacote (MDBLIST_API_KEY), art/mdblist.txt ou conta, nesta ordem de
// precedencia reversa. Sem chave a fileira mostra so o IMDb, que sai do /meta
// do Cinemeta sem chave.
typedef enum {
  EX_TRAKT, EX_IMDB, EX_TMDB, EX_TOMATOES, EX_AUDIENCE, EX_METACRITIC,
  EX_LETTERBOXD, EX_NFONTES
} ExFonte;

// Le art/mdblist.txt. Sem ele o modulo funciona com o IMDb apenas.
void extras_carregar(const char *dirArte);

// Chave do mdblist vinda da CONTA. Mesmo motivo do TMDB: enquanto sair de
// art/mdblist.txt (que esta ate com modo 0600), o pacote distribui a chave de
// quem o montou.
void extras_definir_chave(const char *chave);

// Nota da fonte, CRUA multiplicada por 10 (o imdb vem com uma casa decimal e
// precisa caber em inteiro). 0 = nao ha. Divida por 10 e use
// extras_fonte_percentual() para saber se o resultado e "6.2" ou "66%".
int  extras_nota(int fonte);
int  extras_fonte_percentual(int fonte);
const char *extras_fonte_marca(int fonte);
// Caminho ABSOLUTO do arquivo de marca. Ver a nota em extras.c: relativo nao
// funciona porque o diretorio de trabalho do app nao e a pasta da arte.
const char *extras_caminho_marca(int fonte);
// Marca por NOME de arquivo, para as que nao sao fonte de nota (o wordmark do
// Trakt, "trakt_wordmark").
const char *extras_caminho_marca_nome(const char *nome);

// Comentarios: os mais curtidos primeiro, que e a ordem de `comments/likes`.
int  extras_n_comentarios(void);
const char *extras_comentario_usuario(int i);
const char *extras_comentario_texto(int i);
int  extras_comentario_curtidas(int i);
// Nota de QUEM COMENTOU (user_rating do Trakt), 0..10; 0 quando nao avaliou.
// A referencia mostra "10/10  17 curtidas" no rodape do cartao.
int  extras_comentario_nota(int i);

// COMENTARIOS DO EPISODIO, para o seletor "Série | Episódio" que a referencia
// mostra acima dos cartoes.
//
// Sao uma consulta SEPARADA no Trakt
// (/shows/<id>/seasons/<t>/episodes/<e>/comments/likes), nao um recorte da
// lista da serie — os dois conjuntos nao se cruzam. Feita SOB DEMANDA: uma
// viagem por episodio, e so quando o dono escolhe a aba "Episódio".
//
// `imdbSerie` aceita tanto "tt1234567" quanto o "tt1234567:2:4" que a lista de
// episodios usa; a parte depois do primeiro ':' e ignorada.
void extras_pedir_comentarios_ep(const char *imdbSerie, int temporada, int episodio);
int  extras_n_comentarios_ep(void);
// 1 enquanto a consulta esta em voo. Quem desenha usa isto para mostrar
// "carregando" em vez de "nao ha comentarios" — os dois estados sao a lista
// vazia, e confundi-los faz o episodio parecer sem comentario nenhum.
int  extras_comentarios_ep_carregando(void);

// 1 enquanto a busca de extras esta no ar. Quem desenha usa para mostrar
// esqueleto em vez de secao vazia.
int  extras_carregando(void);
const char *extras_comentario_ep_usuario(int i);
const char *extras_comentario_ep_texto(int i);
int  extras_comentario_ep_curtidas(int i);
int  extras_comentario_ep_nota(int i);

// NOTAS POR EPISODIO, para o painel que o web mostra em SERIE no lugar dos
// cartoes (renderSeriesRatingsPanel, metaDetailsScreen.js:3843): uma fileira de
// temporadas e uma grade de pastilhas "E<n> / nota", coloridas por faixa.
//
// Vem de UMA chamada: /shows/<id>/seasons?extended=episodes,full devolve todas
// as temporadas com a nota de cada episodio junto. Pedir episodio a episodio
// seriam dezenas de chamadas para desenhar uma aba.
#define EX_TEMP_MAX 12
#define EX_EP_MAX   30
int  extras_n_temporadas(void);
int  extras_temporada_numero(int t);
int  extras_n_eps(int t);
int  extras_ep_numero(int t, int i);
// Nota do episodio em DECIMOS (72 = 7.2); 0 = sem nota.
int  extras_ep_nota(int t, int i);

// COLECAO (franquia) do filme, para a aba que o web chama pelo nome dela.
// Vem de /movie/<id> -> belongs_to_collection -> /collection/<id>. As partes
// trazem so o id do TMDB, entao abrir uma delas passa pelo mesmo caminho do
// credito de um ator (desc_pedir_titulo_tmdb).
#define EX_COL_MAX 12
const char *extras_colecao_nome(void);
int  extras_n_colecao(void);
const char *extras_colecao_titulo(int i);
const char *extras_colecao_ano(int i);
long extras_colecao_tmdb(int i);

// EPISODIOS JA ASSISTIDOS, via vistoep (conta Nuvio + marcas locais). O card
// do episodio ganha uma mascara e um check quando o dono ja viu. Sem isto,
// quem acompanha uma serie nao tinha como saber onde parou olhando a lista.
int  extras_ep_visto(int temporada, int episodio);
// 1 apenas depois de receber o historico desta obra. Sem resposta nao inferir
// que todos os episodios estao por assistir.
int extras_progresso_pronto(void);
int extras_proximo_episodio(int *temporada, int *episodio);

// FICHA TECNICA do filme, para a secao "Detalhes do Filme".
//
// Tudo isto sai da MESMA chamada /movie/<id> que a colecao ja fazia — o corpo
// trazia status, runtime, release_date e os paises desde sempre, e o parse lia
// so belongs_to_collection e jogava o resto fora. Com
// `append_to_response=release_dates,videos` a mesma viagem passa a trazer
// tambem a classificacao etaria e os trailers. Nenhum pedido de rede novo.
//
// Vazio ("" ou 0) quando ainda nao chegou ou o TMDB nao tem o campo. Quem
// desenha deve OMITIR a linha nesse caso, nunca escrever um valor de reserva:
// ver a nota sobre a classificacao cravada em descoberta.c.
const char *extras_ficha_status(void);          // "Released", "Post Production"
int         extras_ficha_duracao(void);         // minutos; 0 = nao ha
const char *extras_ficha_paises(void);          // "United States of America, Canada"
const char *extras_ficha_classificacao(void);   // "R", "PG-13", "14"
const char *extras_ficha_lancamento(void);      // "2026-01-15"

// TRAILERS. So o que da para mostrar: id do YouTube, nome e miniatura.
//
// Playback is a fullscreen YouTube iframe overlay owned by
// tools/tizen-shell.html (same approach as the web app). Entry points:
// trailer_abrir() in trailer.c, called from the detail screen's trailer row
// (OK) and hero trailer button.
#define EX_TRAILER_MAX 6
int         extras_n_trailers(void);
const char *extras_trailer_yt(int i);        // id do video ("dQw4w9WgXcQ")
const char *extras_trailer_nome(int i);      // "Official Trailer"
// URL da miniatura. Previsivel a partir do id, sem chamada de API — e o mesmo
// caminho que o web usa (metaDetailsScreen.js:5728). Passe direto a tex_obter:
// o cache de texturas baixa e guarda qualquer URL sozinho.
const char *extras_trailer_miniatura(int i);
// Appends one keyless trailer (Cinemeta /meta `trailers[]`, YouTube id) to the
// title extras is currently showing. Ignores duplicates and anything for
// another title. Locks inside; safe from any thread. This is what fills the
// row with no Trakt and no TMDB key — TMDB entries (with names and language)
// merge alongside by the same dedup.
void extras_trailer_cinemeta(const char *imdb, const char *yt);

// Titulos relacionados, para a aba "Mais como este".
int  extras_n_relacionados(void);
const char *extras_relacionado_titulo(int i);
const char *extras_relacionado_ano(int i);
const char *extras_relacionado_imdb(int i);
// Poster do relacionado (URL). No caminho do catalogo e o poster do item
// (com queda para o backdrop); no legado do Trakt vinha de
// `extended=images`, com o https acrescentado aqui.
const char *extras_relacionado_poster(int i);
// Segunda arte (backdrop) do relacionado, para quando o poster morreu. Vazia
// quando nao ha.
const char *extras_relacionado_fundo(int i);

#endif
