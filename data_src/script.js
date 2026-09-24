const $=s=>document.querySelector(s),$$=s=>document.querySelectorAll(s);
const REL='https://github.com/martijnrenkema/NFC-reader/releases';
let tags={};            // uid -> registered name
let lastUid='',lastSince=null,formsFilled=false;

// ---------- helpers ----------
let toastTimer;
function toast(msg,bad,action){
    const t=$('#toast');
    t.textContent=msg;
    if(action){
        const b=document.createElement('button');
        b.textContent=action.label;
        b.onclick=()=>{t.classList.add('hidden');action.run()};
        t.appendChild(b);
    }
    t.className='toast'+(bad?' bad':'');
    clearTimeout(toastTimer);
    toastTimer=setTimeout(()=>t.classList.add('hidden'),action?6000:3500);
}
async function post(url,params,method){
    const r=await fetch(url,{method:method||'POST',body:params?new URLSearchParams(params):undefined});
    let d={};
    try{d=await r.json()}catch(e){}
    if(!r.ok)throw new Error(d.error||('HTTP '+r.status));
    return d;
}
async function getJSON(url){
    const r=await fetch(url);
    return r.json();
}
const esc=s=>String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
function ago(s){
    if(s<5)return'Just now';
    if(s<60)return s+' sec ago';
    if(s<3600)return Math.floor(s/60)+' min ago';
    if(s<86400)return Math.floor(s/3600)+' h ago';
    return Math.floor(s/86400)+' d ago';
}

// ---------- views (hash routing) ----------
function showView(){
    const v=(location.hash||'#reader').slice(1);
    const name=['reader','tags','settings','firmware'].includes(v)?v:'reader';
    $$('.view').forEach(el=>el.classList.toggle('hidden',el.id!=='v-'+name));
    $$('.tabs a').forEach(a=>{
        const on=a.dataset.view===name;
        a.classList.toggle('on',on);
        if(on)a.setAttribute('aria-current','page');else a.removeAttribute('aria-current');
    });
    if(name==='firmware')fetchUpdateStatus();
    if(name==='tags')fetchTags();
    window.scrollTo(0,0);
}
window.addEventListener('hashchange',showView);
showView();

// ---------- status polling (2 s so scans show up quickly) ----------
let pollInterval=setInterval(fetchStatus,2000);
document.addEventListener('visibilitychange',()=>{
    if(document.hidden){
        clearInterval(pollInterval);
        pollInterval=null;
    }else{
        if(!pollInterval)pollInterval=setInterval(fetchStatus,2000);
        fetchStatus();
    }
});
fetchTags().then(()=>{fetchStatus();fetchScanHistory()});

async function fetchStatus(){
    try{update(await getJSON('/api/status'))}catch(e){console.error(e)}
}

function update(d){
    if(d.wifi){
        const w=d.wifi;
        $('#wifi-dot').classList.toggle('on',!!(w.connected||w.ap_mode));
        $('#wifi-label').textContent=w.ap_mode?'Setup AP':'WiFi';
        $('#wifi-sum').textContent=w.ap_mode?'Setup access point active'
            :w.connected?'Connected to '+w.ssid+' · '+w.ip:'Not connected';
        if(w.rssi)$('#rssi').textContent=w.rssi;
    }
    if(d.mqtt){
        const m=d.mqtt;
        $('#mqtt-dot').classList.toggle('on',m.connected);
        $('#mqtt-sum').textContent=m.connected?'Connected to '+m.host+':'+m.port
            :m.host?'Not connected ('+m.host+')':'Not configured';
    }
    if(d.device){
        const v=d.device;
        $$('.version').forEach(el=>el.textContent='v'+v.version);
        $('#dev-name').textContent=v.name||'NFC Reader';
        $('#name-sum').textContent=v.name||'--';
        $('#mac').textContent=v.mac||'--';
        const nm=$('#night-mode');
        if(!nm._busy)nm.checked=v.night_mode===true;
    }
    // Fill forms once, so polling never overwrites what the user is typing
    if(!formsFilled){
        formsFilled=true;
        if(d.wifi&&d.wifi.ssid&&!d.wifi.ap_mode)$('#w-ssid').value=d.wifi.ssid;
        if(d.mqtt&&d.mqtt.host){$('#m-host').value=d.mqtt.host;$('#m-port').value=d.mqtt.port}
        if(d.device)$('#device-name').value=d.device.name||'';
    }
    if(d.nfc)updateNfc(d.nfc);
    if(d.system)updateSystem(d.system,d.wifi);
}

