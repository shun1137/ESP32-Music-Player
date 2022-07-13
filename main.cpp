#include <Arduino.h>

#define LGFX_USE_V1

#include <SD.h>
#include <LovyanGFX.hpp>
#include <U8g2_for_LovyanGFX.h>

#include <AudioFileSourceSD.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

#define X_PIXEL 128
#define Y_PIXEL 64

#define PREV 14
#define PLAY 26
#define NEXT 27
#define BACK 13

#define VOL_UP 16
#define VOL_DOWN 17
#define INIT_VOLUME 0.5
#define MAX_VOL 0.5

#define I2S_DOUT 32
#define I2S_BCLK 33
#define I2S_LRC 25

#define EXTERNAL_I2S 0

#define ROOT 0

#define N_BUF 50
#define N_DIR 15

#define ICON_WIDTH 14
#define SEL_LINE_HEIGHT 13

#define MPEGFRAME_HEADER_SIZE 4
#define ID3v1_SIZE 128

#define FONT_SELECT &helvR08_tf

#define swap(type, x, y) do { type t = x; x = y; y = t; } while (0)

/**********************************
 *   LovyanGFX ディスプレイ設定
 **********************************/
class LGFX_SSD1306 : public lgfx::LGFX_Device {
  lgfx::Panel_SSD1306   _panel_instance;
  lgfx::Bus_I2C   _bus_instance;

  public:
    LGFX_SSD1306() {
      {
        auto cfg = _bus_instance.config();
        cfg.i2c_port    = 1;                      //! 使用するI2Cポート (0 or 1)
        cfg.freq_write  = 400000;                 //! 送信時クロック
        cfg.freq_read   = 400000;                 //! 受信時クロック
        cfg.pin_sda     = 21;                     //! SDAピン番号
        cfg.pin_scl     = 22;                     //! SCLピン番号
        cfg.i2c_addr    = 0x3C;                   //! I2Cデバイスのアドレス

        _bus_instance.config(cfg);                //! 設定値をバスに反映
        _panel_instance.setBus(&_bus_instance);   //! バスをパネルにセット
      }
      {
        auto cfg = _panel_instance.config();      //! 表示パネル設定用の構造体を取得します。
        cfg.memory_width  = 128;                  //! 最大の幅
        cfg.memory_height =  64;                  //! 最大の高さ

        _panel_instance.config(cfg);              //! 設定をパネルに反映
      }
      setPanel(&_panel_instance);                 //! 使用するパネルをセット
    }
};

/**********************************
 *         列挙型・構造体
 **********************************/

/** 再生モード */
enum Mode {
  normal,                       //!< 通常
  repeat,                       //!< リピート
  shuffle                       //!< シャッフル
};

/** ボタン入力 */
enum Button {
  prev,                         //!< 前
  next,                         //!< 次
  play,                         //!< 再生・決定
  back                          //!< 戻る
};

enum Btn_Status {
  Release,
  ON_start,
  momentPress_determined,
  longPress_determined,
  continuous_press
};

struct Status {
  float volume = INIT_VOLUME;
  enum Mode mode = normal;      //!< 通常:normal / リピート:repeat / シャッフル:shuffle
  bool pause = false;
};

/** 再生中ID3タグ情報 */
struct ID3tag {
  String Album;                 //!< アルバム
  String Title;                 //!< タイトル
  String Performer;             //!< アーティスト
  double Time;                  //!< 長さ
};

/** ディレクトリ移動履歴 */
struct Dir {
  String path;                  //!< パス
  uint8_t numSelectFile = 0;    //!< 選択したファイル番号 (開始0/上から)
  uint8_t totalFileCount = 0;   //!< ディレクトリ内のディレクトリを含むファイル数 (開始1)
  uint8_t dirCount = 0;         //!< ディレクトリ内のディレクトリ数 (開始1)
};

/** ファイルリストバッファ */
struct Buffer {
  String filename;              //!< ファイル名 (ディレクトリを含む)
  bool isDir;                   //!< ディレクトリであるか
};

#pragma pack(1)                 // 境界調整(パディングなし)
/** ID3v2ヘッダ */
struct ID3v2Header {
    char tag[3];                //!< ヘッダ識別子
    uint8_t maj_ver;            //!< バージョン
    uint8_t min_ver;            //!< バージョン
    uint8_t flags;              //!< フラグ
    uint8_t size[4];            //!< サイズ(ID3v2ヘッダ以降のID3v2タグのサイズ)
                                // uint32_tではESP32の使用上read()時にリトルエンディアンで書き込んでしまう様なのでuint8_t[4]とした
};
/** ID3v2拡張ヘッダ */
struct ID3v2ExtHeader {
    uint8_t size[4];            //!< 拡張ヘッダのサイズ
    uint8_t num_flag_bytes;   
    uint8_t extended_flags;
};
#pragma pack()

