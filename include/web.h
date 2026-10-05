// Back office web : une page sur le port 80 de la carte, pour activer ou non les pages et la
// météo, régler la luminosité, choisir le lieu des prévisions, voir l'état de la carte et lui
// envoyer un nouveau firmware ou la redémarrer. Sans mot de passe : à garder sur le réseau local.
// Nécessite netBegin() et pluginsBegin().
//
// Mise à jour par Wi-Fi sans passer par la page :
//   curl -H Expect: --data-binary @.pio/build/waveshare-5b/firmware.bin http://<adresse>/update
#pragma once

void webBegin();

// Vrai pendant qu'un firmware est reçu : l'écran doit rester noir, la carte redémarre ensuite
bool webUpdating();
