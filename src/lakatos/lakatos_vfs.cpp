#include "lakatos_state.h"
#include "seed_apps.h"
#include "seed_gallery.h"

#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <base64.h>
#include <string.h>
#include <time.h>

static bool gLoMounted = false;
static bool gSdMounted = false;
static uint32_t gLastSdRetryMs = 0;

static WiFiClient gShClient;
static HTTPClient gShHttp;
static bool       gShStreamBusy = false;

enum VfsBackend {
  VFS_NONE = 0,
  VFS_LO,
  VFS_SD,
  VFS_SH
};

// Strips "/flash", "/sd", or "/net" prefix and returns normalized sub-path starting with "/"
static VfsBackend parseVfsPath(const char* vfsPath, String& outSubPath) {
  if (!vfsPath || vfsPath[0] != '/') return VFS_NONE;
  VfsBackend b = VFS_NONE;
  const char* rest = nullptr;

  if (strncmp(vfsPath, "/flash", 6) == 0 && (vfsPath[6] == '/' || vfsPath[6] == '\0')) {
    b = VFS_LO;
    rest = vfsPath + 6;
  } else if (strncmp(vfsPath, "/sd", 3) == 0 && (vfsPath[3] == '/' || vfsPath[3] == '\0')) {
    b = VFS_SD;
    rest = vfsPath + 3;
  } else if (strncmp(vfsPath, "/net", 4) == 0 && (vfsPath[4] == '/' || vfsPath[4] == '\0')) {
    b = VFS_SH;
    rest = vfsPath + 4;
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

static bool ensurePrefetchWorkerStarted();

static void seedLoAppFile(const char* loSubPath, const uint8_t* data, size_t len) {
  bool needSeed = true;
  if (LittleFS.exists(loSubPath)) {
    File f = LittleFS.open(loSubPath, "r");
    if (f && f.size() == len) {
      uint8_t hdrA[128];
      if (f.read(hdrA, 128) == 128 && memcmp(hdrA, data, 128) == 0) {
        needSeed = false;
      }
    }
    if (f) f.close();
  }
  if (needSeed) {
    File f = LittleFS.open(loSubPath, "w");
    if (f) {
      f.write(data, len);
      f.close();
      Serial.printf("[Lakatos VFS] Seeded /flash%s (%u bytes)\n",
                    loSubPath, (unsigned)len);
    }
  }
}

void initLakatosVfs() {
  if (LittleFS.begin(true, "/flash", 8, "spiffs")) {
    gLoMounted = true;
    Serial.println("[Lakatos VFS] Mounted /flash (LittleFS on spiffs partition).");
    seedLoAppFile("/life.lapp", kSeedApp_life, sizeof(kSeedApp_life));
    seedLoAppFile("/gallery.lapp", kSeedApp_gallery, sizeof(kSeedApp_gallery));
  } else {
    Serial.println("[Lakatos VFS] ERROR: Failed to mount /flash LittleFS!");
  }

  ensureSdMounted();
  ensurePrefetchWorkerStarted();
}

// ---------------------------------------------------------------------------
// Superhighway HTTP Helpers (/net/...) per doc/superhighway-client.md
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

// Parses a complete Superhighway LS response body:
//   Line 1: <expected_count> (decimal integer >= 0)
//   Next <expected_count> lines: "<YYYY-mm-dd> <HH:MM:SS> <size> <name>[/]"
//   Final line: ";"
// Returns the number of entries stored into outEntries (0..maxEntries),
// or -1 if the body is empty, truncated, malformed, or the entry count
// does not match <expected_count> followed by ";".
static int parseShLsBody(const String& body, LakatosFsEntry* outEntries,
                         int maxEntries, int* outTotalCount = nullptr) {
  if (outTotalCount) *outTotalCount = 0;
  int len = body.length();
  if (len <= 0) return -1;

  int nl = body.indexOf('\n', 0);
  if (nl < 0) return -1;
  String countLine = body.substring(0, nl);
  while (countLine.endsWith("\r")) {
    countLine.remove(countLine.length() - 1);
  }
  countLine.trim();
  if (countLine.length() == 0) return -1;
  for (size_t i = 0; i < countLine.length(); i++) {
    char c = countLine.charAt(i);
    if (c < '0' || c > '9') return -1;
  }
  int expectedCount = atoi(countLine.c_str());
  if (expectedCount < 0) return -1;

  int start = nl + 1;
  int seenCount = 0;
  int storedCount = 0;
  bool sawTerminator = false;

  while (start <= len) {
    if (start == len) break;
    nl = body.indexOf('\n', start);
    if (nl < 0) nl = len;
    String line = body.substring(start, nl);
    start = nl + 1;

    while (line.endsWith("\r")) {
      line.remove(line.length() - 1);
    }

    if (line == ";") {
      sawTerminator = true;
      while (start < len) {
        char c = body.charAt(start++);
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t') {
          return -1;
        }
      }
      break;
    }

    if (seenCount >= expectedCount) {
      return -1;
    }

    LakatosFsEntry tmp;
    if (!parseShLsLine(line, &tmp)) {
      return -1;
    }
    if (outEntries && storedCount < maxEntries) {
      outEntries[storedCount++] = tmp;
    }
    seenCount++;
  }

  if (!sawTerminator || seenCount != expectedCount) {
    return -1;
  }
  if (outTotalCount) *outTotalCount = expectedCount;
  return storedCount;
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
  int count = -1;
  if (b == VFS_LO) {
    if (gLoMounted) count = lsFsImpl(LittleFS, subPath, outEntries, maxEntries);
  } else if (b == VFS_SD) {
    if (ensureSdMounted()) count = lsFsImpl(SD, subPath, outEntries, maxEntries);
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return -1;
    WiFiClient cli;
    HTTPClient http;
    cli.setNoDelay(true);
    String url = buildShTargetUrl(subPath);
    if (!http.begin(cli, url)) {
      return -1;
    }
    addShBasicAuthHeader(http);
    int code = http.sendRequest("LS");
    if (code < 200 || code >= 300) {
      http.end();
      return -1;
    }
    String body = http.getString();
    http.end();

    count = parseShLsBody(body, outEntries, maxEntries);
    if (count < 0) {
      int nl = body.indexOf('\n');
      String firstLine = (nl >= 0) ? body.substring(0, nl) : body;
      String tail = (body.length() > 20) ? body.substring(body.length() - 20) : body;
      tail.replace("\n", "\\n");
      tail.replace("\r", "\\r");
      Serial.printf("[Lakatos VFS] LS %s failed framing/count check (len=%u, first='%s', tail='%s')\r\n",
                    subPath.c_str(), (unsigned)body.length(), firstLine.c_str(), tail.c_str());
    }
  }
  lakatosDevLogf("VFS ls %s -> %d entries", path ? path : "", count);
  return count;
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
    int totalCount = 0;
    if (parseShLsBody(body, outEntry, 1, &totalCount) == 1 && totalCount == 1) {
      return true;
    }
    if (body.endsWith("\n") && body.indexOf('\n') == (int)body.length() - 1) {
      return parseShLsLine(body, outEntry);
    }
    return false;
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
    if (offset == 0) {
      lakatosDevLogf("VFS read %s (len=%u) -> %ld B", path, (unsigned)len, (long)n);
    }
    return n;
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return -1;
    WiFiClient client;
    client.setNoDelay(true);
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
    while ((http.connected() || (stream && stream->available() > 0)) &&
           total < toRead && (millis() - startMs < 8000)) {
      size_t avail = stream ? stream->available() : 0;
      if (avail > 0) {
        size_t chunk = (avail > (toRead - total)) ? (toRead - total) : avail;
        int r = stream->read((uint8_t*)buf + total, chunk);
        if (r > 0) {
          total += (size_t)r;
          startMs = millis();
        }
      } else {
        delayMicroseconds(150);
      }
    }
    http.end();
    if (offset == 0) {
      lakatosDevLogf("VFS read %s (len=%u) -> %ld B", path, (unsigned)len, (long)total);
    }
    return (int32_t)total;
  }
  return -1;
}

