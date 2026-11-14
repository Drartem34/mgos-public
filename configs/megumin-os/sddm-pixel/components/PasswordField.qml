import QtQuick
import "Px.js" as Px

// Поле пароля: піксельна рамка, символи — пікселі-квадратики, курсор — піксельний блок.
FocusScope {
    id: root
    property alias text: input.text
    property alias input: input
    property color accent: "#9929CF"
    property string placeholder: "Пароль"
    signal accepted()

    implicitWidth: 300
    implicitHeight: 48

    PixelPanel {
        anchors.fill: parent
        accent: root.accent
        cell: 2
        round: 2
        alpha: 0.45
        focused: input.activeFocus
    }

    TextInput {
        id: input
        anchors.fill: parent
        anchors.leftMargin: 18
        anchors.rightMargin: 18
        verticalAlignment: TextInput.AlignVCenter
        echoMode: TextInput.Password
        passwordMaskDelay: 0
        color: "transparent"
        selectedTextColor: "transparent"
        selectionColor: "transparent"
        cursorDelegate: Item {}
        font.pixelSize: 16
        focus: true
        clip: true
        onActiveFocusChanged: if (activeFocus) Qt.inputMethod.hide()
        onAccepted: root.accepted()
    }

    readonly property int maxDots: Math.max(1, Math.floor((width - 48) / 15))
    Row {
        id: dots
        x: 18
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6
        Repeater {
            model: Math.min(input.text.length, root.maxDots)
            Rectangle {
                width: 9; height: 9
                color: "#f5f5f7"
                antialiasing: false
                scale: 0
                Component.onCompleted: scale = 1
                Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutBack } }
            }
        }
    }

    Rectangle {   // піксельний курсор
        x: 18 + dots.width + (input.text.length ? 7 : 0)
        anchors.verticalCenter: parent.verticalCenter
        width: 3; height: 18
        antialiasing: false
        color: "#f5f5f7"
        visible: input.activeFocus
        SequentialAnimation on opacity {
            running: input.activeFocus
            loops: Animation.Infinite
            NumberAnimation { to: 1; duration: 0 }
            PauseAnimation { duration: 530 }
            NumberAnimation { to: 0; duration: 0 }
            PauseAnimation { duration: 530 }
        }
    }

    Text {
        x: 30
        anchors.verticalCenter: parent.verticalCenter
        visible: input.text.length === 0
        text: root.placeholder
        font.family: "JetBrainsMono Nerd Font"
        font.weight: Font.Bold
        font.pixelSize: 13
        font.letterSpacing: 1
        color: Qt.rgba(1, 1, 1, 0.45)
    }
}
