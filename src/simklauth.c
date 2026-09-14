#include "simklauth.h"
#include "simkl.h"
#include "descoberta.h"
#include "idioma.h"
#include "nuvem.h"
#include "dados.h"
#include "rede.h"
#include "sync.h"
#include "js.h"
#include "jsw.h"
#include "perfis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SMK_ARQ  "simkl.txt"
#define SMK_BASE "https://api.simkl.com"
// O Simkl nao devolve `interval`; 5s e o passo que o app web usa.
#define SMK_POLL_MS 5000u

static SmkEstado estado = SMK_PARADO;
static char userCode[48];
static char url[200];
static char erro[200];
static char token[300];
static unsigned proximoPoll, comecouMs, limiteMs = 900000u;
// De qual perfil Nuvio e o token em memoria. 0 = seguir perfis_ativo().
// O vinculo e POR PERFIL (a conta guarda um token por p_profile_id, como o
// push ja fazia): sem isto os 4 perfis da TV dividiam um simkl.txt so e a
// biblioteca, o continuar e o resumo misturavam gente diferente.
static int perfilDono;
static int perfilDoPedido;

// Arquivo do token deste perfil. Legado sem sufixo vira o do perfil em vigor
// uma vez (ver trocar_perfil), para o vinculo feito antes desta versao nao
// evaporar no update.
static void arqPerfil(char *dst, size_t tam, int perfil) {
  if (!dst || !tam) return;
  if (perfil < 1) perfil = 1;
  if (perfil == 1) snprintf(dst, tam, "%s", SMK_ARQ);
  else snprintf(dst, tam, "simkl-%d.txt", perfil);
}

static pthread_t fio;
static int fioVivo, fioPronto, tokenNovo;

// Todo pedido leva client_id, app-name e app-version na QUERY, mais dois
// cabecalhos: simkl-api-key (igual ao addon oficial) e um User-Agent
// descritivo. Sem o User-Agent o Simkl devolve 403 mudo — foi o "nao conecta"
// relatado. Mesma forma do addon oficial do Simkl para Kodi.
static char *pegar(const char *caminho, int *status) {
  char completo[500], cid[200], nome[120], chave[400];
  const char *cab[4];
  nuvem_url_escapar(nuvem_simkl_cliente(), cid, sizeof cid);
  nuvem_url_escapar(nuvem_simkl_app()[0] ? nuvem_simkl_app() : "nuvio", nome, sizeof nome);
  snprintf(chave, sizeof chave, "simkl-api-key: %s", nuvem_simkl_cliente());
  // NO redirect param, ever: measured against the live API, any redirect=
  // on /oauth/pin answers 403, bare client_id answers 200. (The official
  // addon sends one, but this server rejects it — tested both encodings.)
  snprintf(completo, sizeof completo,
           "%s%s?client_id=%s&app-name=%s&app-version=1.0.1", SMK_BASE, caminho, cid, nome);
  cab[0] = "Accept: application/vnd.api+json";
  cab[1] = chave;
  cab[2] = "User-Agent: nuvio/1.0.1";
  cab[3] = NULL;
  return rede_baixar_st(completo, 20, cab, status);
}

// ---------------------------------------------------------------- disco

static int perfilEfetivo(void) {
  int p = perfilDono > 0 ? perfilDono : perfis_ativo();
  return p > 0 ? p : 1;
}

static void gravar(void) {
  char buf[400], arq[32];
  snprintf(buf, sizeof buf, "%s\n", token);
  arqPerfil(arq, sizeof arq, perfilEfetivo());
  dados_gravar(arq, buf);
}

int simklauth_carregar(void) {
  char arq[32], *b;
  arqPerfil(arq, sizeof arq, perfilEfetivo());
  b = dados_ler(arq);
  if (!b) return 0;
  { char *fim = b + strlen(b);
    while (fim > b && (fim[-1] == '\n' || fim[-1] == '\r')) *--fim = 0; }
  if (b[0]) { snprintf(token, sizeof token, "%s", b); estado = SMK_LIGADO; }
  free(b);
  return token[0] != 0;
}