bool lakatosFsWrite(const char* path, const void* buf, size_t len) {
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  bool ok = false;
  if (b == VFS_LO || b == VFS_SD) {
    fs::FS* fsPtr = (b == VFS_LO) ? (fs::FS*)&LittleFS : (fs::FS*)&SD;
    if (b == VFS_LO && !gLoMounted) return false;
    if (b == VFS_SD && !ensureSdMounted()) return false;
    File f = fsPtr->open(subPath.c_str(), "w");
    if (!f) return false;
    size_t w = (len > 0 && buf) ? f.write((const uint8_t*)buf, len) : 0;
    f.close();
    ok = (w == len);
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("PUT", (uint8_t*)buf, len);
    http.end();
    ok = (code >= 200 && code < 300);
  }
  lakatosDevLogf("VFS write %s (%u B) -> %s", path ? path : "", (unsigned)len, ok ? "OK" : "FAIL");
  return ok;
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
    client.setNoDelay(true);
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
  lakatosDevLogf("HTTP GET %s (max=%u, freeHeap=%u)",
                 url, (unsigned)maxLen, (unsigned)ESP.getFreeHeap());
  bool isHttps = (strncmp(url, "https://", 8) == 0);
  WiFiClient plainClient;
  NetworkClientSecure secureClient;
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("Lakatos-CYD/1.0");

  bool begun = false;
  if (isHttps) {
    secureClient.setInsecure();
    begun = http.begin(secureClient, url);
  } else {
    begun = http.begin(plainClient, url);
  }
  if (!begun) {
    lakatosDevLogf("HTTP GET begin failed: %s", url);
    return -1;
  }

  int code = http.GET();
  if (code < 200 || code >= 300) {
    lakatosDevLogf("HTTP GET status=%d: %s", code, url);
    http.end();
    return -code;
  }
  int contentLen = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  size_t total = 0;
  uint32_t startMs = millis();
  while ((http.connected() || (stream && stream->available() > 0)) &&
         total < maxLen &&
         (contentLen < 0 || total < (size_t)contentLen) &&
         (millis() - startMs < 4000)) {
    size_t avail = stream ? stream->available() : 0;
    if (avail > 0) {
      size_t chunk = (avail > (maxLen - total)) ? (maxLen - total) : avail;
      int r = stream->read((uint8_t*)buf + total, chunk);
      if (r > 0) {
        total += (size_t)r;
        startMs = millis();
      }
    } else {
      delayMicroseconds(200);
    }
  }
  if (total < maxLen) {
    ((uint8_t*)buf)[total] = 0;
  }
  http.end();
  lakatosDevLogf("HTTP GET ok -> %u B (code=%d)", (unsigned)total, code);
  return (int)total;
}

