#!/usr/bin/env node
/**
 * Generate frame-based logo animations for Codex and Cursor.
 * Rasterizes SVG marks, applies idle motion, quantizes to 40×40 palette grids,
 * writes preview PNGs for visual verification, and JSON for convert_logo_to_c.js.
 *
 * Usage: node generate_logo_anim.js [--out-dir DIR] [--preview-dir DIR]
 */

const fs = require('fs');
const path = require('path');
const https = require('https');
const { Resvg } = require('@resvg/resvg-js');
const { PNG } = require('pngjs');

const GRID = 80;
const FRAME_COUNT = 12;
const HOLD_MS = 120;
const CODEX_HOLD_MS = 133;
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
// 10-entry palette: 0 transparent, 1..8 vertical gradient (top → bottom)
// interpolated from LobeHub Codex stops #B1A7FF → #7A9DFF → #3941FF,
// 9 white (reserved for the > and _ glyph cutouts).
function buildCodexGradientPalette() {
  const stops = [
    [0.0, [0xb1, 0xa7, 0xff]],
    [0.5, [0x7a, 0x9d, 0xff]],
    [1.0, [0x39, 0x41, 0xff]],
  ];
  const sample = (t) => {
    for (let i = 1; i < stops.length; i++) {
      if (t <= stops[i][0]) {
        const a = stops[i - 1];
        const b = stops[i];
        const u = (t - a[0]) / (b[0] - a[0]);
        return [0, 1, 2].map((k) => Math.round(a[1][k] + (b[1][k] - a[1][k]) * u));
      }
    }
    return stops[stops.length - 1][1];
  };
  const toHex = ([r, g, b]) =>
    '#' + [r, g, b].map((v) => v.toString(16).padStart(2, '0')).join('');
  const out = ['transparent'];
  for (let i = 0; i < 8; i++) {
    const t = i / 7;
    out.push(toHex(sample(t)));
  }
  out.push('#ffffff');
  return out;
}
const CODEX_PALETTE = buildCodexGradientPalette();

// Cursor "on" = underscore drawn white (palette index 9);
// "off" = underscore tinted with gradient index 5 so it sinks into the body.
// Frame tables are deterministic; do not "smooth" or compress them.
const CODEX_BLINK_FRAMES = [
  { cursor: 'on',  hold: 1300 },
  { cursor: 'off', hold:  130 },
  { cursor: 'on',  hold:  950 },
  { cursor: 'off', hold:   90 },
  { cursor: 'on',  hold:   90 },
  { cursor: 'off', hold:   90 },
  { cursor: 'on',  hold: 1450 },
  { cursor: 'off', hold:  110 },
  { cursor: 'on',  hold:  580 },
];

// Per-frame affine: (sx, sy) scale about center, then rotate `rot°` about
// center, then translate (tx, ty) in grid cells (1 cell = 24/80 SVG units).
const CODEX_BALANCED_FRAMES = [
  { sx: 1.00, sy: 1.00, ty:  0,    tx:  0,    rot:  0, hold: 380, cursor: 'on'  },
  { sx: 1.00, sy: 1.00, ty: -2,    tx:  0,    rot: -4, hold: 240, cursor: 'on'  },
  { sx: 1.00, sy: 1.00, ty: -2.5,  tx:  0,    rot:  0, hold: 320, cursor: 'on'  },
  { sx: 1.00, sy: 1.00, ty:  0,    tx:  0,    rot:  0, hold: 130, cursor: 'off' },
  { sx: 1.00, sy: 1.00, ty:  0,    tx:  0,    rot:  0, hold: 200, cursor: 'on'  },
  { sx: 1.00, sy: 1.00, ty:  0,    tx:  0,    rot:  0, hold:  90, cursor: 'off' },
  { sx: 1.00, sy: 1.00, ty:  0,    tx:  0,    rot:  0, hold: 160, cursor: 'on'  },
  { sx: 1.10, sy: 0.88, ty:  3,    tx:  0,    rot:  0, hold:  80, cursor: 'on'  },
  { sx: 0.96, sy: 1.07, ty: -2,    tx:  0,    rot:  0, hold:  80, cursor: 'on'  },
  { sx: 1.00, sy: 1.00, ty: -5,    tx:  0,    rot:  0, hold: 140, cursor: 'on'  },
  { sx: 1.00, sy: 1.00, ty: -3,    tx:  0,    rot:  0, hold: 100, cursor: 'on'  },
  { sx: 1.10, sy: 0.84, ty:  3,    tx:  0,    rot:  0, hold: 200, cursor: 'off' },
  { sx: 0.96, sy: 1.05, ty: -1,    tx:  0,    rot:  0, hold: 100, cursor: 'off' },
  { sx: 1.00, sy: 1.00, ty:  0,    tx:  0,    rot:  0, hold: 180, cursor: 'on'  },
  { sx: 1.00, sy: 1.00, ty:  1,    tx: -1.5,  rot:  3, hold: 200, cursor: 'on'  },
  { sx: 1.00, sy: 1.00, ty:  0,    tx:  0,    rot:  0, hold:  80, cursor: 'off' },
  { sx: 1.00, sy: 1.00, ty:  1,    tx:  1.5,  rot: -3, hold: 200, cursor: 'on'  },
  { sx: 1.00, sy: 1.00, ty:  0,    tx:  0,    rot:  0, hold: 380, cursor: 'on'  },
];

