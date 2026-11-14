import QtQuick
import QtQuick.VirtualKeyboard

// Перехоплює qtvirtualkeyboard (щоб Qt не відкривав DesktopInputPanel на пів екрана) і тримає її вимкненою
InputPanel {
    id: panel
    property bool activated: false
    active: false
    visible: false
    enabled: false
    width: 0
    height: 0
    y: 100000
    opacity: 0
    Component.onCompleted: Qt.inputMethod.hide()
    Connections {
        target: Qt.inputMethod
        function onVisibleChanged() {
            if (Qt.inputMethod.visible) Qt.inputMethod.hide()
        }
    }
}
