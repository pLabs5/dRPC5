function refreshStatus() {
  return api('/api/status').then(function(s) {
    status = s;
    $('p-installed').textContent = 'tile: ' + (s.installed ? 'installed' : 'not installed');
    $('p-installed').className = 'pill ' + (s.installed ? 'ok' : 'bad');
    $('p-token').textContent = 'sign-in: ' + (s.has_token ? 'ready' : 'none');
    $('p-token').className = 'pill ' + (s.has_token ? 'ok' : '');
    $('btn-logout').className = 'act danger' + (s.has_token ? '' : ' hidden');
    var host = (s.ip && s.ip !== '?') ? s.ip : location.hostname;
    if (host) $('pc-url').textContent = 'http://' + host + ':' + (s.port || 8642) + '/pc.html';
    renderGateway(s.gateway);
    if (s.config) {
      if (!cfgFocused() && !saveTimer) {
        for (var k in s.config) {
          var el = $('c-' + k);
          if (!el) continue;
          if (el.type === 'checkbox') el.checked = s.config[k] === '1' || s.config[k] === 'true';
          else el.value = s.config[k];
        }
      }
      applyMode();
    }
  }).catch(function(){ $('p-net').className = 'pill bad'; $('p-net').textContent = 'server: down'; });
}

function applyMode() {
  var mode = $('c-mode') ? $('c-mode').value : 'auto';
  var card = $('manual-card');
  if (card) card.className = mode === 'manual' ? 'card' : 'card hidden';
  if ($('m-auto')) $('m-auto').className = mode === 'auto' ? 'active' : '';
  if ($('m-manual')) $('m-manual').className = mode === 'manual' ? 'active' : '';
}

function renderGateway(g) {
  var text, cls;
  if (!g) {
    text = 'presence: ?';
    cls = '';
    $('gw-state').textContent = 'presence: waiting';
  } else if (g.auth_failed) {
    text = 'presence: token rejected';
    cls = 'bad';
    $('gw-state').textContent = 'discord rejected the token - sign in again';
  } else if (g.rest) {
    text = 'presence: resting';
    cls = '';
    $('gw-state').textContent = 'presence: resting (console in standby)';
  } else if (g.ready) {
    text = 'presence: live';
    cls = 'ok';
    $('gw-state').textContent = 'presence: live' + (g.activity ? ' - ' + g.activity : '');
  } else if (g.connected) {
    text = 'presence: connecting';
    cls = '';
    $('gw-state').textContent = 'presence: connecting...';
  } else {
    text = 'presence: off';
    cls = '';
    $('gw-state').textContent = 'presence: off (enable it and save a token)';
  }
  $('p-gw').textContent = text;
  $('p-gw').className = 'pill ' + cls;
}

$('c-mode').onchange = applyMode;
$('m-auto').onclick = function() { $('c-mode').value = 'auto'; applyMode(); autoSave(); };
$('m-manual').onclick = function() { $('c-mode').value = 'manual'; applyMode(); autoSave(); };

function collectConfig() {
  var keys = ['app_id','name','type','state','details','asset_key','asset_text',
              'asset_small_key','asset_small_text','start_timestamp','end_timestamp',
              'status','mode','media_line',
              'poll_ms','hb_min_ms','psn_retry_ms','resync_ms','reidentify_ms'];
  var checks = ['enabled','src_game','src_media','src_app','src_idle',
                'show_artwork','show_platform','rest_mode'];
  var obj = {};
  keys.forEach(function(k){ var el = $('c-'+k); if (el) obj[k] = el.value; });
  checks.forEach(function(k){
    var el = $('c-'+k);
    if (el) obj[k] = el.checked ? '1' : '0';
  });
  return obj;
}

function clockStr() {
  var d = new Date();
  function p(n) { return (n < 10 ? '0' : '') + n; }
  return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
}

function pushConfig() {
  return post('/api/config', collectConfig()).then(function(r) {
    setMsg('cfg-msg', r.error ? r.error : 'saved ' + clockStr(),
           r.error ? 'err' : 'good');
    return r;
  }).catch(function(e) {
    setMsg('cfg-msg', 'save failed: ' + e.message, 'err');
    return { error: e.message };
  });
}

var saveTimer = null;
function autoSave() {
  setMsg('cfg-msg', 'saving...', '');
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(function() { saveTimer = null; pushConfig(); }, 600);
}

Array.prototype.forEach.call(
  document.querySelectorAll('#view-cfg input, #view-cfg select'),
  function(el) {
    el.addEventListener('input', autoSave);
    el.addEventListener('change', autoSave);
  }
);

$('btn-save').onclick = function() {
  setMsg('cfg-msg', 'saving...');
  pushConfig();
};

function showTab(which) {
  $('view-cfg').className = which === 'cfg' ? '' : 'hidden';
  $('view-sign').className = which === 'sign' ? '' : 'hidden';
  $('tab-cfg').className = which === 'cfg' ? 'active' : '';
  $('tab-sign').className = which === 'sign' ? 'active' : '';
}
$('tab-cfg').onclick = function(){ showTab('cfg'); };
$('tab-sign').onclick = function(){ showTab('sign'); };
