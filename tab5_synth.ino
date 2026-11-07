// ============================================================
//  tab5_synth - M5Tab5 + Unit Synth シンセサイザー
// ------------------------------------------------------------
//  M5Stack Tab5 に Unit Synth (U178 / SAM2695) をつないだ、
//  4小節ループ型のタッチシンセサイザー。
//
//  モード:
//    RHYTHM : ドラムパッド (10種) の重ね録り + ステップグリッド編集
//    MELODY : 61鍵タッチキーボード (マルチタッチ/グリッサンド)
//             + GM128音色選択 + ピアノロール式グリッド編集
//    EDIT   : テンポ/音量/音長などの仕上げ調整と全体再生
//  共通: 生tick録音 + 非破壊クォンタイズ、SDへ標準MIDIファイル出力
//
//  接続: Tab5 の HY2.0-4P (PORT.A) へ Grove ケーブルで Unit Synth。
//        Unit Synth は受信専用 (黄線 = UART_RX) なので TX = G53。
//
//  必要ライブラリ: M5Unified / M5GFX / M5Unit-Synth
//  ボード: M5Stack ボードパッケージの M5Tab5
//  ライセンス: MIT (同梱の LICENSE を参照)
// ============================================================

#include <M5Unified.h>
#include <M5UnitSynth.h>
#include <SD.h>
#include <SD_MMC.h>
#include <SPI.h>
#include <FS.h>
#include <vector>
#include <algorithm>

M5UnitSynth synth;

static const int SYNTH_RX_PIN = 54;  // 白 (未使用)
static const int SYNTH_TX_PIN = 53;  // 黄 → Unit Synth UART_RX (Phase 0 で確認済み)

static const uint8_t MELODY_CH = 0;
static const uint8_t DRUM_CH   = 9;

static const uint32_t PPQ = 480;                 // 4分音符あたりの tick 数
static int g_bpm = 120;
static const int LOOP_BARS = 4;
static const uint32_t LOOP_TICKS = LOOP_BARS * 4 * PPQ;  // 7680

// ---------------- 編集パラメータ (Phase 4) ----------------
// いずれも録音データには触れず、再生/書き出し時にだけ効く倍率系。
static int g_masterVolPct = 100;  // 全体音量 %
static int g_melVolPct = 100;     // メロディ音量 % (0=ミュート)
static int g_drumVolPct = 100;    // ドラム音量 % (0=ミュート)
static int g_gatePct = 100;       // メロディの音長 %
static int g_repeats = 4;         // 書き出し時のループ繰り返し回数

// ---------------- 型定義 (先頭に置く) ----------------
struct Note {
  uint32_t start;  // ループ内の生の開始 tick
  uint32_t dur;    // 長さ tick
  uint8_t  ch;
  uint8_t  note;
  uint8_t  vel;
};
struct RawEv {
  uint32_t tick;
  uint8_t  status;  // 0x90 / 0x80 / 0xC0
  uint8_t  d1, d2;
};
static int evPriority(const RawEv& e);
static std::vector<RawEv> buildChannelEvents(uint8_t ch, int repeats);
static void buildTrack(std::vector<uint8_t>& out, const std::vector<RawEv>& evs,
                       uint32_t endTick, bool conductor);
static void drawMelody(void);  // メロディ画面 (定義は後半)
static void finalizeActiveKeys(void);  // 押さえ中の鍵を全て確定 (定義は後半)
static void drawEdit(void);    // 編集画面 (定義は後半)
static void stopTransport(void);   // トランスポート操作 (定義は後半)
static void startTransport(bool rec);

// メロディモード: 押さえ中の鍵 (タッチ点ごと)
struct ActiveKey {
  int id = -1;          // タッチ点の ID
  int note = -1;        // MIDI ノート番号
  int songIdx = -1;     // 録音中の g_song インデックス (録音時以外は -1)
  uint32_t absStart = 0;  // 押し始めの絶対 tick (周回をまたいでも数える)
  bool active = false;
};

// ---------------- 曲データ ----------------
static std::vector<Note> g_song;
static uint8_t g_melodyProgram = 0;  // GM 音色番号 (SOUND 画面で選択)

// GM 音色名 (プログラム 0..127)
static const char* GM_NAMES[128] = {
  "Ac Grand Piano","Bright Piano","Electric Grand","Honky-tonk","Electric Piano 1","Electric Piano 2","Harpsichord","Clavinet",
  "Celesta","Glockenspiel","Music Box","Vibraphone","Marimba","Xylophone","Tubular Bells","Dulcimer",
  "Drawbar Organ","Percussive Organ","Rock Organ","Church Organ","Reed Organ","Accordion","Harmonica","Tango Accordion",
  "Nylon Guitar","Steel Guitar","Jazz Guitar","Clean Guitar","Muted Guitar","Overdrive Guitar","Distortion Guitar","Guitar Harmonics",
  "Acoustic Bass","Finger Bass","Pick Bass","Fretless Bass","Slap Bass 1","Slap Bass 2","Synth Bass 1","Synth Bass 2",
  "Violin","Viola","Cello","Contrabass","Tremolo Strings","Pizzicato Strings","Orchestral Harp","Timpani",
  "String Ensemble 1","String Ensemble 2","Synth Strings 1","Synth Strings 2","Choir Aahs","Voice Oohs","Synth Voice","Orchestra Hit",
  "Trumpet","Trombone","Tuba","Muted Trumpet","French Horn","Brass Section","Synth Brass 1","Synth Brass 2",
  "Soprano Sax","Alto Sax","Tenor Sax","Baritone Sax","Oboe","English Horn","Bassoon","Clarinet",
  "Piccolo","Flute","Recorder","Pan Flute","Blown Bottle","Shakuhachi","Whistle","Ocarina",
  "Lead Square","Lead Sawtooth","Lead Calliope","Lead Chiff","Lead Charang","Lead Voice","Lead Fifths","Lead Bass+Ld",
  "Pad New Age","Pad Warm","Pad Polysynth","Pad Choir","Pad Bowed","Pad Metallic","Pad Halo","Pad Sweep",
  "FX Rain","FX Soundtrack","FX Crystal","FX Atmosphere","FX Brightness","FX Goblins","FX Echoes","FX Sci-fi",
  "Sitar","Banjo","Shamisen","Koto","Kalimba","Bagpipe","Fiddle","Shanai",
  "Tinkle Bell","Agogo","Steel Drums","Woodblock","Taiko Drum","Melodic Tom","Synth Drum","Reverse Cymbal",
  "Fret Noise","Breath Noise","Seashore","Bird Tweet","Telephone Ring","Helicopter","Applause","Gunshot"
};

// ---------------- メロディモード状態 ----------------
static int g_melView = 0;        // 0=キーボード, 1=音色選択, 2=グリッド編集
static int g_gridTop = 71;       // グリッド最上段の音高 (B4 から開始)
static int g_gridLenIdx = 0;     // 置く音の長さ: 0=16分 1=8分 2=4分 3=2分
static int g_audNote = -1;       // 試聴中のノート (グリッド配置時)
static uint32_t g_audOffAt = 0;  // 試聴を止める時刻
static int g_keyWin = 7;         // 表示窓の先頭白鍵インデックス (中央の C 付近から)
static int g_sndPage = 0;        // 音色リストのページ 0..3
static ActiveKey g_activeKeys[10];
static uint32_t g_passCount = 0; // ループの周回数 (絶対tick計算用)

static uint32_t absNowTick(void);  // 下で定義 (プロトタイプ)

