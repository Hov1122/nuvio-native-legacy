// Roteador de telas.
//
// Antes disto o main.c decidia entre home e detalhe com um if. Com menu, busca,
// biblioteca, ajustes e player, esse if viraria um emaranhado onde cada tela
// precisa saber das outras — e a regra de "quem come a tecla" ficaria espalhada
// por seis arquivos. Aqui existe uma tela CORRENTE e uma unica ordem de
// prioridade, escrita num lugar so.
//
// Ordem de quem recebe o D-pad, de cima para baixo:
//   1. player  — cobre a tela inteira
//   2. detalhe — camada sobre a tela corrente
//   3. menu    — camada sobre a tela corrente
//   4. a tela corrente (home, busca, biblioteca ou ajustes)
#include "app.h"
#include "registro.h"
#include "addonsui.h"
#include "login.h"
#include "sessao.h"
#include "perfis.h"
#include "perfilsel.h"
#include "sync.h"
#include "simklauth.h"
#include "simkl.h"
#include "text.h"
#include "vertudo.h"
#include "posplay.h"
#include "ctxmenu.h"
#include "marco.h"
#include <string.h>
#include "home.h"
#include "detail.h"
#include "menu.h"
#include "busca.h"
#include "biblioteca.h"
#include "perfil.h"
#include "salvos.h"
#include "salvospainel.h"
#include "salvosintro.h"
#include "social.h"
#include "ajustes.h"
#include "player.h"
#include "streams.h"
#include "video.h"
#include "addons.h"
#include "descoberta.h"
#include "faixas.h"
#include "episodios.h"
#include "vistoep.h"
#include "idioma.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <math.h>

// Link de debrid expira em minutos; um minuto e folga suficiente para o usuario
// apertar Reproduzir logo depois de abrir o titulo sem pagar uma busca a mais.
#define NV_LINK_VALIDO_MS 60000

static int aguardandoFonte;
static pthread_t fioFonte;
static _Atomic int fonteEscolhida = -2;   // release/acquire entre verificacao e UI

static PerfilDados perfilPendente;
static int perfilSucesso;
static _Atomic int perfilCarga; // 0=ocioso, 1=rede, 2=snapshot pronto
static _Atomic unsigned perfilGeracao = 1;
static pthread_mutex_t perfilTrava = PTHREAD_MUTEX_INITIALIZER;
typedef struct { unsigned geracao; int perfil; char conta[96]; } PerfilPedido;
static void *carregarPerfil(void *u) {
  PerfilPedido *pedido=u;
  PerfilDados novo={0};
  // Snapshot do Simkl, na worker: totais de todo o periodo + identidade.
  // Sem vinculo, sucesso 0 com honestidade (ver o ramo em app_atualizar).
  // BLOQUEIA de proposito — esta funcao ja e um fio destacado.
  int sucesso = simkl_perfil(&novo);
  pthread_mutex_lock(&perfilTrava);
  // A troca de conta/perfil invalida a resposta. O worker termina, mas nunca
  // publica uma identidade antiga nem deixa um snapshot obsoleto na fila.
  if(pedido->geracao==atomic_load_explicit(&perfilGeracao,memory_order_acquire) &&
     atomic_load_explicit(&perfilCarga,memory_order_relaxed)==1 && sessao_logada() &&
     perfis_ativo()==pedido->perfil && !strcmp(sessao_usuario(),pedido->conta)){
    perfilSucesso=sucesso;
    perfilPendente=novo;
    atomic_store_explicit(&perfilCarga,2,memory_order_release);
  }
  pthread_mutex_unlock(&perfilTrava);
  free(pedido);
  return NULL;
}
static void invalidarPerfil(void) {
  atomic_fetch_add_explicit(&perfilGeracao,1,memory_order_acq_rel);
  atomic_store_explicit(&perfilCarga,0,memory_order_release);
  pthread_mutex_lock(&perfilTrava);memset(&perfilPendente,0,sizeof perfilPendente);perfilSucesso=0;pthread_mutex_unlock(&perfilTrava);
  perfil_definir_dados(NULL);
}
static void pedirPerfil(void) {
  pthread_t t;
  int esperado = 0;
  PerfilPedido *pedido;
  perfil_definir_carregando(1);
  if (!atomic_compare_exchange_strong(&perfilCarga, &esperado, 1)) return;
  pedido=calloc(1,sizeof *pedido);
  if(!pedido){atomic_store(&perfilCarga,0);perfil_definir_erro("Nao foi possivel iniciar a consulta. Tente novamente.");return;}
  pedido->geracao=atomic_load_explicit(&perfilGeracao,memory_order_acquire);
  pedido->perfil=perfis_ativo();
  snprintf(pedido->conta,sizeof pedido->conta,"%s",sessao_usuario());
  if (pthread_create(&t, NULL, carregarPerfil, pedido) == 0) pthread_detach(t);
  else { free(pedido); atomic_store(&perfilCarga,0); perfil_definir_erro("Nao foi possivel iniciar a consulta. Tente novamente."); }
}

// A verificacao faz uma requisicao por fonte candidata e bloqueia; num fio
// proprio a tela segue em 60fps mostrando "Abrindo fonte".
static void *escolherFonte(void *u) {
  (void)u;
  // Ate 8: numa lista tipica de 12, as primeiras costumam ser do mesmo
  // provedor e falham juntas quando o arquivo nao esta em cache. Testar poucas
  // devolvia "nenhuma fonte serve" com fontes boas logo adiante.
  fonteEscolhida = stream_primeira_boa(8);
  return NULL;
}
#include "catalogo.h"
#include "gfx.h"
#include "catalogo.h"
#include "layout.h"
#include <stdio.h>

static Tela tela = TELA_HOME;
static int sair = 0;
// perfis_ativo() no instante em que a tela de escolha abriu. So serve para uma
// pergunta: a pessoa TROCOU de perfil, ou confirmou o mesmo? Agora que a tela
// aparece a cada arranque, confirmar o mesmo perfil e o caso comum — e recarga
// (reaplicar ajustes, um ciclo de sync inteiro de ~8 requisicoes) num perfil
// que nao mudou seria pagar o preco da troca em toda abertura do app.
static int perfilAntes = 1;

// Troca de perfil em voo: do OK na escolha ate dados novos na tela, a home
// mostra o perfil ANTERIOR sem dizer nada — ~5 s de conteudo alheio. Enquanto
// vale, um veu com a frase cobre a home. Cai na primeira publicacao depois do
// pedido, ou em 30 s se a rede nao responder (offline).
static int trocaEmAndamento = 0;
static unsigned trocaPublicacao = 0;
static Uint32 trocaDesde = 0;
#define TROCA_TETO_MS 30000u

// O detalhe precisa do retangulo REAL de onde o card saiu para o voo comecar
// dali. Cada tela que abre um titulo entrega o seu; quando nenhuma entrega
// (caso do menu ou de um indice vindo de fora), cai para a tela inteira.
static void abrirTitulo(const HomeItem *it) {
  if (it && it->arte) detail_abrir(it);
}

static void abrirPorIndice(int i) {
  const CatItem *c = cat_item(i);
  if (!c || (!c->backdrop[0] && !c->poster[0])) return;
  HomeItem it;
  GfxRect tudo = { 0, 0, NV_TELA_W, NV_TELA_H };
  // O `it` e da PILHA e esta funcao preenchia todos os campos MENOS o indice —
  // que ia como lixo. Como a biblioteca e a busca abrem por aqui, qualquer
  // titulo escolhido nelas levava ao mesmo filme. A home nao sofria porque ela
  // entrega o HomeItem inteiro, ja com o indice.
  //
  // O campo existe exatamente por causa deste defeito, e o comentario dele em
  // home.h ja avisava: "faltava, e por isso o detalhe abria sempre o item 0".
  // Zerar a struct antes garante que o proximo campo novo nasca definido em vez
  // de repetir a historia.
  memset(&it, 0, sizeof it);
  it.indice = i;
  it.rect = tudo;
  it.arte = c->backdrop[0] ? c->backdrop : c->poster;
  it.titulo = c->titulo;
  it.genero = c->genero;
  it.meta = c->meta;
  abrirTitulo(&it);
}

