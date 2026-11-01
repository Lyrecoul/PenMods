import QtQuick 2.12
import "audiopages"

Item {
    id: test
    width: 320; height: 170
    property int phase: 0
    property QtObject externalPlayer: QtObject {
        property bool hasEmbeddedPlayer: false
        property bool fallbackResult: false
        property int calls: 0
        signal mediaChanged()
        function fallbackToMpv() { calls++; return fallbackResult; }
    }
    ExternalPlayer { id: page }
    function fallbackBarVisible() {
        for (var i = 0; i < page.children.length; ++i) {
            if (page.children[i].objectName === "fallbackBar")
                return page.children[i].visible;
        }
        return false;
    }
    Timer {
        id: check
        interval: 50
        onTriggered: {
            if (page.embedded || !fallbackBarVisible() ||
                    page.fallbackFailed !== (phase === 1) || externalPlayer.calls !== phase) {
                console.error("FAIL: phase=" + phase + " embedded=" + page.embedded + " bar=" + fallbackBarVisible() + " fallbackFailed=" + page.fallbackFailed + " calls=" + externalPlayer.calls);
                Qt.exit(1);
            } else if (phase === 1) {
                externalPlayer.fallbackResult = true;
                phase = 2;
                externalPlayer.mediaChanged();
                check.start();
            } else {
                console.log("PASS: repeated failed loads expose return control and correct mpv result");
                Qt.exit(0);
            }
        }
    }
    Component.onCompleted: {
        externalPlayer.hasEmbeddedPlayer = true;
        phase = 1;
        externalPlayer.mediaChanged();
        check.start();
    }
}
