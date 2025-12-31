// CarSpeaker Phase 4 – bereinigt + stabil (WLAN AP-only, BT aktuell deaktiviert)

#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <driver/i2s.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <BluetoothSerial.h>
#include <nvs_flash.h>
#include <esp_wifi.h>
#include <esp_bt.h>

// =======================
// Debug Flags
// =======================
static const bool WIFI_DEBUG = false;
static const bool AUDIO_DEBUG_PEAK = true;

// GPIO26 (GAIN) hat bei dir Boot-Probleme gemacht -> default AUS
static const bool USE_AMP_GAIN_PIN = false;

// =======================
// WAV Parser (minimal, PCM 16-bit)
// =======================
struct WavInfo {
  uint32_t sampleRate = 44100;
  uint16_t channels = 2;
  uint16_t bitsPerSample = 16;
  uint32_t dataStart = 0;
  uint32_t dataSize = 0;
  bool ok = false;
};

static uint32_t readLE32(File& f) {
  uint8_t b[4];
  if (f.read(b, 4) != 4) return 0;
  return ((uint32_t)b[0]) | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t readLE16(File& f) {
  uint8_t b[2];
  if (f.read(b, 2) != 2) return 0;
  return (uint16_t)(b[0] | (b[1] << 8));
}

static WavInfo parseWav(File& f) {
  WavInfo info;

  char riff[4];
  if (f.read((uint8_t*)riff, 4) != 4) return info;
  if (memcmp(riff, "RIFF", 4) != 0) return info;

  (void)readLE32(f);
  char wave[4];
  if (f.read((uint8_t*)wave, 4) != 4) return info;
  if (memcmp(wave, "WAVE", 4) != 0) return info;

  bool fmtFound = false;
  bool dataFound = false;

  while (f.available()) {
    char chunkId[4];
    if (f.read((uint8_t*)chunkId, 4) != 4) break;
    uint32_t chunkSize = readLE32(f);

    if (memcmp(chunkId, "fmt ", 4) == 0) {
      uint16_t audioFormat = readLE16(f);
      info.channels = readLE16(f);
      info.sampleRate = readLE32(f);
      (void)readLE32(f);
      (void)readLE16(f);
      info.bitsPerSample = readLE16(f);

      if (chunkSize > 16) f.seek(f.position() + (chunkSize - 16));

      if (audioFormat != 1) return info;           // PCM
      if (info.bitsPerSample != 16) return info;   // 16-bit
      if (!(info.channels == 1 || info.channels == 2)) return info;

      fmtFound = true;
    }
    else if (memcmp(chunkId, "data", 4) == 0) {
      info.dataStart = f.position();
      info.dataSize = chunkSize;
      dataFound = true;
      break;
    }
    else {
      f.seek(f.position() + chunkSize);
    }
  }

  info.ok = (fmtFound && dataFound);
  return info;
}

// =======================
// Defaults
// =======================
static const char* AP_SSID_DEFAULT = "Car Speaker";
static const char* AP_PASS_DEFAULT = "speaker314";
static const char* BT_NAME_DEFAULT = "Car-Speaker-BT";

// =======================
// Pins
// =======================
static const int PIN_STATUS_LED = 2;

static const int PIN_SD_CS   = 5;
static const int PIN_SD_MOSI = 23;
static const int PIN_SD_MISO = 19;
static const int PIN_SD_SCK  = 18;

// AMP Control
static const int PIN_AMP_SD   = 27;  // SD/EN
static const int PIN_AMP_GAIN = 26;  // GAIN (optional!)
static const bool AMP_SD_ACTIVE_HIGH = true;

// I2S
static const int PIN_I2S_BCLK  = 14;
static const int PIN_I2S_LRCLK = 25;
static const int PIN_I2S_DOUT  = 32;

// =======================
// Services
// =======================
WebServer server(80);
Preferences prefs;
BluetoothSerial SerialBT;

// =======================
// Config
// =======================
struct Config {
  String wifiMode = "ap";

  String apSsid;
  String apPass;
  int apChannel = 6;

  String staSsid;
  String staPass;

  int volume = 50;
  String powerMode = "always_on";

  String btName;
  String btPeerMac;

  String commMode = "wifi";
  bool btEnabled = false;
};

static Config cfg;

// =======================
// State
// =======================
static bool sdReady = false;

static volatile bool stopRequested = false;
static TaskHandle_t playbackTaskHandle = nullptr;

static String currentPlayingPath = "";
static volatile bool isPlaying = false;

static File uploadFile;

// BT (aktuell nicht gestartet)
static bool btRunning = false;

// =======================
// Helpers
// =======================
static inline void ampEnable(bool on) {
  digitalWrite(PIN_AMP_SD, AMP_SD_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
}

static inline void ampSetGainHigh() {
  if (!USE_AMP_GAIN_PIN) return;
  pinMode(PIN_AMP_GAIN, OUTPUT);
  digitalWrite(PIN_AMP_GAIN, HIGH);
}

static String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

static void sendJson(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(code, "application/json", body);
}

static void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

static bool isValidFolder(const String& f) {
  return (f == "A" || f == "B" || f == "C" || f == "D");
}

static String ensureFolderPath(const String& folder) { return "/" + folder; }
static String makeFilePath(const String& folder, const String& filename) { return "/" + folder + "/" + filename; }

static bool hasWavExt(const String& name) {
  String lower = name; lower.toLowerCase();
  return lower.endsWith(".wav");
}

static String sanitizeFilename(const String& name) {
  String out; out.reserve(name.length());
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.';
    if (ok) out += c;
  }
  return out;
}

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static void dumpHex(const char* label, const String& s) {
  Serial.printf("%s len=%d: '", label, (int)s.length());
  Serial.print(s);
  Serial.println("'");
  Serial.printf("%s HEX: ", label);
  for (int i = 0; i < (int)s.length(); i++) Serial.printf("%02X ", (uint8_t)s[i]);
  Serial.println();
}

// =======================
// I2S
// =======================
static bool i2sInit(uint32_t sampleRate) {
  i2s_config_t i2s_config = {};
  i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2s_config.sample_rate = sampleRate;
  i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2s_config.communication_format = I2S_COMM_FORMAT_I2S;
  i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2s_config.dma_buf_count = 8;
  i2s_config.dma_buf_len = 256;
  i2s_config.use_apll = false;
  i2s_config.tx_desc_auto_clear = true;
  i2s_config.fixed_mclk = 0;

  i2s_pin_config_t pin_config = {};
  pin_config.bck_io_num = PIN_I2S_BCLK;
  pin_config.ws_io_num = PIN_I2S_LRCLK;
  pin_config.data_out_num = PIN_I2S_DOUT;
  pin_config.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err == ESP_ERR_INVALID_STATE) {
    Serial.println("[I2S] driver already installed -> uninstall + retry");
    i2s_driver_uninstall(I2S_NUM_0);
    err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  }
  if (err != ESP_OK) {
    Serial.printf("[I2S] driver_install FAILED err=%d\n", (int)err);
    return false;
  }

  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[I2S] set_pin FAILED err=%d\n", (int)err);
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }

  err = i2s_set_clk(I2S_NUM_0, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  if (err != ESP_OK) {
    Serial.printf("[I2S] set_clk FAILED err=%d\n", (int)err);
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.printf("[I2S] OK sr=%lu BCLK=%d LRCLK=%d DOUT=%d\n",
                (unsigned long)sampleRate, PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);
  return true;
}

// =======================
// Playback
// =======================
struct PlayRequest { String path; };
static PlayRequest currentReq;

static void stopPlayback() {
  stopRequested = true;
  ampEnable(false);
  digitalWrite(PIN_STATUS_LED, LOW);
}

static String findFileByNumber(const String& folder, int number) {
  if (!sdReady) return "";
  if (!isValidFolder(folder)) return "";

  String dirPath = ensureFolderPath(folder);
  if (!SD.exists(dirPath)) return "";

  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) return "";

  String prefix = folder + "_" + String(number) + "_";

  File f;
  while ((f = dir.openNextFile())) {
    if (!f.isDirectory()) {
      String full = f.name();
      int slash = full.lastIndexOf('/');
      String base = (slash >= 0) ? full.substring(slash + 1) : full;
      String lower = base; lower.toLowerCase();
      if (lower.endsWith(".wav") && base.startsWith(prefix)) {
        f.close();
        dir.close();
        return makeFilePath(folder, base);
      }
    }
    f.close();
  }
  dir.close();
  return "";
}

