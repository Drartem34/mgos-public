.pragma library
// Спільне для піксельних елементів теми mgOS: шрифт 5×7 (як у годиннику топбару й інсталятора),
// матриця дизеру Байєра і кольори акценту.

var BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]];

var GLYPHS = {
    "0": [5, "01110100011001110101110011000101110"],
    "1": [5, "00100011000010000100001000010001110"],
    "2": [5, "01110100010000100010001000100011111"],
    "3": [5, "11110000010000101110000010000111110"],
    "4": [5, "00010001100101010010111110001000010"],
    "5": [5, "11111100001111000001000011000101110"],
    "6": [5, "00110010001000011110100011000101110"],
    "7": [5, "11111000010001000100010000100001000"],
    "8": [5, "01110100011000101110100011000101110"],
    "9": [5, "01110100011000101111000010001001100"],
    "A": [5, "01110100011000111111100011000110001"],
    "P": [5, "11110100011000111110100001000010000"],
    "M": [5, "10001110111010110101100011000110001"],
    "-": [5, "00000000000000011111000000000000000"],
    ":": [2, "00111100111100"],
    " ": [2, "00000000000000"]
};

function glyph(ch) { return GLYPHS[ch] || GLYPHS[" "]; }

function textCols(s, bold) {
    var cols = 0;
    for (var i = 0; i < s.length; i++) cols += glyph(s[i])[0] + (bold ? 1 : 0) + (i + 1 < s.length ? 1 : 0);
    return cols;
}

function clamp(v, a, b) { return Math.max(a, Math.min(b, v)); }

// h — градуси, s/l — 0..1 → "rgba(...)"
function hsl(h, s, l, a) {
    h = ((h % 360) + 360) % 360 / 360;
    var r, g, b;
    if (s <= 0) { r = g = b = l; }
    else {
        var q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        function hue2(t) {
            if (t < 0) t += 1;
            if (t > 1) t -= 1;
            if (t < 1 / 6) return p + (q - p) * 6 * t;
            if (t < 0.5) return q;
            if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
            return p;
        }
        r = hue2(h + 1 / 3); g = hue2(h); b = hue2(h - 1 / 3);
    }
    return "rgba(" + Math.round(r * 255) + "," + Math.round(g * 255) + "," + Math.round(b * 255) + "," + (a === undefined ? 1 : a) + ")";
}

// Колір піксельних елементів — рівно акцент системи з легким градієнтом яскравості (як accent_px у SYSui)
function accentPx(acc, x01, light, a) {
    var h = acc.hslHue < 0 ? 280 : acc.hslHue * 360;
    // світлі пастельні акценти темнішають до ~0.6, щоб білий гліф на кнопці читався
    var l = clamp(light + (acc.hslLightness - 0.56) * 0.6 + (x01 - 0.5) * 0.04, 0, 0.62);
    return hsl(h + (x01 - 0.5) * 6, clamp(acc.hslSaturation, 0.5, 0.95), l, a);
}

// Піксельна «плашка» (кнопка / поле / картка): сітка клітинок cell з зазором gap, сходинкові кути.
//   kind: 0 — нейтральна (біле скло), 1 — акцентна, 2 — темна картка
function drawPanel(ctx, w, h, opt) {
    var cell = opt.cell || 3, gap = opt.gap === undefined ? 1 : opt.gap;
    var cols = Math.floor(w / cell), rows = Math.floor(h / cell);
    ctx.clearRect(0, 0, w, h);
    if (cols < 4 || rows < 3) return;
    var ox = (w - cols * cell) / 2, oy = (h - rows * cell) / 2;
    var round = opt.round === undefined ? 1 : opt.round;
    for (var y = 0; y < rows; y++) {
        for (var x = 0; x < cols; x++) {
            var cx = Math.min(x, cols - 1 - x), cy = Math.min(y, rows - 1 - y);
            if (cx + cy < round) continue;                          // піксельні кути
            var edge = cx === 0 || cy === 0;
            var bay = BAYER[y & 3][x & 3] / 16;
            var lift = opt.hover ? 0.16 * Math.exp(-Math.pow((x - y * 0.6 - opt.sweep) / 3, 2)) : 0;
            if (opt.press) lift -= 0.08;
            var col;
            if (opt.kind === 1) {
                var l = 0.56 + (bay - 0.5) * 0.05 + lift;
                if (y === 0 || (cy === 0 && y < rows / 2)) l += 0.1;
                if (y === rows - 1) l -= 0.12;
                col = accentPx(opt.accent, x / cols, clamp(l, 0, 0.92), opt.alpha === undefined ? 1 : opt.alpha);
            } else if (opt.kind === 2) {
                var a2 = (opt.alpha === undefined ? 0.5 : opt.alpha) + (bay - 0.5) * 0.04;
                col = edge ? (opt.focus ? accentPx(opt.accent, x / cols, 0.62, 0.95) : "rgba(255,255,255," + (0.10 + lift * 0.6) + ")")
                           : "rgba(8,8,16," + clamp(a2, 0, 1) + ")";
            } else {
                var a = (opt.hover ? 0.2 : 0.12) + (edge ? 0.06 : 0) + lift * 0.6 + (bay - 0.5) * 0.02;
                col = "rgba(255,255,255," + clamp(a, 0, 1) + ")";
            }
            ctx.fillStyle = col;
            ctx.fillRect(ox + x * cell, oy + y * cell, cell - gap, cell - gap);
        }
    }
}
