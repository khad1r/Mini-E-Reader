/**
 * Mini E-Reader Web Bluetooth PWA
 * Application Logic & BLE Synchronization Engine
 */

// ============================================================================
// BLE Configuration & UUIDs
// ============================================================================
const SERVICE_UUID = '0000fe60-0000-1000-8000-00805f9b34fb';
const CONTROL_UUID = '0000fe61-0000-1000-8000-00805f9b34fb';
const DATA_UUID    = '0000fe62-0000-1000-8000-00805f9b34fb';
const STATUS_UUID  = '0000fe63-0000-1000-8000-00805f9b34fb';

const OPCODES = {
  LIST_BOOKS: 0x01,
  START_UPLOAD: 0x02,
  FINISH_UPLOAD: 0x03,
  DELETE_BOOK: 0x04,
  ABORT_UPLOAD: 0x05
};

const CHUNK_SIZE = 506; // Max BLE payload per packet (508B with 2B seq_id fits in 512B MTU)
const CHUNK_DELAY_MS = 6; // Pacing delay to prevent buffer overruns on ESP32 NimBLE

// ============================================================================
// State Variables
// ============================================================================
let bleDevice = null;
let gattServer = null;
let controlChar = null;
let dataChar = null;
let statusChar = null;

let isConnecting = false;
let isConnected = false;

// Upload State
let isUploading = false;
let cancelUploadRequested = false;
let uploadQueue = [];

// Books State
let remoteBooks = [];
let pendingBookList = [];
let isListingBooks = false;
let listBooksTimeout = null;

// Control Notification Listeners
const controlCallbacks = new Map();

// Pending Delete Book
let bookPendingDelete = null;

// ============================================================================
// DOM Elements
// ============================================================================
const el = {
  themeToggleBtn: document.getElementById('themeToggleBtn'),
  themeIconSun: document.getElementById('themeIconSun'),
  themeIconMoon: document.getElementById('themeIconMoon'),

  connectBtn: document.getElementById('connectBtn'),
  connectBtnText: document.getElementById('connectBtnText'),
  statusDot: document.getElementById('statusDot'),
  compatBanner: document.getElementById('compatBanner'),

  // Device status cards
  deviceStateBadge: document.getElementById('deviceStateBadge'),
  deviceNameText: document.getElementById('deviceNameText'),
  deviceUuidText: document.getElementById('deviceUuidText'),
  deviceMtuText: document.getElementById('deviceMtuText'),

  batteryPctText: document.getElementById('batteryPctText'),
  batteryVoltageText: document.getElementById('batteryVoltageText'),
  batteryStatusDesc: document.getElementById('batteryStatusDesc'),
  chargingBadge: document.getElementById('chargingBadge'),
  batteryMeterFill: document.getElementById('batteryMeterFill'),

  storageFreeText: document.getElementById('storageFreeText'),
  storageUsedText: document.getElementById('storageUsedText'),
  storageTotalText: document.getElementById('storageTotalText'),
  storageBadge: document.getElementById('storageBadge'),
  storageMeterFill: document.getElementById('storageMeterFill'),

  // Upload
  dropzone: document.getElementById('dropzone'),
  fileInput: document.getElementById('fileInput'),
  uploadActiveCard: document.getElementById('uploadActiveCard'),
  uploadingFileName: document.getElementById('uploadingFileName'),
  cancelUploadBtn: document.getElementById('cancelUploadBtn'),
  uploadProgressFill: document.getElementById('uploadProgressFill'),
  uploadPctStat: document.getElementById('uploadPctStat'),
  uploadSpeedStat: document.getElementById('uploadSpeedStat'),
  uploadSeqStat: document.getElementById('uploadSeqStat'),
  uploadEtaStat: document.getElementById('uploadEtaStat'),

  // Books
  booksCountBadge: document.getElementById('booksCountBadge'),
  bookSearchInput: document.getElementById('bookSearchInput'),
  refreshBooksBtn: document.getElementById('refreshBooksBtn'),
  refreshBooksIcon: document.getElementById('refreshBooksIcon'),
  refreshStatusBtn: document.getElementById('refreshStatusBtn'),
  refreshStatusIcon: document.getElementById('refreshStatusIcon'),
  booksList: document.getElementById('booksList'),

  // Modal
  deleteModal: document.getElementById('deleteModal'),
  deleteModalText: document.getElementById('deleteModalText'),
  cancelDeleteBtn: document.getElementById('cancelDeleteBtn'),
  confirmDeleteBtn: document.getElementById('confirmDeleteBtn'),

  // Log
  logHeader: document.getElementById('logHeader'),
  logBody: document.getElementById('logBody'),
  logToggleIcon: document.getElementById('logToggleIcon'),
  logCountBadge: document.getElementById('logCountBadge'),
  clearLogBtn: document.getElementById('clearLogBtn'),

  toastContainer: document.getElementById('toastContainer')
};

