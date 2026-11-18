#!/usr/bin/env python3
"""Exercise bundled chatbot QML logic with real Qt ListModels, without the vendor app.

Requires Qt Quick Test (qmltestrunner, Qt 5 or 6). Reads the committed resource
format, not the ignored editable tree, so stale resource regeneration fails too.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from unpack_qrc import extract

ROOT = Path(__file__).resolve().parents[1]
resources = {p: b.decode() for p, b, _ in extract(
    (ROOT / "resource/models/YDP02X/qrc_qml.h").read_text()
) if p.endswith(".qml")}
source = resources["qml/ChatAssistant.qml"]


def block(name):
    match = re.search(r"^( +)" + re.escape(name) + r"[^\n]*\{\n.*?^\1}", source, re.M | re.S)
    if not match:
        raise RuntimeError("Missing QML block: " + name)
    return match.group()


functions = [
    "clearAttachments", "resetCaptureMode", "_clearDraft", "_clearPendingTexts",
    "_makeReasoningEntry", "_toolRow", "_historyRows", "_upsertToolSnapshot",
    "replaceThinkingOrAppend", "_appendAnswerPlaceholder", "_findCurrentAnswerIndex",
    "_completeLastReasoningCard", "_flushReasoningBuffer", "_flushStreamBuffer",
    "finalizeLastAssistantIfNeeded", "_syncHistoryIndices",
]
# This helper did not exist before #33. The consumer scenarios below expose its
# absence through duplicate row identities, rather than asserting source spelling.
if "function _nextResponseRowId(" in source:
    functions.append("_nextResponseRowId")
logic = "\n".join(block("function " + name + "(") for name in functions)
logic += "\n    function visibilityChanged() " + block("onVisibleChanged:").split(":", 1)[1]
logic += "\n" + block("function onToolStateChanged(")
logic += "\n" + block("function onStreamChunk(")
properties = "\n".join(line for line in source.splitlines()
                       if re.match(r"    property (bool|int|string|var) ", line)
                       and "{" not in line)

menu = "\n".join(re.findall(
    r"^            (?:searchEnabled|searchConfigured|nativeSearchActive):.*$", source, re.M
))
menu += "\n" + block("onWebSearchToggled:")

tests = r'''
import QtQuick 2.15
import QtTest 1.2
Item {
    id: id_chat_assistant_page
    width: 320; height: 170
    property var chatbot: ({currentSessionId: "s", responseMessageId: "a", toolStates: [], messages: [],
                            nativeSearchActive: false, nativeSearchEnabled: true,
                            searchConfigured: false, searchEnabled: false})
    property var keyBoard: ({autoSendScan: true, autoSendScanConfig: true})
    property var cameraCapture: ({captureEnabled: true})
    property var id_pop_container: ({closeAllPages: function() {}})
    property var scrollController: ({beforeLayout: function() {}, schedule: function() {}})
    property var throttlingTimer: ({running: false, stop: function() {}, start: function() {}})
    property var generationDoneTimer: ({stop: function() {}})
    property var streamThrottle: ({content: "", lastIndex: -1, targetKind: "", reasoning: "", reasoningIndex: -1})
    ListModel { id: chatModel; dynamicRoles: true }
    ListModel { id: attachedFilesModel; dynamicRoles: true }
    ListModel { id: attachedMediaModel; dynamicRoles: true }
    function _scheduleScrollToBottom() {}
    function _beginRichContentCommit() {}
    property var toastBanner: ({warning: function(text) { throw new Error(text); }})
    function _disarmDraftRollback(reason) {}
    function _flushIdleWork() {}
    __PROPERTIES__
    __LOGIC__
    MoreMenuPopup { id: id_more_menu; __MENU__ }
    TestCase {
        name: "ChatbotQmlRegression"
        when: windowShown
        function state(status) {
            return {sessionId: "s", batchId: "a", key: "call", callId: "call",
                    state: status, text: "搜索", server: true, answerOffset: 2, reasoningOffset: 1};
        }
        function init() {
            chatModel.clear(); attachedFilesModel.clear(); attachedMediaModel.clear();
            chatbot = {currentSessionId: "s", responseMessageId: "a", toolStates: [], messages: [],
                       nativeSearchActive: false, nativeSearchEnabled: true,
                       searchConfigured: false, searchEnabled: false};
            streamThrottle = {content: "", lastIndex: -1, targetKind: "", reasoning: "", reasoningIndex: -1};
            _activeReasoningIndex = -1;
            id_more_menu.hide();
        }
        function test_draftSurvivesLeavingCachedPage() {
            draftText = "未发送的文字"; draftVisible = true;
            _draftSessionId = "s"; draftEditingMessageIndex = 3;
            attachedFilesModel.append({path: "/tmp/notes.txt"});
            attachedMediaModel.append({url: "data:image/png;base64,example"});
            _draftRollbackArmed = true; _draftRollbackSnapshot = {text: "重试"};
            var request = _draftRequestId, read = _attachmentReadSeq;
            id_chat_assistant_page.visible = false; visibilityChanged();
            compare(draftText, "未发送的文字"); compare(draftVisible, true);
            compare(attachedFilesModel.get(0).path, "/tmp/notes.txt");
            compare(attachedMediaModel.get(0).url, "data:image/png;base64,example");
            compare(draftEditingMessageIndex, 3); compare(_draftSessionId, "s");
            compare(_draftRollbackArmed, true); compare(_draftRollbackSnapshot.text, "重试");
            verify(_draftRequestId > request); verify(_attachmentReadSeq > read);
            compare(cameraCapture.captureEnabled, false);
            id_chat_assistant_page.visible = true; visibilityChanged();
            compare(draftText, "未发送的文字");
            _clearDraft("prepare-session-change");
            compare(draftText, ""); compare(attachedMediaModel.count, 0);
            compare(attachedFilesModel.count, 0);
        }
        function test_nativeSearchToggle() {
            chatbot = {nativeSearchActive: true, nativeSearchEnabled: true,
                       searchConfigured: false, searchEnabled: false};
            id_more_menu.show();
            tryCompare(id_more_menu, "opacity", 1);
            compare(id_more_menu.searchEnabled, true);
            compare(id_more_menu.searchConfigured, true);
            verify(findText(id_more_menu, "厂商搜索"));
            mouseClick(id_chat_assistant_page, 100, 50);
            compare(chatbot.nativeSearchEnabled, false);
            compare(chatbot.searchEnabled, false);
        }
        function test_externalSearchToggle() {
            chatbot = {nativeSearchActive: false, nativeSearchEnabled: true,
                       searchConfigured: true, searchEnabled: false};
            id_more_menu.show();
            tryCompare(id_more_menu, "opacity", 1);
            verify(findText(id_more_menu, "网络搜索"));
            mouseClick(id_chat_assistant_page, 100, 50);
            compare(chatbot.searchEnabled, true);
            compare(chatbot.nativeSearchEnabled, true);
        }
        function findText(item, text) {
            if (item.text === text) return true;
            for (var i = 0; i < item.children.length; ++i)
                if (findText(item.children[i], text)) return true;
            return false;
        }
        function test_historyKeepsToolBetweenAnswerSegments() {
            chatbot.toolStates = [state("done")];
            var rows = _historyRows([{role: "assistant", messageId: "a", content: "前言答案", reasoning: "思考"}]);
            compare(rows.map(function(r) { return r.messageId; }),
                    ["a:reasoning", "a:answer", "a:tools", "a:reasoning:1", "a:answer:2"]);
            compare(rows[1].raw_text, "前言"); compare(rows[4].raw_text, "答案");
        }
        function test_legacyNativeSearchPrecedesAnswer() {
            var old = state("done"); delete old.answerOffset; delete old.reasoningOffset;
            chatbot.toolStates = [old];
            var rows = _historyRows([{role: "assistant", messageId: "a", content: "答案"}]);
            compare(rows[0].isToolCall, true); compare(rows[1].raw_text, "答案");
            old.server = false;
            rows = _historyRows([{role: "assistant", messageId: "a", content: "前言"}]);
            compare(rows[0].raw_text, "前言"); compare(rows[1].isToolCall, true);
        }
        function test_toolProgressDoesNotSplitStreamingAnswer() {
            _appendAnswerPlaceholder();
            onStreamChunk("前言");
            onToolStateChanged(state("searching"));
            onStreamChunk("答");
            onToolStateChanged(state("done"));
            onStreamChunk("案");
            _flushStreamBuffer();
            compare(chatModel.count, 3);
            compare(chatModel.get(0).raw_text, "前言");
            compare(chatModel.get(1).isToolCall, true);
            compare(chatModel.get(2).raw_text, "答案");
            compare(chatModel.get(2).messageId, "a:answer:2");
            chatbot.messages = [{messageId: "a", role: "assistant", content: "前言答案"}];
            _syncHistoryIndices(); compare(chatModel.get(2).historyIndex, 0);
        }
    }
}
'''.replace("__PROPERTIES__", properties).replace("__LOGIC__", logic).replace("__MENU__", menu)
runner = shutil.which("qmltestrunner") or next((str(p) for p in (
    Path("/usr/lib/qt6/bin/qmltestrunner"), Path("/usr/lib/qt5/bin/qmltestrunner")
) if p.is_file()), None)
if not runner:
    raise SystemExit("qmltestrunner not found (install Qt Quick Test)")
with tempfile.TemporaryDirectory(prefix="penmods-chat-qml-") as directory:
    (Path(directory) / "MoreMenuPopup.qml").write_text(resources["qml/assistant/dialogs/MoreMenuPopup.qml"])
    path = Path(directory) / "tst_chatbot.qml"
    path.write_text(tests)
    subprocess.run([runner, "-input", str(path)], check=True,
                   env={**os.environ, "QT_QPA_PLATFORM": "offscreen", "QT_QUICK_BACKEND": "software"})