int lakatosHttpGetCached(const char* url, void* buf, size_t maxLen, uint32_t maxAgeSec) {
  if (!url || !url[0] || !buf || maxLen == 0) return -1;

  uint32_t hash = lakatosCityHash32(url, strlen(url));
  char cachePath[48];
  snprintf(cachePath, sizeof(cachePath), "/net/cache/%u", (unsigned)hash);

  uint8_t* bytes = (uint8_t*)buf;
  int32_t got = lakatosFsRead(cachePath, 0, bytes, maxLen);
  if (got > 0) {
    size_t scanLimit = ((size_t)got < 24) ? (size_t)got : 24;
    size_t nlIdx = 0;
    bool foundNl = false;
    bool validDigits = false;
    uint32_t cachedTime = 0;
    for (size_t i = 0; i < scanLimit; i++) {
      char c = (char)bytes[i];
      if (c == '\n') {
        nlIdx = i;
        foundNl = true;
        break;
      }
      if (c == '\r') continue;
      if (c >= '0' && c <= '9') {
        cachedTime = cachedTime * 10u + (uint32_t)(c - '0');
        validDigits = true;
      } else {
        validDigits = false;
        break;
      }
    }

    uint32_t nowSec = (uint32_t)lakatosGetUnixTime();
    if (foundNl && validDigits && cachedTime > 0 && nowSec > 0) {
      uint32_t age = (nowSec >= cachedTime) ? (nowSec - cachedTime) : 0;
      if (age <= maxAgeSec) {
        size_t payloadOff = nlIdx + 1;
        size_t payloadLen = (size_t)got - payloadOff;
        if (payloadLen > 0) {
          memmove(bytes, bytes + payloadOff, payloadLen);
        }
        if ((size_t)got == maxLen && payloadLen < maxLen) {
          int32_t extra = lakatosFsRead(cachePath, (uint32_t)maxLen,
                                        bytes + payloadLen, maxLen - payloadLen);
          if (extra > 0) {
            payloadLen += (size_t)extra;
          }
        }
        if (payloadLen < maxLen) {
          bytes[payloadLen] = 0;
        }
        lakatosDevLogf("HTTP CACHE HIT %s (age=%us <= %us, %u B)",
                       cachePath, (unsigned)age, (unsigned)maxAgeSec, (unsigned)payloadLen);
        return (int)payloadLen;
      }
      lakatosDevLogf("HTTP CACHE STALE %s (age=%us > %us)",
                     cachePath, (unsigned)age, (unsigned)maxAgeSec);
    }
  } else {
    lakatosDevLogf("HTTP CACHE MISS %s", cachePath);
  }

  int fetched = lakatosHttpGet(url, buf, maxLen);
  if (fetched < 0) {
    return fetched;
  }

  uint32_t saveTime = (uint32_t)lakatosGetUnixTime();
  char hdr[24];
  int hdrLen = snprintf(hdr, sizeof(hdr), "%u\n", (unsigned)saveTime);
  if (hdrLen > 0) {
    size_t totalCacheLen = (size_t)hdrLen + (size_t)fetched;
    uint8_t* cacheBuf = (uint8_t*)malloc(totalCacheLen);
    if (cacheBuf) {
      memcpy(cacheBuf, hdr, (size_t)hdrLen);
      if (fetched > 0) {
        memcpy(cacheBuf + hdrLen, buf, (size_t)fetched);
      }
      static bool sNetCacheDirReady = false;
      if (!sNetCacheDirReady) {
        sNetCacheDirReady = lakatosFsMkdir("/net/cache", true);
      }
      if (!lakatosFsWrite(cachePath, cacheBuf, totalCacheLen)) {
        lakatosFsMkdir("/net/cache", true);
        lakatosFsWrite(cachePath, cacheBuf, totalCacheLen);
      }
      free(cacheBuf);
    }
  }

  return fetched;
}

