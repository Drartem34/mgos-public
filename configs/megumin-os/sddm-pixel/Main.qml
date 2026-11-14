// ============================================================================
//  mgOS — екран входу SDDM, мінімалістичний, у стилі macOS.
//  • Інтро: перший кадр = останній кадр plymouth (чорний, лого mgOS по центру,
//    спінер нижче). Лого тане, шпалери проступають із розмиття — плавний перехід.
//  • Зверху дата й великий годинник, знизу аватарка, ім'я і поле пароля-«пігулка».
//  • Фон — ті самі шпалери, що й на робочому столі: після входу UI тане без «стрибка».
//  Налаштування: «Налаштування» mgOS → Екран входу (пишуть theme.conf.user).
// ============================================================================
import QtQuick
import QtQuick.Effects
import "components"

Rectangle {
    id: root
    width: 1920
    height: 1080
    color: "black"

    // ---------- конфіг ----------
    function cfg(key, def) {
        var v = config[key];
        return (v === undefined || v === null || v === "") ? def : v;
    }
    function cfgBool(key, def) {
        var v = cfg(key, def);
        return v === true || v === "true" || v === "1" || v === 1;
    }
    function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
    readonly property string bgMode: cfg("BackgroundMode", "wallpaper")      // wallpaper | pixel | waves
    readonly property int pixelSize: Math.max(2, parseInt(cfg("PixelSize", 6)))
    readonly property real dim: clamp(parseInt(cfg("Dim", 25)) / 100, 0, 0.9)
    readonly property bool blurOn: cfgBool("Blur", false)
    readonly property color accent: cfg("Accent", "#9929CF")
    readonly property bool clock24: cfgBool("Clock24", true)
    readonly property bool showSeconds: cfgBool("ShowSeconds", false)
    readonly property bool isPrimary: typeof primaryScreen === "undefined" ? true : primaryScreen
    readonly property bool preview: typeof mgosPreview !== "undefined" && mgosPreview

    readonly property real k: Math.max(0.7, Math.min(width / 1920, height / 1080))
    readonly property string sf: "SF Pro Display"
    readonly property string icons: "JetBrainsMono Nerd Font"
    readonly property color fg: "#ffffff"

    // ---------- користувачі й сесії ----------
    property int userIndex: userModel.lastIndex >= 0 ? userModel.lastIndex : 0
    property int sessionIndex: sessionModel.lastIndex >= 0 ? sessionModel.lastIndex : 0
    Item {
        visible: false
        Repeater {
            id: users
            model: userModel
            delegate: Item {
                property string uName: model.name
                property string uReal: model.realName
                property string uIcon: model.icon
            }
        }
        Repeater {
            id: sessions
            model: sessionModel
            delegate: Item { property string sName: model.name }
        }
    }
    readonly property var curUser: users.count > 0 ? users.itemAt(Math.min(userIndex, users.count - 1)) : null
    readonly property string userName: curUser ? curUser.uName : userModel.lastUser
    readonly property string userReal: curUser && curUser.uReal !== "" ? curUser.uReal : userName
    readonly property string userIcon: curUser ? curUser.uIcon : ""
    readonly property string sessionName: sessions.count > 0 && sessions.itemAt(sessionIndex) ? sessions.itemAt(sessionIndex).sName : ""

    // ---------- годинник ----------
    property date now: new Date()
    Timer {
        interval: 1000; repeat: true; running: true; triggeredOnStart: true
        onTriggered: root.now = new Date()
    }
    readonly property var wdays: ["Неділя", "Понеділок", "Вівторок", "Середа", "Четвер", "Пʼятниця", "Субота"]
    readonly property var months: ["січня", "лютого", "березня", "квітня", "травня", "червня",
                                   "липня", "серпня", "вересня", "жовтня", "листопада", "грудня"]
    function two(n) { return n < 10 ? "0" + n : "" + n; }
    readonly property string hhmm: {
        var h = now.getHours();
        if (!clock24) { h = h % 12; if (h === 0) h = 12; }
        return (clock24 ? two(h) : "" + h) + ":" + two(now.getMinutes());
    }

    // ---------- фон ----------
    // typing: під час введення фон трохи розмивається (як на macOS)
    readonly property bool typing: pwInput.text.length > 0
    property real bgBlur: (blurOn ? 0.75 : 0) + (typing ? 0.35 : 0)
    Behavior on bgBlur { NumberAnimation { duration: 450; easing.type: Easing.OutCubic } }
    property real introBlur: 0          // додаткове розмиття під час інтро
    property real bgScale: 1

    Item {
        id: bg
        anchors.fill: parent
        scale: root.bgScale
        layer.enabled: !root.preview || root.bgBlur > 0.01
        layer.effect: MultiEffect {
            blurEnabled: true
            blur: Math.min(1, root.bgBlur + root.introBlur)
            blurMax: 56
            autoPaddingEnabled: false
        }

        Image {   // шпалери (або піксельні — зменшені й розтягнуті без згладжування)
            id: wall
            anchors.fill: parent
            visible: root.bgMode !== "waves" && wall.status === Image.Ready
            source: Qt.resolvedUrl(root.cfg("Background", "Backgrounds/wallpaper.jpg"))
            fillMode: Image.PreserveAspectCrop
            asynchronous: false
            cache: false
            smooth: root.bgMode !== "pixel"
            sourceSize.width: root.bgMode === "pixel" ? Math.ceil(root.width / root.pixelSize) : root.width
            sourceSize.height: root.bgMode === "pixel" ? Math.ceil(root.height / root.pixelSize) : root.height
        }
        Waves {
            anchors.fill: parent
            visible: root.bgMode === "waves" || wall.status === Image.Error
            running: visible
            accent: root.accent
            cell: Math.max(10, root.pixelSize * 2)
        }
    }
    // затемнення: рівне + м'який градієнт зверху і знизу, щоб білий текст читався на будь-яких шпалерах
    Rectangle { anchors.fill: parent; color: "black"; opacity: root.dim }
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.28) }
            GradientStop { position: 0.32; color: Qt.rgba(0, 0, 0, 0) }
            GradientStop { position: 0.62; color: Qt.rgba(0, 0, 0, 0) }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.32) }
        }
    }

    // ---------- уміст ----------
    Item {
        id: ui
        anchors.fill: parent
        visible: root.isPrimary
        opacity: 0

        // ── дата й годинник ──
        Column {
            id: clockCol
            anchors.horizontalCenter: parent.horizontalCenter
            y: root.height * 0.085 + shift
            property real shift: -18
            spacing: -6 * root.k
            layer.enabled: true
            layer.effect: MultiEffect { shadowEnabled: true; shadowBlur: 0.8; shadowOpacity: 0.25; shadowColor: "black"; shadowVerticalOffset: 1; shadowHorizontalOffset: 0 }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.wdays[root.now.getDay()] + ", " + root.now.getDate() + " " + root.months[root.now.getMonth()]
                font.family: root.sf
                font.weight: Font.DemiBold
                font.pixelSize: Math.round(25 * root.k)
                color: root.fg
                opacity: 0.85
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 6 * root.k
                Text {
                    id: bigClock
                    text: root.hhmm
                    font.family: root.sf
                    font.weight: Font.Bold
                    font.pixelSize: Math.round(124 * root.k)
                    font.letterSpacing: -2
                    color: root.fg
                    opacity: 0.92
                }
                Text {
                    anchors.baseline: bigClock.baseline
                    visible: root.showSeconds || !root.clock24
                    text: (root.showSeconds ? root.two(root.now.getSeconds()) : "") +
                          (!root.clock24 ? (root.showSeconds ? " " : "") + (root.now.getHours() < 12 ? "AM" : "PM") : "")
                    font.family: root.sf
                    font.weight: Font.DemiBold
                    font.pixelSize: Math.round(30 * root.k)
                    color: root.fg
                    opacity: 0.6
                }
            }
        }

        // ── вхід: аватарка, ім'я, поле ──
        Column {
            id: login
            anchors.horizontalCenter: parent.horizontalCenter
            y: root.height - height - 150 * root.k + rise
            property real rise: 24
            property real shake: 0
            transform: Translate { x: login.shake }
            spacing: 12 * root.k

            Item {   // кругла аватарка
                id: avatarBox
                width: Math.round(84 * root.k)
                height: width
                anchors.horizontalCenter: parent.horizontalCenter
                layer.enabled: true
                layer.effect: MultiEffect { shadowEnabled: true; shadowBlur: 0.6; shadowOpacity: 0.35; shadowColor: "black"; shadowVerticalOffset: 2; shadowHorizontalOffset: 0 }
                Rectangle { anchors.fill: parent; radius: width / 2; color: Qt.rgba(1, 1, 1, 0.22); antialiasing: true }
                Image {
                    id: avatarImg
                    anchors.fill: parent
                    source: root.userIcon === "" ? "" : (root.userIcon.indexOf("file:") === 0 ? root.userIcon : "file://" + root.userIcon)
                    fillMode: Image.PreserveAspectCrop
                    smooth: true
                    sourceSize.width: width * 2
                    sourceSize.height: height * 2
                    visible: false
                }
                Rectangle {
                    id: avatarMask
                    anchors.fill: parent
                    radius: width / 2
                    color: "white"
                    antialiasing: true
                    visible: false
                    layer.enabled: true
                    layer.smooth: true
                }
                MultiEffect {
                    anchors.fill: parent
                    source: avatarImg
                    maskEnabled: true
                    maskSource: avatarMask
                    maskThresholdMin: 0.5
                    maskSpreadAtMin: 1.0
                    visible: avatarImg.status === Image.Ready
                }
                Text {
                    anchors.centerIn: parent
                    visible: avatarImg.status !== Image.Ready
                    text: root.userReal.length ? root.userReal[0].toUpperCase() : "?"
                    font.family: root.sf
                    font.weight: Font.Medium
                    font.pixelSize: 38 * root.k
                    color: root.fg
                }
            }

            Row {   // ім'я; стрілки — якщо користувачів кілька
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10 * root.k
                GlyphButton {
                    visible: users.count > 1
                    glyph: ""
                    onClicked: root.userIndex = (root.userIndex + users.count - 1) % users.count
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.userReal
                    font.family: root.sf
                    font.weight: Font.DemiBold
                    font.pixelSize: Math.round(17 * root.k)
                    color: root.fg
                    style: Text.Raised
                    styleColor: Qt.rgba(0, 0, 0, 0.15)
                }
                GlyphButton {
                    visible: users.count > 1
                    glyph: ""
                    onClicked: root.userIndex = (root.userIndex + 1) % users.count
                }
            }

            // поле пароля — напівпрозора «пігулка»; стрілка з'являється, коли є текст
            Rectangle {
                id: pill
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.round(230 * root.k)
                height: Math.round(34 * root.k)
                radius: height / 2
                color: Qt.rgba(1, 1, 1, pwInput.activeFocus ? 0.22 : 0.16)
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.22)
                antialiasing: true
                Behavior on color { ColorAnimation { duration: 200 } }

                TextInput {
                    id: pwInput
                    anchors.fill: parent
                    anchors.leftMargin: 15 * root.k
                    anchors.rightMargin: pill.height + 4
                    verticalAlignment: TextInput.AlignVCenter
                    echoMode: TextInput.Password
                    passwordCharacter: "●"
                    passwordMaskDelay: 0
                    color: root.fg
                    selectionColor: Qt.rgba(1, 1, 1, 0.35)
                    font.family: root.sf
                    font.pixelSize: Math.round(12 * root.k)
                    font.letterSpacing: 2
                    focus: true
                    clip: true
                    enabled: !busy.running
                    onActiveFocusChanged: if (activeFocus) Qt.inputMethod.hide()
                    onTextChanged: if (text.length) statusText.failed = false
                    onAccepted: root.doLogin()
                    cursorDelegate: Rectangle {
                        width: 1.5; color: root.fg
                        visible: pwInput.activeFocus
                        SequentialAnimation on opacity {
                            running: pwInput.activeFocus; loops: Animation.Infinite
                            NumberAnimation { to: 1; duration: 0 }
                            PauseAnimation { duration: 530 }
                            NumberAnimation { to: 0; duration: 0 }
                            PauseAnimation { duration: 530 }
                        }
                    }
                }
                Text {   // підказка
                    anchors.fill: pwInput
                    verticalAlignment: Text.AlignVCenter
                    visible: pwInput.text.length === 0
                    text: statusText.failed ? "Невірний пароль" : "Введіть пароль"
                    font.family: root.sf
                    font.pixelSize: Math.round(13 * root.k)
                    color: root.fg
                    opacity: 0.55
                }
                Text {   // caps lock — значок усередині поля
                    anchors.right: arrow.left
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    visible: keyboard.capsLock
                    text: ""
                    font.family: root.icons
                    font.pixelSize: Math.round(12 * root.k)
                    color: root.fg
                    opacity: 0.7
                }
                Rectangle {   // кнопка-стрілка
                    id: arrow
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: pill.height - 8
                    height: width
                    radius: width / 2
                    color: arrowMa.containsMouse ? Qt.rgba(1, 1, 1, 0.42) : Qt.rgba(1, 1, 1, 0.28)
                    opacity: pwInput.text.length > 0 || busy.running ? 1 : 0
                    scale: opacity > 0 ? 1 : 0.6
                    Behavior on opacity { NumberAnimation { duration: 180 } }
                    Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }
                    Text {
                        anchors.centerIn: parent
                        visible: !busy.running
                        text: ""
                        font.family: root.icons
                        font.pixelSize: Math.round(11 * root.k)
                        color: root.fg
                    }
                    Rectangle {   // очікування — тонке кільце, що крутиться
                        anchors.centerIn: parent
                        visible: busy.running
                        width: parent.width - 8; height: width; radius: width / 2
                        color: "transparent"
                        border.width: 2
                        border.color: Qt.rgba(1, 1, 1, 0.25)
                        Rectangle { width: 4; height: 4; radius: 2; color: root.fg; x: parent.width / 2 - 2; y: -1 }
                        RotationAnimation on rotation { running: busy.running; from: 0; to: 360; duration: 800; loops: Animation.Infinite }
                    }
                    MouseArea {
                        id: arrowMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.doLogin()
                    }
                }
            }

            Text {
                id: statusText
                anchors.horizontalCenter: parent.horizontalCenter
                property bool failed: false
                text: keyboard.capsLock ? "Caps Lock увімкнено" : (root.sessionName !== "" ? root.sessionName : " ")
                font.family: root.sf
                font.pixelSize: Math.round(12 * root.k)
                color: root.fg
                opacity: 0.6
                MouseArea {   // натиск — наступна сесія
                    anchors.fill: parent
                    anchors.margins: -6
                    enabled: sessions.count > 1
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.sessionIndex = (root.sessionIndex + 1) % Math.max(1, sessions.count)
                }
            }
        }

        // ── живлення: три круглі кнопки з підписами внизу по центру ──
        Row {
            id: power
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 34 * root.k - rise
            property real rise: 16
            spacing: 34 * root.k
            PowerButton { visible: sddm.canSuspend; glyph: ""; label: "Сон"; onClicked: sddm.suspend() }
            PowerButton { visible: sddm.canReboot; glyph: ""; label: "Перезапуск"; onClicked: sddm.reboot() }
            PowerButton { visible: sddm.canPowerOff; glyph: ""; label: "Вимкнути"; onClicked: sddm.powerOff() }
        }

        // ── праворуч угорі: розкладка ──
        Text {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 22 * root.k
            visible: keyboard.layouts.length > 1
            text: keyboard.layouts.length > 0 ? keyboard.layouts[keyboard.currentLayout].shortName.toUpperCase() : ""
            font.family: root.sf
            font.weight: Font.DemiBold
            font.pixelSize: Math.round(13 * root.k)
            color: root.fg
            opacity: 0.85
            MouseArea {
                anchors.fill: parent
                anchors.margins: -8
                cursorShape: Qt.PointingHandCursor
                onClicked: keyboard.currentLayout = (keyboard.currentLayout + 1) % keyboard.layouts.length
            }
        }
    }

    // ---------- компоненти ----------
    component GlyphButton: Text {
        id: gb
        signal clicked()
        property string glyph: ""
        text: glyph
        font.family: root.icons
        font.pixelSize: Math.round(16 * root.k)
        color: root.fg
        opacity: gbMa.containsMouse ? 1 : 0.6
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        MouseArea { id: gbMa; anchors.fill: parent; anchors.margins: -8; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: gb.clicked() }
    }
    component PowerButton: Column {
        id: pb
        signal clicked()
        property string glyph: ""
        property string label: ""
        spacing: 6 * root.k
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.round(38 * root.k); height: width; radius: width / 2
            color: pbMa.pressed ? Qt.rgba(1, 1, 1, 0.38) : (pbMa.containsMouse ? Qt.rgba(1, 1, 1, 0.28) : Qt.rgba(1, 1, 1, 0.16))
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.18)
            antialiasing: true
            Behavior on color { ColorAnimation { duration: 140 } }
            Text {
                anchors.centerIn: parent
                text: pb.glyph
                font.family: root.icons
                font.pixelSize: Math.round(15 * root.k)
                color: root.fg
            }
            MouseArea { id: pbMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: pb.clicked() }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: pb.label
            font.family: root.sf
            font.pixelSize: Math.round(11 * root.k)
            color: root.fg
            opacity: pbMa.containsMouse ? 0.95 : 0.7
        }
    }

    // ---------- старт із чорного екрана ----------
    // Plymouth закривається ДО SDDM (тримати його живим не можна — вішає X).
    // Тому тема стартує з чистого чорного кадру і плавно розчиняє його в шпалери.
    Rectangle {
        id: boot
        anchors.fill: parent
        color: "black"
        visible: opacity > 0
    }

    // ---------- анімації ----------
    SequentialAnimation {
        id: intro
        PauseAnimation { duration: 150 }
        ParallelAnimation {
            SequentialAnimation {   // шпалери проступають із темряви й розмиття, трохи «наближаючись»
                PauseAnimation { duration: 400 }
                ParallelAnimation {
                    NumberAnimation { target: boot; property: "opacity"; to: 0; duration: 1600; easing.type: Easing.InOutQuad }
                    NumberAnimation { target: root; property: "introBlur"; from: 1; to: 0; duration: 2000; easing.type: Easing.OutCubic }
                    NumberAnimation { target: root; property: "bgScale"; from: 1.07; to: 1; duration: 2300; easing.type: Easing.OutCubic }
                }
            }
            SequentialAnimation {   // далі — годинник і форма входу
                PauseAnimation { duration: 1500 }
                ParallelAnimation {
                    NumberAnimation { target: ui; property: "opacity"; from: 0; to: 1; duration: 800; easing.type: Easing.OutCubic }
                    NumberAnimation { target: clockCol; property: "shift"; from: -18; to: 0; duration: 950; easing.type: Easing.OutCubic }
                    SequentialAnimation {
                        PauseAnimation { duration: 90 }
                        NumberAnimation { target: login; property: "rise"; from: 24; to: 0; duration: 900; easing.type: Easing.OutCubic }
                    }
                    SequentialAnimation {
                        PauseAnimation { duration: 180 }
                        NumberAnimation { target: power; property: "rise"; from: 16; to: 0; duration: 800; easing.type: Easing.OutCubic }
                    }
                }
            }
        }
    }
    SequentialAnimation {
        id: shakeAnim
        NumberAnimation { target: login; property: "shake"; to: -16; duration: 55 }
        NumberAnimation { target: login; property: "shake"; to: 14; duration: 70 }
        NumberAnimation { target: login; property: "shake"; to: -10; duration: 70 }
        NumberAnimation { target: login; property: "shake"; to: 6; duration: 60 }
        NumberAnimation { target: login; property: "shake"; to: 0; duration: 60 }
    }
    // Вхід: UI тане, фон стає різким — далі mgOS показує той самий постер
    ParallelAnimation {
        id: outro
        NumberAnimation { target: ui; property: "opacity"; to: 0; duration: 380; easing.type: Easing.InCubic }
        NumberAnimation { target: root; property: "bgBlur"; to: 0; duration: 380 }
    }
    Timer { id: busy; interval: 15000 }

    Loader {
        id: vk
        active: true
        visible: false
        width: 0
        height: 0
        y: 100000
        source: "components/VKeyboard.qml"
    }

    function doLogin() {
        if (busy.running) return;
        statusText.failed = false;
        busy.start();
        sddm.login(root.userName, pwInput.text, root.sessionIndex);
    }

    Connections {
        target: sddm
        function onLoginFailed() {
            busy.stop();
            pwInput.text = "";
            statusText.failed = true;
            shakeAnim.start();
            pwInput.forceActiveFocus();
        }
        function onLoginSucceeded() {
            busy.stop();
            outro.start();
        }
    }

    Component.onCompleted: {
        if (preview) {   // прев'ю в «Налаштуваннях»: одразу кінцевий кадр
            boot.opacity = 0; ui.opacity = 1; clockCol.shift = 0; login.rise = 0; power.rise = 0;
        } else {
            intro.start();
        }
        pwInput.forceActiveFocus();
    }
}