static void playbackTask(void* param) {
  String path = currentReq.path;

  currentPlayingPath = path;
  isPlaying = true;

  Serial.printf("[PLAY] START path=%s\n", path.c_str());

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("[PLAY] ERROR open failed");
    isPlaying = false; currentPlayingPath = ""; playbackTaskHandle = nullptr;
    vTaskDelete(NULL); return;
  }

  WavInfo info = parseWav(f);
  if (!info.ok) {
    Serial.println("[PLAY] ERROR invalid WAV (need PCM16 mono/stereo)");
    f.close();
    isPlaying = false; currentPlayingPath = ""; playbackTaskHandle = nullptr;
    vTaskDelete(NULL); return;
  }

  Serial.printf("[PLAY] WAV ok sr=%lu ch=%u bits=%u data=%lu\n",
                (unsigned long)info.sampleRate,
                (unsigned)info.channels,
                (unsigned)info.bitsPerSample,
                (unsigned long)info.dataSize);

  if (!i2sInit(info.sampleRate)) {
    Serial.println("[PLAY] ERROR i2sInit failed");
    f.close();
    isPlaying = false; currentPlayingPath = ""; playbackTaskHandle = nullptr;
    vTaskDelete(NULL); return;
  }

  f.seek(info.dataStart);

  const size_t BUF_SAMPLES = 512;
  const size_t inBytes = (info.channels == 2) ? (BUF_SAMPLES * 4) : (BUF_SAMPLES * 2);

  uint8_t* inBuf = (uint8_t*)malloc(inBytes);
  int16_t* outBuf = (int16_t*)malloc(BUF_SAMPLES * 2 * sizeof(int16_t)); // immer stereo raus

  if (!inBuf || !outBuf) {
    Serial.println("[PLAY] ERROR malloc failed");
    if (inBuf) free(inBuf);
    if (outBuf) free(outBuf);
    f.close();
    isPlaying = false; currentPlayingPath = ""; playbackTaskHandle = nullptr;
    vTaskDelete(NULL); return;
  }

  stopRequested = false;

  uint32_t remaining = info.dataSize;
  uint32_t lastBlinkMs = 0;
  uint32_t lastDbg = 0;

  while (!stopRequested && remaining > 0) {
    uint32_t now = millis();
    if (now - lastBlinkMs >= 200) {
      lastBlinkMs = now;
      digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
    }

    size_t toRead = inBytes;
    if (remaining < toRead) toRead = remaining;

    int actuallyRead = f.read(inBuf, toRead);
    if (actuallyRead <= 0) break;

    remaining -= (uint32_t)actuallyRead;

    float vol = (float)cfg.volume / 100.0f;
    size_t frames = 0;

    if (info.channels == 1) {
      frames = (size_t)actuallyRead / 2;
      int16_t* src = (int16_t*)inBuf;
      for (size_t i = 0; i < frames; i++) {
        int16_t s = src[i];
        int32_t scaled = (int32_t)((float)s * vol);
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;
        outBuf[i * 2 + 0] = (int16_t)scaled;
        outBuf[i * 2 + 1] = (int16_t)scaled;
      }
    } else {
      frames = (size_t)actuallyRead / 4;
      int16_t* src = (int16_t*)inBuf;
      for (size_t i = 0; i < frames; i++) {
        int16_t l = src[i * 2 + 0];
        int16_t r = src[i * 2 + 1];
        int32_t sl = (int32_t)((float)l * vol);
        int32_t sr = (int32_t)((float)r * vol);
        if (sl > 32767) sl = 32767;
        if (sl < -32768) sl = -32768;
        if (sr > 32767) sr = 32767;
        if (sr < -32768) sr = -32768;
        outBuf[i * 2 + 0] = (int16_t)sl;
        outBuf[i * 2 + 1] = (int16_t)sr;
      }
    }

    // Peak-Log 1x pro Sekunde (frames ist hier gültig)
    if (AUDIO_DEBUG_PEAK && (now - lastDbg > 1000)) {
      lastDbg = now;
      int16_t peak = 0;
      for (size_t i = 0; i < frames * 2; i++) {
        int16_t v = outBuf[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
      }
      Serial.printf("[AUDIO] peak=%d vol=%d%% frames=%u\n", peak, cfg.volume, (unsigned)frames);
    }

    size_t bytesToWrite = frames * 2 * sizeof(int16_t);
    size_t bytesWritten = 0;
    esp_err_t err = i2s_write(I2S_NUM_0, outBuf, bytesToWrite, &bytesWritten, portMAX_DELAY);
    if (err != ESP_OK) {
      Serial.printf("[PLAY] ERROR i2s_write err=%d\n", (int)err);
      break;
    }
  }

  i2s_zero_dma_buffer(I2S_NUM_0);

  free(inBuf);
  free(outBuf);
  f.close();

  ampEnable(false);
  digitalWrite(PIN_STATUS_LED, LOW);

  isPlaying = false;
  currentPlayingPath = "";
  playbackTaskHandle = nullptr;

  Serial.printf("[PLAY] END stopRequested=%d remaining=%lu\n",
                stopRequested ? 1 : 0,
                (unsigned long)remaining);

  vTaskDelete(NULL);
}