// ============================================================================
// Utilities: Formatting & Logging
// ============================================================================
function formatBytes(bytes) {
  if (isNaN(bytes) || bytes === null || bytes < 0) return '0 B';
  if (bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

function formatKb(kb) {
  return formatBytes(kb * 1024);
}

let logCount = 0;
function log(msg, type = 'info') {
  const now = new Date();
  const timestamp = now.toTimeString().split(' ')[0] + '.' + String(now.getMilliseconds()).padStart(3, '0');

  const entry = document.createElement('div');
  entry.className = `log-entry ${type}`;

  const timeSpan = document.createElement('span');
  timeSpan.className = 'log-time';
  timeSpan.textContent = `[${timestamp}]`;

  const msgSpan = document.createElement('span');
  msgSpan.className = 'log-msg';
  msgSpan.textContent = msg;

  entry.appendChild(timeSpan);
  entry.appendChild(msgSpan);

  el.logBody.appendChild(entry);
  logCount++;
  el.logCountBadge.textContent = logCount;

  // Auto-scroll to bottom
  el.logBody.scrollTop = el.logBody.scrollHeight;

  // Limit entries
  while (el.logBody.childElementCount > 250) {
    el.logBody.removeChild(el.logBody.firstChild);
  }
}

function showToast(message, type = 'info', duration = 3500) {
  const toast = document.createElement('div');
  toast.className = `toast ${type}`;
  toast.textContent = message;

  el.toastContainer.appendChild(toast);

  setTimeout(() => {
    toast.style.opacity = '0';
    toast.style.transform = 'translateY(10px)';
    setTimeout(() => {
      if (toast.parentNode) {
        toast.parentNode.removeChild(toast);
      }
    }, 250);
  }, duration);
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// ============================================================================
// Theme Toggle & Persistence
// ============================================================================
function initTheme() {
  const savedTheme = localStorage.getItem('ereader_pwa_theme');
  const prefersDark = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
  const theme = savedTheme || (prefersDark ? 'dark' : 'light');
  setTheme(theme);

  el.themeToggleBtn.addEventListener('click', () => {
    const currentTheme = document.documentElement.getAttribute('data-theme') || 'dark';
    const nextTheme = currentTheme === 'dark' ? 'light' : 'dark';
    setTheme(nextTheme);
  });
}

function setTheme(theme) {
  document.documentElement.setAttribute('data-theme', theme);
  localStorage.setItem('ereader_pwa_theme', theme);

  if (theme === 'dark') {
    el.themeIconSun.style.display = 'block';
    el.themeIconMoon.style.display = 'none';
  } else {
    el.themeIconSun.style.display = 'none';
    el.themeIconMoon.style.display = 'block';
  }
}

// ============================================================================
// Web Bluetooth Compatibility Check
// ============================================================================
function checkBluetoothCompatibility() {
  if (!navigator.bluetooth) {
    el.compatBanner.style.display = 'flex';
    log('Web Bluetooth API is not supported in this browser.', 'warning');
    return false;
  }
  return true;
}

// ============================================================================
// BLE Connection Management
// ============================================================================
async function handleConnectToggle() {
  if (isConnected) {
    disconnectDevice();
  } else {
    await connectDevice();
  }
}

async function connectDevice() {
  if (!checkBluetoothCompatibility()) {
    showToast('Web Bluetooth not supported on this browser', 'error');
    return;
  }

  try {
    isConnecting = true;
    updateConnectionUI('connecting');
    log('Requesting Bluetooth device filter [0000fe60]...', 'info');

    bleDevice = await navigator.bluetooth.requestDevice({
      filters: [
        { services: [SERVICE_UUID] },
        { namePrefix: 'Mini-E' }
      ],
      optionalServices: [SERVICE_UUID]
    });

    log(`Device selected: ${bleDevice.name || 'Unknown'} (${bleDevice.id})`, 'info');
    bleDevice.addEventListener('gattserverdisconnected', onDisconnected);

    log('Connecting to GATT server...', 'info');
    gattServer = await bleDevice.gatt.connect();

    log('Discovering E-Reader Sync service...', 'info');
    const service = await gattServer.getPrimaryService(SERVICE_UUID);

    log('Binding GATT characteristics...', 'info');
    controlChar = await service.getCharacteristic(CONTROL_UUID);
    dataChar = await service.getCharacteristic(DATA_UUID);
    statusChar = await service.getCharacteristic(STATUS_UUID);

    // Subscribe to notifications
    log('Subscribing to Control notifications (0xfe61)...', 'info');
    await controlChar.startNotifications();
    controlChar.addEventListener('characteristicvaluechanged', onControlNotification);

    log('Subscribing to Status notifications (0xfe63)...', 'info');
    await statusChar.startNotifications();
    statusChar.addEventListener('characteristicvaluechanged', onStatusNotification);

    isConnected = true;
    isConnecting = false;
    updateConnectionUI('connected');
    showToast(`Connected to ${bleDevice.name || 'Mini-E-Reader'}`, 'success');
    log('Connection established successfully.', 'success');

    // Read initial status
    await readDeviceStatus();

    // Query initial book list
    await queryBooksList();

  } catch (error) {
    isConnecting = false;
    isConnected = false;
    updateConnectionUI('disconnected');

    if (error.name === 'NotFoundError') {
      log('Device selection cancelled by user.', 'info');
    } else {
      log(`BLE Connection Error: ${error.message}`, 'error');
      showToast(`Connection failed: ${error.message}`, 'error');
    }
  }
}

function disconnectDevice() {
  if (bleDevice && bleDevice.gatt && bleDevice.gatt.connected) {
    log('Disconnecting from device...', 'info');
    bleDevice.gatt.disconnect();
  } else {
    onDisconnected();
  }
}

function onDisconnected() {
  log('Bluetooth device disconnected.', 'warning');
  showToast('Disconnected from Mini-E-Reader', 'warning');

  isConnected = false;
  isConnecting = false;
  gattServer = null;
  controlChar = null;
  dataChar = null;
  statusChar = null;

  if (isUploading) {
    cancelUploadRequested = true;
    hideUploadActiveCard();
  }

  updateConnectionUI('disconnected');
}

function updateConnectionUI(state) {
  if (state === 'connected') {
    el.statusDot.className = 'status-dot connected';
    el.connectBtn.className = 'btn-connect connected';
    el.connectBtnText.textContent = 'Disconnect';
    el.deviceStateBadge.className = 'card-badge badge-success';
    el.deviceStateBadge.textContent = 'Connected';
    el.deviceNameText.textContent = bleDevice?.name || 'Mini-E-Reader';
    el.refreshBooksBtn.disabled = false;
    el.refreshStatusBtn.disabled = false;
  } else if (state === 'connecting') {
    el.statusDot.className = 'status-dot connecting';
    el.connectBtn.className = 'btn-connect';
    el.connectBtnText.textContent = 'Connecting...';
    el.deviceStateBadge.className = 'card-badge badge-muted';
    el.deviceStateBadge.textContent = 'Connecting';
    el.refreshBooksBtn.disabled = true;
    el.refreshStatusBtn.disabled = true;
  } else {
    el.statusDot.className = 'status-dot disconnected';
    el.connectBtn.className = 'btn-connect';
    el.connectBtnText.textContent = 'Connect E-Reader';
    el.deviceStateBadge.className = 'card-badge badge-muted';
    el.deviceStateBadge.textContent = 'Disconnected';
    el.refreshBooksBtn.disabled = true;
    el.refreshStatusBtn.disabled = true;

    // Reset status indicators to placeholder
    el.batteryPctText.textContent = '—%';
    el.batteryVoltageText.textContent = '— V';
    el.batteryStatusDesc.textContent = 'Not connected';
    el.chargingBadge.className = 'card-badge badge-muted';
    el.chargingBadge.textContent = '—';
    el.batteryMeterFill.style.width = '0%';

    el.storageFreeText.textContent = '—';
    el.storageUsedText.textContent = 'Used: —';
    el.storageTotalText.textContent = 'Total: —';
    el.storageBadge.className = 'card-badge badge-muted';
    el.storageBadge.textContent = '—';
    el.storageMeterFill.style.width = '0%';

    remoteBooks = [];
    renderBooksList();
  }
}

// ============================================================================
// Status Characteristic Parsing & Updates
// ============================================================================
async function readDeviceStatus() {
  if (!statusChar) return;
  try {
    el.refreshStatusIcon.classList.add('spin');
    const val = await statusChar.readValue();
    parseStatusValue(val);
  } catch (err) {
    log(`Failed to read status: ${err.message}`, 'error');
  } finally {
    setTimeout(() => el.refreshStatusIcon.classList.remove('spin'), 500);
  }
}

function onStatusNotification(event) {
  parseStatusValue(event.target.value);
}

function parseStatusValue(dataView) {
  if (!dataView || dataView.byteLength < 12) {
    log(`Received invalid status packet (length ${dataView?.byteLength || 0})`, 'warning');
    return;
  }

  // 12-byte packed struct:
  // offset 0: uint8_t battery_pct (0-100)
  // offset 1: uint8_t is_charging (0 or 1)
  // offset 2: uint16_le battery_mv
  // offset 4: uint32_le free_kb
  // offset 8: uint32_le total_kb
  const batteryPct = dataView.getUint8(0);
  const isCharging = dataView.getUint8(1) !== 0;
  const batteryMv = dataView.getUint16(2, true);
  const freeKb = dataView.getUint32(4, true);
  const totalKb = dataView.getUint32(8, true);

  // Update Battery UI
  el.batteryPctText.textContent = `${batteryPct}%`;
  el.batteryVoltageText.textContent = `${(batteryMv / 1000).toFixed(2)} V`;
  el.batteryMeterFill.style.width = `${Math.min(100, Math.max(0, batteryPct))}%`;

  if (batteryPct > 50) {
    el.batteryMeterFill.style.backgroundColor = 'var(--success-color)';
  } else if (batteryPct > 20) {
    el.batteryMeterFill.style.backgroundColor = 'var(--warning-color)';
  } else {
    el.batteryMeterFill.style.backgroundColor = 'var(--danger-color)';
  }

  if (isCharging) {
    el.chargingBadge.className = 'card-badge badge-charging';
    el.chargingBadge.innerHTML = '⚡ Charging';
    el.batteryStatusDesc.textContent = 'Charging via USB';
  } else {
    el.chargingBadge.className = 'card-badge badge-muted';
    el.chargingBadge.textContent = 'On Battery';
    el.batteryStatusDesc.textContent = 'Operating on battery';
  }

  // Update Storage UI
  const usedKb = totalKb > freeKb ? totalKb - freeKb : 0;
  const usedPct = totalKb > 0 ? ((usedKb / totalKb) * 100).toFixed(1) : 0;

  el.storageFreeText.textContent = `${formatKb(freeKb)} free`;
  el.storageUsedText.textContent = `Used: ${formatKb(usedKb)} (${usedPct}%)`;
  el.storageTotalText.textContent = `Total: ${formatKb(totalKb)}`;
  el.storageBadge.className = 'card-badge badge-muted';
  el.storageBadge.textContent = `${usedPct}% used`;
  el.storageMeterFill.style.width = `${Math.min(100, Math.max(0, usedPct))}%`;

  log(`Status Update: Battery ${batteryPct}% (${(batteryMv/1000).toFixed(2)}V), SD Free: ${formatKb(freeKb)} / ${formatKb(totalKb)}`, 'info');
}

// ============================================================================
// Control Characteristic Notifications & Await Helpers
// ============================================================================
function onControlNotification(event) {
  const val = event.target.value;
  if (!val || val.byteLength < 1) return;

  const opcode = val.getUint8(0);

  // If there is an active Promise waiting for this opcode, notify it
  if (controlCallbacks.has(opcode)) {
    const callback = controlCallbacks.get(opcode);
    controlCallbacks.delete(opcode);
    callback(val);
    return;
  }

  // Opcode 0x01: LIST_BOOKS stream handler
  if (opcode === OPCODES.LIST_BOOKS) {
    handleListBooksPacket(val);
  }
}

function waitForControlResponse(opcode, timeoutMs = 8000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      controlCallbacks.delete(opcode);
      reject(new Error(`Timeout waiting for BLE opcode 0x${opcode.toString(16)} response`));
    }, timeoutMs);

    controlCallbacks.set(opcode, (val) => {
      clearTimeout(timer);
      const status = val.byteLength >= 2 ? val.getUint8(1) : 0;
      resolve({ status, dataView: val });
    });
  });
}

