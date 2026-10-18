// coins.h — версия прошивки и сеть сборки. Сами монеты — приложения (apps/).
//
// Сеть выбирается при сборке: по умолчанию testnet, mainnet — CONFIG_MW_MAINNET.
// Приложения узнают её через mw_net() и сами выбирают пути, префиксы и токены.
#pragma once

#define FW_VERSION "0.8.0"

#ifdef MW_MAINNET
#define NET_NAME "mainnet"
#else
#define NET_NAME "testnet"
#endif