function duration(s){
    const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
    return d?d+' d '+h+' h':h?h+' h '+m+' min':m+' min';
}
const kb=b=>(b/1024).toFixed(0)+' KB';
function updateSystem(s,w){
    $('#diag-sum').textContent='Up '+duration(s.uptime)+' · last reset: '+s.reset_reason;
    $('#d-uptime').textContent=duration(s.uptime);
    $('#d-reset').textContent=s.reset_reason;
    $('#d-heap').textContent=kb(s.free_heap);
    $('#d-minheap').textContent=kb(s.min_free_heap);
    $('#d-nfc').textContent=s.nfc_reconnects;
    $('#d-ip').textContent=w&&w.ip||'--';
}

function updateNfc(n){
    const ok=n.connected;
    $('#nfc-title').textContent=ok?'Ready to scan':'Not responding · retrying';
    const b=$('#nfc-badge');
    b.textContent=ok?'Connected':'Error';
    b.className='badge'+(ok?' ok':' wait');

    const uid=n.last_uid||'';
    const since=n.time_since_scan;
    // New scan: another UID, or the time since the last scan went down
    const isNew=uid&&(uid!==lastUid||(lastSince!==null&&since<lastSince));
    const first=lastSince===null;
    lastSince=uid?since:null;
    if(isNew){
        const r=$('#reader');
        r.classList.remove('flash');void r.offsetWidth;r.classList.add('flash');
        fetchScanHistory();
        if(!first&&!tags[uid])toast('Unknown tag '+uid,false,{label:'Register',run:()=>registerUid(uid)});
    }
    lastUid=uid;

    $('#reader').classList.toggle('on',!!n.tag_present);
    $('#tag-state').textContent=n.tag_present?'Tag on reader':'Last scanned tag';
    $('#last-uid').textContent=uid||'--';
    $('#last-name').textContent=uid?(tags[uid]||'Not registered'):'';
    $('#last-name').classList.toggle('muted',!!uid&&!tags[uid]);
    $('#scan-time').textContent=uid?'Scanned '+ago(since).toLowerCase():'Hold a tag near the reader';
    $('#copy-uid').disabled=!uid;
    $('#register-last').classList.toggle('hidden',!uid||!!tags[uid]);
}

// ---------- reader actions ----------
function copyText(text){
    if(navigator.clipboard&&window.isSecureContext)return navigator.clipboard.writeText(text).then(()=>true,()=>false);
    // Fallback for plain HTTP, where the clipboard API is not available
    const ta=document.createElement('textarea');
    ta.value=text;ta.style.position='fixed';ta.style.opacity='0';
    document.body.appendChild(ta);ta.select();
    let ok=false;
    try{ok=document.execCommand('copy')}catch(e){}
    ta.remove();
    return Promise.resolve(ok);
}
$('#copy-uid').onclick=async()=>{
    if(!lastUid)return;
    toast(await copyText(lastUid)?'UID copied':'Could not copy UID',false);
};
function registerUid(uid){
    $('#tag-uid').value=uid;
    location.hash='#tags';
    setTimeout(()=>$('#tag-name').focus(),50);
}
$('#register-last').onclick=()=>registerUid(lastUid);

$('#night-mode').onchange=async function(){
    this._busy=true;
    try{
        const d=await post('/api/night_mode',{enabled:this.checked?'true':'false'});
        this.checked=d.night_mode;
        toast('Night mode '+(d.night_mode?'on':'off'));
    }catch(err){
        this.checked=!this.checked;
        toast('Could not change night mode',true);
    }
    setTimeout(()=>{this._busy=false},1000);
};

