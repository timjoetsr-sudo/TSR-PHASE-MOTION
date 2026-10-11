## 1.0.3 — PHASE più preciso e più stabile, meno CPU, automazioni pulite

Tutto misurato contro la 1.0.2 sulla stessa macchina, con una copia congelata del motore 1.0.2 (`tests/ref/`).

- **PHASE: il canale corretto non perde più gli alti.** L'interpolatore del ritardo frazionario passa da Lagrange
  di 3° ordine a windowed-sinc a 32 punti (Kaiser β = 7, tabella da 512 fasi). Ritardo di 9,5 campioni (caso peggiore):
  a 48 kHz il canale corretto perdeva −3,25 dB a 16 kHz e −8,42 dB a 20 kHz, ora +0,01 e 0,00 dB;
  a 44,1 kHz, a 20 kHz, da −13,28 a −0,57 dB.
- **Latenza riportata all'host: da 2 a 15 campioni** (0,31 ms a 48 kHz). È il costo del nuovo interpolatore; la DAW la compensa.
- **PHASE: stima del ritardo più precisa.** Picco della correlazione interpolato in banda limitata: errore sui ritardi
  a quarti di campione da 0,107 a 0,033 campioni.
- **PHASE: correlazione normalizzata per ritardo.** Sulla hi-hat ripresa con 2 microfoni (R in ritardo di 9,5 campioni,
  COHERENCE 100 %) la 1.0.2 calcolava correlazioni fino a 152 (il massimo possibile è 1) e perdeva l'aggancio in 718 blocchi
  su 5249; la 1.0.3 in 123. Correlazione L/R del risultato 0,888 → 0,998; bilancio L/R 16–20 kHz −4,06 → −0,07 dB.
- **PHASE non insegue più ritardi che cambiano nota per nota:** corregge solo se anche la media di 4 s conferma il ritardo.
  Stem PIANO (188 s, default): 12 episodi di correzione → 0.
  Costo: un ritardo nuovo che compare dopo 20 s di materiale allineato si aggancia in 4,2 s invece di 1,2 s.
  Avvio a freddo (1,3 s) e rilascio (0,8 s) invariati.
- **Automazione di DEPTH e MIX senza click:** lo shelf di DEPTH e il passa-basso del send seguono il valore a ogni campione
  (nella 1.0.2 si ricalcolavano a scatti di 0,05 dB e 50 Hz). Energia sopra 4 kHz con il parametro commutato min↔max
  ogni 250 ms: DEPTH −70,99 → −108,93 dB, MIX −72,55 → −118,65 dB.
- **Sample rate sotto 11 kHz:** le frequenze dei filtri restano sotto Nyquist. A 8 kHz, ai default, la 1.0.2 dava 47.691
  campioni non validi (NaN o Inf in L o R) su 6 s; la 1.0.3 nessuno, a ogni sample rate da 8 a 384 kHz.
- **CPU.** Meter e vettorscopio calcolati solo con la finestra aperta; filtri dei meter in 4 corsie parallele; prime
  riflessioni e filtri in cache; analisi PHASE distribuita su più blocchi. 48 kHz, blocco 512, default (Xeon 2,1 GHz, Linux):
  2,25 % di un core → 1,91 % con la finestra aperta (−15 %), 1,31 % con la finestra chiusa (−42 %).
  Blocco peggiore a 96 kHz / 32 campioni, tutto al massimo: 14,2 % → 5,6 % del tempo del blocco.
  Istruzioni eseguite (callgrind, 20 s di mix): 3,81 → 3,34 miliardi (finestra aperta), 2,48 miliardi (finestra chiusa).
  Apple Silicon (runner GitHub macos-14, macchina virtuale condivisa, misure più rumorose): 1,51 % → 1,27 % (−16 %) e
  0,93 % (−39 %); blocco peggiore a 96 kHz / 32: 14,8 % → 4,3 %.
- **Suono invariato dove PHASE non interviene:** null test su 20 stem, ai default e tutto al massimo: campioni identici
  bit per bit, o differenze sotto −480 dBFS dovute solo al punto in cui scatta la protezione dai numeri subnormali
  (con la protezione disattivata in entrambe le versioni le uscite coincidono bit per bit su tutti i 14 stem interessati).
  Unica differenza voluta: PIANO, perché la 1.0.3 non applica le correzioni nota per nota.
- Nuovi test T32–T40: EngineTests da 41 a 51.