static bool startPlaybackPath(const String& path) {
  if (!sdReady) return false;
  if (!SD.exists(path)) return false;

  if (playbackTaskHandle != nullptr) {
    stopPlayback();
    unsigned long t0 = millis();
    while (playbackTaskHandle != nullptr && millis() - t0 < 800) delay(10);
  }

  stopRequested = false;

  currentReq.path = path;

  ampSetGainHigh();  // nur wenn USE_AMP_GAIN_PIN=true
  ampEnable(true);
  delay(10);

  BaseType_t ok = xTaskCreatePinnedToCore(playbackTask, "playbackTask", 8192, nullptr, 2, &playbackTaskHandle, 1);
  if (ok != pdPASS) {
    playbackTaskHandle = nullptr;
    ampEnable(false);
    return false;
  }
  return true;
}

// =======================
// Upload
// =======================
static void handleUploadStream() {
  HTTPUpload& up = server.upload();
  String folder = server.hasArg("folder") ? server.arg("folder") : "";
  folder.trim();
  if (!isValidFolder(folder) || !sdReady) return;

  if (up.status == UPLOAD_FILE_START) {
    if (uploadFile) uploadFile.close();

    String filename = sanitizeFilename(up.filename);
    if (filename.length() == 0) filename = "upload.wav";
    if (!hasWavExt(filename)) filename += ".wav";

    String dir = ensureFolderPath(folder);
    if (!SD.exists(dir)) SD.mkdir(dir);

    String full = makeFilePath(folder, filename);
    if (SD.exists(full)) SD.remove(full);

    uploadFile = SD.open(full, FILE_WRITE);
  }
  else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(up.buf, up.currentSize);
  }
  else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
  }
  else if (up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
  }
}

