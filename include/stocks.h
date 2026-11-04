// Cours des actions et ETF choisis dans le back office (web.h), désignés par leur ISIN. Ils sont lus
// chez Lang & Schwarz (ls-tc.de), la bourse où passent les ordres de Trade Republic : mêmes prix,
// en euros, ni compte ni clé. Les quantités et prix d'achat ne quittent pas la carte : elle ne
// demande que les cours. Nécessite netBegin().
#pragma once
#include <Arduino.h>

#define STOCKS_MAX  250  // de quoi ne jamais l'atteindre ; le compteur tient sur un octet

struct Stock {
  char isin[13];    // par exemple "US1912161007"
  char name[16];    // nom affiché ; vide : celui que donne la bourse
  double quantity;  // quantité détenue ; 0 : seul le cours est affiché
  double cost;      // prix d'achat moyen d'une part, en euros ; 0 : inconnu
};

struct Stocks {
  uint8_t count;
  Stock stocks[STOCKS_MAX];
  char title[STOCKS_MAX][24];  // nom donné par la bourse, au format des polices ; vide : ISIN inconnu
  double price[STOCKS_MAX];     // dernier cours, en euros ; 0 : pas de cours
  double previous[STOCKS_MAX];  // clôture de la veille ; 0 : inconnue
};

// Relit les titres gardés en flash et lance la lecture des cours en arrière-plan
void stocksBegin();

// Version des cours, qui change à chaque lecture et à chaque changement de réglages. 0 s'il n'y a
// rien à montrer : aucun titre choisi, cours pas encore reçus ou trop anciens.
uint32_t stocksVersion();

// Titres choisis et leurs derniers cours. La structure fait 22 Ko : à garder en PSRAM.
void stocksGet(Stocks &out);

// Secondes écoulées depuis la dernière lecture réussie, ou -1 s'il n'y en a pas eu
int32_t stocksAge();

// Durée du dernier tour de lecture des cours, en secondes
uint16_t stocksRoundSeconds();

// 1 si la bourse principale du titre est en séance, 0 si elle est fermée, -1 si on ne sait pas :
// pays de l'ISIN sans bourse connue, ou heure pas encore reçue. D'après les heures de séance de
// chaque bourse et le jour de la semaine : un jour férié passe pour ouvert.
int8_t stocksMarketOpen(const char *isin);

// Nombre de titres choisis dont la bourse ne connaît pas l'ISIN
uint8_t stocksUnknown();

// A appeler depuis la boucle principale : enregistre en flash les titres qui viennent de changer.
// Une écriture en flash fait rater des images : elle attend que l'écran soit noir (`dark`, entre
// deux pages), ou au plus 10 minutes.
void stocksLoop(bool dark);

// Remplace les titres choisis et relit les cours sans attendre. Ils sont gardés en flash par stocksLoop().
void stocksConfigure(const Stock *stocks, uint8_t count);
