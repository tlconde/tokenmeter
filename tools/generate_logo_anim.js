#!/usr/bin/env node
/**
 * Generate frame-based logo animations for Codex and Cursor.
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
const CODEX_PALETTE = [
  'transparent',
  '#11184f',
  '#263fcb',
  '#4f6df5',
  '#796ff2',
  '#a89cff',
  '#def5ff',
  '#58d9ff',
  '#9b5cff',
];

const SOURCES = {
  'codex-terminal': {
    filename: 'codex_terminal.json',
    name: 'codex terminal',
    category: 'Codex',
    description: 'Calm Codex terminal idle with a breathing core and live caret',
    procedural: 'codex_terminal',
    frameCount: 18,
    palette: CODEX_PALETTE,
  },
  'codex-scan': {
    filename: 'codex_scan.json',
    name: 'codex scan',
    category: 'Codex',
    description: 'Codex scans its code rails and resolves the signal at center',
    procedural: 'codex_scan',
    frameCount: 18,
    palette: CODEX_PALETTE,
  },
  'codex-compile': {
    filename: 'codex_compile.json',
    name: 'codex compile',
    category: 'Codex',
    description: 'Codex compiles a prompt into a completed check',
    procedural: 'codex_compile',
    frameCount: 20,
    palette: CODEX_PALETTE,
  },
  'codex-orbit': {
    filename: 'codex_orbit.json',
    name: 'codex orbit',
    category: 'Codex',
    description: 'Codex routes luminous packets around its blossom',
    procedural: 'codex_orbit',
    frameCount: 24,
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

function codexFlowerMask(x, y) {
  const cx = 9.5;
  const cy = 9.5;
  if (Math.hypot(x - cx, y - cy) <= 4) return true;
  for (let i = 0; i < 7; i++) {
    const angle = -Math.PI / 2 + i * Math.PI * 2 / 7;
    const lobeX = cx + Math.cos(angle) * 5.1;
    const lobeY = cy + Math.sin(angle) * 5.1;
    if (Math.hypot(x - lobeX, y - lobeY) <= 3.1) return true;
  }
  return false;
}

function drawCodexFlower(grid) {
  const filled = emptyGrid().map((row, y) =>
    row.map((_, x) => codexFlowerMask(x, y))
  );
  for (let y = 0; y < GRID; y++) {
    for (let x = 0; x < GRID; x++) {
      if (!filled[y][x]) continue;
      const edge = y === 0 || y === GRID - 1 || x === 0 || x === GRID - 1 ||
        !filled[y - 1][x] || !filled[y + 1][x] ||
        !filled[y][x - 1] || !filled[y][x + 1];
      if (edge) {
        grid[y][x] = 5;
      } else if (y <= 6) {
        grid[y][x] = 4;
      } else if (y <= 10) {
        grid[y][x] = 3;
      } else if (y <= 14) {
        grid[y][x] = 2;
      } else {
        grid[y][x] = 1;
      }
    }
  }
}

const PROMPT_BITMAP = ['100', '010', '001', '010', '100'];

function drawBitmap(grid, bitmap, x, y, color) {
  for (let by = 0; by < bitmap.length; by++) {
    for (let bx = 0; bx < bitmap[by].length; bx++) {
      if (bitmap[by][bx] === '1') setPixel(grid, x + bx, y + by, color);
    }
  }
}

function drawPrompt(grid, caretColor = 6, promptColor = 6) {
  drawBitmap(grid, PROMPT_BITMAP, 6, 7, promptColor);
  drawLine(grid, 11, 11, 13, 11, caretColor);
}

function drawScanActivity(grid, frameIdx, frameCount) {
  drawPrompt(grid, 4, 5);
  const sequence = [6, 6, 7, 8, 9, 10, 11, 12, 13, 13, 12, 11, 10, 9, 8, 7, 6, 6];
  const scannerX = sequence[frameIdx % sequence.length];
  drawLine(grid, scannerX, 8, scannerX, 12, 7);
  setPixel(grid, scannerX, 10, 6);
}

function drawCompileActivity(grid, frameIdx) {
  if (frameIdx < 4) {
    drawPrompt(grid, frameIdx % 2 ? 4 : 6, 6);
  } else if (frameIdx < 9) {
    drawPrompt(grid, 4, 5);
    const count = frameIdx - 3;
    for (let i = 0; i < count; i++) setPixel(grid, 9 + i, 9 + i % 2, i % 2 ? 8 : 7);
  } else if (frameIdx < 13) {
    drawPrompt(grid, 4, 5);
    const core = [[9, 9], [10, 9], [9, 10], [10, 10]];
    for (const [x, y] of core) setPixel(grid, x, y, frameIdx === 12 ? 6 : 7);
  } else if (frameIdx < 17) {
    drawLine(grid, 6, 10, 8, 12, 7);
    drawLine(grid, 8, 12, 13, 7, 6);
  } else {
    drawPrompt(grid, frameIdx === 17 ? 7 : 4, 6);
  }
}

function drawOrbitActivity(grid, frameIdx, frameCount) {
  drawPrompt(grid, 6, 5);
  for (let packet = 0; packet < 2; packet++) {
    const phase = (frameIdx / frameCount + packet / 2) * Math.PI * 2 - Math.PI / 2;
    const previous = phase - Math.PI * 2 / frameCount;
    const x = Math.round(9.5 + Math.cos(phase) * 5.2);
    const y = Math.round(9.5 + Math.sin(phase) * 5.2);
    const tailX = Math.round(9.5 + Math.cos(previous) * 5.2);
    const tailY = Math.round(9.5 + Math.sin(previous) * 5.2);
    setPixel(grid, tailX, tailY, packet === 1 ? 4 : 3);
    setPixel(grid, x, y, packet === 1 ? 8 : 7);
  }
}

function codexCharacterFrame(frameIdx, motion, frameCount) {
  const grid = emptyGrid();
  drawCodexFlower(grid);

  if (motion === 'codex_scan') {
    drawScanActivity(grid, frameIdx, frameCount);
  } else if (motion === 'codex_compile') {
    drawCompileActivity(grid, frameIdx);
  } else if (motion === 'codex_orbit') {
    drawOrbitActivity(grid, frameIdx, frameCount);
  } else {
    const caretOn = frameIdx < 7 || frameIdx >= 12;
    drawPrompt(grid, caretOn ? 6 : 3);
  }

  return {
    grid,
    hold: motion === 'codex_terminal' && frameIdx === 0 ? 420 : 120,
  };
}

function validateCodexFrames(serviceKey, frames) {
  const maskCells = [];
  for (let y = 0; y < GRID; y++) {
    for (let x = 0; x < GRID; x++) {
      if (codexFlowerMask(x, y)) maskCells.push([x, y]);
    }
  }
  if (maskCells.length < 185 || maskCells.length > 195) {
    throw new Error(`${serviceKey}: Codex body uses ${maskCells.length} cells; expected 185-195`);
  }
  const xs = maskCells.map(([x]) => x);
  const ys = maskCells.map(([, y]) => y);
  const bounds = [Math.min(...xs), Math.min(...ys), Math.max(...xs), Math.max(...ys)];
  if (bounds.join(',') !== '2,2,17,17') {
    throw new Error(`${serviceKey}: Codex body bounds ${bounds.join(',')}; expected 2,2,17,17`);
  }

  for (let i = 0; i < frames.length; i++) {
    for (const [x, y] of maskCells) {
      if (frames[i].grid[y][x] === 0) {
        throw new Error(`${serviceKey}: frame ${i} removes body cell ${x},${y}`);
      }
    }
    if (i === 0) continue;
    let changed = 0;
    for (let y = 0; y < GRID; y++) {
      for (let x = 0; x < GRID; x++) {
        if (frames[i - 1].grid[y][x] !== frames[i].grid[y][x]) changed++;
      }
    }
    if (changed > 20) {
      throw new Error(`${serviceKey}: frame ${i - 1}->${i} changes ${changed} cells; max 20`);
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
  const frameCount = spec.frameCount || FRAME_COUNT;
  let base = null;
  let baseGrid = null;
  if (!spec.procedural) {
    const svg = await loadSvg(serviceKey);
    base = renderSvg(svg, RENDER_SIZE);
    baseGrid = sampleTransformed(base, 0, FRAME_COUNT, 'still', spec.quantize || 'mono');
  }

  const frames = [];
  for (let i = 0; i < frameCount; i++) {
    let frame;
    if (spec.procedural && spec.procedural.startsWith('codex_')) {
      frame = codexCharacterFrame(i, spec.procedural, frameCount);
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
  if (spec.procedural && spec.procedural.startsWith('codex_')) {
    validateCodexFrames(serviceKey, frames);
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
