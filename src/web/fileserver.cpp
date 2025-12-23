// WiFi File Server implementation

#include "fileserver.h"
#include <SD.h>
#include <ESPmDNS.h>

// Static members
WebServer* FileServer::server = nullptr;
FileServerState FileServer::state = FileServerState::IDLE;
char FileServer::statusMessage[64] = "Ready";
char FileServer::targetSSID[64] = "";
char FileServer::targetPassword[64] = "";
uint32_t FileServer::connectStartTime = 0;
uint32_t FileServer::lastReconnectCheck = 0;

// File upload state (needs to be declared early for stop() to access it)
static File uploadFile;
static String uploadDir;

// Black & white HTML interface - Midnight Commander style dual-pane
static const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PORKCHOP COMMANDER</title>
    <style>
        :root { --pink: #FFAEAD; --bg: #000; --sel: #331a1a; --active: #442222; }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { 
            background: var(--bg); 
            color: var(--pink); 
            font-family: 'Courier New', monospace;
            font-size: 0.9em;
            height: 100vh;
            display: flex;
            flex-direction: column;
            overflow: hidden;
        }
        .header {
            padding: 5px 10px;
            border-bottom: 1px solid var(--pink);
            display: flex;
            justify-content: space-between;
            align-items: center;
            flex-shrink: 0;
        }
        .header h1 { font-size: 1em; font-weight: normal; }
        .sd-info { opacity: 0.6; font-size: 0.85em; }
        .panes {
            display: flex;
            flex: 1;
            overflow: hidden;
        }
        .pane {
            flex: 1;
            display: flex;
            flex-direction: column;
            border-right: 1px solid #331a1a;
            overflow: hidden;
        }
        .pane:last-child { border-right: none; }
        .pane.active .pane-header { background: var(--active); }
        .pane-header {
            padding: 6px 10px;
            background: #0a0505;
            border-bottom: 1px solid #331a1a;
            display: flex;
            justify-content: space-between;
            font-size: 0.85em;
            flex-shrink: 0;
        }
        .pane-path { 
            flex: 1; 
            overflow: hidden; 
            text-overflow: ellipsis; 
            white-space: nowrap;
        }
        .pane-select-info { opacity: 0.7; margin-left: 10px; }
        .file-list {
            flex: 1;
            overflow-y: auto;
            overflow-x: hidden;
        }
        .file-item { 
            display: flex; 
            align-items: center;
            padding: 4px 8px;
            cursor: pointer;
            border-bottom: 1px solid #0a0505;
        }
        .file-item:hover { background: #0f0808; }
        .file-item.focused { background: var(--sel); outline: 1px solid var(--pink); }
        .file-item.selected { background: var(--active); }
        .file-item.selected.focused { background: #553333; }
        .file-check { 
            width: 20px; 
            height: 16px;
            display: flex;
            align-items: center;
            justify-content: center;
            opacity: 0.3;
        }
        .file-item.selected .file-check { opacity: 1; }
        .file-icon { 
            width: 20px; 
            text-align: center; 
            opacity: 0.5;
            flex-shrink: 0;
        }
        .file-icon.dir { opacity: 1; color: var(--pink); }
        .file-name { 
            flex: 1; 
            overflow: hidden; 
            text-overflow: ellipsis; 
            white-space: nowrap;
            padding: 0 8px;
        }
        .file-size { 
            opacity: 0.4; 
            min-width: 60px; 
            text-align: right; 
            font-size: 0.85em;
            flex-shrink: 0;
        }
        .toolbar {
            display: flex;
            gap: 5px;
            padding: 8px;
            background: #050505;
            border-top: 1px solid #331a1a;
            flex-shrink: 0;
            flex-wrap: wrap;
        }
        .btn {
            background: var(--pink);
            color: var(--bg);
            border: none;
            padding: 5px 12px;
            cursor: pointer;
            font-family: inherit;
            font-size: 0.8em;
        }
        .btn:hover { opacity: 0.8; }
        .btn:disabled { opacity: 0.3; cursor: not-allowed; }
        .btn-outline { 
            background: transparent; 
            color: var(--pink); 
            border: 1px solid var(--pink); 
            opacity: 0.7; 
        }
        .btn-outline:hover { opacity: 1; background: #1a0d0d; }
        .btn-danger { background: #442222; color: var(--pink); }
        .fkey-bar {
            display: flex;
            background: #111;
            border-top: 1px solid #331a1a;
            flex-shrink: 0;
        }
        .fkey {
            flex: 1;
            padding: 6px 4px;
            text-align: center;
            font-size: 0.75em;
            border-right: 1px solid #222;
            cursor: pointer;
        }
        .fkey:hover { background: #1a0d0d; }
        .fkey:last-child { border-right: none; }
        .fkey span { opacity: 0.5; }
        .status {
            padding: 4px 10px;
            font-size: 0.8em;
            background: #050505;
            border-top: 1px solid #1a0d0d;
            min-height: 22px;
            flex-shrink: 0;
        }
        .modal {
            display: none;
            position: fixed;
            top: 0; left: 0;
            width: 100%; height: 100%;
            background: rgba(0,0,0,0.9);
            justify-content: center;
            align-items: center;
            z-index: 100;
        }
        .modal-content {
            background: var(--bg);
            border: 1px solid var(--pink);
            padding: 20px;
            max-width: 400px;
            width: 90%;
        }
        .modal-content h3 { margin-bottom: 15px; font-weight: normal; }
        .modal-actions { display: flex; gap: 10px; margin-top: 15px; }
        input[type="text"] {
            background: #0a0505;
            color: var(--pink);
            border: 1px solid #331a1a;
            padding: 8px;
            font-family: inherit;
            width: 100%;
        }
        input[type="text"]:focus { outline: none; border-color: var(--pink); }
        .upload-btn { position: relative; overflow: hidden; }
        .upload-btn input[type="file"] {
            position: absolute;
            left: 0; top: 0;
            width: 100%; height: 100%;
            opacity: 0;
            cursor: pointer;
        }
        .progress-bar {
            height: 4px;
            background: #1a0d0d;
            margin-top: 5px;
            display: none;
        }
        .progress-bar.active { display: block; }
        .progress-fill {
            height: 100%;
            background: var(--pink);
            width: 0%;
            transition: width 0.1s;
        }
        @media (max-width: 600px) {
            .panes { flex-direction: column; }
            .pane { border-right: none; border-bottom: 1px solid #331a1a; }
            .file-size { display: none; }
        }
    </style>
</head>
<body>
    <div class="header">
        <h1>PORKCHOP COMMANDER</h1>
        <div class="sd-info" id="sdInfo">...</div>
    </div>
    
    <div class="panes">
        <div class="pane active" id="paneL" onclick="setActivePane('L')">
            <div class="pane-header">
                <div class="pane-path" id="pathL">/</div>
                <div class="pane-select-info" id="selInfoL"></div>
            </div>
            <div class="file-list" id="listL"></div>
        </div>
        <div class="pane" id="paneR" onclick="setActivePane('R')">
            <div class="pane-header">
                <div class="pane-path" id="pathR">/</div>
                <div class="pane-select-info" id="selInfoR"></div>
            </div>
            <div class="file-list" id="listR"></div>
        </div>
    </div>
    
    <div class="toolbar">
        <button class="btn btn-outline" onclick="refresh()">Refresh</button>
        <button class="btn btn-outline" onclick="showNewFolderModal()">+Folder</button>
        <label class="btn upload-btn">Upload<input type="file" multiple onchange="uploadFiles(this.files)"></label>
        <button class="btn" onclick="selectAll()">Sel All</button>
        <button class="btn btn-outline" onclick="selectNone()">Sel None</button>
        <button class="btn" onclick="downloadSelected()" id="btnDownload">Download</button>
        <button class="btn btn-danger" onclick="deleteSelected()" id="btnDelete">Delete</button>
    </div>
    
    <div class="progress-bar" id="progressBar"><div class="progress-fill" id="progressFill"></div></div>
    
    <div class="fkey-bar">
        <div class="fkey" onclick="showHelp()"><span>F1</span> Help</div>
        <div class="fkey" onclick="refresh()"><span>F2</span> Refresh</div>
        <div class="fkey" onclick="showNewFolderModal()"><span>F7</span> MkDir</div>
        <div class="fkey" onclick="deleteSelected()"><span>F8</span> Delete</div>
        <div class="fkey" onclick="downloadSelected()"><span>F9</span> Get</div>
        <div class="fkey"><span>Tab</span> Switch</div>
    </div>
    
    <div class="status" id="status">Ready | Arrow keys navigate | Space select | Enter open | Tab switch pane</div>
    
    <!-- New Folder Modal -->
    <div class="modal" id="newFolderModal" onclick="if(event.target===this)hideModal()">
        <div class="modal-content">
            <h3>New Folder</h3>
            <input type="text" id="newFolderName" placeholder="Folder name" 
                   onkeydown="if(event.key==='Enter')createFolder();if(event.key==='Escape')hideModal()">
            <div class="modal-actions">
                <button class="btn" onclick="createFolder()">Create</button>
                <button class="btn btn-outline" onclick="hideModal()">Cancel</button>
            </div>
        </div>
    </div>
    
    <!-- Help Modal -->
    <div class="modal" id="helpModal" onclick="if(event.target===this)hideModal()">
        <div class="modal-content">
            <h3>Keyboard Shortcuts</h3>
            <pre style="font-size:0.85em;line-height:1.6;opacity:0.8">
Arrow Up/Down  Navigate files
Enter          Open folder / Download file
Space          Toggle selection
Tab            Switch pane
Ctrl+A         Select all
Delete/F8      Delete selected
F7             New folder
F9             Download selected
Backspace      Go to parent folder
            </pre>
            <div class="modal-actions">
                <button class="btn" onclick="hideModal()">Close</button>
            </div>
        </div>
    </div>

<script>
// Pane state
const panes = {
    L: { path: '/', items: [], selected: new Set(), focusIdx: 0 },
    R: { path: '/', items: [], selected: new Set(), focusIdx: 0 }
};
let activePane = 'L';

// Initialize
document.addEventListener('DOMContentLoaded', () => {
    loadSDInfo();
    loadPane('L', '/');
    loadPane('R', '/');
    document.addEventListener('keydown', handleKeydown);
});

function setActivePane(id) {
    activePane = id;
    document.getElementById('paneL').classList.toggle('active', id === 'L');
    document.getElementById('paneR').classList.toggle('active', id === 'R');
}

async function loadSDInfo() {
    try {
        const r = await fetch('/api/sdinfo');
        const d = await r.json();
        const pct = ((d.used / d.total) * 100).toFixed(0);
        document.getElementById('sdInfo').textContent = 
            formatSize(d.used * 1024) + ' / ' + formatSize(d.total * 1024) + ' (' + pct + '%)';
    } catch(e) {
        document.getElementById('sdInfo').textContent = 'SD unavailable';
    }
}

async function loadPane(id, path) {
    const pane = panes[id];
    pane.path = path;
    pane.selected.clear();
    pane.focusIdx = 0;
    
    document.getElementById('path' + id).textContent = path || '/';
    const list = document.getElementById('list' + id);
    list.innerHTML = '<div style="padding:20px;opacity:0.5">Loading...</div>';
    
    try {
        const r = await fetch('/api/ls?dir=' + encodeURIComponent(path) + '&full=1');
        const items = await r.json();
        
        // Sort: directories first, then alphabetically
        pane.items = [];
        
        // Parent directory entry
        if (path !== '/') {
            pane.items.push({ name: '..', isDir: true, isParent: true, size: 0 });
        }
        
        // Directories
        items.filter(i => i.isDir).sort((a,b) => a.name.localeCompare(b.name))
            .forEach(i => pane.items.push(i));
        
        // Files
        items.filter(i => !i.isDir).sort((a,b) => a.name.localeCompare(b.name))
            .forEach(i => pane.items.push(i));
        
        renderPane(id);
    } catch(e) {
        list.innerHTML = '<div style="padding:20px;opacity:0.5">Error loading</div>';
    }
    updateSelectionInfo(id);
}

function renderPane(id) {
    const pane = panes[id];
    const list = document.getElementById('list' + id);
    
    if (pane.items.length === 0) {
        list.innerHTML = '<div style="padding:20px;opacity:0.4;text-align:center">Empty</div>';
        return;
    }
    
    let html = '';
    pane.items.forEach((item, idx) => {
        const isSel = pane.selected.has(idx);
        const isFocus = (idx === pane.focusIdx && activePane === id);
        const cls = 'file-item' + (isSel ? ' selected' : '') + (isFocus ? ' focused' : '');
        const icon = item.isDir ? '/' : '*';
        const iconCls = item.isDir ? 'file-icon dir' : 'file-icon';
        const check = isSel ? '[x]' : '[ ]';
        const size = item.isDir ? '' : formatSize(item.size);
        
        html += '<div class="' + cls + '" data-idx="' + idx + '" data-pane="' + id + '"';
        html += ' onclick="onItemClick(event,' + idx + ',\'' + id + '\')"';
        html += ' ondblclick="onItemDblClick(' + idx + ',\'' + id + '\')">';
        html += '<div class="file-check">' + check + '</div>';
        html += '<div class="' + iconCls + '">' + icon + '</div>';
        html += '<div class="file-name">' + escapeHtml(item.name) + '</div>';
        html += '<div class="file-size">' + size + '</div>';
        html += '</div>';
    });
    list.innerHTML = html;
    
    // Scroll focused item into view
    const focused = list.querySelector('.focused');
    if (focused) focused.scrollIntoView({ block: 'nearest' });
}

function onItemClick(event, idx, paneId) {
    setActivePane(paneId);
    panes[paneId].focusIdx = idx;
    
    if (event.ctrlKey || event.metaKey) {
        toggleSelect(paneId, idx);
    } else if (event.shiftKey) {
        // Range select not implemented for simplicity
        toggleSelect(paneId, idx);
    } else {
        renderPane(paneId);
    }
}

function onItemDblClick(idx, paneId) {
    const pane = panes[paneId];
    const item = pane.items[idx];
    
    if (item.isParent) {
        const parent = pane.path.substring(0, pane.path.lastIndexOf('/')) || '/';
        loadPane(paneId, parent);
    } else if (item.isDir) {
        const newPath = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
        loadPane(paneId, newPath);
    } else {
        downloadFile(paneId, idx);
    }
}

function toggleSelect(paneId, idx) {
    const pane = panes[paneId];
    const item = pane.items[idx];
    if (item.isParent) return; // Can't select parent dir
    
    if (pane.selected.has(idx)) {
        pane.selected.delete(idx);
    } else {
        pane.selected.add(idx);
    }
    renderPane(paneId);
    updateSelectionInfo(paneId);
}

function updateSelectionInfo(id) {
    const pane = panes[id];
    const count = pane.selected.size;
    const el = document.getElementById('selInfo' + id);
    el.textContent = count > 0 ? '[' + count + ' sel]' : '';
    
    // Update toolbar buttons
    const totalSel = panes.L.selected.size + panes.R.selected.size;
    document.getElementById('btnDelete').textContent = totalSel > 0 ? 'Delete (' + totalSel + ')' : 'Delete';
    document.getElementById('btnDownload').textContent = totalSel > 0 ? 'Download (' + totalSel + ')' : 'Download';
}

function selectAll() {
    const pane = panes[activePane];
    pane.items.forEach((item, idx) => {
        if (!item.isParent) pane.selected.add(idx);
    });
    renderPane(activePane);
    updateSelectionInfo(activePane);
}

function selectNone() {
    const pane = panes[activePane];
    pane.selected.clear();
    renderPane(activePane);
    updateSelectionInfo(activePane);
}

function handleKeydown(e) {
    // Don't handle if in modal input
    if (document.activeElement.tagName === 'INPUT') return;
    
    const pane = panes[activePane];
    
    switch(e.key) {
        case 'ArrowUp':
            e.preventDefault();
            if (pane.focusIdx > 0) {
                pane.focusIdx--;
                renderPane(activePane);
            }
            break;
        case 'ArrowDown':
            e.preventDefault();
            if (pane.focusIdx < pane.items.length - 1) {
                pane.focusIdx++;
                renderPane(activePane);
            }
            break;
        case 'Enter':
            e.preventDefault();
            onItemDblClick(pane.focusIdx, activePane);
            break;
        case ' ':
            e.preventDefault();
            toggleSelect(activePane, pane.focusIdx);
            break;
        case 'Tab':
            e.preventDefault();
            setActivePane(activePane === 'L' ? 'R' : 'L');
            renderPane('L');
            renderPane('R');
            break;
        case 'Backspace':
            e.preventDefault();
            if (pane.path !== '/') {
                const parent = pane.path.substring(0, pane.path.lastIndexOf('/')) || '/';
                loadPane(activePane, parent);
            }
            break;
        case 'Delete':
            e.preventDefault();
            deleteSelected();
            break;
        case 'a':
            if (e.ctrlKey || e.metaKey) {
                e.preventDefault();
                selectAll();
            }
            break;
        case 'F1':
            e.preventDefault();
            showHelp();
            break;
        case 'F2':
            e.preventDefault();
            refresh();
            break;
        case 'F7':
            e.preventDefault();
            showNewFolderModal();
            break;
        case 'F8':
            e.preventDefault();
            deleteSelected();
            break;
        case 'F9':
            e.preventDefault();
            downloadSelected();
            break;
    }
}

function getSelectedPaths() {
    const paths = [];
    ['L', 'R'].forEach(id => {
        const pane = panes[id];
        pane.selected.forEach(idx => {
            const item = pane.items[idx];
            if (!item.isParent) {
                const path = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
                paths.push({ path, isDir: item.isDir });
            }
        });
    });
    return paths;
}

async function deleteSelected() {
    const items = getSelectedPaths();
    if (items.length === 0) {
        setStatus('Nothing selected');
        return;
    }
    
    const msg = 'Delete ' + items.length + ' item(s)? This cannot be undone.';
    if (!confirm(msg)) return;
    
    setStatus('Deleting ' + items.length + ' items...');
    
    try {
        const resp = await fetch('/api/bulkdelete', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ paths: items.map(i => i.path) })
        });
        const result = await resp.json();
        setStatus('Deleted ' + result.deleted + '/' + items.length + ' items');
        refresh();
    } catch(e) {
        setStatus('Delete error: ' + e.message);
    }
}

async function downloadSelected() {
    const items = getSelectedPaths().filter(i => !i.isDir);
    if (items.length === 0) {
        setStatus('No files selected (folders not downloadable as bulk)');
        return;
    }
    
    setStatus('Downloading ' + items.length + ' file(s)...');
    
    // Download files sequentially (browser limitation)
    for (let i = 0; i < items.length; i++) {
        await new Promise(resolve => {
            const a = document.createElement('a');
            a.href = '/download?f=' + encodeURIComponent(items[i].path);
            a.download = items[i].path.split('/').pop();
            a.click();
            setTimeout(resolve, 300); // Small delay between downloads
        });
    }
    
    setStatus('Downloaded ' + items.length + ' file(s)');
}

function downloadFile(paneId, idx) {
    const pane = panes[paneId];
    const item = pane.items[idx];
    if (item.isDir) return;
    
    const path = (pane.path === '/' ? '' : pane.path) + '/' + item.name;
    window.location.href = '/download?f=' + encodeURIComponent(path);
}

function refresh() {
    loadPane('L', panes.L.path);
    loadPane('R', panes.R.path);
    loadSDInfo();
}

function showNewFolderModal() {
    document.getElementById('newFolderModal').style.display = 'flex';
    document.getElementById('newFolderName').value = '';
    setTimeout(() => document.getElementById('newFolderName').focus(), 50);
}

function showHelp() {
    document.getElementById('helpModal').style.display = 'flex';
}

function hideModal() {
    document.getElementById('newFolderModal').style.display = 'none';
    document.getElementById('helpModal').style.display = 'none';
}

async function createFolder() {
    const name = document.getElementById('newFolderName').value.trim();
    if (!name) { alert('Enter folder name'); return; }
    if (name.includes('/') || name.includes('..')) { alert('Invalid name'); return; }
    
    const pane = panes[activePane];
    const path = (pane.path === '/' ? '' : pane.path) + '/' + name;
    
    try {
        const resp = await fetch('/mkdir?f=' + encodeURIComponent(path));
        if (resp.ok) {
            setStatus('Created: ' + name);
            hideModal();
            loadPane(activePane, pane.path);
        } else {
            setStatus('Failed to create folder');
        }
    } catch(e) {
        setStatus('Error: ' + e.message);
    }
}

async function uploadFiles(files) {
    if (!files || !files.length) return;
    
    const pane = panes[activePane];
    const bar = document.getElementById('progressBar');
    const fill = document.getElementById('progressFill');
    bar.classList.add('active');
    
    let uploaded = 0;
    for (let i = 0; i < files.length; i++) {
        setStatus('Uploading ' + (i+1) + '/' + files.length + ': ' + files[i].name);
        fill.style.width = '0%';
        
        const formData = new FormData();
        formData.append('file', files[i]);
        
        try {
            await new Promise((resolve, reject) => {
                const xhr = new XMLHttpRequest();
                xhr.upload.onprogress = (e) => {
                    if (e.lengthComputable) fill.style.width = (e.loaded/e.total*100) + '%';
                };
                xhr.onload = () => xhr.status === 200 ? resolve() : reject();
                xhr.onerror = () => reject();
                xhr.open('POST', '/upload?dir=' + encodeURIComponent(pane.path));
                xhr.send(formData);
            });
            uploaded++;
        } catch(e) {
            setStatus('Upload failed: ' + files[i].name);
        }
    }
    
    bar.classList.remove('active');
    setStatus('Uploaded ' + uploaded + '/' + files.length + ' files');
    loadPane(activePane, pane.path);
}

function setStatus(msg) {
    document.getElementById('status').textContent = msg;
}

function formatSize(bytes) {
    if (bytes < 1024) return bytes + 'B';
    if (bytes < 1024*1024) return (bytes/1024).toFixed(1) + 'K';
    if (bytes < 1024*1024*1024) return (bytes/1024/1024).toFixed(1) + 'M';
    return (bytes/1024/1024/1024).toFixed(2) + 'G';
}

function escapeHtml(s) {
    return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}
</script>
</body>
</html>
)rawliteral";

void FileServer::init() {
    state = FileServerState::IDLE;
    strcpy(statusMessage, "Ready");
    targetSSID[0] = '\0';
    targetPassword[0] = '\0';
}

bool FileServer::start(const char* ssid, const char* password) {
    if (state != FileServerState::IDLE) return true;
    
    // Store credentials for reconnection
    strncpy(targetSSID, ssid ? ssid : "", sizeof(targetSSID) - 1);
    strncpy(targetPassword, password ? password : "", sizeof(targetPassword) - 1);
    
    // Check credentials
    if (strlen(targetSSID) == 0) {
        strcpy(statusMessage, "No WiFi SSID set");
        return false;
    }
    
    strcpy(statusMessage, "jacking in.");
    Serial.printf("[FILESERVER] Starting connection to %s\n", targetSSID);
    
    // Start non-blocking connection
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(targetSSID, targetPassword);
    
    state = FileServerState::CONNECTING;
    connectStartTime = millis();
    
    return true;
}

void FileServer::startServer() {
    snprintf(statusMessage, sizeof(statusMessage), "%s", WiFi.localIP().toString().c_str());
    Serial.printf("[FILESERVER] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    
    // Start mDNS
    if (MDNS.begin("porkchop")) {
        Serial.println("[FILESERVER] mDNS: porkchop.local");
    }
    
    // Create and configure web server
    server = new WebServer(80);
    
    server->on("/", HTTP_GET, handleRoot);
    server->on("/api/ls", HTTP_GET, handleFileList);
    server->on("/api/sdinfo", HTTP_GET, handleSDInfo);
    server->on("/api/bulkdelete", HTTP_POST, handleBulkDelete);
    server->on("/download", HTTP_GET, handleDownload);
    server->on("/upload", HTTP_POST, handleUpload, handleUploadProcess);
    server->on("/delete", HTTP_GET, handleDelete);
    server->on("/rmdir", HTTP_GET, handleDelete);  // Same handler, will detect folder
    server->on("/mkdir", HTTP_GET, handleMkdir);
    server->onNotFound(handleNotFound);
    
    server->begin();
    state = FileServerState::RUNNING;
    lastReconnectCheck = millis();
    
    Serial.println("[FILESERVER] Server started on port 80");
}

void FileServer::stop() {
    if (state == FileServerState::IDLE) return;
    
    // Close any pending upload file
    if (uploadFile) {
        uploadFile.close();
        Serial.println("[FILESERVER] Closed pending upload file");
    }
    
    if (server) {
        server->stop();
        delete server;
        server = nullptr;
    }
    
    MDNS.end();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    
    state = FileServerState::IDLE;
    strcpy(statusMessage, "Stopped");
    Serial.println("[FILESERVER] Stopped");
}

void FileServer::update() {
    switch (state) {
        case FileServerState::CONNECTING:
        case FileServerState::RECONNECTING:
            updateConnecting();
            break;
        case FileServerState::RUNNING:
            updateRunning();
            break;
        default:
            break;
    }
}

void FileServer::updateConnecting() {
    uint32_t elapsed = millis() - connectStartTime;
    
    if (WiFi.status() == WL_CONNECTED) {
        startServer();
        return;
    }
    
    // Update status with dots animation
    int dots = (elapsed / 500) % 4;
    snprintf(statusMessage, sizeof(statusMessage), "jacking in%.*s", dots, "...");
    
    // Timeout after 15 seconds
    if (elapsed > 15000) {
        strcpy(statusMessage, "Connection failed");
        Serial.println("[FILESERVER] Connection timeout");
        WiFi.disconnect(true);
        state = FileServerState::IDLE;
    }
}

void FileServer::updateRunning() {
    if (server) {
        server->handleClient();
    }
    
    // Check WiFi connection every 5 seconds
    uint32_t now = millis();
    if (now - lastReconnectCheck > 5000) {
        lastReconnectCheck = now;
        
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[FILESERVER] WiFi lost, reconnecting...");
            strcpy(statusMessage, "retry hack.");
            
            // Stop server but keep credentials
            if (server) {
                server->stop();
                delete server;
                server = nullptr;
            }
            
            // Stop mDNS before reconnect
            MDNS.end();
            
            // Restart connection
            WiFi.disconnect(true);
            WiFi.begin(targetSSID, targetPassword);
            state = FileServerState::RECONNECTING;
            connectStartTime = millis();
        }
    }
}

uint64_t FileServer::getSDFreeSpace() {
    return SD.totalBytes() - SD.usedBytes();
}

uint64_t FileServer::getSDTotalSpace() {
    return SD.totalBytes();
}

void FileServer::handleRoot() {
    server->send(200, "text/html", HTML_TEMPLATE);
}

void FileServer::handleSDInfo() {
    String json = "{\"total\":";
    json += String((unsigned long)(SD.totalBytes() / 1024));  // KB
    json += ",\"used\":";
    json += String((unsigned long)(SD.usedBytes() / 1024));
    json += ",\"free\":";
    json += String((unsigned long)((SD.totalBytes() - SD.usedBytes()) / 1024));
    json += "}";
    server->send(200, "application/json", json);
}

void FileServer::handleFileList() {
    String dir = server->arg("dir");
    bool full = server->arg("full") == "1";
    if (dir.isEmpty()) dir = "/";
    
    // Security: prevent directory traversal
    if (dir.indexOf("..") >= 0) {
        server->send(400, "application/json", "[]");
        return;
    }
    
    File root = SD.open(dir);
    if (!root || !root.isDirectory()) {
        server->send(200, "application/json", "[]");
        return;
    }
    
    String json = "[";
    bool first = true;
    
    File file = root.openNextFile();
    while (file) {
        if (!first) json += ",";
        first = false;
        
        // Escape filename for JSON
        String fname = file.name();
        fname.replace("\\", "\\\\");
        fname.replace("\"", "\\\"");
        
        json += "{\"name\":\"";
        json += fname;
        json += "\",\"size\":";
        json += String(file.size());
        if (full) {
            json += ",\"isDir\":";
            json += file.isDirectory() ? "true" : "false";
        }
        json += "}";
        
        file.close();
        file = root.openNextFile();
    }
    
    root.close();
    json += "]";
    server->send(200, "application/json", json);
}

void FileServer::handleDownload() {
    String path = server->arg("f");
    String dir = server->arg("dir");  // For ZIP download
    
    // ZIP download of folder
    if (!dir.isEmpty()) {
        // Simple implementation: send files one by one is not possible
        // Instead, we'll create a simple text manifest for now
        // Full ZIP requires external library
        server->send(501, "text/plain", "ZIP download not yet implemented - download files individually");
        return;
    }
    
    if (path.isEmpty()) {
        server->send(400, "text/plain", "Missing file path");
        return;
    }
    
    // Security: prevent directory traversal
    if (path.indexOf("..") >= 0) {
        server->send(400, "text/plain", "Invalid path");
        return;
    }
    
    File file = SD.open(path);
    if (!file || file.isDirectory()) {
        server->send(404, "text/plain", "File not found");
        return;
    }
    
    // Get filename for Content-Disposition
    String filename = path;
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash >= 0) {
        filename = path.substring(lastSlash + 1);
    }
    
    // Determine content type
    String contentType = "application/octet-stream";
    if (path.endsWith(".txt")) contentType = "text/plain";
    else if (path.endsWith(".csv")) contentType = "text/csv";
    else if (path.endsWith(".json")) contentType = "application/json";
    else if (path.endsWith(".pcap")) contentType = "application/vnd.tcpdump.pcap";
    
    server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    server->streamFile(file, contentType);
    file.close();
}

void FileServer::handleUpload() {
    server->send(200, "text/plain", "OK");
}

void FileServer::handleUploadProcess() {
    HTTPUpload& upload = server->upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        uploadDir = server->arg("dir");
        if (uploadDir.isEmpty()) uploadDir = "/";
        if (uploadDir != "/" && !uploadDir.endsWith("/")) uploadDir += "/";
        if (uploadDir == "/") uploadDir = "";  // Root doesn't need slash prefix
        
        // Security: prevent directory traversal
        String filename = upload.filename;
        if (filename.indexOf("..") >= 0 || uploadDir.indexOf("..") >= 0) {
            Serial.println("[FILESERVER] Path traversal attempt blocked");
            return;
        }
        
        String path = uploadDir + "/" + filename;
        if (path.startsWith("//")) path = path.substring(1);
        Serial.printf("[FILESERVER] Upload start: %s\n", path.c_str());
        
        uploadFile = SD.open(path, FILE_WRITE);
        if (!uploadFile) {
            Serial.println("[FILESERVER] Failed to open file for writing");
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            uploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            Serial.printf("[FILESERVER] Upload complete: %u bytes\n", upload.totalSize);
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        // Client disconnected or error - close file to prevent leak
        if (uploadFile) {
            uploadFile.close();
            Serial.println("[FILESERVER] Upload aborted - file handle closed");
        }
    }
}

// Recursive delete helper - properly handles nested directories
static bool deletePathRecursive(const String& path) {
    File f = SD.open(path);
    if (!f) return false;
    
    bool isDir = f.isDirectory();
    f.close();
    
    if (!isDir) {
        return SD.remove(path);
    }
    
    // It's a directory - delete all contents first (depth-first)
    File dir = SD.open(path);
    if (!dir) return false;
    
    File entry = dir.openNextFile();
    while (entry) {
        String entryPath = path + "/" + String(entry.name());
        bool entryIsDir = entry.isDirectory();
        entry.close();
        
        if (entryIsDir) {
            // Recurse into subdirectory
            if (!deletePathRecursive(entryPath)) {
                dir.close();
                return false;
            }
        } else {
            if (!SD.remove(entryPath)) {
                dir.close();
                return false;
            }
        }
        entry = dir.openNextFile();
    }
    dir.close();
    
    // Now remove the empty directory
    return SD.rmdir(path);
}

void FileServer::handleDelete() {
    String path = server->arg("f");
    if (path.isEmpty()) {
        server->send(400, "text/plain", "Missing path");
        return;
    }
    
    // Security: prevent directory traversal
    if (path.indexOf("..") >= 0) {
        server->send(400, "text/plain", "Invalid path");
        return;
    }
    
    bool success = deletePathRecursive(path);
    
    if (success) {
        server->send(200, "text/plain", "Deleted");
        Serial.printf("[FILESERVER] Deleted: %s\n", path.c_str());
    } else {
        server->send(500, "text/plain", "Delete failed");
    }
}

void FileServer::handleBulkDelete() {
    // Read JSON body
    if (!server->hasArg("plain")) {
        server->send(400, "application/json", "{\"error\":\"Missing body\"}");
        return;
    }
    
    String body = server->arg("plain");
    
    // Simple JSON parsing for {"paths":["path1","path2",...]}
    // Using manual parsing to avoid ArduinoJson dependency in this module
    int deleted = 0;
    int failed = 0;
    
    int idx = body.indexOf("\"paths\"");
    if (idx < 0) {
        server->send(400, "application/json", "{\"error\":\"Missing paths array\"}");
        return;
    }
    
    int arrStart = body.indexOf('[', idx);
    int arrEnd = body.indexOf(']', arrStart);
    if (arrStart < 0 || arrEnd < 0) {
        server->send(400, "application/json", "{\"error\":\"Invalid paths array\"}");
        return;
    }
    
    String arrContent = body.substring(arrStart + 1, arrEnd);
    
    // Parse each path from the array
    int pos = 0;
    while (pos < (int)arrContent.length()) {
        int quoteStart = arrContent.indexOf('"', pos);
        if (quoteStart < 0) break;
        
        int quoteEnd = arrContent.indexOf('"', quoteStart + 1);
        if (quoteEnd < 0) break;
        
        String path = arrContent.substring(quoteStart + 1, quoteEnd);
        pos = quoteEnd + 1;
        
        // Security check
        if (path.indexOf("..") >= 0) {
            failed++;
            continue;
        }
        
        if (deletePathRecursive(path)) {
            deleted++;
            Serial.printf("[FILESERVER] Bulk deleted: %s\n", path.c_str());
        } else {
            failed++;
        }
        
        yield();  // Feed watchdog during bulk operations
    }
    
    String response = "{\"deleted\":" + String(deleted) + ",\"failed\":" + String(failed) + "}";
    server->send(200, "application/json", response);
}

void FileServer::handleMkdir() {
    String path = server->arg("f");
    if (path.isEmpty()) {
        server->send(400, "text/plain", "Missing path");
        return;
    }
    
    // Security: prevent directory traversal
    if (path.indexOf("..") >= 0) {
        server->send(400, "text/plain", "Invalid path");
        return;
    }
    
    if (SD.mkdir(path)) {
        server->send(200, "text/plain", "Created");
        Serial.printf("[FILESERVER] Created folder: %s\n", path.c_str());
    } else {
        server->send(500, "text/plain", "Create folder failed");
    }
}

void FileServer::handleNotFound() {
    server->send(404, "text/plain", "Not found");
}

const char* FileServer::getHTML() {
    return HTML_TEMPLATE;
}