#pragma pack(1)
/** ID3v2フレーム */
struct ID3v2Frame {
    char frame_id[4];           //!< フレームID
    uint8_t size[4];            //!< フレームヘッダ10バイト(v2.3以降)を除いたフレームのサイズ
    uint8_t flags[2];           //!< フラグ
    uint8_t encoding;           //!< テキストのエンコーディング
    //String data;                //!< フレーム内容
};
#pragma pack()

#pragma pack(1)
/** Xingヘッダ */
struct XingHeader {
  char tag[4];                  //!< ヘッダ識別子
  uint8_t flags[4];             //!< フラグ
  uint8_t num_flames[4];        //!< フレーム数
  uint8_t filesize[4];          //!< MP3ファイルサイズ
  uint8_t toc[100];             //!< TOCエントリ
  uint8_t quality[4];           //!< Quality Indicator
};
#pragma pack()

/** MPEGフレームヘッダ */
struct MPEGFrameHeader {
  uint16_t bitrate;             //!< ビットレート
  uint16_t sampling_rate;       //!< サンプリングレート
  uint8_t padding_bit;          //!< パディングビット
  uint8_t channel;              //!< チャンネル
};

/**********************************
 *       関数プロトタイプ宣言
 **********************************/



/**********************************
 *       各種宣言(グローバル)
 **********************************/

static LGFX_SSD1306 display;
static LGFX_Sprite canvas(&display);
static LGFX_Sprite canvas2;
static LGFX_Sprite menu_icon;
static LGFX_Sprite menu_name;
static LGFX_Sprite playback_title;

AudioGeneratorMP3 *mp3;
AudioFileSourceSD *source;
AudioOutputI2S *out;
AudioFileSourceID3 *id3;
uint8_t *subscript;

ID3tag nowPlaying;                   //!< 再生中ID3v2タグ情報
Status status;
struct MPEGFrameHeader mFrameHeader;

bool ID3flag = false;                //!< ID3取得完了時 true

uint32_t startTime_prev = 0;
uint32_t startTime_next = 0;
uint32_t startTime_play = 0;
uint32_t startTime_back = 0;
uint32_t startTime_volup = 0;
uint32_t startTime_voldown = 0;

enum Btn_Status prev_status = Release;
enum Btn_Status next_status = Release;
enum Btn_Status play_status = Release;
enum Btn_Status back_status = Release;
enum Btn_Status volup_status = Release;
enum Btn_Status voldown_status = Release;

/**********************************
 *              関数
 **********************************/

uint32_t timeMeasure(uint32_t st_time)
{
  return millis() - st_time;
}

uint8_t pushButton(const uint8_t gpio, Btn_Status *button_status, uint32_t *start_time, boolean continuous_set, uint32_t chatter_time, uint32_t long_press_time)
{
  uint8_t ret_state = Release;
  switch (digitalRead(gpio)) {
    case LOW:
      switch (*button_status) {
        case Release:
          *button_status = ON_start;
          *start_time = millis();         //start_timeをリセット
          break;
        case ON_start:
          if (continuous_set) {             
            if (timeMeasure(*start_time) > long_press_time) {
              *button_status = continuous_press;
            }
          } else {
            if (timeMeasure(*start_time) > long_press_time) {
              *button_status = longPress_determined;
              ret_state = longPress_determined;
            }
          }
          break;
        case continuous_press:
          if (timeMeasure(*start_time) > long_press_time) {
            *start_time = millis();       //start_timeをリセット
            return continuous_press;
          }
          break;
        default:
          break;
      }
      break;
    case HIGH:
      if (*button_status == ON_start) {
        if (timeMeasure(*start_time) > chatter_time) {
          ret_state = momentPress_determined;
        }
      }
      *button_status = Release;
      break;
    default:
      break;
  }
  return ret_state;
}

void setVol(uint8_t volUp)
{
  if (volUp) {
    status.volume += 0.01;
    if (status.volume > MAX_VOL) {
      status.volume = MAX_VOL;
    }
  }  else {
    status.volume -= 0.01;
    if (status.volume < 0.01) {
      status.volume = 0.01;
    }
  }

  out->SetGain(status.volume);
  Serial.println(status.volume);
}

boolean isDirectoryHideSys(File file)
{
  String dirname = String(file.name());
  if (dirname.equals("System Volume Information")) {
    return false;
  }
  return file.isDirectory();
}

boolean isSupportedFormat(const String filename)
{
  if (filename.endsWith(".mp3")) {
    return true;
  }
  //if (filename.endsWith(".wav")) {
  //  return true;
  //}
  //if (filename.endsWith(".flac")) {
  //  return true;
  //}
  return false;
}