// ============================================================================
// Remote Books Management (Query & Delete)
// ============================================================================
async function queryBooksList() {
  if (!controlChar) {
    showToast('Not connected to E-Reader', 'warning');
    return;
  }

  try {
    el.refreshBooksIcon.classList.add('spin');
    isListingBooks = true;
    pendingBookList = [];

    // Fallback timer if device stream terminates unexpectedly
    clearTimeout(listBooksTimeout);
    listBooksTimeout = setTimeout(() => {
      if (isListingBooks) {
        log('Book listing stream finalized by timeout.', 'info');
        finalizeBookList();
      }
    }, 4000);

    log('Querying books on SD card (OpCode 0x01)...', 'info');
    const req = new Uint8Array([OPCODES.LIST_BOOKS]);
    await controlChar.writeValueWithResponse(req);

  } catch (err) {
    log(`Failed to request book list: ${err.message}`, 'error');
    showToast(`Error requesting books: ${err.message}`, 'error');
    finalizeBookList();
  }
}

function handleListBooksPacket(val) {
  // Packet layout: [0x01, status, is_last, uint32_le file_size, filename_bytes...]
  if (val.byteLength < 7) {
    log(`Malformed book list packet (length ${val.byteLength})`, 'warning');
    return;
  }

  const status = val.getUint8(1);
  const isLast = val.getUint8(2);
  const fileSize = val.getUint32(3, true);

  // Extract null-terminated filename string
  let filename = '';
  const nameBytes = [];
  for (let i = 7; i < val.byteLength; i++) {
    const b = val.getUint8(i);
    if (b === 0) break;
    nameBytes.push(b);
  }

  if (nameBytes.length > 0) {
    try {
      filename = new TextDecoder('utf-8').decode(new Uint8Array(nameBytes)).trim();
    } catch {
      filename = String.fromCharCode.apply(null, nameBytes).trim();
    }
  }

  if (status === 0 && filename.length > 0) {
    pendingBookList.push({ name: filename, size: fileSize });
  }

  if (isLast === 1) {
    clearTimeout(listBooksTimeout);
    finalizeBookList();
  }
}

