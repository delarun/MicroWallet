// Эталоны Solana для tools/sim/test_sol.py — от официальной @solana/web3.js (не от нашего кода).
//
//   mkdir /tmp/solref && cd /tmp/solref && npm i @solana/web3.js@1 @solana/spl-token@0.4 ed25519-hd-key
//   REF=/tmp/solref node tools/sim/gen_sol_vectors.mjs > tools/sim/sol_vectors.json
//
// Фраза — «abandon ×11 about»; ключи как в Phantom: m/44'/501'/<i>'/0'. Подпись ed25519
// детерминирована, поэтому эталон — сама подпись, байт в байт.
import { createRequire } from 'module';
import { pbkdf2Sync } from 'crypto';

const require = createRequire((process.env.REF || '.') + '/');
const web3 = require('@solana/web3.js');
const { derivePath } = require('ed25519-hd-key');
const { ed25519 } = require('@noble/curves/ed25519');
const spl = require('@solana/spl-token');
const { Keypair, PublicKey, SystemProgram, ComputeBudgetProgram, Transaction, TransactionMessage,
  TransactionInstruction, LAMPORTS_PER_SOL } = web3;

const seed = pbkdf2Sync('abandon '.repeat(11) + 'about', 'mnemonic', 2048, 64, 'sha512');
const kp = (i) => Keypair.fromSeed(derivePath(`m/44'/501'/${i}'/0'`, seed.toString('hex')).key);
const me = kp(0);
// получатель — ключ из фиксированного тестового seed: ни за кем не закреплён
const DEST = Keypair.fromSeed(Buffer.alloc(32, 5)).publicKey;
const OTHER = Keypair.fromSeed(Buffer.alloc(32, 7)).publicKey;
const BLOCKHASH = 'EkSnNWid2cvwEVnVx9aBqawnmiCNiDgp3gUdkDPTKN1N';

const legacy = (ixs, payer = me.publicKey) => {
  const tx = new Transaction({ recentBlockhash: BLOCKHASH, feePayer: payer });
  tx.add(...ixs);
  return tx.compileMessage().serialize();
};
const v0 = (ixs) => new TransactionMessage({ payerKey: me.publicKey, recentBlockhash: BLOCKHASH, instructions: ixs })
  .compileToV0Message().serialize();
const transfer = (lamports, to = DEST, from = me.publicKey) =>
  SystemProgram.transfer({ fromPubkey: from, toPubkey: to, lamports });
const limit = (u) => ComputeBudgetProgram.setComputeUnitLimit({ units: u });
const price = (p) => ComputeBudgetProgram.setComputeUnitPrice({ microLamports: p });
const hex = (b) => Buffer.from(b).toString('hex');

const out = { addrs: [0, 1, 2].map((i) => kp(i).publicKey.toBase58()), dest: DEST.toBase58(), cases: [] };
const add = (name, msg, extra = {}) => out.cases.push({
  name, msg: hex(msg), ...(extra.bad ? {} : { sig: hex(ed25519.sign(msg, me.secretKey.slice(0, 32))) }), ...extra });

add('sol', legacy([transfer(1.5 * LAMPORTS_PER_SOL)]), { amount: '1.5 SOL', fee: '0.000005 SOL' });
add('sol_priority', legacy([limit(1000), price(250000), transfer(12345)]),
  { amount: '0.000012345 SOL', fee: '0.00000525 SOL' });
add('sol_v0', v0([limit(1000), price(1), transfer(LAMPORTS_PER_SOL)]), { amount: '1 SOL', fee: '0.000005001 SOL' });
add('sol_high_fee', legacy([limit(1400000), price(10000000), transfer(1)]), { amount: '0.000000001 SOL', fee: '0.014005 SOL', high: true });
add('two_transfers', legacy([transfer(1), transfer(2)]), { bad: true });
add('foreign_payer', legacy([transfer(1, DEST, OTHER)], OTHER), { bad: true });
add('foreign_source', legacy([transfer(1, DEST, OTHER)]), { bad: true });
add('memo_program', legacy([transfer(1), new TransactionInstruction({
  programId: new PublicKey('MemoSq4gqABAXKb96qnxMmWBz2bdGJQr5HySVu8T5Yc'), keys: [], data: Buffer.from('hi') })]), { bad: true });
