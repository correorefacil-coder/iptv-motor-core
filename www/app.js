// State variables
let inputs = [];
let streams = [];
let systemStatus = {};
let logsInterval = null;
let settingsLogsInterval = null;
let currentUser = null;
let currentProbedUrl = '';
let currentProbedInputId = '';

// API base URL (empty for same host)
const API_BASE = '';

// DOM Elements
const cpuBar = document.getElementById('cpu-bar');
const cpuText = document.getElementById('cpu-text');
const memBar = document.getElementById('mem-bar');
const memText = document.getElementById('mem-text');
const gpuCard = document.getElementById('gpu-card');
const gpuMemCard = document.getElementById('gpu-mem-card');
const gpuBar = document.getElementById('gpu-bar');
const gpuText = document.getElementById('gpu-text');
const gpuMemBar = document.getElementById('gpu-mem-bar');
const gpuMemText = document.getElementById('gpu-mem-text');
const bwInText = document.getElementById('bw-in');
const bwOutText = document.getElementById('bw-out');

const countInputsActive = document.getElementById('count-inputs-active');
const countInputsTotal = document.getElementById('count-inputs-total');
const countStreamsActive = document.getElementById('count-streams-active');
const countStreamsTotal = document.getElementById('count-streams-total');

const streamsGrid = document.getElementById('streams-grid');
const inputsContainer = document.getElementById('inputs-container');

// Modals
const modalInput = document.getElementById('modal-input');
const modalStream = document.getElementById('modal-stream');
const modalProbe = document.getElementById('modal-probe');
const modalSettings = document.getElementById('modal-settings');
const modalFs = document.getElementById('modal-fs');

// Forms
const formInput = document.getElementById('form-input');
const formStream = document.getElementById('form-stream');

// File Explorer DOM Elements
const btnBrowseFiles = document.getElementById('btn-browse-files');
const fsCurrentPath = document.getElementById('fs-current-path');
const fsListContainer = document.getElementById('fs-list-container');
const btnSelectFile = document.getElementById('btn-select-file');
const btnSelectFolder = document.getElementById('btn-select-folder');

// --- Helper: format bitrate ---
function formatBitrate(kbps) {
    if (kbps === undefined || kbps === null || isNaN(kbps) || kbps <= 0) {
        return '0 Kbps';
    }
    if (kbps >= 1000) {
        return `${(kbps / 1000).toFixed(2)} Mbps`;
    }
    return `${Math.round(kbps)} Kbps`;
}

// --- Fetch API helper ---
async function apiCall(endpoint, options = {}) {
    try {
        const response = await fetch(`${API_BASE}${endpoint}`, {
            headers: {
                'Content-Type': 'application/json',
            },
            ...options
        });
        if (!response.ok) {
            if (response.status === 401) {
                window.location.href = '/login.html';
                return null;
            }
            const errText = await response.text();
            try {
                const parsed = JSON.parse(errText);
                return parsed;
            } catch (jsonErr) {
                throw new Error(errText || response.statusText);
            }
        }
        return await response.json();
    } catch (e) {
        console.error(`API Call failed (${endpoint}):`, e);
        return null;
    }
}

// --- Fetch Data ---
async function updateStats() {
    const status = await apiCall('/api/status');
    if (status) {
        systemStatus = status;
        
        // Update hardware metrics
        cpuBar.style.width = `${status.cpu_usage.toFixed(0)}%`;
        cpuText.textContent = `${status.cpu_usage.toFixed(0)}%`;
        memBar.style.width = `${status.mem_usage.toFixed(0)}%`;
        memText.textContent = `${status.mem_usage.toFixed(0)}%`;
        
        if (status.gpu_available) {
            gpuCard.style.display = 'flex';
            gpuMemCard.style.display = 'flex';
            gpuBar.style.width = `${status.gpu_usage.toFixed(0)}%`;
            gpuText.textContent = `${status.gpu_usage.toFixed(0)}%`;
            
            gpuMemBar.style.width = `${status.gpu_mem_usage.toFixed(0)}%`;
            gpuMemText.textContent = `${status.gpu_mem_usage.toFixed(0)}% (${status.gpu_mem_used.toFixed(0)} / ${status.gpu_mem_total.toFixed(0)} MiB)`;
        } else {
            gpuCard.style.display = 'none';
            gpuMemCard.style.display = 'none';
        }
        
        // Update counts
        countInputsActive.textContent = status.inputs_active || 0;
        countInputsTotal.textContent = status.inputs_total || 0;
        countStreamsActive.textContent = status.streams_active || 0;
        countStreamsTotal.textContent = status.streams_total || 0;
    }
}

async function fetchInputs() {
    const data = await apiCall('/api/inputs');
    if (data) {
        inputs = data;
        renderInputs();
        updateInputDropdowns();
    }
}

async function fetchStreams() {
    const data = await apiCall('/api/streams');
    if (data) {
        streams = data;
        renderStreams();
        calculateTotalBandwidths();
    }
}

function calculateTotalBandwidths() {
    let totalInKbps = 0;
    let totalOutKbps = 0;

    inputs.forEach(input => {
        if (input.connected) {
            totalInKbps += (input.bitrate_kbps || 0);
        }
    });

    streams.forEach(stream => {
        if (stream.active) {
            totalOutKbps += (stream.bitrate_kbps || 0);
        }
    });

    bwInText.textContent = formatBitrate(totalInKbps);
    bwOutText.textContent = formatBitrate(totalOutKbps);
}

// --- RENDER DASHBOARD GRID ---
function getPlayableUrl(dest) {
    if (dest.type === 'hls') {
        let path = dest.url;
        if (path.startsWith('www/')) {
            path = path.substring(4);
        } else if (path.startsWith('./www/')) {
            path = path.substring(6);
        }
        if (!path.startsWith('/')) {
            path = '/' + path;
        }
        return `${window.location.protocol}//${window.location.host}${path}`;
    }
    return dest.url;
}

function renderStreams() {
    if (streams.length === 0) {
        streamsGrid.innerHTML = `<div class="no-data">No hay canales configurados. Haz clic en "Nuevo Canal" para empezar.</div>`;
        return;
    }

    streamsGrid.innerHTML = '';
    streams.forEach(stream => {
        // Find input name
        const inputSource = inputs.find(i => i.id === stream.input_id);
        const inputName = inputSource ? inputSource.name : 'Desconocido';

        const card = document.createElement('div');
        card.className = `stream-card ${stream.active ? 'active' : ''} ${stream.error_message ? 'error-state' : ''}`;
        card.id = `card-${stream.id}`;

        const isVideoPackStream = inputSource && inputSource.is_video_pack;
        const channelNumLabel = isVideoPackStream ? `Archivo: ${stream.video_filename || 'Sin seleccionar'}` : `Programa #${stream.program_number}`;

        let outputsHtml = '';
        if (stream.outputs && stream.outputs.length > 0) {
            stream.outputs.forEach(dest => {
                const playableUrl = getPlayableUrl(dest);
                const typeBadge = `<span class="badge-type">${dest.type.toUpperCase()}</span>`;
                const ifaceText = dest.output_interface ? ` (${dest.output_interface})` : '';
                outputsHtml += `
                <div class="card-path" style="margin-top: 4px;">
                    <span class="path-lbl" style="min-width: 45px; display: inline-block;">${typeBadge}</span>
                    <span class="path-val" title="${playableUrl}">
                        ${playableUrl}${ifaceText}
                        <button class="copy-url-btn" title="Copiar URL para reproductor" onclick="copyVlcUrl('${playableUrl}', event)">📋</button>
                    </span>
                </div>`;
            });
        } else {
            const typeBadge = `<span class="badge-type">UDP/SRT</span>`;
            const ifaceText = stream.output_interface ? ` (${stream.output_interface})` : '';
            outputsHtml += `
            <div class="card-path" style="margin-top: 4px;">
                <span class="path-lbl" style="min-width: 45px; display: inline-block;">${typeBadge}</span>
                <span class="path-val" title="${stream.output_url}">
                    ${stream.output_url}${ifaceText}
                    <button class="copy-url-btn" title="Copiar URL para reproductor" onclick="copyVlcUrl('${stream.output_url}', event)">📋</button>
                </span>
            </div>`;
        }

        card.innerHTML = `
            <div class="card-header">
                <div class="channel-info">
                    <span class="channel-num">${channelNumLabel}</span>
                    <span class="channel-name" title="${stream.name}">${stream.name}</span>
                </div>
                <span class="status-indicator" title="${stream.active ? 'Transmitiendo' : 'Inactivo'}"></span>
            </div>
            
            <div class="card-middle">
                <div class="bitrate-value">${formatBitrate(stream.bitrate_kbps).split(' ')[0]}<span class="bitrate-unit">${formatBitrate(stream.bitrate_kbps).split(' ')[1]}</span></div>
            </div>
            
            <div class="card-bottom">
                <div class="card-path">
                    <span class="path-lbl">Origen:</span>
                    <span class="path-val" title="${inputName}">${inputName}</span>
                </div>
                <div style="margin-top: 6px; border-top: 1px solid rgba(255,255,255,0.05); padding-top: 4px;">
                    ${outputsHtml}
                </div>
                ${stream.error_message ? `<div class="card-path error" style="color:var(--red-alert); font-size:11px;">Error: ${stream.error_message}</div>` : ''}
            </div>

            ${currentUser && currentUser.role === 'Consulta' ? '' : `
            <div class="card-actions">
                <button class="btn btn-secondary btn-small" onclick="toggleStream('${stream.id}', ${!stream.enabled})">
                    ${stream.enabled ? 'Pausar' : 'Activar'}
                </button>
                <button class="btn btn-primary btn-small" onclick="editStream('${stream.id}')">Editar</button>
                ${currentUser && currentUser.role === 'Programadores' ? '' : `<button class="btn btn-danger btn-small" onclick="deleteStream('${stream.id}')">Eliminar</button>`}
            </div>
            `}
        `;
        streamsGrid.appendChild(card);
    });
}