// Monta o id que os addons esperam. Para serie e "tt1234567:temporada:episodio";
// sem os dois numeros a resposta volta VAZIA com HTTP 200, e era por isso que o
// addons_buscar cravava ":1:1" — o que fazia toda a serie mostrar as fontes do
// episodio 1, qualquer que fosse o escolhido.
static void idDoAlvo(const CatItem *ci, char *dst, size_t n) {
  int t = 0, e = 0;
  if (!ci) { if (n) dst[0] = 0; return; }
  if (!strcmp(ci->tipo, "series") && detail_ep_foco(&t, &e) && t > 0 && e > 0)
    snprintf(dst, n, "%.*s:%d:%d", (int)strcspn(ci->imdb,":"),ci->imdb, t, e);
  else
    snprintf(dst, n, "%s", ci->imdb);
}

static void trocarTela(Tela nova) {
  if (nova == tela) return;
  tela = nova;
  // Cada tela zera o proprio estado ao ser aberta: voltar para a busca com o
  // texto de duas navegacoes atras seria lixo, nao memoria util.
  switch (tela) {
    case TELA_BUSCA:      busca_iniciar();      break;
    case TELA_BIBLIOTECA: biblioteca_iniciar(); break;
    case TELA_PERFIL:     perfil_abrir(); pedirPerfil(); break;
    case TELA_AJUSTES:    ajustes_iniciar();    break;
    default: break;
  }
}

static void alvoPlayer(char *alvo, size_t tam) {
  const CatItem *c = cat_item(player_indice());
  int t, e;
  player_episodio_atual(&t, &e);
  if (!c) { alvo[0] = 0; return; }
  if (t > 0 && e > 0) snprintf(alvo,tam,"%.*s:%d:%d",(int)strcspn(c->imdb,":"),c->imdb,t,e);
  else snprintf(alvo,tam,"%s",c->imdb);
}
static void buscarParaPlayer(void) {
  const CatItem *c = cat_item(player_indice());
  char alvo[64]; alvoPlayer(alvo,sizeof alvo);
  if (c && alvo[0]) {
    addons_buscar(alvo,c->tipo);
    addons_buscar_legendas(alvo,c->tipo);
  }
}
static void episodioDoDetalhe(void) {
  int t=0,e=0;
  detail_ep_foco(&t,&e);
  player_definir_episodio(t,e);
}

// A home carregou? Sem arte no pacote ela nao carrega, e ate agora isso
// DERRUBAVA o app: app_iniciar devolvia 0 e o main saia com codigo 1. Num
// pacote de dono isso nunca acontecia porque a arte ia junto; num pacote
// distribuivel, que nao pode levar arte nem credencial de ninguem, esse era o
// comportamento da PRIMEIRA execucao de todo mundo — o app abria e fechava,
// antes mesmo da tela de login.
static int homePronta;

int app_iniciar(const char *dirArte) {
  // Carimbo de build no log: a versao em Ajustes e estatica, entao dois
  // pacotes diferentes sao indistinguiveis na tela. Quando "nada mudou" apos
  // instalar, a primeira pergunta e sempre esta linha (tecla vermelha).
  printf("[nuvio] build %s %s\n", __DATE__, __TIME__);
  fflush(stdout);
  homePronta = home_iniciar(dirArte);
  if (!homePronta)
    printf("[app] sem arte no pacote: a home so aparece depois do primeiro sync\n");
  menu_iniciar();
  perfil_iniciar();
  // ANTES do primeiro sync e da primeira descoberta: salvos_aplicar_catalogo
  // marca `naLista` no catalogo do cache, entao o painel e a Biblioteca ja
  // abrem certos no primeiro quadro. Ler depois faria a lista local piscar.
  salvos_iniciar();
  // Sem conta, o app abre no login. Com sessao gravada ele nem passa por ela —
  // pedir o codigo de novo a cada arranque seria o mesmo que nao ter gravado.
  if (sessao_logada()) {
    tela = TELA_HOME;
    // Com sessao gravada o ciclo comeca no arranque: e ele que traz os addons
    // e os vinculos da pessoa, sem os quais a home mostra so o que veio no pacote.
    sync_iniciar();
    // A ESCOLHA DE PERFIL ABRE AQUI, e nao daqui a alguns segundos.
    //
    // Ela ja aparecia em conta com mais de um perfil, mas so quando o sync
    // respondia: a home aparecia primeiro e era COBERTA depois — o pior dos
    // dois mundos, porque a pessoa via a lista de outro perfil por um instante.
    // Com o cache de perfis lido em perfis_carregar_ativo() a pergunta ja pode
    // ser feita no primeiro quadro.
    //
    // Isto NAO custa nada ao arranque medido: home_iniciar() ali em cima ja
    // montou a home do cache ("catalogo do cache na tela", 844 ms na TV) antes
    // desta linha, e o laco de atualizacao continua alimentando a home por tras
    // da tela de escolha. A tela e uma CAMADA na frente de uma home pronta, nao
    // uma etapa antes dela.
    if (perfis_precisa_escolher()) {
      tela = TELA_ESCOLHA_PERFIL;
      perfilAntes = perfis_ativo();
      perfilsel_iniciar();
    }
  } else {
    tela = TELA_LOGIN;
    login_iniciar();
  }
  return 1;
}

