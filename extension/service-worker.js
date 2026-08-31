const HOST = "com.leafreader.piper";
let port = null;
let sequence = 0;
const pending = new Map();
let readingTabId = null;

function updatePageCursor(word) {
  if (readingTabId === null) return;
  chrome.scripting.executeScript({
    target: { tabId: readingTabId },
    func: (index) => globalThis.__leafExtensionSetWord?.(index),
    args: [word]
  }).catch(() => {});
}

function clearPageCursor() {
  if (readingTabId === null) return;
  chrome.scripting.executeScript({
    target: { tabId: readingTabId },
    func: () => CSS.highlights?.delete("leaf-extension-reading-word")
  }).catch(() => {});
}

function connectHost() {
  if (port) return port;
  port = chrome.runtime.connectNative(HOST);
  port.onMessage.addListener((message) => {
    if (message.event === "progress") updatePageCursor(message.word);
    if (message.event === "error") clearPageCursor();
    const callback = pending.get(message.id);
    if (callback) {
      pending.delete(message.id);
      callback(message);
    }
    chrome.runtime.sendMessage({ type: "native-event", payload: message }).catch(() => {});
  });
  port.onDisconnect.addListener(() => {
    const error = chrome.runtime.lastError?.message || "Native Piper host disconnected";
    for (const callback of pending.values()) callback({ ok: false, error });
    pending.clear();
    port = null;
  });
  return port;
}

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.target !== "native") return false;
  if (message.payload?.command === "speak" && Number.isInteger(message.payload.tabId)) readingTabId = message.payload.tabId;
  if (message.payload?.command === "stop") clearPageCursor();
  const id = ++sequence;
  pending.set(id, sendResponse);
  try {
    connectHost().postMessage({ ...message.payload, id });
  } catch (error) {
    pending.delete(id);
    sendResponse({ ok: false, error: error.message });
  }
  return true;
});