boolean isSupportedFormat(File file)
{
  const String filename = String(file.name());
  return isSupportedFormat(filename);
}

void clearBuffer(struct Buffer *buf, uint8_t nArray)
{
  for (uint8_t i = 0; i < nArray; i++) {
    (buf + i)->filename.remove(0);
    (buf + i)->isDir = false;
  }
}

void clearDir(struct Dir *dir)
{
  dir->path.clear();
  dir->numSelectFile = 0;
  dir->totalFileCount = 0;
  dir->dirCount = 0;
}

void initDirBuffer(File file, struct Dir *dir, struct Buffer *buf)
{
  uint8_t fileCount = 0;
  uint8_t dirCount = 0;
  clearBuffer(buf, N_BUF);
  for (uint8_t i = 0; i < 2; i++) {
    while (true) {
      File entry = file.openNextFile();

      if (!entry) {
        file.rewindDirectory();
        break;
      }
      
      if (i < 1) {
        if (isDirectoryHideSys(entry)) {
          (buf + fileCount)->isDir = true;
          dirCount++;
        } else {
          entry.close();
          continue;
        }
      } else {
        if (isSupportedFormat(entry)) {
          (buf + fileCount)->isDir = false;
        } else {
          entry.close();
          continue;
        }
      }
      
      (buf + fileCount)->filename = String(entry.name());
      entry.close();
      fileCount++;
    }
  }

  dir->dirCount = dirCount;
  dir->totalFileCount = fileCount;
}

void printIcon(lgfx::v1::LovyanGFX *dst, int color, struct Buffer *buf, uint8_t pos) {
  LGFX_Sprite icon;

  icon.createSprite(SEL_LINE_HEIGHT, ICON_WIDTH);
  icon.fillSprite((color == TFT_BLACK) ? TFT_WHITE : TFT_BLACK);
  icon.setTextDatum(top_left);
  icon.setCursor(0, 1);
  icon.setFont(&siji_t_6x10);
  icon.setTextColor(color);

  if (buf->isDir) {
    icon.print("");
  } else {
    icon.print("");
  }

  icon.pushSprite(dst, 0, pos);
  icon.deleteSprite();
}

int32_t printFile(lgfx::v1::LovyanGFX *dst, int color, struct Buffer *buf, uint8_t pos) {
  LGFX_Sprite filename;
  int32_t cursor_x;

  filename.createSprite(1000, SEL_LINE_HEIGHT);
  filename.fillSprite((color == TFT_BLACK) ? TFT_WHITE : TFT_BLACK);
  filename.setTextDatum(top_left);
  filename.setCursor(0, 0);
  filename.setFont(FONT_SELECT);
  filename.setTextColor(color);
  filename.setTextWrap(false);
  filename.print(buf->filename);

  cursor_x = filename.getCursorX();

  filename.pushSprite(dst, 0, pos);
  filename.deleteSprite();

  return cursor_x;
}

void printDirectory(struct Buffer *buf, uint8_t pos)
{
  menu_icon.createSprite(ICON_WIDTH, display.height());
  menu_name.createSprite(display.width() - ICON_WIDTH, display.height());
  menu_icon.clear(TFT_BLACK);
  menu_name.clear(TFT_BLACK);

  for (uint8_t i = 0; i < 5; i++) {
    printIcon(&menu_icon, TFT_WHITE, &buf[pos], SEL_LINE_HEIGHT * i);

    printFile(&menu_name, TFT_WHITE, &buf[pos++], SEL_LINE_HEIGHT * i);

    if (buf[pos].filename == NULL) {
      break;
    }
  }
  menu_icon.pushSprite(&canvas, 0, 0);
  menu_name.pushSprite(&canvas, ICON_WIDTH - 1, 0);

  menu_icon.deleteSprite();
  menu_name.deleteSprite();
}

void invertRect(uint8_t x, uint8_t y, uint8_t width, uint8_t height)
{
  std::uint16_t buffer[width];
  for (uint16_t i = y ; i < y + height; i++) {
    display.readRect(x, i, width, 1, buffer);
    for (uint16_t j = 0; j < width; j++) {
      buffer[j] ^= 0xFFFF;
    }
    canvas.pushImage(x, i, width, 1, buffer);
  }
}

void invertYRect(uint8_t y, uint8_t height)
{
  invertRect(0, y, display.width(), height);
}

void invertLine(uint8_t pos)
{
  invertYRect(SEL_LINE_HEIGHT * pos, SEL_LINE_HEIGHT);
  canvas.pushSprite(0, 0);
}

enum Button filenameScroll(struct Buffer *buf, uint8_t displaypos, uint8_t filepos)
{
  enum Button push;
  int scrollPixel = 0;

  menu_icon.createSprite(ICON_WIDTH, SEL_LINE_HEIGHT);
  menu_name.createSprite(1000, SEL_LINE_HEIGHT);

