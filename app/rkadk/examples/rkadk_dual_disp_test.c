/*
 * Copyright (c) 2021 Rockchip, Inc. All Rights Reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Dual camera display test for RV1106/RV1103B.
 *
 * Simultaneously displays two cameras (CamId=0 and CamId=1) in two
 * small 256xN windows on the VO video layer.  The window height is
 * calculated from the camera frame aspect ratio so the image keeps
 * its proportions ("256 proportional").
 *
 * Pipeline per camera:  VI -> VPSS -> VO(layer 0)
 *   CamId 0 -> vo_chn 0, rect at (win0_x, win0_y)
 *   CamId 1 -> vo_chn 1, rect at (win1_x, win1_y)
 */

#include "rkadk_common.h"
#include "rkadk_media_comm.h"
#include "rkadk_disp.h"
#include "rkadk_log.h"
#include "rkadk_param.h"
#include "isp/sample_isp.h"
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int optind;
extern char *optarg;

static bool is_quit = false;
static RKADK_CHAR optstr[] = "a:p:W:H:s:g:h";

#define IQ_FILE_PATH "/etc/iqfiles"

/* Default window size: 256 wide, height proportional to camera aspect */
#define DEFAULT_WIN_SIZE  256
#define DEFAULT_GAP       8  /* pixels between the two windows */

static void print_usage(const RKADK_CHAR *name) {
  printf("usage example:\n");
  printf("\t%s [-a /etc/iqfiles] [-p /data/rkadk] [-W 256] [-H 0] [-s 0] [-g 8]\n",
         name);
  printf("\t-a: enable aiq with dirpath, default /etc/iqfiles\n");
  printf("\t-p: param ini directory path, default /data/rkadk\n");
  printf("\t-W: window width  in pixels, default 256 (0 = use height instead)\n");
  printf("\t-H: window height in pixels, default 0 (0 = proportional to W)\n");
  printf("\t-s: start position offset for the first window, default 0\n");
  printf("\t-g: gap between the two windows in pixels, default 8\n");
}

static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  is_quit = true;
}

/*
 * Calculate a proportional display rectangle.
 *   win_w / win_h : requested window size (one of them may be 0 = auto)
 *   cam_w / cam_h : camera frame size from the VI config
 *
 * If win_h == 0:  height = win_w * cam_h / cam_w   (fix width)
 * If win_w == 0:  width  = win_h * cam_w / cam_h   (fix height)
 * Height is aligned down to 2 (YUV requirement).
 */
static void calc_proportional_rect(RKADK_U32 win_w, RKADK_U32 win_h,
                                   RKADK_U32 cam_w, RKADK_U32 cam_h,
                                   RKADK_U32 *out_w, RKADK_U32 *out_h) {
  if (win_h == 0 && win_w > 0) {
    *out_w = win_w;
    *out_h = win_w * cam_h / cam_w;
  } else if (win_w == 0 && win_h > 0) {
    *out_h = win_h;
    *out_w = win_h * cam_w / cam_h;
  } else {
    *out_w = win_w;
    *out_h = win_h;
  }
  /* align height down to 2 */
  *out_h &= ~1u;
  if (*out_h < 2)
    *out_h = 2;
}

