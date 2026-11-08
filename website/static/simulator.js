// Station simulator: acts like the ESP32 station and calls the same API (website/api.py).
// Flow: tap card -> GET /api/next -> adjust with - / + -> ✓ -> POST /api/log
//       -> next set of the same exercise, or back to "Tap your card" (one tap per exercise).

const IDLE_AFTER_MS = 45000;  // clinic rule: back to "Tap your card" after 30-60 s with no presses
const MESSAGE_MS = 4000;      // how long short messages ("All done!") stay on screen
const MAX_REQUESTS_SHOWN = 20;

const lines = [1, 2, 3, 4].map((n) => document.getElementById(`line${n}`));
const cardInput = document.getElementById("card");
const requestList = document.getElementById("requests");

let card = null;     // card currently tapped in
let current = null;  // exercise on screen (a /api/next reply)
let value = null;    // reps (strength) or minutes (cardio) being adjusted
let timer = null;
let busy = false;    // ignore presses while waiting for the server

// ---------- screen ----------

function show(...text) {
  lines.forEach((line, i) => { line.textContent = text[i] ?? ""; });
}

function goIdleAfter(ms) {
  clearTimeout(timer);
  timer = setTimeout(idle, ms);
}

function idle() {
  clearTimeout(timer);
  card = null;
  current = null;
  value = null;
  show("Tap your card");
}

function showMessage(...text) {
  card = null;
  current = null;
  value = null;
  show(...text);
  goIdleAfter(MESSAGE_MS);
}

function showExercise(next) {
  current = next;
  if (next.type === "strength") value = next.reps ?? 0;
  else if (next.type === "cardio") value = next.minutes ?? 0;
  else value = null;
  render();
  goIdleAfter(IDLE_AFTER_MS);
}

function render() {
  const hi = `Hi ${current.first_name}!`;
  if (current.type === "strength") {
    const load = current.load_lb == null ? "body wt" : `${current.load_lb} lb`;
    show(hi, current.exercise, `Set ${current.set}/${current.sets}  ${load}`, `Reps: ${value}`);
  } else if (current.type === "cardio") {
    show(hi, current.exercise, "Cardio", `Minutes: ${value}`);
  } else {
    show(hi, current.exercise, "Mobility / stretch", "Press ✓ when done");
  }
}

// ---------- talking to the server ----------

async function call(method, url, body) {
  const options = { method };
  if (body) {
    options.headers = { "Content-Type": "application/json" };
    options.body = JSON.stringify(body);
  }
  let status = 0;
  let reply;
  try {
    const response = await fetch(url, options);
    status = response.status;
    reply = await response.json().catch(() => ({ error: "bad_reply", message: `Server error ${status}` }));
  } catch {
    reply = { error: "no_reply", message: "Couldn't reach the server." };
  }
  recordRequest(method, url, body, status, reply);
  return { status, reply };
}

function recordRequest(method, url, body, status, reply) {
  const sent = document.createElement("pre");
  sent.textContent = `${method} ${url}` + (body ? `\n${JSON.stringify(body)}` : "");
  const received = document.createElement("pre");
  received.textContent = `${status || "no reply"}  ${JSON.stringify(reply)}`;
  const item = document.createElement("li");
  item.append(sent, received);
  requestList.prepend(item);
  while (requestList.children.length > MAX_REQUESTS_SHOWN) requestList.lastElementChild.remove();
}

// ---------- buttons ----------

async function tap(cardId) {
  if (busy || !cardId) return;
  busy = true;
  show("Reading card...");
  const { status, reply } = await call("GET", `/api/next?card=${encodeURIComponent(cardId)}`);
  busy = false;

  if (status === 200 && !reply.done) {
    card = cardId;
    showExercise(reply);
  } else if (status === 200) {
    showMessage(
      `Hi ${reply.first_name}!`,
      reply.no_plan ? "No exercises yet." : "All done for today!",
      reply.no_plan ? "Please see staff." : "Great work.",
    );
  } else if (status === 404) {
    showMessage("Card not recognized.", "Please see staff.");
  } else {
    showMessage("Something went wrong.", reply.message ?? `Error ${status}`);
  }
}

function adjust(change) {
  if (busy || !current || value === null) return;
  value = Math.max(0, value + change);
  render();
  goIdleAfter(IDLE_AFTER_MS);
}

async function confirmSet() {
  if (busy || !current) return;
  busy = true;
  const body = { card, dosage_id: current.dosage_id, set: current.set };
  if (current.type === "strength") {
    body.reps = value;
    body.load_lb = current.load_lb;
  } else if (current.type === "cardio") {
    body.minutes = value;
  }

  const saved = await call("POST", "/api/log", body);
  if (saved.status !== 200) {
    busy = false;
    show("Couldn't save.", saved.reply.message ?? `Error ${saved.status}`, "", "Press ✓ to try again");
    goIdleAfter(IDLE_AFTER_MS);
    return;
  }

  // More sets of the same exercise: show the next one now. Otherwise the patient taps again later.
  const finished = current;
  const { status, reply } = await call("GET", `/api/next?card=${encodeURIComponent(card)}`);
  busy = false;
  if (status === 200 && !reply.done && reply.dosage_id === finished.dosage_id) {
    showExercise(reply);
  } else if (status === 200 && reply.done) {
    showMessage("Saved ✓", `${finished.exercise} done.`, "All done for today!");
  } else {
    showMessage("Saved ✓", `${finished.exercise} done.`, "Tap again for the next one.");
  }
}

document.getElementById("minus").addEventListener("click", () => adjust(-1));
document.getElementById("plus").addEventListener("click", () => adjust(1));
document.getElementById("ok").addEventListener("click", confirmSet);
document.getElementById("tap-form").addEventListener("submit", (event) => {
  event.preventDefault();
  tap(cardInput.value.trim());
});
document.querySelectorAll("[data-card]").forEach((button) => {
  button.addEventListener("click", () => {
    cardInput.value = button.dataset.card;
    tap(button.dataset.card);
  });
});

idle();
