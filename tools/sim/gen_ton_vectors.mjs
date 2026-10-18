// Эталоны TON для tools/sim/test_ton.py — от официальной библиотеки @ton/ton (не от нашего кода).
//
//   mkdir /tmp/tonref && cd /tmp/tonref && npm i @ton/ton @ton/core @ton/crypto
//   REF=/tmp/tonref node tools/sim/gen_ton_vectors.mjs > tools/sim/ton_vectors.json
//
// Фраза — «abandon ×11 about» (энтропия из нулей, как restore 00…00 в симуляторе).
// Пути — как в TON-приложении Ledger: m/44'/607'/<сеть>'/0'/<i>'/0' (сеть: 0 — mainnet, 1 — testnet).
// Кошелёк — W5 (v5R1). USDT — официальный жетон на mainnet.
import { createRequire } from 'module';
import { pbkdf2Sync } from 'crypto';

const require = createRequire((process.env.REF || '.') + '/');
const { WalletContractV5R1, internal, SendMode } = require('@ton/ton');
const { beginCell, Address, toNano, comment } = require('@ton/core');
const { deriveEd25519Path, keyPairFromSeed } = require('@ton/crypto');

const MNEMONIC = 'abandon '.repeat(11) + 'about';
const seed = pbkdf2Sync(MNEMONIC, 'mnemonic', 2048, 64, 'sha512');
const USDT = Address.parse('EQCxE6mUtQJKFnGfaROTKOt1lZbDiiX1kCixRv7Nw2Id_sDs');
// код кошелька USDT — library-ячейка со ссылкой на этот хеш (get_jetton_data мастера)
const USDT_WALLET_CODE_HASH = '8f452d7a4dfd74066b682365177259ed05734435be76b5fd4bd5d8af2b7c3d68';
const DEST = Address.parse('0:' + '6f'.repeat(32));

async function wallet(testnet, i) {
  const k = keyPairFromSeed(await deriveEd25519Path(seed, [44, 607, testnet ? 1 : 0, 0, i, 0]));
  const w = WalletContractV5R1.create({ workchain: 0, publicKey: k.publicKey,
    walletId: { networkGlobalId: testnet ? -3 : -239 } });
  return { k, w };
}

// Подписываемая ячейка W5 (без подписи): её хеш и подписывает устройство
async function signing(w, args) {
  let captured;
  await w.createTransfer({ ...args, signer: async (c) => { captured = c; return Buffer.alloc(64); } });
  return { boc: captured.toBoc().toString('hex'), hash: captured.hash().toString('hex') };
}

// Адрес USDT-кошелька владельца: StateInit(code = library-ячейка кода, data = 0, 0, owner, master)
function usdtWallet(owner) {
  const lib = beginCell().storeUint(2, 8).storeBuffer(Buffer.from(USDT_WALLET_CODE_HASH, 'hex')).endCell({ exotic: true });
  const data = beginCell().storeUint(0, 4).storeCoins(0).storeAddress(owner).storeAddress(USDT).endCell();
  const si = beginCell().storeUint(0, 2).storeMaybeRef(lib).storeMaybeRef(data).storeUint(0, 1).endCell();
  return new Address(0, si.hash());
}

function jettonBody(amount, to, response, fwdText) {
  const b = beginCell().storeUint(0x0f8a7ea5, 32).storeUint(0, 64).storeCoins(amount)
    .storeAddress(to).storeAddress(response).storeBit(0).storeCoins(fwdText ? 1n : 0n);
  if (fwdText) b.storeBit(1).storeRef(comment(fwdText)); else b.storeBit(0);
  return b.endCell();
}

const out = { mnemonic: MNEMONIC, dest: DEST.toRawString(), nets: {} };
for (const testnet of [true, false]) {
  const net = testnet ? 'testnet' : 'mainnet';
  const v = { addrs: [], pubkeys: [], cases: [] };
  for (let i = 0; i < 3; i++) {
    const { k, w } = await wallet(testnet, i);
    v.addrs.push(w.address.toString({ bounceable: false, testOnly: testnet }));
    v.pubkeys.push(k.publicKey.toString('hex'));
  }
  const { w } = await wallet(testnet, 0);
  v.raw0 = w.address.toRawString();
  const base = { seqno: 7, timeout: 1800000000, sendMode: SendMode.PAY_GAS_SEPARATELY | SendMode.IGNORE_ERRORS };
  const add = async (name, messages, extra = {}) =>
    v.cases.push({ name, ...extra, ...(await signing(w, { ...base, messages })) });
  await add('gram', [internal({ to: DEST, value: toNano('1.5'), bounce: false })],
    { amount: '1.5 GRAM', to: DEST.toString({ bounceable: false, testOnly: testnet }) });
  await add('gram_comment', [internal({ to: DEST, value: toNano('0.25'), bounce: true, body: 'invoice 42' })],
    { amount: '0.25 GRAM', to: DEST.toString({ bounceable: true, testOnly: testnet }), comment: 'invoice 42' });
  await add('gram_long_comment', [internal({ to: DEST, value: 1n, bounce: false, body: 'x'.repeat(96) })],
    { amount: '0.000000001 GRAM', comment: 'x'.repeat(96) });
  await add('two_messages', [internal({ to: DEST, value: 1n }), internal({ to: DEST, value: 2n })], { bad: true });
  await add('send_all', [internal({ to: DEST, value: 0n })], { bad: true });
  v.cases[v.cases.length - 1] = { ...v.cases[v.cases.length - 1],
    ...(await signing(w, { ...base, sendMode: SendMode.CARRY_ALL_REMAINING_BALANCE + SendMode.IGNORE_ERRORS,
      messages: [internal({ to: DEST, value: 0n })] })) };
  await add('foreign_body', [internal({ to: DEST, value: 1n, body: beginCell().storeUint(0xdeadbeef, 32).endCell() })], { bad: true });
  await add('state_init', [internal({ to: DEST, value: 1n, init: w.init })], { bad: true });
  if (!testnet) {
    const jw = usdtWallet(w.address);
    v.usdt_wallet0 = jw.toRawString();
    await add('usdt', [internal({ to: jw, value: toNano('0.05'), bounce: true, body: jettonBody(12_500_000n, DEST, w.address, null) })],
      { amount: '12.5 USDT', to: DEST.toString({ bounceable: false }) });
    await add('usdt_comment', [internal({ to: jw, value: toNano('0.05'), bounce: true,
      body: jettonBody(1_000_000n, DEST, w.address, 'for coffee') })], { amount: '1 USDT', comment: 'for coffee' });
    const other = usdtWallet(DEST);  // чужой кошелёк жетона: не наш USDT
    await add('usdt_wrong_wallet', [internal({ to: other, value: toNano('0.05'), body: jettonBody(1n, DEST, w.address, null) })], { bad: true });
    await add('usdt_foreign_response', [internal({ to: jw, value: toNano('0.05'), body: jettonBody(1n, DEST, DEST, null) })], { bad: true });
  }
  out.nets[net] = v;
}
console.log(JSON.stringify(out, null, 1));
