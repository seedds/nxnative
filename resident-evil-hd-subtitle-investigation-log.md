# Resident Evil HD Remaster — indagine timing sottotitoli

Data del riepilogo: 16 novembre 2026

## Obiettivo

Indagare il problema dei sottotitoli originali durante i filmati di Resident Evil HD Remaster su Nintendo Switch / NX Native:
- il gioco raggiunge 120 FPS durante il gameplay;
- durante i filmati, video e audio sembrano riprodursi correttamente;
- i sottotitoli avanzano troppo lentamente e risultano fuori sincrono.

Obiettivo tecnico: capire come MT Framework calcola il tempo di simulazione durante i filmati e verificare se il clock dei sottotitoli possa seguire la cadenza effettiva del filmato, ripristinando poi i 120 FPS nel gameplay.

## Repository e ambiente

- Fork pubblico: https://github.com/H3li03/nxnative
- Branch sperimentale: `experiment/re-hd-60fps-4k`
- Branch esistente da preservare: `game/resident-evil-hd-remaster`
- Clone locale: `~/Downloads/nxnative-recovery`
- Commit pubblicato: `f738b599` — `Add experimental Resident Evil HD Remaster 120 FPS profile`

File inclusi nel commit:
- `dm/games/010050F00BC1A000/game.tsv`
- `dm/games/010050F00BC1A000/patches.py`
- `dm/gfx/nvn/hle_nvn.c`
- `dm/host/hle_fpslock.c`

Non sono stati pubblicati i file di backup o i dati shader estratti locali.

Link utili:
- Branch: https://github.com/H3li03/nxnative/tree/experiment/re-hd-60fps-4k
- Profilo gioco: https://github.com/H3li03/nxnative/blob/experiment/re-hd-60fps-4k/dm/games/010050F00BC1A000/game.tsv
- Patch Python: https://github.com/H3li03/nxnative/blob/experiment/re-hd-60fps-4k/dm/games/010050F00BC1A000/patches.py

Percorsi locali:
- ELF analizzato: `/Users/maclifedrive/games/Re/010050F00BC1A000_survey/main.elf`
- NSP: `/Users/maclifedrive/games/Re/Resident Evil [010050F00BC1A000][v0].nsp`
- Cartella build: `/Users/maclifedrive/games/Re/010050F00BC1A000_build`
- App generata: `/Users/maclifedrive/games/Re/Resident Evil.app`

Comando di build usato:
```bash
cd ~/Downloads/nxnative-recovery && python3 scripts/build_ipa_from_nsp.py "/Users/maclifedrive/games/Re" --platform mac-app --work-dir "/Users/maclifedrive/games/Re/010050F00BC1A000_build"
```

Nota: `fps_target` accetta solo `0`, `30` o `60`; impostare `120` in `game.tsv` causa errore di build. La configurazione sperimentale usa `fps_target 60` e `fps_max 120`.

## Risultati già verificati

1. Il gioco raggiunge 120 FPS nel gameplay.
2. Nei filmati `dm_movie_hz()` rileva circa 30 Hz.
3. Audio e video del filmato sembrano corretti; il problema osservato riguarda il ritmo dei sottotitoli.
4. Nessuna delle modifiche sperimentali descritte sotto ha ancora risolto il problema.

### Flusso dei timestamp video

- In `hle_codec_video.c`, per H264 `mavc_take(..., &pts)` restituisce un timestamp; `cv_put(..., pts * 1000)` lo inoltra.
- Per AV1 viene inoltrato `pic.ts`.
- `cv_put` accoda il timestamp e `fill()` chiama `dm_movie_picture(d, "nn::codec", ts, 0)`.
- In `hle_movie_avc.c`, `on_frame(CMTime pts, dur)` calcola `t = pts.value * 1000000 / pts.timescale`; `mavc_take` restituisce il PTS in microsecondi.
- `hle_movie.c` usa i timestamp dei frame decodificati e una finestra mobile di circa 500 ms per stimare `dm_movie_hz()`.

### Logica di presentazione / FPS esaminata

In `hle_nvn.c`, la logica osservata in `present_itv()` include:
```c
static int present_itv(const win_t *o) {
    int movie = dm_movie_hz(), c = fps_present_cap(), k = atomic_load(&g_fs_k);
    if (movie && fps_high()) return fstep_hz() / movie;
    return c ? (k && fstep_on() ? k : fstep_hz() / c)
             : g_display_mode ? 1 : o->interval > 0 ? o->interval : 1;
}
```

La logica osservata in `fps_step_cap()`:
```c
int movie = dm_movie_hz();
if (movie) return movie >= 50 && !thermal ? 60 : 30;
```

`NVNqueuePresentTexture` chiama, tra le altre, `high_cap_tick()`, `dm_movie_hz()`, `fps_step_cap()`, `fstep_tick()` e `dm_fps_lock_tick()`.

### Profilo FPS MT Framework del gioco

Riga esaminata in `dm_fps_lock_tick()`:
```c
{ "6bec9b23b09df46c315aa6f4bfacf91c",
  "Resident Evil MT Framework frame limit",
  0x157daa0, 0x48, FPS_F32, 1 },
```

`dm_fps_lock_tick()` legge `DM_FPS_TARGET` o `DM_GAME_FPS_TARGET`; `high` è vero quando il target è 120. Nel profilo di gioco il campo patchato è all'indirizzo base `0x157daa0`, offset `0x48`.

