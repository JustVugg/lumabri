"use strict";
// No credentials/transcripts in URLs, browser storage, analytics or third-party scripts.
const $ = id => document.getElementById(id);
let token = "", models = [], conversations = [], current = null, busy = false, controller = null;
let unsaved = false, navigation = 0;
let partialNode = null;
const utf8 = new TextEncoder();
const say = text => { $("notice").textContent = text; };
const errors = {
  authentication_required: "The access token was not accepted, or has been revoked. Sign in with a valid token.",
  model_not_authorized: "Your token does not have access to this model. Ask your cluster operator.",
  approved_allocation_unavailable: "A participating computer is unavailable. The model was not replaced or reloaded.",
  host_unavailable_or_busy: "The model's host is busy or unavailable. Try again when capacity is free.",
  resident_plan_not_found: "This saved model allocation is no longer available. Ask the operator to prepare and grant a new one.",
  conversation_changed_reload_before_editing: "This conversation changed in another window. Reopen it from the sidebar before sending.",
  conversation_limit_32: "Your history has reached 32 conversations. Export and delete one before creating another.",
  history_busy_retry: "Another history operation is in progress. Try again in a moment.",
  user_request_limit: "Two requests are already active for your account. Wait or stop one before retrying.",
  history_storage_unavailable: "Private history storage is unavailable. No new question was sent unless generation had already started."
};
const explain = code => errors[code] || String(code).replaceAll("_", " ");
function boundedTitle(text) {
  let title = Array.from(text.trim().replace(/\s+/gu, " ")).slice(0, 80).join("") || "New conversation";
  while (utf8.encode(title).length > 128) title = Array.from(title).slice(0, -1).join("");
  return title;
}
function sidebar(open) {
  $("app").classList.toggle("sidebar-hidden", !open);
  $("expand").hidden = open;
}
$("collapse").onclick = () => sidebar(false);
$("expand").onclick = () => sidebar(true);
if (window.matchMedia("(max-width: 760px)").matches) sidebar(false);