// ---------------- クォンタイズ ----------------
static uint8_t g_qmode = 0;
static const uint32_t Q_GRID[4]     = {0, PPQ / 4, PPQ / 4, PPQ / 2};
static const uint8_t  Q_STRENGTH[4] = {0, 100, 50, 100};
static const char*    Q_NAME[4]     = {"OFF", "16th 100%", "16th 50%", "8th 100%"};

static uint32_t applyQuantize(uint32_t tick) {
  uint32_t grid = Q_GRID[g_qmode];
  if (grid == 0) return tick;
  uint32_t snapped = ((tick + grid / 2) / grid) * grid;
  int32_t diff = (int32_t)snapped - (int32_t)tick;
  return (uint32_t)((int32_t)tick + diff * Q_STRENGTH[g_qmode] / 100);
}
// グリッド表示用: 生tick → 一番近い16分ステップ (0..63)
static int nearestStep16(uint32_t tick) {
  int s = (int)((tick + PPQ / 8) / (PPQ / 4));
  return s % (LOOP_BARS * 16);
}

// ---------------- モード / トランスポート状態 ----------------
enum Mode { MODE_RHYTHM, MODE_MELODY, MODE_EDIT };
static Mode g_mode = MODE_RHYTHM;

static bool     g_running = false;     // ループ再生中
static bool     g_recording = false;   // 重ね録り中
static uint32_t g_loopStartMs = 0;     // 現周回の開始 millis
static uint32_t g_nextClickTick = 0;
static std::vector<RawEv> g_stream;
static size_t   g_streamIdx = 0;

static uint32_t msToTick(uint32_t ms) {
  return (uint32_t)((uint64_t)ms * g_bpm * PPQ / 60000);
}
static uint32_t tickToMs(uint32_t tick) {
  return (uint32_t)((uint64_t)tick * 60000 / ((uint32_t)g_bpm * PPQ));
}
static uint32_t posTickRaw(void) {
  if (!g_running) return 0;
  return msToTick(millis() - g_loopStartMs);
}
// トランスポート開始からの絶対 tick (キーを離した時刻の計算用)
static uint32_t absNowTick(void) {
  return g_passCount * LOOP_TICKS + posTickRaw();
}

// ---------------- イベント列の構築 ----------------
static int evPriority(const RawEv& e) {
  if ((e.status & 0xF0) == 0xC0) return 0;
  if ((e.status & 0xF0) == 0x80 || ((e.status & 0xF0) == 0x90 && e.d2 == 0)) return 1;
  return 2;
}

static std::vector<RawEv> buildChannelEvents(uint8_t ch, int repeats) {
  std::vector<RawEv> evs;
  int trackPct = (ch == MELODY_CH) ? g_melVolPct : g_drumVolPct;
  for (int rep = 0; rep < repeats; ++rep) {
    uint32_t off = (uint32_t)rep * LOOP_TICKS;
    if (ch == MELODY_CH && rep == 0) evs.push_back({0, (uint8_t)(0xC0 | ch), g_melodyProgram, 0});
    if (ch == MELODY_CH && rep > 0)  evs.push_back({off, (uint8_t)(0xC0 | ch), g_melodyProgram, 0});
    if (trackPct == 0) continue;  // トラックミュート
    for (const Note& n : g_song) {
      if (n.ch != ch) continue;
      uint32_t sLoop = applyQuantize(n.start);
      uint32_t s = sLoop + off;
      // 編集パラメータ: 音量 (全体 x トラックのベロシティ倍率)
      int vel = (int)n.vel * g_masterVolPct * trackPct / 10000;
      if (vel < 1) vel = 1;
      if (vel > 127) vel = 127;
      // 編集パラメータ: 音長 (メロディのみ、ループ境界で頭打ち)
      uint32_t dur = n.dur;
      if (ch == MELODY_CH) {
        dur = dur * (uint32_t)g_gatePct / 100;
        if (dur < 10) dur = 10;
        uint32_t maxDur = LOOP_TICKS - sLoop;
        if (dur > maxDur) dur = maxDur;
      }
      evs.push_back({s, (uint8_t)(0x90 | ch), n.note, (uint8_t)vel});
      evs.push_back({s + dur, (uint8_t)(0x80 | ch), n.note, 0});
    }
  }
  std::sort(evs.begin(), evs.end(), [](const RawEv& a, const RawEv& b) {
    if (a.tick != b.tick) return a.tick < b.tick;
    return evPriority(a) < evPriority(b);
  });
  return evs;
}

static void buildStream(void) {
  g_stream.clear();
  std::vector<RawEv> a = buildChannelEvents(MELODY_CH, 1);
  std::vector<RawEv> b = buildChannelEvents(DRUM_CH, 1);
  g_stream.insert(g_stream.end(), a.begin(), a.end());
  g_stream.insert(g_stream.end(), b.begin(), b.end());
  std::sort(g_stream.begin(), g_stream.end(), [](const RawEv& x, const RawEv& y) {
    if (x.tick != y.tick) return x.tick < y.tick;
    return evPriority(x) < evPriority(y);
  });
  g_streamIdx = 0;
}

// ---------------- MIDI 送信 ----------------
static void sendMidi(uint8_t status, uint8_t d1, uint8_t d2) {
  Serial2.write(status);
  Serial2.write(d1);
  if ((status & 0xF0) != 0xC0) Serial2.write(d2);
}

// ---------------- SD / MIDI ファイル (Phase 1 と同じ) ----------------
static fs::FS* g_fs = nullptr;
static const char* g_sdMode = "none";

static bool initSD(void) {
  if (g_fs) return true;
  SPI.begin(43, 39, 44, 42);  // SCK, MISO, MOSI, CS
  if (SD.begin(42, SPI, 25000000)) { g_fs = &SD; g_sdMode = "SPI"; return true; }
  SD_MMC.setPins(43, 44, 39, 40, 41, 42);
  if (SD_MMC.begin("/sdcard", false, false, 20000)) { g_fs = &SD_MMC; g_sdMode = "SDIO"; return true; }
  return false;
}

static void putVLQ(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t buf[5]; int n = 0;
  buf[n++] = v & 0x7F; v >>= 7;
  while (v) { buf[n++] = 0x80 | (v & 0x7F); v >>= 7; }
  for (int i = n - 1; i >= 0; --i) out.push_back(buf[i]);
}
static void put32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(v >> 24); out.push_back(v >> 16); out.push_back(v >> 8); out.push_back(v);
}
static void put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(v >> 8); out.push_back(v);
}

static void buildTrack(std::vector<uint8_t>& out, const std::vector<RawEv>& evs,
                       uint32_t endTick, bool conductor) {
  std::vector<uint8_t> data;
  uint32_t last = 0;
  if (conductor) {
    putVLQ(data, 0);
    data.insert(data.end(), {0xFF, 0x58, 0x04, 0x04, 0x02, 0x24, 0x08});
    uint32_t mpq = 60000000UL / g_bpm;
    putVLQ(data, 0);
    data.insert(data.end(), {0xFF, 0x51, 0x03,
                             (uint8_t)(mpq >> 16), (uint8_t)(mpq >> 8), (uint8_t)mpq});
  }
  for (const RawEv& e : evs) {
    putVLQ(data, e.tick - last);
    last = e.tick;
    data.push_back(e.status);
    data.push_back(e.d1);
    if ((e.status & 0xF0) != 0xC0) data.push_back(e.d2);
  }
  putVLQ(data, endTick > last ? endTick - last : 0);
  data.insert(data.end(), {0xFF, 0x2F, 0x00});

  out.insert(out.end(), {'M', 'T', 'r', 'k'});
  put32(out, data.size());
  out.insert(out.end(), data.begin(), data.end());
}