function finalizeBookList() {
  isListingBooks = false;
  el.refreshBooksIcon.classList.remove('spin');

  // Sort books alphabetically
  remoteBooks = pendingBookList.sort((a, b) => a.name.localeCompare(b.name));
  log(`Found ${remoteBooks.length} book(s) on SD card.`, 'success');
  renderBooksList();
}

function renderBooksList() {
  const searchTerm = el.bookSearchInput.value.trim().toLowerCase();
  const filtered = remoteBooks.filter(b => b.name.toLowerCase().includes(searchTerm));

  el.booksCountBadge.textContent = `${remoteBooks.length} ${remoteBooks.length === 1 ? 'book' : 'books'}`;
  el.booksList.innerHTML = '';

  if (!isConnected) {
    el.booksList.innerHTML = `
      <div class="empty-state">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
          <path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"></path>
          <path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"></path>
        </svg>
        <p>Connect your Mini E-Reader to view and manage books.</p>
      </div>`;
    return;
  }

  if (filtered.length === 0) {
    el.booksList.innerHTML = `
      <div class="empty-state">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="12" cy="12" r="10"></circle>
          <line x1="8" y1="12" x2="16" y2="12"></line>
        </svg>
        <p>${searchTerm ? 'No matching books found' : 'No books found on SD card (/sdcard/books/). Drop a .txt file above to upload!'}</p>
      </div>`;
    return;
  }

  filtered.forEach(book => {
    const item = document.createElement('div');
    item.className = 'book-item';

    const info = document.createElement('div');
    info.className = 'book-info';

    const icon = document.createElement('div');
    icon.className = 'book-icon';
    icon.innerHTML = `
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"></path>
        <path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"></path>
      </svg>`;

    const meta = document.createElement('div');
    meta.className = 'book-meta';

    const name = document.createElement('div');
    name.className = 'book-name';
    name.textContent = book.name;
    name.title = book.name;

    const size = document.createElement('div');
    size.className = 'book-size';
    size.textContent = formatBytes(book.size);

    meta.appendChild(name);
    meta.appendChild(size);
    info.appendChild(icon);
    info.appendChild(meta);

    const deleteBtn = document.createElement('button');
    deleteBtn.className = 'btn-delete-book';
    deleteBtn.title = `Delete ${book.name}`;
    deleteBtn.innerHTML = `
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <polyline points="3 6 5 6 21 6"></polyline>
        <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path>
        <line x1="10" y1="11" x2="10" y2="17"></line>
        <line x1="14" y1="11" x2="14" y2="17"></line>
      </svg>`;

    deleteBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      openDeleteModal(book);
    });

    item.appendChild(info);
    item.appendChild(deleteBtn);
    el.booksList.appendChild(item);
  });
}