struct LakatosVfsStream {
  VfsBackend  backend    = VFS_NONE;
  File        file;
  WiFiClient  ownClient;
  HTTPClient  ownHttp;
  HTTPClient* httpPtr    = nullptr;
  WiFiClient* stream     = nullptr;
  bool        usesShared = false;
  int64_t     contentLen = -1;
  uint32_t    bytesRead  = 0;
  uint32_t    openMs     = 0;
};

LakatosVfsStream* lakatosFsOpenRead(const char* path) {
  if (!path || !path[0]) return nullptr;
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_NONE) return nullptr;

  LakatosVfsStream* s = new LakatosVfsStream();
  if (!s) return nullptr;
  s->backend = b;
  s->openMs  = millis();

  if (b == VFS_LO || b == VFS_SD) {
    fs::FS* fsPtr = (b == VFS_LO) ? (fs::FS*)&LittleFS : (fs::FS*)&SD;
    if (b == VFS_LO && !gLoMounted) { delete s; return nullptr; }
    if (b == VFS_SD && !ensureSdMounted()) { delete s; return nullptr; }
    s->file = fsPtr->open(subPath.c_str(), "r");
    if (!s->file || s->file.isDirectory()) {
      if (s->file) s->file.close();
      delete s;
      return nullptr;
    }
    s->contentLen = (int64_t)s->file.size();
    lakatosDevLogf("VFS open %s (%lld B)", path, (long long)s->contentLen);
    return s;
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) { delete s; return nullptr; }
    String url = buildShTargetUrl(subPath);
    WiFiClient* cli = nullptr;
    if (__sync_bool_compare_and_swap(&gShStreamBusy, false, true)) {
      s->usesShared = true;
      s->httpPtr    = &gShHttp;
      cli           = &gShClient;
    } else {
      s->usesShared = false;
      s->httpPtr    = &s->ownHttp;
      cli           = &s->ownClient;
    }
    cli->setNoDelay(true);
    cli->setTimeout(2000);
    s->httpPtr->setReuse(true);
    s->httpPtr->setTimeout(2500);
    if (!s->httpPtr->begin(*cli, url)) {
      if (s->usesShared) gShStreamBusy = false;
      delete s;
      return nullptr;
    }
    addShBasicAuthHeader(*s->httpPtr);
    uint32_t t0 = millis();
    int code = s->httpPtr->GET();
    if (code < 0 && s->usesShared) {
      // Stale keep-alive socket: close and retry once on a fresh TCP connection
      s->httpPtr->setReuse(false);
      s->httpPtr->end();
      cli->stop();
      cli->setNoDelay(true);
      s->httpPtr->setReuse(true);
      if (s->httpPtr->begin(*cli, url)) {
        addShBasicAuthHeader(*s->httpPtr);
        t0 = millis();
        code = s->httpPtr->GET();
      }
    }
    lakatosDevLogf("VFS-NET GET %s -> HTTP %d, len=%d, hdr=%lu ms (reuse=%d)",
                   path, code, s->httpPtr->getSize(), (unsigned long)(millis() - t0),
                   s->usesShared ? 1 : 0);
    if (code < 200 || code >= 300) {
      s->httpPtr->end();
      if (s->usesShared) gShStreamBusy = false;
      delete s;
      return nullptr;
    }
    s->contentLen = (int64_t)s->httpPtr->getSize();
    s->stream     = s->httpPtr->getStreamPtr();
    return s;
  }

  delete s;
  return nullptr;
}

