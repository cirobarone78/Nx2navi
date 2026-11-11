// The game's own Voice Language option, read and changed while the game runs (voice_lang.c).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct Cpu;

enum { VOICE_LANGS = 9 };   // the choices of the game's option, in its order

int voice_lang_supported(void);       // this game build's option is known (Ocarina 1.1.1)
// 1 once the game has read its options (option.sav): *cur the index of the option now (-1: a value of none of the
// choices), *offered a bit per choice whose voice files the RomFS has. 0 before (the first seconds of a start) and on
// another build.
int voice_lang_state(int *cur, unsigned *offered);
int voice_lang_pending(void);         // the choice the menu asked for and the game has not taken yet, -1 none
const char *voice_lang_code(int i);   // the Switch language code of choice i ("ja", "en-US", "fr-CA", ...)
int voice_lang_name(int i);           // its UiTextId (the language's name)
int voice_lang_index(const char *code);   // the choice of a Switch language code, -1 none
void voice_lang_request(int i);       // the menu: the game's next present sets the option to choice i and saves it
void voice_lang_present(struct Cpu *c);   // nvnQueuePresentTexture, on the game's thread: applies a request
void voice_lang_options_opened(void);     // fs_host.c: the game opens save:/option.sav

#ifdef __cplusplus
}
#endif
