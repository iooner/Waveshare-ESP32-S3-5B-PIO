// Cours des cryptomonnaies choisies dans le back office (web.h), en euros, lus chez CoinGecko :
// ni compte ni clé. Les quantités détenues ne quittent pas la carte : elle ne demande que les
// cours. Nécessite netBegin().
#pragma once
#include <Arduino.h>

#define CRYPTO_MAX  6

struct CryptoCoin {
  char id[48];      // identifiant CoinGecko, par exemple "bitcoin"
  char symbol[8];   // par exemple "BTC"
  double quantity;  // quantité détenue ; 0 : seul le cours est affiché
};

struct Crypto {
  uint8_t count;
  CryptoCoin coins[CRYPTO_MAX];
  double price[CRYPTO_MAX];  // en euros ; 0 : pas de cours pour cette crypto
  float change[CRYPTO_MAX][3];  // variation sur 1 heure, 24 heures et 7 jours, en pour cent ; NAN : inconnue
};

// Relit les cryptos gardées en flash et lance la lecture des cours en arrière-plan
void cryptoBegin();

// Version des cours, qui change à chaque lecture et à chaque changement de réglages. 0 s'il n'y a
// rien à montrer : aucune crypto choisie, cours pas encore reçus ou trop anciens.
uint32_t cryptoVersion();

// Cryptos choisies et leurs derniers cours
void cryptoGet(Crypto &out);

// Secondes écoulées depuis la dernière lecture réussie, ou -1 s'il n'y en a pas eu
int32_t cryptoAge();

// Remplace les cryptos choisies, les garde en flash et relit les cours sans attendre
void cryptoConfigure(const CryptoCoin *coins, uint8_t count);
