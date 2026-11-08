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
static const char* gLastVfsErr = "EIO";

const char* lakatosFsLastError() {
  return gLastVfsErr ? gLastVfsErr : "EIO";
}

static void setShHttpError(const char* op, const String& subPath, int code) {
  if (code == 403 || code == 401 || code == 418) {
    gLastVfsErr = "EACCES";
  } else if (code == 404) {
    gLastVfsErr = "ENOENT";
  } else if (code == 409) {
    gLastVfsErr = "EEXIST";
  } else if (code == 400) {
    gLastVfsErr = "EINVAL";
  } else {
    gLastVfsErr = "EIO";
  }
  Serial.printf("[Lakatos VFS] /net %s %s failed: HTTP %d (%s)\r\n",
                op, subPath.c_str(), code, gLastVfsErr);
}

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
    gLastVfsErr = "ENOMEDIUM";
    return false;
  }
  gLastSdRetryMs = now;
  SD.end();
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (SD.begin(SD_CS_PIN, SPI, 16000000)) {
    gSdMounted = true;
    Serial.println("[Lakatos VFS] Mounted /sd (MicroSD card).");
    return true;
  }
  gLastVfsErr = "ENOMEDIUM";
  return false;
}

bool lakatosSdIsMounted() {
  return ensureSdMounted();
}

void lakatosGetFlashStorageInfo(LakatosStorageInfo* outInfo) {
  if (!outInfo) return;
  outInfo->flashMounted    = false;
  outInfo->flashTotalBytes = 0;
  outInfo->flashUsedBytes  = 0;
  outInfo->flashFreeBytes  = 0;
  if (gLoMounted) {
    outInfo->flashMounted    = true;
    outInfo->flashTotalBytes = (uint64_t)LittleFS.totalBytes();
    outInfo->flashUsedBytes  = (uint64_t)LittleFS.usedBytes();
    outInfo->flashFreeBytes  = (outInfo->flashTotalBytes >= outInfo->flashUsedBytes)
                                   ? (outInfo->flashTotalBytes - outInfo->flashUsedBytes)
                                   : 0;
  }
}

bool lakatosProbeSdMounted(LakatosStorageInfo* outInfo, bool forceProbeSd) {
  if (!outInfo) return false;
  outInfo->sdMounted    = false;
  outInfo->sdCardType   = "None";
  outInfo->sdTotalBytes = 0;
  outInfo->sdUsedBytes  = 0;
  outInfo->sdFreeBytes  = 0;

  if (forceProbeSd) {
    gLastSdRetryMs = 0;
    if (gSdMounted) {
      File root = SD.open("/");
      if (!root) {
        SD.end();
        gSdMounted = false;
      } else {
        root.close();
      }
    }
  }

  if (ensureSdMounted()) {
    if (SD.cardSize() == 0) {
      SD.end();
      gSdMounted = false;
      gLastVfsErr = "ENOMEDIUM";
      return false;
    }
    outInfo->sdMounted = true;
    sdcard_type_t ct = SD.cardType();
    if (ct == CARD_SDHC) {
      outInfo->sdCardType = "SDHC";
    } else if (ct == CARD_SD) {
      outInfo->sdCardType = "SD";
    } else if (ct == CARD_MMC) {
      outInfo->sdCardType = "MMC";
    } else {
      outInfo->sdCardType = "SD";
    }
    return true;
  }
  return false;
}

bool lakatosGetSdStorageSpace(LakatosStorageInfo* outInfo) {
  if (!outInfo || !outInfo->sdMounted) return false;
  uint64_t tot = SD.totalBytes();
  if (tot == 0) {
    SD.end();
    gSdMounted = false;
    gLastVfsErr = "ENOMEDIUM";
    outInfo->sdMounted = false;
    outInfo->sdCardType = "None";
    return false;
  }
  outInfo->sdTotalBytes = tot;
  outInfo->sdUsedBytes  = SD.usedBytes();
  outInfo->sdFreeBytes  = (outInfo->sdTotalBytes >= outInfo->sdUsedBytes)
                              ? (outInfo->sdTotalBytes - outInfo->sdUsedBytes)
                              : 0;
  return true;
}

