// A regra que importa: SEM preferencia, nada e filtrado. Foi o contrario disso
// (dois idiomas cravados no codigo) que deixava quem fala espanhol sem legenda
// nenhuma, em silencio.
//
//   cc tests/linguas.c src/linguas.c -Isrc -o /tmp/t-linguas && /tmp/t-linguas
#include "linguas.h"
#include <stdio.h>
#include <string.h>

static int falhas;
static void ok(const char *oque, int cond) {
  if (!cond) { printf("FALHOU: %s\n", oque); falhas++; }
}

int main(void) {
  // Sem preferencia nenhuma: tudo passa, inclusive idioma que a tabela nem
  // conhece.
  ok("vazio aceita ingles",   ling_casa("en", ""));
  ok("vazio aceita coreano",  ling_casa("kor", ""));
  ok("vazio aceita galego",   ling_casa("glg", ""));

  // Variantes do mesmo idioma casam entre si, nas duas familias ISO.
  ok("pt casa com por",   ling_casa("por", "pt"));
  ok("pt casa com pob",   ling_casa("pob", "pt"));
  ok("pt casa com pt-BR", ling_casa("pt-BR", "pt"));
  ok("en casa com eng",   ling_casa("eng", "en"));
  ok("es NAO casa com pt", !ling_casa("spa", "pt"));

  // Codigo desconhecido pelos dois lados: comparacao crua, sem inventar.
  ok("glg casa com glg",  ling_casa("glg", "glg"));
  ok("glg nao casa cat",  !ling_casa("glg", "cat"));

  // Nomes para a tela, no idioma da interface.
  ok("nome de pob", !strcmp(ling_nome("pob"), "Português (BR)"));
  ok("nome de spa", !strcmp(ling_nome("spa"), "Espanhol"));
  ling_interface_ingles(1);
  ok("nome de pob em ingles", !strcmp(ling_nome("pob"), "Portuguese (BR)"));
  ok("nome de eng em ingles", !strcmp(ling_nome("eng"), "English"));
  ok("nome de spa em ingles", !strcmp(ling_nome("spa"), "Spanish"));
  ling_interface_ingles(0);
  // Sem nome na tabela, o CODIGO em maiusculas — diz mais que "Legenda 3".
  ok("nome de glg", !strcmp(ling_nome("glg"), "GLG"));

  // As sentinelas do app web viram "sem filtro", nunca um idioma inventado.
  ling_conta_audio("DEVICE");   ok("DEVICE = sem filtro",  !ling_audio()[0]);
  ling_conta_audio("DEFAULT");  ok("DEFAULT = sem filtro", !ling_audio()[0]);
  ling_conta_legenda("off");    ok("off = sem filtro",     !ling_legenda()[0]);
  ling_conta_legenda("es");     ok("es entra",             !strcmp(ling_legenda(), "es"));

  // A escolha desta TV ganha da conta, e voltar para "" devolve o comando a ela.
  ling_local_legenda("fr");     ok("local vence a conta",  !strcmp(ling_legenda(), "fr"));
  ling_local_legenda("");       ok("sem local, volta a conta", !strcmp(ling_legenda(), "es"));

  // "Todas" na tela e "*" no codigo, e tem de significar SEM FILTRO.
  ling_local_legenda("*");      ok("* = sem filtro",       !ling_legenda()[0]);

  // Audio secundario da conta: vale quando o primario nao esta no arquivo.
  ling_local_audio("");
  ling_aparelho_idioma("");
  ling_titulo_original("");
  ling_conta_audio("pt");
  ling_conta_audio2("en");
  ok("audio primario pt",   !strcmp(ling_audio(), "pt"));
  ok("audio secundario en", !strcmp(ling_audio2(), "en"));
  { const char *tags[] = { "por", "eng" };
    ok("primario ganha", ling_indice_audio(tags, 2) == 0); }
  { const char *tags[] = { "eng", "spa" };
    ok("secundario quando primario falta", ling_indice_audio(tags, 2) == 0); }
  { const char *tags[] = { "spa", "fra" };
    ok("sem nenhum, padrao", ling_indice_audio(tags, 2) == -1); }
  { const char *tags[] = { "eng", "por" };
    ok("primario aproximado ganha de secundario exato",
       ling_indice_audio(tags, 2) == 1); }

  // Sentinelas resolvidas como no web: sistema/dispositivo viram o aparelho,
  // original vira o titulo (e sem titulo cai no aparelho).
  ling_conta_audio("DEVICE"); ling_conta_audio2("");
  ling_aparelho_idioma("en-US");
  ok("device vira aparelho", !strcmp(ling_audio(), "en"));
  ling_conta_audio("ORIGINAL"); ling_titulo_original("ja");
  ok("original vira titulo", !strcmp(ling_audio(), "ja"));
  ling_titulo_original("");
  ok("original sem titulo cai no aparelho", !strcmp(ling_audio(), "en"));
  ling_conta_audio("DEFAULT");
  ok("default continua sem filtro", !ling_audio()[0]);
  ling_conta_audio("DEVICE");
  { const char *tags[] = { "por", "eng" };
    ok("device/en escolhe eng", ling_indice_audio(tags, 2) == 1); }

  // Escolha local vence ate sentinela resolvida, e esconde a secundaria.
  ling_local_audio("pt");
  ok("local vence device",     !strcmp(ling_audio(), "pt"));
  ok("secundario some com local", !ling_audio2()[0]);
  ling_local_audio("");

  if (falhas) { printf("%d falha(s)\n", falhas); return 1; }
  printf("linguas ok\n");
  return 0;
}
