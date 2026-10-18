// Ethereum: ETH, USDT и USDC (Sepolia — USDC от Circle). Код — apps/evm/evm.c
#define EVM_NAME "Ethereum"
#define EVM_SHOW "Ethereum"
#define EVM_CHAIN_MAIN 1
#define EVM_CHAIN_TEST 11155111
#define EVM_COIN_MAIN "ETH"
#define EVM_COIN_TEST "SepoliaETH"
#define EVM_NET_MAIN "Ethereum"
#define EVM_NET_TEST "Sepolia testnet"
#define EVM_HIGH_FEE 5000000000000000ULL  // 0.005 ETH
// USDT 0xdAC17F958D2ee523a2206206994597C13D831ec7, USDC 0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48;
// Sepolia USDC 0x1c7D4B196Cb0C7B01d743Fbc6116a902379C7238
#define EVM_TOKENS_MAIN                                                                            \
  {{{0xda, 0xc1, 0x7f, 0x95, 0x8d, 0x2e, 0xe5, 0x23, 0xa2, 0x20, 0x62, 0x06, 0x99, 0x45, 0x97, 0xc1, 0x3d, \
     0x83, 0x1e, 0xc7},                                                                            \
    "USDT",                                                                                        \
    6},                                                                                            \
   {{0xa0, 0xb8, 0x69, 0x91, 0xc6, 0x21, 0x8b, 0x36, 0xc1, 0xd1, 0x9d, 0x4a, 0x2e, 0x9e, 0xb0, 0xce, 0x36, \
     0x06, 0xeb, 0x48},                                                                            \
    "USDC",                                                                                        \
    6}}
#define EVM_TOKENS_TEST                                                                            \
  {{{0x1c, 0x7d, 0x4b, 0x19, 0x6c, 0xb0, 0xc7, 0xb0, 0x1d, 0x74, 0x3f, 0xbc, 0x61, 0x16, 0xa9, 0x02, 0x37, \
     0x9c, 0x72, 0x38},                                                                            \
    "USDC",                                                                                        \
    6}}
#include "../evm/evm.c"
