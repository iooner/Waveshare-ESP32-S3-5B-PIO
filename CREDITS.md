# Crédits

Tout ce que ce projet utilise ou reprend d'ailleurs. À tenir à jour à chaque ajout de
bibliothèque, de police, d'image ou de code repris.

## Dans le firmware

| Quoi | Auteur | Licence | Utilisation |
| --- | --- | --- | --- |
| [Arduino core pour ESP32](https://github.com/espressif/arduino-esp32) 3.3.12 | Espressif Systems | LGPL-2.1 | Framework : `Serial`, `Wire`, `WiFi`, `configTzTime()`, `Preferences` (réglages en flash), `Update` (mise à jour par Wi-Fi), `NetworkClientSecure` (HTTPS) |
| [ESP-IDF](https://github.com/espressif/esp-idf) 5.5.5 | Espressif Systems | Apache-2.0 | Pilote LCD RGB `esp_lcd`, `esp_timer`, SNTP, et les composants embarqués (FreeRTOS, lwIP...) |
| [Open Sans](https://github.com/googlefonts/opensans) 3.003 | The Open Sans Project Authors, © 2020 | SIL Open Font License 1.1 ([texte](include/fonts/OFL.txt)) | Glyphes pré-rendus dans `include/fonts/*.h` |
| [TJpgDec](http://elm-chan.org/fsw/tjpgd/) | ChaN | Licence libre de l'auteur (type BSD) | Décodeur JPEG présent dans la ROM de l'ESP32-S3, utilisé pour les pochettes (`src/sonos.cpp`) |

Aucune bibliothèque externe n'est déclarée dans `platformio.ini` : le pilote d'écran, les
primitives de dessin et le rendu de texte sont écrits pour ce projet (`src/lcd.cpp`, `src/gfx.cpp`).
Les pictogrammes météo (`include/fonts/font_weather64.h`) sont dessinés par `tools/mkicons.py`,
sans reprendre de police d'icônes.

## Outils de build

| Quoi | Auteur | Licence | Utilisation |
| --- | --- | --- | --- |
| [PlatformIO](https://platformio.org) | PlatformIO Labs | Apache-2.0 | Compilation et téléversement |
| [pioarduino platform-espressif32](https://github.com/pioarduino/platform-espressif32) 55.03.312 | Communauté pioarduino | Apache-2.0 | Plateforme PlatformIO fournissant l'Arduino core 3.x |
| [Pillow](https://python-pillow.github.io) | Jeffrey A. Clark et contributeurs | MIT-CMU | Rendu des polices par `tools/mkfont.py` et des pictogrammes météo par `tools/mkicons.py` (pas embarqué dans le firmware) |

## Services utilisés à l'exécution

| Quoi | Utilisation |
| --- | --- |
| [NTP Pool Project](https://www.ntppool.org) (`pool.ntp.org`) | Heure réseau du plugin horloge |
| Enceintes Sonos du réseau local (UPnP, port 1400) | Morceau en cours, lu directement sur les enceintes |
| Serveur d'images de Spotify (`i.scdn.co`) | Pochette du morceau en cours |
| [Open-Meteo](https://open-meteo.com) (`api.open-meteo.com`), données sous licence CC BY 4.0, usage non commercial sans clé | Temps actuel et prévisions heure par heure de la page horloge (`src/weather.cpp`) |
| [CoinGecko](https://www.coingecko.com) (`api.coingecko.com`), accès public sans clé | Cours des cryptomonnaies de la page crypto (`src/crypto.cpp`) ; la recherche d'une crypto est appelée par le navigateur depuis le back office |
| [International Space Station APIs](https://github.com/corquaid/international-space-station-APIs) (`corquaid.github.io`), liste tenue à jour par son auteur | Nombre de personnes dans l'espace, sur la page d'accueil (`src/space.cpp`) |
| Recherche de ville d'Open-Meteo (`geocoding-api.open-meteo.com`) | Appelée par le navigateur depuis le back office (`src/web.cpp`), pas par la carte |

## Sources et références

Rien n'est copié tel quel de ces projets, mais le code s'appuie dessus.

- **Wiki Waveshare de la carte ESP32-S3-Touch-LCD-5 / 5B** : brochage, schéma, CH422G.
- **[ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel)** (Espressif,
  Apache-2.0) : broches et timings de départ de la dalle, repris dans `include/board_pins.h`
  (profil `BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_5_B`).
- **[Arduino_GFX](https://github.com/moononournation/Arduino_GFX)** (Moon On Our Nation) :
  utilisée par la première version du projet. Les paramètres `esp_lcd` de `src/lcd.cpp`
  (polarités, ordre des broches de données) reprennent ceux de sa classe `Arduino_ESP32RGBPanel`.
- **Source d'ESP-IDF, `esp_lcd_panel_rgb.c`** : l'échange de framebuffers sans déchirement de
  `lcdPresent()` repose sur le comportement de `lcd_rgb_panel_fill_bounce_buffer()`, et le choix
  du tampon dans `src/lcd.cpp` contourne un défaut de `lcd_rgb_panel_eof_handler()` (image
  décalée après une interruption perdue).
- **[SoCo](https://github.com/SoCo/SoCo)** et **[sonos.svrooij.io](https://sonos.svrooij.io/)** : documentation
  communautaire du protocole UPnP des enceintes Sonos, consultée pour `src/sonos.cpp`.
- **Algorithme de Bresenham** : tracé de lignes de `gfxLine()`.
- **[Astronomy Answers, « Position of the Sun »](https://www.aa.quae.nl/en/reken/zonpositie.html)** (Louis
  Strous) : formules et constantes de la hauteur du soleil et de ses heures de lever et de coucher
  dans `src/astro.cpp` (cycle automatique de luminosité, ligne soleil et lune de la page horloge).
- **Jean Meeus, *Astronomical Algorithms*** : arguments moyens de la lune et ses principales
  inégalités, repris à l'ordre le plus bas pour la phase de la lune de `src/astro.cpp`.
- **Mélange RGB565 en une multiplication** (masque `0x07E0F81F`) : astuce classique, sans auteur
  identifié, utilisée par `blend565()` pour l'antialiasing du texte.

## Développement

Code écrit avec l'assistance de [Claude Code](https://claude.com/claude-code) (Anthropic).