// ---------- scan history ----------
async function fetchScanHistory(){
    try{renderScanHistory((await getJSON('/api/scans')).scans||[])}
    catch(e){$('#scan-history').innerHTML='<div class="empty">Could not load scans</div>'}
}
function renderScanHistory(scans){
    const c=$('#scan-history');
    if(!scans.length){c.innerHTML='<div class="empty">No scans yet</div>';return}
    c.innerHTML=scans.map(s=>{
        const name=tags[s.uid];
        return '<div class="scan"><span class="ico"><svg class="i"><use href="#i-tag"/></svg></span>'+
            '<span class="grow"><b>'+esc(name||'Unknown tag')+'</b><span class="muted sm mono">'+esc(s.uid)+'</span></span>'+
            '<span class="t">'+ago(s.ago)+'</span></div>';
    }).join('');
}
$('#clear-history').onclick=async()=>{
    if(!confirm('Clear the scan history?'))return;
    try{await post('/api/scans',null,'DELETE');fetchScanHistory()}catch(e){toast('Could not clear history',true)}
};

// ---------- tag registry ----------
async function fetchTags(){
    try{
        const d=await getJSON('/api/tags');
        tags={};
        (d.tags||[]).forEach(t=>tags[t.uid]=t.name);
        $('#tag-count').textContent='· '+(d.count||0)+' of '+d.max;
        renderTags(d.tags||[]);
    }catch(e){
        $('#registered-tags').innerHTML='<div class="row static"><span class="muted">Could not load tags</span></div>';
    }
}
function renderTags(list){
    const c=$('#registered-tags');
    if(!list.length){
        c.innerHTML='<div class="row static"><span class="grow"><b>No tags yet</b><span class="muted sm">Scan a tag and register it above.</span></span></div>';
        return;
    }
    c.innerHTML=list.map(t=>
        '<div class="row static" data-uid="'+esc(t.uid)+'"><button class="edit" aria-label="Rename '+esc(t.name)+'"><span class="ico"><svg class="i"><use href="#i-tag"/></svg></span>'+
        '<span class="grow"><b>'+esc(t.name)+'</b><span class="muted sm mono">'+esc(t.uid)+'</span></span></button>'+
        '<button class="del" aria-label="Delete '+esc(t.name)+'"><svg class="i"><use href="#i-trash"/></svg></button></div>'
    ).join('');
    c.querySelectorAll('.edit').forEach(btn=>btn.onclick=()=>editTag(btn.parentElement.dataset.uid));
    c.querySelectorAll('.del').forEach(btn=>btn.onclick=async()=>{
        const uid=btn.parentElement.dataset.uid;
        if(!confirm('Delete tag "'+tags[uid]+'"?'))return;
        try{
            await post('/api/tags?uid='+encodeURIComponent(uid),null,'DELETE');
            toast('Tag deleted');
            fetchTags();
        }catch(err){toast(err.message,true)}
    });
}
// Clicking a registered tag loads it into the form to rename it
function editTag(uid){
    $('#tag-uid').value=uid;
    $('#tag-name').value=tags[uid]||'';
    $('#tag-form-title').textContent='Rename tag';
    $('#tag-submit').textContent='Save';
    $('#tag-cancel').classList.remove('hidden');
    $('#use-last-uid').disabled=true;
    window.scrollTo({top:0,behavior:'smooth'});
    $('#tag-name').focus();
}
function resetTagForm(){
    $('#tag-uid').value='';$('#tag-name').value='';
    $('#tag-form-title').textContent='Register a tag';
    $('#tag-submit').textContent='Register tag';
    $('#tag-cancel').classList.add('hidden');
    $('#use-last-uid').disabled=false;
}
$('#tag-cancel').onclick=resetTagForm;
$('#use-last-uid').onclick=()=>{
    if(!lastUid){toast('No tag scanned yet',true);return}
    $('#tag-uid').value=lastUid;
    $('#tag-name').focus();
};
$('#tag-form').onsubmit=async e=>{
    e.preventDefault();
    const uid=$('#tag-uid').value.trim(),name=$('#tag-name').value.trim();
    if(!uid){toast('Scan a tag first, then press "Use last"',true);return}
    if(!name){toast('Enter a name for the tag',true);$('#tag-name').focus();return}
    const renaming=!!tags[uid];
    try{
        await post('/api/tags',{uid,name});
        resetTagForm();
        toast(renaming?'Tag renamed · Home Assistant updated':'Tag registered · trigger added to Home Assistant');
        await fetchTags();
        const row=$('#registered-tags [data-uid="'+CSS.escape(uid)+'"]');
        if(row)row.classList.add('new');
    }catch(err){toast(err.message,true)}
};
// Export / import the registry as JSON (e.g. as a backup before a factory reset)
$('#export-tags').onclick=()=>{
    const list=Object.keys(tags).map(uid=>({uid,name:tags[uid]}));
    const blob=new Blob([JSON.stringify({tags:list},null,2)],{type:'application/json'});
    const a=document.createElement('a');
    a.href=URL.createObjectURL(blob);
    a.download='nfc-tags.json';
    a.click();
    setTimeout(()=>URL.revokeObjectURL(a.href),1000);
};
$('#import-tags').onclick=()=>$('#import-file').click();
$('#import-file').onchange=async function(){
    const f=this.files[0];
    this.value='';
    if(!f)return;
    let list;
    try{
        const d=JSON.parse(await f.text());
        list=(Array.isArray(d)?d:d.tags||[]).filter(t=>t&&typeof t.uid==='string'&&typeof t.name==='string');
    }catch(e){toast('Not a valid tag export file',true);return}
    if(!list.length){toast('No tags found in this file',true);return}
    if(!confirm('Import '+list.length+' tag(s)? Tags with the same UID are renamed.'))return;
    let ok=0,failed=0;
    for(const t of list){
        try{await post('/api/tags',{uid:t.uid,name:t.name});ok++}catch(e){failed++}
    }
    await fetchTags();
    toast('Imported '+ok+' tag(s)'+(failed?' · '+failed+' failed':''),failed>0);
};
$('#cleanup-form').onsubmit=async e=>{
    e.preventDefault();
    try{
        await post('/api/cleanup_trigger',{uid:$('#cleanup-uid').value.trim()});
        $('#cleanup-uid').value='';
        toast('Trigger removed from Home Assistant');
    }catch(err){toast(err.message,true)}
};