function renderInputs() {
    if (inputs.length === 0) {
        inputsContainer.innerHTML = `<div class="no-data">No hay Packs de Entrada configurados. Haz clic en "+ Agregar Pack" para empezar.</div>`;
        return;
    }

    inputsContainer.innerHTML = '';
    inputs.forEach(input => {
        const card = document.createElement('div');
        card.className = `input-pack-card ${input.connected ? 'connected' : 'offline'} ${!input.enabled ? 'disabled' : ''}`;
        card.id = `input-${input.id}`;

        card.innerHTML = `
            <div class="pack-info">
                <div class="pack-name-row">
                    <span class="pack-name">${input.name}</span>
                    <span class="pack-status-badge">${input.connected ? 'Conectado' : (input.enabled ? 'Conectando...' : 'Deshabilitado')}</span>
                </div>
                <span class="pack-url" title="${input.url}">${input.url}</span>
                <span class="pack-channels-count">${input.programs ? input.programs.length : 0} ${input.is_video_pack ? 'videos' : 'canales'} detectados</span>
            </div>

            <div class="pack-metrics">
                <div class="pack-bitrate">
                    <span class="pack-br-label">Ancho de banda</span>
                    <span class="pack-br-val">${formatBitrate(input.bitrate_kbps)}</span>
                </div>
                ${currentUser && (currentUser.role === 'Consulta' || currentUser.role === 'Programadores') ? '' : `
                <div class="pack-actions">
                    <button class="btn btn-secondary btn-small" onclick="toggleInput('${input.id}', ${!input.enabled})">
                        ${input.enabled ? 'Pausar' : 'Activar'}
                    </button>
                    <button class="btn btn-secondary btn-small" onclick="probeInput('${input.url}', '${input.id}')">Probe / Explorar</button>
                    <button class="btn btn-secondary btn-small" onclick="editInput('${input.id}')">Editar</button>
                    <button class="btn btn-danger btn-small" onclick="deleteInput('${input.id}')">Eliminar</button>
                </div>
                `}
            </div>
        `;
        inputsContainer.appendChild(card);
    });
}

// --- Dynamic Input dropdown list ---
function updateInputDropdowns() {
    const streamInputDropdown = document.getElementById('stream-input-id');
    const currentVal = streamInputDropdown.value;

    streamInputDropdown.innerHTML = '<option value="">Selecciona un pack...</option>';
    inputs.forEach(input => {
        const option = document.createElement('option');
        option.value = input.id;
        option.textContent = input.name;
        streamInputDropdown.appendChild(option);
    });

    if (currentVal && inputs.some(i => i.id === currentVal)) {
        streamInputDropdown.value = currentVal;
    }
}

// Populate Programs based on input source selection
document.getElementById('stream-input-id').addEventListener('change', function(e) {
    populateProgramsFromSelectedInput(e.target.value);
});

function populateProgramsFromSelectedInput(inputId, selectProgramNum = null, selectVideoFilename = null) {
    const streamProgramDropdown = document.getElementById('stream-program');
    const streamVideoFilenameDropdown = document.getElementById('stream-video-filename');
    const programGroup = document.getElementById('stream-program-group');
    const videoGroup = document.getElementById('stream-video-filename-group');

    if (!inputId) {
        programGroup.style.display = 'block';
        videoGroup.style.display = 'none';
        streamProgramDropdown.innerHTML = '<option value="">Selecciona un programa...</option>';
        return;
    }

    const inputSource = inputs.find(i => i.id === inputId);
    const isVideoPack = inputSource && inputSource.is_video_pack;

    if (isVideoPack) {
        programGroup.style.display = 'none';
        videoGroup.style.display = 'block';
        streamVideoFilenameDropdown.innerHTML = '<option value="">Selecciona un video...</option>';
        
        streamProgramDropdown.required = false;
        streamVideoFilenameDropdown.required = true;

        if (inputSource.programs && inputSource.programs.length > 0) {
            inputSource.programs.forEach(prog => {
                const option = document.createElement('option');
                option.value = prog.name;
                option.textContent = prog.name;
                streamVideoFilenameDropdown.appendChild(option);
            });
        } else {
            const option = document.createElement('option');
            option.value = "";
            option.textContent = "Carpeta vacía (Sube videos primero)";
            streamVideoFilenameDropdown.appendChild(option);
        }

        if (selectVideoFilename) {
            streamVideoFilenameDropdown.value = selectVideoFilename;
        }
    } else {
        programGroup.style.display = 'block';
        videoGroup.style.display = 'none';
        
        streamProgramDropdown.required = true;
        streamVideoFilenameDropdown.required = false;

        streamProgramDropdown.innerHTML = '<option value="">Selecciona un programa...</option>';

        if (inputSource && inputSource.programs && inputSource.programs.length > 0) {
            inputSource.programs.forEach(prog => {
                const option = document.createElement('option');
                option.value = prog.program_number;
                option.textContent = `${prog.program_number} - ${prog.name}`;
                streamProgramDropdown.appendChild(option);
            });
        } else {
            const option = document.createElement('option');
            option.value = "";
            option.textContent = "Sin canales detectados (Prueba haciendo 'Escanear')";
            streamProgramDropdown.appendChild(option);
        }

        if (selectProgramNum !== null) {
            if (!Array.from(streamProgramDropdown.options).some(o => o.value == selectProgramNum)) {
                const option = document.createElement('option');
                option.value = selectProgramNum;
                option.textContent = `${selectProgramNum} (Mapeado manualmente)`;
                streamProgramDropdown.appendChild(option);
            }
            streamProgramDropdown.value = selectProgramNum;
        }
    }
}

// Manual probe refresh inside stream modal
document.getElementById('btn-refresh-programs').addEventListener('click', async () => {
    const inputId = document.getElementById('stream-input-id').value;
    if (!inputId) {
        alert('Por favor selecciona un Pack de entrada primero.');
        return;
    }
    const inputSource = inputs.find(i => i.id === inputId);
    if (!inputSource) return;

    probeInput(inputSource.url, inputSource.id);
});

// Handle video upload inside stream modal
document.getElementById('btn-stream-video-upload').addEventListener('click', async () => {
    const fileInput = document.getElementById('stream-video-upload-input');
    const statusDiv = document.getElementById('stream-video-upload-status');
    const inputId = document.getElementById('stream-input-id').value;

    if (!inputId) {
        alert('Por favor selecciona un Pack de entrada primero.');
        return;
    }

    const inputSource = inputs.find(i => i.id === inputId);
    if (!inputSource || !inputSource.is_video_pack) {
        alert('El pack de entrada seleccionado no es un paquete de videos.');
        return;
    }

    if (!fileInput.files || fileInput.files.length === 0) {
        alert('Por favor selecciona un archivo para subir.');
        return;
    }

    const file = fileInput.files[0];
    const formData = new FormData();
    formData.append('file', file);

    statusDiv.textContent = 'Subiendo archivo... 0%';
    statusDiv.style.color = 'var(--text-muted)';

    try {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', `/api/fs/upload?path=${encodeURIComponent(inputSource.url)}`, true);

        xhr.upload.onprogress = (event) => {
            if (event.lengthComputable) {
                const percentComplete = Math.round((event.loaded / event.total) * 100);
                statusDiv.textContent = `Subiendo archivo... ${percentComplete}%`;
            }
        };

        xhr.onload = async () => {
            if (xhr.status >= 200 && xhr.status < 300) {
                const response = JSON.parse(xhr.responseText);
                if (response.success) {
                    statusDiv.textContent = '¡Archivo subido con éxito! Actualizando lista...';
                    statusDiv.style.color = 'var(--green-success)';
                    fileInput.value = '';

                    // Refresh inputs to scan the folder again and update the programs list
                    await fetchInputs();
                    
                    // Repopulate the dropdown and select the newly uploaded file
                    populateProgramsFromSelectedInput(inputId, null, file.name);
                    
                    statusDiv.textContent = '¡Video subido y seleccionado correctamente!';
                } else {
                    statusDiv.textContent = `Error: ${response.error || 'Fallo desconocido'}`;
                    statusDiv.style.color = 'var(--red-alert)';
                }
            } else {
                let errorMsg = 'Error en el servidor';
                try {
                    const errorResponse = JSON.parse(xhr.responseText);
                    errorMsg = errorResponse.error || errorMsg;
                } catch (e) {}
                statusDiv.textContent = `Error: ${errorMsg}`;
                statusDiv.style.color = 'var(--red-alert)';
            }
        };

        xhr.onerror = () => {
            statusDiv.textContent = 'Error de conexión de red.';
            statusDiv.style.color = 'var(--red-alert)';
        };

        xhr.send(formData);
    } catch (error) {
        statusDiv.textContent = `Error: ${error.message}`;
        statusDiv.style.color = 'var(--red-alert)';
    }
});

// --- TOGGLES & TRIGGERS ---
async function toggleStream(id, enable) {
    const stream = streams.find(s => s.id === id);
    if (!stream) return;

    const updated = {
        name: stream.name,
        input_id: stream.input_id,
        program_number: stream.program_number,
        output_url: stream.output_url,
        enabled: enable,
        output_interface: stream.output_interface || ''
    };

    const res = await apiCall(`/api/streams/${id}`, {
        method: 'PUT',
        body: JSON.stringify(updated)
    });
    if (res) {
        fetchStreams();
    }
}

async function toggleInput(id, enable) {
    const input = inputs.find(i => i.id === id);
    if (!input) return;

    const updated = {
        name: input.name,
        url: input.url,
        is_video_pack: input.is_video_pack,
        enabled: enable
    };

    const res = await apiCall(`/api/inputs/${id}`, {
        method: 'PUT',
        body: JSON.stringify(updated)
    });
    if (res) {
        fetchInputs();
    }
}

// --- ADD / EDIT INPUT HANDLERS ---
document.getElementById('btn-new-input').addEventListener('click', () => {
    document.getElementById('input-id').value = '';
    formInput.reset();
    document.getElementById('input-enabled').checked = true;
    document.getElementById('modal-input-title').textContent = 'Agregar Pack de Entrada';
    modalInput.style.display = 'block';
});

