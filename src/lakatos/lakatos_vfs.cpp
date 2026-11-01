#include "lakatos_state.h"
#include "seed_apps.h"

#include <HTTPClient.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <base64.h>
#include <string.h>
#include <time.h>

static bool gLoMounted = false;
static bool gSdMounted = false;
static uint32_t gLastSdRetryMs = 0;

enum VfsBackend {
  VFS_NONE = 0,
  VFS_LO,
  VFS_SD,
  VFS_SH
};

// Strips "/lo", "/sd", or "/sh" prefix and returns normalized sub-path starting with "/"
static VfsBackend parseVfsPath(const char* vfsPath, String& outSubPath) {
  if (!vfsPath || vfsPath[0] != '/') return VFS_NONE;
  VfsBackend b = VFS_NONE;
  const char* rest = nullptr;

  if (strncmp(vfsPath, "/lo", 3) == 0 && (vfsPath[3] == '/' || vfsPath[3] == '\0')) {
    b = VFS_LO;
    rest = vfsPath + 3;
  } else if (strncmp(vfsPath, "/sd", 3) == 0 && (vfsPath[3] == '/' || vfsPath[3] == '\0')) {
    b = VFS_SD;
    rest = vfsPath + 3;
  } else if (strncmp(vfsPath, "/sh", 3) == 0 && (vfsPath[3] == '/' || vfsPath[3] == '\0')) {
    b = VFS_SH;
    rest = vfsPath + 3;
  } else {
    return VFS_NONE;
  }

  if (!rest || rest[0] == '\0') {
    outSubPath = "/";
  } else {
    outSubPath = String(rest);
    if (outSubPath.length() > 1 && outSubPath.endsWith("/")) {
      outSubPath.remove(outSubPath.length() - 1);
    }
  }
  if (outSubPath.indexOf("..") >= 0) {
    return VFS_NONE;
  }
  return b;
}

static bool ensureSdMounted() {
  if (gSdMounted) return true;
  uint32_t now = millis();
  if (now - gLastSdRetryMs < 1500 && gLastSdRetryMs != 0) {
    return false;
  }
  gLastSdRetryMs = now;
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (SD.begin(SD_CS_PIN, SPI, 16000000)) {
    gSdMounted = true;
    Serial.println("[Lakatos VFS] Mounted /sd (MicroSD card).");
    return true;
  }
  return false;
}

bool lakatosSdIsMounted() {
  return ensureSdMounted();
}

void initLakatosVfs() {
  if (LittleFS.begin(true, "/lo", 8, "spiffs")) {
    gLoMounted = true;
    Serial.println("[Lakatos VFS] Mounted /lo (LittleFS on spiffs partition).");

    // Seed /lo/life.lapp if not present or if outdated
    bool needSeed = true;
    if (LittleFS.exists("/life.lapp")) {
      File f = LittleFS.open("/life.lapp", "r");
      if (f && f.size() == sizeof(kSeedApp_life)) {
        uint8_t hdrA[128];
        if (f.read(hdrA, 128) == 128 &&
            memcmp(hdrA, kSeedApp_life, 128) == 0) {
          needSeed = false;
        }
      }
      if (f) f.close();
    }
    if (needSeed) {
      File f = LittleFS.open("/life.lapp", "w");
      if (f) {
        f.write(kSeedApp_life, sizeof(kSeedApp_life));
        f.close();
        Serial.printf("[Lakatos VFS] Seeded /lo/life.lapp (%u bytes)\n",
                      (unsigned)sizeof(kSeedApp_life));
      }
    }
  } else {
    Serial.println("[Lakatos VFS] ERROR: Failed to mount /lo LittleFS!");
  }

  ensureSdMounted();
}

