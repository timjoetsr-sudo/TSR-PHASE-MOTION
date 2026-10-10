# TSR PHASE · DEPTH · MOTION — TSR Audio

Motore di coerenza spaziale per tracce stereo: allinea fase e tempo tra L e R (PHASE), aggiunge profondità
controllata (DEPTH) e movimento/larghezza compatibili col mono (MOTION), con un fail-safe MONO SAFETY.

Formati: **AU + VST3 + Standalone su macOS** (universale Apple Silicon + Intel, macOS 11+). Produttore nella DAW: **TSR Audio**.

## Installare sul Mac
1. Pagina **Releases** di questo repository → scarica `TSR PHASE DEPTH MOTION <versione> (TSR Audio).pkg`.
2. Clic destro sul .pkg → **Apri** → Apri (l'installer non è firmato con un Developer ID Apple, quindi il primo avvio va confermato).
3. Apri la DAW e riscansiona i plugin (Ableton: Impostazioni → Plug-in → Riscansiona; Logic: Plug-in Manager).
   Il plugin compare sotto **TSR Audio**.

Dove si installa: AU `/Library/Audio/Plug-Ins/Components`, VST3 `/Library/Audio/Plug-Ins/VST3/TSR Audio`,
app `/Applications/TSR Audio`.

## Come viene costruito e verificato
Ad ogni push su `main` il workflow `.github/workflows/mac.yml` usa un Mac di GitHub e:
compila (universale), esegue i test DSP, firma ad-hoc, esegue **auval** e **pluginval strictness 10** su AU e VST3,
costruisce l'installer .pkg e lo pubblica nella Release. I log sono nell'artefatto della run e nel ramo `ci-out`.

Build locale sul Mac: `./scripts/build_mac.sh` (servono Xcode Command Line Tools e CMake ≥ 3.22).

Novità: vedi [CHANGELOG.md](CHANGELOG.md).
