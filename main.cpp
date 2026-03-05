#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <cmath>
#include <cstdlib>
#include <numeric>

#include "PointerPredictor.hpp"

#define WINDOW_W 800
#define WINDOW_H 600
#define CIRCLE_RADIUS 40
#define TARGET_FPS 60
#define FRAME_TIME_NS (SDL_NS_PER_SECOND / TARGET_FPS)

static void draw_circle(SDL_Renderer *renderer, float cx, float cy, float radius) {
  const int segments = 32;
  SDL_FPoint points[segments + 1];

  for (int i = 0; i < segments; i++) {
    float theta = (float)i / segments * 2.0f * M_PI;
    points[i].x = cx + SDL_cosf(theta) * radius;
    points[i].y = cy + SDL_sinf(theta) * radius;
  }
  points[segments] = points[0];
  SDL_RenderLines(renderer, points, segments + 1);
}

static void draw_crosshair(SDL_Renderer *renderer, float cx, float cy, float size) {
  float r = size;
  SDL_RenderLine(renderer, cx, cy - r, cx, cy + r);
  SDL_RenderLine(renderer, cx - r, cy, cx + r, cy);
}

const char *translate_vsync_mode(int mode) {
  switch (mode) {
  case 0:
    return "OFF";
  case 1:
    return "ON";
  case 2:
    return "ADAPTIVE";
  default:
    return "UNKNOWN";
  }
}