async function editInput(id) {
    const input = inputs.find(i => i.id === id);
    if (!input) return;

    document.getElementById('input-id').value = input.id;
    document.getElementById('input-name').value = input.name;
    document.getElementById('input-url').value = input.url;
    document.getElementById('input-type').value = input.is_video_pack ? 'videopack' : 'live';
    document.getElementById('input-enabled').checked = input.enabled !== false;
    document.getElementById('modal-input-title').textContent = 'Editar Pack de Entrada';
    modalInput.style.display = 'block';
}

async function deleteInput(id) {
    if (confirm('¿Estás seguro de eliminar esta entrada? También se eliminarán todos los canales asociados a este Pack.')) {
        const res = await apiCall(`/api/inputs/${id}`, {
            method: 'DELETE'
        });
        if (res) {
            fetchInputs();
            fetchStreams();
        }
    }
}

formInput.addEventListener('submit', async (e) => {
    e.preventDefault();
    const id = document.getElementById('input-id').value;
    const name = document.getElementById('input-name').value;
    const url = document.getElementById('input-url').value;
    const is_video_pack = document.getElementById('input-type').value === 'videopack';
    const enabled = document.getElementById('input-enabled').checked;

    const payload = { name, url, is_video_pack, enabled };

    let res;
    if (id) {
        // Edit
        res = await apiCall(`/api/inputs/${id}`, {
            method: 'PUT',
            body: JSON.stringify(payload)
        });
    } else {
        // New
        res = await apiCall('/api/inputs', {
            method: 'POST',
            body: JSON.stringify(payload)
        });
    }

    if (res) {
        modalInput.style.display = 'none';
        fetchInputs();
    } else {
        alert('Error al guardar la entrada. Revisa la consola o los logs.');
    }
});

function applyProgrammerStreamModalRestrictions() {
    const isProgrammer = currentUser && currentUser.role === 'Programadores';
    
    // 1. Disable Channel Name, Input Pack, and Program number (they can only view them)
    document.getElementById('stream-name').disabled = isProgrammer;
    document.getElementById('stream-input-id').disabled = isProgrammer;
    document.getElementById('stream-program').disabled = isProgrammer;
    
    const btnRefreshPrograms = document.getElementById('btn-refresh-programs');
    if (btnRefreshPrograms) btnRefreshPrograms.disabled = isProgrammer;

    // 2. Hide/Show Outputs and Transcoding sections completely
    const outputsGroup = document.getElementById('stream-outputs-group');
    if (outputsGroup) {
        outputsGroup.style.display = isProgrammer ? 'none' : 'block';
    }
    const transcodeGroup = document.getElementById('stream-transcode-group');
    if (transcodeGroup) {
        transcodeGroup.style.display = isProgrammer ? 'none' : 'block';
    }
    const transcodeOptionsContainer = document.getElementById('transcode-options-container');
    if (transcodeOptionsContainer) {
        if (isProgrammer) {
            transcodeOptionsContainer.style.display = 'none';
        }
    }

    // Still keep fallback fields disabled just in case
    const btnAddOutput = document.getElementById('btn-add-output');
    if (btnAddOutput) btnAddOutput.disabled = isProgrammer;
    const container = document.getElementById('stream-outputs-container');
    if (container) {
        container.querySelectorAll('input, select, button').forEach(el => {
            el.disabled = isProgrammer;
        });
    }
    document.getElementById('stream-transcode-enabled').disabled = isProgrammer;
    document.getElementById('stream-transcode-video').disabled = isProgrammer;
    document.getElementById('stream-video-output').disabled = isProgrammer;
    document.getElementById('stream-transcode-audio').disabled = isProgrammer;
    document.getElementById('stream-audio-output').disabled = isProgrammer;
    document.getElementById('stream-limit-bitrate').disabled = isProgrammer;
}

document.getElementById('btn-new-stream').addEventListener('click', async () => {
    document.getElementById('stream-id').value = '';
    formStream.reset();
    document.getElementById('stream-enabled').checked = true;
    document.getElementById('stream-program').innerHTML = '<option value="">Selecciona un programa...</option>';
    document.getElementById('stream-video-filename').innerHTML = '<option value="">Selecciona un video...</option>';
    
    // Reset transcoding fields
    document.getElementById('stream-transcode-enabled').checked = false;
    document.getElementById('stream-transcode-video').checked = false;
    document.getElementById('stream-video-input').value = 'auto';
    document.getElementById('stream-video-output').value = 'h264';
    document.getElementById('stream-transcode-audio').checked = false;
    document.getElementById('stream-audio-input').value = 'auto';
    document.getElementById('stream-audio-output').value = 'aac';
    document.getElementById('stream-limit-bitrate').value = '';
    document.getElementById('stream-video-input-detected').textContent = 'Auto';
    document.getElementById('stream-audio-input-detected').textContent = 'Auto';
    
    document.getElementById('transcode-options-container').style.display = 'none';
    document.getElementById('video-transcode-details').style.display = 'none';
    document.getElementById('audio-transcode-details').style.display = 'none';

    // Clear dynamic outputs and add initial row
    document.getElementById('stream-outputs-container').innerHTML = '';
    await loadNetworkInterfaces();
    addOutputRow('', '', 'udp');

    applyProgrammerStreamModalRestrictions();
    document.getElementById('modal-stream-title').textContent = 'Agregar Canal / Salida';
    modalStream.style.display = 'block';
});

// Bind add output button
const btnAddOutput = document.getElementById('btn-add-output');
if (btnAddOutput) {
    btnAddOutput.addEventListener('click', () => {
        addOutputRow('', '', 'udp');
    });
}

// Bind auto-suggest HLS path on stream name change
document.getElementById('stream-name').addEventListener('input', () => {
    const container = document.getElementById('stream-outputs-container');
    if (!container) return;
    const rows = container.querySelectorAll('.output-row');
    rows.forEach(row => {
        const typeSelect = row.querySelector('.output-type-select');
        const urlInput = row.querySelector('.output-url-input');
        if (typeSelect.value === 'hls') {
            if (!urlInput.value || urlInput.value.startsWith('www/hls/')) {
                suggestHlsPath(urlInput);
            }
        }
    });
});


async function editStream(id) {
    const stream = streams.find(s => s.id === id);
    if (!stream) return;

    document.getElementById('stream-id').value = stream.id;
    document.getElementById('stream-name').value = stream.name;
    document.getElementById('stream-input-id').value = stream.input_id;
    document.getElementById('stream-enabled').checked = stream.enabled;

    // Load transcoding fields
    const transEnabled = !!stream.transcode_enabled;
    const transVideo = !!stream.transcode_video;
    const transAudio = !!stream.transcode_audio;

    document.getElementById('stream-transcode-enabled').checked = transEnabled;
    document.getElementById('stream-transcode-video').checked = transVideo;
    document.getElementById('stream-video-input').value = stream.video_input_format || 'auto';
    document.getElementById('stream-video-output').value = stream.video_output_format || 'h264';
    document.getElementById('stream-transcode-audio').checked = transAudio;
    document.getElementById('stream-audio-input').value = stream.audio_input_format || 'auto';
    document.getElementById('stream-audio-output').value = stream.audio_output_format || 'aac';
    document.getElementById('stream-limit-bitrate').value = stream.limit_bitrate || '';

    // Set detected codec labels
    const detVideo = stream.detected_video_codec || stream.video_input_format || 'Auto';
    const detAudio = stream.detected_audio_codec || stream.audio_input_format || 'Auto';
    document.getElementById('stream-video-input-detected').textContent = detVideo.toUpperCase();
    document.getElementById('stream-audio-input-detected').textContent = detAudio.toUpperCase();

    document.getElementById('transcode-options-container').style.display = transEnabled ? 'block' : 'none';
    document.getElementById('video-transcode-details').style.display = transVideo ? 'block' : 'none';
    document.getElementById('audio-transcode-details').style.display = transAudio ? 'block' : 'none';

    // Clear dynamic outputs and populate them
    document.getElementById('stream-outputs-container').innerHTML = '';
    await loadNetworkInterfaces();
    if (stream.outputs && stream.outputs.length > 0) {
        stream.outputs.forEach(out => {
            addOutputRow(out.url, out.output_interface, out.type);
        });
    } else {
        // Fallback to legacy single output format
        addOutputRow(stream.output_url || '', stream.output_interface || '', 'udp');
    }

    populateProgramsFromSelectedInput(stream.input_id, stream.program_number, stream.video_filename);
    applyProgrammerStreamModalRestrictions();

    document.getElementById('modal-stream-title').textContent = 'Editar Canal / Salida';
    modalStream.style.display = 'block';
}

async function deleteStream(id) {
    if (confirm('¿Estás seguro de eliminar este canal?')) {
        const res = await apiCall(`/api/streams/${id}`, {
            method: 'DELETE'
        });
        if (res) {
            fetchStreams();
        }
    }
}