static void handleUploadFinalize() {
  if (!sdReady) { sendJson(503, "{\"error\":\"SD not available\"}"); return; }
  sendJson(200, "{\"status\":\"uploaded\"}");
}

// =======================
// WiFi (AP-only stabil)
// =======================
static void wifiStopAll() {
  WiFi.persistent(false);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(250);
}

static bool wifiStartAP() {
  cfg.apSsid = cfg.apSsid.length() ? cfg.apSsid : String(AP_SSID_DEFAULT);
  cfg.apPass = cfg.apPass.length() ? cfg.apPass : String(AP_PASS_DEFAULT);
  cfg.apSsid.trim();
  cfg.apPass.trim();
  if (cfg.apSsid.length() == 0) cfg.apSsid = AP_SSID_DEFAULT;

  int ch = cfg.apChannel;
  if (ch < 0) ch = 0;
  if (ch > 13) ch = 13;
  bool wantOpen = (cfg.apPass.length() < 8);

  if (WIFI_DEBUG) {
    dumpHex("[WIFI] SSID", cfg.apSsid);
    dumpHex("[WIFI] PASS", cfg.apPass);
  }

  Serial.printf("[WIFI] AP start SSID='%s' PASSLEN=%d CH=%d (%s)\n",
                cfg.apSsid.c_str(), (int)cfg.apPass.length(), ch,
                wantOpen ? "OPEN" : "WPA2");

  WiFi.persistent(false);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(300);

  WiFi.mode(WIFI_AP);
  esp_wifi_set_max_tx_power(78);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  IPAddress ip(192, 168, 4, 1);
  IPAddress gw(192, 168, 4, 1);
  IPAddress mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, gw, mask);

  bool apOk = false;
  if (wantOpen) apOk = WiFi.softAP(cfg.apSsid.c_str(), nullptr, ch, 0, 4);
  else          apOk = WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.c_str(), ch, 0, 4);

  delay(250);

  if (!wantOpen) {
    wifi_config_t cur = {};
    esp_err_t g = esp_wifi_get_config(WIFI_IF_AP, &cur);
    if (g == ESP_OK) {
      cur.ap.authmode = WIFI_AUTH_WPA2_PSK;
      cur.ap.pmf_cfg.required = false;
      cur.ap.pmf_cfg.capable  = true;
      esp_wifi_set_config(WIFI_IF_AP, &cur);
    }
  }

  Serial.printf("[WIFI] softAP apOk=%d AP_IP=%s\n",
                apOk ? 1 : 0, WiFi.softAPIP().toString().c_str());
  return apOk;
}

