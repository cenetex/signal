const alphabet = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';
export function decode58(text, length) {
  if (typeof text !== 'string' || text.length < 1 || text.length > 88) throw new Error('invalid_address');
  let n = 0n;
  for (const ch of text) {
    const digit = alphabet.indexOf(ch);
    if (digit < 0) throw new Error('invalid_address');
    n = n * 58n + BigInt(digit);
  }
  const bytes = [];
  while (n) { bytes.unshift(Number(n & 255n)); n >>= 8n; }
  for (const ch of text) { if (ch !== '1') break; bytes.unshift(0); }
  if (length !== undefined && bytes.length !== length) throw new Error('invalid_address');
  return Uint8Array.from(bytes);
}
export function encode58(bytes) {
  let n = 0n, out = '', zeros = '';
  for (const byte of bytes) n = (n << 8n) + BigInt(byte);
  while (n) { out = alphabet[Number(n % 58n)] + out; n /= 58n; }
  for (const byte of bytes) { if (byte !== 0) break; zeros += '1'; }
  return zeros + out;
}