  printIcon(&menu_icon, TFT_BLACK, &buf[filepos], 0);
  menu_icon.pushSprite(&canvas, 0, SEL_LINE_HEIGHT * displaypos);

  int32_t text_size = printFile(&menu_name, TFT_BLACK, &buf[filepos], 0);

  if (text_size > display.width() - ICON_WIDTH) {
    menu_name.setScrollRect(0, 0, text_size * 2 + 20, SEL_LINE_HEIGHT, TFT_WHITE);
  }
    
  while (1) {
    menu_name.pushSprite(&canvas, ICON_WIDTH - 1, SEL_LINE_HEIGHT * displaypos);
    canvas.pushSprite(0, 0);
    
    if (text_size > display.width() - ICON_WIDTH) {
      delay(100);

      if (scrollPixel <= 0) {
        scrollPixel = text_size + 20;
        menu_name.setCursor(text_size + 20, 0);
        menu_name.setFont(FONT_SELECT);
        menu_name.setTextDatum(top_left);
        menu_name.setTextColor(TFT_BLACK);
        menu_name.setTextWrap(false);
        menu_name.print(buf[filepos].filename);
      }

      menu_name.scroll(-2, 0);
      scrollPixel -= 2;
    }

    uint8_t prev_state = pushButton(PREV, &prev_status, &startTime_prev, false, 10, 2000);
    if (prev_state == momentPress_determined) {
      menu_icon.fillSprite(TFT_BLACK);
      printIcon(&menu_icon, TFT_WHITE, &buf[filepos], 0);
      menu_icon.pushSprite(&canvas, 0, SEL_LINE_HEIGHT * displaypos);

      menu_name.fillSprite(TFT_BLACK);
      printFile(&menu_name, TFT_WHITE, &buf[filepos], 0);
      menu_name.pushSprite(&canvas, ICON_WIDTH, SEL_LINE_HEIGHT * displaypos);

      canvas.pushSprite(0, 0);
      
      push = prev;
      break;
    }

    uint8_t next_state = pushButton(NEXT, &next_status, &startTime_next, false, 10, 2000);
    if (next_state == momentPress_determined) {
      menu_icon.fillSprite(TFT_BLACK);
      printIcon(&menu_icon, TFT_WHITE, &buf[filepos], 0);
      menu_icon.pushSprite(&canvas, 0, SEL_LINE_HEIGHT * displaypos);

      menu_name.fillSprite(TFT_BLACK);
      printFile(&menu_name, TFT_WHITE, &buf[filepos], 0);
      menu_name.pushSprite(&canvas, ICON_WIDTH, SEL_LINE_HEIGHT * displaypos);

      canvas.pushSprite(0, 0);

      push = next;
      break;
    }

    uint8_t play_state = pushButton(PLAY, &play_status, &startTime_play, false, 10, 500);
    if (play_state == momentPress_determined) {
      push = play;
      break;
    }

    uint8_t back_state = pushButton(BACK, &back_status, &startTime_back, false, 10, 500);
    if (back_state == momentPress_determined) {
      push = back;
      break;
    }

    uint8_t volup_state = pushButton(VOL_UP, &volup_status, &startTime_volup, true, 10, 500);
    if (volup_state == momentPress_determined || volup_state == continuous_press) {
      setVol(1);
    }

    uint8_t voldown_state = pushButton(VOL_DOWN, &voldown_status, &startTime_voldown, true, 10, 500);
    if (voldown_state == momentPress_determined || voldown_state == continuous_press) {
      setVol(0);
    }
  }

  menu_icon.deleteSprite();
  menu_name.deleteSprite();

  return push;
}

