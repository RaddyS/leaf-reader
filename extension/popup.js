const voice = document.querySelector("#voice");
const speed = document.querySelector("#speed");
const speedValue = document.querySelector("#speed-value");
const readButton = document.querySelector("#read");
const stopButton = document.querySelector("#stop");
const status = document.querySelector("#status");

function showStatus(message, isError = false) {
  status.textContent = message;
  status.classList.toggle("error", isError);
}

async function native(command) {
  return chrome.runtime.sendMessage({ target: "native", payload: command });
}

async function loadVoices() {
  const response = await native({ command: "list_voices" });
  if (!response?.ok) throw new Error(response?.error || "Could not load Piper voices");
  voice.replaceChildren(...response.voices.map((item) => {
    const option = document.createElement("option");
    option.value = item.id;
    option.textContent = item.label;
    return option;
  }));
  const saved = await chrome.storage.local.get(["voice", "speed"]);
  if (saved.voice && [...voice.options].some((item) => item.value === saved.voice)) voice.value = saved.voice;
  if (saved.speed) speed.value = saved.speed;
  speedValue.value = `${Number(speed.value).toFixed(2).replace(/0$/, "")}×`;
  showStatus(`${response.voices.length} local voices available`);
}

function extractReadableText() {
  const selected = window.getSelection();
  const preferred = document.querySelector("article, main, [role='main']") || document.body;
  const scope = document.createRange();
  if (selected?.toString().trim() && selected.rangeCount) {
    const chosen = selected.getRangeAt(0);
    scope.setStart(chosen.startContainer, chosen.startOffset);
    scope.setEnd(chosen.endContainer, chosen.endOffset);
  } else {
    scope.selectNodeContents(preferred);
  }
  const ranges = [];
  const root = scope.commonAncestorContainer;
  const collect = (node, start, end) => {
    if (node.parentElement?.closest("script, style, noscript, nav, header, footer, aside, form, button")) return;
    for (const match of node.data.slice(start, end).matchAll(/\S+/g)) {
      const range = document.createRange();
      range.setStart(node, start + match.index);
      range.setEnd(node, start + match.index + match[0].length);
      ranges.push(range);
    }
  };
  if (root.nodeType === Node.TEXT_NODE) {
    collect(root, scope.startOffset, scope.endOffset);
  } else {
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) {
      const node = walker.currentNode;
      if (!scope.intersectsNode(node)) continue;
      collect(node, node === scope.startContainer ? scope.startOffset : 0,
        node === scope.endContainer ? scope.endOffset : node.length);
    }
  }
  globalThis.__leafExtensionWordRanges = ranges;
  globalThis.__leafExtensionSetWord = (index) => {
    if (!CSS.highlights || !ranges.length) return;
    const range = ranges[Math.max(0, Math.min(index, ranges.length - 1))];
    CSS.highlights.set("leaf-extension-reading-word", new Highlight(range));
    range.startContainer.parentElement?.scrollIntoView({ block: "center", behavior: "smooth" });
  };
  if (!document.querySelector("#leaf-extension-highlight-style")) {
    const style = document.createElement("style");
    style.id = "leaf-extension-highlight-style";
    style.textContent = "::highlight(leaf-extension-reading-word){background:#55b87999;color:inherit}";
    (document.head || document.documentElement).appendChild(style);
  }
  return ranges.map((range) => range.toString()).join(" ");
}

speed.addEventListener("input", () => {
  speedValue.value = `${Number(speed.value).toFixed(2).replace(/0$/, "")}×`;
});

readButton.addEventListener("click", async () => {
  readButton.disabled = true;
  showStatus("Collecting readable text…");
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    const [{ result = "" }] = await chrome.scripting.executeScript({ target: { tabId: tab.id }, func: extractReadableText });
    const text = result.slice(0, 100000);
    if (!text) throw new Error("No readable text found on this page");
    await chrome.storage.local.set({ voice: voice.value, speed: speed.value });
    const response = await native({ command: "speak", voice: voice.value, speed: Number(speed.value), text, tabId: tab.id });
    if (!response?.ok) throw new Error(response?.error || "Piper could not start");
    showStatus("Generating local speech…");
  } catch (error) {
    showStatus(error.message, true);
  } finally {
    readButton.disabled = false;
  }
});

stopButton.addEventListener("click", async () => {
  const response = await native({ command: "stop" });
  showStatus(response?.ok ? "Stopped" : (response?.error || "Could not stop"), !response?.ok);
});

chrome.runtime.onMessage.addListener((message) => {
  if (message?.type !== "native-event") return;
  const event = message.payload;
  if (event.event === "playing") showStatus("Reading aloud");
  if (event.event === "finished") showStatus("Finished");
  if (event.event === "error") showStatus(event.error || "Speech failed", true);
});

loadVoices().catch((error) => showStatus(error.message, true));
