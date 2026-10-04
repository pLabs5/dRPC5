var captchaSitekey = null;
var captchaRqdata = null;
var captchaWidget = null;
var captchaTarget = null;
var captchaMsgId = 'login-msg';
var captchaRetry = null;

function loadCaptcha(sitekey, rqdata, boxId, msgId, retry) {
  captchaSitekey = sitekey;
  captchaRqdata = rqdata || null;
  captchaMsgId = msgId || 'login-msg';
  captchaRetry = retry || null;
  captchaWidget = null;
  captchaTarget = (boxId || 'cap') + '-target';
  $(boxId || 'cap').className = '';
  $(boxId || 'cap').innerHTML = '<div id="' + captchaTarget + '"></div>';
  if (window.hcaptcha) { renderCaptcha(); return; }
  window.__onHCaptcha = renderCaptcha;
  var s = document.createElement('script');
  s.src = 'https://js.hcaptcha.com/1/api.js?render=explicit&onload=__onHCaptcha';
  s.async = true;
  s.onerror = function() {
    setMsg(captchaMsgId, 'captcha could not load in this browser - paste a token instead', 'err');
  };
  document.head.appendChild(s);
}
function renderCaptcha() {
  if (!window.hcaptcha || captchaWidget) return;
  try {
    var opts = {sitekey: captchaSitekey,
                callback: function() { if (captchaRetry) captchaRetry(); }};
    if (captchaRqdata) opts.rqdata = captchaRqdata;
    captchaWidget = hcaptcha.render(captchaTarget, opts);
  } catch (e) {
    setMsg(captchaMsgId, 'captcha failed to load: ' + e.message, 'err');
  }
}
function captchaPayload(p) {
  if (captchaSitekey && window.hcaptcha && captchaWidget !== null) {
    try { p.captcha_key = [hcaptcha.getResponse(captchaWidget)]; } catch (e) {}
    if (captchaRqdata) p.captcha_rqdata = captchaRqdata;
  }
  return p;
}

var lastHcHost = '';

function hcHost(tok) {
  try {
    var raw = tok.indexOf('P1_') === 0 ? tok.slice(3) : tok;
    var parts = raw.split('.');
    var seg = parts[1] || parts[0];
    var txt = bytesToText(b64urlDecode(seg)).replace(/[\x00-\x1f]/g, '.');
    var o = null;
    try { o = JSON.parse(txt); } catch (e) {}
    if (o) return o.host || o.hostname || ('keys=' + Object.keys(o).join(','));
    return 'raw=' + txt.slice(0, 90);
  } catch (e) { return 'decode failed: ' + e.message; }
}

function raLogin(ticket, caps) {
  var payload = {ticket: ticket};
  if (caps && caps.rqdata) payload.captcha_rqdata = caps.rqdata;
  var req = {method: 'POST',
             path: '/api/v9/users/@me/remote-auth/login',
             body: JSON.stringify(payload)};
  if (caps && caps.key) {
    req.captcha_key = caps.key;
    if (caps.session) req.captcha_session_id = caps.session;
    if (caps.rqtoken) req.captcha_rqtoken = caps.rqtoken;
  }
  return post('/api/discord', req).then(function(r) {
    if (r.error) throw new Error(r.error);
    var body = JSON.parse(r.body);
    if (body.captcha_sitekey) {
      if (caps && caps.key) {
        setMsg('qr-msg', 'captcha rejected (' + r.status + '): ' +
               JSON.stringify(body.captcha_key) + ' (token host=' + lastHcHost + ')',
               'err');
      } else {
        setMsg('qr-msg', 'captcha required - solve it below and this continues automatically', '');
      }
      loadCaptcha(body.captcha_sitekey, body.captcha_rqdata, 'qrcap', 'qr-msg',
                  function() {
        var tok = null;
        try { tok = hcaptcha.getResponse(captchaWidget); } catch (e) {}
        if (!tok) { setMsg('qr-msg', 'solve the checkbox first', 'err'); return; }
        lastHcHost = hcHost(tok);
        setMsg('qr-msg', 'solved (host=' + lastHcHost + ') - replaying...', '');
        setTimeout(function() {
          raLogin(ticket, {key: tok,
                           rqdata: body.captcha_rqdata,
                           session: body.captcha_session_id,
                           rqtoken: body.captcha_rqtoken});
        }, 2500);
      });
      return null;
    }
    if (!body.encrypted_token)
      throw new Error('no encrypted token (' + r.status + '): ' + r.body);
    return raState.keys.priv.decrypt(
      {name:'RSA-OAEP'},
      b64urlDecode(body.encrypted_token));
  }).then(function(buf) {
    return buf ? bytesToText(new Uint8Array(buf)) : null;
  });
}

function raSave(ticket) {
  raLogin(ticket).then(function(token) {
    if (!token) return;
    return post('/api/token', {token: token}).then(function(r) {
      if (r.error) throw new Error(r.error);
      setMsg('qr-msg', 'signed in. token stored.', 'good');
      $('qrbox').innerHTML = '';
refreshStatus();
      if (raState) { post('/api/ra/close', {id: raState.id}); raState = null; }
    });
  }).catch(function(e) {
    setMsg('qr-msg', 'qr login failed: ' + e.message, 'err');
  });
}