formStream.addEventListener('submit', async (e) => {
    e.preventDefault();
    const id = document.getElementById('stream-id').value;
    const name = document.getElementById('stream-name').value;
    const input_id = document.getElementById('stream-input-id').value;
    const enabled = document.getElementById('stream-enabled').checked;

    const transcode_enabled = document.getElementById('stream-transcode-enabled').checked;
    const transcode_video = document.getElementById('stream-transcode-video').checked;
    const video_input_format = document.getElementById('stream-video-input').value;
    const video_output_format = document.getElementById('stream-video-output').value;
    const transcode_audio = document.getElementById('stream-transcode-audio').checked;
    const audio_input_format = document.getElementById('stream-audio-input').value;
    const audio_output_format = document.getElementById('stream-audio-output').value;
    const limit_bitrate = parseInt(document.getElementById('stream-limit-bitrate').value) || 0;

    const inputSource = inputs.find(i => i.id === input_id);
    const isVideoPack = inputSource && inputSource.is_video_pack;

    let program_number = 1;
    let video_filename = "";

    if (isVideoPack) {
        video_filename = document.getElementById('stream-video-filename').value;
        if (!video_filename) {
            alert('Por favor selecciona un archivo de video.');
            return;
        }
    } else {
        program_number = parseInt(document.getElementById('stream-program').value);
        if (isNaN(program_number)) {
            alert('Por favor selecciona un programa válido.');
            return;
        }
    }

    // Collect dynamic outputs
    const outputs = [];
    const container = document.getElementById('stream-outputs-container');
    if (container) {
        const rows = container.querySelectorAll('.output-row');
        rows.forEach(row => {
            const type = row.querySelector('.output-type-select').value;
            const url = row.querySelector('.output-url-input').value.trim();
            const output_interface = row.querySelector('.output-iface-select').value;
            if (url) {
                outputs.push({ url, output_interface, type });
            }
        });
    }

    if (outputs.length === 0) {
        alert('Por favor agrega al menos un destino de salida.');
        return;
    }

    const payload = {
        name,
        input_id,
        program_number,
        output_url: outputs[0].url,
        enabled,
        output_interface: outputs[0].output_interface,
        outputs,
        transcode_enabled,
        transcode_video,
        video_input_format,
        video_output_format,
        transcode_audio,
        audio_input_format,
        audio_output_format,
        limit_bitrate,
        video_filename
    };

    let res;
    if (id) {
        // Edit
        res = await apiCall(`/api/streams/${id}`, {
            method: 'PUT',
            body: JSON.stringify(payload)
        });
    } else {
        // New
        res = await apiCall('/api/streams', {
            method: 'POST',
            body: JSON.stringify(payload)
        });
    }

    if (res) {
        modalStream.style.display = 'none';
        fetchStreams();
    } else {
        alert('Error al guardar el canal. Revisa los logs o la salida.');
    }
});

// --- PROBE PACK LOGIC ---
async function probeInput(url, id = '') {
    currentProbedUrl = url;
    currentProbedInputId = id;
    
    document.getElementById('probe-pack-desc').textContent = `Conectando a la fuente: ${url}`;
    document.getElementById('probe-loading').style.display = 'block';
    document.getElementById('probe-results').style.display = 'none';
    modalProbe.style.display = 'block';

    const res = await apiCall('/api/inputs/probe', {
        method: 'POST',
        body: JSON.stringify({ url })
    });

    document.getElementById('probe-loading').style.display = 'none';

    if (res && res.success) {
        const tbody = document.getElementById('probe-tbody');
        tbody.innerHTML = '';

        if (!res.programs || res.programs.length === 0) {
            tbody.innerHTML = '<tr><td colspan="4" style="text-align:center;">No se detectaron programas (PAT/PMT vacía o error de red).</td></tr>';
        } else {
            res.programs.forEach(prog => {
                const tr = document.createElement('tr');
                const pidsList = prog.pids ? prog.pids.map(pid => `<span class="pid-tag">${pid}</span>`).join(' ') : 'Sin PIDs';
                
                tr.innerHTML = `
                    <td style="font-weight:600; font-family:var(--font-mono);">${prog.program_number}</td>
                    <td>${prog.name}</td>
                    <td>${pidsList}</td>
                    <td>
                        <button class="btn btn-primary btn-small" onclick="mapProbedProgram('${prog.program_number}', '${prog.name.replace(/'/g, "\\'")}')">Mapear Canal</button>
                    </td>
                `;
                tbody.appendChild(tr);
            });
        }
        document.getElementById('probe-results').style.display = 'block';
    } else {
        document.getElementById('probe-pack-desc').innerHTML = `<span style="color:var(--red-alert);">Error al analizar flujo: ${res ? res.error : 'Sin respuesta'}</span>`;
        document.getElementById('probe-tbody').innerHTML = '<tr><td colspan="4" style="text-align:center;">Fallo de conexión o formato de señal incorrecto.</td></tr>';
        document.getElementById('probe-results').style.display = 'block';
    }
}

// Triggered from probe table to map a program to a channel
async function mapProbedProgram(progNum, progName) {
    modalProbe.style.display = 'none';
    
    // Fill the add stream modal
    document.getElementById('stream-id').value = '';
    formStream.reset();
    document.getElementById('stream-name').value = progName;
    document.getElementById('stream-enabled').checked = true;
    
    if (currentProbedInputId) {
        document.getElementById('stream-input-id').value = currentProbedInputId;
        populateProgramsFromSelectedInput(currentProbedInputId, progNum);
    }
    
    // Clear dynamic outputs and populate with initial preset
    document.getElementById('stream-outputs-container').innerHTML = '';
    await loadNetworkInterfaces();
    
    const randomIpEnd = 100 + parseInt(progNum);
    const presetUrl = `udp://239.2.2.${randomIpEnd}:9009`;
    addOutputRow(presetUrl, '', 'udp');
    
    applyProgrammerStreamModalRestrictions();
    document.getElementById('modal-stream-title').textContent = 'Agregar Canal / Salida';
    modalStream.style.display = 'block';
}

// --- SETTINGS MODAL & TABS LOGIC ---
const btnConfig = document.getElementById('btn-config');
const btnLogout = document.getElementById('btn-logout');

if (btnConfig) {
    btnConfig.addEventListener('click', () => {
        modalSettings.style.display = 'block';
        openSettingsTab('settings-users');
    });
}

if (btnLogout) {
    btnLogout.addEventListener('click', () => {
        window.location.href = '/logout';
    });
}

function openSettingsTab(tabId) {
    // Stop logs polling if active
    if (settingsLogsInterval) {
        clearInterval(settingsLogsInterval);
        settingsLogsInterval = null;
    }

    // Toggle active class on tab buttons
    document.querySelectorAll('.tab-btn').forEach(btn => {
        if (btn.getAttribute('data-tab') === tabId) {
            btn.classList.add('active');
        } else {
            btn.classList.remove('active');
        }
    });

    // Toggle active class on tab panes
    document.querySelectorAll('.tab-pane').forEach(pane => {
        if (pane.id === tabId) {
            pane.classList.add('active');
        } else {
            pane.classList.remove('active');
        }
    });

    // Load corresponding data
    if (tabId === 'settings-users') {
        fetchUsers();
    } else if (tabId === 'settings-channel-logs') {
        fetchChannelLogs();
        settingsLogsInterval = setInterval(fetchChannelLogs, 1000);
    } else if (tabId === 'settings-user-logs') {
        fetchUserActivityLogs();
    } else if (tabId === 'settings-global') {
        loadGlobalSettings();
    } else if (tabId === 'settings-messages') {
        fetchMessages();
        renderMessageChannelsCheckboxSelection();
    } else if (tabId === 'settings-sessions') {
        fetchActiveSessions();
        settingsLogsInterval = setInterval(fetchActiveSessions, 3000);
    } else if (tabId === 'settings-blocked-ips') {
        fetchBlockedIPs();
    }
}

// Attach event listeners to tab buttons dynamically
document.querySelectorAll('.tab-btn').forEach(btn => {
    btn.addEventListener('click', () => {
        openSettingsTab(btn.getAttribute('data-tab'));
    });
});

// --- TAB 1: USERS SECTION ---
let loadedUsers = [];
async function fetchUsers() {
    const data = await apiCall('/api/users');
    loadedUsers = data || [];
    const tbody = document.getElementById('users-tbody');
    if (!tbody) return;
    
    tbody.innerHTML = '';
    
    if (data && data.length > 0) {
        data.forEach(u => {
            const tr = document.createElement('tr');
            
            let actions = '';
            if (currentUser && currentUser.role === 'Consulta') {
                if (u.username === currentUser.username) {
                    actions = `<button class="btn btn-primary btn-small" onclick="editUser('${u.username}', '${u.role}')">Cambiar Contraseña</button>`;
                }
            } else {
                let deleteBtn = `<button class="btn btn-danger btn-small" onclick="deleteUser('${u.username}')">Eliminar</button>`;
                if (u.username === currentUser.username) {
                    deleteBtn = '';
                }
                actions = `
                    <button class="btn btn-primary btn-small" onclick="editUser('${u.username}', '${u.role}')">Editar</button>
                    ${deleteBtn}
                `;
            }
            
            tr.innerHTML = `
                <td style="font-weight:600;">${u.username}</td>
                <td><span class="badge-ver">${u.role}</span></td>
                <td><div style="display:flex; gap:6px;">${actions}</div></td>
            `;
            tbody.appendChild(tr);
        });
    } else {
        tbody.innerHTML = '<tr><td colspan="3" style="text-align:center; color:var(--text-muted);">No hay usuarios cargados</td></tr>';
    }
    
    const roleSelect = document.getElementById('user-role');
    if (roleSelect && currentUser) {
        roleSelect.innerHTML = '';
        if (currentUser.role === 'SuperAdmin') {
            roleSelect.innerHTML = `
                <option value="Consulta">Consulta</option>
                <option value="Programadores">Programadores</option>
                <option value="Admin">Admin</option>
                <option value="SuperAdmin">SuperAdmin</option>
            `;
        } else if (currentUser.role === 'Admin') {
            roleSelect.innerHTML = `
                <option value="Consulta">Consulta</option>
                <option value="Programadores">Programadores</option>
                <option value="Admin">Admin</option>
            `;
        }
    }
}

function isVideoFileUrl(url) {
    if (!url) return true;
    const lower = url.toLowerCase();
    return !(
        lower.startsWith('srt://') ||
        lower.startsWith('udp://') ||
        lower.startsWith('rtmp://') ||
        lower.startsWith('rtsp://') ||
        lower.startsWith('rtp://') ||
        lower.startsWith('http://') ||
        lower.startsWith('https://')
    );
}