void app_evento(const SDL_Event *e) {
  if (e->type == SDL_QUIT) { sair = 1; return; }

  // O PAINEL DE LOG VEM ANTES DE TUDO, inclusive do login.
  //
  // Ele e uma ferramenta de diagnostico, e o momento em que mais se precisa
  // dela e justamente aquele em que o app esta preso numa tela — o travamento
  // que motivou o painel DOM do Tizen aconteceu NO LOGIN. Roteado depois do
  // login, a tecla vermelha nao chegaria ali e o painel seria inutil onde ele
  // mais importa. Enquanto aberto ele engole todo o teclado, incluindo o KEYUP
  // do toque que o abriu ou fechou (ver a nota da armadilha em registro.c).
  if (registro_evento(e)) return;

  // O login vem antes de tudo, inclusive do player: enquanto nao ha conta o
  // resto do app nao tem dado nenhum para operar. A escolha de perfil vem logo
  // depois, porque e ela que define para QUEM o resto do app vai sincronizar.
  if (tela == TELA_LOGIN)          { login_evento(e);     return; }
  if (tela == TELA_ESCOLHA_PERFIL) { perfilsel_evento(e); return; }

  // ALTERNA O PAINEL DE SALVOS, COM REPOUSO — e o repouso e o conserto.
  //
  // A TECLA AZUL MUDOU DE DONO. Ate esta versao ela abria o painel "Sua
  // atividade" (perfil_abrir_lateral), um resumo de minutos assistidos. Agora
  // abre "Salvos". A troca foi pedida assim: o atalho mais rapido do controle
  // deve responder "o que eu guardei para ver?", que leva a uma acao, e nao
  // "quantos minutos assisti?", que e relatorio — e relatorio ganhou tela
  // inteira (MENU_PERFIL abre TELA_PERFIL direto). Quem apertava a AZUL por
  // habito e avisado uma vez pelo explicador de salvosintro.c.
  //
  // `!e->key.repeat` sozinho nao segura uma tecla SEGURADA num controle de TV.
  // A bandeira `repeat` do SDL vale para a repeticao que o PROPRIO SDL gera a
  // partir de um teclado; o firmware da TV manda a tecla segurada como uma
  // sequencia de KEYDOWNs SEPARADOS, cada um com repeat=0. Este bloco entao
  // alternava abre/fecha/abre/fecha varias vezes por segundo, e o que a pessoa
  // ve e o painel do nome dela "aparecendo e sumindo rapido demais para
  // escolher alguma coisa" — o relato do issue #11.
  //
  // 400 ms: acima do intervalo de repeticao de qualquer controle e bem abaixo
  // de dois toques deliberados. Vale para os dois sentidos (abrir e fechar),
  // porque o defeito nao distingue: o segundo evento e que sobra.
  if(e->type==SDL_KEYDOWN && !e->key.repeat &&
     (e->key.keysym.sym==SDLK_s || e->key.keysym.scancode==NV_SCANCODE_BLUE) &&
     tela==TELA_HOME && !player_aberto() && !detail_aberto() && !vertudo_aberta() && !menu_aberto()) {
    static Uint32 ultimoToque;
    Uint32 agoraTecla = SDL_GetTicks();
    if (agoraTecla - ultimoToque < 400) return;   // repeticao do firmware
    ultimoToque = agoraTecla;
    if(spainel_aberto())spainel_fechar();
    else spainel_abrir();
    return;
  }

  // O EXPLICADOR DE PRIMEIRA VEZ E O MAIS ALTO de todos, depois do login. Ele
  // aparece uma unica vez e faz uma pergunta; qualquer coisa respondendo por
  // baixo dele moveria o foco de uma tela que a pessoa nem esta vendo.
  if (sintro_aberto()) { sintro_evento(e); return; }

  // A folha de fontes fica acima de tudo: ela e uma pergunta, e enquanto ela
  // esta em pe nada mais deve responder ao D-pad.
  // Espaco passa direto para o player: e play/pause fisico (ver shell) e a
  // folha o ignora — sem isto a pausa morria na folha aberta.
  if (faixas_aberta()) {
    if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_SPACE &&
        player_aberto()) { player_evento(e); return; }
    faixas_evento(e); return;
  }
  if (episodios_aberto()) { episodios_evento(e); return; }
  if (stream_folha_aberta()) { stream_folha_evento(e); return; }
  if (player_aberto()) { player_evento(e); return; }
  if (detail_aberto()) { detail_evento(e); return; }
  if (spainel_aberto()) { spainel_evento(e); return; }
  if (menu_aberto())   { menu_evento(e);   return; }
  // "Ver tudo" fica ENTRE a home e o detalhe: ela cobre a home e o detalhe
  // cobre ela. Por isso vem depois do detalhe e antes do roteamento por tela.
  // O menu do cartaz fica ACIMA de tudo que a home mostra: ele e modal.
  if (ctx_aberto())     { ctx_evento(e);     return; }
  if (vertudo_aberta()) { vertudo_evento(e); return; }

  switch (tela) {
    case TELA_BUSCA:      busca_evento(e);      break;
    case TELA_BIBLIOTECA: biblioteca_evento(e); break;
    case TELA_PERFIL:     perfil_evento(e);     break;
    case TELA_SOCIAL:     social_evento(e);     break;
    case TELA_ADDONS:     addonsui_evento(e);   break;
    case TELA_AJUSTES:    ajustes_evento(e);    break;
    default:              home_evento(e);       break;
  }

  // O menu abre AQUI, no mesmo evento que o pediu, e nao no proximo
  // app_atualizar. Diferido por um quadro, as teclas que vierem logo depois do
  // ESQUERDA — e num controle elas vem — sao entregues a tela de tras, que
  // ainda acha que e a dona do foco.
  if (tela == TELA_HOME && home_pediu_menu()) menu_abrir();
  if (tela == TELA_BIBLIOTECA && biblioteca_pediu_menu()) menu_abrir();
}

// A tela de detalhe pode pedir para abrir OUTRO titulo (um credito da
// filmografia de um ator, um item de "Mais como este"). Quem troca e aqui, e
// nao ela: reabrir a si mesma no meio do proprio desenho e o tipo de coisa que
// quebra em silencio, e o roteador ja e o unico lugar que sabe abrir titulo.
// O botao do olho: marcar como ASSISTIDO. Grava progresso cheio no arquivo do
// app e espelha no historico do Simkl, que e a fonte que o dono usa nos outros
// aparelhos. Fica no roteador pelo mesmo motivo de tudo mais: e ele que
// conhece catalogo e Simkl, e a tela de detalhe nao precisa conhecer nenhum
// dos dois.
static void marcarAssistidoSeSolicitado(void) {
  const CatItem *c;
  int i;
  if (!detail_pediu_assistido()) return;
  i = detail_indice();
  c = cat_item(i);
  if (!c) return;
  // ALTERNA, e manda para o HISTORICO do Simkl (/sync/history, o endpoint de
  // "assisti").
  //
  // E alterna em vez de so marcar: o icone ja mostra os dois estados, entao um
  // botao que so soma nao teria como desfazer um toque errado.
  // Antes: cat_salvar_progresso(i, ..., 1.0). A guarda `durSeg <= 1.0` daquela
  // funcao devolvia antes de gravar — o espelho local nunca mudava.
  // Sem duracao conhecida do item, usa-se uma hora inteira como sentinela: o
  // que importa e a porcentagem (100% ou 0%), e e isso que sync e fileira leem.
  { int visto = (c->progresso >= 90);
    const double dur = 3600.0;
    cat_salvar_progresso(i, visto ? 0.0 : dur, dur);
    // Espelho no Simkl quando vinculado. Best-effort: o progresso acima ja
    // foi salvo localmente e na conta; um post falho so aparece no log.
    if (c->imdb[0]) {
      int t = 0, e = 0;
      detail_ep_foco(&t, &e);
      simkl_marcar(c->imdb, c->tipo, t, e, !visto);
    }
    printf("[app] assistido %s: %s\n", visto ? "desmarcado" : "marcado",
           c->titulo); fflush(stdout); }
}

static void trocaDeTituloSeSolicitada(void) {
  int alvo = detail_pediu_abrir();
  if (alvo >= 0) { abrirPorIndice(alvo); return; }
  // Titulo que veio de FORA do catalogo: a descoberta buscou o meta num fio e
  // avisa aqui quando ele entrou. Abrir no fio da rede seria mexer na tela de
  // outro fio; este e o unico lugar que abre titulo.
  { int novo = desc_titulo_pronto();
    // Pedido de dentro do PLAYER (relacionado que nao esta no catalogo): o
    // detalhe abria por baixo do video e ninguem via — "clico e nao faz nada".
    // O caminho do titulo que JA esta no catalogo (posplay_pediu_titulo)
    // encerra o player antes; este faz o mesmo.
    if (novo >= 0) { if (player_aberto()) player_encerrar(); abrirPorIndice(novo); } }
}

