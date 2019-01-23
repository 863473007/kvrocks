#include <glog/logging.h>
#include <iostream>
#include <chrono>

#include "util.h"
#include "redis_cmd.h"
#include "redis_reply.h"
#include "redis_request.h"
#include "storage.h"

namespace Redis {

Connection::Connection(bufferevent *bev, Worker *owner)
    : bev_(bev), req_(owner->svr_), owner_(owner) {
  time_t now;
  time(&now);
  create_time_ = now;
  last_interaction_ = now;
}

Connection::~Connection() {
  if (bev_) { bufferevent_free(bev_); }
}

void Connection::OnRead(struct bufferevent *bev, void *ctx) {
  DLOG(INFO) << "on read: " << bufferevent_getfd(bev);
  auto conn = static_cast<Connection *>(ctx);

  conn->SetLastInteraction();
  conn->req_.Tokenize(conn->Input());
  conn->req_.ExecuteCommands(conn);
}

void Connection::OnWrite(struct bufferevent *bev, void *ctx) {
  auto conn = static_cast<Connection *>(ctx);
  if (conn->IsFlagEnabled(kCloseAfterReply)) {
    conn->owner_->RemoveConnection(conn->GetFD());
  }
}

void Connection::OnEvent(bufferevent *bev, short events, void *ctx) {
  auto conn = static_cast<Connection *>(ctx);
  if (events & BEV_EVENT_ERROR) {
    LOG(ERROR) << "bev error: "
               << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());
  }
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
    DLOG(INFO) << "deleted: fd=" << conn->GetFD();
    conn->owner_->RemoveConnection(conn->GetFD());
    return;
  }
  if (events & BEV_EVENT_TIMEOUT) {
    LOG(INFO) << "timeout, fd=" << conn->GetFD();
    bufferevent_enable(bev, EV_READ | EV_WRITE);
  }
}

void Connection::Reply(const std::string &msg) {
  owner_->svr_->stats_.IncrOutbondBytes(msg.size());
  Redis::Reply(bufferevent_get_output(bev_), msg);
}

void Connection::SendFile(int fd) {
  // NOTE: we don't need to close the fd, the libevent will do that
  auto output = bufferevent_get_output(bev_);
  evbuffer_add_file(output, fd, 0, -1);
}

uint64_t Connection::GetAge() {
  time_t now;
  time(&now);
  return static_cast<uint64_t>(now-create_time_);
}

void Connection::SetLastInteraction() {
  time(&last_interaction_);
}

uint64_t Connection::GetIdleTime() {
  time_t now;
  time(&now);
  return static_cast<uint64_t>(now-last_interaction_);
}

void Connection::SetFlag(Flag flag) {
  flags_ |= flag;
}

bool Connection::IsFlagEnabled(Flag flag) {
  return (flags_ & flag) > 0;
}

void Connection::SubscribeChannel(std::string &channel) {
  for (const auto &chan : subscribe_channels_) {
    if (channel == chan) return;
  }
  subscribe_channels_.emplace_back(channel);
  owner_->svr_->SubscribeChannel(channel, this);
}

void Connection::UnSubscribeChannel(std::string &channel) {
  auto iter = subscribe_channels_.begin();
  for (; iter != subscribe_channels_.end(); iter++) {
    if (*iter == channel) {
      subscribe_channels_.erase(iter);
      owner_->svr_->UnSubscribeChannel(channel, this);
    }
  }
}

void Connection::UnSubscribeAll() {
  if (subscribe_channels_.empty()) return;
  for (auto chan : subscribe_channels_) {
    owner_->svr_->UnSubscribeChannel(chan, this);
  }
  subscribe_channels_.clear();
}

int Connection::SubscriptionsCount() {
  return static_cast<int>(subscribe_channels_.size());
}