static volatile bool gPrefetchCancelReq = false;

int32_t lakatosFsStreamRead(LakatosVfsStream* s, void* buf, size_t len) {
  if (!s || !buf || len == 0) return 0;
  if (s->backend == VFS_LO || s->backend == VFS_SD) {
    int32_t got = (int32_t)s->file.read((uint8_t*)buf, len);
    if (got > 0) s->bytesRead += (uint32_t)got;
    return got;
  } else if (s->backend == VFS_SH) {
    if (!s->stream || !s->httpPtr) return -1;
    if (s->contentLen >= 0) {
      int64_t rem = s->contentLen - (int64_t)s->bytesRead;
      if (rem <= 0) return 0;
      if ((int64_t)len > rem) len = (size_t)rem;
    }
    size_t total = 0;
    uint32_t startMs = millis();
    uint32_t lastYieldMs = startMs;
    bool onCore0 = (xPortGetCoreID() == 0);
    while (total < len &&
           !(onCore0 && gPrefetchCancelReq) &&
           (s->httpPtr->connected() || s->stream->available() > 0) &&
           (millis() - startMs < 5000)) {
      size_t avail = s->stream->available();
      if (avail > 0) {
        size_t chunk = (avail > (len - total)) ? (len - total) : avail;
        int r = s->stream->read((uint8_t*)buf + total, chunk);
        if (r > 0) {
          total += (size_t)r;
          s->bytesRead += (uint32_t)r;
          startMs = millis();
        }
      } else {
        uint32_t now = millis();
        if (now - lastYieldMs >= 40) {
          vTaskDelay(1);
          lastYieldMs = millis();
        } else {
          delayMicroseconds(100);
        }
      }
    }
    return (int32_t)total;
  }
  return -1;
}

bool lakatosFsStreamSkip(LakatosVfsStream* s, size_t len) {
  if (!s) return false;
  if (len == 0) return true;
  if (s->backend == VFS_LO || s->backend == VFS_SD) {
    bool ok = s->file.seek(len, SeekCur);
    if (ok) s->bytesRead += (uint32_t)len;
    return ok;
  } else if (s->backend == VFS_SH) {
    uint8_t discard[256];
    size_t rem = len;
    while (rem > 0) {
      size_t chunk = (rem > sizeof(discard)) ? sizeof(discard) : rem;
      int32_t got = lakatosFsStreamRead(s, discard, chunk);
      if (got <= 0) return false;
      rem -= (size_t)got;
    }
    return true;
  }
  return false;
}

