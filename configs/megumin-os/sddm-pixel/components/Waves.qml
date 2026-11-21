import QtQuick
import "Px.js" as Px

// Живі піксельні хвилі на весь екран (як шапка «Про систему» / інсталятора mgOS).
// Малюються в маленьке полотно (екран / cell) і розтягуються без згладжування.
Item {
    id: root
    property color accent: "#9929CF"
    property int cell: 8
    property real t: 0
    property bool running: true

    readonly property int cols: Math.max(8, Math.ceil(width / cell))
    readonly property int rows: Math.max(8, Math.ceil(height / cell))

    Canvas {
        id: cv
        width: root.cols
        height: root.rows
        smooth: false
        renderStrategy: Canvas.Cooperative
        transform: Scale { xScale: root.cell; yScale: root.cell }
        property var pal: []

        function buildPalette() {
            var hue = root.accent.hslHue < 0 ? 280 : root.accent.hslHue * 360;
            var hh0 = hue + 15 * Math.sin(root.t * 0.21 + 0.6 * Math.sin(root.t * 0.13));
            var s = Px.clamp(root.accent.hslSaturation, 0.4, 0.92);
            var HB = 12, L = 6, out = [];
            for (var b = 0; b < HB; b++) {
                var hh = hh0 + 15 * (b / (HB - 1) - 0.5), band = [];
                var hn = ((hh % 360) + 360) % 360 / 360;
                for (var q = 0; q < L; q++) band.push(Qt.hsla(hn, s, 0.13 + q * 0.085, 1));   // готові кольори — без розбору рядків
                band.push(Qt.hsla(hn, 0.55, 0.86, 1));   // піна
                out.push(band);
            }
            pal = out;
        }

        // putImageData у Qt Canvas не малює — тож fillRect, але відрізками однакового кольору в рядку
        onPaint: {
            var ctx = getContext("2d");
            var W = width, H = height, t = root.t, L = 6, HB = 12;
            buildPalette();
            var cx = W * (0.5 + 0.35 * Math.sin(t * 0.23)), cy = H * (0.45 + 0.3 * Math.cos(t * 0.31));
            for (var y = 0; y < H; y++) {
                var fy = y / H, run = 0, runCol = null;
                for (var x = 0; x <= W; x++) {
                    var col = null;
                    if (x < W) {
                        var bay = (Px.BAYER[y & 3][x & 3] + 0.5) / 16;
                        var b = Math.min(HB - 1, Math.floor(x * HB / W));
                        var dx = x - cx, dy = (y - cy) * 1.6;
                        var v = 1.25 * Math.sin(x * 0.085 + y * 0.17 - t * 1.3)
                              + 0.75 * Math.sin(x * 0.16 - y * 0.08 + t * 0.9)
                              + 0.45 * Math.sin(x * 0.045 + y * 0.31 + t * 1.75)
                              + 0.4 * Math.sin(Math.sqrt(dx * dx + dy * dy) * 0.38 - t * 2.4);
                        v = (v / 2.85 + 1) * 0.5 - (fy - 0.5) * 0.12;
                        var vq = v + (bay - 0.5) / L;
                        col = vq > 0.93 ? pal[b][L] : pal[b][Math.max(0, Math.min(L - 1, Math.floor(vq * L)))];
                    }
                    if (col === runCol) { run++; continue; }
                    if (runCol !== null) { ctx.fillStyle = runCol; ctx.fillRect(x - run, y, run, 1); }
                    runCol = col; run = 1;
                }
            }
        }
    }

    Timer {
        interval: 125   // 8 кадрів/с — хвилі повільні, більше не треба (≈15% одного ядра)
        repeat: true
        running: root.running && root.visible
        onTriggered: { root.t += interval / 1000; cv.requestPaint(); }
    }
    Component.onCompleted: cv.requestPaint()
}
