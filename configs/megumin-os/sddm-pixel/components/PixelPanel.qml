import QtQuick
import "Px.js" as Px

// Темна піксельна картка / рамка (сходинкові кути, дизер). focus — рамка в кольорі акценту.
Canvas {
    id: root
    property color accent: "#9929CF"
    property int cell: 4
    property int round: 2
    property real alpha: 0.42
    property bool focused: false
    renderStrategy: Canvas.Cooperative
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onFocusedChanged: requestPaint()
    onAccentChanged: requestPaint()
    onPaint: Px.drawPanel(getContext("2d"), width, height, {
        kind: 2, cell: cell, gap: 0, round: round, alpha: alpha, accent: accent, focus: focused })
}