async function request(path, method = "GET", body) {
  const credential = token;
  const options = {method, headers: {Authorization: `Bearer ${credential}`}, cache: "no-store"};
  if (body !== undefined) { options.body = JSON.stringify(body); options.headers["Content-Type"] = "application/json"; }
  const abort = new AbortController(), timer = setTimeout(() => abort.abort(), 15000);
  options.signal = abort.signal;
  try {
    const response = await fetch(path, options);
    const answer = await response.json();
    if (credential !== token) throw new Error("Connection changed; this result was discarded");
    if (!response.ok) throw new Error(explain(answer.error || `Request failed (${response.status})`));
    return answer;
  } finally { clearTimeout(timer); }
}
function controls() {
  const selected = models.find(model => model.id === $("model").value);
  const canSend = Boolean(token && selected && !busy);
  $("new-chat").disabled = !token || busy;
  $("model").disabled = !token || busy || Boolean(current);
  $("refresh").disabled = !token || busy;
  $("sign-out").disabled = busy;
  $("prompt").disabled = !canSend;
  $("send").disabled = !canSend;
  $("stop").hidden = !busy;
  $("delete").disabled = !current || busy;
  $("export").disabled = !current || busy;
  const state = current?.conversation.state;
  $("retry").hidden = !current || !["pending", "interrupted"].includes(state);
  $("retry").disabled = !canSend;
  $("save-status").textContent = unsaved ? "Not saved — export before leaving" : busy ? "Generating · you can stop this response" : !current ?
    "History is saved on this computer." : ["pending", "interrupted"].includes(state) ?
    "Incomplete response · retry explicitly, or start a new conversation" : "Saved privately on this computer";
  if (!busy && current && ["pending", "interrupted"].includes(state)) {
    $("prompt").disabled = true; $("send").disabled = true;
  }
  if (!busy && current?.conversation.messages.some(message => message.content.includes("\u0000"))) {
    $("prompt").disabled = true; $("send").disabled = true;
    $("save-status").textContent = unsaved ? "Not saved — export before leaving" : "Saved · reply contains a NUL byte; start a new chat to continue";
  }
  for (const button of $("history").querySelectorAll("button")) button.disabled = busy;
}
function renderModels() {
  const selected = current?.conversation.model || (models.some(model => model.id === $("model").value) ? $("model").value : "");
  $("model").replaceChildren();
  for (const model of models) {
    const option = new Option(model.name, model.id); $("model").add(option);
  }
  if (selected && !models.some(model => model.id === selected)) {
    $("model").add(new Option("Saved model — currently unavailable", selected));
  } else if (!models.length) $("model").add(new Option("No approved resident models", ""));
  if (selected) $("model").value = selected;
}
function renderHistory() {
  $("history").replaceChildren();
  if (!conversations.length) {
    const empty = document.createElement("p"); empty.className = "muted";
    empty.textContent = "A little space for your next idea."; $("history").append(empty);
  }
  for (const entry of [...conversations].sort((a, b) => b.updated - a.updated || b.id.localeCompare(a.id))) {
    const button = document.createElement("button"); button.textContent = entry.title; button.title = entry.title;
    button.classList.toggle("active", entry.id === current?.id);
    button.onclick = () => openConversation(entry.id);
    $("history").append(button);
  }
  controls();
}
function appendMessage(role, content, interrupted = false) {
  const block = document.createElement("article"); block.className = `message ${role}`;
  const name = document.createElement("div"); name.className = "role"; name.textContent = role === "user" ? "You" : "Lumabri";
  const text = document.createElement("div"); text.className = "content"; text.textContent = content;
  block.append(name, text);
  if (interrupted) {
    block.classList.add("interrupted"); const label = document.createElement("div"); label.className = "incomplete";
    label.textContent = "Incomplete response"; block.append(label);
  }
  $("messages").append(block); return text;
}
function scrollToEnd() { $("chat").scrollTop = $("chat").scrollHeight; }
function renderMessages() {
  $("messages").replaceChildren(); const messages = current?.conversation.messages || [];
  $("empty").hidden = Boolean(messages.length);
  messages.forEach((message, i) => appendMessage(message.role, message.content,
    i === messages.length - 1 && message.role === "assistant" && current.conversation.state !== "complete"));
  scrollToEnd(); controls();
}
async function refresh() {
  // Sequential calls leave the second per-user permit free for another tab.
  models = (await request("/api/v1/models")).models;
  conversations = (await request("/api/v1/conversations")).conversations;
  renderModels(); renderHistory();
}
async function openConversation(id) {
  if (busy) return;
  if (unsaved && !confirm("This answer is not saved. Leave it without exporting?")) return;
  const ticket = ++navigation;
  try {
    const loaded = await request(`/api/v1/conversations/${id}`);
    if (ticket !== navigation) return;
    current = loaded; unsaved = false;
    renderModels(); renderHistory(); renderMessages(); say("");
    if (["pending", "interrupted"].includes(current.conversation.state))
      say("This response was not saved as complete. Retry sends the last question again; it does not resume a hidden generation.");
    if (window.matchMedia("(max-width: 760px)").matches) sidebar(false);
  } catch (error) { say(error.message); }
}
function newConversation() {
  if (busy) return;
  if (unsaved && !confirm("This answer is not saved. Leave it without exporting?")) return;
  navigation++; unsaved = false;
  current = null; $("prompt").value = ""; say(""); renderModels(); renderHistory(); renderMessages(); $("prompt").focus();
  if (window.matchMedia("(max-width: 760px)").matches) sidebar(false);
}
async function saveConversation(conversation) {
  const saved = await request(current ? `/api/v1/conversations/${current.id}` : "/api/v1/conversations", "POST",
    {revision: current?.revision || 0, conversation});
  current = saved;
  unsaved = false;
  const summary = {id: saved.id, revision: saved.revision, updated: saved.updated,
    model: conversation.model, title: conversation.title, state: conversation.state};
  conversations = [summary, ...conversations.filter(item => item.id !== saved.id)];
  renderHistory(); return saved;
}
async function consumeStream(response, onText) {
  const reader = response.body.getReader(), wire = new TextDecoder("utf-8", {fatal: true});
  const text = new TextDecoder("utf-8", {fatal: true});
  let pending = "", done = false, bytes = 0;
  try {
    for (;;) {
      const chunk = await reader.read();
      pending += chunk.done ? wire.decode() : wire.decode(chunk.value, {stream: true});
      let end;
      while ((end = pending.indexOf("\n\n")) !== -1) {
        const event = pending.slice(0, end); pending = pending.slice(end + 2);
        const lines = event.split("\n");
        if (lines.length !== 2 || !lines[0].startsWith("event: ") || !lines[1].startsWith("data: ") || done)
          throw new Error("Invalid response stream");
        const kind = lines[0].slice(7), value = JSON.parse(lines[1].slice(6));
        if (kind === "delta") {
          const decoded = Uint8Array.from(atob(value.bytes), char => char.charCodeAt(0));
          bytes += decoded.length;
          if (bytes > 65536) throw new Error("Response exceeded the conversation limit; the partial answer is preserved.");
          onText(text.decode(decoded, {stream: true}));
        } else if (kind === "done") { onText(text.decode()); done = true; }
        else if (kind === "error") throw new Error(value.message || "Generation failed");
        else throw new Error("Unknown response event");
      }
      if (pending.length > 65536) throw new Error("Response frame too large");
      if (chunk.done) break;
    }
    if (!done || pending.trim()) throw new Error("Connection closed before the response completed");
  } finally { await reader.cancel().catch(() => {}); reader.releaseLock(); }
}
async function generate(retry = false) {
  if (busy || !token) return;
  const model = models.find(item => item.id === $("model").value);
  if (!model) { say("This resident model is not available. Refresh the model list or ask the operator to prepare it."); return; }
  let conversation;
  if (retry) {
    if (!current || !["pending", "interrupted"].includes(current.conversation.state)) return;
    conversation = structuredClone(current.conversation);
    if (conversation.messages.at(-1)?.role === "assistant") conversation.messages.pop();
  } else {
    const prompt = $("prompt").value.trim(); if (!prompt) return;
    if (current && current.conversation.state !== "complete" && current.conversation.state !== "idle") return;
    if (current?.conversation.messages.some(message => message.content.includes("\u0000"))) {
      say("This reply is saved losslessly, but contains a NUL byte that the text engine interface cannot replay. Start a new conversation."); return;
    }
    conversation = current ? structuredClone(current.conversation) : {model: model.id, title: boundedTitle(prompt), messages: []};
    conversation.messages.push({role: "user", content: prompt});
  }
  if (conversation.messages.length > 129) { say("This conversation reached its 65-question limit. Start a new conversation; your history stays available."); return; }
  busy = true; controller = new AbortController(); controls(); say(""); let started = false, answer = "", complete = false;
  try {
    conversation.state = "pending";
    await saveConversation(conversation); // Save the question BEFORE sending any text to the model.
    started = true; $("prompt").value = ""; $("prompt").style.height = "auto";
    renderMessages(); partialNode = appendMessage("assistant", "");
    const timeout = setTimeout(() => controller?.abort(), 305000);
    try {
      const response = await fetch("/api/v1/chat", {method: "POST", signal: controller.signal,
        headers: {Authorization: `Bearer ${token}`, "Content-Type": "application/json"},
        body: JSON.stringify({model: model.id, messages: conversation.messages, max_tokens: Math.min(256, model.max_tokens)})});
      if (!response.ok) { const problem = await response.json(); throw new Error(explain(problem.error || `Generation failed (${response.status})`)); }
      await consumeStream(response, delta => {
        const nearEnd = $("chat").scrollHeight - $("chat").scrollTop - $("chat").clientHeight < 100;
        answer += delta; partialNode.textContent = answer; if (nearEnd) scrollToEnd();
      });
      complete = true;
    } finally { clearTimeout(timeout); }
  } catch (error) {
    say(error.name === "AbortError" ? "Response stopped. The incomplete answer is kept separately from completed turns." : error.message);
  } finally {
    controller = null;
    if (started) {
      if (complete || answer) conversation.messages.push({role: "assistant", content: answer});
      conversation.state = complete ? "complete" : "interrupted";
      try { await saveConversation(conversation); renderMessages(); }
      catch (error) {
        // Keep visible text and an exportable in-memory copy, without pretending it was saved.
        current.conversation = conversation;
        unsaved = true;
        say(`This answer is NOT saved: ${error.message}. Export it before reloading. Another window may have changed this conversation.`);
      }
    }
    busy = false; partialNode = null; controls();
  }
}
$("login-form").onsubmit = async event => {
  event.preventDefault(); const button = $("login-form").querySelector("button");
  if (button.disabled) return;
  button.disabled = true; token = $("token").value.trim(); $("token").value = "";
  try {
    await refresh(); $("login").hidden = true; $("chat").hidden = false; $("composer-area").hidden = false;
    $("identity").textContent = token.split(".")[0]; $("sign-out").hidden = false; $("app").classList.add("connected");
    newConversation();
    if (!models.length) say("No approved resident models are assigned to this token yet. Your operator can grant access to a prepared model.");
  } catch (error) { token = ""; models = []; conversations = []; renderModels(); renderHistory(); say(error.message); }
  finally { button.disabled = false; }
};
$("sign-out").onclick = () => {
  if (busy) return;
  if (unsaved && !confirm("This answer is not saved. Sign out without exporting?")) return;
  navigation++; unsaved = false;
  token = ""; models = []; conversations = []; current = null;
  $("login").hidden = false; $("chat").hidden = true; $("composer-area").hidden = true; $("sign-out").hidden = true;
  $("identity").textContent = "Not connected"; $("app").classList.remove("connected");
  $("prompt").value = ""; renderModels(); renderHistory(); renderMessages(); say("");
};
$("new-chat").onclick = newConversation;
$("refresh").onclick = () => refresh().then(() => say("")).catch(error => say(error.message));
$("model").onchange = controls;
$("composer").onsubmit = event => { event.preventDefault(); generate(); };
$("prompt").onkeydown = event => {
  if (event.key === "Enter" && !event.shiftKey && !event.isComposing) { event.preventDefault(); generate(); }
};
$("prompt").oninput = () => { $("prompt").style.height = "auto"; $("prompt").style.height = `${Math.min(160, $("prompt").scrollHeight)}px`; };
$("stop").onclick = () => controller?.abort();
$("retry").onclick = () => generate(true);
$("delete").onclick = async () => {
  if (!current || busy || !confirm("Delete this conversation from the local service? This cannot be undone.")) return;
  try {
    await request(`/api/v1/conversations/${current.id}`, "DELETE", {revision: current.revision});
    conversations = conversations.filter(item => item.id !== current.id); newConversation();
  } catch (error) { say(error.message); }
};
$("export").onclick = () => {
  if (!current) return;
  const blob = new Blob([JSON.stringify(current, null, 2)], {type: "application/json"});
  const url = URL.createObjectURL(blob), link = document.createElement("a");
  link.href = url; link.download = `lumabri-chat-${current.id}.json`; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
};
window.addEventListener("beforeunload", event => { if (busy || unsaved) { event.preventDefault(); event.returnValue = ""; } });