void app_atualizar(float dt, Uint32 agora) {
  // O backend precisa progredir mesmo no login, perfis e transicoes que
  // retornam cedo: seek pendente no Tizen e prazo de recuo DV no webOS.
  video_bombear();
  if (tela == TELA_LOGIN) {
    login_atualizar(dt, agora);
    // A troca so acontece AQUI, quando a sessao existe de verdade — nao no
    // instante em que o servidor respondeu. Assim a home nunca abre com uma
    // sessao pela metade.
    if (login_concluido()) {
      // Logo apos entrar, o primeiro ciclo de sync: e ele que descobre quantos
      // perfis a conta tem, e sem isso a tela de escolha nao teria o que
      // mostrar.
      sync_reaplicar_ajustes();
      sync_iniciar();
      tela = TELA_ESCOLHA_PERFIL;
      perfilAntes = perfis_ativo();
      perfilsel_iniciar();
      menu_definir_destino(MENU_INICIO);
    }
    return;
  }

  // Sessao perdida no meio do uso (renovacao recusada): voltar ao login e a
  // unica saida honesta. Continuar na home mostraria o catalogo de exemplo do
  // pacote como se fosse o da pessoa.
  if (!sessao_logada()) {
    invalidarPerfil();
    tela = TELA_LOGIN;
    login_iniciar();
    return;
  }

  // O vinculo do Simkl avanca TODO quadro, em qualquer tela: ele faz poll.
  // MEDIDO na TV: esta linha vivia dentro do `if` da tela de perfil (a
  // indentacao enganava), entao o poll so rodava ali — em Ajustes, onde o
  // vinculo e feito, "Aguardando a autorizacao" nunca saia do lugar mesmo
  // com a pessoa ja tendo autorizado no celular.
  simklauth_passo((unsigned)agora);

  // Fim da troca de perfil (ver o comentario nos estaticos): primeira
  // publicacao depois do pedido, ou o teto. Sem isto o veu ficaria para
  // sempre numa troca offline.
  if (trocaEmAndamento &&
      (cat_publicacoes() != trocaPublicacao ||
       (Uint32)(SDL_GetTicks() - trocaDesde) >= TROCA_TETO_MS)) {
    trocaEmAndamento = 0;
    printf("[app] troca de perfil pronta\n");
    fflush(stdout);
  }

  if (tela == TELA_ESCOLHA_PERFIL) {
    sync_passo((unsigned)agora);
    // A HOME CONTINUA SE MONTANDO POR TRAS DA PERGUNTA.
    //
    // home_atualizar() comeca com sincronizarFileiras(), que e quem transforma
    // o catalogo em fileiras. Sem esta linha, o catalogo que chega do cache e
    // da rede enquanto a tela de escolha esta em pe ficaria parado, e a home so
    // comecaria a montar DEPOIS da escolha — a pergunta cobraria do arranque os
    // segundos que o cache de 844 ms existe para economizar. Custa uma
    // atualizacao sem desenho: a tela de escolha e opaca e a home nao e pintada
    // aqui (ver desenharTelas).
    home_atualizar(dt, agora);
    perfilsel_atualizar(dt, agora);
    if (perfilsel_pediu_repetir()) { sync_iniciar(); return; }
    if (perfilsel_quer_sair()) {
      // Voltar = "segue com quem ja estava". So vale quando ha uma escolha
      // gravada e ela nao esta atras de um PIN — senao o Voltar entraria no
      // perfil 1 por acidente, ou seria a chave da fechadura.
      if (perfis_pode_dispensar()) {
        perfis_manter_ativo();
        tela = TELA_HOME;
        menu_definir_destino(MENU_INICIO);
      }
      return;
    }
    if (perfilsel_concluido()) {
      // O perfil mudou o destino do sync: rodar de novo traz os addons e o
      // progresso DESTE perfil, e nao os do perfil anterior que o primeiro
      // ciclo pegou. So quando MUDOU: confirmar o mesmo perfil e o caso comum
      // agora que a tela abre a cada arranque, e recarregar tudo ali seria
      // cobrar o preco de uma troca em toda abertura do app.
      // O CICLO PRECISA CONTINUAR MESMO QUANDO O PERFIL NAO MUDOU, e foi isso
      // que eu errei ao economizar aqui.
      //
      // rodar() PARA depois de perfis_puxar() quando ha escolha pendente — o
      // sync fica interrompido justamente esperando esta tela. Se confirmar o
      // mesmo perfil nao reinicia nada, o ciclo nunca termina e a conta NAO
      // SINCRONIZA a sessao inteira: os addons e o progresso ficam nos
      // do cache da abertura anterior. Medido na TV do dono: ele acrescentou um
      // addon na conta, confirmou o mesmo perfil, e o app seguiu listando os 4
      // addons antigos, com "[sync] ciclo interrompido" como ultima linha.
      // Confirmar o mesmo perfil e o caso COMUM, entao o defeito valia para
      // quase toda abertura de quem tem mais de um perfil.
      //
      // O que continua condicionado a troca e so o CARO: invalidarPerfil()
      // joga fora o que ja foi carregado, e reaplicar ajustes so faz sentido
      // quando o destino mudou.
      if (perfis_ativo() != perfilAntes) {
        int nProg;
        invalidarPerfil();
        sync_reaplicar_ajustes();
        // Troca de VERDADE: cada superficie abaixo guarda estado do perfil
        // anterior e nenhuma se limpa sozinha. Sem isto o continuar, os
        // vistos e o Simkl do perfil 1 seguiam na tela do perfil 2.
        simklauth_trocar_perfil(perfis_ativo());
        simkl_esquecer();
        simkl_puxar();
        vistoep_esquecer();
        cat_historico_esquecer();
        nProg = cat_reaplicar_progresso();
        printf("[app] troca de perfil: %d progresso(s) reaplicados\n", nProg);
        fflush(stdout);
        desc_remontar_fileiras();
        desc_repetir();
        trocaEmAndamento = 1;
        trocaPublicacao = cat_publicacoes();
        trocaDesde = SDL_GetTicks();
      }
      sync_iniciar();
      tela = TELA_HOME;
    }
    return;
  }

  // Um ciclo por vez, e so quando a conta existe. O passo e barato: sem fio
  // terminado ele nao faz nada.
  sync_passo((unsigned)agora);

  // A REDE DESCOBRIU PERFIS QUE O CACHE NAO TINHA: perguntar mesmo assim.
  //
  // app_iniciar ja faz esta pergunta com o cache em disco, e e por ali que ela
  // passa em toda abertura depois da primeira. Esta segunda porta cobre o caso
  // que o cache nao cobre: a PRIMEIRA vez que a conta e usada neste aparelho
  // (ou a primeira depois de um "sair"), quando quem descobre que existem dois
  // perfis e o sync, segundos depois. Sem ela o app assumiria o perfil 1 para
  // sempre nessa primeira sessao.
  if (tela == TELA_HOME && !player_aberto() && !detail_aberto() &&
      perfis_precisa_escolher()) {
    tela = TELA_ESCOLHA_PERFIL;
    perfilAntes = perfis_ativo();
    perfilsel_iniciar();
    return;
  }
  // AVISO DE PRIMEIRA EXECUCAO, e AQUI e nao antes.
  //
  // Ele ensina a tecla vermelha, e o lugar errado para isso e a frente do
  // login: la a pessoa esta tentando enquadrar um QR com o celular, e um cartao
  // por cima atrapalha a unica coisa que ela precisa fazer. Depois da escolha
  // de perfil, na primeira vez que a home aparece de verdade (homePronta, sem
  // player nem detalhe por cima), o cartao nao disputa com nada.
  //
  // Sem cartoes de primeira execucao: o aviso do log e o explicador de
  // "Salvos" sairam a pedido — a home abre limpa desde o primeiro login. O
  // painel de log continua na tecla vermelha; a escolha de onde o "+" salva
  // continua em Ajustes › Interface e conta, com o padrao na lista do Nuvio.

  // E o ciclo automatico — nunca com o player aberto: rajada de HTTP no meio
  // do video disputa CPU e rede com o decodificador.
  if (!player_aberto()) sync_periodico((unsigned)agora);

  // A LISTA LOCAL TEM DE SOBREVIVER A REPUBLICACAO DO CATALOGO.
  //
  // cat_definir_tudo apaga `naLista` de todo item, e a descoberta o chama
  // varias vezes por ciclo. Sem esta linha o titulo salvo aparecia por alguns
  // segundos e sumia sozinho — exatamente o defeito que contalib_reconciliar ja
  // tinha resolvido para a biblioteca da conta (a chamada dele vive em sync.c,
  // no fim do ciclo; esta precisa ser por quadro porque a lista local tambem
  // muda por acao do dono, e nao so quando o sync termina).
  salvos_reconciliar();

  // Durante a verificacao nao substituir a lista que os workers consultam.
  if (aguardandoFonte != 2) addons_estado();
  trocaDeTituloSeSolicitada();
  marcarAssistidoSeSolicitado();
  if (atomic_load_explicit(&perfilCarga, memory_order_acquire) == 2) {
    PerfilDados snapshot; int sucesso;
    pthread_mutex_lock(&perfilTrava); snapshot=perfilPendente; sucesso=perfilSucesso; pthread_mutex_unlock(&perfilTrava);
    if (sucesso) perfil_definir_dados(&snapshot);
    else if (!simkl_ligado()) perfil_definir_estado(PERFIL_ESTADO_INDISPONIVEL,
                               "Vincule o Simkl em Ajustes › Interface e conta para ver seu resumo.");
    else perfil_definir_estado(PERFIL_ESTADO_INDISPONIVEL,
                               "Resumo indisponível neste pacote. Seu progresso continua salvo na conta Nuvio e no Simkl.");
    atomic_store_explicit(&perfilCarga, 0, memory_order_release);
  }
  if (tela == TELA_PERFIL && perfil_pediu_atualizar()) pedirPerfil();
  // Titulo escolhido no painel de Salvos. Ele entrega o IMDb e nao um indice:
  // a lista dele inclui titulos que NAO estao no catalogo (vieram do arquivo
  // local, ver salvospainel.c), e um indice para esses nao existe. Sem item no
  // catalogo, pede o titulo a descoberta — o mesmo caminho que perfil.c e
  // social.c ja usam para um id que so eles conhecem.
  if (!detail_aberto() && !player_aberto()) {
    const char *alvo = spainel_pediu_abrir();
    if (alvo && alvo[0]) {
      int k = cat_indice_por_imdb(alvo);
      if (k >= 0) abrirPorIndice(k); else desc_pedir_titulo(alvo);
    }
  }

  if (tela==TELA_HOME) {
    CatItem pessoa;
    if(home_pediu_pessoa_social(&pessoa)) {
      social_abrir(&pessoa);trocarTela(TELA_SOCIAL);
    }
  }
  if (tela==TELA_HOME && home_pediu_social()) {
    trocarTela(TELA_AJUSTES);menu_definir_destino(MENU_AJUSTES);
  }
  // A lista de addons foi aberta DE Ajustes, entao o Back dela volta para
  // Ajustes. Cair na home aqui faria a pessoa refazer o caminho inteiro so
  // para ligar dois addons seguidos.
  if (tela == TELA_ADDONS && addonsui_quer_sair()) {
    trocarTela(TELA_AJUSTES); menu_definir_destino(MENU_AJUSTES);
  }
  if (tela == TELA_AJUSTES && ajustes_pediu_addons()) {
    addonsui_abrir();
    trocarTela(TELA_ADDONS);
  }

  // Fora da home, o Back tem para onde voltar: a home. So nela ele fecha o app.
  if (tela != TELA_HOME) {
    int fechar = (tela == TELA_BUSCA      && busca_quer_sair())
              || (tela == TELA_BIBLIOTECA && biblioteca_quer_sair())
              || (tela == TELA_PERFIL      && perfil_quer_sair())
              || (tela == TELA_SOCIAL      && social_quer_sair())
              || (tela == TELA_AJUSTES    && ajustes_quer_sair());
    if (fechar) { trocarTela(TELA_HOME); menu_definir_destino(MENU_INICIO); }
  } else if (home_quer_sair()) {
    sair = 1;
  }

  // Trocar de usuario, pedido pelo rodape da barra lateral. Vem ANTES do
  // destino: as duas coisas saem do mesmo menu, e quem pediu troca nao quer
  // mudar de aba.
  if (menu_pediu_trocar()) {
    invalidarPerfil();
    tela = TELA_ESCOLHA_PERFIL;
    perfilAntes = perfis_ativo();
    perfilsel_iniciar();
    return;
  }

  if (menu_mudou_destino()) {
    switch (menu_destino()) {
      case MENU_BUSCAR:     trocarTela(TELA_BUSCA);      break;
      case MENU_BIBLIOTECA: trocarTela(TELA_BIBLIOTECA); break;
      // DIRETO PARA A TELA, e nao mais para o painel lateral. O item do menu
      // se chama "Perfil e Stats" e abria um resumo de tres botoes por cima da
      // home — para ver as estatisticas de verdade era preciso descer ate "Ver
      // perfil completo" e apertar OK, dois passos para chegar onde o rotulo ja
      // prometia. trocarTela(TELA_PERFIL) ja chama perfil_abrir + pedirPerfil.
      case MENU_PERFIL:     trocarTela(TELA_PERFIL);     break;
      case MENU_AJUSTES:    trocarTela(TELA_AJUSTES);    break;
      default:              trocarTela(TELA_HOME);       break;
    }
  }
  // Pedidos de abrir um titulo, vindos de qualquer tela.
  if (!detail_aberto() && !player_aberto()) {
    int idx = -1;
    HomeItem it;
    if (tela == TELA_HOME && home_pediu_abrir()) {
      if (home_item_focado(&it)) abrirTitulo(&it);
    } else if (tela == TELA_BUSCA && busca_pediu_abrir(&idx)) {
      if (busca_item_focado(&it)) abrirTitulo(&it); else abrirPorIndice(idx);
    } else if (tela == TELA_BIBLIOTECA && biblioteca_pediu_abrir(&idx)) {
      abrirPorIndice(idx);
    } else if (tela == TELA_PERFIL) {
      PerfilDestaque p;
      if (perfil_item_selecionado(&p) && p.id[0]) {
        idx = cat_indice_por_imdb(p.id);
        if (idx >= 0) abrirPorIndice(idx); else desc_pedir_titulo(p.id);
      }
    } else if (tela == TELA_SOCIAL) {
      SocialItemSelecionado s;
      if (social_item_selecionado(&s) && s.imdb[0]) {
        idx=cat_indice_por_imdb(s.imdb);
        if(idx>=0)abrirPorIndice(idx); else desc_pedir_titulo(s.imdb);
      }
    }
  }

  // Botoes do detalhe: quem sabe que existe player e biblioteca e o roteador,
  // nao a tela de detalhe.
  if (detail_aberto()) {
    // Reproduzir sem escolher = modo automatico: a regra do stream_automatico
    // (MP4 4K Dolby Vision primeiro, senao o primeiro da lista) decide sozinha.
    // Sem lista, o player abre sem video em vez de nao abrir — a tela dizendo
    // que nao ha fonte e melhor que um botao que parece nao responder.
    // Ao abrir um titulo, perguntar as fontes JA — a busca leva segundos e
    // esperar o usuario apertar Reproduzir para so entao comecar faria a
    // primeira reproducao parecer travada.
    // O gatilho e o ID, nao o indice do titulo. Com o indice, mudar de EPISODIO
    // nao repetia a busca e a lista continuava a do episodio anterior — meia
    // correcao seria pior que nenhuma, porque a tela mostraria fontes de um
    // episodio com o nome de outro.
    { static char ultimoAlvo[32] = "";
      int i = detail_indice();
      const CatItem *ci = cat_item(i);
      char alvo[32];
      idDoAlvo(ci, alvo, sizeof alvo);
      if (!player_aberto() && aguardandoFonte != 2 && ci && ci->imdb[0] && strcmp(alvo, ultimoAlvo)) {
        snprintf(ultimoAlvo, sizeof ultimoAlvo, "%s", alvo);
        { addons_buscar(alvo, ci->tipo); }
        // Episodios do titulo aberto, na temporada onde o dono parou. Sai da
        // rede na hora: guardar a lista de episodios de 40 titulos no pacote
        // envelhecia a cada temporada nova.
        // No FILME o mesmo fio busca o /meta/movie quando o catalogo ainda nao
        // tem elenco: e de la que saem atores, direcao e generos da pagina.
        if (!strcmp(ci->tipo, "series") || ci->nElenco == 0) desc_episodios(i, 0);
        // Legendas do OpenSubtitles junto: sao dezenas por titulo e a busca
        // leva segundos. Pedir so quando o dono abre a folha de faixas faria
        // ele esperar de olho numa lista vazia.
        addons_buscar_legendas(alvo, ci->tipo);
      } }
    if (detail_pediu_reproduzir() && aguardandoFonte != 2) {
      // A tela abre JA, no estado "abrindo fonte", e a escolha acontece depois.
      // Escolher antes deixaria o botao sem resposta por segundos, e escolher
      // sem verificar entregava o video de aviso do debrid — que toca normal e
      // por isso passa por sucesso.
      const CatItem *ci = cat_item(detail_indice());
      player_abrir(detail_indice(), NULL);
      episodioDoDetalhe();
      // O episodio so fica definitivo DEPOIS de abrir o player. Refaça sempre
      // o pedido de legenda nesse ponto; a busca de prefetch pode ter comecado
      // no episodio anteriormente focado e o worker agora troca para o pedido
      // mais recente sem publicar resultados velhos.
      if (ci && ci->imdb[0]) {
        char alvoLeg[64]; alvoPlayer(alvoLeg, sizeof alvoLeg);
        addons_buscar_legendas(alvoLeg, ci->tipo);
      }
      if (stream_idade_ms() > NV_LINK_VALIDO_MS && ci && ci->imdb[0]) {
        char alvo[32]; idDoAlvo(ci, alvo, sizeof alvo);
        printf("fonte: lista com %ums, renovando (%s)\n",
               (unsigned)stream_idade_ms(), alvo);
        addons_buscar(alvo, ci->tipo);
      }
      aguardandoFonte = 1;
    }
    if (detail_pediu_marcar()) {
      // Alterna no Simkl (quando pedido) E no espelho local. O estado de
      // partida vem de ci->naLista, que a conta e a lista local preencheram;
      // sem ele o botao adicionava de novo um titulo que ja estava la.
      int i = detail_indice();
      const CatItem *c = cat_item(i);
      // A INTENCAO E CAPTURADA ANTES DE QUALQUER ESCRITA, e as tres escritas
      // usam o MESMO valor. Antes cada linha relia `c->naLista`, e a segunda
      // ja lia o campo que a primeira tinha mudado — o "+" mandava ao Simkl o
      // oposto do que gravava no espelho local sempre que a ordem mudasse. E a
      // mesma disciplina que ctxmenu.c ja aplica (e que o teste de contrato
      // cobra la).
      int entrar = c ? !c->naLista : 0;
      biblioteca_alternar_lista(i);
      // LOCAL SEMPRE, e primeiro. E o unico destino que sobrevive ao
      // fechamento do app sem depender de conta nenhuma; ver salvos.h. Sem
      // isto, quem nao tem nada vinculado apertava "+" e nao guardava nada.
      if (c) salvos_definir(c, entrar);
      // O SIMKL SO SE A PESSOA PEDIU. A escolha vem do explicador de primeira
      // vez e continua em Ajustes › Interface e conta ("Onde o + salva").
      // Best-effort como o historico: o espelho local abaixo nao espera.
      if (c && c->imdb[0] && ajustes_salvos_no_simkl())
        simkl_lista(c->imdb, c->tipo, entrar);
      if (c) cat_definir_na_lista(i, entrar);
    }
    if (detail_pediu_fontes())     stream_folha_abrir();
  }
  // Escolher uma fonte na folha inicia a reproducao DELA. Trocar de fonte com o
  // player ja aberto tambem vale: fecha a sessao atual e abre na nova, senao
  // duas ficariam presas no mesmo pipeline.
  // A busca disparada por Reproduzir terminou: agora VERIFICA as fontes, em
  // ordem, ate achar uma que leve ao arquivo — e so entao liga o video.
  if (aguardandoFonte == 1 && addons_estado() != ADD_BUSCANDO) {
    aguardandoFonte = 2;
    fonteEscolhida = -2;
    if (pthread_create(&fioFonte, NULL, escolherFonte, NULL) != 0) {
      aguardandoFonte = 0; player_erro_fonte();
    }
  }
  if (aguardandoFonte == 2 && fonteEscolhida != -2) {
    pthread_join(fioFonte,NULL);
    const Stream *s = fonteEscolhida >= 0 ? stream_item(fonteEscolhida) : NULL;
    aguardandoFonte = 0;
    printf("automatico (verificado): %s\n", s ? s->rotulo : "(nenhuma fonte serve)");
    // A afirmacao de HDR/DV vai ANTES do tocar: e ela que o bind do ACB
    // descreve ao tv.display. Sem isto o C9 exibe tudo mapeado em SDR.
    if (s) video_definir_dv(s->dolbyVision);
    // Anuncia o CONTENTOR pelo mesmo caminho: e o que dispensa a sonda de
    // Matroska num arquivo que nunca teria um cabecalho desses.
    if (s) video_definir_mp4(s->mp4 || strstr(s->url, ".mp4") != NULL);
    marco(s ? "fonte escolhida" : "nenhuma fonte serve");
    if (player_aberto() && !player_quer_sair()) {
      stream_definir_atual(fonteEscolhida);
      if (s) player_definir_fonte(s->url); else player_erro_fonte();
    }
  }

  int fonte;
  if (aguardandoFonte != 2 && stream_folha_escolheu(&fonte)) {
    const Stream *s = stream_item(fonte);
    printf("fonte escolhida: %s\n", s ? s->rotulo : "?");
    if (s) video_definir_dv(s->dolbyVision);
    if (s) {
      aguardandoFonte=0;
      int titulo=player_aberto()?player_indice():detail_indice(), t=0,e=0;
      if (player_aberto()) { player_episodio_atual(&t,&e); player_encerrar(); }
      else detail_ep_foco(&t,&e);
      player_abrir(titulo,NULL);
      player_definir_episodio(t,e);
      stream_definir_atual(fonte);
      player_definir_fonte(s->url);
    }
  }
  if (aguardandoFonte != 2 && player_pediu_fontes()) {
    stream_folha_contexto(player_linha_episodio());
    stream_folha_abrir();
  }
  if (aguardandoFonte != 2 && stream_folha_recarregar()) {
    if (player_aberto()) buscarParaPlayer();
    else {
      const CatItem *ci=cat_item(detail_indice()); char id[64];
      idDoAlvo(ci,id,sizeof id);
      if (ci) addons_buscar(id,ci->tipo);
    }
  }
  { int t,e;
    if (aguardandoFonte != 2 && episodios_escolheu(&t,&e) && player_aberto()) {
      int titulo=player_indice();
      player_encerrar(); player_abrir(titulo,NULL);
      player_definir_episodio(t,e);
      marco("buscando fontes"); buscarParaPlayer(); aguardandoFonte=1;
    }
  }
  { int t,e;
    if (aguardandoFonte != 2 && player_pediu_proximo(&t,&e) && player_aberto()) {
      int titulo=player_indice();
      player_encerrar(); player_abrir(titulo,NULL); player_definir_episodio(t,e);
      marco("proximo episodio: buscando fontes"); buscarParaPlayer(); aguardandoFonte=1;
    }
  }
  episodios_atualizar(dt);
  // O player devolve 1 para a coluna de audio e 2 para a de legenda.
  { int q = player_pediu_faixas();
    if (q) faixas_abrir_em(q == 2 ? 1 : 0); }
  faixas_atualizar(dt, agora);
  stream_folha_atualizar(dt, agora);

  // SAIR DO PLAYER PODE PRECISAR REFAZER "CONTINUAR ASSISTINDO".
  //
  // Relato: "assisto um filme ou episodio, saio do player, e o titulo so
  // aparece em Continuar assistindo depois de fechar e reabrir o app".
  // Certissimo: o progresso e gravado na hora, mas a FILEIRA e montada durante
  // a descoberta (montarContinuar), e nada a refazia ao sair.
  //
  // POR QUE desc_repetir() E NAO UMA REMONTAGEM BARATA, que era o meu primeiro
  // reflexo: montarContinuar chama a rede (simkl_continuar, enfeite Cinemeta)
  // e usa buffers `static` do fio de descoberta — chama-la daqui seria I/O
  // bloqueante no fio de desenho E corrida com aquele fio. desc_repetir() ja e
  // a resposta da casa para "esta fileira precisa ser refeita": ajustes.c a usa
  // quando a FONTE do Continuar assistindo muda, que e o mesmo problema.
  //
  // SO QUANDO O TITULO NAO ESTA LA. Continuar algo que ja esta na fileira e o
  // caso comum, e ali nada mudou de lugar — pagar um ciclo inteiro (~20 s nesta
  // TV) a cada saida do player seria cobrar do comum o preco do raro.
  if (player_quer_sair() && !player_aberto()) {
    int idx = player_indice();
    const CatItem *ci = idx >= 0 ? cat_item(idx) : NULL;
    int naFileira = 0;
    if (ci && ci->imdb[0]) {
      int r;
      for (r = 0; r < cat_n_fileiras() && !naFileira; r++) {
        const CatFileira *f = cat_fileira(r);
        if (!f || strcmp(f->chave, "continue_watching")) continue;
        { int k;
          for (k = 0; k < f->n; k++) {
            const CatItem *it = cat_item(f->ini + k);
            if (it && !strcmp(it->imdb, ci->imdb)) { naFileira = 1; break; }
          } }
      }
      if (!naFileira) {
        printf("[home] %s nao estava em Continuar assistindo: refazendo\n",
               ci->imdb);
        fflush(stdout);
        desc_repetir();
      }
    }
    player_encerrar();
  }

  player_atualizar(dt, agora);
  detail_atualizar(dt, agora);
  // A biblioteca e full-bleed: a faixa fixa comeria a primeira coluna da
  // grade (x=96 contra 144 da faixa). A camada continua abrindo em toda tela.
  menu_rail_fixa_visivel(tela != TELA_BIBLIOTECA);
  menu_atualizar(dt, agora);
  // PÓS-REPRODUÇÃO. O proximo episodio reabre a busca de fonte com o id novo;
  // o titulo relacionado sai do player e abre o detalhe, que e onde o dono
  // escolhe se quer mesmo assistir.
  { int t = 0, e = 0;
    if (aguardandoFonte != 2 && posplay_pediu_episodio(&t, &e) && player_aberto()) {
      // O MESMO CAMINHO do botao de proximo episodio do player, logo acima, e
      // nao um atalho proprio.
      //
      // Este bloco tinha um caminho curto (compor o id na mao e chamar
      // addons_buscar) que NUNCA rodou: posplay_evento zerava o pedido ao
      // fechar o painel, e so a correcao em posplay.c abriu este portao. Ao
      // abrir, tres coisas que o atalho nao fazia passaram a importar:
      //
      //   - player_encerrar SALVA o progresso do episodio que acabou. Sem ele,
      //     maratonar nunca marcava nada como visto.
      //   - player_definir_episodio atualiza epT/epE, o rotulo da barra e os
      //     marcos de introducao. Sem ele o player seguia se dizendo no
      //     episodio anterior, e o proximo player_encerrar gravaria o
      //     progresso do episodio NOVO por cima do ANTIGO.
      //   - buscarParaPlayer pede LEGENDA tambem, e compoe o id por alvoPlayer
      //     (que ja corta no ':' — era o que o atalho reimplementava a mao).
      int titulo = player_indice();
      player_encerrar();
      player_abrir(titulo, NULL);
      player_definir_episodio(t, e);
      // DEPOIS de player_definir_episodio, e a ordem e o conserto de um bug
      // que so existe nesta ordem invertida: definir_episodio retoma a posicao
      // quando o episodio pedido E o que o item aponta. Apontar antes faria o
      // episodio NOVO herdar o progresso do que acabou — quem apertasse OK aos
      // 85% comecaria o seguinte aos 85%.
      //
      // Apontar, porem, tem de acontecer: e de ci->temporada/ci->episodio que
      // o proximo pos-reproducao descobre qual episodio oferecer.
      cat_apontar_episodio(titulo, t, e);
      marco("posplay: proximo episodio");
      buscarParaPlayer();
      aguardandoFonte = 1;
    } }
  { int i = posplay_pediu_titulo();
    if (i >= 0) {
      const CatItem *ci = cat_item(i);
      HomeItem it;
      player_encerrar();
      memset(&it, 0, sizeof it);
      it.indice = i;
      it.rect = (GfxRect){ NV_TELA_W * 0.5f - 124.0f, NV_TELA_H * 0.5f - 186.0f,
                           248.0f, 372.0f };
      it.arte   = ci ? (ci->poster[0] ? ci->poster : ci->backdrop) : NULL;
      it.titulo = ci ? ci->titulo : NULL;
      it.genero = ci ? ci->genero : NULL;
      it.meta   = ci ? ci->meta : NULL;
      detail_abrir(&it);
    } }

  vertudo_atualizar(dt, agora);
  ctx_atualizar(dt, agora);
  { int i = ctx_pediu_detalhes();
    if (i >= 0) {
      const CatItem *ci = cat_item(i);
      HomeItem it;
      memset(&it, 0, sizeof it);
      it.indice = i;
      it.rect = (GfxRect){ NV_TELA_W * 0.5f - 124.0f, NV_TELA_H * 0.5f - 186.0f,
                           248.0f, 372.0f };
      it.arte   = ci ? (ci->poster[0] ? ci->poster : ci->backdrop) : NULL;
      it.titulo = ci ? ci->titulo : NULL;
      it.genero = ci ? ci->genero : NULL;
      it.meta   = ci ? ci->meta : NULL;
      detail_abrir(&it);
    } }
  // Titulo escolhido na grade: abre o detalhe, como se tivesse vindo da home.
  { int idx = vertudo_pediu_abrir();
    if (idx >= 0) {
      const CatItem *ci = cat_item(idx);
      // A grade nao tem retangulo de origem para a transicao crescer a partir
      // dele: o card fica na tela que esta saindo. Entra centrado, do tamanho
      // de um cartaz — o detalhe cobre a tela em seguida de qualquer forma.
      HomeItem it;
      memset(&it, 0, sizeof it);
      it.indice = idx;
      it.rect = (GfxRect){ NV_TELA_W * 0.5f - 124.0f, NV_TELA_H * 0.5f - 186.0f,
                           248.0f, 372.0f };
      it.arte   = ci ? (ci->poster[0] ? ci->poster : ci->backdrop) : NULL;
      it.titulo = ci ? ci->titulo : NULL;
      it.genero = ci ? ci->genero : NULL;
      it.meta   = ci ? ci->meta : NULL;
      detail_abrir(&it);
    } }
  switch (tela) {
    case TELA_BUSCA:      busca_atualizar(dt, agora);      break;
    case TELA_BIBLIOTECA: biblioteca_atualizar(dt, agora); break;
    case TELA_PERFIL:     break;
    case TELA_AJUSTES:    ajustes_atualizar(dt, agora);    break;
    default:              home_atualizar(dt, agora);       break;
  }
  perfil_atualizar(dt, agora);
  spainel_atualizar(dt, agora);
  sintro_atualizar(dt, agora);
  if(tela==TELA_SOCIAL) social_atualizar(dt, agora);
  if(tela==TELA_ADDONS) addonsui_atualizar(dt, agora);
}