uint8_t select(File root, struct Dir *dir, struct Buffer *buf, uint8_t level)
{
  while (1) {
    canvas.clear(TFT_BLACK);

    initDirBuffer(root, dir + level, buf);
    uint8_t selectNum = (dir + level)->numSelectFile;
    int8_t position;

    if (selectNum >= 0 && selectNum <= 2) {
      printDirectory(buf, 0);
      position = selectNum;
    } else if (selectNum <= (dir + level)->totalFileCount - 1 && selectNum >= (dir + level)->totalFileCount - 3) {
      printDirectory(buf, (dir + level)->totalFileCount - 5);
      position = 4 - (((dir + level)->totalFileCount - 1) - selectNum);
    } else {
      printDirectory(buf, selectNum - 2);
      position = 2;
    }

    if ((dir + level)->totalFileCount <= 0) {
      canvas.clear(TFT_BLACK);
      canvas.setFont(&b12_t_japanese2);
      canvas.setTextDatum(middle_center);
      canvas.drawString("ファイルがありません", 64, 32); 
      canvas.pushSprite(0, 0);

      while (1) {
          if (digitalRead(BACK) == LOW && level > 0) {
              delay(100);
              (dir + level)->path.clear();
              root.close();
              root = SD.open((dir + (--level))->path);
              break;
          }
      }
      continue;
    }

    while (1) {
      enum Button push = filenameScroll(buf, position, selectNum);

      if (push == prev) {
        if ((dir + level)->totalFileCount != 1) {
          if (position > 0) {
            position--;
            selectNum--;
          } else {
            if (selectNum <= 0) {
              selectNum = (dir + level)->totalFileCount - 1;
              if ((dir + level)->totalFileCount < 5) {
                position = (dir + level)->totalFileCount - 1;
              } else {
                position = 4;
                canvas.clear(TFT_BLACK);
                printDirectory(buf, selectNum - 4);
              }
            } else {
              canvas.clear(TFT_BLACK);
              printDirectory(buf, selectNum - 1);
              selectNum--;
            }
          }
        }
      }

      if (push == next) {
        if ((dir + level)->totalFileCount != 1) {
          if (position < 4) {
            if (selectNum >= (dir + level)->totalFileCount - 1) {
              selectNum = 0;
              position = 0;
            } else {
              position++;
              selectNum++;
            }
          } else {
            if (selectNum >= (dir + level)->totalFileCount - 1) {
              selectNum = 0;
              position = 0;
              canvas.clear(TFT_BLACK);
              printDirectory(buf, selectNum);
            } else {
              canvas.clear(TFT_BLACK);
              printDirectory(buf, selectNum - 3);
              selectNum++;
            }
          }
        }
      }

      if (push == play) {
        (dir + level)->numSelectFile = selectNum;
        (dir + level + 1)->path.clear();

        if (level > 0) {
          (dir + level + 1)->path = String((dir + level)->path + "/");
          (dir + level + 1)->path.concat((buf + selectNum)->filename);
        } else {
          (dir + level + 1)->path = String("/" + (buf + selectNum)->filename);
        }

        root.close();
        root = SD.open((dir + level + 1)->path);
        if (root.isDirectory()) {
          level++;
        }
        break;
      }

      if (push == back && level > 0) {
          clearDir(dir + level);
          root.close();
          root = SD.open((dir + (--level))->path);
          break;
      }
    }

    if (!root.isDirectory()) {
      root.close();
      break;
    }
  }
  return level;
}

uint8_t countLatestDir(const struct Dir *dir)
{
  uint8_t count = 0;
  while (dir[count].path != NULL) {
    count++;
  }
  return count - 2;
}

size_t getTagData(File file)
{
  uint16_t bitrate[16] = {
    0, 32, 40, 48, 56, 64, 80, 96,
    112, 128, 160, 192, 224, 256, 320, 0
  };

  uint16_t sampling_rate[4] = {
    44100, 48000, 32000, 0
  };

  uint16_t posBuff;
  int tagpos = 0;

  if (!file) {
    Serial.println("File could not open.");
    return 0;
  }

  // ID3v2ヘッダ 読込
  struct ID3v2Header header = {0};
  uint8_t buff[4];

  posBuff = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
  if (posBuff == -1) {
    Serial.println("ID3v2 Header read failed.");
    return 0;
  }

  if (memcmp(header.tag, "ID3", 3) == 0) {
    tagpos += posBuff;

    //Syncsafe Integer->整数へ変換
    //ID3v2ヘッダ以降のID3v2タグのサイズ
    uint32_t ID3v2_size = (header.size[0] << 21) + (header.size[1] << 14) + (header.size[2] << 7) + header.size[3];

    // ID3v2 読込スキップ
    tagpos += ID3v2_size;
    file.seek(tagpos);

  } else {
    file.seek(0);
  }

  //MPEGフレームヘッダ 読込
  struct MPEGFrameHeader null_struct = {0};
  mFrameHeader = null_struct;

  uint8_t buff_mFrameHeader[4];
  if (file.read(buff_mFrameHeader, sizeof(buff_mFrameHeader)) == -1) {
    Serial.println("MPEG Frame Header read failed.");
    return 0;
  }
  uint32_t combine_mFrameHeader = (buff_mFrameHeader[0] << 24) + (buff_mFrameHeader[1] << 16) + (buff_mFrameHeader[2] << 8) + buff_mFrameHeader[3];

  combine_mFrameHeader >>= 6;
  if (!((combine_mFrameHeader >> 15) && 0x07FF)) {
    Serial.println("An unexpected error occurred while reading MPEG Frame Header. -1");
    return 0;
  }
  combine_mFrameHeader &= 0x03FF;

  mFrameHeader.channel = combine_mFrameHeader & 0x03;
  combine_mFrameHeader >>= 2;
  mFrameHeader.padding_bit = combine_mFrameHeader & 0x03;
  combine_mFrameHeader >>= 2;
  uint8_t samplingrateBit = combine_mFrameHeader & 0x03;
  combine_mFrameHeader >>= 2;
  uint8_t bitrateBit = combine_mFrameHeader & 0x0F;
  combine_mFrameHeader >>= 4;

  if (combine_mFrameHeader != 0) {
    Serial.println("An unexpected error occurred while reading MPEG Frame Header. -2");
    return -8;
  }

  mFrameHeader.bitrate = bitrate[bitrateBit];
  mFrameHeader.sampling_rate = sampling_rate[samplingrateBit];

  if (mFrameHeader.bitrate == 0) {
    Serial.println("Unsupported bitrate.");
    return 0;
  }

  if (mFrameHeader.sampling_rate == 0) {
    Serial.println("Unsupported sampling rate.");
    return 0;
  }
  
  tagpos += MPEGFRAME_HEADER_SIZE;

  size_t header_size = tagpos;
  size_t footer_size = 0;

  //ID3v1タグ確認
  file.seek(file.size() - ID3v1_SIZE);
  uint8_t tag[3];
  file.read(tag, sizeof(tag));
  if (memcmp(tag, "TAG", 3) == 0) {
    footer_size = ID3v1_SIZE;
  }

  return header_size + footer_size;
}