// ---------------------------------------------------------------------------
// Superhighway HTTP Helpers (/sh/...) per doc/superhighway-client.md
// ---------------------------------------------------------------------------
static bool isUnreservedUrlChar(uint8_t c) {
  return (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || c == '~';
}

static String urlEncodePath(const String& remotePath) {
  String out = "";
  for (size_t i = 0; i < remotePath.length(); i++) {
    uint8_t c = (uint8_t)remotePath[i];
    if (c == '/') {
      out += '/';
    } else if (isUnreservedUrlChar(c)) {
      out += (char)c;
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  if (!out.startsWith("/")) out = "/" + out;
  return out;
}

static String buildShTargetUrl(const String& subPath, const char* query = nullptr) {
  String base = String(lakatosGetSuperhighwayUrl());
  base.trim();
  while (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  String url = base + urlEncodePath(subPath);
  if (query && query[0]) {
    url += "?";
    url += query;
  }
  return url;
}

static void addShBasicAuthHeader(HTTPClient& http) {
  String user = String(lakatosGetDeviceName()) + String(lakatosGetSerialNumber());
  String pass = String(lakatosGetSuperhighwayPassword());
  if (user.length() > 0 || pass.length() > 0) {
    String raw = user + ":" + pass;
    String b64 = base64::encode(raw);
    http.addHeader("Authorization", "Basic " + b64);
  }
}

// Parses "<YYYY-mm-dd> <HH:MM:SS> <size> <name>[/]" by splitting on first 3 spaces
static bool parseShLsLine(const String& rawLine, LakatosFsEntry* out) {
  String line = rawLine;
  while (line.endsWith("\r") || line.endsWith("\n")) {
    line.remove(line.length() - 1);
  }
  if (line.length() < 21) return false;

  int sp1 = line.indexOf(' ');
  if (sp1 != 10) return false;
  int sp2 = line.indexOf(' ', sp1 + 1);
  if (sp2 != 19) return false;
  int sp3 = line.indexOf(' ', sp2 + 1);
  if (sp3 < 21) return false;

  String sizeStr = line.substring(sp2 + 1, sp3);
  String nameStr = line.substring(sp3 + 1);
  if (nameStr.length() == 0) return false;

  memset(out, 0, sizeof(*out));
  out->size = atoll(sizeStr.c_str());
  out->mtime = 0;

  if (nameStr.endsWith("/")) {
    out->isDir = true;
    nameStr.remove(nameStr.length() - 1);
  } else {
    out->isDir = false;
  }
  strncpy(out->name, nameStr.c_str(), sizeof(out->name) - 1);
  out->name[sizeof(out->name) - 1] = '\0';
  return out->name[0] != '\0';
}

// ---------------------------------------------------------------------------
// Unified VFS Implementation
// ---------------------------------------------------------------------------
static int lsFsImpl(fs::FS& fs, const String& subPath,
                    LakatosFsEntry* outEntries, int maxEntries) {
  File dir = fs.open(subPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return -1;
  }
  int count = 0;
  File f = dir.openNextFile();
  while (f && count < maxEntries) {
    LakatosFsEntry& e = outEntries[count];
    memset(&e, 0, sizeof(e));
    const char* nm = f.name();
    const char* slash = strrchr(nm, '/');
    if (slash) nm = slash + 1;
    strncpy(e.name, nm, sizeof(e.name) - 1);
    e.size = f.size();
    e.mtime = f.getLastWrite();
    e.isDir = f.isDirectory();
    if (e.name[0] != '\0') count++;
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return count;
}

int lakatosFsLs(const char* path, LakatosFsEntry* outEntries, int maxEntries) {
  if (!outEntries || maxEntries <= 0) return -1;
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_LO) {
    if (!gLoMounted) return -1;
    return lsFsImpl(LittleFS, subPath, outEntries, maxEntries);
  } else if (b == VFS_SD) {
    if (!ensureSdMounted()) return -1;
    return lsFsImpl(SD, subPath, outEntries, maxEntries);
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return -1;
    WiFiClient client;
    HTTPClient http;
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return -1;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("LS");
    if (code < 200 || code >= 300) {
      http.end();
      return -1;
    }
    String body = http.getString();
    http.end();

    int count = 0;
    int start = 0;
    while (start < (int)body.length() && count < maxEntries) {
      int nl = body.indexOf('\n', start);
      if (nl < 0) nl = body.length();
      String line = body.substring(start, nl);
      if (parseShLsLine(line, &outEntries[count])) {
        count++;
      }
      start = nl + 1;
    }
    return count;
  }
  return -1;
}

bool lakatosFsStat(const char* path, LakatosFsEntry* outEntry) {
  if (!outEntry) return false;
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_LO || b == VFS_SD) {
    fs::FS* fsPtr = (b == VFS_LO) ? (fs::FS*)&LittleFS : (fs::FS*)&SD;
    if (b == VFS_LO && !gLoMounted) return false;
    if (b == VFS_SD && !ensureSdMounted()) return false;
    File f = fsPtr->open(subPath.c_str(), "r");
    if (!f) return false;
    memset(outEntry, 0, sizeof(*outEntry));
    const char* nm = f.name();
    const char* slash = strrchr(nm, '/');
    if (slash) nm = slash + 1;
    strncpy(outEntry->name, nm, sizeof(outEntry->name) - 1);
    outEntry->size = f.size();
    outEntry->mtime = f.getLastWrite();
    outEntry->isDir = f.isDirectory();
    f.close();
    return true;
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("LS");
    if (code < 200 || code >= 300) {
      http.end();
      return false;
    }
    String body = http.getString();
    http.end();
    return parseShLsLine(body, outEntry);
  }
  return false;
}

int32_t lakatosFsRead(const char* path, uint32_t offset, void* buf, size_t len) {
  if (!buf || len == 0) return 0;
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_LO || b == VFS_SD) {
    fs::FS* fsPtr = (b == VFS_LO) ? (fs::FS*)&LittleFS : (fs::FS*)&SD;
    if (b == VFS_LO && !gLoMounted) return -1;
    if (b == VFS_SD && !ensureSdMounted()) return -1;
    File f = fsPtr->open(subPath.c_str(), "r");
    if (!f || f.isDirectory()) {
      if (f) f.close();
      return -1;
    }
    if (offset > 0 && !f.seek(offset, SeekSet)) {
      f.close();
      return 0;
    }
    int32_t n = (int32_t)f.read((uint8_t*)buf, len);
    f.close();
    return n;
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return -1;
    WiFiClient client;
    HTTPClient http;
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return -1;
    addShBasicAuthHeader(http);
    char rangeHdr[48];
    snprintf(rangeHdr, sizeof(rangeHdr), "bytes=%lu-%lu",
             (unsigned long)offset, (unsigned long)(offset + len - 1));
    http.addHeader("Range", rangeHdr);
    int code = http.GET();
    if (code != 200 && code != 206) {
      http.end();
      return -1;
    }
    WiFiClient* stream = http.getStreamPtr();
    int contentLen = http.getSize();
    size_t toRead = len;
    if (contentLen >= 0 && (size_t)contentLen < toRead) {
      toRead = (size_t)contentLen;
    }
    size_t total = 0;
    uint32_t startMs = millis();
    while (http.connected() && total < toRead && (millis() - startMs < 8000)) {
      size_t avail = stream->available();
      if (avail > 0) {
        size_t chunk = (avail > (toRead - total)) ? (toRead - total) : avail;
        int r = stream->read((uint8_t*)buf + total, chunk);
        if (r > 0) {
          total += (size_t)r;
          startMs = millis();
        }
      } else {
        delay(2);
      }
    }
    http.end();
    return (int32_t)total;
  }
  return -1;
}

bool lakatosFsWrite(const char* path, const void* buf, size_t len) {
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_LO || b == VFS_SD) {
    fs::FS* fsPtr = (b == VFS_LO) ? (fs::FS*)&LittleFS : (fs::FS*)&SD;
    if (b == VFS_LO && !gLoMounted) return false;
    if (b == VFS_SD && !ensureSdMounted()) return false;
    File f = fsPtr->open(subPath.c_str(), "w");
    if (!f) return false;
    size_t w = (len > 0 && buf) ? f.write((const uint8_t*)buf, len) : 0;
    f.close();
    return w == len;
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("PUT", (uint8_t*)buf, len);
    http.end();
    return (code >= 200 && code < 300);
  }
  return false;
}

bool lakatosFsWriteAt(const char* path, uint32_t offset, const void* buf, size_t len) {
  if (!buf || len == 0) return false;
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_LO || b == VFS_SD) {
    fs::FS* fsPtr = (b == VFS_LO) ? (fs::FS*)&LittleFS : (fs::FS*)&SD;
    if (b == VFS_LO && !gLoMounted) return false;
    if (b == VFS_SD && !ensureSdMounted()) return false;
    File f = fsPtr->open(subPath.c_str(), "r+");
    if (!f) {
      f = fsPtr->open(subPath.c_str(), "w");
    }
    if (!f) return false;
    if (!f.seek(offset, SeekSet)) {
      f.close();
      return false;
    }
    size_t w = f.write((const uint8_t*)buf, len);
    f.close();
    return w == len;
  }
  return false;
}

