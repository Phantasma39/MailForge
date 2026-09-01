'use strict';

// ================= 页面元素 =================
const $ = (id) => document.getElementById(id);
const terminal   = $('terminal');
const cmdInput   = $('cmdInput');
const btnSend    = $('btnSend');
const btnConnect = $('btnConnect');
const btnDisconnect = $('btnDisconnect');
const btnDemo    = $('btnDemo');
const btnClear   = $('btnClear');
const connBadge  = $('connBadge');
const dataBox    = $('dataBox');
const dataBody   = $('dataBody');
const btnSendData = $('btnSendData');

// ================= 状态 =================
let sessionId = null;   // 桥接服务分配的会话 id
let connected = false;  // 是否已连接 SMTP 服务器
let dataMode  = false;  // 服务器是否处于 DATA 模式
let busy      = false;  // 正在等待服务器响应

// ================= 终端输出 =================
function addLine(cls, text) {
  const pre = document.createElement('pre');
  pre.className = 'line ' + cls;
  pre.textContent = text;
  terminal.appendChild(pre);
  terminal.scrollTop = terminal.scrollHeight;
}

function setConnected(on) {
  connected = on;
  connBadge.textContent = on ? '● 已连接' : '○ 未连接';
  connBadge.className = 'badge ' + (on ? 'ok' : 'off');
  btnConnect.disabled = on;
  btnDisconnect.disabled = !on;
  refreshInput();
}

function refreshInput() {
  cmdInput.disabled = !connected || dataMode || busy;
  btnSend.disabled  = !connected || dataMode || busy;
  dataBox.classList.toggle('hidden', !dataMode);
  if (dataMode) dataBody.focus();
  else if (connected && !busy) cmdInput.focus();
}

// ================= HTTP 请求封装 =================
async function post(path, session, bodyText) {
  let url = '/api/' + path;
  if (session) url += '?session=' + encodeURIComponent(session);
  const resp = await fetch(url, { method: 'POST', body: bodyText || '' });
  const text = await resp.text();
  if (!resp.ok) throw new Error(text);
  const sid = resp.headers.get('X-Session');
  if (sid) sessionId = sid;
  return text;
}

// ================= 连接 / 断开 =================
async function connect() {
  if (busy) return;
  busy = true;
  refreshInput();
  addLine('sys', '── 正在连接 SMTP 服务器 127.0.0.1:2525 ──');
  try {
    const greeting = await post('connect');
    addLine('srv', greeting.trim());
    setConnected(true);
    addLine('sys', '── 连接成功！请开始 SMTP 对话 ──');
  } catch (e) {
    addLine('err', '连接失败：' + e.message);
    setConnected(false);
  }
  busy = false;
  refreshInput();
}

async function disconnect() {
  if (busy) return;
  busy = true;
  refreshInput();
  try {
    if (sessionId) {
      const reply = await post('disconnect', sessionId);
      addLine('srv', reply.trim());
    }
  } catch (e) {
    addLine('err', '断开出错：' + e.message);
  }
  sessionId = null;
  dataMode = false;
  busy = false;
  setConnected(false);
  addLine('sys', '── 已断开 ──');
}

// ================= 发送命令 / DATA 正文 =================
async function sendCommand(rawCmd) {
  if (!connected || !sessionId || busy) return;
  const cmd = rawCmd.trim();
  if (!cmd) return;

  if (dataMode) return sendDataBody(cmd);   // DATA 模式下输入一律当正文

  busy = true;
  addLine('cli', '> ' + cmd);
  refreshInput();
  try {
    const reply = await post('command', sessionId, cmd);
    addLine('srv', reply.trim());
    if (reply.startsWith('354')) {
      dataMode = true;
      addLine('sys', '── 已进入 DATA 模式：请在下方输入邮件正文，发送时自动补 . ──');
    }
  } catch (e) {
    addLine('err', '发送出错：' + e.message);
    if (/会话不存在|会话已结束|断开/.test(e.message)) {
      sessionId = null;
      dataMode = false;
      setConnected(false);
    }
  }
  busy = false;
  refreshInput();
}

async function sendDataBody(explicitText) {
  if (!connected || !sessionId || busy) return;
  const text = explicitText !== undefined ? explicitText : dataBody.value;
  const lines = text.split('\n');
  for (const l of lines) addLine('cli', '> ' + l);
  if (lines.length === 0 || lines[lines.length - 1] !== '.') addLine('cli', '> .');

  busy = true;
  refreshInput();
  try {
    const reply = await post('command', sessionId, text);
    addLine('srv', reply.trim());
  } catch (e) {
    addLine('err', '发送出错：' + e.message);
    if (/会话不存在|会话已结束|断开/.test(e.message)) {
      sessionId = null;
      setConnected(false);
    }
  }
  dataMode = false;
  busy = false;
  dataBody.value = '';
  addLine('sys', '── DATA 结束 ──');
  refreshInput();
}

// ================= 自动演示 =================
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function runDemo() {
  if (busy) return;
  if (!connected) await connect();
  if (!connected) return;

  for (const s of [
    'EHLO localhost',
    'MAIL FROM:<alice@example.com>',
    'RCPT TO:<bob@example.com>',
    'DATA',
  ]) {
    await sleep(600);
    await sendCommand(s);
  }

  await sleep(600);
  await sendDataBody(
    'Subject: Hello from MailForge\n\n' +
    '这是一封由「浏览器 SMTP 客户端」发送的测试邮件！\n' +
    '正文通过 DATA 命令逐行传给服务器。\n' +
    '祝课程设计顺利～'
  );

  await sleep(600);
  await sendCommand('QUIT');
  await sleep(400);
  await disconnect();
}

// ================= 事件绑定 =================
btnConnect.addEventListener('click', () => connect());
btnDisconnect.addEventListener('click', () => disconnect());
btnSend.addEventListener('click', () => { sendCommand(cmdInput.value); cmdInput.value = ''; });
btnSendData.addEventListener('click', () => sendDataBody());
btnDemo.addEventListener('click', runDemo);
btnClear.addEventListener('click', () => { terminal.innerHTML = ''; });

cmdInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    sendCommand(cmdInput.value);
    cmdInput.value = '';
  }
});

dataBody.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
    e.preventDefault();
    sendDataBody();
  }
});

document.querySelectorAll('.chip').forEach((chip) => {
  chip.addEventListener('click', () => {
    if (dataMode || busy) return;
    cmdInput.value = chip.dataset.cmd;
    cmdInput.focus();
  });
});

// 打开页面自动连接
window.addEventListener('load', () => {
  addLine('sys', '欢迎使用 MailForge SMTP 客户端终端');
  addLine('sys', '正在自动连接本地 SMTP 服务器 (127.0.0.1:2525) ...');
  connect();
});