void lakatosGetStorageInfo(LakatosStorageInfo* outInfo, bool forceProbeSd) {
  if (!outInfo) return;
  memset(outInfo, 0, sizeof(*outInfo));
  outInfo->sdCardType = "None";
  lakatosGetFlashStorageInfo(outInfo);
  if (lakatosProbeSdMounted(outInfo, forceProbeSd)) {
    lakatosGetSdStorageSpace(outInfo);
  }
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
    if (!LittleFS.exists("/apps")) {
      LittleFS.mkdir("/apps");
    }
    seedLoAppFile("/apps/life.lapp", kSeedApp_life, sizeof(kSeedApp_life));
    seedLoAppFile("/apps/gallery.lapp", kSeedApp_gallery, sizeof(kSeedApp_gallery));
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
  String user = String(lakatosGetDeviceName()) + "/" + String(lakatosGetSerialNumber());
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

// Reads one '\n'-terminated line from stream (stripping trailing '\r').
// Returns false on EOF/timeout before any byte or if line exceeds maxLen-1.
static bool readShStreamLine(WiFiClient* stream, HTTPClient& http,
                             char* lineBuf, size_t maxLen, uint32_t timeoutMs = 6000) {
  if (!stream || !lineBuf || maxLen < 2) return false;
  size_t pos = 0;
  uint32_t startMs = millis();
  while (!lakatosShouldExitApp() && (millis() - startMs < timeoutMs)) {
    int avail = stream->available();
    if (avail > 0) {
      int ch = stream->read();
      if (ch < 0) continue;
      startMs = millis();
      if (ch == '\n') {
        while (pos > 0 && lineBuf[pos - 1] == '\r') pos--;
        lineBuf[pos] = '\0';
        return true;
      }
      if (pos + 1 >= maxLen) {
        return false; // Line too long
      }
      lineBuf[pos++] = (char)ch;
    } else if (!http.connected() && stream->available() <= 0) {
      break;
    } else {
      delayMicroseconds(200);
    }
  }
  if (pos > 0) {
    while (pos > 0 && lineBuf[pos - 1] == '\r') pos--;
    lineBuf[pos] = '\0';
    return true;
  }
  return false;
}

// Streams and parses a Superhighway LS response directly from the TCP socket
// without buffering the entire response body in heap:
//   Line 1: <expected_count> (decimal integer >= 0)
//   Next <expected_count> lines: "<YYYY-mm-dd> <HH:MM:SS> <size> <name>[/]"
//   Final line: ";"
static int parseShLsStream(WiFiClient* stream, HTTPClient& http,
                           LakatosFsEntry* outEntries, int maxEntries,
                           int* outTotalCount = nullptr,
                           LakatosFsEntryCallback cb = nullptr,
                           void* cbUser = nullptr) {
  if (outTotalCount) *outTotalCount = 0;
  char lineBuf[160];
  if (!readShStreamLine(stream, http, lineBuf, sizeof(lineBuf))) {
    Serial.println("[Lakatos VFS] LS stream: empty response");
    return -1;
  }
  size_t cLen = strlen(lineBuf);
  while (cLen > 0 && (lineBuf[cLen - 1] == ' ' || lineBuf[cLen - 1] == '\t')) {
    lineBuf[--cLen] = '\0';
  }
  if (cLen == 0) return -1;
  for (size_t i = 0; i < cLen; i++) {
    if (lineBuf[i] < '0' || lineBuf[i] > '9') {
      Serial.printf("[Lakatos VFS] LS stream: bad count line '%s'\r\n", lineBuf);
      return -1;
    }
  }
  int expectedCount = atoi(lineBuf);
  if (expectedCount < 0) return -1;

  int seenCount = 0;
  int storedCount = 0;
  bool sawTerminator = false;

  while (readShStreamLine(stream, http, lineBuf, sizeof(lineBuf))) {
    if (strcmp(lineBuf, ";") == 0) {
      sawTerminator = true;
      break;
    }
    if (seenCount >= expectedCount) {
      Serial.printf("[Lakatos VFS] LS stream: extra line after %d entries: '%s'\r\n",
                    expectedCount, lineBuf);
      return -1;
    }
    LakatosFsEntry tmp;
    if (!parseShLsLine(String(lineBuf), &tmp)) {
      Serial.printf("[Lakatos VFS] LS stream: bad entry line %d: '%s'\r\n",
                    seenCount, lineBuf);
      return -1;
    }
    if (outEntries && storedCount < maxEntries) {
      outEntries[storedCount++] = tmp;
    } else if (!outEntries && cb) {
      cb(tmp, cbUser);
      storedCount++;
    }
    seenCount++;
  }

  if (!sawTerminator || seenCount != expectedCount) {
    Serial.printf("[Lakatos VFS] LS stream: framing mismatch (sawTerm=%d, seen=%d, expected=%d)\r\n",
                   sawTerminator ? 1 : 0, seenCount, expectedCount);
    return -1;
  }
  if (outTotalCount) *outTotalCount = expectedCount;
  return storedCount;
}

// ---------------------------------------------------------------------------
// Unified VFS Implementation
// ---------------------------------------------------------------------------
static int lsFsImpl(fs::FS& fs, const String& subPath,
                    LakatosFsEntry* outEntries, int maxEntries,
                    LakatosFsEntryCallback cb = nullptr,
                    void* cbUser = nullptr) {
  File dir = fs.open(subPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return -1;
  }
  int count = 0;
  File f = dir.openNextFile();
  while (f && (!outEntries || count < maxEntries)) {
    LakatosFsEntry e;
    memset(&e, 0, sizeof(e));
    const char* nm = f.name();
    const char* slash = strrchr(nm, '/');
    if (slash) nm = slash + 1;
    strncpy(e.name, nm, sizeof(e.name) - 1);
    e.size = f.size();
    e.mtime = f.getLastWrite();
    e.isDir = f.isDirectory();
    if (e.name[0] != '\0') {
      if (outEntries) {
        outEntries[count] = e;
      } else if (cb) {
        cb(e, cbUser);
      }
      count++;
    }
    f.close();
    f = dir.openNextFile();
  }
  if (f) f.close();
  dir.close();
  return count;
}

static int lakatosFsLsCommon(const char* path, LakatosFsEntry* outEntries, int maxEntries,
                             LakatosFsEntryCallback cb, void* cbUser) {
  gLastVfsErr = "EIO";
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  int count = -1;
  if (b == VFS_LO) {
    if (gLoMounted) count = lsFsImpl(LittleFS, subPath, outEntries, maxEntries, cb, cbUser);
  } else if (b == VFS_SD) {
    if (ensureSdMounted()) {
      count = lsFsImpl(SD, subPath, outEntries, maxEntries, cb, cbUser);
      if (count < 0 && subPath == "/") {
        SD.end();
        gSdMounted = false;
        gLastSdRetryMs = 0;
        if (ensureSdMounted()) {
          count = lsFsImpl(SD, subPath, outEntries, maxEntries, cb, cbUser);
        }
      }
    }
  } else if (b == VFS_SH) {
    if (WiFi.status() != WL_CONNECTED) return -1;
    WiFiClient cli;
    HTTPClient http;
    cli.setNoDelay(true);
    http.useHTTP10(true);
    String url = buildShTargetUrl(subPath);
    if (!http.begin(cli, url)) {
      return -1;
    }
    addShBasicAuthHeader(http);
    int code = http.sendRequest("LS");
    if (code < 200 || code >= 300) {
      setShHttpError("LS", subPath, code);
      http.end();
      return -1;
    }
    count = parseShLsStream(http.getStreamPtr(), http, outEntries, maxEntries, nullptr, cb, cbUser);
    http.end();

    if (count < 0) {
      Serial.printf("[Lakatos VFS] LS %s failed framing/count check\r\n", subPath.c_str());
    }
  }
  lakatosDevLogf("VFS ls %s -> %d entries", path ? path : "", count);
  return count;
}

int lakatosFsLs(const char* path, LakatosFsEntry* outEntries, int maxEntries) {
  if (!outEntries || maxEntries <= 0) return -1;
  return lakatosFsLsCommon(path, outEntries, maxEntries, nullptr, nullptr);
}

int lakatosFsLsEach(const char* path, LakatosFsEntryCallback cb, void* userData) {
  if (!cb) return -1;
  return lakatosFsLsCommon(path, nullptr, 0, cb, userData);
}

bool lakatosFsStat(const char* path, LakatosFsEntry* outEntry) {
  gLastVfsErr = "EIO";
  if (!outEntry) return false;
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  if (b == VFS_LO || b == VFS_SD) {
    fs::FS* fsPtr = (b == VFS_LO) ? (fs::FS*)&LittleFS : (fs::FS*)&SD;
    if (b == VFS_LO && !gLoMounted) return false;
    if (b == VFS_SD && !ensureSdMounted()) return false;
    File f = fsPtr->open(subPath.c_str(), "r");
    if (!f) {
      gLastVfsErr = "ENOENT";
      return false;
    }
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
    http.useHTTP10(true);
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("LS");
    if (code < 200 || code >= 300) {
      http.end();
      return false;
    }
    const char* slash = strrchr(subPath.c_str(), '/');
    const char* baseName = slash ? (slash + 1) : subPath.c_str();
    int totalCount = 0;
    int parsed = parseShLsStream(http.getStreamPtr(), http, outEntry, 1, &totalCount);
    http.end();
    if (parsed >= 0) {
      if (totalCount == 1 && parsed == 1 && !outEntry->isDir &&
          strcmp(outEntry->name, baseName) == 0) {
        return true;
      }
      memset(outEntry, 0, sizeof(*outEntry));
      strncpy(outEntry->name, baseName[0] ? baseName : "/", sizeof(outEntry->name) - 1);
      outEntry->size = 4096;
      outEntry->isDir = true;
      return true;
    }
    return false;
  }
  return false;
}

int32_t lakatosFsRead(const char* path, uint32_t offset, void* buf, size_t len) {
  gLastVfsErr = "EIO";
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
    while (!lakatosShouldExitApp() &&
           (http.connected() || (stream && stream->available() > 0)) &&
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
  gLastVfsErr = "EIO";
  String subPath;
  VfsBackend b = parseVfsPath(path, subPath);
  bool ok = false;
  int httpCode = 0;
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
    http.useHTTP10(true);
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    httpCode = http.sendRequest("PUT", (uint8_t*)buf, len);
    http.end();
    ok = (httpCode >= 200 && httpCode < 300);
    if (!ok) {
      setShHttpError("PUT", subPath, httpCode);
    }
  }
  if (b == VFS_SH) {
    lakatosDevLogf("VFS write %s (%u B) -> %s (HTTP %d)",
                   path ? path : "", (unsigned)len, ok ? "OK" : "FAIL", httpCode);
  } else {
    lakatosDevLogf("VFS write %s (%u B) -> %s",
                   path ? path : "", (unsigned)len, ok ? "OK" : "FAIL");
  }
  return ok;
}

bool lakatosFsWriteAt(const char* path, uint32_t offset, const void* buf, size_t len) {
  gLastVfsErr = "EIO";
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
  gLastVfsErr = "EIO";
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
    http.useHTTP10(true);
    String url = buildShTargetUrl(subPath, parents ? "p=1" : nullptr);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("MKDIR");
    http.end();
    if (code >= 200 && code < 300) return true;
    setShHttpError("MKDIR", subPath, code);
    return false;
  }
  return false;
}