bool lakatosFsMkdir(const char* path, bool parents) {
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_LO) {
    return gLoMounted && LittleFS.mkdir(subPath.c_str());
  } else if (b == VFS_SD) {
    return ensureSdMounted() && SD.mkdir(subPath.c_str());
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    String url = buildShTargetUrl(subPath, parents ? "p=1" : nullptr);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("MKDIR");
    http.end();
    return (code >= 200 && code < 300);
  }
  return false;
}

bool lakatosFsRmdir(const char* path) {
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_LO) {
    return gLoMounted && LittleFS.rmdir(subPath.c_str());
  } else if (b == VFS_SD) {
    return ensureSdMounted() && SD.rmdir(subPath.c_str());
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("RMDIR");
    http.end();
    return (code >= 200 && code < 300);
  }
  return false;
}

bool lakatosFsRm(const char* path) {
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_LO) {
    return gLoMounted && LittleFS.remove(subPath.c_str());
  } else if (b == VFS_SD) {
    return ensureSdMounted() && SD.remove(subPath.c_str());
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("RM");
    http.end();
    return (code >= 200 && code < 300);
  }
  return false;
}

static int64_t duFsRecursive(fs::FS& fs, const String& subPath) {
  File dir = fs.open(subPath.c_str());
  if (!dir) return -1;
  if (!dir.isDirectory()) {
    int64_t sz = dir.size();
    dir.close();
    return sz;
  }
  int64_t sum = 0;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      sum += f.size();
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return sum;
}