static int exportMidiFile(void) {
  if (!initSD()) return -1;
  uint32_t end = LOOP_TICKS * g_repeats;
  std::vector<uint8_t> file;
  file.insert(file.end(), {'M', 'T', 'h', 'd'});
  put32(file, 6);
  put16(file, 1);
  put16(file, 3);
  put16(file, (uint16_t)PPQ);
  buildTrack(file, {}, end, true);
  buildTrack(file, buildChannelEvents(MELODY_CH, g_repeats), end, false);
  buildTrack(file, buildChannelEvents(DRUM_CH, g_repeats), end, false);

  File f = g_fs->open("/tab5song.mid", "w");
  if (!f) return -2;
  size_t written = f.write(file.data(), file.size());
  f.close();
  return (int)written;
}

// ---------------- ドラム定義 ----------------
static const uint8_t DRUM_NOTES[10] = {36, 38, 42, 46, 39, 50, 45, 49, 51, 56};
static const char* DRUM_NAMES[10] = {"KICK", "SNARE", "C-HAT", "O-HAT", "CLAP",
                                     "TOM-H", "TOM-L", "CRASH", "RIDE", "BELL"};
static int g_selDrum = 0;   // グリッド表示中の楽器
static int g_gridBar = 0;   // グリッド表示中の小節 0..3

// ---------------- 画面 ----------------
static String g_msg = "RHYTHM: RECでループ録音、パッド演奏とグリッド編集ができます";

static void drawTabs(void) {
  static const char* names[3] = {"RHYTHM", "MELODY", "EDIT / PLAY"};
  static const int xs[3] = {20, 440, 860};
  static const int ws[3] = {400, 400, 380};
  for (int i = 0; i < 3; ++i) {
    uint16_t col = (g_mode == (Mode)i) ? TFT_BLUE : TFT_DARKGREY;
    M5.Display.fillRect(xs[i], 8, ws[i], 54, col);
    M5.Display.drawRect(xs[i], 8, ws[i], 54, TFT_WHITE);
    M5.Display.setTextColor(TFT_WHITE, col);
    M5.Display.setTextSize(3);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(names[i], xs[i] + ws[i] / 2, 35);
  }
  M5.Display.setTextDatum(top_left);
}

static void drawTransport(void) {
  static const char* names[5] = {"REC", "PLAY", "STOP", "Q", "EXPORT"};
  for (int i = 0; i < 5; ++i) {
    int x = 20 + i * 246;
    uint16_t col = TFT_DARKGREY;
    if (i == 0 && g_recording) col = TFT_RED;
    if (i == 1 && g_running)   col = TFT_GREEN;
    if (i == 3) col = TFT_BLUE;
    M5.Display.fillRect(x, 74, 236, 66, col);
    M5.Display.drawRect(x, 74, 236, 66, TFT_WHITE);
    M5.Display.setTextColor(TFT_WHITE, col);
    M5.Display.setTextSize(3);
    M5.Display.setTextDatum(middle_center);
    String label = (i == 3) ? String("Q:") + Q_NAME[g_qmode] : String(names[i]);
    M5.Display.drawString(label, x + 118, 107);
  }
  M5.Display.setTextDatum(top_left);
}

