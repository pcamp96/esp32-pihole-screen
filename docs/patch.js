// Writes settings into the firmware's BAKED block, then fixes the app image's
// checksum and SHA-256 so the ESP32 bootloader accepts it.
// Field order and sizes must match struct BakedConfig in the sketch.

export const FIELDS = [
  ["magic", 16],
  ["id", 16],
  ["ssid", 64],
  ["wpass", 64],
  ["url", 128],
  ["pass", 128],
  ["pause", 16],
];
const MAGIC = new TextEncoder().encode("PHSCFG1-ZZZZZZZ\0");
const APP_OFFSET = 0x10000; // app partition offset inside the merged image

function findMagic(buf) {
  outer: for (let i = APP_OFFSET; i <= buf.length - MAGIC.length; i++) {
    for (let j = 0; j < MAGIC.length; j++) if (buf[i + j] !== MAGIC[j]) continue outer;
    return i;
  }
  return -1;
}

export function fieldLimit(name) {
  return FIELDS.find(([n]) => n === name)[1] - 1; // room for the terminating zero
}

export async function patchFirmware(original, values) {
  const buf = new Uint8Array(original); // copy
  const at = findMagic(buf);
  if (at < 0) throw new Error("Settings block not found in firmware");

  const enc = new TextEncoder();
  let pos = at;
  for (const [name, size] of FIELDS) {
    if (name !== "magic") {
      const bytes = enc.encode(values[name] ?? "");
      if (bytes.length > size - 1) throw new Error(`${name} is too long`);
      buf.fill(0, pos, pos + size);
      buf.set(bytes, pos);
    }
    pos += size;
  }

  // ESP app image: 24-byte header, then segments of (load addr, length, data).
  const view = new DataView(buf.buffer);
  if (buf[APP_OFFSET] !== 0xe9) throw new Error("No app image at 0x10000");
  const segments = buf[APP_OFFSET + 1];
  const hashAppended = buf[APP_OFFSET + 23] === 1;
  let p = APP_OFFSET + 24;
  let checksum = 0xef;
  for (let s = 0; s < segments; s++) {
    const len = view.getUint32(p + 4, true);
    for (let i = p + 8; i < p + 8 + len; i++) checksum ^= buf[i];
    p += 8 + len;
  }
  // The checksum byte sits where (offset in image) % 16 == 15
  while ((p - APP_OFFSET) % 16 !== 15) p++;
  buf[p] = checksum;
  if (hashAppended) {
    const digest = await crypto.subtle.digest("SHA-256", buf.slice(APP_OFFSET, p + 1));
    buf.set(new Uint8Array(digest), p + 1);
  }
  return buf;
}