void Request::Tokenize(evbuffer *input) {
  char *line;
  size_t len;
  while (true) {
    switch (state_) {
      case ArrayLen:
        line = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF_STRICT);
        if (!line) return;
        svr_->stats_.IncrInbondBytes(len);
        multi_bulk_len_ = len > 0 ? std::strtoull(line + 1, nullptr, 10) : 0;
        free(line);
        state_ = BulkLen;
        break;
      case BulkLen:
        line = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF_STRICT);
        if (!line) return;
        svr_->stats_.IncrInbondBytes(len);
        bulk_len_ = std::strtoull(line + 1, nullptr, 10);
        free(line);
        state_ = BulkData;
        break;
      case BulkData:
        if (evbuffer_get_length(input) < bulk_len_ + 2) return;
        char *data =
            reinterpret_cast<char *>(evbuffer_pullup(input, bulk_len_ + 2));
        tokens_.emplace_back(data, bulk_len_);
        evbuffer_drain(input, bulk_len_ + 2);
        svr_->stats_.IncrInbondBytes(bulk_len_ + 2);
        --multi_bulk_len_;
        if (multi_bulk_len_ <= 0) {
          state_ = ArrayLen;
          commands_.push_back(std::move(tokens_));
          tokens_.clear();
        } else {
          state_ = BulkLen;
        }
        break;
    }
  }
}

void Request::ExecuteCommands(Connection *conn) {
  if (commands_.empty()) return;

  if (svr_->IsLoading()) {
    conn->Reply(Redis::Error("replication in progress"));
    return;
  }

  Config *config = svr_->GetConfig();
  std::string reply;
  for (auto &cmd_tokens : commands_) {
    if (conn->IsFlagEnabled(Redis::Connection::kCloseAfterReply)) break;
    if (conn->GetNamespace().empty()
        && Util::ToLower(cmd_tokens.front()) != "auth") {
      conn->Reply(Redis::Error("NOAUTH Authentication required."));
      continue;
    }
    auto s = LookupCommand(cmd_tokens.front(), &conn->current_cmd_, conn->IsRepl());
    if (!s.IsOK()) {
      conn->Reply(Redis::Error("ERR unknown command"));
      continue;
    }
    int arity = conn->current_cmd_->GetArity();
    int tokens = static_cast<int>(cmd_tokens.size());
    if ((arity > 0 && tokens != arity)
        || (arity < 0 && tokens < -arity)) {
      conn->Reply(Redis::Error("ERR wrong number of arguments"));
      continue;
    }
    conn->current_cmd_->SetArgs(cmd_tokens);
    s = conn->current_cmd_->Parse(cmd_tokens);
    if (!s.IsOK()) {
      conn->Reply(Redis::Error(s.Msg()));
      continue;
    }
    if (config->slave_readonly && svr_->IsSlave() && conn->current_cmd_->IsWrite()) {
      conn->Reply(Redis::Error("READONLY You can't write against a read only slave."));
      continue;
    }
    conn->SetLastCmd(conn->current_cmd_->Name());

    svr_->stats_.IncrCalls(conn->current_cmd_->Name());
    auto start = std::chrono::high_resolution_clock::now();
    s = conn->current_cmd_->Execute(svr_, conn, &reply);
    auto end = std::chrono::high_resolution_clock::now();
    long long duration = std::chrono::duration_cast<std::chrono::microseconds>(end-start).count();
    svr_->SlowlogPushEntryIfNeeded(conn->current_cmd_->Args(), static_cast<uint64_t>(duration));
    svr_->stats_.IncrLatency(static_cast<uint64_t>(duration), conn->current_cmd_->Name());
    if (!s.IsOK()) {
      conn->Reply(Redis::Error("ERR " + s.Msg()));
      LOG(ERROR) << "Failed to execute redis command: " << conn->current_cmd_->Name()
                 << ", err: " << s.Msg();
      continue;
    }
    if (!reply.empty()) conn->Reply(reply);
  }
  commands_.clear();
}

}  // namespace Redis
