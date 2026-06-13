#!/usr/bin/env node
/**
 * Generate frame-based logo animations for Codex (OpenAI) and Cursor.
 * Rasterizes SVG marks, applies idle motion, quantizes to 20×20 palette grids,
 * writes preview PNGs for visual verification, and JSON for convert_logo_to_c.js.
 *
 * Usage: node generate_logo_anim.js [--out-dir DIR] [--preview-dir DIR]
 */

const fs = require('fs');
const path = require('path');
const https = require('https');
const { Resvg } = require('@resvg/resvg-js');
const { PNG } = require('pngjs');

const GRID = 20;
const FRAME_COUNT = 12;
const HOLD_MS = 120;
const RENDER_SIZE = 256;

const args = process.argv.slice(2);
const opt = (k, def) => {
  const i = args.indexOf(k);
  return i >= 0 ? args[i + 1] : def;
};

const OUT_DIR = path.resolve(opt('--out-dir', path.join(__dirname, 'logo_anim_data')));
const PREVIEW_DIR = path.resolve(opt('--preview-dir', '/tmp/clawdmeter_logo_preview'));
const ONLY_SERVICE = opt('--service', null);

const PALETTE = ['transparent', '#ffffff', '#888888'];

const SOURCES = {
  codex: {
    name: 'codex idle',
    category: 'Logo',
    urls: [
      'https://cdn.jsdelivr.net/npm/simple-icons@v11/icons/openai.svg',
      'https://upload.wikimedia.org/wikipedia/commons/4/4d/OpenAI_Logo.svg',
    ],
    motion: 'bob_pulse',
    quantize: 'mono',
  },
  cursor: {
    filename: 'cursor_splash.json',
    name: 'cursor splash',
    category: 'Logo',
    localPath: path.join(__dirname, '..', 'assets', 'logo_sources', 'cursor.svg'),
    urls: [
      'https://cursor.com/favicon.svg',
    ],
    motion: 'consume_grow',
    quantize: 'bright_only',
  },
};

function fetchUrl(url) {
  return new Promise((resolve, reject) => {
    https
      .get(url, { headers: { 'User-Agent': 'ClawdmeterLogoGen/1.0' } }, (res) => {
        if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
          fetchUrl(res.headers.location).then(resolve).catch(reject);
          return;
        }
        if (res.statusCode !== 200) {
          reject(new Error(`HTTP ${res.statusCode} for ${url}`));
          res.resume();
          return;
        }
        const chunks = [];
        res.on('data', (c) => chunks.push(c));
        res.on('end', () => resolve(Buffer.concat(chunks)));
      })
      .on('error', reject);
  });
}

async function loadSvg(serviceKey) {
  const spec = SOURCES[serviceKey];
  if (spec.localPath && fs.existsSync(spec.localPath)) {
    const buf = fs.readFileSync(spec.localPath);
    console.log(`  ${serviceKey}: loaded ${spec.localPath} (${buf.length} bytes)`);
    return buf;
  }
  let lastErr = null;
  for (const url of spec.urls) {
    try {
      const buf = await fetchUrl(url);
      const text = buf.toString('utf8');
      if (!text.includes('<svg') && !text.includes('<?xml')) {
        throw new Error('not svg');
      }
      console.log(`  ${serviceKey}: loaded ${url} (${buf.length} bytes)`);
      return buf;
    } catch (e) {
      lastErr = e;
      console.warn(`  ${serviceKey}: failed ${url}: ${e.message}`);
    }
  }
  throw lastErr || new Error(`no source for ${serviceKey}`);
}

function renderSvg(svgBuf, size) {
  const resvg = new Resvg(svgBuf, {
    fitTo: { mode: 'width', value: size },
    background: 'rgba(0,0,0,0)',
  });
  const rendered = resvg.render();
  const png = PNG.sync.read(rendered.asPng());
  return png;
}

