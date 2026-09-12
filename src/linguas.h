// Idiomas de MIDIA: nome legivel, comparacao tolerante a variante, e a
// preferencia de audio/legenda da pessoa.
//
// POR QUE ISTO EXISTE. O port nasceu com os idiomas CRAVADOS: addons.c tinha
// duas listas ("pob","pt-br",... e "eng","en",...) e um comentario dizendo "o
// usuario pediu explicitamente estes dois grupos". Toda legenda que nao fosse
// portugues ou ingles era DESCARTADA em silencio. Isso funcionava para uma
// casa; para quem instalar o pacote e falar espanhol, significa abrir o player
// e nao encontrar legenda nenhuma, sem explicacao.
//
// DE ONDE VEM A PREFERENCIA, nesta ordem:
//   1. Ajustes desta TV, quando a pessoa escolheu explicitamente.
//   2. A CONTA. O blob de ajustes traz, sob `player_settings`:
//        subtitle_preferred_language, subtitle_secondary_language,
//        preferred_audio_language, secondary_preferred_audio_language
//      MEDIDO no app web (profileSettingsSyncService.js:1066). Os valores sao
//      codigos ISO minusculos ("en", "pt"), mais as sentinelas "none"/"off"
//      para legenda e "DEVICE"/"DEFAULT"/"ORIGINAL" para audio.
//   3. NADA. E aqui esta a regra que importa: sem preferencia, NAO SE FILTRA.
//      Mostrar tudo e a resposta honesta para "nao sei o que voce quer"; um
//      filtro chutado esconde a legenda que a pessoa procurava e ela nao tem
//      como saber que houve filtro.
#ifndef NV_LINGUAS_H
#define NV_LINGUAS_H

// Display name for the screen ("pob" -> "Português (BR)" in Portuguese UI,
// "Portuguese (BR)" in English UI). Unknown codes come back UPPERCASED, which
// at least identifies. Set from Ajustes whenever the interface language loads
// or changes; defaults to Portuguese (matches the standalone unit tests,
// which never set it).
void ling_interface_ingles(int sim);
const char *ling_nome(const char *codigo);

// O codigo casa com a preferencia? Tolera variante e as duas familias ISO:
// pedir "pt" aceita "por", "pob", "pt-BR", "ptb". Preferencia vazia casa com
// TUDO — e o modo sem filtro.
int ling_casa(const char *codigo, const char *pref);

// Preferencias em vigor. Devolvem "" quando nao ha preferencia (sem filtro) e
// "none" quando a pessoa pediu explicitamente NENHUMA legenda.
const char *ling_legenda(void);
const char *ling_legenda2(void);
const char *ling_audio(void);

// Vindas da CONTA (blob de ajustes). Nao sobrescrevem escolha local.
void ling_conta_legenda(const char *v);
void ling_conta_legenda2(const char *v);
void ling_conta_audio(const char *v);
// Secondary audio preference from the account blob
// (secondary_preferred_audio_language). Used when the primary is absent from
// the file. Ignored while this TV has its own audio choice, same rule as the
// secondary subtitle language.
void ling_conta_audio2(const char *v);
const char *ling_audio2(void);
// Device locale ("en-US", "pt-BR"...), set once at startup. Resolves the web
// "system"/"device" audio sentinel the same way the web player does
// (getStartupSystemAudioLanguageTarget): the device's own language, not "no
// filter". Empty when unknown — then those sentinels stay unfiltered.
void ling_aparelho_idioma(const char *locale);
// Current title's original language ("en", "ja"...), for the web "original"
// sentinel. Reset when a detail screen opens, filled when the title's TMDB
// ficha arrives (movies). "" when unknown — then "original" falls back to the
// device locale, which is also the web's own fallback.
void ling_titulo_original(const char *lang);
// Web-parity startup pick: index into `idiomas` (n track tags) for the
// preferred audio track, or -1 to leave the file default. Tries the primary
// target then the secondary, exact match before family match — the same order
// as findStartupPreferredAudioOption in the web player.
int ling_indice_audio(const char * const *idiomas, int n);

// Vindas dos AJUSTES desta TV. "" volta a seguir a conta.
void ling_local_legenda(const char *v);
void ling_local_audio(const char *v);

// Lista fixa oferecida em Ajustes. O indice 0 e "seguir a conta" e o 1 e "sem
// filtro"; do 2 em diante sao codigos ISO.
int         ling_opcao_n(void);
const char *ling_opcao_codigo(int i);   // "" para conta, "*" para sem filtro

#endif