static void drawFooter(void) {
  M5.Display.fillRect(0, 640, 1280, 80, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(20, 648);
  int drums = 0, mels = 0;
  for (const Note& n : g_song) (n.ch == DRUM_CH ? drums : mels)++;
  M5.Display.printf("notes: drum %d / melody %d   BPM %d   loop %d bars   SD:%s",
                    drums, mels, g_bpm, LOOP_BARS, g_sdMode);
  M5.Display.setCursor(20, 682);
  M5.Display.print(g_msg);
}

static void drawPads(void) {
  for (int i = 0; i < 10; ++i) {
    int x = 20 + (i % 5) * 246;
    int y = 156 + (i / 5) * 124;
    uint16_t col = (i == g_selDrum) ? TFT_ORANGE : TFT_RED;
    M5.Display.fillRect(x, y, 236, 114, col);
    M5.Display.drawRect(x, y, 236, 114, TFT_WHITE);
    if (i == g_selDrum) M5.Display.drawRect(x + 3, y + 3, 230, 108, TFT_YELLOW);
    M5.Display.setTextColor(TFT_WHITE, col);
    M5.Display.setTextSize(3);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(DRUM_NAMES[i], x + 118, y + 57);
  }
  M5.Display.setTextDatum(top_left);
}

// グリッドヘッダ: 楽器名 / 小節ボタン / UNDO / CLEAR
static void drawGridHeader(void) {
  M5.Display.fillRect(0, 412, 1280, 56, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(20, 424);
  M5.Display.printf("STEP: %s", DRUM_NAMES[g_selDrum]);
  for (int b = 0; b < 4; ++b) {
    int x = 330 + b * 76;
    uint16_t col = (b == g_gridBar) ? TFT_BLUE : TFT_DARKGREY;
    M5.Display.fillRect(x, 416, 66, 48, col);
    M5.Display.drawRect(x, 416, 66, 48, TFT_WHITE);
    M5.Display.setTextColor(TFT_WHITE, col);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(String(b + 1), x + 33, 440);
  }
  M5.Display.fillRect(660, 416, 180, 48, TFT_DARKGREY);
  M5.Display.drawRect(660, 416, 180, 48, TFT_WHITE);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.drawString("UNDO", 750, 440);
  M5.Display.fillRect(856, 416, 384, 48, TFT_DARKGREY);
  M5.Display.drawRect(856, 416, 384, 48, TFT_WHITE);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.drawString("CLEAR DRUMS", 1048, 440);
  M5.Display.setTextDatum(top_left);
}

static bool stepHasNote(int absStep) {
  for (const Note& n : g_song) {
    if (n.ch == DRUM_CH && n.note == DRUM_NOTES[g_selDrum] &&
        nearestStep16(n.start) == absStep) return true;
  }
  return false;
}

static int g_lastStepShown = -1;
static void drawSteps(bool force) {
  int curStep = -1;
  if (g_running) {
    int s = (int)(posTickRaw() / (PPQ / 4));
    if (s / 16 == g_gridBar) curStep = s % 16;
  }
  if (!force && curStep == g_lastStepShown) return;
  g_lastStepShown = curStep;
  for (int c = 0; c < 16; ++c) {
    int x = 20 + c * 77;
    int absStep = g_gridBar * 16 + c;
    bool on = stepHasNote(absStep);
    uint16_t col = on ? TFT_ORANGE : ((c % 4 == 0) ? TFT_DARKGREY : TFT_BLACK);
    M5.Display.fillRect(x, 484, 70, 130, col);
    M5.Display.drawRect(x, 484, 70, 130, (c == curStep) ? TFT_YELLOW : TFT_WHITE);
    if (c == curStep) M5.Display.drawRect(x + 3, 487, 64, 124, TFT_YELLOW);
  }
}

static void drawRhythm(void) {
  M5.Display.fillRect(0, 148, 1280, 492, TFT_BLACK);
  drawPads();
  drawGridHeader();
  g_lastStepShown = -2;
  drawSteps(true);
}

static void drawPlaceholder(const char* text) {
  M5.Display.fillRect(0, 148, 1280, 492, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(4);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(text, 640, 380);
  M5.Display.setTextDatum(top_left);
}

static void drawAll(void) {
  drawTabs();
  drawTransport();
  if (g_mode == MODE_RHYTHM) drawRhythm();
  else if (g_mode == MODE_MELODY) drawMelody();
  else drawEdit();
  drawFooter();
}

// ---------------- メロディモード ----------------
static const uint8_t WHITES[36] = {
  36,38,40,41,43,45,47, 48,50,52,53,55,57,59, 60,62,64,65,67,69,71,
  72,74,76,77,79,81,83, 84,86,88,89,91,93,95, 96
};
static const int VIS_WHITES = 15;
static const int KEY_X = 25, KEY_Y = 206, KEY_W = 82, KEY_H = 330;
static const int BLK_W = 48, BLK_H = 195;
static const int STRIP_X = 90, STRIP_Y = 548, STRIP_W = 1100, STRIP_H = 48;

static bool hasBlackAfter(int whiteIdx) {
  int pc = WHITES[whiteIdx] % 12;
  return (pc == 0 || pc == 2 || pc == 5 || pc == 7 || pc == 9) && whiteIdx < 35;
}
static bool noteIsActive(int note) {
  for (const ActiveKey& k : g_activeKeys) if (k.active && k.note == note) return true;
  return false;
}
static int whiteIndexOf(int note) {
  for (int i = 0; i < 36; ++i) if (WHITES[i] == note) return i;
  return -1;
}

static void drawWhiteKey(int vis) {
  int w = g_keyWin + vis;
  int note = WHITES[w];
  int x = KEY_X + vis * KEY_W;
  uint16_t col = noteIsActive(note) ? TFT_SKYBLUE : TFT_WHITE;
  M5.Display.fillRect(x, KEY_Y, KEY_W, KEY_H, col);
  M5.Display.drawRect(x, KEY_Y, KEY_W, KEY_H, TFT_BLACK);
  if (note % 12 == 0) {
    M5.Display.setTextColor(TFT_BLACK, col);
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("C" + String(note / 12 - 1), x + KEY_W / 2, KEY_Y + KEY_H - 22);
    M5.Display.setTextDatum(top_left);
  }
}
static void drawBlackKey(int vis) {
  int w = g_keyWin + vis;
  if (!hasBlackAfter(w)) return;
  int note = WHITES[w] + 1;
  int x = KEY_X + (vis + 1) * KEY_W - BLK_W / 2;
  uint16_t col = noteIsActive(note) ? TFT_NAVY : TFT_BLACK;
  M5.Display.fillRect(x, KEY_Y, BLK_W, BLK_H, col);
  M5.Display.drawRect(x, KEY_Y, BLK_W, BLK_H, TFT_WHITE);
}
static void drawKeyboard(void) {
  for (int i = 0; i < VIS_WHITES; ++i) drawWhiteKey(i);
  for (int i = 0; i < VIS_WHITES; ++i) drawBlackKey(i);
}
static void drawKeyByNote(int note) {
  int w = whiteIndexOf(note);
  if (w >= 0) {
    int vis = w - g_keyWin;
    if (vis >= 0 && vis < VIS_WHITES) {
      drawWhiteKey(vis);
      drawBlackKey(vis - 1);
      drawBlackKey(vis);
    }
  } else {
    int bw = whiteIndexOf(note - 1);
    int vis = bw - g_keyWin;
    if (bw >= 0 && vis >= 0 && vis < VIS_WHITES) drawBlackKey(vis);
  }
}
static void drawStrip(void) {
  M5.Display.fillRect(STRIP_X, STRIP_Y, STRIP_W, STRIP_H, TFT_DARKGREY);
  M5.Display.drawRect(STRIP_X, STRIP_Y, STRIP_W, STRIP_H, TFT_WHITE);
  int wx = STRIP_X + g_keyWin * STRIP_W / 36;
  int ww = VIS_WHITES * STRIP_W / 36;
  M5.Display.fillRect(wx, STRIP_Y + 4, ww, STRIP_H - 8, TFT_BLUE);
  M5.Display.drawRect(wx, STRIP_Y + 4, ww, STRIP_H - 8, TFT_WHITE);
  // オクターブ移動ボタン
  M5.Display.fillRect(20, STRIP_Y, 60, STRIP_H, TFT_DARKGREY);
  M5.Display.drawRect(20, STRIP_Y, 60, STRIP_H, TFT_WHITE);
  M5.Display.fillRect(1200, STRIP_Y, 60, STRIP_H, TFT_DARKGREY);
  M5.Display.drawRect(1200, STRIP_Y, 60, STRIP_H, TFT_WHITE);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setTextSize(3);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("<", 50, STRIP_Y + STRIP_H / 2);
  M5.Display.drawString(">", 1230, STRIP_Y + STRIP_H / 2);
  M5.Display.setTextDatum(top_left);
}

static void drawMelSubBar(void) {
  M5.Display.fillRect(0, 148, 1280, 52, TFT_BLACK);
  uint16_t kc = (g_melView == 0) ? TFT_BLUE : TFT_DARKGREY;
  uint16_t sc = (g_melView == 1) ? TFT_BLUE : TFT_DARKGREY;
  uint16_t gc = (g_melView == 2) ? TFT_BLUE : TFT_DARKGREY;
  M5.Display.fillRect(20, 150, 140, 46, kc);
  M5.Display.drawRect(20, 150, 140, 46, TFT_WHITE);
  M5.Display.fillRect(170, 150, 140, 46, sc);
  M5.Display.drawRect(170, 150, 140, 46, TFT_WHITE);
  M5.Display.fillRect(320, 150, 140, 46, gc);
  M5.Display.drawRect(320, 150, 140, 46, TFT_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_WHITE, kc);
  M5.Display.drawString("KEYS", 90, 173);
  M5.Display.setTextColor(TFT_WHITE, sc);
  M5.Display.drawString("SOUND", 240, 173);
  M5.Display.setTextColor(TFT_WHITE, gc);
  M5.Display.drawString("GRID", 390, 173);
  M5.Display.fillRect(880, 150, 130, 46, TFT_DARKGREY);
  M5.Display.drawRect(880, 150, 130, 46, TFT_WHITE);
  M5.Display.fillRect(1020, 150, 220, 46, TFT_DARKGREY);
  M5.Display.drawRect(1020, 150, 220, 46, TFT_WHITE);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.drawString("UNDO", 945, 173);
  M5.Display.drawString("CLEAR MEL", 1130, 173);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(475, 160);
  M5.Display.printf("%03d %s", g_melodyProgram + 1, GM_NAMES[g_melodyProgram]);
}

static void drawSoundList(void) {
  M5.Display.fillRect(0, 202, 1280, 396, TFT_BLACK);
  for (int i = 0; i < 32; ++i) {
    int p = g_sndPage * 32 + i;
    int col = i / 16, row = i % 16;
    int x = (col == 0) ? 20 : 650;
    int y = 206 + row * 24;
    uint16_t bg = (p == g_melodyProgram) ? TFT_BLUE : TFT_BLACK;
    M5.Display.fillRect(x, y, 610, 23, bg);
    M5.Display.setTextColor(TFT_WHITE, bg);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(x + 8, y + 3);
    M5.Display.printf("%03d %s", p + 1, GM_NAMES[p]);
  }
  M5.Display.fillRect(20, 600, 200, 36, TFT_DARKGREY);
  M5.Display.drawRect(20, 600, 200, 36, TFT_WHITE);
  M5.Display.fillRect(1060, 600, 200, 36, TFT_DARKGREY);
  M5.Display.drawRect(1060, 600, 200, 36, TFT_WHITE);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("< PAGE", 120, 618);
  M5.Display.drawString("PAGE >", 1160, 618);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString("PAGE " + String(g_sndPage + 1) + " / 4", 640, 618);
  M5.Display.setTextDatum(top_left);
}

// ---------------- メロディ グリッド編集 (Phase 5) ----------------
static const int GRID_HDR_Y = 200;
static const int GRID_Y = 248, GRID_ROW_H = 31;
static const int GRID_CELL_X = 116, GRID_CELL_W = 68, GRID_CELL_STEP = 70;
static const uint32_t GRID_LENS[4] = {PPQ / 4, PPQ / 2, PPQ, PPQ * 2};
static const char* GRID_LEN_NAMES[4] = {"16th", "8th", "4th", "2nd"};
static const char* NOTE_NAMES12[12] = {"C", "C#", "D", "D#", "E", "F",
                                       "F#", "G", "G#", "A", "A#", "B"};
static int g_lastMelStepShown = -1;

static void fitGridWindow(void) {
  int mn = 128, mx = -1;
  for (const Note& n : g_song) {
    if (n.ch != MELODY_CH) continue;
    if ((int)n.note < mn) mn = n.note;
    if ((int)n.note > mx) mx = n.note;
  }
  if (mx < 0) return;
  int top = mx;
  if (top - 11 < mn) top = mn + 11;
  if (top < 47) top = 47;
  if (top > 96) top = 96;
  g_gridTop = top;
}

// セルの状態: 0=なし, 1=音の開始, 2=音の続き (長さ分)
static int gridCellState(int pitch, int absStep) {
  for (const Note& n : g_song) {
    if (n.ch != MELODY_CH || (int)n.note != pitch) continue;
    int ss = nearestStep16(n.start);
    if (ss == absStep) return 1;
    int span = (int)((n.dur + PPQ / 4 - 1) / (PPQ / 4));
    if (absStep > ss && absStep < ss + span) return 2;
  }
  return 0;
}

static void drawMelGridHeader(void) {
  M5.Display.fillRect(0, GRID_HDR_Y, 1280, 44, TFT_BLACK);
  for (int b = 0; b < 4; ++b) {
    int x = GRID_CELL_X + b * 68;
    uint16_t col = (b == g_gridBar) ? TFT_BLUE : TFT_DARKGREY;
    M5.Display.fillRect(x, GRID_HDR_Y, 60, 44, col);
    M5.Display.drawRect(x, GRID_HDR_Y, 60, 44, TFT_WHITE);
    M5.Display.setTextColor(TFT_WHITE, col);
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(String(b + 1), x + 30, GRID_HDR_Y + 22);
  }
  M5.Display.fillRect(420, GRID_HDR_Y, 170, 44, TFT_DARKGREY);
  M5.Display.drawRect(420, GRID_HDR_Y, 170, 44, TFT_WHITE);
  M5.Display.fillRect(610, GRID_HDR_Y, 70, 44, TFT_DARKGREY);
  M5.Display.drawRect(610, GRID_HDR_Y, 70, 44, TFT_WHITE);
  M5.Display.fillRect(690, GRID_HDR_Y, 70, 44, TFT_DARKGREY);
  M5.Display.drawRect(690, GRID_HDR_Y, 70, 44, TFT_WHITE);
  M5.Display.fillRect(880, GRID_HDR_Y, 170, 44, TFT_DARKGREY);
  M5.Display.drawRect(880, GRID_HDR_Y, 170, 44, TFT_WHITE);
  M5.Display.fillRect(1060, GRID_HDR_Y, 180, 44, TFT_DARKGREY);
  M5.Display.drawRect(1060, GRID_HDR_Y, 180, 44, TFT_WHITE);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("LEN:" + String(GRID_LEN_NAMES[g_gridLenIdx]), 505, GRID_HDR_Y + 22);
  M5.Display.drawString("-1", 645, GRID_HDR_Y + 22);
  M5.Display.drawString("+1", 725, GRID_HDR_Y + 22);
  M5.Display.drawString("OCT -", 965, GRID_HDR_Y + 22);
  M5.Display.drawString("OCT +", 1150, GRID_HDR_Y + 22);
  M5.Display.setTextDatum(top_left);
}

static void drawMelGridRows(void) {
  int curCol = -1;
  if (g_running) {
    int s = (int)(posTickRaw() / (PPQ / 4));
    if (s / 16 == g_gridBar) curCol = s % 16;
  }
  for (int r = 0; r < 12; ++r) {
    int pitch = g_gridTop - r;
    int y = GRID_Y + r * GRID_ROW_H;
    uint16_t lc = (pitch % 12 == 0) ? TFT_BLUE : TFT_DARKGREY;
    M5.Display.fillRect(20, y, 88, GRID_ROW_H - 2, lc);
    M5.Display.setTextColor(TFT_WHITE, lc);
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(String(NOTE_NAMES12[pitch % 12]) + String(pitch / 12 - 1),
                          64, y + GRID_ROW_H / 2);
    for (int c = 0; c < 16; ++c) {
      int x = GRID_CELL_X + c * GRID_CELL_STEP;
      int st = gridCellState(pitch, g_gridBar * 16 + c);
      uint16_t col = (st == 1) ? TFT_ORANGE
                   : (st == 2) ? M5.Display.color565(140, 70, 0)
                   : ((c % 4 == 0) ? M5.Display.color565(40, 40, 40) : TFT_BLACK);
      M5.Display.fillRect(x, y, GRID_CELL_W, GRID_ROW_H - 2, col);
      M5.Display.drawRect(x, y, GRID_CELL_W, GRID_ROW_H - 2,
                          (c == curCol) ? TFT_YELLOW : M5.Display.color565(90, 90, 90));
    }
  }
  M5.Display.setTextDatum(top_left);
}

static void drawMelGrid(void) {
  M5.Display.fillRect(0, 196, 1280, 444, TFT_BLACK);
  drawMelGridHeader();
  g_lastMelStepShown = -2;
  drawMelGridRows();
}

static void toggleMelStep(int pitch, int absStep) {
  finalizeActiveKeys();  // 削除で録音中ノートの参照がずれるのを防ぐ
  bool removed = false;
  for (size_t i = 0; i < g_song.size();) {
    const Note& n = g_song[i];
    bool hit = false;
    if (n.ch == MELODY_CH && (int)n.note == pitch) {
      int ss = nearestStep16(n.start);
      int span = (int)((n.dur + PPQ / 4 - 1) / (PPQ / 4));
      if (span < 1) span = 1;
      if (absStep >= ss && absStep < ss + span) hit = true;
    }
    if (hit) { g_song.erase(g_song.begin() + i); removed = true; }
    else ++i;
  }
  if (!removed) {
    uint32_t start = (uint32_t)absStep * (PPQ / 4);
    uint32_t dur = GRID_LENS[g_gridLenIdx];
    if (start + dur > LOOP_TICKS) dur = LOOP_TICKS - start;
    g_song.push_back({start, dur, MELODY_CH, (uint8_t)pitch, 110});
    synth.setNoteOn(MELODY_CH, (uint8_t)pitch, 110);  // 試聴
    g_audNote = pitch;
    g_audOffAt = millis() + 150;
  }
}

static void drawMelody(void) {
  M5.Display.fillRect(0, 148, 1280, 492, TFT_BLACK);
  drawMelSubBar();
  if (g_melView == 0) { drawKeyboard(); drawStrip(); }
  else if (g_melView == 1) drawSoundList();
  else drawMelGrid();
}

static void selectProgram(int p) {
  g_melodyProgram = (uint8_t)p;
  sendMidi(0xC0 | MELODY_CH, (uint8_t)p, 0);
  if (!g_running) {  // 停止中だけ試聴 (短い単音)
    synth.setNoteOn(MELODY_CH, 60, 100);
    delay(130);
    synth.setNoteOff(MELODY_CH, 60, 0);
  }
  g_msg = "音色: " + String(p + 1) + " " + GM_NAMES[p];
}

static void keyOn(int note, int id) {
  ActiveKey* slot = nullptr;
  for (ActiveKey& k : g_activeKeys) if (k.active && k.id == id) return;
  for (ActiveKey& k : g_activeKeys) if (!k.active) { slot = &k; break; }
  if (!slot) return;
  slot->id = id;
  slot->note = note;
  slot->songIdx = -1;
  slot->absStart = absNowTick();
  slot->active = true;
  synth.setNoteOn(MELODY_CH, (uint8_t)note, 110);
  if (g_running && g_recording) {
    uint32_t start = absNowTick() % LOOP_TICKS;
    g_song.push_back({start, 0, MELODY_CH, (uint8_t)note, 110});
    slot->songIdx = (int)g_song.size() - 1;
  }
  drawKeyByNote(note);
}

static void keyOffById(int id) {
  for (ActiveKey& k : g_activeKeys) {
    if (!k.active || k.id != id) continue;
    int note = k.note;
    synth.setNoteOff(MELODY_CH, (uint8_t)note, 0);
    if (k.songIdx >= 0 && k.songIdx < (int)g_song.size()) {
      Note& n = g_song[k.songIdx];
      uint32_t d = absNowTick() - k.absStart;
      uint32_t maxDur = LOOP_TICKS - n.start;
      if (d > maxDur) d = maxDur;
      n.dur = d ? d : 30;
    }
    k.active = false;
    k.id = -1;
    k.songIdx = -1;
    drawKeyByNote(note);
    return;
  }
}

static void finalizeActiveKeys(void) {
  for (const ActiveKey& k : g_activeKeys) if (k.active) keyOffById(k.id);
}

static int keyAtPoint(int x, int y) {
  if (y < KEY_Y || y >= KEY_Y + KEY_H || x < KEY_X || x >= KEY_X + VIS_WHITES * KEY_W) return -1;
  if (y < KEY_Y + BLK_H) {  // 黒鍵を先に判定
    for (int i = 0; i < VIS_WHITES; ++i) {
      if (!hasBlackAfter(g_keyWin + i)) continue;
      int bx = KEY_X + (i + 1) * KEY_W - BLK_W / 2;
      if (x >= bx && x < bx + BLK_W) return WHITES[g_keyWin + i] + 1;
    }
  }
  int i = (x - KEY_X) / KEY_W;
  if (i >= 0 && i < VIS_WHITES) return WHITES[g_keyWin + i];
  return -1;
}

static void scrollStripTo(int x) {
  float cell = (float)STRIP_W / 36.0f;
  int center = (int)((x - STRIP_X) / cell);
  int win = center - VIS_WHITES / 2;
  if (win < 0) win = 0;
  if (win > 36 - VIS_WHITES) win = 36 - VIS_WHITES;
  if (win != g_keyWin) {
    g_keyWin = win;
    drawKeyboard();
    drawStrip();
  }
}

// キーボード面の全タッチ点を処理 (マルチタッチ / グリッサンド / ストリップ)
//
// ★ Phase 3 初版のバグ修正 (和音で鍵が押しっぱなしになる件):
//   初版は「離した」イベント (wasReleased) だけを頼りに音を止めていた。
//   ところがマルチタッチでは、指が 1 本離れるたびにタッチ点の並びが
//   詰め直されるため、離した指の detail が getCount() の範囲外に出て
//   wasReleased を拾えないことがある (単音では起きないので和音だけ
//   症状が出た)。対策として毎フレーム「今いる指の ID 一覧」と
//   ActiveKey を突き合わせ、一覧に無い ID の音は強制的に止める
//   照合方式に変えた。wasReleased が届く場合も従来通り即座に止まる。
static void handleMelodyPoints(void) {
  int cnt = (int)M5.Touch.getCount();
  if (cnt > 10) cnt = 10;

  // まず全タッチ点を集め、「今触れている ID」の一覧を作る
  struct Pt { int id, x, y; bool pressed, down, released; };
  Pt pts[10];
  bool idDown[256] = {};
  for (int i = 0; i < cnt; ++i) {
    auto d = M5.Touch.getDetail(i);
    pts[i] = {(int)d.id, (int)d.x, (int)d.y,
              d.wasPressed(), d.isPressed(), d.wasReleased()};
    if (pts[i].down || pts[i].pressed) idDown[pts[i].id & 0xFF] = true;
  }

  // 照合: 今いる指の中に無い ID の音は、離されたとみなして止める
  for (const ActiveKey& k : g_activeKeys) {
    if (k.active && !idDown[k.id & 0xFF]) keyOffById(k.id);
  }

  // 各タッチ点の処理
  for (int i = 0; i < cnt; ++i) {
    const Pt& p = pts[i];
    bool inStrip = (p.y >= STRIP_Y && p.y < STRIP_Y + STRIP_H &&
                    p.x >= STRIP_X && p.x < STRIP_X + STRIP_W);
    if (p.released) { keyOffById(p.id); continue; }
    if (p.pressed) {
      if (inStrip) {
        scrollStripTo(p.x);
      } else {
        int n = keyAtPoint(p.x, p.y);
        if (n >= 0) keyOn(n, p.id);
      }
    } else if (p.down) {
      if (inStrip) {
        scrollStripTo(p.x);
      } else {
        // グリッサンド: 同じ指が別の鍵へ移動したら鳴り替える
        for (const ActiveKey& k : g_activeKeys) {
          if (k.active && k.id == p.id) {
            int n = keyAtPoint(p.x, p.y);
            if (n >= 0 && n != k.note) { keyOffById(p.id); keyOn(n, p.id); }
            break;
          }
        }
        // ID が付け替わって wasPressed を取りこぼした指への保険:
        // 押されているのに担当の音が無い指は、その場の鍵を鳴らす
        bool hasSlot = false;
        for (const ActiveKey& k : g_activeKeys) if (k.active && k.id == p.id) hasSlot = true;
        if (!hasSlot) {
          int n = keyAtPoint(p.x, p.y);
          if (n >= 0) keyOn(n, p.id);
        }
      }
    }
  }
}

static void undoLastMelody(void) {
  finalizeActiveKeys();
  for (int i = (int)g_song.size() - 1; i >= 0; --i) {
    if (g_song[i].ch == MELODY_CH) { g_song.erase(g_song.begin() + i); return; }
  }
}
static void clearMelody(void) {
  finalizeActiveKeys();
  for (size_t i = 0; i < g_song.size();) {
    if (g_song[i].ch == MELODY_CH) g_song.erase(g_song.begin() + i);
    else ++i;
  }
}

// ---------------- 編集 / 再生モード ----------------
struct ParamDef {
  const char* label;
  int* val;
  int minv, maxv, step;
};
static ParamDef PARAMS[6] = {
  {"TEMPO (BPM)",     &g_bpm,          60, 200, 2},
  {"MASTER VOLUME %", &g_masterVolPct, 10, 200, 5},
  {"MELODY VOLUME %", &g_melVolPct,     0, 200, 5},
  {"DRUM VOLUME %",   &g_drumVolPct,    0, 200, 5},
  {"NOTE LENGTH %",   &g_gatePct,      10, 200, 5},
  {"EXPORT LOOPS",    &g_repeats,       1,   8, 1},
};
static const int ROW_Y0 = 200, ROW_H = 68;

static void drawEditRow(int i) {
  const ParamDef& p = PARAMS[i];
  int y = ROW_Y0 + i * ROW_H;
  M5.Display.fillRect(0, y, 1280, ROW_H, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(20, y + 20);
  M5.Display.print(p.label);
  // - / + ボタンと値
  M5.Display.fillRect(440, y + 6, 90, 56, TFT_DARKGREY);
  M5.Display.drawRect(440, y + 6, 90, 56, TFT_WHITE);
  M5.Display.fillRect(790, y + 6, 90, 56, TFT_DARKGREY);
  M5.Display.drawRect(790, y + 6, 90, 56, TFT_WHITE);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setTextSize(4);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("-", 485, y + 34);
  M5.Display.drawString("+", 835, y + 34);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(String(*p.val), 660, y + 34);
  // バー
  M5.Display.drawRect(920, y + 14, 320, 40, TFT_WHITE);
  int fw = 316 * (*p.val - p.minv) / (p.maxv - p.minv);
  if (fw > 0) M5.Display.fillRect(922, y + 16, fw, 36, TFT_BLUE);
  M5.Display.setTextDatum(top_left);
}

static void drawEdit(void) {
  M5.Display.fillRect(0, 148, 1280, 492, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(20, 162);
  M5.Display.print("仕上げの調整: 再生と書き出しに反映されます (元の録音は変わりません)");
  M5.Display.fillRect(1000, 152, 240, 40, TFT_DARKGREY);
  M5.Display.drawRect(1000, 152, 240, 40, TFT_WHITE);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("RESET PARAMS", 1120, 172);
  M5.Display.setTextDatum(top_left);
  for (int i = 0; i < 6; ++i) drawEditRow(i);
}

static void resetParams(void) {
  g_bpm = 120;
  g_masterVolPct = 100;
  g_melVolPct = 100;
  g_drumVolPct = 100;
  g_gatePct = 100;
  g_repeats = 4;
}

static void adjustParam(int i, int dir) {
  ParamDef& p = PARAMS[i];
  int nv = *p.val + dir * p.step;
  if (nv < p.minv) nv = p.minv;
  if (nv > p.maxv) nv = p.maxv;
  if (nv == *p.val) return;
  *p.val = nv;
  if (i == 0 && g_running && !g_recording) {
    // テンポ変更はループ頭から再生し直して即反映する
    stopTransport();
    startTransport(false);
    drawTransport();
  }
  g_msg = String(p.label) + " = " + String(nv);
  drawEditRow(i);
  drawFooter();
}

// ---------------- トランスポート操作 ----------------
static void stopTransport(void) {
  finalizeActiveKeys();  // 押さえ中の鍵を確定してから止める
  g_running = false;
  g_recording = false;
  synth.setAllNotesOff(MELODY_CH);
  synth.setAllNotesOff(DRUM_CH);
}

static void startTransport(bool rec) {
  if (!g_running) {
    g_running = true;
    g_recording = rec;
    g_loopStartMs = millis();
    g_nextClickTick = 0;
    g_passCount = 0;
    buildStream();
  } else {
    g_recording = rec ? !g_recording : g_recording;  // 再生中の REC は録音トグル
  }
}

static void hitDrum(int i) {
  g_selDrum = i;
  synth.setNoteOn(DRUM_CH, DRUM_NOTES[i], 110);
  if (g_running && g_recording) {
    uint32_t pos = posTickRaw() % LOOP_TICKS;
    g_song.push_back({pos, 30, DRUM_CH, DRUM_NOTES[i], 110});
  }
}

static void toggleStep(int absStep) {
  finalizeActiveKeys();  // 削除で録音中ノートの参照がずれるのを防ぐ
  bool removed = false;
  for (size_t i = 0; i < g_song.size();) {
    const Note& n = g_song[i];
    if (n.ch == DRUM_CH && n.note == DRUM_NOTES[g_selDrum] &&
        nearestStep16(n.start) == absStep) {
      g_song.erase(g_song.begin() + i);
      removed = true;
    } else ++i;
  }
  if (!removed) {
    uint32_t start = (uint32_t)absStep * (PPQ / 4);
    g_song.push_back({start, 30, DRUM_CH, DRUM_NOTES[g_selDrum], 110});
    synth.setNoteOn(DRUM_CH, DRUM_NOTES[g_selDrum], 110);
  }
  // 再生中なら次の周回から反映 (ストリームは周回頭で再構築される)
}

static void undoLastDrum(void) {
  finalizeActiveKeys();
  for (int i = (int)g_song.size() - 1; i >= 0; --i) {
    if (g_song[i].ch == DRUM_CH) { g_song.erase(g_song.begin() + i); return; }
  }
}

static void clearDrums(void) {
  finalizeActiveKeys();
  for (size_t i = 0; i < g_song.size();) {
    if (g_song[i].ch == DRUM_CH) g_song.erase(g_song.begin() + i);
    else ++i;
  }
}

// ---------------- Arduino ----------------
void setup(void) {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  M5.Display.setRotation(1);
  M5.Display.fillScreen(TFT_BLACK);
  drawAll();

  synth.begin(&Serial2, UNIT_SYNTH_BAUD, SYNTH_RX_PIN, SYNTH_TX_PIN);
  Serial.println("tab5_synth ready");
}

void loop(void) {
  M5.update();

  // ----- ループ再生スケジューラ (周回処理つき) -----
  if (g_running) {
    uint32_t pos = posTickRaw();
    if (pos >= LOOP_TICKS) {  // 周回: 残りを流してから次の周へ
      while (g_streamIdx < g_stream.size()) {
        const RawEv& e = g_stream[g_streamIdx++];
        sendMidi(e.status, e.d1, e.d2);
      }
      g_loopStartMs += tickToMs(LOOP_TICKS);
      g_passCount++;
      buildStream();
      g_nextClickTick = 0;
      pos = posTickRaw();
      if (g_mode == MODE_RHYTHM) { g_lastStepShown = -2; }
    }
    while (g_streamIdx < g_stream.size() && g_stream[g_streamIdx].tick <= pos) {
      const RawEv& e = g_stream[g_streamIdx++];
      sendMidi(e.status, e.d1, e.d2);
    }
    // 録音中のクリック
    if (g_recording && pos >= g_nextClickTick) {
      uint32_t beat = g_nextClickTick / PPQ;
      synth.setNoteOn(DRUM_CH, 42, (beat % 4 == 0) ? 120 : 70);
      g_nextClickTick += PPQ;
    }
  }

  // ----- タッチ -----
  auto touch = M5.Touch.getDetail();
  if (touch.wasPressed()) {
    int tx = touch.x, ty = touch.y;
    if (ty >= 8 && ty < 62) {  // タブ
      Mode m = (tx < 440) ? MODE_RHYTHM : (tx < 860 ? MODE_MELODY : MODE_EDIT);
      if (m != g_mode) { g_mode = m; drawAll(); }
    } else if (ty >= 74 && ty < 140) {  // トランスポート
      int i = (tx - 20) / 246;
      if (i >= 0 && i < 5 && tx - 20 - i * 246 < 236) {
        switch (i) {
          case 0: startTransport(true);  g_msg = g_recording ? "ループ録音中 (重ね録り)" : "録音オフ (再生継続)"; break;
          case 1: startTransport(false); g_msg = "ループ再生中"; break;
          case 2: stopTransport();       g_msg = "停止"; break;
          case 3:
            g_qmode = (g_qmode + 1) % 4;
            g_msg = "クォンタイズ: " + String(Q_NAME[g_qmode]);
            if (g_running) { /* 次の周回から適用 */ }
            break;
          case 4: {
            int r = exportMidiFile();
            g_msg = (r >= 0) ? ("書き出し完了 /tab5song.mid (" + String(r) + " bytes, loop x" +
                                String(g_repeats) + ", SD:" + g_sdMode + ")")
                             : "SD書き出し失敗";
            break;
          }
        }
        drawTransport(); drawFooter();
        if (g_mode == MODE_RHYTHM) drawSteps(true);
      }
    } else if (g_mode == MODE_RHYTHM) {
      if (ty >= 156 && ty < 404) {  // パッド
        int col = (tx - 20) / 246, row = (ty - 156) / 124;
        if (col >= 0 && col < 5 && tx - 20 - col * 246 < 236 && (ty - 156) % 124 < 114) {
          hitDrum(row * 5 + col);
          drawPads(); drawGridHeader(); drawSteps(true); drawFooter();
          if (!(g_running && g_recording)) {
            g_msg = "パッド選択: " + String(DRUM_NAMES[g_selDrum]) + " (記録は REC 中のみ)";
            drawFooter();
          }
        }
      } else if (ty >= 416 && ty < 464) {  // グリッドヘッダ
        if (tx >= 330 && tx < 634) {
          int b = (tx - 330) / 76;
          if (b >= 0 && b < 4 && (tx - 330) % 76 < 66) { g_gridBar = b; drawGridHeader(); drawSteps(true); }
        } else if (tx >= 660 && tx < 840) {
          undoLastDrum(); g_msg = "最後のドラム音を取り消しました"; drawSteps(true); drawFooter();
        } else if (tx >= 856 && tx < 1240) {
          clearDrums(); g_msg = "ドラムを全て消しました"; drawSteps(true); drawFooter();
        }
      } else if (ty >= 484 && ty < 614) {  // ステップセル
        int c = (tx - 20) / 77;
        if (c >= 0 && c < 16 && tx - 20 - c * 77 < 70) {
          toggleStep(g_gridBar * 16 + c);
          drawSteps(true); drawFooter();
        }
      }
    } else if (g_mode == MODE_MELODY) {
      if (ty >= 150 && ty < 196) {  // サブバー
        if (tx >= 20 && tx < 160) { g_melView = 0; drawMelody(); }
        else if (tx >= 170 && tx < 310) { g_melView = 1; drawMelody(); }
        else if (tx >= 320 && tx < 460) { g_melView = 2; fitGridWindow(); drawMelody(); }
        else if (tx >= 880 && tx < 1010) { undoLastMelody(); g_msg = "最後のメロディ音を取り消しました"; drawFooter(); }
        else if (tx >= 1020 && tx < 1240) { clearMelody(); g_msg = "メロディを全て消しました"; drawFooter(); }
      } else if (g_melView == 2) {  // グリッド編集
        if (ty >= GRID_HDR_Y && ty < GRID_HDR_Y + 44) {
          if (tx >= GRID_CELL_X && tx < GRID_CELL_X + 4 * 68) {
            int b = (tx - GRID_CELL_X) / 68;
            if ((tx - GRID_CELL_X) % 68 < 60) { g_gridBar = b; drawMelGridHeader(); drawMelGridRows(); }
          } else if (tx >= 420 && tx < 590) {
            g_gridLenIdx = (g_gridLenIdx + 1) % 4;
            g_msg = "置く音の長さ: " + String(GRID_LEN_NAMES[g_gridLenIdx]);
            drawMelGridHeader(); drawFooter();
          } else if (tx >= 610 && tx < 680) {
            if (g_gridTop > 47) { g_gridTop--; drawMelGridRows(); }
          } else if (tx >= 690 && tx < 760) {
            if (g_gridTop < 96) { g_gridTop++; drawMelGridRows(); }
          } else if (tx >= 880 && tx < 1050) {
            g_gridTop = max(47, g_gridTop - 12); drawMelGridRows();
          } else if (tx >= 1060 && tx < 1240) {
            g_gridTop = min(96, g_gridTop + 12); drawMelGridRows();
          }
        } else if (ty >= GRID_Y && ty < GRID_Y + 12 * GRID_ROW_H && tx >= GRID_CELL_X) {
          int col = (tx - GRID_CELL_X) / GRID_CELL_STEP;
          int row = (ty - GRID_Y) / GRID_ROW_H;
          if (col >= 0 && col < 16 && (tx - GRID_CELL_X) % GRID_CELL_STEP < GRID_CELL_W &&
              row >= 0 && row < 12) {
            toggleMelStep(g_gridTop - row, g_gridBar * 16 + col);
            drawMelGridRows(); drawFooter();
          }
        }
      } else if (g_melView == 1) {  // 音色リスト
        if (ty >= 600 && ty < 636) {
          if (tx >= 20 && tx < 220) g_sndPage = (g_sndPage + 3) % 4;
          else if (tx >= 1060 && tx < 1260) g_sndPage = (g_sndPage + 1) % 4;
          drawSoundList();
        } else if (ty >= 206 && ty < 590) {
          int col = (tx >= 650) ? 1 : 0;
          int lx = (col == 0) ? 20 : 650;
          if (tx >= lx && tx < lx + 610) {
            int row = (ty - 206) / 24;
            if (row >= 0 && row < 16) {
              selectProgram(g_sndPage * 32 + col * 16 + row);
              drawMelSubBar(); drawSoundList(); drawFooter();
            }
          }
        }
      } else {  // キーボード画面のオクターブボタン (鍵盤自体は handleMelodyPoints)
        if (ty >= STRIP_Y && ty < STRIP_Y + STRIP_H) {
          if (tx >= 20 && tx < 80) {
            g_keyWin = max(0, g_keyWin - 7);
            drawKeyboard(); drawStrip();
          } else if (tx >= 1200 && tx < 1260) {
            g_keyWin = min(36 - VIS_WHITES, g_keyWin + 7);
            drawKeyboard(); drawStrip();
          }
        }
      }
    } else if (g_mode == MODE_EDIT) {
      if (ty >= 152 && ty < 192 && tx >= 1000 && tx < 1240) {  // RESET PARAMS
        resetParams();
        g_msg = "パラメータを初期値に戻しました";
        drawEdit(); drawFooter();
      } else if (ty >= ROW_Y0 && ty < ROW_Y0 + 6 * ROW_H) {
        int row = (ty - ROW_Y0) / ROW_H;
        if (tx >= 440 && tx < 530) adjustParam(row, -1);
        else if (tx >= 790 && tx < 880) adjustParam(row, +1);
      }
    }
  }

  // グリッド配置時の試聴音を止める
  if (g_audNote >= 0 && (int32_t)(millis() - g_audOffAt) >= 0) {
    synth.setNoteOff(MELODY_CH, (uint8_t)g_audNote, 0);
    g_audNote = -1;
  }

  // メロディ鍵盤のマルチタッチ処理 (毎フレーム)
  if (g_mode == MODE_MELODY && g_melView == 0) handleMelodyPoints();

  // 再生中のステップハイライト更新
  if (g_running && g_mode == MODE_RHYTHM) drawSteps(false);
  if (g_running && g_mode == MODE_MELODY && g_melView == 2) {
    int s = (int)(posTickRaw() / (PPQ / 4));
    if (s != g_lastMelStepShown) { g_lastMelStepShown = s; drawMelGridRows(); }
  }

  delay(5);
}
