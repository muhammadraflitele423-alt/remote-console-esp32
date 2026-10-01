# Remote Console & OTA Manager ESP32 (PT Bekaert Indonesia)

Aplikasi web dashboard dan backend server untuk memonitor, mengontrol, dan melakukan pembaruan firmware (Over-The-Air / OTA) pada node sensor ESP32 secara nirkabel dari jarak jauh tanpa memerlukan port-forwarding atau IP publik di sisi node.

---

## 🌟 Fitur Utama

1. **Live Web Serial Monitor (Real-time)**:
   - Melihat output log serial ESP32 (115200 bps) langsung dari browser secara remote.
   - Dilengkapi filter pencarian log, pewarnaan tag otomatis (LoRa, Sensor, WiFi, HTTP, Error, OK), auto-scroll, salin log, dan unduh log ke file `.txt`.

2. **Reverse Polling Architecture**:
   - ESP32 memanggil server keluar (*outbound HTTPS/HTTP*) secara berkala.
   - Node tetap dapat terhubung meskipun berada di balik NAT, router seluler 4G/GSM, atau firewall pabrik tanpa perlu setting router / IP publik di sisi node.

3. **Adaptive Sync (Hemat Bandwidth & Baterai)**:
   - Saat ada admin yang membuka Serial Monitor, server memerintahkan node untuk sync cepat (`FAST_MS`, default 5 detik).
   - Saat tidak ada yang menonton, node otomatis melambat (`SLOW_MS`, default 30 detik) untuk menghemat resource dan bandwidth WiFi.

4. **Remote Commands**:
   - **Reboot Node**: Restart ESP32 dari jarak jauh jika terjadi kendala.
   - **Kirim Data Sekarang**: Memaksa node segera membaca sensor dan mentransmisikan data tanpa menunggu interval periodik.

5. **Wireless OTA Firmware Deployment**:
   - Unggah file binary hasil compile Arduino IDE (`.bin`) langsung ke web console.
   - Deploy firmware ke node target dengan satu kali klik. Node akan mengunduh firmware, memverifikasi checksum MD5, melakukan flashing ke partisi OTA, dan restart otomatis.

---

## 🚀 Panduan Menjalankan Web Console

### 1. Kebutuhan Sistem
- **Node.js**: Versi 18 ke atas (direkomendasikan Node.js 20+)
- **NPM**: Bawaan Node.js

### 2. Instalasi & Menjalankan Lokal

```bash
# 1. Clone repository ini
git clone https://github.com/username/remote-console-esp32.git
cd remote-console-esp32

# 2. Install dependensi
npm install

# 3. Buat file .env dari template
cp .env.example .env

# 4. Edit file .env dan sesuaikan kredensial Anda
# Pastikan ADMIN_PASSWORD minimal 10 karakter dan DEVICE_API_KEY minimal 16 karakter

# 5. Jalankan server
npm start
```

Setelah server aktif, buka browser di:
👉 **`http://localhost:3000`**

Login dengan username dan password yang telah Anda tentukan di file `.env`.

---

## 🌐 Panduan Deploy ke Cloud / Hosting Publik

Agar web console ini bisa diakses dari mana saja lewat internet (sehingga node ESP32 di pabrik/lapangan bisa terhubung):

### Opsi 1: Railway (Sangat Mudah & Gratis/Murah)
1. Buat repository di GitHub dan push project ini ke GitHub.
2. Buka [Railway.app](https://railway.app) lalu klik **New Project** -> **Deploy from GitHub repo**.
3. Pilih repository ini.
4. Di tab **Variables**, tambahkan environment variable:
   - `PORT` = `3000`
   - `ADMIN_USER` = `admin`
   - `ADMIN_PASSWORD` = `PasswordRahasiaAnda123`
   - `DEVICE_API_KEY` = `KunciApiRahasiaPerangkat123456`
5. Di tab **Settings** -> **Networking**, klik **Generate Domain**. Anda akan mendapatkan URL HTTPS publik (misal: `https://remote-console-production.up.railway.app`).

### Opsi 2: Render.com
1. Buat **Web Service** baru dari repository GitHub Anda.
2. Runtime: **Node**.
3. Build Command: `npm install`
4. Start Command: `npm start`
5. Masukkan Environment Variables di menu **Environment**.

---

## 🔌 Konfigurasi di Sisi ESP32

Buka file firmware Arduino ESP32 (`firmware_node_esp32.ino`), sesuaikan bagian berikut:

```cpp
// ==============================================================================
// KONEKSI KE REMOTE CONSOLE & OTA MANAGER (server.js)
// ==============================================================================
#define REMOTE_CONSOLE_ENABLED   1

// Masukkan alamat IP komputer jika tes di WiFi lokal yang sama,
// ATAU masukkan URL domain HTTPS jika web sudah di-deploy ke cloud (Railway/Render/VPS)
const char* REMOTE_SERVER_URL    = "http://192.168.1.100:3000"; 
// const char* REMOTE_SERVER_URL = "https://domain-anda.up.railway.app";

// Wajib sama persis dengan DEVICE_API_KEY di file .env server
const char* REMOTE_API_KEY       = "bekaert_esp32_device_key_2026";
```

### Cara Export File Firmware `.bin` di Arduino IDE:
1. Buka sketch Arduino di Arduino IDE.
2. Pilih board `ESP32 Dev Module`.
3. Klik menu **Sketch** -> **Export Compiled Binary** (atau tekan `Ctrl + Alt + S`).
4. File `.bin` akan terbentuk di folder sketch (pilih file `.bin` yang **tanpa** kata `bootloader` atau `partitions`).
5. Buka Web Console -> tab **Firmware (OTA)** -> pilih file `.bin` tersebut -> klik **Deploy**.

---

## 📁 Struktur File Repository

```text
├── .env.example              # Template konfigurasi environment variable
├── .gitignore                # Mencegah node_modules & kredensial rahasia ter-commit
├── index.html                # Frontend dashboard admin single-page application (SPA)
├── package.json              # Definisi project & dependensi Node.js (express)
├── server.js                 # Backend server (REST API, reverse sync, OTA distributor)
├── firmware_node_esp32.ino   # Firmware Arduino ESP32 lengkap siap pakai
└── README.md                 # Dokumentasi project
```

---

## 🛡️ Keamanan

- **JANGAN PERNAH** meng-commit file `.env` ke repository publik GitHub karena berisi password admin dan API key perangkat. File `.gitignore` sudah dikonfigurasi untuk mencegah file `.env` ter-commit.
- Gunakan HTTPS di lingkungan produksi untuk melindungi transmisi data kredensial dan firmware.