static bool wifiApplyMode() {
  wifiStopAll();
  cfg.wifiMode = "ap";
  return wifiStartAP();
}

// =======================
// Config persistence
// =======================
static void loadConfig() {
  cfg.wifiMode = "ap";

  cfg.apSsid = prefs.getString("apSsid", AP_SSID_DEFAULT);
  cfg.apPass = prefs.getString("apPass", AP_PASS_DEFAULT);
  cfg.apSsid.trim();
  cfg.apPass.trim();

  cfg.apChannel = prefs.getInt("apCh", 6);
  if (cfg.apChannel < 0) cfg.apChannel = 0;
  if (cfg.apChannel > 13) cfg.apChannel = 13;

  cfg.staSsid = prefs.getString("staSsid", "");
  cfg.staPass = prefs.getString("staPass", "");

  cfg.volume = prefs.getInt("volume", 50);
  cfg.volume = clampInt(cfg.volume, 0, 100);

  cfg.powerMode = prefs.getString("powerMode", "always_on");
  cfg.btName = prefs.getString("btName", BT_NAME_DEFAULT);
  if (cfg.btName.length() == 0) cfg.btName = BT_NAME_DEFAULT;
  cfg.btPeerMac = prefs.getString("btPeerMac", "");

  cfg.commMode = "wifi";
  cfg.btEnabled = false;
}

static void saveConfig() {
  prefs.putString("wifiMode", cfg.wifiMode);
  prefs.putString("apSsid", cfg.apSsid);
  prefs.putString("apPass", cfg.apPass);
  prefs.putInt("apCh", cfg.apChannel);

  prefs.putString("staSsid", cfg.staSsid);
  prefs.putString("staPass", cfg.staPass);

  prefs.putInt("volume", cfg.volume);
  prefs.putString("powerMode", cfg.powerMode);

  prefs.putString("btName", cfg.btName);
  prefs.putString("btPeerMac", cfg.btPeerMac);
  prefs.putString("commMode", cfg.commMode);
  prefs.putBool("btEnabled", cfg.btEnabled);
}

// =======================
// HTTP Handlers
// =======================
static void handleFactoryReset() {
  sendJson(200, "{\"status\":\"factory_reset\"}");
  delay(150);
  prefs.clear();
  nvs_flash_erase();
  nvs_flash_init();
  delay(150);
  ESP.restart();
}

static void handleStatus() {
  String body = "{";
  body += "\"sdReady\":" + String(sdReady ? "true" : "false") + ",";
  body += "\"isPlaying\":" + String(isPlaying ? "true" : "false") + ",";
  body += "\"current\":\"" + jsonEscape(currentPlayingPath) + "\",";
  body += "\"volume\":" + String(cfg.volume) + ",";
  body += "\"wifiMode\":\"" + jsonEscape(cfg.wifiMode) + "\",";
  body += "\"apSsid\":\"" + jsonEscape(cfg.apSsid) + "\",";
  body += "\"apIp\":\"" + jsonEscape(WiFi.softAPIP().toString()) + "\",";
  body += "\"stations\":" + String(WiFi.softAPgetStationNum()) + ",";
  body += "\"commMode\":\"" + jsonEscape(cfg.commMode) + "\",";
  body += "\"btEnabled\":" + String(cfg.btEnabled ? "true" : "false");
  body += "}";
  sendJson(200, body);
}