// Delete Book Dialog
function openDeleteModal(book) {
  bookPendingDelete = book;
  el.deleteModalText.textContent = `Are you sure you want to delete "${book.name}" (${formatBytes(book.size)}) from the SD card? This cannot be undone.`;
  el.deleteModal.classList.add('active');
}

function closeDeleteModal() {
  bookPendingDelete = null;
  el.deleteModal.classList.remove('active');
}

async function confirmDeleteBook() {
  if (!bookPendingDelete || !controlChar) {
    closeDeleteModal();
    return;
  }

  const book = bookPendingDelete;
  closeDeleteModal();

  try {
    log(`Deleting book "${book.name}" (OpCode 0x04)...`, 'info');

    // Build packet: [0x04, filename_bytes..., 0x00]
    const enc = new TextEncoder();
    const nameBytes = enc.encode(book.name);
    const packet = new Uint8Array(1 + nameBytes.length + 1);
    packet[0] = OPCODES.DELETE_BOOK;
    packet.set(nameBytes, 1);
    packet[1 + nameBytes.length] = 0; // null terminator

    const respPromise = waitForControlResponse(OPCODES.DELETE_BOOK, 6000);
    await controlChar.writeValueWithResponse(packet);
    const { status } = await respPromise;

    if (status === 0) {
      log(`Deleted "${book.name}" successfully.`, 'success');
      showToast(`Deleted "${book.name}"`, 'success');
      // Refresh list & status
      await queryBooksList();
      await readDeviceStatus();
    } else {
      throw new Error(`Device reported error code: ${status}`);
    }

  } catch (err) {
    log(`Delete failed: ${err.message}`, 'error');
    showToast(`Delete failed: ${err.message}`, 'error');
  }
}

