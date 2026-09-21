/** Soft green-screen matting for non-green characters, at native source resolution. */
export function matteGreen(data: Buffer, width: number, height: number): Buffer {
    const size = width * height, foreground = new Uint8Array(size), core = new Uint8Array(size);
    for (let i = 0; i < size; i++) {
        const p = i * 4;
        foreground[i] = data[p + 1] - Math.max(data[p], data[p + 2]) < 18 ? 1 : 0;
    }
    for (let y = 2; y < height - 2; y++) for (let x = 2; x < width - 2; x++) {
        const i = y * width + x;
        if (!foreground[i]) continue;
        let solid = true;
        for (let dy = -2; dy <= 2 && solid; dy++) for (let dx = -2; dx <= 2; dx++) {
            if (!foreground[i + dy * width + dx]) { solid = false; break; }
        }
        if (solid) core[i] = 1;
    }
    const pixels = Buffer.alloc(data.length);
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
        const i = y * width + x, p = i * 4, r = data[p], g = data[p + 1], b = data[p + 2];
        if (core[i]) { data.copy(pixels, p, p, p + 4); continue; }
        if (g > 180 && r < 25 && b < 25) continue;
        let nearest = -1, best = 50;
        for (let dy = -7; dy <= 7; dy++) for (let dx = -7; dx <= 7; dx++) {
            const distance = dx * dx + dy * dy;
            if (distance >= best || x + dx < 0 || x + dx >= width || y + dy < 0 || y + dy >= height) continue;
            const j = (y + dy) * width + x + dx;
            if (core[j]) { nearest = j; best = distance; }
        }
        if (nearest < 0) { if (foreground[i]) data.copy(pixels, p, p, p + 4); continue; }
        const q = nearest * 4, key = [0, 255, 0], color = [r, g, b], reference = [data[q], data[q + 1], data[q + 2]];
        let dot = 0, length = 0;
        for (let c = 0; c < 3; c++) { dot += (color[c] - key[c]) * (reference[c] - key[c]); length += (reference[c] - key[c]) ** 2; }
        let alpha = Math.max(0, Math.min(1, dot / Math.max(1, length)));
        if (foreground[i]) alpha = Math.max(alpha, 0.95);
        if (alpha < 0.025) continue;
        for (let c = 0; c < 3; c++) pixels[p + c] = Math.round(Math.max(0, Math.min(255, (color[c] - (1 - alpha) * key[c]) / alpha)));
        if (alpha < 0.98) {
            for (let c = 0; c < 3; c++) pixels[p + c] = Math.round(0.75 * reference[c] + 0.25 * pixels[p + c]);
            pixels[p + 1] = Math.min(pixels[p + 1], Math.max(pixels[p], pixels[p + 2]));
        }
        pixels[p + 3] = Math.round(alpha * 255);
    }
    return pixels;
}