// ---------- settings ----------
$('#wifi-form').onsubmit=async e=>{
    e.preventDefault();
    try{
        const d=await post('/api/wifi',{ssid:$('#w-ssid').value,password:$('#w-pass').value});
        toast(d.message||'Saved');
    }catch(err){toast(err.message,true)}
};
$('#mqtt-form').onsubmit=async e=>{
    e.preventDefault();
    try{
        const d=await post('/api/mqtt',{host:$('#m-host').value,port:$('#m-port').value,user:$('#m-user').value,password:$('#m-pass').value});
        toast(d.message||'Saved');
    }catch(err){toast(err.message,true)}
};
$('#device-form').onsubmit=async e=>{
    e.preventDefault();
    const name=$('#device-name').value.trim();
    try{
        await post('/api/device',{name});
        $('#name-sum').textContent=name;
        toast('Name saved');
    }catch(err){toast(err.message,true)}
};

async function fetchPasswords(){
    try{
        const d=await getJSON('/api/passwords');
        $('#pass-sum').textContent='OTA '+(d.ota_custom?'custom':'default')+' · Access point '+(d.ap_custom?'custom':'default');
    }catch(e){}
}
fetchPasswords();
$('#pass-form').onsubmit=async e=>{
    e.preventDefault();
    const p={};
    if($('#p-ota').value)p.ota_password=$('#p-ota').value;
    if($('#p-ap').value)p.ap_password=$('#p-ap').value;
    if(!p.ota_password&&!p.ap_password){toast('Enter at least one new password',true);return}
    try{
        const d=await post('/api/passwords',p);
        $('#p-ota').value='';$('#p-ap').value='';
        toast(d.message||'Saved');
        fetchPasswords();
    }catch(err){toast(err.message,true)}
};