// O corpo do desenho de TELA. Saiu de app_desenhar para uma funcao propria por
// um motivo unico: os `return` daqui (login, escolha de perfil, estado vazio da
// home) impediam qualquer coisa de ser desenhada DEPOIS deles, e o painel de
// log tem de aparecer tambem nessas telas — que sao exatamente onde o app ja
// travou uma vez.
static void desenharTelas(Uint32 agora) {
  if (tela == TELA_LOGIN)          { login_desenhar(agora);     return; }
  if (tela == TELA_ESCOLHA_PERFIL) { perfilsel_desenhar(agora); return; }

  // A HOME PODE FICAR PRONTA DEPOIS DO ARRANQUE, e ate agora ninguem reparava.
  //
  // homePronta saia de home_iniciar() e nunca mais era reavaliada. Num pacote
  // COM arte prebaked ela nasce 1 e o defeito nao existe — que e o caso de todo
  // pacote que este projeto ja montou. Num pacote SEM arte (o distribuivel, e o
  // do alvo Tizen, onde o .wgt e read-only) ela nasce 0, o catalogo chega pelo
  // sync segundos depois, home_atualizar monta as fileiras — e o app segue
  // desenhando "Preparando seu catalogo..." para sempre, com 32 fileiras
  // prontas atras. MEDIDO no navegador: "[home] 32 fileiras vindas do catalogo"
  // no log, estado vazio na tela.
  if (!homePronta && home_tem_fileiras()) {
    homePronta = 1;
    printf("[app] catalogo chegou depois do arranque: home liberada\n");
    fflush(stdout);
  }

  // Estado vazio de verdade, em vez de uma tela preta que parece travamento.
  if (!homePronta && tela == TELA_HOME && !player_aberto() && !detail_aberto()) {
    GfxRect fundo = { 0, 0, NV_TELA_W, NV_TELA_H };
    TxtLinha t, sb;
    gfx_cor(fundo, 0.0f, NV_COR_FUNDO_R, NV_COR_FUNDO_G, NV_COR_FUNDO_B, 1.0f);
    t = txt_linha(TXT_TITULO2, "Preparando seu catálogo…", 255, 255, 255, 255);
    txt_desenhar(t, (NV_TELA_W - t.w) * 0.5f, 460.0f);
    sb = txt_linha(TXT_BODY,
                   sync_estado() == SYNC_RODANDO
                     ? "Buscando seus addons e o que você estava assistindo."
                     : "Se isto não sair daqui, confira seus addons na conta.",
                   160, 162, 170, 255);
    txt_desenhar(sb, (NV_TELA_W - sb.w) * 0.5f, 546.0f);
    if (menu_visivel()) menu_desenhar(agora);
    return;
  }

  // O player cobre tudo; desenhar o que esta atras dele e trabalho jogado fora
  // — a mesma conta que ja valia para o cartao de detalhe esticado.
  if (!player_aberto()) {
    // "Ver tudo" cobre a tela de tras por completo (fundo opaco), entao a home
    // nao precisa ser desenhada por baixo — a mesma conta do detail_cobre_tela.
    if (!detail_cobre_tela() && !vertudo_aberta()) {
      switch (tela) {
        case TELA_BUSCA:      busca_desenhar(agora);      break;
        case TELA_BIBLIOTECA: biblioteca_desenhar(agora); break;
        case TELA_PERFIL:     perfil_desenhar(agora);     break;
        case TELA_SOCIAL:     social_desenhar(agora);     break;
        case TELA_ADDONS:     addonsui_desenhar(agora);   break;
        case TELA_AJUSTES:    ajustes_desenhar(agora);    break;
        default:              home_desenhar(agora);       break;
      }
    }
    if (!detail_cobre_tela()) vertudo_desenhar(agora);
    ctx_desenhar(agora);
    detail_desenhar(agora);
    // A rail NAO existe na tela de detalhe do app web: ela e full-bleed e a
    // coluna de conteudo comeca em x=72, ou seja, DENTRO do que a rail ocuparia.
    // Com a rail por cima, o logo, o botao "Reproduzir" e a linha de duracao
    // ficavam cortados pela faixa preta de 144px — foi o primeiro defeito que
    // apareceu na captura do aparelho depois do port.
    // A rail some com o detalhe aberto (o web nao a tem nessa tela) e some
    // tambem quando `collapseSidebar` esta ligado, que e o estado do perfil do
    // dono. Recolhida ela nao ocupa largura nenhuma: o conteudo passa a comecar
    // em 104, e quem devolve esse x e ajustes_conteudo_x().
    // A guarda de `collapseSidebar` NAO entra aqui. Ela ja existe DENTRO do
    // menu_desenhar, e la ela pula so a RAIL FIXA — que e o correto: recolhida,
    // a barra nao ocupa largura, mas continua abrindo como CAMADA ao ganhar
    // foco, exatamente como o web faz.
    //
    // Com a guarda tambem neste ponto, o menu_desenhar nunca era chamado no
    // perfil do dono (collapseSidebar ligado): o menu abria, engolia as teclas
    // e nao desenhava nada. Ficava sem menu e sem caminho para os Ajustes — foi
    // o defeito relatado como "nao ta mostrando o menu e nao tem os ajustes".
    // Guarda repetida em dois lugares para a mesma regra: no de dentro ela
    // significa "nao pinte a faixa", no de fora significava "nao exista".
    if (menu_visivel() && !detail_aberto())
      menu_desenhar(agora);
    // Depois do menu: as duas camadas de "Salvos" escurecem a tela inteira e
    // tem de ficar por cima de tudo que a home desenhou, inclusive da rail.
    if (spainel_visivel() && !detail_aberto()) spainel_desenhar(agora);
  }
  player_desenhar(agora);
  episodios_desenhar();
  stream_folha_desenhar(agora);
  faixas_desenhar(agora);
  // Veu da troca de perfil (ver os estaticos): a home de baixo e a do perfil
  // anterior ate a rede responder. Tres pontos pulsantes + frase; so
  // informacao, sem roubar tecla — a navegacao na grade velha por alguns
  // segundos e inofensiva perto de travar o controle.
  if (trocaEmAndamento && tela == TELA_HOME && !player_aberto()) {
    GfxRect veu = { 0, 0, NV_TELA_W, NV_TELA_H };
    float cx = NV_TELA_W * 0.5f, cy = NV_TELA_H * 0.5f;
    int i;
    gfx_cor(veu, 0.0f, 0, 0, 0, 0.55f);
    for (i = 0; i < 3; i++) {
      float f;
      if (ajustes_animacoes_reduzidas()) f = (i == 1) ? 1.0f : 0.45f;
      else {
        float ph = (float)((agora + (Uint32)(i * 300)) % 900) / 900.0f;
        f = 0.35f + 0.65f * (0.5f - 0.5f * cosf(ph * 6.28318f));
      }
      gfx_cor((GfxRect){ cx - 54.0f + (float)i * 54.0f - 11.0f, cy - 45.0f,
                         22.0f, 22.0f },
              0.5f, 0.96f, 0.96f, 0.98f, f);
    }
    { TxtLinha t = txt_linha(TXT_TITULO2, i18n("A trocar de perfil…"),
                             245, 246, 250, 255);
      txt_desenhar_alpha(t, cx - (float)t.w * 0.5f, cy - 2.0f, 1.0f); }
  }
}

