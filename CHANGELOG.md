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
