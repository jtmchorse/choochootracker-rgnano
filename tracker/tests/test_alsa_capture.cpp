// RG Nano USB audio in: /proc/asound parsing, name-based selection and the
// hot-plug refresh policy. Fixture text is taken from the round 4.9 usbdiag
// report (/proc/asound/cards with the TANCHJIM DAC and the Sonos adapter).
#include "doctest.h"
#include "alsa_capture.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <cstring>

namespace fs = std::filesystem;

namespace {
// Two cards, one capture PCM: card 1 is the USB DAC (playback only), card 2
// is the Sonos line-in adapter (capture).
const char* kCardsTwo =
  " 1 [DSP            ]: USB-Audio - TANCHJIM-ONE DSP\n"
  "                      KTMicro TANCHJIM-ONE DSP at usb-1c1a000.usb-1.4, full speed\n"
  " 2 [Adapter        ]: USB-Audio - Sonos USB-C Line-in Adapter\n"
  "                      Sonos Electronics Inc. Sonos USB-C Line-in Adapter at usb-1c1a000.usb-1.2, full\n";
// Same two devices after a re-plug: the order flips.
const char* kCardsSwapped =
  " 1 [Adapter        ]: USB-Audio - Sonos USB-C Line-in Adapter\n"
  "                      Sonos Electronics Inc. Sonos USB-C Line-in Adapter at usb-1c1a000.usb-1.2, full\n"
  " 2 [DSP            ]: USB-Audio - TANCHJIM-ONE DSP\n"
  "                      KTMicro TANCHJIM-ONE DSP at usb-1c1a000.usb-1.4, full speed\n";

void writeFile(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << text;
}

fs::path makeTree(const char* tag, const char* cards, int dacCard, int captureCard) {
  fs::path root = fs::path("build") / "asound-fixture" / tag;
  fs::remove_all(root);
  writeFile(root / "cards", cards);
  fs::create_directories(root / ("card" + std::to_string(dacCard)) / "pcm0p");
  fs::create_directories(root / ("card" + std::to_string(captureCard)) / "pcm0c");
  return root;
}

int closes = 0, quits = 0, scanCalls = 0;
fs::path scanRoot;
int scanFixture(AlsaCaptureCard* out, int maxCards) {
  scanCalls++;
  return alsaScanCaptureCards(scanRoot.string().c_str(), out, maxCards);
}
void countClose(void) { closes++; }
void countQuit(void) { quits++; }
const AlsaCaptureOps mockOps = {scanFixture, countClose, countQuit};
}  // namespace

TEST_CASE("alsa capture: parses cards text (long names, continuation lines skipped)") {
  AlsaCaptureCard cards[8];
  int n = alsaParseCards(kCardsTwo, cards, 8);
  REQUIRE(n == 2);
  CHECK(cards[0].card == 1);
  CHECK(std::string(cards[0].name) == "TANCHJIM-ONE DSP");
  CHECK(cards[1].card == 2);
  CHECK(std::string(cards[1].name) == "Sonos USB-C Line-in Adapter");
}

TEST_CASE("alsa capture: scan lists only cards with a capture PCM, as 'card N: name'") {
  scanRoot = makeTree("two", kCardsTwo, 1, 2);
  AlsaCaptureCard cards[8];
  int n = alsaScanCaptureCards(scanRoot.string().c_str(), cards, 8);
  REQUIRE(n == 1);
  CHECK(cards[0].card == 2);
  CHECK(cards[0].pcm == 0);
  char label[96], dev[32];
  alsaCaptureLabel(cards[0], label, sizeof(label));
  CHECK(std::string(label) == "card 2: Sonos USB-C Line-in Adapter");
  alsaCaptureDeviceName(cards[0], dev, sizeof(dev));
  CHECK(std::string(dev) == "plughw:2,0");
  CHECK(alsaScanCaptureCards("build/asound-fixture/does-not-exist", cards, 8) == 0);
}

TEST_CASE("alsa capture: saved name survives a card index change") {
  AlsaCaptureCard cards[8];
  fs::path a = makeTree("before", kCardsTwo, 1, 2);
  int n = alsaScanCaptureCards(a.string().c_str(), cards, 8);
  REQUIRE(n == 1);
  char label[96];
  alsaCaptureLabel(cards[0], label, sizeof(label));
  // What settings.txt holds: the key, with no card index in it.
  std::string saved = alsaCaptureKey(label);
  CHECK(saved == "Sonos USB-C Line-in Adapter");
  CHECK(saved.find("card") == std::string::npos);

  fs::path b = makeTree("after", kCardsSwapped, 2, 1);  // DAC now card 2, Sonos card 1
  n = alsaScanCaptureCards(b.string().c_str(), cards, 8);
  REQUIRE(n == 1);
  CHECK(cards[0].card == 1);
  int found = alsaCaptureFindByName(cards, n, saved.c_str());
  REQUIRE(found == 0);
  char dev[32];
  alsaCaptureDeviceName(cards[found], dev, sizeof(dev));
  CHECK(std::string(dev) == "plughw:1,0");
  // The label form (with the old index) resolves to the same device.
  CHECK(alsaCaptureFindByName(cards, n, label) == 0);
  CHECK(alsaCaptureFindByName(cards, n, "Some Other Interface") == -1);
}

TEST_CASE("alsa capture: refresh never quits the SDL audio subsystem; loss closes only capture") {
  closes = quits = scanCalls = 0;
  scanRoot = makeTree("hot", kCardsTwo, 1, 2);
#ifdef RGNANO_BUILD
  const bool allowRestart = false;  // what corelib_audio.cpp passes on the Nano
#else
  const bool allowRestart = true;
#endif
  alsaCaptureRescan(mockOps);
  alsaCaptureSetOpenName("card 2: Sonos USB-C Line-in Adapter");

  // Device still there: nothing closed, nothing lost.
  alsaCaptureRefresh(mockOps, allowRestart);
  CHECK(closes == 0);
  CHECK(alsaCaptureTakeLost() == false);

  // Unplug the interface: only the capture device is closed, lost is raised once.
  fs::remove_all(scanRoot / "card2");
  alsaCaptureRefresh(mockOps, allowRestart);
  CHECK(closes == 1);
  CHECK(alsaCaptureTakeLost() == true);
  CHECK(alsaCaptureTakeLost() == false);

  // Further refreshes with nothing open are quiet.
  alsaCaptureRefresh(mockOps, allowRestart);
  CHECK(closes == 1);
#ifdef RGNANO_BUILD
  CHECK(quits == 0);  // SDL_QuitSubSystem(SDL_INIT_AUDIO) must never run on the Nano
#endif
  alsaCaptureSetOpenName(NULL);
}
