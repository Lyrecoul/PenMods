import QtQuick 2.15
import "messages"

Item {
    id: test
    width: 320; height: 170
    property string answer: "起始内容\n".repeat(18)
    property real oldHeight: 0
    property real oldY: 0
    property real stoppedY: 0
    property bool faded: false
    property bool grew: false
    property bool scrolled: false
    property int stage: 0
    property var bubble: null

    ListModel { id: rows; ListElement { messageId: "animation-replay" } }
    ListView {
        id: list
        width: 320; height: 170
        model: rows
        clip: true
        function probeViewportIndex() { return 0; }
        onContentHeightChanged: controller.schedule()
        delegate: MessageDelegate {
            messageId: model.messageId
            sessionId: "animation-replay"
            rawText: test.answer
            listWidth: 320
            listContentY: list.contentY
            listViewportHeight: list.height
            listScrolledUp: controller.mode !== "following"
            onRichContentCommitStarted: controller.beforeLayout()
            Component.onCompleted: test.bubble = this
        }
    }
    ChatScrollController { id: controller; view: list; model: rows }
    StreamingText { id: fade; visible: false; width: 276; rawText: "已有文字" }

    Timer {
        interval: 350; running: true
        onTriggered: {
            controller.follow();
            begin.restart();
        }
    }
    Timer {
        id: begin; interval: 250
        onTriggered: {
            test.oldHeight = test.bubble.height;
            test.oldY = list.contentY;
            fade.rawText += "新增文字";
            test.answer += "流式新增一行\n".repeat(5);
            sample.start();
        }
    }
    Timer {
        id: sample; interval: 16; repeat: true
        onTriggered: {
            ++test.stage;
            // qmlscene can be occluded by the host app; flush the Column's
            // deferred polish so the same geometry is tested without hiding it.
            test.bubble.contentBubble.parent.parent.forceLayout();
            var alpha = fade.children[0].opacity;
            test.faded = test.faded || (alpha > 0.15 && alpha < 0.99);
            test.grew = test.grew || (test.bubble.height > test.oldHeight + 0.5
                         && test.bubble.height < test.bubble.contentBubble.implicitHeight + 27);
            var bottom = Math.max(list.originY, list.originY + list.contentHeight - list.height);
            test.scrolled = test.scrolled || (list.contentY > test.oldY + 0.5 && list.contentY < bottom - 0.5);
            if (test.stage === 24) {
                controller.gestureStarted();
                test.stoppedY = list.contentY;
                test.answer += "手动接管后新增\n".repeat(3);
            }
            if (test.stage === 45) {
                var held = Math.abs(list.contentY - test.stoppedY) < 1 && controller.mode === "reading";
                console.log("ANIMATION " + (test.faded && test.grew && test.scrolled && held ? "PASS" : "FAIL")
                            + " fade=" + test.faded + " grow=" + test.grew + " scroll=" + test.scrolled + " held=" + held);
                Qt.quit();
            }
        }
    }
}