function doLogin() {
  var login = $('l-login').value;
  var pass = $('l-pass').value;
  var mfa = $('l-mfa').value;
  var useMfa = !$('mfa-wrap').className.match(/hidden/);
  var payload;

  if (!login || (!pass && !useMfa)) { setMsg('login-msg', 'enter email and password', 'err'); return; }
  setMsg('login-msg', 'signing in...');

  if (useMfa) {
    payload = {code: mfa, ticket: raState && raState.ticket ? raState.ticket : (window.__ticket || ''), sms: false, gift_code_sku_id: null};
    post('/api/discord', {method:'POST', path:'/api/v9/auth/mfa/totp', body: JSON.stringify(payload)})
      .then(mfaDone).catch(function(e){ setMsg('login-msg', String(e.message || e), 'err'); });
    return;
  }

  payload = {login: login, password: pass, undelete: false, captcha_key: null, login_source: null, gift_code_sku_id: null};
  captchaPayload(payload);
  post('/api/discord', {method:'POST', path:'/api/v9/auth/login', body: JSON.stringify(payload)})
    .then(loginDone)
    .catch(function(e){ setMsg('login-msg', String(e.message || e), 'err'); });
}

function loginDone(r) {
  if (r.error) { setMsg('login-msg', r.error, 'err'); return finishLogin(); }
  var body;
  try { body = JSON.parse(r.body); } catch (e) { setMsg('login-msg', 'bad response', 'err'); return finishLogin(); }
  if (body.token) return saveToken(body.token, 'login-msg');
  if (body.ticket && (body.mfa || r.status === 400)) {
    window.__ticket = body.ticket;
    $('mfa-wrap').className = '';
    setMsg('login-msg', 'enter your 2fa code', '');
    return finishLogin();
  }
  if (body.captcha_sitekey) {
    loadCaptcha(body.captcha_sitekey, body.captcha_rqdata);
    setMsg('login-msg', 'solve the captcha then sign in again', '');
    return finishLogin();
  }
  setMsg('login-msg', body.message || ('login failed (' + r.status + ')'), 'err');
  finishLogin();
}

function mfaDone(r) {
  if (r.error) { setMsg('login-msg', r.error, 'err'); return finishLogin(); }
  var body;
  try { body = JSON.parse(r.body); } catch (e) { setMsg('login-msg', 'bad response', 'err'); return finishLogin(); }
  if (body.token) return saveToken(body.token, 'login-msg');
  setMsg('login-msg', body.message || ('2fa failed (' + r.status + ')'), 'err');
  finishLogin();
}

function finishLogin() {
  $('btn-login').disabled = false;
}

function saveToken(token, msgId) {
  return post('/api/token', {token: token}).then(function(r) {
    if (r.error) { setMsg(msgId, r.error, 'err'); return; }
    setMsg(msgId, 'signed in. token stored.', 'good');
    $('l-pass').value = '';
    $('t-paste').value = '';
    refreshStatus();
  });
}

$('btn-login').onclick = function() {
  $('btn-login').disabled = true;
  doLogin();
};

$('btn-paste').onclick = function() {
  var t = $('t-paste').value.trim();
  if (t.length < 20) { setMsg('token-msg', 'that does not look like a token', 'err'); return; }
  saveToken(t, 'token-msg');
};

$('btn-logout').onclick = function() {
  post('/api/token/delete').then(function() {
    setMsg('token-msg', 'token deleted', '');
    refreshStatus();
  });
};

var OAUTH_CLIENT_ID = '1554245082281017386';
var OAUTH_REDIRECT = 'http://localhost:8642/callback';

function oauthCapture() {
  var h = window.location.hash || '';
  if (h.indexOf('access_token=') < 0) return;
  var tok = '', err = '';
  h.replace(/^#/, '').split('&').forEach(function(kv) {
    var p = kv.split('='), k = p[0];
    var v = decodeURIComponent(p.slice(1).join('=') || '');
    if (k === 'access_token') tok = v;
    if (k === 'error' || k === 'error_description') err = err ? err + ' ' + v : v;
  });
  try { history.replaceState(null, '', window.location.pathname); } catch(e) {}
  showTab('sign');
  if (err) { setMsg('oauth-msg', 'discord said: ' + err, 'err'); return; }
  if (!tok) { setMsg('oauth-msg', 'no access token in redirect', 'err'); return; }
  setMsg('oauth-msg', 'got token, saving...', '');
  post('/api/token', {token: tok}).then(function(r) {
    if (r.error) throw new Error(r.error);
    setMsg('oauth-msg', 'signed in. token stored.', 'good');
    var back = function() { showTab('sign'); };
    refreshStatus().then(back, back);
  }).catch(function(e) {
    setMsg('oauth-msg', 'failed to save token: ' + e.message, 'err');
  });
}

function oauthStart() {
  var url = 'https://discord.com/oauth2/authorize?client_id=' + OAUTH_CLIENT_ID +
            '&response_type=token&redirect_uri=' + encodeURIComponent(OAUTH_REDIRECT) +
            '&scope=identify';
  setMsg('oauth-msg', 'opening discord.com/authorize ...', '');
  setTimeout(function() { window.location.assign(url); }, 1200);
}

$('btn-oauth').onclick = function() { oauthStart(); };

refreshStatus();
oauthCapture();
setInterval(function() { refreshStatus(); }, 5000);