$('#restart').onclick=async()=>{
    if(!confirm('Restart the NFC reader?'))return;
    try{await post('/api/restart');toast('Restarting...')}catch(err){toast(err.message,true)}
};
$('#reset').onclick=async()=>{
    if(!confirm('Reset all settings, including registered tags? This cannot be undone.'))return;
    try{await post('/api/reset');toast('Resetting · connect to the NFC-READER access point afterwards')}catch(err){toast(err.message,true)}
};

// ---------- system log ----------
async function fetchLogs(){
    try{renderLogs(await getJSON('/api/logs'))}
    catch(e){$('#logs-container').innerHTML='<div class="log">Could not load logs</div>'}
}
function renderLogs(logs){
    const c=$('#logs-container');
    if(!logs||!logs.length){c.innerHTML='<div class="log">No log entries</div>';return}
    c.innerHTML=logs.slice().reverse().map(l=>
        '<div class="log '+esc(l.l.toLowerCase())+'"><span class="t">'+formatLogTime(l)+'</span><span class="l">'+esc(l.l)+'</span><span>'+esc(l.m)+'</span></div>'
    ).join('');
}
function formatLogTime(l){
    if(l.e>0){
        const d=new Date(l.e*1000),now=new Date();
        const t=d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'});
        if(d.toDateString()===now.toDateString())return t;
        if(new Date(now-86400000).toDateString()===d.toDateString())return'Yesterday '+t;
        return d.toLocaleDateString([],{day:'2-digit',month:'2-digit'})+' '+t;
    }
    // No NTP time yet: show uptime
    const s=Math.floor((l.u||0)/1000),m=Math.floor(s/60),h=Math.floor(m/60);
    return h>0?'+'+h+'h'+(m%60)+'m':m>0?'+'+m+'m'+(s%60)+'s':'+'+s+'s';
}
$('#refresh-logs').onclick=fetchLogs;
$('#clear-logs').onclick=async()=>{
    if(!confirm('Clear all log entries?'))return;
    try{await post('/api/logs',null,'DELETE');fetchLogs()}catch(e){toast('Could not clear logs',true)}
};
$('#logs-section').addEventListener('toggle',function(){if(this.open)fetchLogs()});

