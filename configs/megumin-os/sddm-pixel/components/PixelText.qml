import QtQuick
import "Px.js" as Px

// Піксельний текст шрифтом 5×7 (цифри годинника). wave — «живий» колір акценту, як у топбарі mgOS.
Canvas {
    id: root
    property string text: "00:00"
    property int cell: 8
    property bool bold: true
    property bool wave: false
    property bool shadow: true
    property color color: "#f5f5f7"
    property color accent: "#9929CF"
    property real t: 0
    property real alpha: 1

    width: (Px.textCols(text, bold) + 1) * cell
    height: 8 * cell
    renderStrategy: Canvas.Cooperative
    onTextChanged: requestPaint()
    onCellChanged: requestPaint()
    onWaveChanged: requestPaint()
    onTChanged: if (wave) requestPaint()
    onAccentChanged: requestPaint()

    onPaint: {
        var ctx = getContext("2d");
        ctx.reset();
        ctx.clearRect(0, 0, width, height);
        var hue = accent.hslHue < 0 ? 280 : accent.hslHue * 360;
        var sat = Px.clamp(accent.hslSaturation, 0.4, 0.92);
        for (var pass = shadow ? 0 : 1; pass < 2; pass++) {
            var gx0 = 0;
            for (var i = 0; i < text.length; i++) {
                var g = Px.glyph(text[i]), w = g[0], b = g[1];
                var dw = w + (bold ? 1 : 0);
                for (var r = 0; r < 7; r++) {
                    for (var c = 0; c < dw; c++) {
                        var on = (c < w && b[r * w + c] === "1") || (bold && c >= 1 && c - 1 < w && b[r * w + c - 1] === "1");
                        if (!on) continue;
                        var gx = gx0 + c, px = gx * cell, py = r * cell;
                        if (pass === 0) {
                            ctx.fillStyle = "rgba(0,0,0," + (0.45 * alpha) + ")";
                            ctx.fillRect(px + cell * 0.4, py + cell * 0.4, cell - 1, cell - 1);
                        } else {
                            if (wave) {
                                var wv = 0.5 + 0.5 * Math.sin(gx * 0.45 - t * 4 + r * 0.4);
                                ctx.fillStyle = Px.hsl(hue + 1.6 * gx + 10 * Math.sin(t * 0.21 + gx * 0.08), sat, 0.6 + 0.14 * wv - r * 0.015, alpha);
                            } else {
                                ctx.fillStyle = Qt.rgba(root.color.r, root.color.g, root.color.b, alpha);
                            }
                            var s = cell >= 3 ? cell - 1 : cell;
                            ctx.fillRect(px, py, s, s);
                        }
                    }
                }
                gx0 += dw + 1;
            }
        }
    }
}
