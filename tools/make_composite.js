#!/usr/bin/env node
/** Build a 1200×630 portfolio hero from the Tokenmeter selector screen. */

const fs = require("fs");
const path = require("path");
const { PNG } = require("pngjs");

const OUT_WIDTH = 1200;
const OUT_HEIGHT = 630;
const SCREEN_HEIGHT = 520;
const RADIUS = 14;

const BG_TOP = { r: 0x24, g: 0x21, b: 0x1a };
const BG_BOTTOM = { r: 0x2e, g: 0x2a, b: 0x22 };
const BORDER = { r: 0xff, g: 0xff, b: 0xff, a: 0.1 };

function loadPng(filePath) {
  return PNG.sync.read(fs.readFileSync(filePath));
}

function scaleNearest(src, targetW, targetH) {
  const dst = new PNG({ width: targetW, height: targetH });
  for (let y = 0; y < targetH; y++) {
    for (let x = 0; x < targetW; x++) {
      const sx = Math.min(
        src.width - 1,
        Math.floor((x * src.width) / targetW),
      );
      const sy = Math.min(
        src.height - 1,
        Math.floor((y * src.height) / targetH),
      );
      const si = (src.width * sy + sx) << 2;
      const di = (targetW * y + x) << 2;
      dst.data[di] = src.data[si];
      dst.data[di + 1] = src.data[si + 1];
      dst.data[di + 2] = src.data[si + 2];
      dst.data[di + 3] = src.data[si + 3];
    }
  }
  return dst;
}

function lerp(a, b, t) {
  return Math.round(a + (b - a) * t);
}

function bgColor(y) {
  const t = y / (OUT_HEIGHT - 1);
  return {
    r: lerp(BG_TOP.r, BG_BOTTOM.r, t),
    g: lerp(BG_TOP.g, BG_BOTTOM.g, t),
    b: lerp(BG_TOP.b, BG_BOTTOM.b, t),
  };
}

function insideRoundedRect(x, y, w, h, r) {
  if (x < r && y < r) {
    return (x - r) ** 2 + (y - r) ** 2 <= r ** 2;
  }
  if (x >= w - r && y < r) {
    return (x - (w - r - 1)) ** 2 + (y - r) ** 2 <= r ** 2;
  }
  if (x < r && y >= h - r) {
    return (x - r) ** 2 + (y - (h - r - 1)) ** 2 <= r ** 2;
  }
  if (x >= w - r && y >= h - r) {
    return (x - (w - r - 1)) ** 2 + (y - (h - r - 1)) ** 2 <= r ** 2;
  }
  return true;
}

function blend(dst, di, color, alpha) {
  const inv = 1 - alpha;
  dst.data[di] = Math.round(color.r * alpha + dst.data[di] * inv);
  dst.data[di + 1] = Math.round(color.g * alpha + dst.data[di + 1] * inv);
  dst.data[di + 2] = Math.round(color.b * alpha + dst.data[di + 2] * inv);
  dst.data[di + 3] = 255;
}

function main() {
  const inputDir = path.resolve(__dirname, "../screenshots/amoled_18");
  const outPath = process.argv[2];
  if (!outPath) {
    console.error("usage: node make_composite.js <output.png>");
    process.exit(1);
  }

  const src = loadPng(path.join(inputDir, "selector.png"));
  const panelH = SCREEN_HEIGHT;
  const panelW = Math.round((panelH * src.width) / src.height);
  const panel = scaleNearest(src, panelW, panelH);

  const canvas = new PNG({ width: OUT_WIDTH, height: OUT_HEIGHT });
  for (let y = 0; y < OUT_HEIGHT; y++) {
    const bg = bgColor(y);
    for (let x = 0; x < OUT_WIDTH; x++) {
      const i = (OUT_WIDTH * y + x) << 2;
      canvas.data[i] = bg.r;
      canvas.data[i + 1] = bg.g;
      canvas.data[i + 2] = bg.b;
      canvas.data[i + 3] = 255;
    }
  }

  const x0 = Math.floor((OUT_WIDTH - panelW) / 2);
  const y0 = Math.floor((OUT_HEIGHT - panelH) / 2);

  for (let py = 0; py < panelH; py++) {
    for (let px = 0; px < panelW; px++) {
      if (!insideRoundedRect(px, py, panelW, panelH, RADIUS)) continue;
      const si = (panelW * py + px) << 2;
      const dx = x0 + px;
      const dy = y0 + py;
      const di = (OUT_WIDTH * dy + dx) << 2;
      const alpha = panel.data[si + 3] / 255;
      blend(canvas, di, { r: panel.data[si], g: panel.data[si + 1], b: panel.data[si + 2] }, alpha);
    }
  }

  for (let py = 0; py < panelH; py++) {
    for (let px = 0; px < panelW; px++) {
      const onEdge =
        !insideRoundedRect(px, py, panelW, panelH, RADIUS) &&
        (insideRoundedRect(px + 1, py, panelW, panelH, RADIUS) ||
          insideRoundedRect(px - 1, py, panelW, panelH, RADIUS) ||
          insideRoundedRect(px, py + 1, panelW, panelH, RADIUS) ||
          insideRoundedRect(px, py - 1, panelW, panelH, RADIUS));
      if (!onEdge) continue;
      const dx = x0 + px;
      const dy = y0 + py;
      const di = (OUT_WIDTH * dy + dx) << 2;
      blend(canvas, di, BORDER, BORDER.a);
    }
  }

  fs.writeFileSync(outPath, PNG.sync.write(canvas));
  console.log(`wrote ${outPath} (${OUT_WIDTH}x${OUT_HEIGHT})`);
}

main();
