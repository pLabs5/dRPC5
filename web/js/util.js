var $ = function(id){ return document.getElementById(id); };
var status = null;
var raState = null;

function api(path, opts) {
  return fetch(path, opts).then(function(r){
    return r.json().catch(function(){ return {}; }).then(function(j){
      if (!r.ok && !j.error) j.error = 'http ' + r.status;
      return j;
    });
  });
}
function post(path, obj) {
  return api(path, {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(obj || {})});
}
function b64url(bytes) {
  var s = '';
  var i;
  for (i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s).replace(/\+/g,'-').replace(/\//g,'_').replace(/=+$/,'');
}
function b64(bytes) {
  var s = '';
  var i;
  for (i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s);
}
function b64urlDecode(str) {
  var s = str.replace(/-/g,'+').replace(/_/g,'/');
  while (s.length % 4) s += '=';
  var bin = atob(s);
  var out = new Uint8Array(bin.length);
  for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}
function textToBytes(t) {
  var out = new Uint8Array(t.length);
  for (var i = 0; i < t.length; i++) out[i] = t.charCodeAt(i) & 0xff;
  return out;
}
function setMsg(id, text, cls) {
  var el = $(id);
  el.textContent = text || '';
  el.className = 'msg' + (cls ? ' ' + cls : '');
}
function cryptoOk() {
  return !!(window.crypto && crypto.subtle && crypto.subtle.encrypt);
}

function cfgFocused() {
  var a = document.activeElement;
  return !!(a && a.id && a.id.indexOf('c-') === 0);
}