const char *simklauth_token(void) { return token; }

// Troca o perfil em vigor: esquece o token em memoria (SEM apagar arquivo)
// e carrega o do novo perfil. Quem nao vinculou fica PARADO, e e isso mesmo:
// herdar o vinculo do perfil anterior era o defeito. Chamar ao confirmar a
// troca e no arranque, depois de perfis_carregar_ativo.
void simklauth_trocar_perfil(int perfil) {
  if (perfil < 1) perfil = 1;
  if (perfilDono == perfil) return;
  perfilDono = perfil;
  token[0] = userCode[0] = url[0] = erro[0] = 0;
  estado = SMK_PARADO;
  fioVivo = 0;
  fioPronto = 0;
  tokenNovo = 0;
  simklauth_carregar();
}

void simklauth_esquecer(void) {
  char arq[32];
  int i;
  token[0] = userCode[0] = url[0] = erro[0] = 0;
  estado = SMK_PARADO;
  // Sair apaga de TODOS os perfis: manter o token de um perfil na TV depois
  // do logout deixaria a proxima conta a um PIN de distancia dele.
  for (i = 1; i <= CONTA_PERFIL_MAX; i++) {
    arqPerfil(arq, sizeof arq, i);
    dados_apagar(arq);
  }
  simkl_esquecer();
}

// Credencial vinda da CONTA para o perfil em vigor (pull com p_profile_id).
// So quando nao ha vinculo local: quem vinculou nesta TV manda, nao recebe.
int simklauth_definir_remoto(const char *tk) {
  char arq[32];
  if (!tk || !*tk) return 0;
  if (token[0]) return 0;
  snprintf(token, sizeof token, "%s", tk);
  estado = SMK_LIGADO;
  arqPerfil(arq, sizeof arq, perfilEfetivo());
  { char buf[400];
    snprintf(buf, sizeof buf, "%s\n", token);
    dados_gravar(arq, buf); }
  printf("[simkl] vinculo da conta aplicado ao perfil %d\n", perfilEfetivo());
  fflush(stdout);
  return 1;
}

// ---------------------------------------------------------------- fluxo

static void *fioPedir(void *u) {
  char *r;
  int st = 0;
  (void)u;
  erro[0] = userCode[0] = 0;

  if (!nuvem_simkl_cliente()[0]) {
    snprintf(erro, sizeof erro, "pacote sem a chave do Simkl");
    estado = SMK_ERRO;
    fioPronto = 1;
    return NULL;
  }

  r = pegar("/oauth/pin", &st);
  if (r && st >= 200 && st < 300) {
    const char *fim = r + strlen(r);
    double expira;
    js_texto(r, fim, "user_code", userCode, sizeof userCode);
    // O campo aparece nas duas grafias na documentacao; aceitar as duas evita
    // uma tela vazia por causa de um "i" a menos.
    if (!js_texto(r, fim, "verification_url", url, sizeof url))
      js_texto(r, fim, "verification_uri", url, sizeof url);
    expira = js_num(r, fim, "expires_in", 0);
    if (expira > 30.0 && expira < 3600.0) limiteMs = (unsigned)(expira * 1000.0);
  }
  if (!userCode[0]) {
    snprintf(erro, sizeof erro, i18n("nao consegui pedir o codigo ao Simkl (HTTP %d)"), st);
    estado = SMK_ERRO;
  } else {
    if (!url[0]) snprintf(url, sizeof url, "https://simkl.com/pin");
    estado = SMK_AGUARDANDO;
  }
  free(r);
  fioPronto = 1;
  return NULL;
}

