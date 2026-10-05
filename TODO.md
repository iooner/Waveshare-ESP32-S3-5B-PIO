# À faire

## Page Sonos

- [ ] **Afficher la pochette d'un coup, sans balayage vertical.** Aujourd'hui elle est recopiée dans l'image de l'écran par bandes de 20 lignes, en 0,4 seconde, pour ne pas écrire 180 Ko en PSRAM pendant que l'écran la relit (`drawArtSlice()` dans `src/plugins/sonos.cpp`). Piste : que le balayage lise la pochette directement dans sa mémoire de décodage, sans copie (`onBounceEmpty()` dans `src/lcd.cpp`). À prévoir : la teinte de nuit, appliquée aujourd'hui pendant la copie, et une vérification par les compteurs d'images ratées.
- [ ] **Raccourcir le délai avant la pochette au changement de morceau.** Deux leviers dans `src/sonos.cpp` et `src/net.cpp` : interroger l'enceinte plus souvent que toutes les 2 secondes (`POLL_MS`), et couper l'économie d'énergie du Wi-Fi, qui ralentit chaque échange. Le second rend tout le réseau plus réactif, back office compris, mais augmente la consommation. Mesurer d'abord le temps réel de chaque étape (interrogation, téléchargement, décodage).
