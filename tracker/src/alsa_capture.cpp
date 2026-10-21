#include "alsa_capture.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int alsaParseCards(const char* text, AlsaCaptureCard* out, int maxCards) {
  int count = 0;
  const char* p = text;
  while (p && *p && count < maxCards) {
    const char* eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char line[200];
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = 0;
    p = eol ? eol + 1 : NULL;
    // " 2 [Adapter        ]: USB-Audio - Sonos USB-C Line-in Adapter"
    int card = -1;
    const char* q = line;
    while (*q == ' ') q++;
    if (*q < '0' || *q > '9') continue;  // continuation line
    card = atoi(q);
    const char* colon = strstr(q, "]:");
    if (!colon) continue;
    const char* name = strstr(colon, " - ");
    name = name ? name + 3 : colon + 2;
    while (*name == ' ') name++;
    AlsaCaptureCard& c = out[count++];
    c.card = card;
    c.pcm = 0;
    snprintf(c.name, sizeof(c.name), "%s", name);
    for (size_t n = strlen(c.name); n > 0 && (c.name[n - 1] == ' ' || c.name[n - 1] == '\r'); n--) c.name[n - 1] = 0;
  }
  return count;
}

static bool firstCapturePcm(const char* root, int card, int* pcm) {
  char dir[300];
  snprintf(dir, sizeof(dir), "%s/card%d", root, card);
  DIR* d = opendir(dir);
  if (!d) return false;
  int best = -1;
  while (struct dirent* e = readdir(d)) {
    int n;
    char tail;
    if (sscanf(e->d_name, "pcm%d%c", &n, &tail) == 2 && tail == 'c' &&
        e->d_name[strlen(e->d_name) - 1] == 'c') {
      if (best < 0 || n < best) best = n;
    }
  }
  closedir(d);
  if (best < 0) return false;
  *pcm = best;
  return true;
}

int alsaScanCaptureCards(const char* root, AlsaCaptureCard* out, int maxCards) {
  char path[300];
  snprintf(path, sizeof(path), "%s/cards", root);
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  char text[2048];
  size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = 0;
  AlsaCaptureCard all[ALSA_CAPTURE_MAX_CARDS];
  int total = alsaParseCards(text, all, ALSA_CAPTURE_MAX_CARDS);
  int count = 0;
  for (int i = 0; i < total && count < maxCards; i++) {
    int pcm = 0;
    if (!firstCapturePcm(root, all[i].card, &pcm)) continue;
    out[count] = all[i];
    out[count].pcm = pcm;
    count++;
  }
  return count;
}

void alsaCaptureLabel(const AlsaCaptureCard& c, char* buffer, int bufferSize) {
  snprintf(buffer, (size_t)bufferSize, "card %d: %s", c.card, c.name);
}

const char* alsaCaptureKey(const char* s) {
  int card, used = 0;
  if (sscanf(s, "card %d: %n", &card, &used) >= 1 && used > 0) return s + used;
  return s;
}

int alsaCaptureFindByName(const AlsaCaptureCard* cards, int count, const char* labelOrKey) {
  const char* key = alsaCaptureKey(labelOrKey);
  if (!key[0]) return -1;
  for (int i = 0; i < count; i++)
    if (strcmp(cards[i].name, key) == 0) return i;
  return -1;
}

void alsaCaptureDeviceName(const AlsaCaptureCard& c, char* buffer, int bufferSize) {
  snprintf(buffer, (size_t)bufferSize, "plughw:%d,%d", c.card, c.pcm);
}

static AlsaCaptureCard g_cards[ALSA_CAPTURE_MAX_CARDS];
static int g_count = 0;
static char g_openName[ALSA_CAPTURE_NAME_LENGTH + 16];
static bool g_lost = false;

int alsaCaptureCount(void) { return g_count; }
const AlsaCaptureCard* alsaCaptureCard(int index) {
  return index >= 0 && index < g_count ? &g_cards[index] : NULL;
}

int alsaCaptureRescan(const AlsaCaptureOps& ops) {
  g_count = ops.scan ? ops.scan(g_cards, ALSA_CAPTURE_MAX_CARDS) : 0;
  return g_count;
}

void alsaCaptureSetOpenName(const char* labelOrKey) {
  snprintf(g_openName, sizeof(g_openName), "%s", labelOrKey ? alsaCaptureKey(labelOrKey) : "");
}

int alsaCaptureRefresh(const AlsaCaptureOps& ops, bool allowSubsystemRestart) {
  if (allowSubsystemRestart && ops.quitAudioSubsystem) ops.quitAudioSubsystem();
  alsaCaptureRescan(ops);
  if (g_openName[0] && alsaCaptureFindByName(g_cards, g_count, g_openName) < 0) {
    if (ops.closeCapture) ops.closeCapture();
    g_openName[0] = 0;
    g_lost = true;
  }
  return 0;
}

bool alsaCaptureTakeLost(void) {
  bool v = g_lost;
  g_lost = false;
  return v;
}
