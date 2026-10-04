
function raSend(obj) {
  if (!raState) return Promise.reject('no session');
  return post('/api/ra/send', {id: raState.id, data: JSON.stringify(obj)});
}

function handleRa(frame) {
  var d;
  try { d = JSON.parse(frame); } catch (e) { return; }
  var op = d.op;
  if (op === 'hello') return;

  if (op === 'nonce_proof') {
    var enc = d.encrypted_nonce || (d.d && d.d.encrypted_nonce);
    if (!enc || !raState) return;
    oaepDecrypt(raState.keys.priv, enc).then(function(buf) {
      return raSend({op: 'nonce_proof', nonce: b64url(new Uint8Array(buf))});
    }).catch(function(e) {
      setMsg('qr-msg', 'nonce decrypt failed: ' + e.message, 'err');
    });
    return;
  }

  if (op === 'pending_remote_init') {
    var fp = d.fingerprint || (d.d && d.d.fingerprint) || '';
    setMsg('qr-msg', 'scan with the discord mobile app (settings \u2192 link with other devices)');
    try {
      var q = qrcode(0, 'M');
      q.addData('https://discord.com/ra/' + fp);
      q.make();
      $('qrbox').innerHTML = q.createImgTag(4, 8);
    } catch (e) {
      setMsg('qr-msg', 'qr render failed: ' + e.message, 'err');
    }
    return;
  }
  if (op === 'pending_ticket') {
    setMsg('qr-msg', 'scanned, waiting for confirmation...');
    return;
  }
  if (op === 'pending_login') {
    var ticket = d.ticket || (d.d && d.d.ticket);
    if (!ticket) {
      setMsg('qr-msg', 'no ticket in pending_login: ' + frame, 'err');
      return;
    }
    setMsg('qr-msg', 'exchanging login...');
    raSave(ticket);
    return;
  }
  if (op === 'cancel') {
    setMsg('qr-msg', 'cancelled on the phone', 'err');
    return;
  }
  if (op === '_closed') {
    if (raState) {
      post('/api/ra/close', {id: raState.id});
      raState = null;
    }
    return;
  }
  if (op === 'error') {
    setMsg('qr-msg', 'gateway error: ' + (d.message || d.code || 'unknown'), 'err');
    return;
  }
}

function bytesToText(bytes) {
  var s = '';
  var i;
  for (i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return s;
}

function oaepDecrypt(priv, b64) {
  return crypto.subtle.decrypt({name:'RSA-OAEP'}, priv, b64urlDecode(b64));
}

function raLoop() {
  if (!raState) return;
  var wait = 25000;
  api('/api/ra/poll?id=' + raState.id + '&wait=' + wait).then(function(r) {
    if (r.error) throw new Error(r.error);
    (r.frames || []).forEach(function(f) {
      handleRa(f);
    });
    if (raState) raLoop();
  }).catch(function(e) {
    setMsg('qr-msg', 'connection lost: ' + e.message, 'err');
    raState = null;
  });
}

$('btn-qr').onclick = function() {
  if (!cryptoOk()) { setMsg('qr-msg', 'webcrypto unavailable (needs secure context)', 'err'); return; }
  if (raState) { post('/api/ra/close', {id: raState.id}); raState = null; }
  setMsg('qr-msg', 'generating keys...');
  $('qrbox').innerHTML = '';
  $('btn-qr').disabled = true;
  crypto.subtle.generateKey(
    {name:'RSA-OAEP', modulusLength:2048, publicExponent:new Uint8Array([1,0,1]), hash:'SHA-256'},
    true, ['encrypt','decrypt']
  ).then(function(kp) {
    return crypto.subtle.exportKey('spki', kp.publicKey).then(function(spki) {
      var pk = new Uint8Array(spki);
      return crypto.subtle.digest('SHA-256', pk).then(function(dg) {
        var fp = b64url(new Uint8Array(dg));
        return post('/api/ra/start', {public_key: b64(pk)}).then(function(r) {
          if (r.error) throw new Error(r.error);
          raState = {id: r.id, keys: {pub: kp.publicKey, priv: kp.privateKey}, fp: fp};
          setMsg('qr-msg', 'waiting for gateway...');
          raLoop();
        });
      });
    });
  }).catch(function(e) {
    setMsg('qr-msg', 'start failed: ' + e.message, 'err');
  }).then(function() {
    $('btn-qr').disabled = false;
  });
};