add('assign', legacy([SystemProgram.assign({ accountPubkey: me.publicKey, programId: OTHER })]), { bad: true });
add('zero', legacy([transfer(0)]), { bad: true });
add('heap_frame', legacy([ComputeBudgetProgram.requestHeapFrame({ bytes: 65536 }), transfer(1)]), { bad: true });
add('two_limits', legacy([limit(1000), limit(2000), transfer(1)]), { bad: true });
// ---- токены SPL: TransferChecked со своего токен-счёта; счёт получателя — ATA(владелец, mint).
// Устройству передаются владелец и bump: оно само выводит ATA и сверяет с транзакцией ----
const MINTS = {
  mainnet: { USDC: 'EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v', USDT: 'Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB' },
  testnet: { USDC: '4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU' },  // Circle, devnet
};
const ata = (owner, mint) => PublicKey.findProgramAddressSync(
  [owner.toBuffer(), spl.TOKEN_PROGRAM_ID.toBuffer(), mint.toBuffer()], spl.ASSOCIATED_TOKEN_PROGRAM_ID);
const tokenCase = (net, name, ixs, owner, bump, extra) => {
  const msg = legacy(ixs);
  out.cases.push({ name, net, msg: hex(msg), args: `${owner.toBase58()} ${bump}`,
    ...(extra.bad ? {} : { sig: hex(ed25519.sign(msg, me.secretKey.slice(0, 32))) }), ...extra });
};
for (const net of ['mainnet', 'testnet']) {
  for (const [sym, m] of Object.entries(MINTS[net])) {
    const mint = new PublicKey(m);
    const [src] = ata(me.publicKey, mint);
    const [dst, bump] = ata(DEST, mint);
    const xfer = (amount, to = dst, dec = 6, authority = me.publicKey) =>
      spl.createTransferCheckedInstruction(src, mint, to, authority, amount, dec);
    const create = (owner = DEST, a = dst) => spl.createAssociatedTokenAccountIdempotentInstruction(me.publicKey, a, owner, mint);
    tokenCase(net, `${sym.toLowerCase()}`, [limit(15000), price(100000), xfer(12_500_000n)], DEST, bump,
      { amount: `12.5 ${sym}`, fee: '0.0000065 SOL' });
    tokenCase(net, `${sym.toLowerCase()}_create`, [limit(50000), price(100000), create(), xfer(1_000_000n)], DEST, bump,
      { amount: `1 ${sym}`, fee: '0.00001 SOL', create: true });
    const [otherDst, otherBump] = ata(OTHER, mint);
    tokenCase(net, `${sym.toLowerCase()}_wrong_recipient`, [xfer(1n, otherDst)], DEST, bump, { bad: true });
    tokenCase(net, `${sym.toLowerCase()}_wrong_bump`, [xfer(1n)], DEST, (bump + 255) % 256, { bad: true });
    tokenCase(net, `${sym.toLowerCase()}_decimals`, [xfer(1n, dst, 9)], DEST, bump, { bad: true });
    tokenCase(net, `${sym.toLowerCase()}_create_other`, [create(OTHER, otherDst), xfer(1n)], DEST, bump, { bad: true });
    tokenCase(net, `${sym.toLowerCase()}_plus_sol`, [xfer(1n), transfer(1)], DEST, bump, { bad: true });
    tokenCase(net, `${sym.toLowerCase()}_unchecked`, [spl.createTransferInstruction(src, dst, me.publicKey, 1n)], DEST, bump, { bad: true });
    tokenCase(net, `${sym.toLowerCase()}_no_args`, [xfer(1n)], DEST, bump, { bad: true, args: '' });
  }
  const fake = Keypair.fromSeed(Buffer.alloc(32, 9)).publicKey;  // токен не из белого списка
  const [fsrc] = ata(me.publicKey, fake);
  const [fdst, fb] = ata(DEST, fake);
  tokenCase(net, 'unknown_mint', [spl.createTransferCheckedInstruction(fsrc, fake, fdst, me.publicKey, 1n, 6)], DEST, fb, { bad: true });
}
console.log(JSON.stringify(out, null, 1));