bool lakatosFsRmdir(const char* path) {
  gLastVfsErr = "EIO";
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
    http.useHTTP10(true);
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("RMDIR");
    http.end();
    if (code >= 200 && code < 300) return true;
    setShHttpError("RMDIR", subPath, code);
    return false;
  }
  return false;
}

bool lakatosFsRm(const char* path) {
  gLastVfsErr = "EIO";
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
    http.useHTTP10(true);
    String url = buildShTargetUrl(subPath);
    if (!http.begin(client, url)) return false;
    addShBasicAuthHeader(http);
    int code = http.sendRequest("RM");
    http.end();
    if (code >= 200 && code < 300) return true;
    setShHttpError("RM", subPath, code);
    return false;
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
  while (!lakatosShouldExitApp() &&
         (http.connected() || (stream && stream->available() > 0)) &&
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
           !(!onCore0 && lakatosShouldExitApp()) &&
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
        if (now - lastYieldMs >= 5) {
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
        if (s->contentLen > 0 && total < (size_t)s->contentLen && total < gPrefetchMaxLen) {
          err = true;
          if (s->backend == VFS_SH && s->httpPtr) {
            s->httpPtr->setReuse(false);
          }
        }
        break; // EOF or timeout
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

// ---------------------------------------------------------------------------
// Live HTTP / Raw TCP / WebSocket Stream (Non-Blocking Reader/Writer)
// ---------------------------------------------------------------------------
static WiFiClient gLiveStreamClient;
static HTTPClient gLiveStreamHttp;
static bool       gLiveStreamOpen  = false;
static bool       gLiveStreamIsRaw = false;
static bool       gWsOpen          = false;

void lakatosWsClose() {
  if (gWsOpen) {
    // Send a masked RFC 6455 Close frame (FIN=1, opcode=8, mask=1, len=0, key=0)
    if (gLiveStreamClient.connected()) {
      const uint8_t closeFrame[6] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
      gLiveStreamClient.write(closeFrame, sizeof(closeFrame));
    }
    gLiveStreamClient.stop();
    gWsOpen          = false;
    gLiveStreamOpen  = false;
    gLiveStreamIsRaw = false;
  }
}

void lakatosHttpStreamClose() {
  lakatosWsClose();
  if (gLiveStreamOpen) {
    if (!gLiveStreamIsRaw) {
      gLiveStreamHttp.end();
    }
    gLiveStreamClient.stop();
    gLiveStreamOpen  = false;
    gLiveStreamIsRaw = false;
  }
}

bool lakatosHttpStreamOpen(const char* url) {
  lakatosHttpStreamClose();
  if (!url || !url[0] || WiFi.status() != WL_CONNECTED) return false;

  if (strncmp(url, "tcp://", 6) == 0) {
    const char* hp = url + 6;
    char host[96];
    uint16_t port = 80;
    size_t i = 0;
    while (hp[i] && hp[i] != ':' && hp[i] != '/' && i < sizeof(host) - 1) {
      host[i] = hp[i];
      i++;
    }
    host[i] = '\0';
    if (hp[i] == ':') {
      int p = atoi(hp + i + 1);
      if (p > 0 && p <= 65535) port = (uint16_t)p;
    }
    if (!host[0]) return false;
    if (!gLiveStreamClient.connect(host, port, 5000)) {
      gLiveStreamClient.stop();
      return false;
    }
    gLiveStreamClient.setNoDelay(true);
    gLiveStreamOpen  = true;
    gLiveStreamIsRaw = true;
    Serial.printf("[Lakatos TCP] Stream opened: %s:%u (heap=%u)\r\n",
                  host, (unsigned)port, (unsigned)ESP.getFreeHeap());
    return true;
  }

  gLiveStreamClient.setNoDelay(true);
  gLiveStreamHttp.useHTTP10(true);
  gLiveStreamHttp.setTimeout(8000);
  gLiveStreamHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  gLiveStreamHttp.setUserAgent("Lakatos-CYD/1.0");
  if (!gLiveStreamHttp.begin(gLiveStreamClient, url)) {
    return false;
  }
  int code = gLiveStreamHttp.GET();
  if (code < 200 || code >= 300) {
    gLiveStreamHttp.end();
    gLiveStreamClient.stop();
    return false;
  }
  gLiveStreamOpen  = true;
  gLiveStreamIsRaw = false;
  Serial.printf("[Lakatos HTTP] Stream opened: %s (HTTP %d, heap=%u)\r\n",
                url, code, (unsigned)ESP.getFreeHeap());
  return true;
}

int32_t lakatosHttpStreamRead(void* buf, size_t maxLen) {
  if (!gLiveStreamOpen || !buf || maxLen == 0) return -1;
  if (lakatosShouldExitApp()) {
    lakatosHttpStreamClose();
    return -1;
  }
  WiFiClient* stream = gLiveStreamIsRaw ? &gLiveStreamClient : gLiveStreamHttp.getStreamPtr();
  if (!stream) {
    lakatosHttpStreamClose();
    return -1;
  }
  size_t avail = stream->available();
  if (avail > 0) {
    size_t toRead = (avail > maxLen) ? maxLen : avail;
    int r = stream->read((uint8_t*)buf, toRead);
    return (r > 0) ? (int32_t)r : 0;
  }
  bool conn = gLiveStreamIsRaw ? gLiveStreamClient.connected()
                               : (gLiveStreamHttp.connected() || stream->connected());
  if (!conn) {
    lakatosHttpStreamClose();
    return -1;
  }
  return 0;
}

int32_t lakatosHttpStreamWrite(const void* buf, size_t len) {
  if (!gLiveStreamOpen || !buf || len == 0) return -1;
  if (lakatosShouldExitApp()) {
    lakatosHttpStreamClose();
    return -1;
  }
  WiFiClient* stream = gLiveStreamIsRaw ? &gLiveStreamClient : gLiveStreamHttp.getStreamPtr();
  if (!stream || !stream->connected()) {
    lakatosHttpStreamClose();
    return -1;
  }
  size_t w = stream->write((const uint8_t*)buf, len);
  return (w > 0) ? (int32_t)w : -1;
}

static bool wsReadExact(WiFiClient& cli, uint8_t* dst, size_t n, uint32_t timeoutMs = 600) {
  size_t got = 0;
  uint32_t startMs = millis();
  while (got < n && (millis() - startMs < timeoutMs)) {
    int avail = cli.available();
    if (avail > 0) {
      size_t take = ((size_t)avail < (n - got)) ? (size_t)avail : (n - got);
      int r = cli.read(dst + got, take);
      if (r > 0) {
        got += (size_t)r;
        startMs = millis();
      }
    } else if (!cli.connected()) {
      break;
    } else {
      delayMicroseconds(200);
    }
  }
  return got == n;
}

bool lakatosWsOpen(const char* urlOrPath) {
  lakatosHttpStreamClose();
  if (!urlOrPath || !urlOrPath[0] || WiFi.status() != WL_CONNECTED) return false;

  String fullUrl = String(urlOrPath);
  fullUrl.trim();
  if (fullUrl.startsWith("/")) {
    String base = String(lakatosGetSuperhighwayUrl());
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    if (base.length() == 0) return false;
    fullUrl = base + fullUrl;
  }

  // Strip scheme (ws://, http://, wss://, https://)
  if (fullUrl.startsWith("ws://")) {
    fullUrl = fullUrl.substring(5);
  } else if (fullUrl.startsWith("http://")) {
    fullUrl = fullUrl.substring(7);
  } else if (fullUrl.startsWith("wss://")) {
    fullUrl = fullUrl.substring(6);
  } else if (fullUrl.startsWith("https://")) {
    fullUrl = fullUrl.substring(8);
  }

  int slashIdx = fullUrl.indexOf('/');
  String hostPort = (slashIdx >= 0) ? fullUrl.substring(0, slashIdx) : fullUrl;
  String reqPath  = (slashIdx >= 0) ? fullUrl.substring(slashIdx) : String("/");

  // Append ?id=<deviceName> if not already present so the server knows our CYD name
  const char* devName = lakatosGetDeviceName();
  if (devName && devName[0] && reqPath.indexOf("id=") < 0) {
    reqPath += (reqPath.indexOf('?') >= 0) ? "&id=" : "?id=";
    reqPath += urlEncodePath(String(devName)).substring(1);
  }

  String host = hostPort;
  uint16_t port = 80;
  int colonIdx = hostPort.indexOf(':');
  if (colonIdx >= 0) {
    host = hostPort.substring(0, colonIdx);
    int p = hostPort.substring(colonIdx + 1).toInt();
    if (p > 0 && p <= 65535) port = (uint16_t)p;
  }
  if (host.length() == 0) return false;

  if (!gLiveStreamClient.connect(host.c_str(), port, 4000)) {
    gLiveStreamClient.stop();
    return false;
  }
  gLiveStreamClient.setNoDelay(true);

  String req = "GET " + reqPath + " HTTP/1.1\r\n";
  req += "Host: " + hostPort + "\r\n";
  req += "Upgrade: websocket\r\n";
  req += "Connection: Upgrade\r\n";
  req += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
  req += "Sec-WebSocket-Version: 13\r\n";

  String user = String(lakatosGetDeviceName()) + "/" + String(lakatosGetSerialNumber());
  String pass = String(lakatosGetSuperhighwayPassword());
  if (user.length() > 0 || pass.length() > 0) {
    String b64 = base64::encode(user + ":" + pass);
    req += "Authorization: Basic " + b64 + "\r\n";
  }
  req += "\r\n";

  gLiveStreamClient.print(req);

  // Read HTTP response headers until blank line
  char lineBuf[160];
  bool saw101 = false;
  bool firstLine = true;
  uint32_t t0 = millis();
  while (millis() - t0 < 4000) {
    size_t pos = 0;
    bool gotLine = false;
    while (millis() - t0 < 4000) {
      if (gLiveStreamClient.available() > 0) {
        int ch = gLiveStreamClient.read();
        if (ch == '\n') {
          while (pos > 0 && lineBuf[pos - 1] == '\r') pos--;
          lineBuf[pos] = '\0';
          gotLine = true;
          break;
        }
        if (pos + 1 < sizeof(lineBuf)) {
          lineBuf[pos++] = (char)ch;
        }
      } else if (!gLiveStreamClient.connected()) {
        break;
      } else {
        delayMicroseconds(200);
      }
    }
    if (!gotLine) break;
    if (firstLine) {
      firstLine = false;
      if (strstr(lineBuf, " 101") != nullptr) {
        saw101 = true;
      }
    } else if (lineBuf[0] == '\0') {
      break; // End of HTTP headers
    }
  }

  if (!saw101) {
    gLiveStreamClient.stop();
    return false;
  }

  gWsOpen          = true;
  gLiveStreamOpen  = true;
  gLiveStreamIsRaw = true;
  Serial.printf("[Lakatos WS] Connected: ws://%s%s\r\n", hostPort.c_str(), reqPath.c_str());
  return true;
}

int32_t lakatosWsSend(const void* buf, size_t len) {
  if (!gWsOpen || !buf || len > 4096) return -1;
  if (lakatosShouldExitApp() || !gLiveStreamClient.connected()) {
    lakatosWsClose();
    return -1;
  }

  // RFC 6455 client-to-server masked text frame (opcode 0x1, FIN 0x80, MASK 0x80).
  // Using mask key {0, 0, 0, 0} keeps payload bytes identical while satisfying RFC 6455.
  uint8_t hdr[8];
  size_t hdrLen = 0;
  hdr[0] = 0x81; // FIN | Text
  if (len < 126) {
    hdr[1] = (uint8_t)(0x80u | len);
    hdr[2] = 0; hdr[3] = 0; hdr[4] = 0; hdr[5] = 0;
    hdrLen = 6;
  } else {
    hdr[1] = (uint8_t)(0x80u | 126u);
    hdr[2] = (uint8_t)((len >> 8) & 0xFFu);
    hdr[3] = (uint8_t)(len & 0xFFu);
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;
    hdrLen = 8;
  }

  if (gLiveStreamClient.write(hdr, hdrLen) != hdrLen) {
    lakatosWsClose();
    return -1;
  }
  if (len > 0 && gLiveStreamClient.write((const uint8_t*)buf, len) != len) {
    lakatosWsClose();
    return -1;
  }
  return (int32_t)len;
}

int32_t lakatosWsRecv(void* buf, size_t maxLen) {
  if (!gWsOpen || !buf || maxLen == 0) return -1;
  if (lakatosShouldExitApp()) {
    lakatosWsClose();
    return -1;
  }
  int avail = gLiveStreamClient.available();
  if (avail < 2) {
    if (!gLiveStreamClient.connected()) {
      lakatosWsClose();
      return -1;
    }
    return 0;
  }

  uint8_t hdr[2];
  if (!wsReadExact(gLiveStreamClient, hdr, 2)) {
    lakatosWsClose();
    return -1;
  }
  uint8_t opcode     = hdr[0] & 0x0Fu;
  bool    masked     = (hdr[1] & 0x80u) != 0;
  size_t  payloadLen = (size_t)(hdr[1] & 0x7Fu);

  if (payloadLen == 126) {
    uint8_t ext[2];
    if (!wsReadExact(gLiveStreamClient, ext, 2)) {
      lakatosWsClose();
      return -1;
    }
    payloadLen = ((size_t)ext[0] << 8) | (size_t)ext[1];
  } else if (payloadLen == 127) {
    uint8_t ext[8];
    if (!wsReadExact(gLiveStreamClient, ext, 8)) {
      lakatosWsClose();
      return -1;
    }
    payloadLen = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16) |
                 ((size_t)ext[6] << 8) | (size_t)ext[7];
  }

  uint8_t maskKey[4] = {0, 0, 0, 0};
  if (masked) {
    if (!wsReadExact(gLiveStreamClient, maskKey, 4)) {
      lakatosWsClose();
      return -1;
    }
  }

  uint8_t* dst = (uint8_t*)buf;
  size_t copyCap = (maxLen > 1) ? (maxLen - 1) : 0;
  size_t copied  = (payloadLen < copyCap) ? payloadLen : copyCap;
  if (copied > 0) {
    if (!wsReadExact(gLiveStreamClient, dst, copied)) {
      lakatosWsClose();
      return -1;
    }
    if (masked) {
      for (size_t i = 0; i < copied; i++) {
        dst[i] ^= maskKey[i & 3];
      }
    }
  }
  dst[copied] = '\0';

  // Discard any bytes exceeding maxLen - 1
  size_t rem = payloadLen - copied;
  uint8_t discard[64];
  while (rem > 0) {
    size_t chunk = (rem > sizeof(discard)) ? sizeof(discard) : rem;
    if (!wsReadExact(gLiveStreamClient, discard, chunk)) {
      lakatosWsClose();
      return -1;
    }
    rem -= chunk;
  }

  if (opcode == 0x8) { // Close
    lakatosWsClose();
    return -1;
  }
  if (opcode == 0x9) { // Ping -> reply Pong
    const uint8_t pongFrame[6] = {0x8A, 0x80, 0x00, 0x00, 0x00, 0x00};
    gLiveStreamClient.write(pongFrame, sizeof(pongFrame));
    return 0;
  }
  if (opcode == 0x1 || opcode == 0x2) {
    return (int32_t)copied;
  }
  return 0;
}