static void handleList() {
  String folder = server.hasArg("folder") ? server.arg("folder") : "";
  folder.trim();

  if (!isValidFolder(folder)) { sendJson(400, "{\"error\":\"Invalid folder\",\"files\":[]}"); return; }
  if (!sdReady) { sendJson(503, "{\"error\":\"SD not available\",\"files\":[]}"); return; }

  String path = ensureFolderPath(folder);
  if (!SD.exists(path)) { sendJson(200, "{\"files\":[]}"); return; }

  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { sendJson(200, "{\"files\":[]}"); return; }

  String json = "{\"files\":[";
  bool first = true;
  File f;
  while ((f = dir.openNextFile())) {
    if (!f.isDirectory()) {
      String full = f.name();
      int slash = full.lastIndexOf('/');
      String base = (slash >= 0) ? full.substring(slash + 1) : full;
      String lower = base; lower.toLowerCase();
      if (lower.endsWith(".wav")) {
        if (!first) json += ",";
        json += "\"" + jsonEscape(base) + "\"";
        first = false;
      }
    }
    f.close();
  }
  dir.close();
  json += "]}";
  sendJson(200, json);
}

static void handleStop() {
  stopPlayback();
  sendJson(200, "{\"status\":\"stopping\"}");
}

static void handleVolume() {
  int iv = server.hasArg("value") ? server.arg("value").toInt() : cfg.volume;
  cfg.volume = clampInt(iv, 0, 100);
  saveConfig();
  sendJson(200, String("{\"volume\":") + cfg.volume + "}");
}

static void handlePlay() {
  String folder = server.hasArg("folder") ? server.arg("folder") : "";
  String file   = server.hasArg("file") ? server.arg("file") : "";
  folder.trim(); file.trim();

  if (!isValidFolder(folder) || file.length() == 0) {
    sendJson(400, "{\"error\":\"Usage: /play?folder=A&file=NAME.wav\"}");
    return;
  }
  String path = makeFilePath(folder, file);
  bool ok = startPlaybackPath(path);
  sendJson(ok ? 200 : 404, ok ? "{\"status\":\"playing\"}" : "{\"error\":\"not found\"}");
}

static void handlePlayNum() {
  String folder = server.hasArg("folder") ? server.arg("folder") : "";
  int num = server.hasArg("num") ? server.arg("num").toInt() : -1;
  folder.trim();

  if (!isValidFolder(folder) || num < 0) {
    sendJson(400, "{\"error\":\"Usage: /playnum?folder=A&num=3\"}");
    return;
  }

  String path = findFileByNumber(folder, num);
  if (path.length() == 0) {
    sendJson(404, "{\"error\":\"No matching file\"}");
    return;
  }

  bool ok = startPlaybackPath(path);
  sendJson(ok ? 200 : 500, ok ? "{\"status\":\"playing\"}" : "{\"error\":\"failed\"}");
}

static void handleDelete() {
  String folder = server.hasArg("folder") ? server.arg("folder") : "";
  String file = server.hasArg("file") ? server.arg("file") : "";
  folder.trim(); file.trim();

  if (!isValidFolder(folder) || file.length() == 0) { sendJson(400, "{\"error\":\"Usage: /delete?folder=A&file=...\"}"); return; }
  if (!sdReady) { sendJson(503, "{\"error\":\"SD not available\"}"); return; }

  String path = makeFilePath(folder, file);
  if (!SD.exists(path)) { sendJson(404, "{\"error\":\"File not found\"}"); return; }

  if (isPlaying && currentPlayingPath == path) stopPlayback();

  bool ok = SD.remove(path);
  sendJson(ok ? 200 : 500, ok ? "{\"status\":\"deleted\"}" : "{\"error\":\"delete failed\"}");
}