void lakatosFsClose(LakatosVfsStream* s) {
  if (!s) return;
  if (s->backend == VFS_LO || s->backend == VFS_SD) {
    if (s->file) s->file.close();
  } else if (s->backend == VFS_SH && s->httpPtr) {
    // If only a few trailing bytes remain (e.g. 16-byte PNG IEND), drain them
    // so HTTP Keep-Alive can reuse the open TCP socket cleanly.
    // If more than 512 bytes remain (e.g. truncated at 100 KB or canceled),
    // disable reuse so HTTPClient::end() closes the socket cleanly.
    if (s->contentLen > 0 && s->bytesRead < (uint32_t)s->contentLen) {
      if (((uint32_t)s->contentLen - s->bytesRead) <= 512) {
        lakatosFsStreamSkip(s, (size_t)((uint32_t)s->contentLen - s->bytesRead));
      } else {
        s->httpPtr->setReuse(false);
      }
    }
    lakatosDevLogf("VFS-NET CLOSE read=%lu B, streamTotal=%lu ms",
                   (unsigned long)s->bytesRead,
                   (unsigned long)(millis() - s->openMs));
    s->httpPtr->end();
    if (s->usesShared) gShStreamBusy = false;
  }
  delete s;
}

// ---------------------------------------------------------------------------
// Core 0 Background File Prefetcher
// ---------------------------------------------------------------------------
static TaskHandle_t      gPrefetchTaskHandle   = nullptr;
static SemaphoreHandle_t gPrefetchWakeSem      = nullptr;
static volatile int      gPrefetchStatus       = 0; // 0=IDLE, 1=BUSY, 2=READY, -1=ERR
static volatile uint32_t gPrefetchBytesGot     = 0;
static volatile bool     gPrefetchWorkerActive = false;
static char              gPrefetchPath[128];
static uint8_t*          gPrefetchDstBuf       = nullptr;
static size_t            gPrefetchMaxLen       = 0;
static uint8_t*          gPrefetchOwnedBuf     = nullptr;
static size_t            gPrefetchOwnedCap     = 0;

static void prefetchWorkerTaskCore0(void* arg) {
  (void)arg;
  for (;;) {
    xSemaphoreTake(gPrefetchWakeSem, portMAX_DELAY);
    if (gPrefetchCancelReq || gPrefetchStatus != 1) {
      gPrefetchWorkerActive = false;
      continue;
    }
    gPrefetchWorkerActive = true;
    uint32_t t0 = millis();
    LakatosVfsStream* s = lakatosFsOpenRead(gPrefetchPath);
    if (!s) {
      if (!gPrefetchCancelReq) {
        gPrefetchStatus = -1;
      }
      gPrefetchWorkerActive = false;
      continue;
    }

    size_t total = 0;
    bool err = false;
    uint32_t lastYieldMs = millis();
    while (total < gPrefetchMaxLen && !gPrefetchCancelReq) {
      size_t toRead = gPrefetchMaxLen - total;
      if (toRead > 2048) toRead = 2048;
      int32_t got = lakatosFsStreamRead(s, gPrefetchDstBuf + total, toRead);
      if (got < 0) {
        err = true;
        break;
      }
      if (got == 0) {
        break; // EOF
      }
      total += (size_t)got;
      gPrefetchBytesGot = (uint32_t)total;
      if (millis() - lastYieldMs >= 40) {
        vTaskDelay(1);
        lastYieldMs = millis();
      }
    }

    if (gPrefetchCancelReq) {
      if (s->backend == VFS_SH && s->httpPtr) {
        s->httpPtr->setReuse(false);
      }
      lakatosFsClose(s);
      gPrefetchWorkerActive = false;
      continue;
    }

    lakatosFsClose(s);
    uint32_t elapsed = millis() - t0;
    if (err || total == 0) {
      gPrefetchStatus = -1;
      lakatosDevLogf("CORE0-PREFETCH ERROR %s (%lu ms)",
                     gPrefetchPath, (unsigned long)elapsed);
    } else {
      gPrefetchBytesGot = (uint32_t)total;
      gPrefetchStatus   = 2;
      lakatosDevLogf("CORE0-PREFETCH READY %s -> %lu B in %lu ms (core=%d)",
                     gPrefetchPath, (unsigned long)total, (unsigned long)elapsed,
                     (int)xPortGetCoreID());
    }
    gPrefetchWorkerActive = false;
  }
}

