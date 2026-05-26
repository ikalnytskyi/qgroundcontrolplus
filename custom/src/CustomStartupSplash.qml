import QtQuick

Item {
    id: root
    anchors.fill: parent
    z: 100000

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    Image {
        id: logo
        anchors.centerIn: parent
        source: "qrc:/custom/img/dronecode-white.svg"
        fillMode: Image.PreserveAspectFit
        sourceSize.width: Math.round(width)
        sourceSize.height: Math.round(height)
        width: Math.min(parent.width * 0.48, 680)
        height: Math.min(parent.height * 0.14, 160)
    }

    Timer {
        interval: 700
        running: true
        repeat: false
        onTriggered: fadeOut.start()
    }

    SequentialAnimation {
        id: fadeOut
        running: false

        PauseAnimation { duration: 150 }
        NumberAnimation {
            target: root
            property: "opacity"
            from: 1
            to: 0
            duration: 220
        }
        ScriptAction { script: root.destroy() }
    }
}