static void *fioPoll(void *u) {
  char caminho[120], *r;
  int st = 0;
  (void)u;
  snprintf(caminho, sizeof caminho, "/oauth/pin/%s", userCode);
  r = pegar(caminho, &st);
  if (r && st >= 200 && st < 300) {
    char res[16], t[300];
    const char *fim = r + strlen(r);
    js_texto(r, fim, "result", res, sizeof res);
    if (!strcmp(res, "KO")) {
      /* ainda nao autorizado */
    } else if (js_texto(r, fim, "access_token", t, sizeof t) && t[0]) {
      snprintf(token, sizeof token, "%s", t);
      tokenNovo = 1;
      estado = SMK_LIGADO;
    } else {
      // Resposta que nao e KO nem traz token: o Simkl invalidou este PIN.
      snprintf(erro, sizeof erro, "o Simkl invalidou este código");
      estado = SMK_ERRO;
    }
  } else if (st) {
    snprintf(erro, sizeof erro, i18n("falha ao consultar o Simkl (HTTP %d)"), st);
    estado = SMK_ERRO;
  }
  free(r);
  fioPronto = 1;
  return NULL;
}

static void soltar(void *(*rotina)(void *)) {
  if (fioVivo) return;
  fioPronto = 0;
  if (pthread_create(&fio, NULL, rotina, NULL) == 0) { pthread_detach(fio); fioVivo = 1; }
  else { snprintf(erro, sizeof erro, "sem fio para falar com o Simkl"); estado = SMK_ERRO; }
}

void simklauth_comecar(void) {
  if (estado == SMK_PEDINDO || estado == SMK_AGUARDANDO) return;
  erro[0] = 0;
  comecouMs = 0;
  estado = SMK_PEDINDO;
  perfilDoPedido = perfilEfetivo();
  soltar(fioPedir);
}

void simklauth_passo(unsigned agoraMs) {
  if (fioVivo && fioPronto) { fioVivo = 0; fioPronto = 0; }
  if (fioVivo) return;

  if (tokenNovo) {
    Jsw c;
    tokenNovo = 0;
    // Troca de perfil no meio do PIN: o token e do perfil que pediu, nao do
    // que esta em vigor. Aplicar aqui vincularia a conta errada — descarta e
    // a pessoa repete o gesto, que dura segundos.
    if (perfilDoPedido != perfilEfetivo()) {
      printf("[simkl] vinculo descartado (perfil trocou no meio)\n");
      fflush(stdout);
      token[0] = userCode[0] = url[0] = erro[0] = 0;
      estado = SMK_PARADO;
      return;
    }
    gravar();
    jsw_iniciar(&c);
    jsw_obj_ini(&c);
    jsw_cs(&c, "access_token", token);
    jsw_obj_fim(&c);
    sync_empurrar_credencial("simkl", jsw_texto_final(&c));
    jsw_livre(&c);
    printf("[simkl] vinculado nesta TV\n");
    fflush(stdout);
    // Fresh token: pull watched history now (startup covers the reboot case
    // once the boot hook lands; this covers linking mid-session).
    simkl_puxar();
    // E a fileira de retomada: a perna do Simkl no montarContinuar so entra
    // com o vinculo, e sem remontar ela nasceria vazia ate o proximo ciclo
    // (o mesmo "so aparece ao trocar a fonte" do progresso da conta).
    if (desc_continuar_vazio()) desc_repetir();
  }

  if (estado != SMK_AGUARDANDO) return;
  if (!comecouMs) comecouMs = agoraMs;
  if (agoraMs - comecouMs > limiteMs) {
    snprintf(erro, sizeof erro, "o código expirou");
    estado = SMK_ERRO;
    return;
  }
  if (agoraMs >= proximoPoll) {
    proximoPoll = agoraMs + SMK_POLL_MS;
    soltar(fioPoll);
  }
}

void simklauth_cancelar(void) {
  if (estado == SMK_PEDINDO || estado == SMK_AGUARDANDO || estado == SMK_ERRO)
    estado = token[0] ? SMK_LIGADO : SMK_PARADO;
}

SmkEstado   simklauth_estado(void) { return estado; }
const char *simklauth_codigo(void) { return userCode; }
const char *simklauth_url(void)    { return url; }
const char *simklauth_erro(void)   { return erro; }
