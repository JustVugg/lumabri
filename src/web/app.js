"use strict";
// No credentials/transcripts in URLs, browser storage, analytics or third-party scripts.
const $ = id => document.getElementById(id);
let token = "", models = [], conversations = [], current = null, busy = false, controller = null;
let unsaved = false, navigation = 0;
let partialNode = null;
let operator = false, workspaceOpen = false, workspacePending = false, workspaceEpoch = 0;
let workspaceExpires = 0;
let management = false, managementBusy = false;
const utf8 = new TextEncoder();
const say = text => { $("notice").textContent = text; };
const errors = {
  authentication_required: "The access token was not accepted, or has been revoked. Sign in with a valid token.",
  model_not_authorized: "Your token does not have access to this model. Ask your cluster operator.",
  approved_allocation_unavailable: "A participating computer is unavailable. The model was not replaced or reloaded.",
  host_unavailable_or_busy: "The model's host is busy or unavailable. Try again when capacity is free.",
  replicas_busy: "All approved replicas are serving another API request. Your question is saved; retry when capacity is free.",
  admission_unavailable: "Local request admission could not be checked. No response was started.",
  management_permission_required: "Your token has no management permission. An operator must grant it explicitly.",
  allocation_changed_refresh_before_retry: "The allocation changed after it was inspected. Review its current state before retrying.",
  retirement_started_cannot_resume: "Release has already started and cannot be reversed. Reconcile that release instead.",
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
  $("sign-out").disabled = busy || managementBusy;
  $("workspace-open").hidden = !operator || !token;
  $("workspace-open").disabled = busy;
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
  const permissions = await request("/api/v1/session");
  operator = permissions.operator === true; management = permissions.management === true;
  if (!operator && workspaceOpen) closeWorkspace();
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
    closeWorkspace();
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
  closeWorkspace();
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
        } else if (kind === "recovering") {
          say("A replica stopped. Recovering on another approved computer; checking the text already shown…");
        } else if (kind === "done") {
          onText(text.decode()); done = true;
          if (value.recovery_attempts) say("Recovered on another approved replica. The text already shown was verified and was not repeated.");
        }
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
  if (busy || managementBusy) return;
  if (unsaved && !confirm("This answer is not saved. Sign out without exporting?")) return;
  navigation++; unsaved = false;
  token = ""; models = []; conversations = []; current = null;
  operator = false; management = false; closeWorkspace(); clearWorkspace();
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

