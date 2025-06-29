// data/ui.js for Door Controller – PRODUCTION
let adminCode = null;
function codeFetch(url, opts, require=true) {
  opts = opts || {};
  if (require) {
    if (!adminCode) return requireCode(c=>{if(c) {adminCode=c; codeFetch(url,opts, false);} });
    opts.headers = Object.assign({}, opts.headers, {'X-Admin-Code': adminCode});
  }
  return fetch(url,opts).then(r=>{
    if (r.status==401) {adminCode=null;alert('Feil kode!');throw '401';} return r.json? r.json() : r; });
}
async function fetchJson(url, opts) {
  let r = await fetch(url,opts);
  return await r.json();
}

// STATUS
async function loadStatus() {
  let s = await fetchJson('/status');
  document.getElementById('status-albue').textContent = s.albue ? 'Trykket' : 'Ikke trykket';
  document.getElementById('status-door').textContent = s.door ? 'Åpen' : 'Lukket';
  document.getElementById('status-relay').textContent = s.relay ? 'På' : 'Av';
}
async function toggleRelay() {
  await codeFetch('/relay', {method:'POST'});
  await loadStatus();
}

// SETTINGS
async function loadSettings() {
  let s = await fetchJson('/settings');
  document.getElementById('device_id').value = s.device_id;
  document.getElementById('ssid').value = s.ssid;
  document.getElementById('relay_ms').value = s.relay_ms;
  document.getElementById('wiegand_mode').value = s.wiegand_mode;
  document.getElementById('admin_code').value = s.admin_code || '';
  document.getElementById('fwver').textContent = s.fw_version || '';
}
async function saveSettings() {
  let fd = new FormData(document.getElementById('settings-form'));
  let obj = {};
  fd.forEach((v,k)=>obj[k]=v);
  await codeFetch('/settings', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(obj)});
  alert('Innstillinger lagret, starter på nytt');
  return false;
}

// LOGG
async function loadLog() {
  let logs = await fetchJson('/log');
  let box = document.getElementById('logbox');
  box.innerHTML = logs.reverse().map(l=>l.replace(/</g,'&lt;')).join('<br>');
}
function downloadLog() {
  fetch('/log').then(r=>r.json()).then(logs=>{
    let blob = new Blob([logs.join('\n')], {type:'text/plain'});
    let a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='doorlog.txt';a.click();
  });
}

// USERS
async function loadUsers() {
  let users = await fetchJson('/users');
  let tbody = document.querySelector('#userstable tbody');
  tbody.innerHTML = '';
  users.forEach(u => {
    let tr = document.createElement('tr');
    tr.innerHTML = `<td>${u.username}</td><td>${u.tag}</td><td><button class='btn btn-danger' onclick='deleteUser(\"${u.username}\")'>Slett</button></td>`;
    tbody.appendChild(tr);
  });
}
async function addUser() {
  let username = document.getElementById('newuser').value;
  let tag = document.getElementById('newtag').value;
  await codeFetch('/users', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username,tag})});
  document.getElementById('newuser').value = '';
  document.getElementById('newtag').value = '';
  await loadUsers();
  return false;
}
async function deleteUser(username) {
  await codeFetch('/users/'+encodeURIComponent(username), {method:'DELETE'});
  await loadUsers();
}

// MQTT/Management
async function loadMQTT() {
  let s = await fetchJson('/settings');
  document.getElementById('mqtt_host').value = s.mqtt_host || '';
  document.getElementById('mqtt_port').value = s.mqtt_port || 1883;
  document.getElementById('mqtt_user').value = s.mqtt_user || '';
  document.getElementById('mqtt_pass').value = s.mqtt_pass || '';
}
async function saveMQTT() {
  let o={ mqtt_host:document.getElementById('mqtt_host').value, mqtt_port:document.getElementById('mqtt_port').value,
      mqtt_user:document.getElementById('mqtt_user').value, mqtt_pass:document.getElementById('mqtt_pass').value };
  await codeFetch('/settings', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});
  alert('MQTT lagret!');
  return false;
}

// RULES
async function loadRules() {
  let rules = await fetchJson('/rules');
  let tbody = document.querySelector('#rulestable tbody');
  tbody.innerHTML = '';
  rules.forEach((r,i) => {
    let tr = document.createElement('tr');
    tr.innerHTML = `<td>${r.name}</td>
      <td>${r.trigger}</td>
      <td>${r.param||''}</td>
      <td>${r.action}</td>
      <td>${r.actparam||''}</td>
      <td>
        <button class='btn' onclick='editRule(${i})'>Endre</button>
        <button class='btn btn-danger' onclick='deleteRule(${i})'>Slett</button>
      </td>`;
    tbody.appendChild(tr);
  });
}
function showRuleForm(idx) {
  document.getElementById('ruleformdiv').classList.remove('hide');
  if(idx!=null) { // Rediger
    fetchJson('/rules').then(rules=>{
      let r = rules[idx];
      document.getElementById('rname').value = r.name;
      document.getElementById('rtrigger').value = r.trigger;
      document.getElementById('rparam').value = r.param;
      document.getElementById('raction').value = r.action;
      document.getElementById('ractparam').value = r.actparam;
      document.getElementById('ruleidx').value = idx;
      document.getElementById('ruleformtitle').textContent = 'Endre regel';
    });
  } else { // Ny
    document.getElementById('ruleform').reset();
    document.getElementById('ruleidx').value = '';
    document.getElementById('ruleformtitle').textContent = 'Ny regel';
  }
}
function hideRuleForm() { document.getElementById('ruleformdiv').classList.add('hide'); }
async function saveRule() {
  let rule = {
    name: document.getElementById('rname').value,
    trigger: document.getElementById('rtrigger').value,
    param: document.getElementById('rparam').value,
    action: document.getElementById('raction').value,
    actparam: document.getElementById('ractparam').value
  };
  let idx = document.getElementById('ruleidx').value;
  if(idx) await codeFetch('/rules/'+idx, {method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(rule)});
  else await codeFetch('/rules', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(rule)});
  hideRuleForm(); await loadRules();
  return false;
}
async function deleteRule(idx) {
  await codeFetch('/rules/'+idx, {method:'DELETE'});
  await loadRules();
}
function onTriggerChange() {} // utvid ved behov

// Konsoll/Om
function loadAbout() {
  fetchJson('/settings').then(s=>{
    document.getElementById('about-fw').textContent = s.fw_version || '';
    document.getElementById('about-id').textContent = s.device_id || '';
  });
}
function loadConsole() { document.getElementById('console-out').textContent = 'Kommando-konsoll kommer!'; }
function sendConsole() { alert('Kommando-konsoll kommer!'); }

// INIT
window.onload = async () => {
  await loadSettings(); await loadStatus(); await loadUsers(); await loadLog(); loadAbout(); await loadRules();
  setInterval(loadStatus, 2000); setInterval(loadLog, 4000); setInterval(loadUsers, 12000); setInterval(loadRules,20000);
}