Non cambiato, di proposito:
- Riverbero DEPTH (4 linee), risposta all'impulso identica alla 1.0.2: densità d'eco bassa all'inizio (densità d'eco
  normalizzata ≤ 0,33 fino a 80 ms, coda densa solo da ~190 ms). Cambiarlo cambierebbe il carattere.
- MONO SAFETY: comportamento identico alla 1.0.2, campione per campione. Su materiale molto largo resta nervoso: circa
  30 riduzioni brusche al minuto su PERCUSION LOOP. Un hold di 100–500 ms, provato, aumentava la riduzione media
  (RIM 5,0 → 7,6–18,1 %, PERCUSION LOOP 31,4 → 35,6–48,1 %) e su RIM anche l'oscillazione: non applicato.
- Ai default DEPTH scurisce gli alti (−2,5 dB a 8 kHz, −2,9 dB da 12 a 20 kHz) e alza di +0,4…+0,6 dB sotto i 2 kHz per
  compensare il livello (con DEPTH a 0 la risposta è piatta): è il comportamento di progetto, uguale alla 1.0.2.

## 1.0.2 — WIDTH/MOTION corretti in fase, mono safety più chiaro

- **WIDTH stringeva invece di allargare tra 2 e 3 kHz e MOTION si muoveva al contrario vicino al crossover.**
  Causa: le bande LR4 hanno lo sfasamento del filtro, che a 2,5 kHz arriva a −180°. Ora la divisione in bande è complementare
  a fase coerente (all-pass del 1° ordine a 400 Hz e 4 kHz): la somma delle bande è esattamente il segnale di ingresso.
  Misura su ESO – SAFE (MONO SAFETY 0 %, WIDTH 150 %), canale side sopra 300 Hz: +3,04 dB (ideale +3,52) contro +1,66 dB della 1.0.1.
- **MONO SAFETY:** con MONO SAFETY ≥ 90 % adesso tiene la correlazione ≥ 0,30 e la perdita in mono ≥ −1,5 dB anche quando WIDTH allarga davvero.
  Sotto il 90 % i limiti si allentano; con MONO SAFETY 0 % non riduce niente.
- **Stato più chiaro:** quando l'ingresso è già largo (correlazione < 0,5 o perdita in mono < −1,3 dB) lo stato dice
  "Stereo già largo · mono safety trattiene WIDTH/MOTION" invece di una semplice percentuale.
- Indicatore di riduzione stabilizzato: sale in fretta, scende lento.
- Nuovo test T31d (stereo normale, nessuna riduzione eccessiva): EngineTests da 40 a 41.

## 1.0.1 — correzione dei risultati con le impostazioni di default

Problemi trovati misurando il plugin sui tuoi stem reali (impostazioni di default) e corretti:

- **MONO SAFETY bloccava tutto sul materiale stereo largo.** Su PAD, PAD_2, PIANO, PERCUSSION LOOP e REESE la riduzione
  media era 70–98 %: WIDTH, MOTION e DEPTH non facevano quasi niente e lo stato mostrava sempre "movimento ridotto".
  Causa: la regola sulla correlazione poteva chiedere che la correlazione *salisse* per lasciar passare qualunque
  movimento, anche innocuo (con WIDTH 100 % e MOTION 0 la riduzione restava al 73–94 %). Ora fare niente è sempre
  ammesso, il materiale sano può essere allargato fino alla soglia, quello già problematico non viene peggiorato.
  Riduzione media ora: PAD 0 %, PAD_2 7 %, PERCUSSION 10 %, PIANO 26 %, REESE 0 %, mix 2 %.
- **Il riverbero di DEPTH sporcava i bassi:** ora riflessioni e coda partono da 200 Hz (filtro LR4 sul send).
- **WIDTH e MOTION toccavano anche 100–150 Hz:** ora agiscono da 300 Hz in su.
- **L'indice di integrità penalizzava la normale larghezza stereo:** ora la correlazione conta solo come indicatore di
  problema (da +0,3 in su il punteggio è pieno). Con i default l'integrità non scende più sul materiale sano.
- Display: la riduzione di MONO SAFETY è mostrata lisciata e solo sopra il 10 %.
- Nuovo test T31 (fallisce sulla 1.0.0, passa sulla 1.0.1).
- Installer .pkg: AU in /Library/Audio/Plug-Ins/Components, VST3 in /Library/Audio/Plug-Ins/VST3/TSR Audio,
  app in /Applications/TSR Audio. Rimuove le vecchie copie per-utente installate con INSTALLA.command.
