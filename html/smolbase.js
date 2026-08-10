/* Shared helpers for smolbase pages (tickets #31/#36). ES5 syntax on purpose —
   the portal runs inside captive-portal webviews of unknown vintage.
   CONVENTION: only helpers used by 2+ pages live here (esc, the eye toggle,
   setMsg/toast, postSettings); page flows keep their own DOM rendering and
   error handling. Move something here when a second page needs it, not before.
   NOTE: the firmware-embedded pages (fallback portal, /recover) must stay
   self-contained and never reference this file. */
"use strict";

/* Inline status line: sets text on a .msg element; err=true colors it danger.
   Contract (ticket #41): inline messages are overwritten by the next attempt,
   cleared when their input changes, and never fade on a timer. */
function setMsg(el, text, err) {
  el.textContent = text;
  el.classList.toggle("err", !!err);
}

/* Floating toast (ticket #41): SUCCESS confirmations only — quick "Saved"
   feedback that autohides. Failures stay inline where the user will retry. */
var __toastEl = null;
var __toastTimer = null;
function toast(text) {
  if (!__toastEl) {
    __toastEl = document.createElement("div");
    __toastEl.className = "toast";
    document.body.appendChild(__toastEl);
  }
  __toastEl.textContent = text;
  __toastEl.classList.add("show");
  clearTimeout(__toastTimer);
  __toastTimer = setTimeout(function () { __toastEl.classList.remove("show"); }, 1600);
}

/* POST a flat {key: value} map to /api/settings; resolves with the parsed
   JSON reply, rejects on HTTP error. Used by settings.html (per-section save)
   and index.html (per-setting save). */
function postSettings(values) {
  return fetch("/api/settings", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(values),
  }).then(function (r) {
    if (!r.ok) throw new Error("HTTP " + r.status);
    return r.json();
  });
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, function (c) {
    return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
  });
}

/* Password reveal: any <button data-eye="inputId"> toggles that input between
   password/text. The icon shows the STATE — slashed eye = hidden, open eye =
   visible. Buttons ship with EYE_OFF as their initial innerHTML. */
var EYE = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M1 12s4-7 11-7 11 7 11 7-4 7-11 7-11-7-11-7z"/><circle cx="12" cy="12" r="3"/></svg>';
var EYE_OFF = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M17.94 17.94A10.9 10.9 0 0 1 12 19c-7 0-11-7-11-7a20.3 20.3 0 0 1 5.06-5.94"/><path d="M9.9 4.24A10.6 10.6 0 0 1 12 5c7 0 11 7 11 7a20.4 20.4 0 0 1-2.88 3.88"/><line x1="1" y1="1" x2="23" y2="23"/><path d="M14.12 14.12a3 3 0 1 1-4.24-4.24"/></svg>';

document.addEventListener("click", function (e) {
  var t = e.target.closest("[data-eye]");
  if (!t) return;
  var inp = document.getElementById(t.dataset.eye);
  inp.type = inp.type === "password" ? "text" : "password";
  var hidden = inp.type === "password";
  t.innerHTML = hidden ? EYE_OFF : EYE;
  t.setAttribute("aria-label", hidden ? "Show password" : "Hide password");
});