const SOURCES = {
  'codex-blink': {
    filename: 'codex_blink.json',
    name: 'codex blink',
    category: 'Codex',
    description: 'Codex usage-screen mini: static body, terminal-prompt cursor blink',
    procedural: 'codex_blink',
    frames: CODEX_BLINK_FRAMES,
    palette: CODEX_PALETTE,
  },
  'codex-balanced': {
    filename: 'codex_balanced.json',
    name: 'codex balanced',
    category: 'Codex',
    description: 'Codex splash personality animation: drift, hop, sway, blink choreography',
    procedural: 'codex_balanced',
    frames: CODEX_BALANCED_FRAMES,
    palette: CODEX_PALETTE,
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
  'cursor-idle': {
    filename: 'cursor_idle.json',
    name: 'cursor idle',
    category: 'Logo',
    localPath: path.join(__dirname, '..', 'assets', 'logo_sources', 'cursor.svg'),
    urls: [
      'https://cursor.com/favicon.svg',
    ],
    motion: 'bob_tilt',
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

function drawLine(grid, x0, y0, x1, y1, color) {
  const dx = Math.abs(x1 - x0);
  const sx = x0 < x1 ? 1 : -1;
  const dy = -Math.abs(y1 - y0);
  const sy = y0 < y1 ? 1 : -1;
  let error = dx + dy;
  while (true) {
    if (x0 >= 0 && y0 >= 0 && x0 < GRID && y0 < GRID) grid[y0][x0] = color;
    if (x0 === x1 && y0 === y1) break;
    const twice = 2 * error;
    if (twice >= dy) {
      error += dy;
      x0 += sx;
    }
    if (twice <= dx) {
      error += dx;
      y0 += sy;
    }
  }
}

function setPixel(grid, x, y, color) {
  if (x >= 0 && y >= 0 && x < GRID && y < GRID) grid[y][x] = color;
}

const CODEX_SOURCE_PATH = path.join(__dirname, '..', 'assets', 'logo_sources', 'codex-color.svg');

let codexSvgCache = null;

function extractCodexOuterPath(svg) {
  // The blob path begins at "M9.064 3.344"; underscore subpath is appended via
  // a lowercase `zm`. Grab from M up to the first `z` followed by another m/M.
  const blobMatch = svg.match(/<path d="(M9\.064 3\.344.+?)" fill="url/s);
  if (!blobMatch) throw new Error('codex-color.svg: blob path not found');
  const outer = blobMatch[1].match(/^M.+?z(?=[mM])/s);
  if (!outer) throw new Error('codex-color.svg: failed to split outer silhouette');
  return outer[0];
}

function loadCodexSvg() {
  if (codexSvgCache) return codexSvgCache;
  if (!fs.existsSync(CODEX_SOURCE_PATH)) {
    throw new Error(`Missing Codex color source: ${CODEX_SOURCE_PATH}`);
  }
  const text = fs.readFileSync(CODEX_SOURCE_PATH, 'utf8');
  codexSvgCache = { text, outerD: extractCodexOuterPath(text) };
  return codexSvgCache;
}

// Affine string applied about the viewBox center (12,12). Transforms are in
// SVG units (viewBox 0..24); tx/ty inputs are grid cells (1 cell = 24/80).
function codexAffineString(t) {
  const sx = t?.sx ?? 1;
  const sy = t?.sy ?? 1;
  const rot = t?.rot ?? 0;
  const txCells = t?.tx ?? 0;
  const tyCells = t?.ty ?? 0;
  const txSvg = (txCells * 24) / GRID;
  const tySvg = (tyCells * 24) / GRID;
  return (
    `translate(12 12) translate(${txSvg} ${tySvg}) ` +
    `rotate(${rot}) scale(${sx} ${sy}) translate(-12 -12)`
  );
}

function renderCodexPair(transform) {
  const { text, outerD } = loadCodexSvg();
  const affine = codexAffineString(transform);

  // Tagged: swap iOS square fill to magenta, then wrap the original two paths
  // in a transformed <g>. The <defs> stays outside so the gradient still
  // resolves to its original userSpaceOnUse coordinates after transform.
  const taggedBody = text
    .replace('fill="#fff"', 'fill="#ff00ff"')
    .replace(/<title>.*?<\/title>/, '')
    .replace(/<svg[^>]*>/, '')
    .replace(/<\/svg>$/, '')
    .replace(/<defs>.*?<\/defs>/s, '');
  const defsMatch = text.match(/<defs>.*?<\/defs>/s);
  const defs = defsMatch ? defsMatch[0] : '';
  const taggedSvg =
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="${GRID * 8}" height="${GRID * 8}">` +
    defs +
    `<g transform="${affine}">${taggedBody}</g></svg>`;

  const silhSvg =
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="${GRID * 8}" height="${GRID * 8}">` +
    `<g transform="${affine}"><path d="${outerD}" fill="#ffffff"></path></g></svg>`;

  return {
    tagged: renderSvg(Buffer.from(taggedSvg, 'utf8'), GRID * 8),
    silh: renderSvg(Buffer.from(silhSvg, 'utf8'), GRID * 8),
  };
}

function quantizeCodexGrid(transform) {
  const { tagged, silh } = renderCodexPair(transform);
  const grid = emptyGrid();
  const padding = 0.04;
  const drawable = 1 - padding * 2;
  const samples = 10;

  const cellMeta = Array.from({ length: GRID }, () => Array(GRID).fill(null));
  let yMin = GRID;
  let yMax = 0;
  let blobCount = 0;
  let glyphCount = 0;

  for (let gy = 0; gy < GRID; gy++) {
    for (let gx = 0; gx < GRID; gx++) {
      let silhAlphaSum = 0;
      let inSilh = 0;
      let magentaInBlob = 0;
      let n = 0;
      for (let sy = 0; sy < samples; sy++) {
        for (let sx = 0; sx < samples; sx++) {
          const ux = ((gx + (sx + 0.5) / samples) / GRID - padding) / drawable;
          const uy = ((gy + (sy + 0.5) / samples) / GRID - padding) / drawable;
          n++;
          if (ux < 0 || uy < 0 || ux >= 1 || uy >= 1) continue;
          const x = Math.min(silh.width - 1, Math.floor(ux * silh.width));
          const y = Math.min(silh.height - 1, Math.floor(uy * silh.height));
          const sIdx = (y * silh.width + x) * 4;
          const sa = silh.data[sIdx + 3];
          silhAlphaSum += sa;
          if (sa > 64) {
            inSilh++;
            const tIdx = (y * tagged.width + x) * 4;
            const tr = tagged.data[tIdx];
            const tg = tagged.data[tIdx + 1];
            const tb = tagged.data[tIdx + 2];
            if (tr > 220 && tg < 60 && tb > 220) magentaInBlob++;
          }
        }
      }
      const cs = silhAlphaSum / (255 * n);
      if (cs < 0.04) continue;
      const gf = inSilh > 0 ? magentaInBlob / inSilh : 0;
      cellMeta[gy][gx] = { cs, gf };
      if (gf > 0.3) {
        glyphCount++;
      } else {
        blobCount++;
        if (gy < yMin) yMin = gy;
        if (gy > yMax) yMax = gy;
      }
    }
  }

  const spanY = Math.max(1, yMax - yMin);
  for (let gy = 0; gy < GRID; gy++) {
    for (let gx = 0; gx < GRID; gx++) {
      const meta = cellMeta[gy][gx];
      if (!meta) continue;
      if (meta.gf > 0.3) {
        grid[gy][gx] = 9;
      } else {
        let t = (gy - yMin) / spanY;
        t -= (1 - meta.cs) * 0.1;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        const idx = Math.max(1, Math.min(8, 1 + Math.round(t * 7)));
        grid[gy][gx] = idx;
      }
    }
  }

  return { grid, blobCount, glyphCount };
}

let codexBaseGrid = null;
function buildCodexBaseGrid() {
  if (codexBaseGrid) return codexBaseGrid;
  const { grid, blobCount, glyphCount } = quantizeCodexGrid(null);
  console.log(`  codex base: ${blobCount} blob cells, ${glyphCount} glyph cells`);
  if (glyphCount === 0) {
    throw new Error('codex base: 0 glyph cells — silhouette path extraction likely failed');
  }
  codexBaseGrid = grid;
  return codexBaseGrid;
}

// Blink overlay: find connected components of cells valued 9 (white glyphs),
// keep the larger (chevron `>`) untouched, replace the smaller (underscore `_`)
// with index 5 so it sinks into the gradient body.
function applyBlinkOff(grid) {
  const visited = Array.from({ length: GRID }, () => Array(GRID).fill(false));
  const components = [];
  for (let y = 0; y < GRID; y++) {
    for (let x = 0; x < GRID; x++) {
      if (grid[y][x] !== 9 || visited[y][x]) continue;
      const stack = [[x, y]];
      const cells = [];
      visited[y][x] = true;
      while (stack.length) {
        const [cx, cy] = stack.pop();
        cells.push([cx, cy]);
        for (const [dx, dy] of [
          [1, 0], [-1, 0], [0, 1], [0, -1],
        ]) {
          const nx = cx + dx;
          const ny = cy + dy;
          if (nx < 0 || ny < 0 || nx >= GRID || ny >= GRID) continue;
          if (visited[ny][nx] || grid[ny][nx] !== 9) continue;
          visited[ny][nx] = true;
          stack.push([nx, ny]);
        }
      }
      components.push(cells);
    }
  }
  if (components.length < 2) return;
  components.sort((a, b) => a.length - b.length);
  const smallest = components[0];
  for (const [x, y] of smallest) grid[y][x] = 5;
}

function codexBlinkFrame(spec) {
  const base = buildCodexBaseGrid();
  const grid = base.map((row) => row.slice());
  if (spec.cursor === 'off') applyBlinkOff(grid);
  return { grid, hold: spec.hold };
}

function codexBalancedFrame(spec) {
  const isIdentity =
    (spec.sx ?? 1) === 1 &&
    (spec.sy ?? 1) === 1 &&
    (spec.tx ?? 0) === 0 &&
    (spec.ty ?? 0) === 0 &&
    (spec.rot ?? 0) === 0;
  let grid;
  if (isIdentity) {
    grid = buildCodexBaseGrid().map((row) => row.slice());
  } else {
    grid = quantizeCodexGrid(spec).grid;
  }
  if (spec.cursor === 'off') applyBlinkOff(grid);
  return { grid, hold: spec.hold };
}

function validateCodexBlinkFrames(serviceKey, frames) {
  const base = buildCodexBaseGrid();
  for (let i = 0; i < frames.length; i++) {
    for (let y = 0; y < GRID; y++) {
      for (let x = 0; x < GRID; x++) {
        const bv = base[y][x];
        const fv = frames[i].grid[y][x];
        if (bv >= 1 && bv <= 8 && fv !== bv) {
          throw new Error(
            `${serviceKey}: frame ${i} body cell ${x},${y} changed (${bv}→${fv}); blink must hold body still`
          );
        }
      }
    }
  }
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

// Snacks are 4-cell sprites in the original 40-grid space. Scale up so they
// stay proportionally visible on the 80-grid without becoming pinpricks.
function cursorParticle(grid, x, y, color) {
  const sc = GRID / 40;
  const points = [
    [1, 0], [2, 0],
    [0, 1], [3, 1],
    [0, 2], [2, 2],
    [1, 3], [2, 3],
  ];
  for (const [dx, dy] of points) {
    for (let oy = 0; oy < sc; oy++) {
      for (let ox = 0; ox < sc; ox++) {
        const px = Math.round(x + dx * sc + ox);
        const py = Math.round(y + dy * sc + oy);
        if (px >= 0 && py >= 0 && px < GRID && py < GRID) grid[py][px] = color;
      }
    }
  }
}

function consumingCursorFrame(base, frameIdx) {
  // Eater cube travels from left-of-center toward canvas center, vertically
  // centered. Snacks scatter around the right half so the cube reaches them
  // as it grows. Scale-agnostic — everything is expressed as a fraction of
  // GRID rather than baked-in 40-grid coordinates.
  const grid = emptyGrid();
  const progress = Math.min(frameIdx / 8, 1);
  const cy = GRID / 2;
  const eaterX = GRID * (0.28 + progress * 0.22);
  const eaterScale = 0.42 + progress * 0.58;

  const snacks = [
    { eatenAt: 0.24, x: GRID * 0.68, y: GRID * 0.35 },
    { eatenAt: 0.50, x: GRID * 0.72, y: GRID * 0.62 },
    { eatenAt: 0.82, x: GRID * 0.58, y: GRID * 0.78 },
  ];
  for (const snack of snacks) {
    if (progress < snack.eatenAt) cursorParticle(grid, snack.x, snack.y, 2);
  }

  // The Cursor mark itself is the eater: it advances through the smaller
  // marks and grows after every bite until it reaches the original max size.
  stampMask(grid, base, eaterX, cy, eaterScale, 1);
  return grid;
}

function paletteToRgba(palette) {
  return palette.map((color, index) => {
    if (index === 0 || color === 'transparent') return [0, 0, 0, 255];
    const hex = color.replace('#', '');
    if (!/^[0-9a-fA-F]{6}$/.test(hex)) throw new Error(`invalid palette color: ${color}`);
    return [
      Number.parseInt(hex.slice(0, 2), 16),
      Number.parseInt(hex.slice(2, 4), 16),
      Number.parseInt(hex.slice(4, 6), 16),
      255,
    ];
  });
}

function gridToPreviewPng(grid, cellSize = 16, palette = PALETTE) {
  const w = GRID * cellSize;
  const h = GRID * cellSize;
  const out = new PNG({ width: w, height: h });
  const colors = paletteToRgba(palette);
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

function compositePreview(frames, cellSize = 16, palette = PALETTE) {
  const fw = GRID * cellSize;
  const fh = GRID * cellSize;
  const cols = Math.min(frames.length, 4);
  const rows = Math.ceil(frames.length / cols);
  const out = new PNG({ width: fw * cols, height: fh * rows });
  for (let i = 0; i < frames.length; i++) {
    const png = gridToPreviewPng(frames[i].grid, cellSize, palette);
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
  const palette = spec.palette || PALETTE;
  const isCodex = spec.procedural && spec.procedural.startsWith('codex_');
  const frameCount = spec.frames
    ? spec.frames.length
    : spec.frameCount || FRAME_COUNT;
  let base = null;
  let baseGrid = null;
  if (!isCodex && !spec.procedural) {
    const svg = await loadSvg(serviceKey);
    base = renderSvg(svg, RENDER_SIZE);
    baseGrid = sampleTransformed(base, 0, FRAME_COUNT, 'still', spec.quantize || 'mono');
  }

  const frames = [];
  for (let i = 0; i < frameCount; i++) {
    let frame;
    if (spec.procedural === 'codex_blink') {
      frame = codexBlinkFrame(spec.frames[i]);
    } else if (spec.procedural === 'codex_balanced') {
      frame = codexBalancedFrame(spec.frames[i]);
    } else if (spec.motion === 'consume_grow') {
      frame = { hold: HOLD_MS, grid: consumingCursorFrame(baseGrid, i) };
    } else {
      frame = {
        hold: HOLD_MS,
        grid: sampleTransformed(base, i, FRAME_COUNT, spec.motion, spec.quantize || 'mono'),
      };
    }
    frames.push(frame);
    const grid = frame.grid;
    const preview = gridToPreviewPng(grid, 20, palette);
    const framePath = path.join(PREVIEW_DIR, `${serviceKey}_frame_${String(i).padStart(2, '0')}.png`);
    fs.writeFileSync(framePath, PNG.sync.write(preview));
  }
  if (spec.procedural === 'codex_blink') {
    validateCodexBlinkFrames(serviceKey, frames);
  }

  const composite = compositePreview(frames, 20, palette);
  fs.writeFileSync(path.join(PREVIEW_DIR, `${serviceKey}_composite.png`), PNG.sync.write(composite));

  const data = {
    filename: spec.filename || `${serviceKey}_idle.json`,
    name: spec.name,
    category: spec.category,
    description: spec.description || `Idle logo animation for ${serviceKey}`,
    palette,
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
