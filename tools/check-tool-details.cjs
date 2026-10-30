// Run with: node tools/check-tool-details.cjs
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const context = {};
vm.createContext(context);
vm.runInContext(fs.readFileSync(path.join(__dirname,
    '../resource/models/YDP02X/qml/assistant/messages/ToolDetails.js'), 'utf8')
    .replace(/^\.pragma library\s*/, ''), context);

const calls = context.entries(JSON.stringify([
    {name: 'shell_exec', state: 'done', arguments: '{"command":"echo 中文"}', result: 'Exit code: 0\nstdout:\n中文\n'},
    {name: 'web_search', state: 'error', arguments: '{"query":"示例"}', result: '连接失败'}
]), '', '', '');
assert.equal(calls.length, 2);
assert.equal(calls[0].name, 'shell_exec');
assert.equal(calls[1].state, 'error');
const shell = context.rows('Exit code: 7\nstdout:\nhello\nstderr:\nfailed\n', true);
assert.equal(shell.length, 3);
assert.equal(shell[0].value, '7');
assert.equal(shell[1].value, 'hello');
assert.equal(shell[2].value, 'failed');
const nested = context.rows('{"options":{"sites":["a","b"]},"empty":null}', false);
assert(nested.some(row => row.label === '[1]' && row.value === 'b' && row.depth === 3));
assert(nested.some(row => row.label === 'empty' && row.value === 'null'));
const search = context.rows('网络搜索结果（查询: "test"）：\n\n1. 第一项\n   https://example.com/a\n   摘要\n\n2. 第二项\n   https://example.com/b\n   更多内容\n', false);
assert.equal(search.filter(row => row.group).length, 2);
assert(search.some(row => row.label === '来源' && row.value === 'https://example.com/b' && row.depth === 1));
for (const text of ['{incomplete', '<b>untrusted</b>', '旧版结果'])
    assert.equal(context.rows(text, false)[0].value, text);
assert.equal(context.entries('invalid', 'legacy', 'error', 'old')[0].result, 'old');
const large = JSON.stringify(Array.from({length: 250}, (_, i) => i));
assert.equal(JSON.parse(context.rows(large, false)[0].value).length, 250);
console.log('Tool detail fixtures passed');