function nearestPaletteIndex(r, g, b, a, mode) {
  if (a < 32) return 0;
  const lum = 0.299 * r + 0.587 * g + 0.114 * b;
  if (mode === 'bright_only') {
    return lum > 150 ? 1 : 0;
  }
  // Monochrome marks (Simple Icons): any opaque ink becomes white.
  return 1;
}

function sampleTransformed(png, frameIdx, total, motion, quantize) {
  const t = frameIdx / total;
  const phase = t * Math.PI * 2;
  let bobY = Math.sin(phase) * 2.5;
  let scaleX = 1;
  let scaleY = 1 + Math.sin(phase) * 0.06;
  let rotDeg = 0;

  if (motion === 'bob_tilt') {
    rotDeg = Math.sin(phase) * 8;
    scaleY = 1 + Math.sin(phase + Math.PI / 4) * 0.05;
  } else {
    scaleX = 1 + Math.sin(phase + Math.PI / 2) * 0.04;
  }

  const grid = [];
  for (let gy = 0; gy < GRID; gy++) {
    const row = [];
    for (let gx = 0; gx < GRID; gx++) {
      const cx = GRID / 2;
      const cy = GRID / 2 + bobY / (RENDER_SIZE / GRID);
      let lx = gx - cx;
      let ly = gy - cy - bobY / (RENDER_SIZE / GRID);

      const rad = (rotDeg * Math.PI) / 180;
      const cos = Math.cos(rad);
      const sin = Math.sin(rad);
      const rx = lx * cos - ly * sin;
      const ry = lx * sin + ly * cos;
      lx = rx / scaleX;
      ly = ry / scaleY;

      const u = (lx / GRID + 0.5) * png.width;
      const v = (ly / GRID + 0.5) * png.height;
      const x = Math.floor(u);
      const y = Math.floor(v);
      if (x < 0 || y < 0 || x >= png.width || y >= png.height) {
        row.push(0);
        continue;
      }
      const idx = (y * png.width + x) << 2;
      row.push(nearestPaletteIndex(png.data[idx], png.data[idx + 1], png.data[idx + 2], png.data[idx + 3], quantize));
    }
    grid.push(row);
  }
  return grid;
}

function emptyGrid() {
  return Array.from({ length: GRID }, () => Array(GRID).fill(0));
}

function stampMask(target, source, cx, cy, scale, color) {
  for (let y = 0; y < GRID; y++) {
    for (let x = 0; x < GRID; x++) {
      const sx = Math.round((x - cx) / scale + GRID / 2);
      const sy = Math.round((y - cy) / scale + GRID / 2);
      if (sx < 0 || sy < 0 || sx >= GRID || sy >= GRID) continue;
      if (source[sy][sx] !== 0) target[y][x] = color;
    }
  }
}

function cursorParticle(grid, x, y, color) {
  const points = [
    [1, 0], [2, 0],
    [0, 1], [3, 1],
    [0, 2], [2, 2],
    [1, 3], [2, 3],
  ];
  for (const [dx, dy] of points) {
    const px = x + dx;
    const py = y + dy;
    if (px >= 0 && py >= 0 && px < GRID && py < GRID) grid[py][px] = color;
  }
}

function consumingCursorFrame(base, frameIdx) {
  const grid = emptyGrid();
  const progress = Math.min(frameIdx / 8, 1);
  const eaterX = 4.0 + progress * 6.0;
  const eaterScale = 0.42 + progress * 0.58;

  const snacks = [
    { eatenAt: 0.24, x: 7, y: 3 },
    { eatenAt: 0.50, x: 12, y: 13 },
    { eatenAt: 0.82, x: 16, y: 5 },
  ];
  for (const snack of snacks) {
    if (progress < snack.eatenAt) cursorParticle(grid, snack.x, snack.y, 2);
  }

  // The Cursor mark itself is the eater: it advances through the smaller
  // marks and grows after every bite until it reaches the original max size.
  stampMask(grid, base, eaterX, 10, eaterScale, 1);
  return grid;
}