Sono state provate modifiche sperimentali a `nvn_fps_high_now()` per restituire 30 durante un filmato e 120/60 fuori dal filmato, e una condizione dedicata alla riga `Resident Evil MT Framework frame limit`. Il test nel gioco non ha corretto la velocità dei sottotitoli. Non considerare questa modifica una soluzione.

È stata anche provata una variazione di `mtl_link_mode_set(...)` dipendente da `dm_movie_hz()`: il filmato risultava leggermente più lento, ma non corretto (circa 40 FPS osservati). La modifica è stata annullata e ripristinata la chiamata originale:
```c
if (!link_env) mtl_link_mode_set(ok ? 2 : 1);
```

## Analisi del binario `main.elf`

Architettura ELF: AArch64 little-endian.

Simboli dinamici individuati:
- `_ZN2K26cMovie9execMovieEv` — `0x39ff0`
- `_ZN2K26cMovie12dispSubtitleEf` — `0x3a980`
- `_ZN2K26cMovie6isPlayEv` — `0x396d0`
- `_ZN2K26cMovie10execNormalEv` — `0x3a000`
- `_ZN2K26cMovie8execBackEv` — `0x3a520`
- `_ZN2K26cMovie9initMovieENS_6nMovie2IDEfbb` — `0x39790`
- `_ZN2K26cMovie15setPresentationModeEN6uMovie17PRESENTATION_MODEE` — `0x39f30`
- `_ZN2K26cMovie16mSubtitleInfoTblE` — `0x14d24a8`
- `_ZTVN2K26cMovieE` — `0x14d27a0`

`dispSubtitle(float)` riceve un valore `float` e lo confronta con la tabella dei sottotitoli.

In `execNormal()` (intorno a `0x3a3f0`) viene caricato un `float` dall'oggetto globale all'indirizzo base `0x157daa0`, offset `0x58418`, e sommato al timer del movie object a offset `0x3c`:
```asm
ldr s0, [x8, x9]       // oggetto + 0x58418
ldr s1, [x19, #0x3c]
fadd s8, s0, s1
str s8, [x19, #0x3c]
```
Questo collega il valore `+0x58418` al timer del movie object, ma non ne è ancora stata stabilita la provenienza né il significato esatto.

Sono stati trovati lettori di valori temporali in altre routine (indirizzi annotati durante l'analisi: `0x4560`, `0x650c`, `0x845c`, `0x84f0`, `0x3eb68`).

### Calcolo dei campi temporali a `0xcf290`

La routine `0xcf290` (e una seconda routine intorno a `0xcf870`) scrive tre campi nell'oggetto passato in `x19`:
- `+0x5841c = global_object[+0x48] / 30`
- `+0x58418 = max(global_object[+0x78] / (+0x48 / 30), float_global[0x12360d78])`
- `+0x58420 = object[+0x78] / object[+0x48]`

Disassemblato rilevante:
```asm
ldr s0, [x8, #0x48]
fmov s1, #30.0
fdiv s0, s0, s1
str s0, [x19, #0x5841c]

ldr s1, [x8, #0x78]
fdiv s0, s1, s0
ldr s1, [global_float]
fmax s0, s0, s1
str s0, [x19, #0x58418]
```

Attenzione: questa è una descrizione delle operazioni aritmetiche viste nel disassemblato, non una prova del significato semantico dei campi. In particolare, non è ancora identificata la routine che aggiorna `global_object[+0x48]` e `global_object[+0x78]`.

Un controllo su `0x1b84` ha mostrato che una routine di inizializzazione/esecuzione richiama `0xd1450` tramite il puntatore globale `0x157daa0`, ma il blocco disassemblato non scrive a `+0x48`.

## Stato dell'indagine e prossimo passo

Stato: **problema non risolto**. Non è stata ancora applicata una patch finale ai sottotitoli.

Prossimo passo concordato:
- individuare le istruzioni che scrivono nel campo `global_object[+0x48]`;
- non confondere le istruzioni `str ..., [registro, #0x48]` appartenenti ad altri oggetti: occorre prima dimostrare che il registro base è il puntatore all'oggetto globale caricato da `0x157daa0`;
- poi risalire al valore scritto e capire se cambia durante i filmati.

Ultimo comando richiesto all'utente (sola lettura):
```bash
xcrun llvm-objdump --disassemble "/Users/maclifedrive/games/Re/010050F00BC1A000_survey/main.elf" | grep -E 'str[[:space:]]+s[0-9]+, \[x[0-9]+, #0x48\]' | head -12
```
L'utente non ha ancora inviato il risultato di questo comando.

## Modalità di lavoro concordata

- Comunicare in italiano.
- L'utente è alle prime armi con Terminale/macOS.
- Un solo comando per volta, breve e completo, preferibilmente in sola lettura.
- Limitare rigorosamente l'output; usare `head` o filtri mirati.
- Non chiedere grandi dump o lunghi blocchi di codice.
- Non chiedere all'utente di incollare sorgenti già pubblici su GitHub: ispezionare il repository pubblico quando possibile.
- Non fare modifiche o commit senza autorizzazione esplicita.
- Distinguere chiaramente fatti verificati, ipotesi e test falliti.
- Non dichiarare risolto il problema finché l'utente non lo verifica nel gioco.
