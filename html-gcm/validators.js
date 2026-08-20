// Client-side validators for CGM secrets. Loaded by settings.html into
// window.SECRET_VALIDATORS before the page renders.
window.SECRET_VALIDATORS["llu_email"] = function(v) {
  return /^[a-zA-Z0-9._%+\-]+@[a-zA-Z0-9.\-]+\.[a-zA-Z]{2,}$/.test(v.trim())
    ? null : "Not a valid email address";
};