int64_t lakatosFsDu(const char* path) {
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_LO) {
    return gLoMounted ? duFsRecursive(LittleFS, subPath) : -1;
  } else if (b == VFS_SD) {
    return ensureSdMounted() ? duFsRecursive(SD, subPath) : -1;
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return -1;
    WiFiClient client;
    HTTPClient http;
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return -1;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("DU");
    if (code < 200 || code >= 300) {
      http.end();
      return -1;
    }
    String body = http.getString();
    http.end();
    body.trim();
    return atoll(body.c_str());
  }
  return -1;
}

int lakatosHttpGet(const char* url, void* buf, size_t maxLen) {
  if (!url || !buf || maxLen == 0 || WiFi.status() != WL_CONNECTED) return -1;
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return -1;
  int code = http.GET();
  if (code < 200 || code >= 300) {
    http.end();
    return -code;
  }
  WiFiClient* stream = http.getStreamPtr();
  size_t total = 0;
  uint32_t startMs = millis();
  while (http.connected() && total < maxLen && (millis() - startMs < 6000)) {
    size_t avail = stream->available();
    if (avail > 0) {
      size_t chunk = (avail > (maxLen - total)) ? (maxLen - total) : avail;
      int r = stream->read((uint8_t*)buf + total, chunk);
      if (r > 0) {
        total += (size_t)r;
        startMs = millis();
      }
    } else {
      delay(2);
    }
  }
  http.end();
  return (int)total;
}
