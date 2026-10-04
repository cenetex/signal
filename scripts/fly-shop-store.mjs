import { mkdir, readFile, open, rename } from 'node:fs/promises';
import path from 'node:path';
import { randomBytes } from 'node:crypto';
export class FlyShopStore {
  constructor(file) { this.file = file; this.rows = []; this.tail = Promise.resolve(); }
  async init() {
    await mkdir(path.dirname(this.file), { recursive: true, mode: 0o700 });
    try {
      const data = JSON.parse(await readFile(this.file, 'utf8'));
      if (data.version !== 1 || !Array.isArray(data.quotes) || data.quotes.length > 5000)
        throw new Error('invalid_shop_store');
      const ids = new Set();
      for (const q of data.quotes) {
        if (!/^[a-f0-9]{64}$/.test(q.id) || ids.has(q.id) || typeof q.wallet !== 'string' ||
            !Number.isInteger(q.station) || q.station < 0 || q.station > 2 ||
            !Number.isSafeInteger(q.createdAt) || !['quoted', 'paid', 'fulfilled'].includes(q.state))
          throw new Error('invalid_shop_store');
        ids.add(q.id);
      }
      this.rows = data.quotes;
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
    return this;
  }
  exclusive(fn) {
    const next = this.tail.then(fn);
    this.tail = next.catch(() => {});
    return next;
  }
  async save() {
    const dir = path.dirname(this.file);
    const tmp = `${this.file}.${randomBytes(8).toString('hex')}.tmp`;
    const handle = await open(tmp, 'wx', 0o600);
    try { await handle.writeFile(JSON.stringify({ version: 1, quotes: this.rows })); await handle.sync(); }
    finally { await handle.close(); }
    await rename(tmp, this.file);
    const directory = await open(dir, 'r');
    try { await directory.sync(); } finally { await directory.close(); }
  }
}
