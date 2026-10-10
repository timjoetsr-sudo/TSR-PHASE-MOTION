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
