#pragma once

#define IR_JPEG ("/tmp/ir.jpeg")
#define RGB_JPEG ("/tmp/rgb.jpeg")

//разрешение картинок для нейронки = 720*1280
#define NN_W (360UL *2)
#define NN_H (640UL *2)
// w=360 h=640 (16xN) - поднял вдвое для

#define NN_RAT ((NN_W * 1.0f) / (NN_H * 1.0f))
#define NN_VECTOR_LEN (512)
//#define NN_SCAN_PERIOD (25)
#define FR_DROP_MS (100)
// - это время между кадрами, дольше которого они не синхронны
#define NN_CONFIG_CHECK_PERIOD (5000)
#define CONFIG_FILE ("/home/percomedia.json")
#define DUMP_FILE ("/home/percomedia_dump.json")

#define VO_IN_W (800)
#define VO_IN_H (1280)
#define VO_DISP_W (800)
#define VO_DISP_H (1280)

#define CAM_RGB_W (1920)
#define CAM_RGB_H (1080)

#define CAM_IR_W (1920)
#define CAM_IR_H (1080)

#define ENC_JPEG_W (1920)
#define ENC_JPEG_H (1080)

#define FPS_IR_IN (20)
#define FPS_RGB_IN (20)

#define FPS_IR_OUT (10)
#define FPS_RGB_OUT (1)

enum UserState{
    CHECK, CHECK_ERR, NN, OK
};
enum PassState{
    PASS_INIT, PASS_ERR, PASS_OK, PASS_WAIT
};

namespace isp {
typedef struct ISPConfig {
  unsigned char display_ch;
  bool ir_auto;
  bool rgb_auto;
  int rga_vo_angle;
  int rga_nn_angle;
  bool dbg_en;
  bool hdr_mode;
  int manual_shutter_ir;
  int manual_shutter_rgb;
  ISPConfig() {
    display_ch = 0;
    ir_auto = true;
    rgb_auto = true;
    dbg_en = true;
    rga_vo_angle = 270;
    rga_nn_angle = 90;
    manual_shutter_ir = 3;
    manual_shutter_rgb = 8;
    hdr_mode = false;

  }
  void Load();
} t_isp;

bool loadConf(t_isp &data);
} // namespace isp

namespace nn {
typedef struct NNConf {
public:
  bool rgb_imagequal;
  float framediff_max;
  float facescore_min;
  float livescore_min;
  unsigned int min_size_px2;
  float max_rect_diff;
  float max_pitch;
  float min_pitch;
  float max_yaw;
  float min_yaw;
  float max_roll;
  float min_roll;
  float rgb_blur_min;
  float rgb_blur_max;
  float ir_blur_min;
  float ir_blur_max;
  unsigned int dbg_en;
  bool photo_en;
  int photo_h;
  int photo_w;
  float photo_margin;
  NNConf() {
    rgb_imagequal = true;
    framediff_max = 0.1f;
    facescore_min = 0.9f;
    livescore_min = 0.04f;
    min_size_px2 = 20'000;
    max_rect_diff = 0.5;
    // Pitch angle ( < 0: Up, > 0: Down )
    // Yaw angle ( < 0: Left, > 0: Right )
    // Roll angle ( < 0: Right, > 0: Left )
    max_pitch = 50.0;
    min_pitch = -50.0;

    max_yaw = 20.0;
    min_yaw = -20.0;

    max_roll = 30.0;
    min_roll = -30.0;

    rgb_blur_min = 0.0;
    rgb_blur_max = 0.3;

    ir_blur_min = 0.0;
    ir_blur_max = 0.3;

    dbg_en = 0;

    photo_en = true;
    photo_h = 300;
    photo_w = 300;
    photo_margin = 1.5;
  }
  void Load();
} t_nn;

bool loadConf(t_nn &data);
} // namespace nn


namespace thrctl {
typedef struct ThrCtrlConfig {
    int scan_period_ms;
    int frame_drop_ms;
    ThrCtrlConfig() {
        scan_period_ms = 100;
        frame_drop_ms = 100;
    }
    void Load();
} t_thrctrl;

bool loadConf(t_thrctrl &data);
}

void saveDefConfig();
