// Client-side validators for CGM secrets. Loaded by settings.html into
// window.SECRET_VALIDATORS before the page renders.
window.SECRET_VALIDATORS["llu_email"] = function(v) {
  var at = v.indexOf("@");
  return (at > 0 && v.indexOf(".", at + 1) > at + 1) ? null : "Not a valid email address";
};