// ============================================================================
// Chunked File Upload Engine
// ============================================================================
function handleFilesSelected(files) {
  if (!files || files.length === 0) return;

  if (!isConnected) {
    showToast('Please connect to your Mini E-Reader before uploading files', 'warning');
    return;
  }

  for (let i = 0; i < files.length; i++) {
    const file = files[i];
    if (!file.name.toLowerCase().endsWith('.txt')) {
      showToast(`Warning: "${file.name}" is not a .txt file, attempting plain text upload`, 'warning');
    }
    uploadQueue.push(file);
  }

  processUploadQueue();
}

async function processUploadQueue() {
  if (isUploading) return;
  if (uploadQueue.length === 0) return;

  const file = uploadQueue.shift();
  await executeFileUpload(file);

  // If there are more files in queue, continue
  if (uploadQueue.length > 0 && !cancelUploadRequested) {
    processUploadQueue();
  }
}

async function executeFileUpload(file) {
  if (!controlChar || !dataChar) {
    showToast('Upload failed: Bluetooth connection lost', 'error');
    return;
  }

  isUploading = true;
  cancelUploadRequested = false;

  const fileName = file.name;
  const fileSize = file.size;

  log(`Starting upload: "${fileName}" (${formatBytes(fileSize)})`, 'info');
  showUploadActiveCard(fileName);

  try {
    // Read entire file as ArrayBuffer
    const fileBuffer = await file.arrayBuffer();
    const fileBytes = new Uint8Array(fileBuffer);

    // Step 1: Send START_UPLOAD (0x02)
    // Packet: [0x02, uint32_le file_size, filename_null_terminated]
    const enc = new TextEncoder();
    const nameBytes = enc.encode(fileName);
    const startPacket = new Uint8Array(1 + 4 + nameBytes.length + 1);
    startPacket[0] = OPCODES.START_UPLOAD;
    const view = new DataView(startPacket.buffer);
    view.setUint32(1, fileSize, true);
    startPacket.set(nameBytes, 5);
    startPacket[5 + nameBytes.length] = 0; // null terminator

    log(`Sending START_UPLOAD for "${fileName}" (size: ${fileSize} bytes)...`, 'info');
    const startRespPromise = waitForControlResponse(OPCODES.START_UPLOAD, 8000);
    await controlChar.writeValueWithResponse(startPacket);
    const startResp = await startRespPromise;

    if (startResp.status !== 0) {
      throw new Error(`Device rejected upload start (error code: ${startResp.status})`);
    }

    log(`Device accepted upload. Beginning chunk transfer (Chunk size: ${CHUNK_SIZE} B)...`, 'info');

    // Step 2: Stream Data Chunks
    const totalChunks = Math.ceil(fileBytes.length / CHUNK_SIZE);
    let seqId = 0;
    let bytesSent = 0;
    const startTime = performance.now();

    for (let offset = 0; offset < fileBytes.length; offset += CHUNK_SIZE) {
      if (cancelUploadRequested) {
        throw new Error('Upload cancelled by user');
      }

      if (!isConnected || !dataChar) {
        throw new Error('Connection lost during transmission');
      }

      const chunk = fileBytes.subarray(offset, Math.min(offset + CHUNK_SIZE, fileBytes.length));
      const packet = new Uint8Array(2 + chunk.length);
      const pktView = new DataView(packet.buffer);
      pktView.setUint16(0, seqId & 0xFFFF, true);
      packet.set(chunk, 2);

      // Write chunk without response
      if (dataChar.writeValueWithoutResponse) {
        await dataChar.writeValueWithoutResponse(packet);
      } else {
        await dataChar.writeValue(packet);
      }

      bytesSent += chunk.length;
      seqId = (seqId + 1) & 0xFFFF;

      // Update UI Progress Stats
      updateUploadStats(bytesSent, fileSize, seqId, totalChunks, startTime);

      // Pacing pause to allow ESP32 SDMMC/flash write
      if (CHUNK_DELAY_MS > 0) {
        await sleep(CHUNK_DELAY_MS);
      }
    }

    // Step 3: Send FINISH_UPLOAD (0x03)
    log('All chunks sent. Sending FINISH_UPLOAD (0x03)...', 'info');
    const finishPacket = new Uint8Array([OPCODES.FINISH_UPLOAD]);
    const finishRespPromise = waitForControlResponse(OPCODES.FINISH_UPLOAD, 10000);
    await controlChar.writeValueWithResponse(finishPacket);
    const finishResp = await finishRespPromise;

    if (finishResp.status !== 0) {
      throw new Error(`Device failed to finish upload (error code: ${finishResp.status})`);
    }

    const elapsedTotal = ((performance.now() - startTime) / 1000).toFixed(1);
    const avgSpeed = (fileSize / 1024 / (elapsedTotal || 1)).toFixed(1);
    log(`Upload complete: "${fileName}" in ${elapsedTotal}s (avg ${avgSpeed} KB/s)`, 'success');
    showToast(`Uploaded "${fileName}" successfully!`, 'success');

    // Refresh book list & storage stats
    await queryBooksList();
    await readDeviceStatus();

  } catch (err) {
    log(`Upload Error: ${err.message}`, 'error');
    showToast(`Upload failed: ${err.message}`, 'error');
    if (controlChar && isConnected) {
      try {
        const abortPacket = new Uint8Array([OPCODES.ABORT_UPLOAD]);
        await controlChar.writeValueWithResponse(abortPacket);
      } catch (_) {}
    }
  } finally {
    isUploading = false;
    hideUploadActiveCard();
  }
}

