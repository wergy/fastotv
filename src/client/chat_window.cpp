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

#include "client/chat_window.h"

#include <common/application/application.h>
#include <common/convert2string.h>

#include "client/player/draw/draw.h"
#include "client/player/gui/widgets/button.h"

#include "client/chat_list_window.h"

namespace fastotv {
namespace client {

ChatWindow::ChatWindow(const SDL_Color& back_ground_color)
    : base_class(),
      hide_button_img_(nullptr),
      show_button_img_(nullptr),
      watchers_(0),
      chat_window_(nullptr),
      send_message_button_(nullptr),
      font_(nullptr),
      text_color_() {
  SetTransparent(true);

  chat_window_ = new ChatListWindow(back_ground_color);
  chat_window_->SetVisible(true);

  send_message_button_ = new player::gui::Button(player::draw::blue_color);
  send_message_button_->SetText("Post");
  send_message_button_->SetDrawType(player::gui::Label::CENTER_TEXT);
  send_message_button_->SetVisible(true);
}

ChatWindow::~ChatWindow() {
  destroy(&send_message_button_);
  destroy(&chat_window_);
}

void ChatWindow::SetPostClickedCallback(mouse_clicked_callback_t cb) {
  send_message_button_->SetMouseClickedCallback(cb);
}

void ChatWindow::SetTextColor(const SDL_Color& color) {
  chat_window_->SetTextColor(color);
  send_message_button_->SetTextColor(color);
  text_color_ = color;
}

void ChatWindow::SetFont(TTF_Font* font) {
  chat_window_->SetFont(font);
  send_message_button_->SetFont(font);
  font_ = font;
}

void ChatWindow::SetWatchers(size_t watchers) {
  watchers_ = watchers;
}

void ChatWindow::SetHideButtonImage(SDL_Texture* img) {
  hide_button_img_ = img;
}

void ChatWindow::SetShowButtonImage(SDL_Texture* img) {
  show_button_img_ = img;
}

void ChatWindow::SetMessages(const messages_t& msgs) {
  chat_window_->SetMessages(msgs);
}

void ChatWindow::SetRowHeight(int row_height) {
  chat_window_->SetRowHeight(row_height);
}

void ChatWindow::Draw(SDL_Renderer* render) {
  if (!IsCanDraw()) {
    base_class::Draw(render);
    return;
  }

  if (fApp->IsCursorVisible()) {
    if (!chat_window_->IsVisible()) {
      if (show_button_img_) {
        SDL_Rect show_button_rect = GetShowButtonChatRect();
        SDL_RenderCopy(render, show_button_img_, NULL, &show_button_rect);
      }
      return;
    }

    if (hide_button_img_) {
      SDL_Rect hide_button_rect = GetHideButtonChatRect();
      SDL_RenderCopy(render, hide_button_img_, NULL, &hide_button_rect);
    }

    SDL_Rect watchers_rect = GetWatcherRect();
    std::string watchers_str = common::ConvertToString(watchers_);
    player::draw::FillRectColor(render, watchers_rect, player::draw::red_color);
    player::draw::DrawCenterTextInRect(render, watchers_str, font_, text_color_, watchers_rect);
  }

  chat_window_->SetRect(GetRect());
  chat_window_->Draw(render);

  send_message_button_->SetRect(GetSendButtonRect());
  send_message_button_->Draw(render);
}

void ChatWindow::HandleMousePressEvent(player::gui::events::MousePressEvent* event) {
  player::gui::events::MousePressInfo inf = event->GetInfo();
  SDL_MouseButtonEvent sinfo = inf.mevent;
  if (sinfo.button == SDL_BUTTON_LEFT) {
    SDL_Point point = inf.GetMousePoint();
    if (chat_window_->IsVisible()) {
      if (IsHideButtonChatRect(point)) {
        chat_window_->SetVisible(false);
      }
    } else {
      if (IsShowButtonChatRect(point)) {
        chat_window_->SetVisible(true);
      }
    }
  }

  base_class::HandleMousePressEvent(event);
}

bool ChatWindow::IsHideButtonChatRect(const SDL_Point& point) const {
  const SDL_Rect hide_button_rect = GetHideButtonChatRect();
  return player::draw::IsPointInRect(point, hide_button_rect);
}

bool ChatWindow::IsShowButtonChatRect(const SDL_Point& point) const {
  const SDL_Rect show_button_rect = GetShowButtonChatRect();
  return player::draw::IsPointInRect(point, show_button_rect);
}

SDL_Rect ChatWindow::GetHideButtonChatRect() const {
  if (!font_) {
    return player::draw::empty_rect;
  }

  int font_height_2line = player::draw::CalcHeightFontPlaceByRowCount(font_, 2);
  SDL_Rect chat_rect = GetRect();
  SDL_Rect show_button_rect = {chat_rect.w / 2, chat_rect.y - font_height_2line, font_height_2line, font_height_2line};
  return show_button_rect;
}

SDL_Rect ChatWindow::GetShowButtonChatRect() const {
  if (!font_) {
    return player::draw::empty_rect;
  }

  int font_height_2line = player::draw::CalcHeightFontPlaceByRowCount(font_, 2);
  SDL_Rect chat_rect = GetRect();
  SDL_Rect hide_button_rect = {chat_rect.w / 2, chat_rect.y + chat_rect.h - font_height_2line, font_height_2line,
                               font_height_2line};
  return hide_button_rect;
}

SDL_Rect ChatWindow::GetWatcherRect() const {
  if (!font_) {
    return player::draw::empty_rect;
  }

  int font_height_2line = player::draw::CalcHeightFontPlaceByRowCount(font_, 2);
  SDL_Rect chat_rect = GetRect();
  SDL_Rect hide_button_rect = {chat_rect.w - font_height_2line, chat_rect.y - font_height_2line, font_height_2line,
                               font_height_2line};
  return hide_button_rect;
}

SDL_Rect ChatWindow::GetSendButtonRect() const {
  if (!font_) {
    return player::draw::empty_rect;
  }

  int font_height_2line = player::draw::CalcHeightFontPlaceByRowCount(font_, 2);
  SDL_Rect chat_rect = GetRect();
  int button_width = font_height_2line * 4;
  SDL_Rect hide_button_rect = {chat_rect.w - button_width, chat_rect.y + chat_rect.h - font_height_2line, button_width,
                               font_height_2line};
  return hide_button_rect;
}

}  // namespace client
}  // namespace fastotv
