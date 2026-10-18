// BNB Chain: BNB и USDT-BEP20. Код — apps/evm/evm.c
#define EVM_NAME "BSC"
#define EVM_SHOW "BSC"
#define EVM_CHAIN_MAIN 56
#define EVM_CHAIN_TEST 97
#define EVM_COIN_MAIN "BNB"
#define EVM_COIN_TEST "tBNB"
#define EVM_NET_MAIN "BNB Chain"
#define EVM_NET_TEST "BSC testnet"
#define EVM_HIGH_FEE 5000000000000000ULL  // 0.005 BNB
// USDT: 0x55d398326f99059fF775485246999027B3197955 / testnet 0x337610d27c682E347C9cD60BD4b3b107C9d34dDd
#define EVM_TOKENS_MAIN                                                                            \
  {{{0x55, 0xd3, 0x98, 0x32, 0x6f, 0x99, 0x05, 0x9f, 0xf7, 0x75, 0x48, 0x52, 0x46, 0x99, 0x90, 0x27, 0xb3, \
     0x19, 0x79, 0x55},                                                                            \
    "USDT",                                                                                        \
    18}}
#define EVM_TOKENS_TEST                                                                            \
  {{{0x33, 0x76, 0x10, 0xd2, 0x7c, 0x68, 0x2e, 0x34, 0x7c, 0x9c, 0xd6, 0x0b, 0xd4, 0xb3, 0xb1, 0x07, 0xc9, \
     0xd3, 0x4d, 0xdd},                                                                            \
    "USDT",                                                                                        \
    18}}
#include "../evm/evm.c"
