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
  const selected = window.getSelection()?.toString().trim();
  if (selected) return selected;
  const preferred = document.querySelector("article, main, [role='main']");
  const root = (preferred || document.body).cloneNode(true);
  root.querySelectorAll("script, style, noscript, nav, header, footer, aside, form, button").forEach((node) => node.remove());
  return (root.innerText || root.textContent || "").replace(/\n{3,}/g, "\n\n").trim();
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
    const response = await native({ command: "speak", voice: voice.value, speed: Number(speed.value), text });
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