int main(int argc, char *argv[]) {
  int c, ret;
  const char *iniPath = NULL;
  char path[RKADK_PATH_LEN];
  char sensorPath[RKADK_MAX_SENSOR_CNT][RKADK_PATH_LEN];

  RKADK_U32 win_w = DEFAULT_WIN_SIZE;
  RKADK_U32 win_h = 0;             /* 0 = proportional */
  RKADK_U32 start_offset = 0;     /* x offset of first window */
  RKADK_U32 gap = DEFAULT_GAP;

#ifdef RKAIQ
  SAMPLE_ISP_PARAM stIspParam;
  memset(&stIspParam, 0, sizeof(SAMPLE_ISP_PARAM));
  stIspParam.iqFileDir = IQ_FILE_PATH;
  stIspParam.WDRMode = RK_AIQ_WORKING_MODE_NORMAL;
  stIspParam.bMultiCam = true;    /* enable multi-camera concurrent mode */
#endif

  while ((c = getopt(argc, argv, optstr)) != -1) {
    switch (c) {
#ifdef RKAIQ
    case 'a':
      stIspParam.iqFileDir = optarg;
      break;
#endif
    case 'p':
      iniPath = optarg;
      RKADK_LOGP("iniPath: %s", iniPath);
      break;
    case 'W':
      win_w = atoi(optarg);
      break;
    case 'H':
      win_h = atoi(optarg);
      break;
    case 's':
      start_offset = atoi(optarg);
      break;
    case 'g':
      gap = atoi(optarg);
      break;
    case 'h':
    default:
      print_usage(argv[0]);
      optind = 0;
      return 0;
    }
  }
  optind = 0;

  RKADK_LOGP("Dual camera display: win %ux%u (0=proportional), gap %u, offset %u",
             win_w, win_h, gap, start_offset);

  /* ---- 1. System init ---- */
  RKADK_MPI_SYS_Init();

  /* ---- 2. Param init (load both sensor configs) ---- */
  if (iniPath) {
    memset(path, 0, RKADK_PATH_LEN);
    memset(sensorPath, 0, RKADK_MAX_SENSOR_CNT * RKADK_PATH_LEN);
    sprintf(path, "%s/rkadk_setting.ini", iniPath);
    for (int i = 0; i < RKADK_MAX_SENSOR_CNT; i++)
      sprintf(sensorPath[i], "%s/rkadk_setting_sensor_%d.ini", iniPath, i);

    char *sPath[] = {sensorPath[0], sensorPath[1], NULL};
    RKADK_PARAM_Init(path, sPath);
  } else {
    RKADK_PARAM_Init(NULL, NULL);
  }

  /* ---- 3. Start ISP for both cameras ---- */
  int sensor_count = RKADK_PARAM_GetCommCfg()->sensor_count;
  if (sensor_count < 2) {
    RKADK_LOGW("sensor_count=%d, forcing to 2 for dual display", sensor_count);
    sensor_count = 2;
  }
  RKADK_LOGP("sensor_count: %d", sensor_count);

#ifdef RKAIQ
  for (int i = 0; i < sensor_count; i++) {
    RKADK_PARAM_SENSOR_CFG_S *pstSensorCfg = RKADK_PARAM_GetSensorCfg(i);
    if (!pstSensorCfg) {
      RKADK_LOGE("GetSensorCfg(%d) failed", i);
      goto _FAIL_PARAM;
    }

    if (!pstSensorCfg->used_isp) {
      RKADK_LOGW("Cam %d: used_isp=false, skipping ISP start (non-ISP sensor)", i);
      continue;
    }

    RKADK_PARAM_FPS_S stFps;
    stFps.enStreamType = RKADK_STREAM_TYPE_SENSOR;
    ret = RKADK_PARAM_GetCamParam(i, RKADK_PARAM_TYPE_FPS, &stFps);
    if (ret) {
      RKADK_LOGE("RKADK_PARAM_GetCamParam fps failed for cam %d", i);
      goto _FAIL_PARAM;
    }
    stIspParam.fps = stFps.u32Framerate;

    ret = SAMPLE_ISP_Start(i, stIspParam);
    if (ret) {
      RKADK_LOGE("SAMPLE_ISP_Start(%d) failed", i);
      goto _FAIL_ISP;
    }
    RKADK_LOGP("ISP started for cam %d (fps=%d)", i, stIspParam.fps);
  }
#endif

  /* ---- 4. Init display for both cameras ---- */
  for (int i = 0; i < sensor_count; i++) {
    ret = RKADK_DISP_Init(i);
    if (ret) {
      RKADK_LOGE("RKADK_DISP_Init(%d) failed(%d)", i, ret);
      goto _FAIL_DISP;
    }
    RKADK_LOGP("DISP initialized for cam %d", i);
  }

  /* ---- 5. Set 256xproportional display rectangles ---- */
  for (int i = 0; i < sensor_count; i++) {
    RKADK_PARAM_DISP_CFG_S *pstDispCfg = RKADK_PARAM_GetDispCfg(i);
    if (!pstDispCfg) {
      RKADK_LOGE("GetDispCfg(%d) failed", i);
      continue;
    }

    /* Camera frame size from the VI channel used for display */
    RKADK_U32 cam_w = pstDispCfg->vi_attr.stChnAttr.stSize.u32Width;
    RKADK_U32 cam_h = pstDispCfg->vi_attr.stChnAttr.stSize.u32Height;
    if (cam_w == 0 || cam_h == 0) {
      /* fallback to the display config size */
      cam_w = pstDispCfg->width;
      cam_h = pstDispCfg->height;
    }

    RKADK_U32 rw, rh;
    calc_proportional_rect(win_w, win_h, cam_w, cam_h, &rw, &rh);

    /* VPSS crop: use full camera frame (no crop) */
    RKADK_DISP_ATTR_S stDispAttr;
    memset(&stDispAttr, 0, sizeof(RKADK_DISP_ATTR_S));
    stDispAttr.stVpssCropRect.u32X = 0;
    stDispAttr.stVpssCropRect.u32Y = 0;
    stDispAttr.stVpssCropRect.u32Width = cam_w;
    stDispAttr.stVpssCropRect.u32Height = cam_h;

    /* VO display rect: position windows side by side */
    stDispAttr.stVoRect.u32X = start_offset + i * (rw + gap);
    stDispAttr.stVoRect.u32Y = 0;
    stDispAttr.stVoRect.u32Width = rw;
    stDispAttr.stVoRect.u32Height = rh;

    ret = RKADK_DISP_SetAttr(i, &stDispAttr);
    if (ret) {
      RKADK_LOGE("RKADK_DISP_SetAttr(%d) failed(%d)", i, ret);
    } else {
      RKADK_LOGP("Cam %d: VO rect (%u, %u, %u, %u)  cam=%ux%u  win=%ux%u",
                 i, stDispAttr.stVoRect.u32X, stDispAttr.stVoRect.u32Y,
                 rw, rh, cam_w, cam_h, rw, rh);
    }
  }

  /* ---- 6. Run until 'quit' ---- */
  signal(SIGINT, sigterm_handler);
  char cmd[64];
  printf("\n#Dual camera display running.\n"
         "#Input 'quit' to exit.\n");

  while (!is_quit) {
    fgets(cmd, sizeof(cmd), stdin);
    if (strstr(cmd, "quit") || is_quit) {
      RKADK_LOGP("#Get 'quit' cmd!");
      break;
    }
    usleep(500000);
  }

_FAIL_DISP:
  for (int i = sensor_count - 1; i >= 0; i--)
    RKADK_DISP_DeInit(i);

_FAIL_ISP:
#ifdef RKAIQ
  for (int i = sensor_count - 1; i >= 0; i--) {
    RKADK_PARAM_SENSOR_CFG_S *pstSensorCfg = RKADK_PARAM_GetSensorCfg(i);
    if (pstSensorCfg && pstSensorCfg->used_isp) {
      SAMPLE_ISP_Stop(i);
      RKADK_LOGP("ISP stopped for cam %d", i);
    }
  }
#endif

_FAIL_PARAM:
  RKADK_PARAM_Deinit();
  RKADK_MPI_SYS_Exit();
  return 0;
}
