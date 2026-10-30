import QtQuick 2.15
import "messages"

// Run through run.py on a YDP02X. Exercises the actual streaming component.
Item {
    width: 320
    height: 170
    property int tick: 0
    property double last: Date.now()
    property double worst: 0
    property double total: 0

    MixedContentBubble { id: bubble; maxWidth: 276; isComplete: false }
    Timer {
        interval: 60
        running: true
        repeat: true
        onTriggered: {
            var now = Date.now();
            if (tick > 0) {
                worst = Math.max(worst, now - last);
                total += now - last;
            }
            last = now;
            bubble.rawText += "这是一段流式输出，用于测量长回答重新排版时事件循环的延迟。Long streaming answer with wrapping.\n".repeat(6);
            if (++tick === 30) {
                console.log("PERF chars=" + bubble.rawText.length + " worst_ms=" + worst + " mean_ms=" + total / 29);
                Qt.quit();
            }
        }
    }
}