static void maybe_set_renderer_extra(SDL_Renderer *renderer) {
  if (!renderer)
    return;
  const char *renderer_name = SDL_GetRendererName(renderer);
  if (!renderer_name)
    return;
  auto props = SDL_GetRendererProperties(renderer);
  if (SDL_strcmp(renderer_name, "vulkan") == 0) {
    if (props) {
      auto swapchain_count = SDL_GetNumberProperty(props, SDL_PROP_RENDERER_VULKAN_SWAPCHAIN_IMAGE_COUNT_NUMBER, 1);
      SDL_Log("Vulkan swapchain image count: %" SDL_PRIs64, swapchain_count);
    }
  } else if (SDL_strcmp(renderer_name, "gpu") == 0) {
    if (props) {
      auto gpudev = (SDL_GPUDevice *)SDL_GetPointerProperty(props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr);
      SDL_SetGPUAllowedFramesInFlight(gpudev, 2);
      SDL_Log("GPU frames in flight set to 2");
    }
  }
  SDL_SetRenderVSync(renderer, 1);
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }

  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;

  auto render_driver_hint = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
  if (render_driver_hint && SDL_strcasecmp(render_driver_hint, "help") == 0) {
    int num = SDL_GetNumRenderDrivers();
    SDL_Log("Available render drivers:");
    for (int i = 0; i < num; i++) {
      SDL_Log("%s", SDL_GetRenderDriver(i));
    }
    SDL_Quit();
    return 0;
  }

  if (!SDL_CreateWindowAndRenderer("Input Delay Visualizer", WINDOW_W, WINDOW_H,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer)) {
    SDL_Log("CreateWindowAndRenderer failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  const char *renderer_name = SDL_GetRendererName(renderer);
  maybe_set_renderer_extra(renderer);

  int mouse_down = 0;
  SDL_FPoint mouse_pos = {0, 0};
  PointerPredictor predictor;
  bool running = true;
  int vsync_flag = 0;
  bool fps_limit = false;
  Uint64 last_mouse_timestamp = 0;
  Uint64 last_begin_render_timestamp = 0;
  Uint64 last_present_timestamp = 0;
  Uint64 fps_timer = SDL_GetTicksNS();
  uint64_t fps_count = 0;
  uint64_t frame_count = 0;
  uint64_t last_input_delay_ns = 0;
  int motion_estimate_frames = 0;
  float display_fps = 0.0f;
  int vsync_interval = 0;
  int osd_scale = SDL_GetWindowPixelDensity(window);

  static constexpr int FRAME_TIME_HISTORY = 60;
  uint64_t frame_times[FRAME_TIME_HISTORY] = {};
  int frame_time_index = 0;
  int frame_time_count = 0;
  uint64_t avg_frame_time = 0;

  SDL_GetRenderVSync(renderer, &vsync_interval);
  switch (vsync_interval) {
  case 1:
    vsync_flag = 1;
    break;
  case SDL_RENDERER_VSYNC_ADAPTIVE:
    vsync_flag = 2;
    break;
  default:
    vsync_flag = 0;
    break;
  }

  while (running) {
    bool mouse_moved = false;
    if (fps_limit && vsync_flag == 0) {
      uint64_t last_render_time = last_present_timestamp - last_begin_render_timestamp;
      Uint64 elapsed = SDL_GetTicksNS() - last_present_timestamp;
      if (elapsed < FRAME_TIME_NS) {
        SDL_DelayNS(FRAME_TIME_NS - elapsed - last_render_time);
      }
    }

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {
      case SDL_EVENT_QUIT:
        running = false;
        break;

      case SDL_EVENT_MOUSE_MOTION:
        mouse_moved = true;
        // Use SDL_GetTicksNS() for sample timestamps — ev.motion.timestamp
        // is unreliable on X11 (different clock epoch).
        last_mouse_timestamp = ev.motion.timestamp;
        mouse_pos.x = ev.motion.x;
        mouse_pos.y = ev.motion.y;
        predictor.add_sample(mouse_pos, last_mouse_timestamp);
        break;

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev.button.button == SDL_BUTTON_LEFT) {
          mouse_down = 1;
        }
        break;
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev.button.button == SDL_BUTTON_LEFT) {
          mouse_down = 0;
        }
        break;

      case SDL_EVENT_KEY_DOWN:
        if (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT)) {
          Uint32 flags = SDL_GetWindowFlags(window);
          if (flags & SDL_WINDOW_FULLSCREEN)
            SDL_SetWindowFullscreen(window, false);
          else
            SDL_SetWindowFullscreen(window, true);
        }
        if (ev.key.key == SDLK_V) {
          switch (vsync_flag) {
          case 0:
            SDL_SetRenderVSync(renderer, 1);
            vsync_flag = 1;
            break;
          case 1:
            SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);
            vsync_flag = 2;
            break;
          case 2:
            SDL_SetRenderVSync(renderer, 0);
            vsync_flag = 0;
            break;
          }
        }
        if (ev.key.key == SDLK_F) {
          fps_limit = !fps_limit;
          if (fps_limit && vsync_flag != 0) {
            /* disable vsync when enabling delay-based limiter */
            SDL_SetRenderVSync(renderer, 0);
            vsync_flag = 0;
          }
        }
        if (ev.key.key == SDLK_S) {
          osd_scale++;
          if (osd_scale > 4)
            osd_scale = 1;
        }
        if (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_Q) {
          running = false;
        }
        if (ev.key.key == SDLK_MINUS) {
          motion_estimate_frames = SDL_max(0, motion_estimate_frames - 1);
        }
        if (ev.key.key == SDLK_EQUALS) {
          motion_estimate_frames = SDL_min(10, motion_estimate_frames + 1);
        }
        break;

      default:
        break;
      }
    }

    uint64_t last_frame_time = last_present_timestamp - last_begin_render_timestamp;
    last_begin_render_timestamp = SDL_GetTicksNS();

    if (!mouse_moved) {
      // If the mouse didn't move this frame, add a sample with the last
      // known position but current timestamp.  This keeps the predictor's
      // velocity estimate from aging out while idle, so it can produce a
      // reasonable prediction immediately when motion resumes.
      predictor.add_sample(mouse_pos, last_begin_render_timestamp);
    }

    /* Update moving average of frame time */
    frame_times[frame_time_index] = last_frame_time;
    frame_time_index = (frame_time_index + 1) % FRAME_TIME_HISTORY;
    if (frame_time_count < FRAME_TIME_HISTORY)
      frame_time_count++;
    avg_frame_time = std::accumulate(frame_times, frame_times + frame_time_count, uint64_t(0)) / frame_time_count;

    /* Clear to dark background */
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    /* Scale logical mouse coords to the renderer's physical-pixel space. */
    float density = SDL_GetWindowPixelDensity(window);
    float px = mouse_pos.x * density;
    float py = mouse_pos.y * density;

    /* Draw circle at cursor */
    if (mouse_down) {
      SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    } else {
      SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
    }
    draw_crosshair(renderer, px, py, 32 * osd_scale);

    if (motion_estimate_frames > 0 && predictor.can_predict()) {
      // Prediction target: current frame start + N frame times into the future.
      // last_begin_render_timestamp is when this frame began (≈ when samples
      // were timestamped), so adding N * avg_frame_time predicts N frames
      // ahead.
      auto predict = predictor.predict(last_begin_render_timestamp + motion_estimate_frames * avg_frame_time);
      draw_circle(renderer, predict.x * density, predict.y * density, 5 * osd_scale);
    }

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    float old_scale_x, old_scale_y;
    SDL_GetRenderScale(renderer, &old_scale_x, &old_scale_y);
    SDL_SetRenderScale(renderer, osd_scale, osd_scale);
    int line = 0;
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "Avg FPS: %.1f", display_fps);
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "Frames: %" SDL_PRIu64, frame_count);
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "Renderer: %s", renderer_name);
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "Last input-present time: %.2f ms",
                              (float)last_input_delay_ns / SDL_NS_PER_MS);
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "Avg frame time: %.2f ms",
                              (float)avg_frame_time / SDL_NS_PER_MS);
    SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255);
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "[V] Toggle VSync (%s)", translate_vsync_mode(vsync_flag));
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "[F] Toggle 60 FPS Limit (%s)",
                              fps_limit ? (vsync_flag ? "INACTIVE" : "ON") : "OFF");
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "[S] Toggle OSD Scale (%dx)", osd_scale);
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "[-/=] Toggle Motion Estimate (%d frames)",
                              motion_estimate_frames);
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "[Alt-Enter] Toggle Fullscreen");
    SDL_RenderDebugTextFormat(renderer, 0, (line++) * 9, "[Esc/Q] Quit");

    SDL_SetRenderScale(renderer, old_scale_x, old_scale_y);

    SDL_RenderPresent(renderer);
    last_present_timestamp = SDL_GetTicksNS();
    if (mouse_moved) {
      last_input_delay_ns = last_present_timestamp - last_mouse_timestamp;
    }
    /* Update FPS counter once per second */
    fps_count++;
    frame_count++;
    Uint64 fps_elapsed = last_present_timestamp - fps_timer;
    if (fps_elapsed >= SDL_NS_PER_SECOND) {
      display_fps = (float)fps_count / ((float)fps_elapsed / SDL_NS_PER_SECOND);
      fps_count = 0;
      fps_timer = last_present_timestamp;
    }
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
