import QtQuick 2.15

// Basic QtQuick button for devices without a usable QtQuick.Controls plugin.
Item {
    id: button
    property Item contentItem: null
    property Item background: null
    property bool checked: false
    readonly property bool pressed: pointer.pressed
    readonly property bool down: pressed
    readonly property bool hovered: pointer.containsMouse
    signal clicked()

    onContentItemChanged: {
        if (contentItem) {
            contentItem.parent = button
            contentItem.anchors.fill = button
        }
    }
    onBackgroundChanged: {
        if (background) {
            background.parent = button
            background.anchors.fill = button
            background.z = -1
        }
    }

    Accessible.role: Accessible.Button
    Accessible.onPressAction: if (enabled) clicked()

    MouseArea {
        id: pointer
        anchors.fill: parent
        z: 1
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
