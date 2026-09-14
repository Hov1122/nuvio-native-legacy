#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# rg nem sempre existe (Linux sem ripgrep): grep -P entende os mesmos padroes
# PCRE usados abaixo. Todas as buscas sao de arquivo unico, entao o -n devolve
# "linha:texto" nos dois.
if command -v rg >/dev/null 2>&1; then RG=rg; else RG="grep -P"; fi

# Exercita o contrato de compilacao do escopo sem abrir SDL nem fazer rede.
cc -Wall -Wextra -Werror -fsyntax-only \
  src/ctxmenu.c src/catalogo.c src/trakt.c \
  -Isrc -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 \
  -Wno-deprecated-declarations -Wno-macro-redefined

# Regressao da intencao: ela tem de ser capturada antes do POST e o mesmo
# valor tem de chegar ao espelho local quando a resposta confirmar.
linha_intencao=$($RG -n 'intencao = !ci->naLista;' src/ctxmenu.c | cut -d: -f1)
linha_post=$($RG -n 'simkl_lista\(ci->imdb, ci->tipo, intencao\)' src/ctxmenu.c | cut -d: -f1)
# O POST e fire-and-forget e o espelho e aplicado ali mesmo, sem votacao
# posterior: a escrita local ja terminou e o Simkl sincroniza atras. O que o
# teste cobra e a ORDEM — intencao capturada antes do POST, espelho depois.
linha_espelho=$($RG -n 'cat_definir_na_lista\(atual, intencao\)' src/ctxmenu.c | cut -d: -f1 | head -1)
[ "$linha_intencao" -lt "$linha_post" ]
[ "$linha_post" -lt "$linha_espelho" ]

# O outro espelho (marcar como assistido) segue a mesma ordem: intencao,
# simkl_marcar, cat_historico_definir_id — tudo dentro de aplicar().
linha_hist_post=$($RG -n 'simkl_marcar\(ci->imdb, ci->tipo, ci->temporada, ci->episodio,' src/ctxmenu.c | cut -d: -f1 | head -1)
linha_hist_espelho=$($RG -n 'cat_historico_definir_id\(ci->imdb, ci->tipo, intencao\)' src/ctxmenu.c | cut -d: -f1 | head -1)
[ "$linha_hist_post" -lt "$linha_hist_espelho" ]

# Resposta nao nula sozinha nunca pode virar sucesso: o contrato exige HTTP
# 2xx e usa a variante que devolve o status.
$RG -q 'rede_postar_st\(url, 20, cab, corpo, &status\)' src/trakt.c
$RG -q 'confirmado = status >= 200 && status < 300' src/trakt.c

# A pressao longa continua pertencendo a home.c e chega ao modal via KEYUP.
$RG -q 'home.c:.*NV_HOLD_MS.*KEYUP' src/ctxmenu.h

# O TETO DO MENU TEM DE CABER O MENU. Issue #36: CTX_MAX era 3 e havia quatro
# opcoes possiveis ao mesmo tempo (filme com progresso), entao montar() escrevia
# em ops[3]. O sintoma foi a ultima linha nunca acender, porque focoAnim so era
# animado ate CTX_MAX. Aqui se cobra que o numero de opcoes que montar() pode
# empilhar nunca passe do vetor, e que ops[] so seja escrito por juntar().
ctx_max=$($RG -n '^#define CTX_MAX ' src/ctxmenu.c | sed 's/.*CTX_MAX *//')
n_juntar=$($RG -c '^\s*juntar\(' src/ctxmenu.c)
[ "$n_juntar" -le "$ctx_max" ] || {
  echo "ctxmenu: $n_juntar opcoes possiveis para CTX_MAX=$ctx_max" >&2; exit 1; }
$RG -q 'ops\[nOps\]\.rot = rot' src/ctxmenu.c
[ "$($RG -c 'ops\[nOps\]' src/ctxmenu.c)" -eq 1 ] || {
  echo 'ctxmenu: ops[nOps] escrito fora de juntar()' >&2; exit 1; }

# O foco tem de ser animado ate onde o desenho le. Ler focoAnim[i] com i < nOps
# enquanto o laco de animacao vai so ate CTX_MAX foi exatamente o defeito.
$RG -q 'for \(i = 0; i < CTX_MAX; i\+\+\)' src/ctxmenu.c

echo 'ctxmenu contract: PASS'