static bool ensurePrefetchWorkerStarted() {
  if (gPrefetchTaskHandle != nullptr) return true;
  if (!gPrefetchWakeSem) {
    gPrefetchWakeSem = xSemaphoreCreateBinary();
    if (!gPrefetchWakeSem) return false;
  }
  BaseType_t ok = xTaskCreatePinnedToCore(
      prefetchWorkerTaskCore0,
      "lak_prefetch",
      3072,
      nullptr,
      2,
      &gPrefetchTaskHandle,
      0 // Pin to Core 0 (PRO_CPU)
  );
  return (ok == pdPASS && gPrefetchTaskHandle != nullptr);
}

bool lakatosPrefetchReserve(size_t cap) {
  if (cap == 0) return false;
  if (!gPrefetchOwnedBuf || gPrefetchOwnedCap < cap) {
    if (gPrefetchOwnedBuf) {
      free(gPrefetchOwnedBuf);
      gPrefetchOwnedBuf = nullptr;
      gPrefetchOwnedCap = 0;
    }
    // Allocate the 100 KB contiguous buffer FIRST from DRAM1 before starting
    // the worker task or allocating LCodeVM/arena structures.
    gPrefetchOwnedBuf = (uint8_t*)malloc(cap);
    if (!gPrefetchOwnedBuf) {
      Serial.printf("[Lakatos Prefetch] reserve(%u) failed (maxContig=%u, freeHeap=%u)\r\n",
                    (unsigned)cap,
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                    (unsigned)ESP.getFreeHeap());
      return false;
    }
    gPrefetchOwnedCap = cap;
  }
  return ensurePrefetchWorkerStarted();
}

void lakatosPrefetchCancel() {
  gPrefetchCancelReq = true;
  uint32_t t0 = millis();
  while (gPrefetchWorkerActive && (millis() - t0 < 3000)) {
    delay(1);
  }
  gPrefetchStatus    = 0;
  gPrefetchCancelReq = false;
}

void lakatosPrefetchFree() {
  lakatosPrefetchCancel();
  if (gPrefetchOwnedBuf) {
    free(gPrefetchOwnedBuf);
    gPrefetchOwnedBuf = nullptr;
    gPrefetchOwnedCap = 0;
  }
  gPrefetchDstBuf   = nullptr;
  gPrefetchBytesGot = 0;
}

bool lakatosPrefetchStart(const char* vfsPath, uint8_t* dstBuf, size_t maxLen) {
  if (!vfsPath || !vfsPath[0] || maxLen == 0) return false;

  lakatosPrefetchCancel();

  if (!dstBuf) {
    if (!lakatosPrefetchReserve(maxLen)) return false;
    dstBuf = gPrefetchOwnedBuf;
  } else {
    if (!ensurePrefetchWorkerStarted()) return false;
  }

  strncpy(gPrefetchPath, vfsPath, sizeof(gPrefetchPath) - 1);
  gPrefetchPath[sizeof(gPrefetchPath) - 1] = '\0';
  gPrefetchDstBuf       = dstBuf;
  gPrefetchMaxLen       = maxLen;
  gPrefetchBytesGot     = 0;
  gPrefetchCancelReq    = false;
  gPrefetchStatus       = 1;
  gPrefetchWorkerActive = true;
  xSemaphoreGive(gPrefetchWakeSem);
  return true;
}

int lakatosPrefetchStatus(uint32_t* outBytesGot) {
  if (outBytesGot) *outBytesGot = gPrefetchBytesGot;
  return gPrefetchStatus;
}

const uint8_t* lakatosPrefetchBuffer(uint32_t* outBytesGot) {
  if (outBytesGot) *outBytesGot = gPrefetchBytesGot;
  return gPrefetchDstBuf;
}
