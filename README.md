# Écran d'accueil pour Waveshare ESP32-S3-LCD-5B

Une horloge de salon sur un écran de 5 pouces (1024 × 600) : l'heure, la météo des heures à venir, le soleil et la lune, le morceau en cours sur les enceintes Sonos et le cours de quelques cryptomonnaies. Tout se règle depuis une page web servie par la carte, y compris la mise à jour du firmware.

Le firmware est écrit pour PlatformIO, sans bibliothèque externe : le pilote d'écran, le dessin et le rendu du texte sont faits maison.

![La page horloge, de jour](docs/ecran-jour.png)

*Les images de l'écran sont des rendus calculés avec les polices et les couleurs de la carte, pas des photos.*

## Ce que l'écran affiche

- **Heure et date**, dans le fuseau horaire de la ville choisie.
- **Soleil et lune** : heures de lever et de coucher, phase de la lune. Calculés sur la carte, sans réseau.
- **Météo** : le temps actuel puis les six heures suivantes, avec le risque de pluie. Une ligne annonce la pluie à venir (« Pluie dans 25 min ») ou sa fin. Données [Open-Meteo](https://open-meteo.com), sans compte ni clé.

La météo, le soleil et la lune s'activent ou se désactivent séparément, et la page se recentre toute seule pour rester équilibrée.

## Sonos

Quand une enceinte du réseau joue, une page prend l'écran : pochette, pièce, titre, artiste, album et progression. La carte interroge directement les enceintes, sans compte ni cloud.

![La page Sonos](docs/ecran-sonos.png)

*La pochette de cet exemple est un dessin fait pour l'image.*

## Cryptomonnaies

Une page donne le cours en euros de six cryptomonnaies au plus, avec leurs variations sur 1 heure, 24 heures et 7 jours. Elles se choisissent dans le back office, par une recherche. Si une quantité est saisie, la page affiche aussi ce qu'elle vaut, et le total du portefeuille.

Les cours viennent de [CoinGecko](https://www.coingecko.com), sans compte ni clé. La carte ne demande que les cours : les quantités ne la quittent pas. Elles sont en revanche visibles dans le back office, qui n'a pas de mot de passe.

![La page crypto](docs/ecran-crypto.png)

*Cours et quantités d'exemple.*

## Diaporama

Les pages activées se succèdent, chacune pendant une durée réglable : l'accueil, la page crypto, et la page Sonos quand une enceinte joue. Sonos peut aussi être prioritaire : elle garde alors l'écran pour elle tant que la musique joue.

En option, un fondu au noir adoucit le passage d'une page à l'autre. Il est appliqué pendant le balayage de l'écran, sans rien redessiner.

## La nuit

- **Cycle automatique** : la luminosité suit le soleil de la ville choisie, en fondu pendant l'aube et le crépuscule.
- **Rouge sombre** : en option, toutes les couleurs glissent vers le rouge quand le soleil se couche.
- **Extinction programmée** : l'écran s'éteint entre deux heures. En option, la carte passe aussi en veille profonde et se réveille seule à l'heure du rallumage.

![La page horloge, de nuit avec l'option rouge](docs/ecran-nuit.png)

## Le back office

La carte sert une page de réglages sur son adresse (`http://<adresse>/`, affichée sur le port série au démarrage). Chaque réglage est enregistré dès qu'il change et gardé en flash : pages du diaporama et leurs durées, cryptomonnaies, contenu de l'accueil, luminosité, lieu.

On y trouve aussi une recherche de ville, l'état de la carte (page affichée, mémoire libre, images ratées, cause du dernier démarrage) et l'envoi d'un nouveau firmware par Wi-Fi.

La page n'a pas de mot de passe : elle est faite pour rester sur le réseau local.

<img src="docs/back-office.png" alt="Le back office, sur un téléphone" width="320">

## Matériel

- [Waveshare ESP32-S3-LCD-5B](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-5), version sans tactile : ESP32-S3-WROOM-1-N16R8 (16 Mo de flash, 8 Mo de PSRAM), dalle RGB 1024 × 600.
- Une alimentation USB-C.

## Installation

1. Installer [PlatformIO](https://platformio.org).
2. Copier `include/secrets.example.h` en `include/secrets.h` et y mettre le nom et le mot de passe du Wi-Fi. Le lieu de la météo se règle ensuite dans le back office.
3. Brancher la carte en USB, puis compiler et flasher :

   ```sh
   pio run -t upload
   ```

4. Les mises à jour suivantes peuvent passer par le Wi-Fi, depuis le back office ou en ligne de commande :

   ```sh
   pio run
   curl -H Expect: --data-binary @.pio/build/waveshare-5b/firmware.bin http://<adresse>/update
   ```

## Organisation du code

| Fichier | Rôle |
| --- | --- |
| `src/lcd.cpp` | Pilote de la dalle RGB : deux images en PSRAM, échange sans déchirement, lignes gardées en RAM interne |
| `src/gfx.cpp` | Dessin et texte antialiasé, teinte de nuit |
| `src/plugin.cpp`, `src/plugins/` | Pages et leur diaporama : accueil (horloge et météo), Sonos, crypto, mire de test |
| `src/weather.cpp` | Lecture d'Open-Meteo, fuseau horaire du lieu |
| `src/astro.cpp` | Soleil et lune |
| `src/sonos.cpp` | Lecture des enceintes Sonos du réseau local |
| `src/crypto.cpp` | Cours des cryptomonnaies, en HTTPS |
| `src/brightness.cpp` | Luminosité, cycle, extinction et veille profonde |
| `src/web.cpp` | Back office et mise à jour par Wi-Fi |
| `tools/` | Génération des polices et des pictogrammes |

Une contrainte guide tout le reste : pendant que le Wi-Fi travaille, l'écran ne peut lire sans saut d'image que la RAM interne de la puce, qui est petite. Chaque ligne affichée y est donc gardée sous forme compacte, limitée à 16 couleurs. Une page nouvelle doit s'y tenir : un fond et un texte d'une seule couleur par ligne, ou jusqu'à trois couleurs avec un lissage réduit, comme sur la page crypto.

Pour la même raison, la mémoire des connexions chiffrées et les piles des tâches réseau sont placées en PSRAM.

## Crédits

Polices, services et sources utilisés : voir [CREDITS.md](CREDITS.md).