double getmp3TotalTime(const String path)
{
  File file = SD.open(path);

  size_t tag_size = getTagData(file);
  if (tag_size == 0) {
    return -1;
  }

  size_t mpeg_size = file.size() - tag_size;

  //時間算出
  size_t frame_size = 144 * (mFrameHeader.bitrate * 1000) / mFrameHeader.sampling_rate + mFrameHeader.padding_bit;
  double frame_count = mpeg_size / frame_size;

  double duration_sec = frame_count * (1152.0 / mFrameHeader.sampling_rate);

  file.close();

  return duration_sec;
}

String printDuration(double duration)
{
  if (duration < 0) {
    return String("--:--");
  }
  int min = (int)duration / 60;
  int sec = (int)duration - min * 60;

  char formatted_time[8];
  sprintf(formatted_time, "%d:%02d", min, sec);
  return String(formatted_time);
}

void screenPlayback(struct Dir *dir)
{
  canvas.clear(TFT_BLACK);
  
  playback_title.setTextWrap(false);
  canvas2.setTextWrap(false);

  //タイトル表示
  playback_title.createSprite(128, 16);
  playback_title.clear(TFT_WHITE);
  playback_title.setTextDatum(top_left);
  playback_title.setTextColor(TFT_BLACK);
  playback_title.setFont(&b16_t_japanese3);
  playback_title.setCursor(0, 0);

  if (nowPlaying.Title.isEmpty()) {
    playback_title.print((dir + 1)->path.substring((dir + 1)->path.lastIndexOf('/') + 1));
  } else {
    playback_title.print(nowPlaying.Title);
  }
  
  if (!nowPlaying.Performer.isEmpty()) {
    playback_title.print("/");
    playback_title.print(nowPlaying.Performer);
  }

  playback_title.pushSprite(&canvas, 0, 0);
  playback_title.deleteSprite();

  //総時間表示
  canvas.setFont(&_7x14B_tn);
  canvas.setTextDatum(bottom_left);
  canvas.setCursor(36, 48);
  canvas.print("--:--");              // 現在再生時間の表示予定地
  canvas.setFont(&_6x10_tn);
  canvas.setCursor(canvas.getCursorX() + 3, canvas.getCursorY());
  canvas.print("/");
  canvas.setCursor(canvas.getCursorX() + 3, canvas.getCursorY());
  canvas.print(printDuration(nowPlaying.Time));

  //モード表示
  canvas2.createSprite(16, 12);
  canvas2.clear(TFT_BLACK);
  canvas2.setTextColor(TFT_WHITE);
  canvas2.setTextDatum(top_left);
  
  char modeIcon[2];
  switch (status.mode) {
    case normal:
      strcpy(modeIcon, "\0");
      break;
    case shuffle:
      strcpy(modeIcon, "Y");
      break;
    case repeat:
      canvas2.setFont(&_6x12_tn);
      canvas2.drawString("1", 0, 0);
      strcpy(modeIcon, "V");
      break;
    default:
      strcpy(modeIcon, "\0");
      break;
  }
  canvas2.setFont(&open_iconic_arrow_1x_t);
  canvas2.drawString(modeIcon, 7, 3);
  canvas2.pushSprite(&canvas, 67, 17);
  canvas2.deleteSprite();

  //再生アイコン表示
  canvas2.createSprite(16, 16);
  canvas2.clear(TFT_BLACK);
  canvas2.setTextColor(TFT_WHITE);
  canvas2.setFont(&open_iconic_play_2x_t);
  char playStatus[2];
  if (status.pause) {
    strcpy(playStatus, "D");
  } else {
    strcpy(playStatus, "E");
  }
  canvas2.setTextDatum(top_left);
  canvas2.drawString(playStatus, 0, 0);
  canvas2.pushSprite(&canvas, 10, 33);
  canvas2.deleteSprite();

  //トラックナンバー表示
  canvas2.createSprite(42, 12);
  canvas2.clear(TFT_BLACK);
  canvas2.setTextColor(TFT_WHITE);
  canvas2.setCursor(0, 0);
  canvas2.setFont(&siji_t_6x10);
  canvas2.print("");
  canvas2.setFont(&_6x12_tr);
  canvas2.printf("%02d/%02d", (dir->numSelectFile + 1) - dir->dirCount, dir->totalFileCount - dir->dirCount);
  canvas2.pushSprite(&canvas, 0, 17);
  canvas2.deleteSprite();

  //ファイル種類表示
  canvas2.createSprite(43, 9);
  canvas2.clear(TFT_BLACK);
  canvas2.fillRoundRect(0, 0, 43, 9, 2, TFT_WHITE);
  canvas2.setTextColor(TFT_BLACK);
  canvas2.setCursor(3, -2);
  canvas2.setFont(&_6x12_tr);
  if ((dir + 1)->path.endsWith(".mp3")) {
    canvas2.print("MP3");
  } else if ((dir + 1)->path.endsWith(".wav")) {
    canvas2.print("WAV");
  } else {
    canvas2.print("N/A");
  }
  canvas2.setCursor(canvas2.getCursorX() + 2, canvas2.getCursorY());
  canvas2.printf("%3d", mFrameHeader.bitrate);
  canvas2.pushSprite(&canvas, 85, 19);

  canvas.pushSprite(0, 0);
}