static void handleMove() {
  String from = server.hasArg("from") ? server.arg("from") : "";
  String to   = server.hasArg("to") ? server.arg("to") : "";
  String file = server.hasArg("file") ? server.arg("file") : "";
  from.trim(); to.trim(); file.trim();

  if (!isValidFolder(from) || !isValidFolder(to) || file.length() == 0) { sendJson(400, "{\"error\":\"Usage: /move?from=A&to=B&file=...\"}"); return; }
  if (!sdReady) { sendJson(503, "{\"error\":\"SD not available\"}"); return; }
  if (from == to) { sendJson(200, "{\"status\":\"noop\"}"); return; }

  String src = makeFilePath(from, file);
  String dst = makeFilePath(to, file);

  if (!SD.exists(src)) { sendJson(404, "{\"error\":\"Source not found\"}"); return; }
  if (SD.exists(dst))  { sendJson(409, "{\"error\":\"Destination exists\"}"); return; }

  String dstDir = ensureFolderPath(to);
  if (!SD.exists(dstDir)) SD.mkdir(dstDir);

  if (isPlaying && currentPlayingPath == src) stopPlayback();

  bool ok = SD.rename(src, dst);
  sendJson(ok ? 200 : 500, ok ? "{\"status\":\"moved\"}" : "{\"error\":\"move failed\"}");
}

//static void handleUploadFinalize() {
//  if (!sdReady) { sendJson(503, "{\"error\":\"SD not available\"}"); return; }
//  sendJson(200, "{\"status\":\"uploaded\"}");
//}

static void handleConfigGet() {
  String body = "{";
  body += "\"wifiMode\":\"" + jsonEscape(cfg.wifiMode) + "\",";
  body += "\"apSsid\":\"" + jsonEscape(cfg.apSsid) + "\",";
  body += "\"apPass\":\"" + jsonEscape(cfg.apPass) + "\",";
  body += "\"apChannel\":" + String(cfg.apChannel) + ",";
  body += "\"staSsid\":\"" + jsonEscape(cfg.staSsid) + "\",";
  body += "\"staPass\":\"" + jsonEscape(cfg.staPass) + "\",";
  body += "\"volume\":" + String(cfg.volume) + ",";
  body += "\"powerMode\":\"" + jsonEscape(cfg.powerMode) + "\",";
  body += "\"btName\":\"" + jsonEscape(cfg.btName) + "\",";
  body += "\"btPeerMac\":\"" + jsonEscape(cfg.btPeerMac) + "\",";
  body += "\"commMode\":\"" + jsonEscape(cfg.commMode) + "\",";
  body += "\"btEnabled\":" + String(cfg.btEnabled ? "true" : "false");
  body += "}";
  sendJson(200, body);
}

static void handleConfigPost() {
  String raw = server.arg("plain");
  if (raw.length() == 0) { sendJson(400, "{\"error\":\"Empty JSON\"}"); return; }

  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, raw)) { sendJson(400, "{\"error\":\"Invalid JSON\"}"); return; }

  if (doc.containsKey("apSsid"))    cfg.apSsid    = (const char*)doc["apSsid"];
  if (doc.containsKey("apPass"))    cfg.apPass    = (const char*)doc["apPass"];
  if (doc.containsKey("apChannel")) cfg.apChannel = (int)doc["apChannel"];

  if (doc.containsKey("volume"))    cfg.volume    = (int)doc["volume"];

  cfg.volume = clampInt(cfg.volume, 0, 100);
  if (cfg.apChannel < 0) cfg.apChannel = 0;
  if (cfg.apChannel > 13) cfg.apChannel = 13;

  cfg.commMode = "wifi";
  cfg.btEnabled = false;
  cfg.wifiMode = "ap";

  bool applyWifi = doc.containsKey("applyWifi") ? (bool)doc["applyWifi"] : false;

  saveConfig();

  if (applyWifi) {
    bool ok = wifiApplyMode();
    sendJson(ok ? 200 : 500, ok ? "{\"status\":\"saved_applied\"}" : "{\"status\":\"saved_but_wifi_failed\"}");
  } else {
    sendJson(200, "{\"status\":\"saved\"}");
  }
}

