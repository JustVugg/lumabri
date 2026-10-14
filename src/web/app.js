"use strict";
// No credentials/transcripts in URLs, browser storage, analytics or third-party scripts.
const $ = id => document.getElementById(id);
let token = "", models = [], conversations = [], current = null, busy = false, controller = null;
let unsaved = false, navigation = 0, opening = null;
const deleting = new Set();
let partialNode = null;
let operator = false, workspaceOpen = false, workspacePending = false, workspaceEpoch = 0;
let workspaceExpires = 0;
let management = false, managementBusy = false;
let preparationReview = null, preparationBusy = false, preparationOperation = "";
let preparationNames = new Map();
let elasticPending = false, elasticEditor = null, elasticReview = null, elasticEpoch = 0;
const utf8 = new TextEncoder();
const say = text => { $("notice").textContent = text; };
const errors = {
  authentication_required: "The access token was not accepted, or has been revoked. Sign in with a valid token.",
  model_not_authorized: "Your token does not have access to this model. Ask your cluster operator.",
  approved_allocation_unavailable: "A participating computer is unavailable. The model was not replaced or reloaded.",
  host_unavailable_or_busy: "The model's host is busy or unavailable. Try again when capacity is free.",
  replicas_busy: "All approved replicas are serving another API request. Your question is saved; retry when capacity is free.",
  model_dormant_preparation_required: "This model is not resident. Replica preparation has been requested if its policy permits it. Your question is saved; retry after the operator reports it ready.",
  elastic_revision_or_ownership_conflict: "The policy, model route or allocation ownership changed. Reload and review the current settings. No automatic retry was sent.",
  elastic_source_or_budget_unavailable: "The checkpoint, selected computers or memory budget cannot satisfy this policy. Check the source and approved replicas before retrying.",
  elastic_state_unavailable: "Replica policy state is temporarily unavailable. Inspect it before retrying; a timed-out change may already have been applied.",
  invalid_elastic_policy: "The replica policy contains unsupported or inconsistent settings. Reload and review its limits.",
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
  const canSend = Boolean(token && selected && !busy && !deleting.has(current?.id));
  $("new-chat").disabled = !token || busy;
  $("model").disabled = !token || busy || Boolean(current);
  $("refresh").disabled = !token || busy;
  $("sign-out").disabled = busy || managementBusy;
  $("workspace-open").hidden = !operator || !token;
  $("workspace-open").disabled = busy;
  $("preparation").hidden = !management || !token;
  $("elastic-form").inert = managementBusy || !management;
  $("elastic-apply").disabled = managementBusy || !management;
  for (const button of $("elastic-policies").querySelectorAll("button")) {
    button.hidden = !management;
    button.disabled = managementBusy || !management || button.dataset.allowed !== "true";
  }
  if (!management && elasticEditor) closeElasticEditor();
  $("prompt").disabled = !canSend;
  $("send").disabled = !canSend;
  $("stop").hidden = !busy;
  $("delete").disabled = !current || busy || deleting.has(current.id);
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
  opening = {id, ticket};
  try {
    const loaded = await request(`/api/v1/conversations/${id}`);
    if (ticket !== navigation) return;
    current = loaded; unsaved = false;
    closeWorkspace();
    renderModels(); renderHistory(); renderMessages(); say("");
    if (["pending", "interrupted"].includes(current.conversation.state))
      say("This response was not saved as complete. Retry sends the last question again; it does not resume a hidden generation.");
    if (window.matchMedia("(max-width: 760px)").matches) sidebar(false);
  } catch (error) { if (ticket === navigation) say(error.message); }
  finally { if (opening?.ticket === ticket) opening = null; }
}
function newConversation() {
  if (busy) return;
  if (unsaved && !confirm("This answer is not saved. Leave it without exporting?")) return;
  navigation++; opening = null;
  resetConversation();
}
function resetConversation() {
  unsaved = false;
  closeWorkspace();
  current = null; $("prompt").value = ""; say(""); renderModels(); renderHistory(); renderMessages(); $("prompt").focus();
  if (window.matchMedia("(max-width: 760px)").matches) sidebar(false);
}
async function saveConversation(conversation, target) {
  const saved = await request(target ? `/api/v1/conversations/${target.id}` : "/api/v1/conversations", "POST",
    {revision: target?.revision || 0, conversation});
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
  if (busy || !token || deleting.has(current?.id)) return;
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
  // A history GET started before this turn must not replace its destination.
  navigation++; opening = null;
  let saved = current;
  busy = true; controller = new AbortController(); controls(); say(""); let started = false, answer = "", complete = false;
  try {
    conversation.state = "pending";
    saved = await saveConversation(conversation, saved); // Save the question BEFORE sending any text to the model.
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
      try { await saveConversation(conversation, saved); renderMessages(); }
      catch (error) {
        // Keep visible text and an exportable in-memory copy, without pretending it was saved.
        current = {...saved, conversation};
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
  navigation++; opening = null; unsaved = false;
  token = ""; models = []; conversations = []; current = null;
  operator = false; management = false; closeWorkspace(); clearWorkspace();
  preparationOperation = ""; preparationInvalidate();
  $("prepare-form").hidden = true; $("prepare-models").replaceChildren(); $("prepare-nodes").replaceChildren();
  $("prepare-status").textContent = ""; $("prepare-operation").replaceChildren(new Option("No operation selected", ""));
  $("prepare-cancel").hidden = true;
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
  if (!current || busy || deleting.has(current.id) || !confirm("Delete this conversation from the local service? This cannot be undone.")) return;
  const {id, revision} = current, credential = token;
  deleting.add(id); controls();
  try {
    await request(`/api/v1/conversations/${id}`, "DELETE", {revision});
    conversations = conversations.filter(item => item.id !== id);
    // Discard a stale load of the deleted document, but preserve navigation
    // to another conversation even when its GET has not finished yet.
    if (opening?.id === id) { navigation++; opening = null; }
    if (current?.id === id) resetConversation();
    else renderHistory();
  } catch (error) { if (credential === token) say(error.message); }
  finally { deleting.delete(id); controls(); }
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
  closeElasticEditor(); $("elastic-policies").replaceChildren(); $("elastic-status").textContent = "";
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
  const demand = data.demand, demandFresh = demand && demand.snapshot_age_ms <= 1000;
  workspaceCard($("workspace-summary"), "Gateway demand", [
    ["Scope", "This gateway only · direct TUI chats are not counted"],
    ["Observation", !demandFresh ? "Unavailable or stale" : demand.window_complete ?
      "Last 60 seconds" : "Partial window — startup or telemetry gap"],
    ["Live requests", demandFresh ? `${demand.models.reduce((n, m) => n + m.admitted, 0)} admitted · ${demand.models.reduce((n, m) => n + m.opening, 0)} opening` : "Unknown"]]);
  const traffic = id => {
    const m = demandFresh && demand.models.find(model => model.id === id);
    if (!m) return [["Gateway activity", demandFresh ? "No recent activity observed" : "Unknown"]];
    return [["Gateway requests", `${m.admitted} admitted · ${m.opening} opening (not a queue)`],
      ["Window outcomes", `${m.completed} completed · ${m.capacity_rejected} capacity rejected · ${m.failed + m.interrupted} failed/interrupted · ${m.cancelled} cancelled`],
      ["First text / completion p95", `${m.latency.ttft_ms ? m.latency.ttft_ms.p95.toFixed(0) + " ms" : "Unknown"} / ${m.latency.completion_ms ? m.latency.completion_ms.p95.toFixed(0) + " ms" : "Unknown"} · ${m.latency.completion_samples} successful samples`]];
  };
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
  for (const model of data.models) {
    const card = workspaceCard($("workspace-models"), model.name, [
      ["Adapter", model.adapter], ["Approved replica references", String(model.replicas)],
      ["Context / output limit", `${model.context} / ${model.max_tokens} tokens`],
      ["Revision", String(model.revision)], ["Readiness", model.state === "dormant" ? "Dormant — preparation needed" : "Saved route; actual hosts checked on request"], ...traffic(model.id)]);
    if (management) {
      const button = document.createElement("button"); button.className = "text-button"; button.textContent = "Manage replicas";
      button.onclick = () => openElasticEditor(model.id); card.append(button);
    }
  }
  if (!data.models.length) $("workspace-models").textContent = data.registry_ok ? "No managed model names registered yet." : "Registry unavailable.";
  for (const allocation of data.allocations) {
    const observation = allocation.observation;
    const speed = observation.state === "measured" ? `${observation.decode_tok_s.toFixed(2)} tok/s · last ${observation.generated_tokens}-token reply (${observation.prompt_tokens}-token prompt), not capacity` :
      observation.state === "obsolete" ? "Obsolete — configuration changed; observe a new turn" : "Not measured";
    const card = workspaceCard($("workspace-allocations"), allocation.name, [
      ["Adapter", allocation.adapter || "Unknown"], ["Speed", speed],
      ["Context / session limit", `${allocation.context} tokens / ${allocation.session_limit} configured slots`],
      ["Last preparation", allocation.preparation_seconds === null ? "Not measured" : `${allocation.preparation_seconds.toFixed(2)}s · approval and indexing excluded`],
      ...traffic(allocation.id),
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
  if (epoch === workspaceEpoch && workspaceOpen) await refreshElastic();
}
$("workspace-open").onclick = () => {
  if (busy || !operator) return;
  workspaceOpen = true; workspaceEpoch++; say("");
  $("workspace").hidden = false; $("chat").hidden = true; $("composer-area").hidden = true;
  clearWorkspace(); $("workspace-status").textContent = "Reading the local service…";
  if (window.matchMedia("(max-width: 760px)").matches) sidebar(false);
  refreshWorkspace();
  if (management) loadPreparations().then(refreshElastic);
};
function preparationInvalidate() {
  preparationReview = null; $("prepare-review").replaceChildren(); $("prepare-start").hidden = true;
}
function preparationCheck(parent, value, label, enabled = true) {
  const row = document.createElement("label"), box = document.createElement("input");
  row.className = "prepare-choice"; box.type = "checkbox"; box.value = value; box.disabled = !enabled;
  row.append(box, document.createTextNode(label)); parent.append(row);
}
async function preparationTask(fn) {
  if (!management || preparationBusy || managementBusy || !workspaceOpen) return;
  preparationBusy = true; managementBusy = true; controls();
  $("prepare-form").inert = true;
  for (const id of ["prepare-catalogue", "prepare-preview", "prepare-start", "prepare-cancel", "prepare-operation"]) $(id).disabled = true;
  const credential = token, epoch = workspaceEpoch;
  const valid = () => credential === token && epoch === workspaceEpoch && workspaceOpen;
  try { await fn(valid); } catch (error) { if (valid()) $("prepare-status").textContent = error.message; }
  finally {
    preparationBusy = false; managementBusy = false; controls();
    $("prepare-form").inert = false;
    for (const id of ["prepare-catalogue", "prepare-preview", "prepare-start", "prepare-cancel", "prepare-operation"]) $(id).disabled = false;
  }
}
async function loadPreparations() {
  return preparationTask(async valid => {
    const data = await request("/api/v1/preparation"); if (!valid()) return;
    const list = data.operations.sort((a, b) => b.updated_at - a.updated_at);
    $("prepare-operation").replaceChildren(new Option("No operation selected", ""));
    for (const entry of list) $("prepare-operation").add(new Option(`${entry.recorded_state} · ${entry.operation.slice(0, 12)} · ${entry.detail}`, entry.operation));
    preparationOperation = list.some(entry => entry.operation === preparationOperation) ? preparationOperation : list[0]?.operation || "";
    $("prepare-operation").value = preparationOperation;
  });
}
$("prepare-catalogue").onclick = () => preparationTask(async valid => {
  const catalogue = await request("/api/v1/catalogue"), workspace = await request("/api/v1/workspace");
  if (!valid()) return;
  preparationInvalidate(); $("prepare-models").replaceChildren(); $("prepare-nodes").replaceChildren();
  preparationNames = new Map(workspace.nodes.map(node => [node.id, `${node.name} (${node.id.slice(0, 8)})`]));
  for (const model of catalogue.models) preparationCheck($("prepare-models"), model.name,
    `${model.name} · ${model.adapter} · ${gib(model.checkpoint_bytes)}${model.source_ready ? "" : " · source unavailable"}`, model.source_ready);
  for (const node of workspace.nodes) preparationCheck($("prepare-nodes"), node.id, `${preparationNames.get(node.id)} · ${gib(node.ram_offered_bytes)} offered`);
  $("prepare-form").hidden = false; $("prepare-status").textContent = "Nothing is selected automatically. Preview does not reserve memory.";
});
$("prepare-form").oninput = preparationInvalidate;
$("prepare-form").onsubmit = event => {
  event.preventDefault();
  preparationTask(async valid => {
    const selected = id => Array.from($(id).querySelectorAll("input:checked"), input => input.value);
    const intent = {models: selected("prepare-models"), nodes: selected("prepare-nodes"),
      context: Number($("prepare-context").value), sessions: Number($("prepare-sessions").value), max_new: Number($("prepare-output").value)};
    if (!intent.models.length || !intent.nodes.length) throw new Error("Choose at least one model and one computer.");
    preparationInvalidate();
    const data = await request("/api/v1/preparation", "POST", {action: "preview", ...intent});
    if (!valid()) return;
    preparationReview = {intent, review: data.review};
    for (const model of data.plan.models) workspaceCard($("prepare-review"), model.name, model.slices.map(slice => [
      `Layers ${slice.begin}–${slice.end - 1}`, `${preparationNames.get(data.plan.nodes[slice.node].id) || data.plan.nodes[slice.node].id.slice(0, 12)} · ${gib(slice.reserved_bytes)} reserved${slice.node === model.edge_node ? " · chat host" : ""}`]));
    $("prepare-start").hidden = false; $("prepare-status").textContent = "Resident Segment plan. Speed is not calibrated. Review and request approval below.";
  });
};
$("prepare-start").onclick = () => preparationTask(async valid => {
  if (!preparationReview) return;
  const reviewed = preparationReview;
  if (!reviewed.operation) reviewed.operation = Array.from(crypto.getRandomValues(new Uint8Array(32)), byte => byte.toString(16).padStart(2, "0")).join("");
  preparationOperation = reviewed.operation;
  $("prepare-status").textContent = `Starting ${preparationOperation}. Closing this page will not cancel preparation.`;
  const data = await request("/api/v1/preparation", "POST", {action: "start", operation: reviewed.operation, review: reviewed.review, ...reviewed.intent});
  if (!valid()) return;
  preparationInvalidate(); $("prepare-status").textContent = data.detail || "Request started. Participating donors must accept it.";
  $("prepare-operation").add(new Option(`Current · ${preparationOperation.slice(0, 12)}`, preparationOperation));
  $("prepare-operation").value = preparationOperation;
});
$("prepare-operation").onchange = () => { preparationOperation = $("prepare-operation").value; $("prepare-cancel").hidden = true; };
async function preparationObserve() {
  if (!preparationOperation || preparationBusy || managementBusy || !workspaceOpen || !management || document.hidden) return;
  return preparationTask(async valid => {
    const operation = preparationOperation;
    const data = await request("/api/v1/preparation", "POST", {action: "status", operation});
    if (!valid() || operation !== preparationOperation) return;
    $("prepare-status").textContent = `${data.state.replaceAll("_", " ")} · ${data.detail}`;
    $("prepare-cancel").hidden = !data.live || data.state !== "running";
  });
}
$("prepare-cancel").onclick = () => preparationTask(async valid => {
  if (!preparationOperation || !confirm("Cancel incomplete preparation? Models already ready will stay loaded.")) return;
  const data = await request("/api/v1/preparation", "POST", {action: "cancel", operation: preparationOperation});
  if (valid()) $("prepare-status").textContent = data.cancel_requested ? "Cancellation requested. Waiting for cleanup." : data.detail;
});
setInterval(preparationObserve, 3000);

// The browser edits policy intent only. Placement, approvals, reservations and
// reconciliation remain in the same service used by the CLI and other clients.
function invalidateElasticReview() {
  elasticReview = null; $("elastic-review").replaceChildren(); $("elastic-apply").hidden = true;
}
function closeElasticEditor() {
  elasticEditor = null; invalidateElasticReview(); $("elastic-form").hidden = true;
  $("elastic-form").reset(); $("elastic-source").replaceChildren(); $("elastic-nodes").replaceChildren();
  $("elastic-title").textContent = "Manage replicas";
}
function elasticGiB(bytes) {
  // A power-of-two divisor has a finite decimal expansion. Preserve every
  // byte when editing an existing uint64 budget, without Number rounding.
  const n = BigInt(bytes), unit = 1073741824n;
  const fraction = ((n % unit) * 10n ** 30n / unit).toString().padStart(30, "0").replace(/0+$/, "");
  return String(n / unit) + (fraction ? "." + fraction : "");
}
function elasticBytes(text) {
  if (!/^\d{1,12}(\.\d{1,30})?$/.test(text)) throw new Error("Enter a positive memory budget in GiB, using a decimal point.");
  const [whole, fraction = ""] = text.split("."), denominator = 10n ** BigInt(fraction.length);
  const bytes = (BigInt(whole) * denominator + BigInt(fraction || "0")) * 1073741824n / denominator;
  if (bytes <= 0n || bytes > (1n << 60n)) throw new Error("The memory budget is outside the supported range.");
  return bytes.toString();
}
function renderElastic(data) {
  $("elastic-policies").replaceChildren();
  $("elastic-status").textContent = data.policies.length ? "Saved policies, not a host health or throughput guarantee." :
    "No automatic replica management is configured. Choose Manage replicas on a managed model to review a policy.";
  for (const policy of data.policies) {
    const card = workspaceCard($("elastic-policies"), `${policy.source_model} · ${policy.id.slice(0, 12)}`, [
      ["Policy", policy.faulted ? "Operator review required" : policy.enabled ? "Enabled" : "Paused"],
      ["Phase", policy.phase.replaceAll("_", " ")],
      ["Registered replicas / limits", `${policy.replicas} / ${policy.min_replicas}–${policy.max_replicas}`],
      ["Reserved / maximum memory", `${gib(Number(policy.reserved_bytes))} / ${gib(Number(policy.max_reserved_bytes))}`],
      ["Reason", policy.reason.replaceAll("_", " ")],
      ["Pending operation", policy.pending_operation || "None"],
      ["Pending allocation", policy.pending_allocation || "None"],
      ["Last controller check", policy.last_check ? new Date(policy.last_check * 1000).toLocaleString() : "Not observed"]]);
    if (!management) continue;
    const actions = document.createElement("div"); actions.className = "allocation-actions";
    const buttons = [["configure", "Edit policy", policy.phase === "idle"],
      [policy.enabled ? "disable" : "resume", policy.enabled ? "Pause policy" : "Resume policy", policy.enabled || !policy.faulted],
      ["reconcile", "Reconcile preparation", policy.phase === "preparing"]];
    for (const [action, label, enabled] of buttons) {
      const button = document.createElement("button"); button.className = "text-button"; button.textContent = label;
      button.disabled = !enabled; button.dataset.allowed = String(enabled);
      button.onclick = () => action === "configure" ? openElasticEditor(policy.id) : controlElastic(policy.id, action);
      actions.append(button);
    }
    card.append(actions);
  }
  controls();
}
async function refreshElastic() {
  if (!workspaceOpen || !operator || !token || elasticPending || managementBusy || document.hidden) return;
  const epoch = workspaceEpoch, generation = elasticEpoch, credential = token;
  const valid = () => epoch === workspaceEpoch && generation === elasticEpoch &&
    credential === token && workspaceOpen && operator && !managementBusy;
  elasticPending = true;
  try {
    const data = await request("/api/v1/elastic-policy");
    if (valid()) renderElastic(data);
  } catch (error) {
    if (valid()) {
      $("elastic-policies").replaceChildren(); $("elastic-status").textContent = `Replica policies unavailable: ${error.message}`;
    }
  } finally { elasticPending = false; }
}
async function elasticTask(fn) {
  if (!management || managementBusy || !workspaceOpen) return;
  // A GET begun before an operator action must not restore stale controls or
  // overwrite its outcome, even when it completes after the POST response.
  elasticEpoch++;
  const epoch = workspaceEpoch, credential = token;
  const valid = () => epoch === workspaceEpoch && credential === token && workspaceOpen && management;
  managementBusy = true; controls(); let submitted = false;
  try {
    await fn({valid, submit: body => {
      if (!valid()) throw new Error("Workspace changed; review the policy again.");
      submitted = true; return request("/api/v1/elastic-policy", "POST", body);
    }});
  } catch (error) {
    if (valid()) $("elastic-status").textContent = error.message + (submitted ?
      " The operation may already have been applied. Refresh and inspect before retrying; no automatic retry was sent." : "");
  } finally { managementBusy = false; controls(); }
}
async function openElasticEditor(id) {
  return elasticTask(async ({valid}) => {
    closeElasticEditor();
    const workspace = await request("/api/v1/workspace"), policies = await request("/api/v1/elastic-policy");
    if (!valid()) return;
    const model = workspace.models.find(item => item.id === id), policy = policies.policies.find(item => item.id === id);
    if (!workspace.registry_ok || !workspace.inventory_ok || !model) throw new Error("Current model and computer reports are required before configuring replicas.");
    if (policy && policy.phase !== "idle") throw new Error("An operation still owns pending resources. Inspect or reconcile it before replacing its policy.");
    if (!Array.isArray(model.allocations)) throw new Error("The service did not provide exact replica identities. Update the complete runtime before editing.");
    if (policy && (!Array.isArray(policy.nodes) || !Array.isArray(policy.adopt_allocations) ||
        ![policy.context, policy.sessions, policy.max_new, policy.up_rejections, policy.ttft_ms].every(Number.isSafeInteger)))
      throw new Error("The service did not provide the complete saved policy. Update the complete runtime before editing.");
    const catalogue = await request("/api/v1/catalogue"); if (!valid()) return;
    elasticEditor = {model, policy, nodes: workspace.nodes};
    $("elastic-title").textContent = `Manage replicas · ${model.name}`;
    $("elastic-source").add(new Option("Choose a checkpoint source", ""));
    for (const source of catalogue.models) {
      const option = new Option(`${source.name}${source.source_ready ? "" : " · unavailable"}`, source.name);
      option.disabled = !source.source_ready; $("elastic-source").add(option);
    }
    if (policy) $("elastic-source").value = policy.source_model;
    const nodes = new Map(workspace.nodes.map(node => [node.id, node]));
    for (const id of policy?.nodes || []) if (!nodes.has(id)) nodes.set(id, {id, name: "Computer not reporting", ram_offered_bytes: null});
    elasticEditor.nodes = Array.from(nodes.values());
    for (const node of nodes.values()) {
      preparationCheck($("elastic-nodes"), node.id, `${node.name} · ${node.id.slice(0, 12)} · ${gib(node.ram_offered_bytes)} offered`);
      $("elastic-nodes").lastElementChild.querySelector("input").checked = Boolean(policy?.nodes.includes(node.id));
    }
    const allocations = model.allocations.map(id => workspace.allocations.find(a => a.id === id));
    const fields = {min: policy?.min_replicas ?? 1, max: policy?.max_replicas ?? Math.max(2, model.replicas),
      cooldown: policy?.cooldown_seconds ?? 60, idle: policy?.idle_seconds ?? 300,
      rejections: policy?.up_rejections ?? 3, ttft: policy?.ttft_ms ?? 0,
      context: policy?.context ?? model.context, sessions: policy?.sessions ?? allocations[0]?.session_limit ?? 1,
      output: policy?.max_new ?? model.max_tokens};
    for (const [name, value] of Object.entries(fields)) $("elastic-" + name).value = value;
    $("elastic-budget").value = policy ? elasticGiB(policy.max_reserved_bytes) : "";
    $("elastic-enabled").checked = policy?.enabled ?? false;
    $("elastic-form").hidden = false;
    $("elastic-status").textContent = "Choose the computers and budget explicitly. Review does not reserve memory or contact donors.";
    $("elastic-form").scrollIntoView({block: "nearest"});
  });
}
async function controlElastic(id, action) {
  return elasticTask(async ({valid, submit}) => {
    const data = await request("/api/v1/elastic-policy"); if (!valid()) return;
    const policy = data.policies.find(item => item.id === id);
    if (!policy) throw new Error("This policy no longer exists. Refresh the workspace.");
    const effects = {
      disable: "Pause new decisions. Keep pending reservations and prepared models. An already withdrawn replica continues its safe retirement.",
      resume: "Resume this saved policy and its exact pending operation. This does not clear faults or grant donor permission.",
      reconcile: "Inspect the exact preparation receipt. Only proven quiescent operations can be reconciled. Unknown allocations remain reserved; this does not force an unload."
    };
    if (!confirm(`${policy.source_model}\n\n${effects[action]}\n\nContinue?`)) return;
    const result = await submit({action, id, revision: policy.revision}); if (!valid()) return;
    closeElasticEditor();
    $("elastic-status").textContent = `Policy revision ${result.revision}: ${result.reason.replaceAll("_", " ")}. Refresh to inspect its current phase.`;
    $("elastic-policies").replaceChildren();
  });
}
$("elastic-form").oninput = invalidateElasticReview;
$("elastic-form").onsubmit = event => {
  event.preventDefault();
  if (!management || managementBusy || !elasticEditor || !workspaceOpen) return;
  invalidateElasticReview();
  try {
    const {model, policy, nodes} = elasticEditor;
    const selected = Array.from($("elastic-nodes").querySelectorAll("input:checked"), input => input.value);
    if (!selected.length) throw new Error("Choose at least one authorized computer.");
    const number = name => {
      const input = $("elastic-" + name), n = Number(input.value);
      if (!input.checkValidity() || !Number.isSafeInteger(n)) throw new Error("Review the integer limits in the form.");
      return n;
    };
    const body = {action: "configure", id: model.id, revision: policy?.revision || 0, route_revision: model.revision,
      enabled: $("elastic-enabled").checked, min_replicas: number("min"), max_replicas: number("max"),
      cooldown_seconds: number("cooldown"), idle_seconds: number("idle"), up_rejections: number("rejections"), ttft_ms: number("ttft"),
      max_reserved_bytes: elasticBytes($("elastic-budget").value.trim()), source_model: $("elastic-source").value,
      nodes: selected, context: number("context"), sessions: number("sessions"), max_new: number("output"),
      adopt_allocations: [...model.allocations]};
    if (!body.source_model || body.min_replicas > body.max_replicas || model.replicas > body.max_replicas ||
        body.context < model.context || body.max_new < model.max_tokens || body.max_new > body.context)
      throw new Error("Replica counts and context/output limits must preserve the existing model contract.");
    elasticReview = {body, name: model.name, epoch: workspaceEpoch, credential: token};
    workspaceCard($("elastic-review"), `Review · ${model.name}`, [
      ["Policy", body.enabled ? "Enable automatic replica management" : "Save paused; no new automatic decisions"],
      ["Replica limits", `${body.min_replicas}–${body.max_replicas}${body.min_replicas === 0 ? " · idle unloading allowed" : " · resident minimum"}`],
      ["Maximum reserved memory", `${elasticGiB(body.max_reserved_bytes)} GiB (${body.max_reserved_bytes} bytes)`],
      ["Checkpoint", body.source_model], ["Authorized computers", selected.map(id => nodes.find(n => n.id === id)?.name || id).join(", ")],
      ["Allocations adopted for possible retirement", body.adopt_allocations.join("\n") || "None — model is dormant"],
      ["Fences", `Policy ${body.revision} · model route ${body.route_revision}`]]);
    $("elastic-apply").hidden = false;
  } catch (error) { $("elastic-status").textContent = error.message; }
};
$("elastic-apply").onclick = () => elasticTask(async ({valid, submit}) => {
  const review = elasticReview;
  if (!review || review.epoch !== workspaceEpoch || review.credential !== token) return;
  if (!confirm(`${review.name}\n\nApply this reviewed policy? It can prepare replicas and retire the exact adopted allocations within your limits. Donor approval is still required. No cloud servers will be created.`)) return;
  invalidateElasticReview(); // A lost response must not trigger a blind resubmit.
  const saved = await submit(review.body); if (!valid()) return;
  closeElasticEditor(); $("elastic-policies").replaceChildren();
  $("elastic-status").textContent = `Policy saved at revision ${saved.revision}. ${saved.enabled ? "Automation enabled within the reviewed limits." : "Paused; no new automatic decisions."}`;
});
$("elastic-reload").onclick = () => elasticEditor && openElasticEditor(elasticEditor.model.id);
$("elastic-close").onclick = closeElasticEditor;
$("workspace-refresh").onclick = refreshWorkspace;
setInterval(refreshWorkspace, 10000);
setInterval(() => {
  if (workspaceOpen && !managementBusy && workspaceExpires && Date.now() >= workspaceExpires) {
    clearWorkspace(); $("workspace-status").textContent = "Machine reports expired — refreshing before showing resource values.";
    refreshWorkspace();
  }
}, 1000);