async function renderAllowedStreamsCheckboxes(selectedStreams = []) {
    const listContainer = document.getElementById('user-allowed-packs-list');
    if (!listContainer) return;
    listContainer.innerHTML = '';
    
    const inputs = await apiCall('/api/inputs') || [];
    const videoPackIds = inputs.filter(inp => inp.is_video_pack).map(inp => inp.id);
    
    const streams = await apiCall('/api/streams') || [];
    const videoStreams = streams.filter(s => videoPackIds.includes(s.input_id));
    
    if (videoStreams.length === 0) {
        listContainer.innerHTML = '<div style="color:var(--text-muted); font-size:0.9em;">No hay canales de video disponibles</div>';
        return;
    }
    
    videoStreams.forEach(stream => {
        const div = document.createElement('div');
        div.style.margin = '4px 0';
        div.style.display = 'flex';
        div.style.alignItems = 'center';
        
        const checked = selectedStreams.includes(stream.id) ? 'checked' : '';
        div.innerHTML = `
            <input type="checkbox" id="user-stream-${stream.id}" value="${stream.id}" ${checked} style="margin-right: 8px; cursor: pointer;">
            <label for="user-stream-${stream.id}" style="cursor: pointer; font-size: 0.9em;">${stream.name}</label>
        `;
        listContainer.appendChild(div);
    });
}

// Bind listener on role dropdown to show allowed streams checkboxes when Programadores is selected
document.addEventListener('DOMContentLoaded', () => {
    const roleSelect = document.getElementById('user-role');
    if (roleSelect) {
        roleSelect.addEventListener('change', async (e) => {
            const group = document.getElementById('user-allowed-packs-group');
            if (group) {
                if (e.target.value === 'Programadores') {
                    group.style.display = 'block';
                    await renderAllowedStreamsCheckboxes();
                } else {
                    group.style.display = 'none';
                }
            }
        });
    }
});

function editUser(username, role) {
    const title = document.getElementById('user-form-title');
    const action = document.getElementById('user-action');
    const origUsername = document.getElementById('user-original-username');
    const uField = document.getElementById('user-username');
    const pField = document.getElementById('user-password');
    const rField = document.getElementById('user-role');
    const cancelBtn = document.getElementById('btn-cancel-user');
    
    if (title) title.textContent = `Editar Usuario: ${username}`;
    if (action) action.value = 'edit';
    if (origUsername) origUsername.value = username;
    
    if (uField) {
        uField.value = username;
        uField.disabled = true;
    }
    
    // Toggle current password and confirm password groups depending on own profile edit
    const currentGroup = document.getElementById('user-current-password-group');
    const confirmGroup = document.getElementById('user-confirm-password-group');
    const currentInput = document.getElementById('user-current-password');
    const confirmInput = document.getElementById('user-confirm-password');
    const passwordLabel = document.getElementById('user-password-label');
    
    if (currentUser && username === currentUser.username) {
        if (currentGroup) currentGroup.style.display = 'block';
        if (confirmGroup) confirmGroup.style.display = 'block';
        if (currentInput) {
            currentInput.value = '';
            currentInput.required = true;
        }
        if (confirmInput) {
            confirmInput.value = '';
            confirmInput.required = true;
        }
        if (passwordLabel) passwordLabel.textContent = 'Nueva Contraseña';
        if (pField) {
            pField.value = '';
            pField.required = true;
        }
    } else {
        if (currentGroup) currentGroup.style.display = 'none';
        if (confirmGroup) confirmGroup.style.display = 'none';
        if (currentInput) {
            currentInput.value = '';
            currentInput.required = false;
        }
        if (confirmInput) {
            confirmInput.value = '';
            confirmInput.required = false;
        }
        if (passwordLabel) passwordLabel.textContent = 'Contraseña';
        if (pField) {
            pField.value = '';
            pField.required = false;
        }
    }
    
    if (rField) {
        rField.value = role;
        if (currentUser && currentUser.role === 'Consulta') {
            rField.disabled = true;
        } else {
            rField.disabled = false;
        }
        
        // Show/hide allowed streams checklist dynamically when editing
        const group = document.getElementById('user-allowed-packs-group');
        if (group) {
            if (role === 'Programadores' && (currentUser.role === 'SuperAdmin' || currentUser.role === 'Admin')) {
                group.style.display = 'block';
                const uObj = loadedUsers.find(u => u.username === username);
                const selectedStreams = uObj && uObj.allowed_streams ? uObj.allowed_streams : [];
                renderAllowedStreamsCheckboxes(selectedStreams);
            } else {
                group.style.display = 'none';
            }
        }
    }
    
    if (cancelBtn) cancelBtn.style.display = 'inline-block';
}
window.editUser = editUser;

function resetUserForm() {
    const form = document.getElementById('form-user');
    if (form) form.reset();
    
    const title = document.getElementById('user-form-title');
    const action = document.getElementById('user-action');
    const origUsername = document.getElementById('user-original-username');
    const uField = document.getElementById('user-username');
    const pField = document.getElementById('user-password');
    const rField = document.getElementById('user-role');
    const cancelBtn = document.getElementById('btn-cancel-user');
    
    if (title) title.textContent = 'Crear Nuevo Usuario';
    if (action) action.value = 'create';
    if (origUsername) origUsername.value = '';
    
    if (uField) {
        if (currentUser && currentUser.role === 'Consulta') {
            uField.value = currentUser.username;
            uField.disabled = true;
        } else {
            uField.value = '';
            uField.disabled = false;
        }
    }
    
    if (pField) {
        pField.required = true;
    }
    
    const currentGroup = document.getElementById('user-current-password-group');
    const confirmGroup = document.getElementById('user-confirm-password-group');
    const currentInput = document.getElementById('user-current-password');
    const confirmInput = document.getElementById('user-confirm-password');
    const passwordLabel = document.getElementById('user-password-label');
    
    if (currentGroup) currentGroup.style.display = 'none';
    if (confirmGroup) confirmGroup.style.display = 'none';
    if (currentInput) {
        currentInput.value = '';
        currentInput.required = false;
    }
    if (confirmInput) {
        confirmInput.value = '';
        confirmInput.required = false;
    }
    if (passwordLabel) passwordLabel.textContent = 'Contraseña';
    
    if (rField) {
        rField.disabled = false;
        rField.value = 'Consulta';
    }
    
    // Clear allowed streams checklist
    const group = document.getElementById('user-allowed-packs-group');
    if (group) group.style.display = 'none';
    const listContainer = document.getElementById('user-allowed-packs-list');
    if (listContainer) listContainer.innerHTML = '';
    
    if (cancelBtn) cancelBtn.style.display = 'none';
}

const btnCancelUser = document.getElementById('btn-cancel-user');
if (btnCancelUser) {
    btnCancelUser.addEventListener('click', resetUserForm);
}

const formUser = document.getElementById('form-user');
if (formUser) {
    formUser.addEventListener('submit', async (e) => {
        e.preventDefault();
        
        const action = document.getElementById('user-action').value;
        const username = document.getElementById('user-username').value;
        const password = document.getElementById('user-password').value;
        const role = document.getElementById('user-role').value;
        const origUsername = document.getElementById('user-original-username').value;
        
        const allowed_streams = [];
        if (role === 'Programadores') {
            const checkboxes = document.querySelectorAll('#user-allowed-packs-list input[type="checkbox"]:checked');
            checkboxes.forEach(cb => allowed_streams.push(cb.value));
        }
        
        let res;
        if (action === 'create') {
            res = await apiCall('/api/users', {
                method: 'POST',
                body: JSON.stringify({ username, password, role, allowed_streams })
            });
        } else {
            let current_password = '';
            if (currentUser && origUsername === currentUser.username) {
                current_password = document.getElementById('user-current-password').value;
                const confirm_password = document.getElementById('user-confirm-password').value;
                if (password !== confirm_password) {
                    alert('Las nuevas contraseñas no coinciden.');
                    return;
                }
            }
            res = await apiCall(`/api/users/${origUsername}`, {
                method: 'PUT',
                body: JSON.stringify({ password, role, allowed_streams, current_password })
            });
        }
        
        if (res && res.success) {
            resetUserForm();
            fetchUsers();
        } else {
            alert('Error al guardar el usuario: ' + (res ? res.error : 'Operación no permitida'));
        }
    });
}

async function deleteUser(username) {
    if (confirm(`¿Estás seguro de eliminar al usuario '${username}'?`)) {
        const res = await apiCall(`/api/users/${username}`, {
            method: 'DELETE'
        });
        if (res && res.success) {
            fetchUsers();
        } else {
            alert('Error al eliminar usuario: ' + (res ? res.error : 'Operación no permitida'));
        }
    }
}
window.deleteUser = deleteUser;

// --- TAB 2: CHANNEL LOGS SECTION ---
async function fetchChannelLogs() {
    const data = await apiCall('/api/logs');
    const logsBox = document.getElementById('channel-logs-box');
    if (!logsBox) return;
    
    const isScrolledToBottom = logsBox.scrollHeight - logsBox.clientHeight <= logsBox.scrollTop + 50;
    logsBox.innerHTML = '';
    
    if (data) {
        data.forEach(log => {
            const line = document.createElement('div');
            line.className = 'log-line';
            if (log.includes('[INFO]')) line.classList.add('info');
            else if (log.includes('[WARN]')) line.classList.add('warn');
            else if (log.includes('[ERROR]')) line.classList.add('error');
            line.textContent = log;
            logsBox.appendChild(line);
        });
        
        if (isScrolledToBottom) {
            logsBox.scrollTop = logsBox.scrollHeight;
        }
    }
}

const btnClearChannelLogs = document.getElementById('btn-clear-channel-logs-ui');
if (btnClearChannelLogs) {
    btnClearChannelLogs.addEventListener('click', () => {
        const logsBox = document.getElementById('channel-logs-box');
        if (logsBox) logsBox.innerHTML = '';
    });
}