void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string)
{
  (void)cbData;

  if (isUnicode) {
    string += 2;
  }
  
  while (*string) {
    char letter = *(string++);
    if (isUnicode) {
      string++;
    }
    if (strcmp(type, "Album") == 0) {
      nowPlaying.Album.concat(letter);
    } else if (strcmp(type, "Title") == 0) {
      nowPlaying.Title.concat(letter);
    } else if (strcmp(type, "Performer") == 0) {
      nowPlaying.Performer.concat(letter);
    } else if (strcmp(type, "eof") == 0) {
      ID3flag = true;
    } else {
      break;
    }
  }
}

void clearID3()
{
  nowPlaying.Album.clear();
  nowPlaying.Title.clear();
  nowPlaying.Performer.clear();
  nowPlaying.Time = 0;
}

void makeIndex(struct Dir *dir)
{
  uint8_t num = dir->totalFileCount;
  subscript = new uint8_t[num];

  for (uint8_t i = 0; i < num; i++) {
    subscript[i] = i;
  }
} 

void shuffleIndex(struct Dir *dir)
{
  int8_t num = dir->totalFileCount;
  randomSeed(199);

  for (int8_t i = num - 1; i >= 0; i--) {
    uint8_t j = random(num);
    if (i != j && i != dir->numSelectFile && j != dir->numSelectFile) {
      swap(uint8_t, subscript[i], subscript[j]);
    }
  }
}

void deleteIndex()
{
  delete subscript;
}

String getNextPath(struct Dir *dir, struct Buffer *buffer)
{
  int8_t select = dir->numSelectFile;
  String songPath;

  do {
    do {
      if (select >= dir->totalFileCount - 1) {
        select = 0;
      } else {
        select++;
      }
    } while ((buffer + subscript[select])->isDir);
    
    if (dir->path == "/") {
      songPath = String("/" + buffer[subscript[select]].filename);
    } else {
      songPath = String(dir->path + "/");
      songPath.concat(buffer[subscript[select]].filename);
    }
    
    dir->numSelectFile = select;
  } while (!isSupportedFormat(songPath));

  delay(100);
  return songPath;
}

String getPrevPath(struct Dir *dir, struct Buffer *buffer)
{
  int8_t select = dir->numSelectFile;
  String songPath;

  do {
    do {
      if (select < 0) {
        select = dir->totalFileCount - 1;
      } else {
        select--;
      }
    } while ((buffer + subscript[select])->isDir);

    if (dir->path == "/") {
      songPath = String("/" + buffer[subscript[select]].filename);
    } else {
      songPath = String(dir->path + "/");
      songPath.concat(buffer[subscript[select]].filename);
    }

    dir->numSelectFile = select;
  } while (!isSupportedFormat(songPath));

  delay(100);
  return songPath;
}

void mp3Begin(const String filename)
{
  clearID3();
  source = new AudioFileSourceSD(filename.c_str());
  id3 = new AudioFileSourceID3(source);
  id3->RegisterMetadataCB(MDCallback, (void*)"ID3TAG");
  mp3 = new AudioGeneratorMP3();
  mp3->begin(id3, out);
  nowPlaying.Time = getmp3TotalTime(filename);
}