function showUploadActiveCard(fileName) {
  el.uploadingFileName.textContent = fileName;
  el.uploadProgressFill.style.width = '0%';
  el.uploadPctStat.textContent = '0.0%';
  el.uploadSpeedStat.textContent = '0.0 KB/s';
  el.uploadSeqStat.textContent = '#0 / 0';
  el.uploadEtaStat.textContent = 'Calculating...';
  el.uploadActiveCard.classList.add('visible');
}

function hideUploadActiveCard() {
  el.uploadActiveCard.classList.remove('visible');
  el.fileInput.value = '';
}

function updateUploadStats(bytesSent, totalBytes, seqId, totalChunks, startTime) {
  const pct = totalBytes > 0 ? ((bytesSent / totalBytes) * 100).toFixed(1) : 0;
  const elapsedSec = (performance.now() - startTime) / 1000;
  const speedKBps = elapsedSec > 0 ? (bytesSent / 1024 / elapsedSec) : 0;
  const remainingBytes = totalBytes - bytesSent;
  const etaSec = speedKBps > 0 ? Math.ceil((remainingBytes / 1024) / speedKBps) : 0;

  el.uploadProgressFill.style.width = `${pct}%`;
  el.uploadPctStat.textContent = `${pct}%`;
  el.uploadSpeedStat.textContent = `${speedKBps.toFixed(1)} KB/s`;
  el.uploadSeqStat.textContent = `#${seqId} / ${totalChunks}`;
  el.uploadEtaStat.textContent = `${etaSec}s`;
}

