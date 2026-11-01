import QtQuick 2.12

Item {
    id: page
    property bool zoomMode: false
    property QtObject oldPlayer: QtObject {}
    property QtObject newPlayer: QtObject { property bool downsamplingEnabled: true }

    function bindOptional(player) {
        if ("downsamplingEnabled" in player)
            player.downsamplingEnabled = Qt.binding(function() { return !page.zoomMode; });
    }

    Timer {
        interval: 50
        running: true
        onTriggered: {
            bindOptional(oldPlayer);
            bindOptional(newPlayer);
            if (!newPlayer.downsamplingEnabled) {
                console.error("FAIL: initial sampling state");
                Qt.exit(1);
                return;
            }
            zoomMode = true;
            if (newPlayer.downsamplingEnabled) {
                console.error("FAIL: sampling state did not follow zoom");
                Qt.exit(1);
                return;
            }
            console.log("PASS: optional sampling binding");
            Qt.exit(0);
        }
    }
}