void mp3Stop() {
  mp3->stop();
  id3->close();

  delete mp3;
  delete id3;
  delete source;
}

void pause(bool *status)
{
  if (*status) {
    out->begin();
    *status = false;
  } else {
    out->stop();
    *status = true;
  }
}

void mp3Playback(struct Dir *dir, struct Buffer *buffer)
{
  status.pause = false;
  
  mp3Begin((dir + 1)->path);

  while (1) {
    if (mp3->isRunning()) {
      if (!mp3->loop()) {
        mp3Stop();
      }
    } else {
      switch (status.mode) {
        case normal:
        case shuffle:
          (dir + 1)->path = getNextPath(dir, buffer);
          mp3Begin((dir + 1)->path);
          break;
        case repeat:
          mp3Begin((dir + 1)->path);
          break;
        default:
          break;
      }
    }

    if (ID3flag == true) {
      screenPlayback(dir);
      ID3flag = false;
    }

    uint8_t back_state = pushButton(BACK, &back_status, &startTime_back, true, 10, 500);
    if (back_state == momentPress_determined) {
      mp3Stop();
      break;
    }
    if (back_state == continuous_press) {
      switch (status.mode) {
        case normal:
          status.mode = shuffle;
          shuffleIndex(dir);
          break;
        case shuffle:
          status.mode = repeat;
          break;
        case repeat:
          status.mode = normal;
          deleteIndex();
          makeIndex(dir);
          break;
        default:
          break;
      }
      screenPlayback(dir);
    }

    uint8_t play_state = pushButton(PLAY, &play_status, &startTime_play, false, 10, 2000);
    if (play_state == momentPress_determined) {
      pause(&status.pause);
      screenPlayback(dir);
    }

    uint8_t volup_state = pushButton(VOL_UP, &volup_status, &startTime_volup, true, 10, 500);
    if (volup_state == momentPress_determined || volup_state == continuous_press) {
      setVol(1);
    }

    uint8_t voldown_state = pushButton(VOL_DOWN, &voldown_status, &startTime_voldown, true, 10, 500);
    if (voldown_state == momentPress_determined || voldown_state == continuous_press) {
      setVol(0);
    }
    
    uint8_t next_state = pushButton(NEXT, &next_status, &startTime_next, false, 10, 2000);
    if (next_state == momentPress_determined) {
      mp3Stop();
      (dir + 1)->path = getNextPath(dir, buffer);
      mp3Begin((dir + 1)->path);
      status.pause = false;
    }

    uint8_t prev_state = pushButton(PREV, &prev_status, &startTime_prev, false, 10, 2000);
    if (prev_state == momentPress_determined) {
      mp3Stop();
      (dir + 1)->path = getPrevPath(dir, buffer);
      mp3Begin((dir + 1)->path);
      status.pause = false;
    }
  }
}

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);

  pinMode(PREV, INPUT_PULLUP);
  pinMode(PLAY, INPUT_PULLUP);
  pinMode(NEXT, INPUT_PULLUP);
  pinMode(BACK, INPUT_PULLUP);
  pinMode(VOL_UP, INPUT_PULLUP);
  pinMode(VOL_DOWN, INPUT_PULLUP);

  audioLogger = &Serial;
  out = new AudioOutputI2S(I2S_NUM_0, EXTERNAL_I2S);
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(status.volume);
  out->begin();

  display.init();
  canvas.setTextWrap(false);            // 右端到達時のカーソル折り返しを禁止
  canvas.createSprite(display.width(), display.height());

  canvas.fillScreen(TFT_BLACK);
  canvas.setTextColor(TFT_WHITE);
  
  if (!SD.begin()) {
    canvas.clear(TFT_BLACK);
    canvas.setCursor(0, 0);
    canvas.setFont(&b10_t_japanese2);
    canvas.setTextDatum(middle_center);
    canvas.drawString("カードを挿入してください", 64, 32);
    canvas.pushSprite(0, 0);
    
    while (1) {
      if (SD.begin()) {
        break;
      }
    }
  }
}

void loop()
{
  // put your main code here, to run repeatedly:
  struct Dir directory[N_DIR];
  struct Buffer buffer[N_BUF];
  
  directory[ROOT].path = String("/");
  File file_instance = SD.open("/");
  uint8_t level = ROOT;

  while (1) {
    level = select(file_instance, directory, buffer, level);
    file_instance.close();

    makeIndex(&directory[level]);
    if (directory[level + 1].path.endsWith(".mp3")) {
      mp3Playback(&directory[level], buffer);
    }

    file_instance = SD.open(directory[level].path);
  }
}