// --- TAB 3: USER ACTIVITY LOGS SECTION ---
async function fetchUserActivityLogs() {
    const data = await apiCall('/api/user_logs');
    const tbody = document.getElementById('user-logs-tbody');
    if (!tbody) return;
    
    tbody.innerHTML = '';
    
    if (data && data.length > 0) {
        data.forEach(log => {
            const tr = document.createElement('tr');
            tr.innerHTML = `
                <td style="font-family:var(--font-mono); font-size:12px; white-space:nowrap;">${log.timestamp}</td>
                <td style="font-weight:600;">${log.username}</td>
                <td style="font-size:13px; color:var(--text-secondary);">${log.action}</td>
            `;
            tbody.appendChild(tr);
        });
    } else {
        tbody.innerHTML = '<tr><td colspan="3" style="text-align:center; color:var(--text-muted);">No hay registros de actividad</td></tr>';
    }
}

// --- TAB 4: GLOBAL SETTINGS SECTION ---
async function loadGlobalSettings() {
    const interfaces = await apiCall('/api/interfaces');
    const settings = await apiCall('/api/settings');
    const select = document.getElementById('global-output-interface');
    if (!select) return;
    
    select.innerHTML = '<option value="">Por defecto (Cualquiera / Auto)</option>';
    if (interfaces) {
        interfaces.forEach(iface => {
            const option = document.createElement('option');
            option.value = iface.name;
            let suffix = '';
            if (iface.is_loopback) suffix = ' (Loopback)';
            else if (!iface.is_up) suffix = ' (Desconectado)';
            option.textContent = `${iface.name} - ${iface.ip}${suffix}`;
            select.appendChild(option);
        });
    }
    
    if (settings) {
        if (settings.output_interface) {
            select.value = settings.output_interface;
        } else {
            select.value = '';
        }
        
        const nvencSelect = document.getElementById('global-nvenc-preset');
        if (nvencSelect && settings.nvenc_preset) {
            nvencSelect.value = settings.nvenc_preset;
        }
        const cpuSelect = document.getElementById('global-cpu-preset');
        if (cpuSelect && settings.cpu_preset) {
            cpuSelect.value = settings.cpu_preset;
        }
    } else {
        select.value = '';
    }
}

const formGlobal = document.getElementById('form-global');
if (formGlobal) {
    formGlobal.addEventListener('submit', async (e) => {
        e.preventDefault();
        const iface = document.getElementById('global-output-interface').value;
        const nvenc_preset = document.getElementById('global-nvenc-preset').value;
        const cpu_preset = document.getElementById('global-cpu-preset').value;
        const res = await apiCall('/api/settings', {
            method: 'POST',
            body: JSON.stringify({
                output_interface: iface,
                nvenc_preset: nvenc_preset,
                cpu_preset: cpu_preset
            })
        });
        if (res && res.success) {
            alert('Configuración guardada correctamente.');
        } else {
            alert('Error al guardar la configuración: ' + (res ? res.error : 'Operación no permitida'));
        }
    });
}

// --- TAB 6: ACTIVE HLS SESSIONS ---
async function fetchActiveSessions() {
    const sessions = await apiCall('/api/sessions');
    const tbody = document.getElementById('sessions-tbody');
    const badge = document.getElementById('sessions-count-badge');
    if (!tbody) return;

    if (!sessions || sessions.length === 0) {
        tbody.innerHTML = `<tr><td colspan="5" class="no-data">No hay sesiones activas de HLS en este momento.</td></tr>`;
        if (badge) badge.textContent = 'Total: 0';
        return;
    }

    if (badge) badge.textContent = `Total: ${sessions.length}`;

    tbody.innerHTML = sessions.map(s => {
        const uptimeStr = formatUptime(s.uptime_secs);
        return `
            <tr>
                <td><span class="badge-type" style="background:rgba(16,185,129,0.15); color:var(--green-alert); border-color:rgba(16,185,129,0.3);">HLS</span> <strong>${s.stream_name}</strong></td>
                <td style="font-family:var(--font-mono);">${s.ip}</td>
                <td style="font-size:12px; color:var(--text-secondary); max-width: 250px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;" title="${s.user_agent}">${s.user_agent || 'Desconocido'}</td>
                <td style="font-family:var(--font-mono);">${uptimeStr}</td>
                <td style="text-align:center;">
                    <button class="btn btn-danger btn-small" onclick="blockIPFromSession('${s.ip}')" title="Bloquear IP">Bloquear</button>
                </td>
            </tr>
        `;
    }).join('');
}

function formatUptime(secs) {
    if (secs < 60) return `${secs}s`;
    const mins = Math.floor(secs / 60);
    const s = secs % 60;
    if (mins < 60) return `${mins}m ${s}s`;
    const hrs = Math.floor(mins / 60);
    const m = mins % 60;
    return `${hrs}h ${m}m`;
}

async function blockIPFromSession(ip) {
    if (confirm(`¿Estás seguro de que deseas bloquear la dirección IP ${ip}? Perderá el acceso de inmediato.`)) {
        const res = await apiCall('/api/blocked_ips', {
            method: 'POST',
            body: JSON.stringify({ ip })
        });
        if (res && res.success) {
            fetchActiveSessions();
        } else {
            alert('No se pudo bloquear la IP: ' + (res ? res.error : 'error desconocido'));
        }
    }
}

// --- TAB 7: BLOCKED IPS ---
async function fetchBlockedIPs() {
    const blocked = await apiCall('/api/blocked_ips');
    const tbody = document.getElementById('blocked-ips-tbody');
    if (!tbody) return;

    if (!blocked || blocked.length === 0) {
        tbody.innerHTML = `<tr><td colspan="2" class="no-data">No hay direcciones IP bloqueadas actualmente.</td></tr>`;
        return;
    }

    tbody.innerHTML = blocked.map(ip => {
        return `
            <tr>
                <td style="font-family:var(--font-mono); font-weight:600; color:var(--red-alert);">${ip}</td>
                <td style="text-align:center;">
                    <button class="btn btn-secondary btn-small" onclick="unblockIP('${ip}')" title="Permitir IP">Desbloquear</button>
                </td>
            </tr>
        `;
    }).join('');
}

async function unblockIP(ip) {
    if (confirm(`¿Desbloquear la dirección IP ${ip}?`)) {
        const res = await apiCall(`/api/blocked_ips/${ip}`, {
            method: 'DELETE'
        });
        if (res && res.success) {
            fetchBlockedIPs();
        } else {
            alert('No se pudo desbloquear la IP: ' + (res ? res.error : 'error desconocido'));
        }
    }
}

// Setup block IP form listener
const blockIpForm = document.getElementById('form-block-ip');
if (blockIpForm) {
    blockIpForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const ipInput = document.getElementById('block-ip-address');
        if (!ipInput) return;
        const ip = ipInput.value.trim();
        if (!ip) return;

        const res = await apiCall('/api/blocked_ips', {
            method: 'POST',
            body: JSON.stringify({ ip })
        });
        if (res && res.success) {
            ipInput.value = '';
            fetchBlockedIPs();
        } else {
            alert('No se pudo bloquear la IP: ' + (res ? res.error : 'error desconocido'));
        }
    });
}

// --- TAB 5: SCHEDULED MESSAGES SECTION ---
let messages = [];

// DOM elements for messages
const formMessage = document.getElementById('form-message');
const messageTargetType = document.getElementById('message-target-type');
const messageChannelsSelectionGroup = document.getElementById('message-channels-selection-group');
const messageChannelsCheckboxes = document.getElementById('message-channels-checkboxes');
const btnCancelMessage = document.getElementById('btn-cancel-message');

if (messageTargetType) {
    messageTargetType.addEventListener('change', () => {
        if (messageTargetType.value === 'select') {
            messageChannelsSelectionGroup.style.display = 'block';
        } else {
            messageChannelsSelectionGroup.style.display = 'none';
        }
    });
}

async function renderMessageChannelsCheckboxSelection() {
    if (!messageChannelsCheckboxes) return;
    
    const data = await apiCall('/api/streams');
    messageChannelsCheckboxes.innerHTML = '';
    if (data) {
        data.forEach(stream => {
            const div = document.createElement('div');
            div.style.display = 'flex';
            div.style.alignItems = 'center';
            div.style.gap = '8px';
            div.style.margin = '4px 0';
            
            const checkbox = document.createElement('input');
            checkbox.type = 'checkbox';
            checkbox.name = 'message-channels';
            checkbox.value = stream.id;
            checkbox.id = `msg-ch-${stream.id}`;
            
            const label = document.createElement('label');
            label.htmlFor = `msg-ch-${stream.id}`;
            label.textContent = stream.name;
            label.style.fontWeight = 'normal';
            label.style.cursor = 'pointer';
            
            div.appendChild(checkbox);
            div.appendChild(label);
            messageChannelsCheckboxes.appendChild(div);
        });
    }
}

async function fetchMessages() {
    const data = await apiCall('/api/messages');
    const tbody = document.getElementById('messages-tbody');
    if (!tbody) return;
    tbody.innerHTML = '';
    
    if (data && Array.isArray(data)) {
        messages = data;
        messages.forEach(msg => {
            const tr = document.createElement('tr');
            
            const tdText = document.createElement('td');
            tdText.textContent = msg.text;
            tdText.style.fontWeight = 'bold';
            
            const tdSchedule = document.createElement('td');
            const start = msg.start_time ? msg.start_time.replace('T', ' ') : '';
            const end = msg.end_time ? msg.end_time.replace('T', ' ') : '';
            tdSchedule.innerHTML = `<small>Desde: ${start}<br>Hasta: ${end}</small>`;
            
            const tdChannels = document.createElement('td');
            if (msg.all_channels) {
                tdChannels.textContent = 'Todos los canales';
            } else {
                tdChannels.textContent = `${msg.channel_ids ? msg.channel_ids.length : 0} canal(es)`;
            }
            
            const tdActions = document.createElement('td');
            
            const btnEdit = document.createElement('button');
            btnEdit.className = 'btn btn-secondary btn-small';
            btnEdit.textContent = 'Editar';
            btnEdit.style.marginRight = '5px';
            btnEdit.onclick = () => editMessage(msg);
            
            const btnDel = document.createElement('button');
            btnDel.className = 'btn btn-danger btn-small';
            btnDel.textContent = 'Eliminar';
            btnDel.onclick = () => deleteMessage(msg.id);
            
            tdActions.appendChild(btnEdit);
            tdActions.appendChild(btnDel);
            
            tr.appendChild(tdText);
            tr.appendChild(tdSchedule);
            tr.appendChild(tdChannels);
            tr.appendChild(tdActions);
            tbody.appendChild(tr);
        });
    }
}

