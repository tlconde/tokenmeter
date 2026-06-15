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
  '#1f32d8',
  '#536cf5',
  '#8393ff',
  '#b9b2ff',
  '#ecf4ff',
  '#8fe9ff',
];

const SOURCES = {
  'codex-look': {
    filename: 'codex_look_around.json',
    name: 'codex look around',
    category: 'Codex',
    procedural: 'codex_look',
    frameCount: 16,
    palette: CODEX_PALETTE,
  },
  'codex-happy': {
    filename: 'codex_happy.json',
    name: 'codex happy',
    category: 'Codex',
    procedural: 'codex_happy',
    frameCount: 16,
    palette: CODEX_PALETTE,
  },
  'codex-terminal': {
    filename: 'codex_terminal.json',
    name: 'codex terminal',
    category: 'Codex',
    procedural: 'terminal_face',
    frameCount: 20,
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

function samplePlaced(png, scale, shiftX, shiftY, quantize) {
  const grid = [];
  for (let gy = 0; gy < GRID; gy++) {
    const row = [];
    for (let gx = 0; gx < GRID; gx++) {
      const lx = (gx + 0.5 - GRID / 2 - shiftX) / scale;
      const ly = (gy + 0.5 - GRID / 2 - shiftY) / scale;
      const u = (lx / GRID + 0.5) * png.width;
      const v = (ly / GRID + 0.5) * png.height;
      const x = Math.floor(u);
      const y = Math.floor(v);
      if (x < 0 || y < 0 || x >= png.width || y >= png.height) {
        row.push(0);
        continue;
      }
      const idx = (y * png.width + x) << 2;
      row.push(nearestPaletteIndex(
        png.data[idx], png.data[idx + 1], png.data[idx + 2], png.data[idx + 3],
        quantize,
      ));
    }
    grid.push(row);
  }
  return grid;
}

function codexFocusFrame(base, frameIdx, quantize) {
  const poses = [
    { x: 0, y: 0, scale: 0.70, hold: 180 },
    { x: 0, y: 0, scale: 0.74, hold: 120 },
    { x: 0.6, y: -0.3, scale: 0.74, hold: 120 },
    { x: 1.0, y: -0.5, scale: 0.74, hold: 240 },
    { x: 0.6, y: -0.3, scale: 0.74, hold: 120 },
    { x: 0, y: 0, scale: 0.74, hold: 180 },
    { x: -0.6, y: 0.3, scale: 0.74, hold: 120 },
    { x: -1.0, y: 0.5, scale: 0.74, hold: 240 },
    { x: -0.6, y: 0.3, scale: 0.74, hold: 120 },
    { x: 0, y: -0.5, scale: 0.76, hold: 140 },
    { x: 0, y: 0, scale: 0.74, hold: 140 },
    { x: 0, y: 0, scale: 0.70, hold: 180 },
  ];
  const pose = poses[frameIdx % poses.length];
  return {
    grid: samplePlaced(base, pose.scale, pose.x, pose.y, quantize),
    hold: pose.hold,
  };
}

function codexHappyFrame(base, frameIdx, quantize) {
  const poses = [
    { x: 0, y: 0, scale: 0.70, hold: 180 },
    { x: 0, y: -0.4, scale: 0.72, hold: 100 },
    { x: 0, y: -1.0, scale: 0.76, hold: 100 },
    { x: 0, y: -1.4, scale: 0.78, hold: 140 },
    { x: 0, y: -0.8, scale: 0.76, hold: 100 },
    { x: 0, y: 0.2, scale: 0.72, hold: 100 },
    { x: 0, y: 0.7, scale: 0.68, hold: 140 },
    { x: 0, y: 0.1, scale: 0.72, hold: 100 },
    { x: 0, y: -0.5, scale: 0.75, hold: 100 },
    { x: 0, y: -0.9, scale: 0.76, hold: 120 },
    { x: 0, y: -0.3, scale: 0.73, hold: 100 },
    { x: 0, y: 0, scale: 0.70, hold: 220 },
  ];
  const pose = poses[frameIdx % poses.length];
  return {
    grid: samplePlaced(base, pose.scale, pose.x, pose.y, quantize),
    hold: pose.hold,
  };
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

function codexCharacterFrame(frameIdx, motion) {
  const grid = emptyGrid();
  // Original Codex choreography inspired by the motion grammar common to
  // small pixel characters: hold, anticipate, stretch, hop, squash, recover,
  // look around, blink, and return to idle.
  const terminalPoses = [
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 220 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 120 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt-open', hold: 90 },
    { x: 0, y: 1, sx: 0.35, sy: -0.45, face: 'prompt', hold: 75 },
    { x: 0, y: 1, sx: 0.55, sy: -0.75, face: 'focus', hold: 75 },
    { x: 0, y: 0, sx: -0.25, sy: 0.55, face: 'focus', hold: 75 },
    { x: 0, y: -1, sx: -0.15, sy: 0.35, face: 'focus', hold: 75 },
    { x: 0, y: -2, sx: 0, sy: 0.1, face: 'prompt-open', hold: 95 },
    { x: 0, y: -2, sx: 0.1, sy: 0, face: 'prompt-open', hold: 95 },
    { x: 0, y: -1, sx: 0, sy: 0.15, face: 'prompt', hold: 75 },
    { x: 0, y: 1, sx: 0.65, sy: -0.8, face: 'blink', hold: 85 },
    { x: 0, y: 0, sx: -0.15, sy: 0.35, face: 'prompt', hold: 85 },
    { x: 0, y: 0, sx: 0.15, sy: -0.2, face: 'prompt', hold: 110 },
    { x: -1, y: 0, sx: 0, sy: 0, face: 'look-left', hold: 150 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 90 },
    { x: 1, y: 0, sx: 0, sy: 0, face: 'look-right', hold: 150 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 100 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'blink', hold: 90 },
    { x: 0, y: 0, sx: 0.1, sy: 0.1, face: 'caret', hold: 140 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 260 },
  ];
  const lookPoses = [
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 260 },
    { x: -1, y: 0, sx: 0.1, sy: 0, face: 'look-left', hold: 130 },
    { x: -1, y: 0, sx: 0, sy: 0, face: 'look-left', hold: 180 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 100 },
    { x: 1, y: 0, sx: 0.1, sy: 0, face: 'look-right', hold: 130 },
    { x: 1, y: 0, sx: 0, sy: 0, face: 'look-right', hold: 180 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 120 },
    { x: 0, y: -1, sx: -0.1, sy: 0.2, face: 'caret', hold: 150 },
    { x: 0, y: -1, sx: 0, sy: 0.1, face: 'caret', hold: 170 },
    { x: 0, y: 0, sx: 0.2, sy: -0.2, face: 'blink', hold: 90 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt-open', hold: 100 },
    { x: -1, y: 0, sx: 0, sy: 0, face: 'look-left', hold: 120 },
    { x: 1, y: 0, sx: 0, sy: 0, face: 'look-right', hold: 120 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'blink', hold: 90 },
    { x: 0, y: 0, sx: 0.1, sy: 0.1, face: 'prompt', hold: 120 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 240 },
  ];
  const happyPoses = [
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 180 },
    { x: 0, y: 1, sx: 0.55, sy: -0.65, face: 'focus', hold: 75 },
    { x: -1, y: 0, sx: -0.2, sy: 0.4, face: 'prompt-open', hold: 75 },
    { x: -1, y: -2, sx: 0, sy: 0.15, face: 'prompt-open', hold: 95 },
    { x: 0, y: -3, sx: 0.15, sy: 0, face: 'caret', hold: 95 },
    { x: 1, y: -2, sx: 0, sy: 0.15, face: 'prompt-open', hold: 95 },
    { x: 1, y: 0, sx: -0.2, sy: 0.4, face: 'prompt-open', hold: 75 },
    { x: 0, y: 1, sx: 0.7, sy: -0.8, face: 'blink', hold: 85 },
    { x: 0, y: 0, sx: -0.2, sy: 0.35, face: 'prompt', hold: 85 },
    { x: 0, y: 0, sx: 0.15, sy: -0.15, face: 'prompt-open', hold: 100 },
    { x: 1, y: -1, sx: 0, sy: 0.1, face: 'look-right', hold: 100 },
    { x: -1, y: -1, sx: 0, sy: 0.1, face: 'look-left', hold: 100 },
    { x: 0, y: 1, sx: 0.5, sy: -0.6, face: 'focus', hold: 75 },
    { x: 0, y: -2, sx: -0.1, sy: 0.35, face: 'caret', hold: 110 },
    { x: 0, y: 0, sx: 0.2, sy: -0.2, face: 'blink', hold: 90 },
    { x: 0, y: 0, sx: 0, sy: 0, face: 'prompt', hold: 240 },
  ];
  const poses = motion === 'codex_look'
    ? lookPoses
    : motion === 'codex_happy'
      ? happyPoses
      : terminalPoses;
  const pose = poses[frameIdx % poses.length];
  const cx = 9.5 + pose.x;
  const cy = 9.5 + pose.y;
  const lobes = [];
  const lobeRadius = 4.15;
  const orbitX = 4.75 + pose.sx;
  const orbitY = 4.75 + pose.sy;
  for (let i = 0; i < 7; i++) {
    const angle = -Math.PI / 2 + i * Math.PI * 2 / 7;
    lobes.push({
      x: cx + Math.cos(angle) * orbitX,
      y: cy + Math.sin(angle) * orbitY,
    });
  }

  // Round seven-lobed Codex flower/cloud. The overlapping circles avoid the
  // flat visor-like band produced by the earlier polar silhouette.
  for (let y = 0; y < GRID; y++) {
    for (let x = 0; x < GRID; x++) {
      const centerDx = (x - cx) / (5.8 + pose.sx * 0.45);
      const centerDy = (y - cy) / (5.8 + pose.sy * 0.45);
      const insideCenter = Math.hypot(centerDx, centerDy) <= 1;
      const insideLobe = lobes.some((lobe) => Math.hypot(x - lobe.x, y - lobe.y) <= lobeRadius);
      const inside = insideCenter || insideLobe;
      if (!inside) continue;

      const radial = Math.hypot(x - cx, y - cy) / 9;
      const light = (x - cx) * -0.025 + (y - cy) * -0.055;
      if (radial > 0.82) grid[y][x] = light > 0.05 ? 5 : 3;
      else if (light > 0.22) grid[y][x] = 4;
      else if (light < -0.28) grid[y][x] = 1;
      else if (light < -0.06) grid[y][x] = 2;
      else grid[y][x] = 3;
    }
  }

  // Thin luminous rim, brightest at the upper-left light source.
  const filled = grid.map((row) => row.map((cell) => cell !== 0));
  for (let y = 0; y < GRID; y++) {
    for (let x = 0; x < GRID; x++) {
      if (!filled[y][x]) continue;
      const exposed = y === 0 || y === GRID - 1 || x === 0 || x === GRID - 1 ||
        !filled[y - 1][x] || !filled[y + 1][x] ||
        !filled[y][x - 1] || !filled[y][x + 1];
      if (exposed) {
        grid[y][x] = x + y < cx + cy + 1 ? 5 : 3;
      }
    }
  }

  const faceX = Math.round(pose.x);
  const faceY = 10 + pose.y;
  const expression = pose.face;
  if (expression === 'look-left') {
    drawLine(grid, 5 + faceX, faceY - 1, 7 + faceX, faceY, 5);
    drawLine(grid, 7 + faceX, faceY, 5 + faceX, faceY + 1, 5);
    drawLine(grid, 10 + faceX, faceY + 1, 13 + faceX, faceY + 1, 5);
  } else if (expression === 'look-right') {
    drawLine(grid, 7 + faceX, faceY - 1, 9 + faceX, faceY, 5);
    drawLine(grid, 9 + faceX, faceY, 7 + faceX, faceY + 1, 5);
    drawLine(grid, 12 + faceX, faceY + 1, 15 + faceX, faceY + 1, 5);
  } else if (expression === 'focus') {
    drawLine(grid, 6 + faceX, faceY - 1, 8 + faceX, faceY, 5);
    drawLine(grid, 8 + faceX, faceY, 6 + faceX, faceY + 1, 5);
    drawLine(grid, 12 + faceX, faceY + 1, 14 + faceX, faceY + 1, 5);
  } else if (expression === 'blink') {
    drawLine(grid, 6 + faceX, faceY, 8 + faceX, faceY, 5);
    drawLine(grid, 12 + faceX, faceY, 14 + faceX, faceY, 5);
  } else if (expression === 'caret') {
    drawLine(grid, 6 + faceX, faceY - 2, 6 + faceX, faceY + 2, 5);
    drawLine(grid, 11 + faceX, faceY + 1, 13 + faceX, faceY - 1, 5);
    drawLine(grid, 13 + faceX, faceY - 1, 15 + faceX, faceY + 1, 5);
  } else {
    drawLine(grid, 5 + faceX, faceY - 2, 8 + faceX, faceY, 5);
    drawLine(grid, 8 + faceX, faceY, 5 + faceX, faceY + 2, 5);
    if (expression !== 'prompt-open') {
      drawLine(grid, 11 + faceX, faceY + 2, 15 + faceX, faceY + 2, 5);
    }
  }

  return {
    grid,
    hold: pose.hold,
  };
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
    if ((spec.procedural && spec.procedural.startsWith('codex_')) ||
        spec.procedural === 'terminal_face') {
      frame = codexCharacterFrame(i, spec.procedural);
    } else if (spec.motion === 'consume_grow') {
      frame = { hold: HOLD_MS, grid: consumingCursorFrame(baseGrid, i) };
    } else if (spec.motion === 'focus_shift') {
      frame = codexFocusFrame(base, i, spec.quantize || 'mono');
    } else if (spec.motion === 'happy_bounce') {
      frame = codexHappyFrame(base, i, spec.quantize || 'mono');
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

  const composite = compositePreview(frames, 20, palette);
  fs.writeFileSync(path.join(PREVIEW_DIR, `${serviceKey}_composite.png`), PNG.sync.write(composite));

  const data = {
    filename: spec.filename || `${serviceKey}_idle.json`,
    name: spec.name,
    category: spec.category,
    description: `Idle logo animation for ${serviceKey}`,
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
