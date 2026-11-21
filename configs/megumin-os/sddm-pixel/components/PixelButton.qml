import QtQuick
import "Px.js" as Px

// Піксельна кнопка як у mgOS (годинник-поповер, інсталятор): клітинки 3 px із зазором,
// сходинкові кути, дизер Байєра, при наведенні — відблиск, що пробігає по пікселях.
FocusScope {
    id: root
    property string text: ""
    property string icon: ""          // гліф Nerd Font
    property bool primary: false
    property color accent: "#9929CF"
    property int fontSize: 13
    property int padH: 18
    property int padV: 11
    property int cell: 3
    property int round: 1
    property int gap: 1
    property bool square: false
    signal clicked()

    readonly property bool hovered: ma.containsMouse
    readonly property bool pressed: ma.pressed
    property real sweep: -12

    implicitWidth: square ? implicitHeight : row.implicitWidth + padH * 2
    implicitHeight: row.implicitHeight + padV * 2
    activeFocusOnTab: true

    Canvas {
        id: cv
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: Px.drawPanel(getContext("2d"), width, height, {
            kind: root.primary ? 1 : 0, cell: root.cell, gap: root.gap, round: root.round, accent: root.accent,
            hover: root.hovered || root.activeFocus, press: root.pressed, sweep: root.sweep })
    }

    Row {
        id: row
        anchors.centerIn: parent
        anchors.verticalCenterOffset: root.pressed ? 1 : 0
        spacing: root.icon !== "" && root.text !== "" ? 9 : 0
        Text {
            visible: root.icon !== ""
            text: root.icon
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: root.fontSize + 4
            color: "#f5f5f7"
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            visible: root.text !== ""
            text: root.text.toUpperCase()
            font.family: "JetBrainsMono Nerd Font"
            font.weight: Font.ExtraBold
            font.pixelSize: root.fontSize
            font.letterSpacing: 1
            color: "#f5f5f7"
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
    Keys.onReturnPressed: root.clicked()
    Keys.onEnterPressed: root.clicked()
    Keys.onSpacePressed: root.clicked()

    NumberAnimation on sweep {
        running: root.hovered || root.activeFocus
        from: -12; to: root.width / root.cell + 24
        duration: Math.max(500, (root.width / root.cell + 36) * 1000 / 28)
        loops: Animation.Infinite
    }
    onSweepChanged: cv.requestPaint()
    onHoveredChanged: cv.requestPaint()
    onPressedChanged: cv.requestPaint()
    onActiveFocusChanged: cv.requestPaint()
    onAccentChanged: cv.requestPaint()
}