function editMessage(msg) {
    document.getElementById('message-form-title').textContent = 'Editar Mensaje';
    document.getElementById('message-action').value = 'edit';
    document.getElementById('message-id').value = msg.id;
    document.getElementById('message-text').value = msg.text;
    document.getElementById('message-start-time').value = msg.start_time;
    document.getElementById('message-end-time').value = msg.end_time;
    
    if (msg.all_channels) {
        messageTargetType.value = 'all';
        messageChannelsSelectionGroup.style.display = 'none';
    } else {
        messageTargetType.value = 'select';
        messageChannelsSelectionGroup.style.display = 'block';
        
        // Uncheck all first
        document.querySelectorAll('input[name="message-channels"]').forEach(cb => cb.checked = false);
        // Check selected
        msg.channel_ids.forEach(chId => {
            const cb = document.getElementById(`msg-ch-${chId}`);
            if (cb) cb.checked = true;
        });
    }
    
    if (btnCancelMessage) btnCancelMessage.style.display = 'inline-block';
}

function resetMessageForm() {
    document.getElementById('message-form-title').textContent = 'Programar Nuevo Mensaje';
    document.getElementById('message-action').value = 'create';
    document.getElementById('message-id').value = '';
    document.getElementById('message-text').value = '';
    document.getElementById('message-start-time').value = '';
    document.getElementById('message-end-time').value = '';
    messageTargetType.value = 'all';
    messageChannelsSelectionGroup.style.display = 'none';
    document.querySelectorAll('input[name="message-channels"]').forEach(cb => cb.checked = false);
    if (btnCancelMessage) btnCancelMessage.style.display = 'none';
}

if (btnCancelMessage) {
    btnCancelMessage.addEventListener('click', resetMessageForm);
}

if (formMessage) {
    formMessage.addEventListener('submit', async (e) => {
        e.preventDefault();
        
        const action = document.getElementById('message-action').value;
        const msgId = document.getElementById('message-id').value;
        const text = document.getElementById('message-text').value;
        const startTime = document.getElementById('message-start-time').value;
        const endTime = document.getElementById('message-end-time').value;
        const targetType = messageTargetType.value;
        const allChannels = targetType === 'all';
        
        const channelIds = [];
        if (!allChannels) {
            document.querySelectorAll('input[name="message-channels"]:checked').forEach(cb => {
                channelIds.push(cb.value);
            });
            if (channelIds.length === 0) {
                alert('Por favor selecciona al menos un canal.');
                return;
            }
        }
        
        const payload = {
            text: text,
            start_time: startTime,
            end_time: endTime,
            all_channels: allChannels,
            channel_ids: channelIds
        };
        
        let url = '/api/messages';
        let method = 'POST';
        if (action === 'edit') {
            url = `/api/messages/${msgId}`;
            method = 'PUT';
        }
        
        const res = await apiCall(url, {
            method: method,
            body: JSON.stringify(payload)
        });
        
        if (res && res.success) {
            alert('Mensaje guardado correctamente.');
            resetMessageForm();
            fetchMessages();
        } else {
            alert('Error al guardar el mensaje: ' + (res ? res.error : 'Operación no permitida'));
        }
    });
}

async function deleteMessage(id) {
    if (!confirm('¿Estás seguro de que deseas eliminar este mensaje programado?')) return;
    const res = await apiCall(`/api/messages/${id}`, {
        method: 'DELETE'
    });
    if (res && res.success) {
        const currentFormId = document.getElementById('message-id').value;
        if (currentFormId === id) {
            resetMessageForm();
        }
        fetchMessages();
    } else {
        alert('Error al eliminar el mensaje: ' + (res ? res.error : 'Operación no permitida'));
    }
}

// --- FILE EXPLORER LOGIC ---
let fsCurrentPathVal = '';
let fsSelectedFilePath = '';

