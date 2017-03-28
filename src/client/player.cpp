/*  Copyright (C) 2014-2017 FastoGT. All right reserved.

    This file is part of FastoTV.

    FastoTV is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    FastoTV is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with FastoTV. If not, see <http://www.gnu.org/licenses/>.
*/

#include "player.h"

#include <SDL2/SDL_audio.h>  // for SDL_MIX_MAXVOLUME, etc
#include <SDL2/SDL_hints.h>

extern "C" {
#include <libavutil/time.h>
}

#include "third-party/json-c/json-c/json.h"  // for json_object_...

#include <common/file_system.h>
#include <common/application/application.h>
#include <common/threads/thread_manager.h>

#include "sdl_utils.h"
#include "network_controller.h"

#include "client/core/video_state.h"
#include "client/core/app_options.h"
#include "client/core/utils.h"
#include "client/core/video_frame.h"

/* Step size for volume control */
#define VOLUME_STEP 1
#define CURSOR_HIDE_DELAY_MSEC 1000  // 1 sec

#define USER_FIELD "user"
#define URLS_FIELD "urls"

#define IMG_PATH "resources/offline.png"

#undef ERROR

namespace fasto {
namespace fastotv {
namespace client {

namespace {

int ConvertToSDLVolume(int val) {
  val = av_clip(val, 0, 100);
  return av_clip(SDL_MIX_MAXVOLUME * val / 100, 0, SDL_MIX_MAXVOLUME);
}

bool CreateWindowFunc(int width,
                      int height,
                      bool is_full_screen,
                      const std::string& title,
                      SDL_Renderer** renderer,
                      SDL_Window** window) {
  if (!renderer || !window) {  // invalid input
    return false;
  }

  Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
  if (is_full_screen) {
    flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  }
  SDL_Window* lwindow = SDL_CreateWindow(NULL, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                         width, height, flags);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SDL_Renderer* lrenderer = NULL;
  if (lwindow) {
    SDL_RendererInfo info;
    lrenderer =
        SDL_CreateRenderer(lwindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (lrenderer) {
      if (!SDL_GetRendererInfo(lrenderer, &info)) {
        DEBUG_LOG() << "Initialized " << info.name << " renderer.";
      }
    } else {
      WARNING_LOG() << "Failed to initialize a hardware accelerated renderer: " << SDL_GetError();
      lrenderer = SDL_CreateRenderer(lwindow, -1, 0);
    }
  }

  if (!lwindow || !lrenderer) {
    ERROR_LOG() << "SDL: could not set video mode - exiting";
    return false;
  }

  SDL_SetWindowSize(lwindow, width, height);
  SDL_SetWindowTitle(lwindow, title.c_str());

  *window = lwindow;
  *renderer = lrenderer;
  return true;
}
}  // namespace

PlayerOptions::PlayerOptions()
    : exit_on_keydown(false),
      exit_on_mousedown(false),
      is_full_screen(false),
      default_width(width),
      default_height(height),
      screen_width(0),
      screen_height(0),
      audio_volume(volume),
      muted(false) {}

Player::Player(const PlayerOptions& options,
               const core::AppOptions& opt,
               const core::ComplexOptions& copt)
    : options_(options),
      opt_(opt),
      copt_(copt),
      play_list_(),
      audio_params_(nullptr),
      renderer_(NULL),
      window_(NULL),
      cursor_hidden_(false),
      cursor_last_shown_(0),
      last_mouse_left_click_(0),
      curent_stream_pos_(0),
      stream_(nullptr),
      width_(0),
      height_(0),
      xleft_(0),
      ytop_(0),
      controller_(new NetworkController) {
  // stable options
  if (options_.audio_volume < 0) {
    WARNING_LOG() << "-volume=" << options_.audio_volume << " < 0, setting to 0";
  }
  if (options_.audio_volume > 100) {
    WARNING_LOG() << "-volume=" << options_.audio_volume << " > 100, setting to 100";
  }
  options_.audio_volume = av_clip(options_.audio_volume, 0, 100);

  fApp->Subscribe(this, core::events::PostExecEvent::EventType);
  fApp->Subscribe(this, core::events::PreExecEvent::EventType);
  fApp->Subscribe(this, core::events::TimerEvent::EventType);

  fApp->Subscribe(this, core::events::AllocFrameEvent::EventType);
  fApp->Subscribe(this, core::events::QuitStreamEvent::EventType);

  fApp->Subscribe(this, core::events::KeyPressEvent::EventType);

  fApp->Subscribe(this, core::events::MouseMoveEvent::EventType);
  fApp->Subscribe(this, core::events::MousePressEvent::EventType);

  fApp->Subscribe(this, core::events::WindowResizeEvent::EventType);
  fApp->Subscribe(this, core::events::WindowExposeEvent::EventType);
  fApp->Subscribe(this, core::events::WindowCloseEvent::EventType);

  fApp->Subscribe(this, core::events::QuitEvent::EventType);

  fApp->Subscribe(this, core::events::ClientDisconnectedEvent::EventType);
  fApp->Subscribe(this, core::events::ClientConnectedEvent::EventType);
  fApp->Subscribe(this, core::events::ClientConfigChangeEvent::EventType);
  fApp->Subscribe(this, core::events::ReceiveChannelsEvent::EventType);
}

void Player::SetFullScreen(bool full_screen) {
  options_.is_full_screen = full_screen;
  SDL_SetWindowFullscreen(window_, full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  if (stream_) {
    stream_->RefreshRequest();
  }
}

void Player::SetMute(bool mute) {
  options_.muted = mute;
}

Player::~Player() {
  destroy(&controller_);
  fApp->UnSubscribe(this);
}

void Player::HandleEvent(event_t* event) {
  if (event->GetEventType() == core::events::PreExecEvent::EventType) {
    core::events::PreExecEvent* pre_event = static_cast<core::events::PreExecEvent*>(event);
    HandlePreExecEvent(pre_event);
  } else if (event->GetEventType() == core::events::PostExecEvent::EventType) {
    core::events::PostExecEvent* post_event = static_cast<core::events::PostExecEvent*>(event);
    HandlePostExecEvent(post_event);
  } else if (event->GetEventType() == core::events::AllocFrameEvent::EventType) {
    core::events::AllocFrameEvent* avent = static_cast<core::events::AllocFrameEvent*>(event);
    HandleAllocFrameEvent(avent);
  } else if (event->GetEventType() == core::events::QuitStreamEvent::EventType) {
    core::events::QuitStreamEvent* quit_stream_event =
        static_cast<core::events::QuitStreamEvent*>(event);
    HandleQuitStreamEvent(quit_stream_event);
  } else if (event->GetEventType() == core::events::TimerEvent::EventType) {
    core::events::TimerEvent* tevent = static_cast<core::events::TimerEvent*>(event);
    HandleTimerEvent(tevent);
  } else if (event->GetEventType() == core::events::KeyPressEvent::EventType) {
    core::events::KeyPressEvent* key_press_event = static_cast<core::events::KeyPressEvent*>(event);
    HandleKeyPressEvent(key_press_event);
  } else if (event->GetEventType() == core::events::WindowResizeEvent::EventType) {
    core::events::WindowResizeEvent* win_resize_event =
        static_cast<core::events::WindowResizeEvent*>(event);
    HandleWindowResizeEvent(win_resize_event);
  } else if (event->GetEventType() == core::events::WindowExposeEvent::EventType) {
    core::events::WindowExposeEvent* win_expose =
        static_cast<core::events::WindowExposeEvent*>(event);
    HandleWindowExposeEvent(win_expose);
  } else if (event->GetEventType() == core::events::WindowCloseEvent::EventType) {
    core::events::WindowCloseEvent* window_close =
        static_cast<core::events::WindowCloseEvent*>(event);
    HandleWindowCloseEvent(window_close);
  } else if (event->GetEventType() == core::events::MouseMoveEvent::EventType) {
    core::events::MouseMoveEvent* mouse_move = static_cast<core::events::MouseMoveEvent*>(event);
    HandleMouseMoveEvent(mouse_move);
  } else if (event->GetEventType() == core::events::MousePressEvent::EventType) {
    core::events::MousePressEvent* mouse_press = static_cast<core::events::MousePressEvent*>(event);
    HandleMousePressEvent(mouse_press);
  } else if (event->GetEventType() == core::events::QuitEvent::EventType) {
    core::events::QuitEvent* quit_event = static_cast<core::events::QuitEvent*>(event);
    HandleQuitEvent(quit_event);
  } else if (event->GetEventType() == core::events::ClientConnectedEvent::EventType) {
    core::events::ClientConnectedEvent* connect_event =
        static_cast<core::events::ClientConnectedEvent*>(event);
    HandleClientConnectedEvent(connect_event);
  } else if (event->GetEventType() == core::events::ClientDisconnectedEvent::EventType) {
    core::events::ClientDisconnectedEvent* disc_event =
        static_cast<core::events::ClientDisconnectedEvent*>(event);
    HandleClientDisconnectedEvent(disc_event);
  } else if (event->GetEventType() == core::events::ClientConfigChangeEvent::EventType) {
    core::events::ClientConfigChangeEvent* conf_change_event =
        static_cast<core::events::ClientConfigChangeEvent*>(event);
    HandleClientConfigChangeEvent(conf_change_event);
  } else if (event->GetEventType() == core::events::ReceiveChannelsEvent::EventType) {
    core::events::ReceiveChannelsEvent* channels_event =
        static_cast<core::events::ReceiveChannelsEvent*>(event);
    HandleReceiveChannelsEvent(channels_event);
  }
}

void Player::HandleExceptionEvent(event_t* event, common::Error err) {
  UNUSED(event);
  UNUSED(err);
}

bool Player::HandleRequestAudio(core::VideoState* stream,
                                int64_t wanted_channel_layout,
                                int wanted_nb_channels,
                                int wanted_sample_rate,
                                core::AudioParams* audio_hw_params) {
  UNUSED(stream);

  if (audio_params_) {
    *audio_hw_params = *audio_params_;
    return true;
  }

  /* prepare audio output */
  core::AudioParams laudio_hw_params;
  int ret = core::audio_open(this, wanted_channel_layout, wanted_nb_channels, wanted_sample_rate,
                             &laudio_hw_params, sdl_audio_callback);
  if (ret < 0) {
    return false;
  }

  SDL_PauseAudio(0);
  audio_params_ = new core::AudioParams(laudio_hw_params);
  *audio_hw_params = *audio_params_;
  return true;
}

void Player::HanleAudioMix(uint8_t* audio_stream_ptr,
                           const uint8_t* src,
                           uint32_t len,
                           int volume) {
  SDL_MixAudio(audio_stream_ptr, src, len, ConvertToSDLVolume(volume));
}

bool Player::HandleRealocFrame(core::VideoState* stream, core::VideoFrame* frame) {
  UNUSED(stream);

  Uint32 sdl_format;
  if (frame->format == AV_PIX_FMT_YUV420P) {
    sdl_format = SDL_PIXELFORMAT_YV12;
  } else {
    sdl_format = SDL_PIXELFORMAT_ARGB8888;
  }

  if (ReallocTexture(&frame->bmp, sdl_format, frame->width, frame->height, SDL_BLENDMODE_NONE,
                     false) < 0) {
    /* SDL allocates a buffer smaller than requested if the video
     * overlay hardware is unable to support the requested size. */

    ERROR_LOG() << "Error: the video system does not support an image\n"
                   "size of "
                << frame->width << "x" << frame->height
                << " pixels. Try using -lowres or -vf \"scale=w:h\"\n"
                   "to reduce the image size.";
    return false;
  }

  return true;
}

void Player::HanleDisplayFrame(core::VideoState* stream, const core::VideoFrame* frame) {
  UNUSED(stream);
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
  SDL_RenderClear(renderer_);

  SDL_Rect rect;
  core::calculate_display_rect(&rect, xleft_, ytop_, width_, height_, frame->width, frame->height,
                               frame->sar);
  SDL_RenderCopyEx(renderer_, frame->bmp, NULL, &rect, 0, NULL,
                   frame->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);

  SDL_RenderPresent(renderer_);
}

bool Player::HandleRequestWindow(core::VideoState* stream) {
  if (!stream) {  // invalid input
    return false;
  }

  CalculateDispalySize();

  std::string name;
  for (size_t i = 0; i < play_list_.size(); ++i) {
    if (play_list_[i].Id() == stream->Id()) {
      name = play_list_[i].GetName();
      break;
    }
  }

  if (!window_) {
    bool created =
        CreateWindowFunc(width_, height_, options_.is_full_screen, name, &renderer_, &window_);
    if (!created) {
      return false;
    }
  } else {
    SDL_SetWindowSize(window_, width_, height_);
    SDL_SetWindowTitle(window_, name.c_str());
  }

  return true;
}

void Player::HandleDefaultWindowSize(int width, int height, AVRational sar) {
  SDL_Rect rect;
  core::calculate_display_rect(&rect, 0, 0, INT_MAX, height, width, height, sar);
  options_.default_width = rect.w;
  options_.default_height = rect.h;
}

void Player::HandleAllocFrameEvent(core::events::AllocFrameEvent* event) {
  int res = event->info().stream_->HandleAllocPictureEvent();
  if (res == ERROR_RESULT_VALUE) {
    if (stream_) {
      stream_->Abort();
      destroy(&stream_);
    }
    fApp->Exit(EXIT_FAILURE);
  }
}

void Player::HandleQuitStreamEvent(core::events::QuitStreamEvent* event) {
  core::events::QuitStreamInfo inf = event->info();
  if (inf.stream_ && inf.stream_->IsAborted()) {
    return;
  }
  destroy(&stream_);
  SwitchToErrorMode();
}

void Player::HandlePreExecEvent(core::events::PreExecEvent* event) {
  core::events::PreExecInfo inf = event->info();
  if (inf.code == EXIT_SUCCESS) {
    surface_ = IMG_LoadPNG(IMG_PATH);
    controller_->Start();
  }
}

void Player::HandlePostExecEvent(core::events::PostExecEvent* event) {
  core::events::PostExecInfo inf = event->info();
  if (inf.code == EXIT_SUCCESS) {
    controller_->Stop();
    if (stream_) {
      stream_->Abort();
      destroy(&stream_);
    }
    SDL_FreeSurface(surface_);

    SDL_CloseAudio();
    destroy(&audio_params_);

    if (renderer_) {
      SDL_DestroyRenderer(renderer_);
      renderer_ = NULL;
    }
    if (window_) {
      SDL_DestroyWindow(window_);
      window_ = NULL;
    }
  } else {
    NOTREACHED();
  }
}

void Player::HandleTimerEvent(core::events::TimerEvent* event) {
  UNUSED(event);
  core::msec_t diff = core::GetCurrentMsec() - cursor_last_shown_;
  if (!cursor_hidden_ && diff > CURSOR_HIDE_DELAY_MSEC) {
    fApp->HideCursor();
    cursor_hidden_ = true;
  }
  core::msec_t remaining_time = REFRESH_RATE_MSEC;
  if (stream_) {
    stream_->TryRefreshVideo(&remaining_time);
  } else {
    if (surface_) {
      SDL_Texture* img = SDL_CreateTextureFromSurface(renderer_, surface_);
      if (img) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderCopy(renderer_, img, NULL, NULL);
        SDL_RenderPresent(renderer_);
        SDL_DestroyTexture(img);
      }
    } else {
      SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
      SDL_RenderClear(renderer_);
      SDL_RenderPresent(renderer_);
    }
  }
}

void Player::HandleKeyPressEvent(core::events::KeyPressEvent* event) {
  if (options_.exit_on_keydown) {
    fApp->Exit(EXIT_SUCCESS);
    return;
  }

  core::events::KeyPressInfo inf = event->info();
  switch (inf.ks.sym) {
    case FASTO_KEY_ESCAPE:
    case FASTO_KEY_q: {
      fApp->Exit(EXIT_SUCCESS);
      return;
    }
    case FASTO_KEY_f: {
      bool full_screen = !options_.is_full_screen;
      SetFullScreen(full_screen);
      break;
    }
    case FASTO_KEY_p:
    case FASTO_KEY_SPACE:
      if (stream_) {
        stream_->TogglePause();
      }
      break;
    case FASTO_KEY_m: {
      bool muted = !options_.muted;
      SetMute(muted);
      break;
    }
    case FASTO_KEY_KP_MULTIPLY:
    case FASTO_KEY_0:
      UpdateVolume(VOLUME_STEP);
      break;
    case FASTO_KEY_KP_DIVIDE:
    case FASTO_KEY_9:
      UpdateVolume(-VOLUME_STEP);
      break;
    case FASTO_KEY_s:  // S: Step to next frame
      if (stream_) {
        stream_->StepToNextFrame();
      }
      break;
    case FASTO_KEY_a:
      if (stream_) {
        stream_->StreamCycleChannel(AVMEDIA_TYPE_AUDIO);
      }
      break;
    case FASTO_KEY_v:
      if (stream_) {
        stream_->StreamCycleChannel(AVMEDIA_TYPE_VIDEO);
      }
      break;
    case FASTO_KEY_c:
      if (stream_) {
        stream_->StreamCycleChannel(AVMEDIA_TYPE_VIDEO);
        stream_->StreamCycleChannel(AVMEDIA_TYPE_AUDIO);
      }
      break;
    case FASTO_KEY_t:
      // StreamCycleChannel(AVMEDIA_TYPE_SUBTITLE);
      break;
    case FASTO_KEY_w: {
      break;
    }
    case FASTO_KEY_LEFTBRACKET: {
      core::VideoState* st = stream_;
      if (st) {
        common::thread th([st]() {
          st->Abort();
          delete st;
        });
        th.detach();
      }
      stream_ = CreatePrevStream();
      if (!stream_) {
        SwitchToErrorMode();
      }
      break;
    }
    case FASTO_KEY_RIGHTBRACKET: {
      core::VideoState* st = stream_;
      if (st) {
        common::thread th([st]() {
          st->Abort();
          delete st;
        });
        th.detach();
      }
      stream_ = CreateNextStream();
      if (!stream_) {
        SwitchToErrorMode();
      }
      break;
    }
    default:
      break;
  }
}

void Player::HandleMousePressEvent(core::events::MousePressEvent* event) {
  if (options_.exit_on_mousedown) {
    fApp->Exit(EXIT_SUCCESS);
    return;
  }

  core::msec_t cur_time = core::GetCurrentMsec();
  core::events::MousePressInfo inf = event->info();
  if (inf.button == FASTO_BUTTON_LEFT) {
    if (cur_time - last_mouse_left_click_ <= 500) {  // double click 0.5 sec
      bool full_screen = !options_.is_full_screen;
      SetFullScreen(full_screen);
      last_mouse_left_click_ = 0;
    } else {
      last_mouse_left_click_ = cur_time;
    }
  }

  if (cursor_hidden_) {
    fApp->ShowCursor();
    cursor_hidden_ = false;
  }
  cursor_last_shown_ = cur_time;
}

void Player::HandleMouseMoveEvent(core::events::MouseMoveEvent* event) {
  UNUSED(event);
  if (cursor_hidden_) {
    fApp->ShowCursor();
    cursor_hidden_ = false;
  }
  core::msec_t cur_time = core::GetCurrentMsec();
  cursor_last_shown_ = cur_time;
}

void Player::HandleWindowResizeEvent(core::events::WindowResizeEvent* event) {
  core::events::WindowResizeInfo inf = event->info();
  width_ = inf.width;
  height_ = inf.height;
  if (stream_) {
    stream_->RefreshRequest();
  }
}

void Player::HandleWindowExposeEvent(core::events::WindowExposeEvent* event) {
  UNUSED(event);
  if (stream_) {
    stream_->RefreshRequest();
  }
}

void Player::HandleWindowCloseEvent(core::events::WindowCloseEvent* event) {
  UNUSED(event);
  fApp->Exit(EXIT_SUCCESS);
}

void Player::HandleQuitEvent(core::events::QuitEvent* event) {
  UNUSED(event);
  fApp->Exit(EXIT_SUCCESS);
}

void Player::HandleClientConnectedEvent(core::events::ClientConnectedEvent* event) {
  UNUSED(event);

  controller_->RequestChannels();
}

void Player::HandleClientDisconnectedEvent(core::events::ClientDisconnectedEvent* event) {
  UNUSED(event);
}

void Player::HandleClientConfigChangeEvent(core::events::ClientConfigChangeEvent* event) {
  UNUSED(event);
}

void Player::HandleReceiveChannelsEvent(core::events::ReceiveChannelsEvent* event) {
  channels_t chan = event->info();
  play_list_ = chan;
  stream_ = CreateCurrentStream();
  if (!stream_) {
    SwitchToErrorMode();
  }
}

bool Player::GetCurrentUrl(Url* url) const {
  if (!url || play_list_.empty()) {
    return false;
  }

  *url = play_list_[curent_stream_pos_];
  return true;
}

void Player::sdl_audio_callback(void* opaque, uint8_t* stream, int len) {
  Player* player = static_cast<Player*>(opaque);
  core::VideoState* st = player->stream_;
  if (!player->options_.muted && st && st->IsStreamReady()) {
    st->UpdateAudioBuffer(stream, len, player->options_.audio_volume);
  } else {
    memset(stream, 0, len);
  }
}

void Player::UpdateVolume(int step) {
  options_.audio_volume = av_clip(options_.audio_volume + step, 0, 100);
}

int Player::ReallocTexture(SDL_Texture** texture,
                           Uint32 new_format,
                           int new_width,
                           int new_height,
                           SDL_BlendMode blendmode,
                           bool init_texture) {
  Uint32 format;
  int access, w, h;
  if (SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w ||
      new_height != h || new_format != format) {
    SDL_DestroyTexture(*texture);
    *texture = CreateTexture(renderer_, new_format, new_width, new_height, blendmode, init_texture);
    if (!*texture) {
      return ERROR_RESULT_VALUE;
    }
  }
  return SUCCESS_RESULT_VALUE;
}

void Player::SwitchToErrorMode() {
  Url url;
  std::string name_str = "Unknown";
  if (GetCurrentUrl(&url)) {
    name_str = url.GetName();
  }

  CalculateDispalySize();

  if (!window_) {
    CreateWindowFunc(width_, height_, options_.is_full_screen, name_str, &renderer_, &window_);
  } else {
    SDL_SetWindowTitle(window_, name_str.c_str());
  }
}

void Player::CalculateDispalySize() {
  if (width_ && height_) {
    return;
  }

  if (options_.screen_width && options_.screen_height) {
    width_ = options_.screen_width;
    height_ = options_.screen_height;
  } else {
    width_ = options_.default_width;
    height_ = options_.default_height;
  }
}

core::VideoState* Player::CreateCurrentStream() {
  if (play_list_.empty()) {
    return nullptr;
  }

  size_t pos = curent_stream_pos_;
  core::VideoState* stream = CreateStreamPos(pos);
  int res = stream->Exec();
  if (res == EXIT_FAILURE) {
    delete stream;
    return nullptr;
  }

  return stream;
}

core::VideoState* Player::CreateNextStream() {
  // check is executed in main thread?
  if (play_list_.empty()) {
    return nullptr;
  }

  size_t pos = GenerateNextPosition();
  core::VideoState* stream = CreateStreamPos(pos);
  int res = stream->Exec();
  if (res == EXIT_FAILURE) {
    delete stream;
    return nullptr;
  }

  return stream;
}

core::VideoState* Player::CreatePrevStream() {
  // check is executed in main thread?
  if (play_list_.empty()) {
    return nullptr;
  }

  size_t pos = GeneratePrevPosition();
  core::VideoState* stream = CreateStreamPos(pos);
  int res = stream->Exec();
  if (res == EXIT_FAILURE) {
    delete stream;
    return nullptr;
  }

  return stream;
}

core::VideoState* Player::CreateStreamPos(size_t pos) {
  CHECK(THREAD_MANAGER()->IsMainThread());
  curent_stream_pos_ = pos;
  Url url = play_list_[curent_stream_pos_];
  core::VideoState* stream = new core::VideoState(url.Id(), url.GetUrl(), opt_, copt_, this);
  return stream;
}

size_t Player::GenerateNextPosition() const {
  if (curent_stream_pos_ + 1 == play_list_.size()) {
    return 0;
  }

  return curent_stream_pos_ + 1;
}

size_t Player::GeneratePrevPosition() const {
  if (curent_stream_pos_ == 0) {
    return play_list_.size() - 1;
  }

  return curent_stream_pos_ - 1;
}
}
}
}