// ---------- firmware: update checker ----------
let updateDismissed=false,updatePoll=null;
function updateUpdateUI(d){
    if(!d)return;
    const cur=d.current_version,latest=d.latest_version&&d.latest_version!=='unknown'?d.latest_version:'';
    $('#current-ver').textContent=cur?'v'+cur:'--';
    $('#latest-ver').textContent=latest?'v'+latest:'--';

    const st=$('#update-state');
    if(d.state==='downloading'){st.className='note';st.textContent='Downloading update... the reader restarts when done'}
    else if(d.state==='checking'){st.className='note';st.textContent='Checking for updates...'}
    else if(d.state==='error'){st.className='note bad';st.textContent='Update failed'+(d.error?': '+d.error:'')}
    else if(d.available){st.className='note';st.textContent='Version '+latest+' is available'}
    else if(latest){st.className='note ok';st.textContent='Up to date'}
    else{st.className='note';st.textContent='Not checked yet'}

    $('#fw-sum').textContent='v'+(cur||'--')+(d.available?' · update available':latest?' · up to date':'');
    $('#fw-badge').classList.toggle('hidden',!d.available);

    const showProg=d.state==='downloading';
    $('#update-progress-section').classList.toggle('hidden',!showProg);
    if(showProg){
        $('#update-progress-bar').style.width=d.download_progress+'%';
        $('#update-progress-text').textContent=d.download_progress+'%';
    }

    const busy=d.state==='checking'||d.state==='downloading';
    $('#check-update').disabled=busy;
    $('#do-install').classList.toggle('hidden',!(d.has_firmware_url&&d.available));
    $('#do-install').disabled=busy;
    $('#download-link').href=d.release_url||REL;

    const showBanner=d.available&&!updateDismissed;
    $('#update-banner').classList.toggle('hidden',!showBanner);
    if(showBanner)$('#update-version').textContent='v'+latest;
}
async function fetchUpdateStatus(){
    try{const d=await getJSON('/api/update/status');updateUpdateUI(d);return d}
    catch(e){return null}
}
function pollUpdate(){
    clearInterval(updatePoll);
    updatePoll=setInterval(async()=>{
        const d=await fetchUpdateStatus();
        if(!d){  // Device went away: it is restarting with the new firmware
            clearInterval(updatePoll);updatePoll=null;
            $('#update-state').className='note';$('#update-state').textContent='Restarting...';
            setTimeout(()=>location.reload(),10000);
        }else if(d.state==='idle'||d.state==='error'){clearInterval(updatePoll);updatePoll=null}
    },1000);
}
$('#check-update').onclick=async()=>{
    $('#update-state').className='note';
    $('#update-state').textContent='Checking for updates...';
    try{await post('/api/update/check');pollUpdate()}catch(err){toast(err.message,true)}
};
$('#do-install').onclick=async()=>{
    if(!confirm('Install the update? The reader restarts when the download is complete.'))return;
    try{await post('/api/update/install');pollUpdate()}catch(err){toast(err.message,true)}
};
$('#banner-dismiss').onclick=()=>{updateDismissed=true;$('#update-banner').classList.add('hidden')};
setTimeout(fetchUpdateStatus,3000);
setInterval(fetchUpdateStatus,30000);

// ---------- firmware: manual upload ----------
function setupUpload(p,endpoint){
    const card=$('#'+p+'-up'),input=$('#'+p+'-file'),btn=$('#'+p+'-upload-btn');
    const name=$('#'+p+'-file-name'),bar=$('#'+p+'-bar'),fill=$('#'+p+'-fill'),pct=$('#'+p+'-pct'),status=$('#'+p+'-status');
    const hint=name.textContent;
    card.querySelector('.pick').onclick=()=>input.click();
    card.addEventListener('dragover',e=>{e.preventDefault();card.classList.add('drag')});
    card.addEventListener('dragleave',()=>card.classList.remove('drag'));
    card.addEventListener('drop',e=>{
        e.preventDefault();card.classList.remove('drag');
        if(e.dataTransfer.files.length){input.files=e.dataTransfer.files;picked()}
    });
    input.onchange=picked;
    function picked(){
        const f=input.files[0];
        name.textContent=f?f.name+' · '+(f.size/1024).toFixed(0)+' KB':hint;
        btn.disabled=!f;
        status.textContent='';
    }
    btn.onclick=()=>{
        if(!input.files.length)return;
        const fd=new FormData();
        fd.append('file',input.files[0]);
        const xhr=new XMLHttpRequest();
        xhr.open('POST',endpoint,true);
        xhr.upload.onprogress=e=>{
            if(e.lengthComputable){
                const v=Math.round(e.loaded/e.total*100);
                fill.style.width=v+'%';pct.textContent=v+'%';
            }
        };
        xhr.onloadstart=()=>{
            btn.disabled=true;bar.classList.remove('hidden');fill.style.width='0%';pct.textContent='0%';
            status.className='muted sm';status.textContent='Uploading · keep this page open and the reader powered';
        };
        xhr.onload=()=>{
            if(xhr.status===200){
                status.className='sm';status.textContent='Update installed. Restarting...';
                setTimeout(()=>{location.href='/'},8000);
            }else{
                status.className='sm danger';status.textContent='Upload failed: '+(xhr.responseText||xhr.status);
                btn.disabled=false;
            }
        };
        xhr.onerror=()=>{
            status.className='sm danger';status.textContent='Connection lost during upload';
            btn.disabled=false;
        };
        xhr.send(fd);
    };
}
setupUpload('fw','/api/update/firmware');
setupUpload('fs','/api/update/filesystem');