// ============================================================================
// Service Worker Registration
// ============================================================================
function registerServiceWorker() {
  if ('serviceWorker' in navigator) {
    window.addEventListener('load', () => {
      navigator.serviceWorker.register('./sw.js')
        .then((reg) => {
          log(`Service Worker registered with scope: ${reg.scope}`, 'info');
        })
        .catch((err) => {
          log(`Service Worker registration failed: ${err.message}`, 'warning');
        });
    });
  }
}

// ============================================================================
// Event Listeners Initialization
// ============================================================================
function initEventListeners() {
  // BLE Connection Button
  el.connectBtn.addEventListener('click', handleConnectToggle);

  // Status & Books Refresh Buttons
  el.refreshStatusBtn.addEventListener('click', readDeviceStatus);
  el.refreshBooksBtn.addEventListener('click', queryBooksList);

  // Search filter input
  el.bookSearchInput.addEventListener('input', renderBooksList);

  // File Dropzone & Input
  el.dropzone.addEventListener('click', () => el.fileInput.click());
  el.fileInput.addEventListener('change', (e) => handleFilesSelected(e.target.files));

  el.dropzone.addEventListener('dragover', (e) => {
    e.preventDefault();
    e.stopPropagation();
    el.dropzone.classList.add('dragover');
  });

  el.dropzone.addEventListener('dragleave', (e) => {
    e.preventDefault();
    e.stopPropagation();
    el.dropzone.classList.remove('dragover');
  });

  el.dropzone.addEventListener('drop', (e) => {
    e.preventDefault();
    e.stopPropagation();
    el.dropzone.classList.remove('dragover');
    if (e.dataTransfer && e.dataTransfer.files) {
      handleFilesSelected(e.dataTransfer.files);
    }
  });

  // Cancel Upload
  el.cancelUploadBtn.addEventListener('click', () => {
    if (isUploading) {
      cancelUploadRequested = true;
      log('User requested upload cancellation.', 'warning');
      showToast('Cancelling upload...', 'warning');
    }
  });

  // Delete Modal
  el.cancelDeleteBtn.addEventListener('click', closeDeleteModal);
  el.confirmDeleteBtn.addEventListener('click', confirmDeleteBook);
  el.deleteModal.addEventListener('click', (e) => {
    if (e.target === el.deleteModal) closeDeleteModal();
  });

  // Log drawer
  el.logHeader.addEventListener('click', (e) => {
    if (e.target === el.clearLogBtn) return;
    el.logBody.classList.toggle('collapsed');
    el.logToggleIcon.textContent = el.logBody.classList.contains('collapsed') ? '\u25B6' : '\u25BC';
  });

  el.clearLogBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    el.logBody.innerHTML = '';
    logCount = 0;
    el.logCountBadge.textContent = '0';
  });
}

// ============================================================================
// App Bootstrap
// ============================================================================
function init() {
  initTheme();
  checkBluetoothCompatibility();
  initEventListeners();
  registerServiceWorker();
  log('Mini E-Reader Web Bluetooth PWA initialized.', 'info');
}

document.addEventListener('DOMContentLoaded', init);