function closeWorkspace() {
  workspaceOpen = false; workspaceEpoch++;
  $("workspace").hidden = true;
  $("chat").hidden = !token; $("composer-area").hidden = !token;
}
function clearWorkspace() {
  workspaceExpires = 0;
  for (const id of ["workspace-summary", "workspace-nodes", "workspace-models", "workspace-allocations"]) $(id).replaceChildren();
  $("workspace-status").textContent = "";
}
const gib = bytes => Number.isFinite(bytes) ? `${(bytes / 1073741824).toFixed(2)} GiB` : "Unknown";
function workspaceCard(parent, title, facts) {
  const card = document.createElement("article"); card.className = "workspace-card";
  const heading = document.createElement("h3"); heading.textContent = title; card.append(heading);
  const list = document.createElement("dl");
  for (const [label, value] of facts) {
    const term = document.createElement("dt"), detail = document.createElement("dd");
    term.textContent = label; detail.textContent = value; list.append(term, detail);
  }
  card.append(list); parent.append(card); return card;
}
async function controlAllocation(allocation, action) {
  if (!management || managementBusy || !workspaceOpen) return;
  const epoch = workspaceEpoch, credential = token;
  managementBusy = true; controls();
  for (const button of $("workspace-allocations").querySelectorAll("button")) button.disabled = true;
  let submitted = false, message = "";
  try {
    const path = `/api/v1/allocations/${allocation.id}/control`;
    const observed = await request(path);
    if (epoch !== workspaceEpoch || credential !== token || !workspaceOpen) return;
    if (action === "status" || observed.state === "released") {
      message = `${allocation.name}: ${observed.state.replaceAll("_", " ")}.`;
    } else {
      const effects = {
        drain: "Stop admitting new turns. Already admitted work can finish; weights remain in RAM.",
        resume: "Allow new turns on this already approved allocation. No new model is loaded.",
        retire: "Stop admitting turns, finish admitted work and release this allocation's RAM on its computers. This cannot be undone; using it again requires preparation and approval. Other models, checkpoint files and chat histories are kept."
      };
      if (observed.state === "retirement_pending" && action !== "retire")
        throw new Error("Release is already in progress. Choose Release RAM to reconcile it.");
      if (!confirm(`${allocation.name}\n\n${effects[action]}\n\nContinue?`)) return;
      submitted = true;
      const result = await request(path, "POST", {action, instance: observed.instance, revision: observed.revision});
      message = result.state === "retirement_pending" ? "Release is waiting for admitted work. Choose Release RAM again to reconcile; no forced interruption was used." :
        `${allocation.name}: ${result.state.replaceAll("_", " ")}.`;
    }
  } catch (error) {
    message = submitted ? `${error.message} The operation may have started. Inspect its status before retrying; no automatic retry was sent.` : error.message;
  } finally {
    managementBusy = false; controls();
    if (epoch === workspaceEpoch && credential === token && workspaceOpen) {
      await refreshWorkspace();
      if (message) say(message);
    }
  }
}
function renderWorkspace(data) {
  clearWorkspace();
  if (data.inventory_ok && data.nodes.length) {
    const oldest = Math.max(...data.nodes.map(node => node.age_ms));
    workspaceExpires = Date.now() + Math.max(0, data.inventory_ttl_ms - oldest);
  }
  const time = new Date(data.captured_at * 1000).toLocaleTimeString();
  $("workspace-status").textContent = !data.inventory_ok ? "Inventory unavailable — live resource values withheld." :
    `Reports captured at ${time} · leases expire after ${data.inventory_ttl_ms / 1000}s · refreshed every 10s while visible`;
  if (!data.registry_ok) $("workspace-status").textContent += " Model registry unavailable.";
  workspaceCard($("workspace-summary"), "Service overview", [
    ["Reporting computers", data.inventory_ok ? String(data.nodes.length) : "Unknown"],
    ["Saved allocations", String(data.allocations.length)],
    ["Managed model names", data.registry_ok ? String(data.models.length) : "Unknown"]]);
  const names = new Map(data.nodes.map(node => [node.id, node.name]));
  for (const node of data.nodes) {
    const workload = node.workload;
    workspaceCard($("workspace-nodes"), node.name, [
      ["Hardware", `${node.cpu} · ${node.os} / ${node.arch}`],
      ["Compute threads", node.runtime_threads ? `${node.runtime_threads} runtime / ${node.threads} detected` : "Runtime not verified"],
      ["RAM", `${gib(node.ram_available_bytes)} available / ${gib(node.ram_total_bytes)} total`],
      ["Offered / reserved RAM", `${gib(node.ram_offered_bytes)} / ${workload ? gib(workload.reserved_bytes) : "Unknown"}`],
      ["GPU", `${node.gpu_detected} detected · ${gib(node.vram_inventory_bytes)} inventory · execution not verified`],
      ["Workload", workload ? `${workload.allocations} allocations · ${workload.active} active · ${workload.queued} queued` : "Unknown"],
      ["Machine cost", node.machine_cost ? `${node.machine_cost.currency} ${(node.machine_cost.micro_units_per_hour / 1000000).toFixed(4)}/h · declared` : "Not supplied"],
      ["Power / energy", node.power ? `${node.power.watts} W · declared estimate; energy not measured` : "Not measured"],
      ["Report age", `${(node.age_ms / 1000).toFixed(1)}s at capture`]]);
  }
  if (!data.nodes.length) $("workspace-nodes").textContent = "No current machine reports. Sharing and connectivity are managed by the local service.";
  for (const model of data.models) workspaceCard($("workspace-models"), model.name, [
    ["Adapter", model.adapter], ["Approved replica references", String(model.replicas)],
    ["Context / output limit", `${model.context} / ${model.max_tokens} tokens`],
    ["Revision", String(model.revision)], ["Readiness", "Saved route; actual hosts checked on request"]]);
  if (!data.models.length) $("workspace-models").textContent = data.registry_ok ? "No managed model names registered yet." : "Registry unavailable.";
  for (const allocation of data.allocations) {
    const observation = allocation.observation;
    const speed = observation.state === "measured" ? `${observation.decode_tok_s.toFixed(2)} tok/s · last ${observation.generated_tokens}-token reply (${observation.prompt_tokens}-token prompt), not capacity` :
      observation.state === "obsolete" ? "Obsolete — configuration changed; observe a new turn" : "Not measured";
    const card = workspaceCard($("workspace-allocations"), allocation.name, [
      ["Adapter", allocation.adapter || "Unknown"], ["Speed", speed],
      ["Context / session limit", `${allocation.context} tokens / ${allocation.session_limit} configured slots`],
      ["Last preparation", allocation.preparation_seconds === null ? "Not measured" : `${allocation.preparation_seconds.toFixed(2)}s · approval and indexing excluded`],
      ...allocation.ranges.map(range => [range.edge ? "Edge + layers" : "Layers",
        `${range.begin}–${range.end - 1} · ${names.get(range.node) || "Computer not reporting"} · ${gib(range.reserved_bytes)} reserved`])]);
    if (management && /^[0-9a-f]{64}$/.test(allocation.id)) {
      const actions = document.createElement("div"); actions.className = "allocation-actions";
      for (const [action, label] of [["status", "Inspect"], ["drain", "Drain"], ["resume", "Resume"], ["retire", "Release RAM"]]) {
        const button = document.createElement("button"); button.textContent = label; button.className = "text-button";
        if (action === "retire") button.classList.add("danger");
        button.onclick = () => controlAllocation(allocation, action); actions.append(button);
      }
      card.append(actions);
    }
  }
  if (!data.allocations.length) $("workspace-allocations").textContent = "No saved allocations for this household.";
}
async function refreshWorkspace() {
  if (!workspaceOpen || !operator || !token || workspacePending || managementBusy || document.hidden) return;
  workspacePending = true; $("workspace-refresh").disabled = true;
  const epoch = workspaceEpoch;
  try {
    const data = await request("/api/v1/workspace");
    if (epoch === workspaceEpoch && workspaceOpen) renderWorkspace(data);
  } catch (error) {
    if (epoch === workspaceEpoch && workspaceOpen) {
      clearWorkspace(); $("workspace-status").textContent = `Workspace unavailable: ${error.message}`;
    }
  } finally { workspacePending = false; $("workspace-refresh").disabled = false; }
}
$("workspace-open").onclick = () => {
  if (busy || !operator) return;
  workspaceOpen = true; workspaceEpoch++; say("");
  $("workspace").hidden = false; $("chat").hidden = true; $("composer-area").hidden = true;
  clearWorkspace(); $("workspace-status").textContent = "Reading the local service…";
  if (window.matchMedia("(max-width: 760px)").matches) sidebar(false);
  refreshWorkspace();
};
$("workspace-refresh").onclick = refreshWorkspace;
setInterval(refreshWorkspace, 10000);
setInterval(() => {
  if (workspaceOpen && !managementBusy && workspaceExpires && Date.now() >= workspaceExpires) {
    clearWorkspace(); $("workspace-status").textContent = "Machine reports expired — refreshing before showing resource values.";
    refreshWorkspace();
  }
}, 1000);
