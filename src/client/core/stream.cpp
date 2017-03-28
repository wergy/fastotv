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

#include "client/core/stream.h"

#include <stddef.h>  // for NULL
#include <atomic>    // for atomic

#include "client/core/clock.h"
#include "client/core/packet_queue.h"
#include "client/core/utils.h"

namespace fasto {
namespace fastotv {
namespace client {
namespace core {

Stream::Stream() : packet_queue_(nullptr), clock_(nullptr), stream_index_(-1), stream_st_(NULL) {
  packet_queue_ = PacketQueue::MakePacketQueue();
  clock_ = new Clock();
}

bool Stream::Open(int index, AVStream* av_stream_st) {
  stream_index_ = index;
  stream_st_ = av_stream_st;
  return IsOpened();
}

bool Stream::IsOpened() const {
  return stream_index_ != -1 && stream_st_ != NULL;
}

void Stream::Close() {
  stream_index_ = -1;
  stream_st_ = NULL;
}

bool Stream::HasEnoughPackets() const {
  if (!IsOpened()) {
    return false;
  }

  if (packet_queue_->IsAborted()) {
    return false;
  }

  bool attach = stream_st_->disposition & AV_DISPOSITION_ATTACHED_PIC;
  if (!attach) {
    return false;
  }

  return (packet_queue_->NbPackets() > minimum_frames &&
          (!packet_queue_->Duration() || q2d() * packet_queue_->Duration() > 1000));
}

Stream::~Stream() {
  stream_index_ = -1;
  stream_st_ = NULL;
  destroy(&clock_);
  destroy(&packet_queue_);
}

int Stream::Index() const {
  return stream_index_;
}

AVStream* Stream::AvStream() const {
  return stream_st_;
}

double Stream::q2d() const {
  return q2d_diff(stream_st_->time_base);
}

clock_t Stream::GetClock() const {
  return clock_->GetClock();
}

void Stream::SetClockAt(clock_t pts, clock_t time) {
  clock_->SetClockAt(pts, time);
}

void Stream::SetClock(clock_t pts) {
  clock_->SetClock(pts);
}

void Stream::SetPaused(bool pause) {
  clock_->SetPaused(pause);
}

clock_t Stream::LastUpdatedClock() const {
  return clock_->LastUpdated();
}

void Stream::SyncSerialClock() {
  SetClock(clock_->GetClock());
}

PacketQueue* Stream::Queue() const {
  return packet_queue_;
}

VideoStream::VideoStream() : Stream() {}

AudioStream::AudioStream() : Stream() {}

}  // namespace core
}
}
}