void app_desenhar(Uint32 agora) {
  // COM O PAINEL DE LOG ABERTO A INTERFACE NAO E PINTADA.
  //
  // O painel e um cartao de tela quase cheia e opaco (alpha 0.94): pintar a
  // home por baixo dele seria uma SEGUNDA camada de tela cheia, e gfx.c ja
  // mediu que duas dessas derrubam a Mali-G71 da TV para ~40fps. Nada se perde
  // visualmente — nao ha nada visivel por baixo — e o que aparece na moldura de
  // 56px e a cor de limpeza do quadro, que o main.c ja pintou.
  //
  // As animacoes nao param por isso: elas avancam em app_atualizar, que
  // continua rodando. O video tambem continua tocando; o que ele perde e o furo
  // de alpha que o revela, entao o plano de video fica coberto enquanto o
  // painel esta em pe — que e o comportamento desejado para quem parou o app
  // para LER o log.
  if (!registro_aberto()) desenharTelas(agora);
  // O explicador fica ACIMA de qualquer tela (menos do painel de log, que e
  // ferramenta de diagnostico): ele e a primeira coisa que a pessoa ve depois
  // desta atualizacao, e nada pode aparecer por cima dele.
  if (!registro_aberto()) sintro_desenhar(agora);
  registro_desenhar();
}

int app_quer_sair(void) { return sair; }

void app_encerrar(void) {
  if (aguardandoFonte == 2) pthread_join(fioFonte, NULL);
  aguardandoFonte = 0;
  player_encerrar();
  ajustes_encerrar();
  biblioteca_encerrar();
  perfil_encerrar();
  busca_encerrar();
  home_encerrar();
}