function formatBytes(bytes) {
    if (bytes === 0) return '';
    const k = 1024;
    const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

async function loadDirectory(path) {
    fsListContainer.innerHTML = '<div style="padding: 12px; color: var(--text-muted); text-align: center;">Cargando...</div>';
    btnSelectFile.disabled = true;
    if (btnSelectFolder) {
        btnSelectFolder.textContent = 'Seleccionar Carpeta Actual';
    }
    fsSelectedFilePath = '';
    
    let url = '/api/fs/list';
    if (path) {
        url += `?path=${encodeURIComponent(path)}`;
    }
    
    const data = await apiCall(url);
    if (data && data.success) {
        fsCurrentPathVal = data.current_path;
        fsCurrentPath.textContent = data.current_path;
        fsListContainer.innerHTML = '';
        
        if (data.items.length === 0) {
            fsListContainer.innerHTML = '<div style="padding: 12px; color: var(--text-muted); text-align: center;">Carpeta vacía</div>';
            return;
        }
        
        data.items.forEach(item => {
            const div = document.createElement('div');
            div.className = `fs-item ${item.is_directory ? 'directory' : 'file'}`;
            div.dataset.path = item.path;
            div.dataset.isdir = item.is_directory;
            div.dataset.name = item.name;
            
            const icon = item.is_directory ? '📁' : '🎥';
            const sizeText = item.is_directory ? '' : formatBytes(item.size);
            
            div.innerHTML = `
                <span class="fs-icon">${icon}</span>
                <span class="fs-name" title="${item.name}">${item.name}</span>
                <span class="fs-size">${sizeText}</span>
            `;
            
            if (item.is_directory) {
                div.addEventListener('dblclick', () => {
                    loadDirectory(item.path);
                });
                div.addEventListener('click', () => {
                    document.querySelectorAll('.fs-item').forEach(el => el.classList.remove('selected'));
                    div.classList.add('selected');
                    btnSelectFile.disabled = true;
                    fsSelectedFilePath = '';
                    if (btnSelectFolder) {
                        btnSelectFolder.textContent = `Seleccionar Carpeta: ${item.name}`;
                    }
                });
            } else {
                div.addEventListener('click', () => {
                    document.querySelectorAll('.fs-item').forEach(el => el.classList.remove('selected'));
                    div.classList.add('selected');
                    fsSelectedFilePath = item.path;
                    btnSelectFile.disabled = false;
                    if (btnSelectFolder) {
                        btnSelectFolder.textContent = 'Seleccionar Carpeta Actual';
                    }
                });
                
                div.addEventListener('dblclick', () => {
                    fsSelectedFilePath = item.path;
                    selectAndCloseFile();
                });
            }
            
            fsListContainer.appendChild(div);
        });
    } else {
        fsListContainer.innerHTML = `<div style="padding: 12px; color: var(--red-alert); text-align: center;">Error al cargar la ruta: ${data ? data.error : 'Desconocido'}</div>`;
    }
}

function selectAndCloseFile() {
    if (fsSelectedFilePath) {
        document.getElementById('input-url').value = fsSelectedFilePath;
        modalFs.style.display = 'none';
    }
}

function selectAndCloseFolder() {
    let selectedPath = fsCurrentPathVal;
    
    // Check if a directory item is selected in the list
    const selectedItem = fsListContainer.querySelector('.fs-item.selected.directory');
    if (selectedItem) {
        selectedPath = selectedItem.dataset.path;
    }
    
    if (selectedPath) {
        document.getElementById('input-url').value = selectedPath;
        modalFs.style.display = 'none';
    }
}

btnBrowseFiles.addEventListener('click', () => {
    modalFs.style.display = 'block';
    loadDirectory(fsCurrentPathVal || '');
});

btnSelectFile.addEventListener('click', () => {
    selectAndCloseFile();
});

if (btnSelectFolder) {
    btnSelectFolder.addEventListener('click', () => {
        selectAndCloseFolder();
    });
}

// File upload handlers inside the File Explorer modal
const btnUploadFile = document.getElementById('btn-upload-file');
const fsUploadFile = document.getElementById('fs-upload-file');
const fsUploadStatus = document.getElementById('fs-upload-status');

if (btnUploadFile && fsUploadFile) {
    btnUploadFile.addEventListener('click', () => {
        fsUploadFile.click();
    });

    fsUploadFile.addEventListener('change', async (e) => {
        const file = e.target.files[0];
        if (!file) return;

        if (fsUploadStatus) {
            fsUploadStatus.textContent = 'Subiendo...';
            fsUploadStatus.style.color = 'var(--text-muted)';
        }

        const formData = new FormData();
        formData.append('file', file);

        try {
            const res = await fetch(`${API_BASE}/api/fs/upload?path=${encodeURIComponent(fsCurrentPathVal)}`, {
                method: 'POST',
                body: formData
            });

            if (!res.ok) {
                const errText = await res.text();
                throw new Error(errText || res.statusText);
            }

            const data = await res.json();
            if (data && data.success) {
                if (fsUploadStatus) {
                    fsUploadStatus.textContent = 'Subido con éxito';
                    fsUploadStatus.style.color = '#4caf50';
                }
                
                fsUploadFile.value = '';
                await loadDirectory(fsCurrentPathVal);
                
                fsSelectedFilePath = data.path;
                btnSelectFile.disabled = false;
                
                const items = fsListContainer.querySelectorAll('.fs-item');
                items.forEach(item => {
                    if (item.dataset.path === data.path) {
                        item.classList.add('selected');
                        item.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
                    } else {
                        item.classList.remove('selected');
                    }
                });
            } else {
                throw new Error(data ? data.error : 'Error desconocido');
            }
        } catch (err) {
            console.error('File upload failed:', err);
            if (fsUploadStatus) {
                fsUploadStatus.textContent = 'Error: ' + err.message;
                fsUploadStatus.style.color = '#f44336';
            }
        }
    });
}

// --- CLOSE MODALS ---
const closeButtons = [
    { btn: 'close-modal-input', modal: modalInput },
    { btn: 'close-modal-stream', modal: modalStream },
    { btn: 'close-modal-probe', modal: modalProbe },
    { btn: 'close-modal-settings', modal: modalSettings },
    { btn: 'close-modal-fs', modal: modalFs },
    { btn: 'btn-cancel-input', modal: modalInput },
    { btn: 'btn-cancel-stream', modal: modalStream },
    { btn: 'btn-close-fs', modal: modalFs },
    { btn: 'close-settings-modal-btn', modal: modalSettings }
];

closeButtons.forEach(cfg => {
    const el = document.getElementById(cfg.btn);
    if (el) {
        el.addEventListener('click', () => {
            cfg.modal.style.display = 'none';
            if (cfg.modal === modalSettings && settingsLogsInterval) {
                clearInterval(settingsLogsInterval);
                settingsLogsInterval = null;
            }
        });
    }
});

// Close when clicking outside modal contents
window.onclick = function(e) {
    if (e.target === modalInput) modalInput.style.display = 'none';
    if (e.target === modalStream) modalStream.style.display = 'none';
    if (e.target === modalProbe) modalProbe.style.display = 'none';
    if (e.target === modalFs) modalFs.style.display = 'none';
    if (e.target === modalSettings) {
        modalSettings.style.display = 'none';
        if (settingsLogsInterval) {
            clearInterval(settingsLogsInterval);
            settingsLogsInterval = null;
        }
    }
};

// Transcode UI Interactive Visibility
const transEnabledEl = document.getElementById('stream-transcode-enabled');
if (transEnabledEl) {
    transEnabledEl.addEventListener('change', function(e) {
        const optContainer = document.getElementById('transcode-options-container');
        if (optContainer) optContainer.style.display = e.target.checked ? 'block' : 'none';
    });
}

const transVideoEl = document.getElementById('stream-transcode-video');
if (transVideoEl) {
    transVideoEl.addEventListener('change', function(e) {
        const vidDetails = document.getElementById('video-transcode-details');
        if (vidDetails) vidDetails.style.display = e.target.checked ? 'block' : 'none';
    });
}

const transAudioEl = document.getElementById('stream-transcode-audio');
if (transAudioEl) {
    transAudioEl.addEventListener('change', function(e) {
        const audDetails = document.getElementById('audio-transcode-details');
        if (audDetails) audDetails.style.display = e.target.checked ? 'block' : 'none';
    });
}

function applyRoleRestrictions() {
    if (!currentUser) return;
    
    if (currentUser.role === 'Consulta' || currentUser.role === 'Programadores') {
        const btnNewInput = document.getElementById('btn-new-input');
        if (btnNewInput) btnNewInput.style.display = 'none';
        
        const btnNewStream = document.getElementById('btn-new-stream');
        if (btnNewStream) {
            // Programmers can create new channels (streams) for their allowed packs, but Consulta cannot
            btnNewStream.style.display = (currentUser.role === 'Programadores') ? 'inline-block' : 'none';
        }
        
        const tabMessages = document.getElementById('tab-messages-btn');
        if (tabMessages) tabMessages.style.display = 'none';
        const tabChannelLogs = document.getElementById('tab-channel-logs-btn');
        if (tabChannelLogs) tabChannelLogs.style.display = 'none';
        const tabUserLogs = document.getElementById('tab-user-logs-btn');
        if (tabUserLogs) tabUserLogs.style.display = 'none';
        const tabGlobal = document.getElementById('tab-global-btn');
        if (tabGlobal) tabGlobal.style.display = 'none';
        const tabSessions = document.getElementById('tab-sessions-btn');
        if (tabSessions) tabSessions.style.display = 'none';
        const tabBlockedIps = document.getElementById('tab-blocked-ips-btn');
        if (tabBlockedIps) tabBlockedIps.style.display = 'none';
        
        const userRoleGroup = document.getElementById('user-role-group');
        if (userRoleGroup) userRoleGroup.style.display = 'none';
        
        const userUsername = document.getElementById('user-username');
        if (userUsername) {
            userUsername.value = currentUser.username;
            userUsername.disabled = true;
        }
    }
}

// --- INITIALIZE & POLLING ---
async function init() {
    // Fetch user profile first
    const profile = await apiCall('/api/me');
    if (profile) {
        currentUser = profile;
        applyRoleRestrictions();
    }

    // Initial fetch
    await fetchInputs();
    await fetchStreams();
    await updateStats();

    // Start background pollers
    setInterval(async () => {
        await updateStats();
        // Silent updates in background
        await fetchInputs();
        await fetchStreams();
    }, 1000);
}

let networkInterfaces = [];

async function loadNetworkInterfaces() {
    try {
        const interfaces = await apiCall('/api/interfaces');
        if (interfaces) {
            networkInterfaces = interfaces;
        }
    } catch (e) {
        console.error('Error fetching interfaces:', e);
    }
}

async function populateStreamInterfaces(selectedInterface = '') {
    await loadNetworkInterfaces();
}

function getPlaceholderForType(type) {
    switch (type) {
        case 'udp':
            return 'udp://239.2.2.109:9009';
        case 'srt':
            return 'srt://127.0.0.1:4001?mode=listener';
        case 'rtp':
            return 'rtp://239.1.1.114:1014';
        case 'hls':
            return 'www/hls/canal/index.m3u8';
        default:
            return '';
    }
}

function slugify(text) {
    return text.toString().toLowerCase().trim()
        .replace(/\s+/g, '-')           // Replace spaces with -
        .replace(/&/g, '-y-')           // Replace & with 'y'
        .replace(/[^\w\-]+/g, '')       // Remove all non-word chars
        .replace(/\-\-+/g, '-');        // Replace multiple - with single -
}

function suggestHlsPath(urlInput) {
    const streamName = document.getElementById('stream-name').value;
    if (streamName) {
        const slug = slugify(streamName);
        urlInput.value = `www/hls/${slug}/index.m3u8`;
    }
}

function addOutputRow(url = '', iface = '', type = 'udp') {
    const container = document.getElementById('stream-outputs-container');
    if (!container) return;

    const row = document.createElement('div');
    row.className = 'output-row';

    // Type select
    const typeSelect = document.createElement('select');
    typeSelect.className = 'output-type-select';
    ['udp', 'srt', 'rtp', 'hls'].forEach(t => {
        const opt = document.createElement('option');
        opt.value = t;
        opt.textContent = t.toUpperCase();
        if (t === type) opt.selected = true;
        typeSelect.appendChild(opt);
    });

    // URL input
    const urlInput = document.createElement('input');
    urlInput.type = 'text';
    urlInput.className = 'output-url-input';
    urlInput.placeholder = getPlaceholderForType(type);
    urlInput.value = url;
    urlInput.required = true;

    // Interface select
    const ifaceSelect = document.createElement('select');
    ifaceSelect.className = 'output-iface-select';
    
    // Populate interface select options
    const defaultOpt = document.createElement('option');
    defaultOpt.value = '';
    defaultOpt.textContent = 'Auto / Defecto';
    ifaceSelect.appendChild(defaultOpt);

    networkInterfaces.forEach(i => {
        const opt = document.createElement('option');
        opt.value = i.name;
        let suffix = '';
        if (i.is_loopback) suffix = ' (Loopback)';
        else if (!i.is_up) suffix = ' (Desconectado)';
        opt.textContent = `${i.name} - ${i.ip}${suffix}`;
        if (i.name === iface) opt.selected = true;
        ifaceSelect.appendChild(opt);
    });

    // If type is HLS, disable interface select
    if (type === 'hls') {
        ifaceSelect.disabled = true;
    }

    // Bind type change event
    typeSelect.addEventListener('change', (e) => {
        const currentType = e.target.value;
        urlInput.placeholder = getPlaceholderForType(currentType);
        if (currentType === 'hls') {
            ifaceSelect.disabled = true;
            ifaceSelect.value = '';
            suggestHlsPath(urlInput);
        } else {
            const isProgrammer = currentUser && currentUser.role === 'Programadores';
            ifaceSelect.disabled = isProgrammer;
        }
    });

    // Remove button
    const removeBtn = document.createElement('button');
    removeBtn.type = 'button';
    removeBtn.className = 'btn btn-danger btn-small';
    removeBtn.textContent = '✕';
    removeBtn.title = 'Eliminar este destino';
    removeBtn.addEventListener('click', () => {
        row.remove();
    });

    // Append everything to row
    row.appendChild(typeSelect);
    row.appendChild(urlInput);
    row.appendChild(ifaceSelect);
    row.appendChild(removeBtn);

    container.appendChild(row);

    // If programmer role is active, disable mutation immediately
    const isProgrammer = currentUser && currentUser.role === 'Programadores';
    if (isProgrammer) {
        typeSelect.disabled = true;
        urlInput.disabled = true;
        ifaceSelect.disabled = true;
        removeBtn.disabled = true;
    }
}

function copyVlcUrl(url, event) {
    if (event) {
        event.stopPropagation();
    }
    
    let vlcUrl = url;
    if (url.startsWith('udp://') && !url.includes('udp://@')) {
        vlcUrl = url.replace('udp://', 'udp://@');
    }
    
    if (vlcUrl.includes('?')) {
        const parts = vlcUrl.split('?');
        vlcUrl = parts[0];
    }
    
    navigator.clipboard.writeText(vlcUrl).then(() => {
        const btn = event ? event.target : null;
        if (btn) {
            const origText = btn.textContent;
            btn.textContent = '✅';
            setTimeout(() => {
                btn.textContent = origText;
            }, 1500);
        }
    }).catch(err => {
        console.error('No se pudo copiar la URL: ', err);
    });
}
window.copyVlcUrl = copyVlcUrl;

// Run initialisation
init();