function gridToPreviewPng(grid, cellSize = 16) {
  const w = GRID * cellSize;
  const h = GRID * cellSize;
  const out = new PNG({ width: w, height: h });
  const colors = [
    [0, 0, 0, 255],
    [255, 255, 255, 255],
    [136, 136, 136, 255],
  ];
  for (let gy = 0; gy < GRID; gy++) {
    for (let gx = 0; gx < GRID; gx++) {
      const c = colors[grid[gy][gx]] || colors[0];
      for (let dy = 0; dy < cellSize; dy++) {
        for (let dx = 0; dx < cellSize; dx++) {
          const px = ((gy * cellSize + dy) * w + (gx * cellSize + dx)) << 2;
          out.data[px] = c[0];
          out.data[px + 1] = c[1];
          out.data[px + 2] = c[2];
          out.data[px + 3] = c[3];
        }
      }
    }
  }
  return out;
}

function compositePreview(frames, cellSize = 16) {
  const fw = GRID * cellSize;
  const fh = GRID * cellSize;
  const cols = Math.min(frames.length, 4);
  const rows = Math.ceil(frames.length / cols);
  const out = new PNG({ width: fw * cols, height: fh * rows });
  for (let i = 0; i < frames.length; i++) {
    const png = gridToPreviewPng(frames[i].grid, cellSize);
    const col = i % cols;
    const row = Math.floor(i / cols);
    for (let y = 0; y < fh; y++) {
      for (let x = 0; x < fw; x++) {
        const si = (y * fw + x) << 2;
        const di = ((row * fh + y) * fw * cols + col * fw + x) << 2;
        out.data[di] = png.data[si];
        out.data[di + 1] = png.data[si + 1];
        out.data[di + 2] = png.data[si + 2];
        out.data[di + 3] = png.data[si + 3];
      }
    }
  }
  return out;
}

async function generateService(serviceKey) {
  const spec = SOURCES[serviceKey];
  const svg = await loadSvg(serviceKey);
  const base = renderSvg(svg, RENDER_SIZE);
  const baseGrid = sampleTransformed(base, 0, FRAME_COUNT, 'still', spec.quantize || 'mono');

  const frames = [];
  for (let i = 0; i < FRAME_COUNT; i++) {
    const grid = spec.motion === 'consume_grow'
      ? consumingCursorFrame(baseGrid, i)
      : sampleTransformed(base, i, FRAME_COUNT, spec.motion, spec.quantize || 'mono');
    frames.push({ hold: HOLD_MS, grid });
    const preview = gridToPreviewPng(grid, 20);
    const framePath = path.join(PREVIEW_DIR, `${serviceKey}_frame_${String(i).padStart(2, '0')}.png`);
    fs.writeFileSync(framePath, PNG.sync.write(preview));
  }

  const composite = compositePreview(frames, 20);
  fs.writeFileSync(path.join(PREVIEW_DIR, `${serviceKey}_composite.png`), PNG.sync.write(composite));

  const data = {
    filename: spec.filename || `${serviceKey}_idle.json`,
    name: spec.name,
    category: spec.category,
    description: `Idle logo animation for ${serviceKey}`,
    palette: PALETTE,
    frame_count: frames.length,
    frames,
  };

  fs.mkdirSync(OUT_DIR, { recursive: true });
  const jsonPath = path.join(OUT_DIR, data.filename);
  fs.writeFileSync(jsonPath, JSON.stringify(data, null, 2));
  console.log(`  wrote ${jsonPath} (${frames.length} frames)`);
  return data;
}

async function main() {
  fs.mkdirSync(PREVIEW_DIR, { recursive: true });
  fs.mkdirSync(OUT_DIR, { recursive: true });
  console.log(`Preview PNGs -> ${PREVIEW_DIR}`);
  console.log(`JSON data    -> ${OUT_DIR}`);

  const keys = ONLY_SERVICE ? [ONLY_SERVICE] : Object.keys(SOURCES);
  for (const key of keys) {
    if (!SOURCES[key]) throw new Error(`unknown service: ${key}`);
    console.log(`Generating ${key}...`);
    await generateService(key);
  }
  console.log('Done. Inspect preview PNGs before running convert_logo_to_c.js');
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