static void handleReboot() {
  sendJson(200, "{\"status\":\"rebooting\"}");
  delay(200);
  ESP.restart();
}

static void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) { handleOptions(); return; }
  sendJson(404, "{\"error\":\"Not Found\"}");
}

// =======================
// WiFi Events (AP side)
// =======================
static void printMac(const uint8_t* mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.print("[WIFI] STA CONNECTED mac=");
      printMac(info.wifi_ap_staconnected.mac);
      Serial.printf(" aid=%d stations=%d\n",
                    info.wifi_ap_staconnected.aid,
                    WiFi.softAPgetStationNum());
      break;

    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.print("[WIFI] STA DISCONNECTED mac=");
      printMac(info.wifi_ap_stadisconnected.mac);
      Serial.printf(" reason=%d stations=%d\n",
                    info.wifi_ap_stadisconnected.reason,
                    WiFi.softAPgetStationNum());
      break;

    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.print("[WIFI] STA GOT IP: ");
      Serial.print(IPAddress(info.wifi_ap_staipassigned.ip.addr).toString());
      Serial.printf(" stations=%d\n", WiFi.softAPgetStationNum());
      break;

    default:
      break;
  }
}

// =======================
// Setup / Loop
// =======================
static void blinkBoot(int n, int onMs, int offMs) {
  for (int i = 0; i < n; i++) {
    digitalWrite(PIN_STATUS_LED, HIGH); delay(onMs);
    digitalWrite(PIN_STATUS_LED, LOW);  delay(offMs);
  }
}

static void doFactoryResetBoot() {
  Serial.println("[FACTORY] BOOT held -> FULL NVS ERASE");
  prefs.clear();
  nvs_flash_erase();
  nvs_flash_init();
  blinkBoot(20, 80, 80);
  delay(200);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.onEvent(onWiFiEvent);

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  // AMP Pins
  pinMode(PIN_AMP_SD, OUTPUT);
  ampEnable(false);

  if (USE_AMP_GAIN_PIN) ampSetGainHigh();

  pinMode(0, INPUT_PULLUP);

  prefs.begin("carspeaker", false);

  Serial.println();
  Serial.println("=== CarSpeaker Phase 4 Boot ===");

  blinkBoot(5, 160, 160);

  if (digitalRead(0) == LOW) {
    doFactoryResetBoot();
  }

  loadConfig();

  // WLAN (AP-only)
  wifiApplyMode();

  Serial.printf("[WIFI] BOOT AP_IP=%s stations=%d\n",
                WiFi.softAPIP().toString().c_str(),
                WiFi.softAPgetStationNum());

  // SD init
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdReady = SD.begin(PIN_SD_CS, SPI, 25000000);
  if (sdReady) {
    if (!SD.exists("/A")) SD.mkdir("/A");
    if (!SD.exists("/B")) SD.mkdir("/B");
    if (!SD.exists("/C")) SD.mkdir("/C");
    if (!SD.exists("/D")) SD.mkdir("/D");
    Serial.println("[SD] OK");
  } else {
    Serial.println("[SD] ERROR: mount failed");
  }

  // HTTP routes
  server.on("/ping",     HTTP_GET,  [](){ sendJson(200, "{\"status\":\"OK\"}"); });
  server.on("/status",   HTTP_GET,  handleStatus);

  server.on("/list",     HTTP_GET,  handleList);
  server.on("/play",     HTTP_GET,  handlePlay);
  server.on("/playnum",  HTTP_GET,  handlePlayNum);

  server.on("/stop",     HTTP_POST, handleStop);
  server.on("/volume",   HTTP_POST, handleVolume);

  server.on("/delete",   HTTP_POST, handleDelete);
  server.on("/move",     HTTP_POST, handleMove);

  server.on("/upload",   HTTP_POST, handleUploadFinalize, handleUploadStream);

  server.on("/config",   HTTP_GET,  handleConfigGet);
  server.on("/config",   HTTP_POST, handleConfigPost);

  server.on("/reboot",   HTTP_POST, handleReboot);
  server.on("/factory",  HTTP_POST, handleFactoryReset);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("[HTTP] Ready.");

  btRunning = false;
  Serial.println("[BT] Disabled (WiFi-only build).");
}

void loop() {
  server.handleClient();
}
