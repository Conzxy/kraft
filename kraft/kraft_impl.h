// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
//
// This file is must be included by kraft.cc only
#include "kraft/kraft.h"

#include <cassert>
#include <stdarg.h>

#include "error_code.h"

using namespace kraft;
using namespace kerror;

struct Raft::RaftImpl {
  static void Log(Raft *raft, char const *fmt, ...)
  {
    if (!raft->log_cb_) return;

    char buf[4096];

    va_list args;
    va_start(args, fmt);
    const auto writen = vsprintf(buf, fmt, args);
    if (writen < 0) return;

    raft->log_cb_(buf, writen);
    va_end(args);
  }

  KRAFT_INLINE static ConfChange MakeConfChange(Raft *raft,
                                                ConfChangeType conf_type,
                                                u64 id, void const *conf_ctx,
                                                size_t conf_ctx_len)
  {
    ConfChange conf_chg;
    conf_chg.set_type(conf_type);
    conf_chg.set_node_id(id);
    conf_chg.set_context(conf_ctx, conf_ctx_len);
    return conf_chg;
  }

  KRAFT_INLINE static void InitNextIndices(Raft *raft)
  {
    KRAFT_ASSERT1(raft->log_get_last_entry_meta_cb_);
    const auto last_log_index = raft->log_get_last_entry_meta_cb_().index;
    for (auto &idx : raft->next_indices_) {
      idx = last_log_index + 1;
    }
  }

  KRAFT_INLINE static void InitMatchIndices(Raft *raft)
  {
    for (auto &idx : raft->match_indices_) {
      idx = 0;
    }
  }

  KRAFT_INLINE static bool CheckResponseTerm(Raft *raft,
                                             Response const &response)
  {
    // Peer's term is newer than me,
    // indicates I'm might a stale leader/candidate.
    if (response.term() > raft->term()) {
      SetTerm(raft, response.term());
      if (!raft->IsFollower()) raft->BecomeFollower();
      return true; // to stop process, update state
    }
    return false;
  }

  KRAFT_INLINE static bool CheckRequestTerm(Raft *raft, Request &request,
                                            Response *response, void *ctx)
  {
    // AppendEntriest & RequestVote Receiver Impl Rule 1
    if (request.term() < raft->term()) {
      response->set_success(false);
      response->set_term(raft->term());
      Log(raft, "Peer term < current term");
      raft->send_message_cb_(response, ctx);
      return true; // to stop process, peer is a stale candidate/leader
    }

    // new leader has later term is OK.
    if (request.term() > raft->term()) {
      Log(raft, "Peer term > current term");
      SetTerm(raft, raft->term());
    }
    return false;
  }

  KRAFT_INLINE static bool IsLogUptodate(Raft *raft, u64 term,
                                         u64 index) noexcept
  {
    // Compare last entry term instead of current term since current term
    // may greater than last entry term.
    // e.g.
    // current term = 13(candidate also)
    // request term = 12
    // last entry term = 8
    const auto last_entry_meta = raft->log_get_last_entry_meta_cb_();
    return term > last_entry_meta.term ||
           (term == raft->term() && index > last_entry_meta.index);
  }

  KRAFT_INLINE static Request MakeRequest(Raft *raft, MessageType type)
  {
    Request request;

    KRAFT_ASSERT(raft->self_node_.id != INVALID_ID,
                 "The message's id can't be invalid for request");
    request.set_from(raft->self_node_.id);
    request.set_type(type);
    request.set_term(raft->term());
    return request;
  }

  KRAFT_INLINE static Response MakeResponse(Raft *raft, Request const &request)
  {
    Response response;

    KRAFT_ASSERT(raft->self_node_.id != INVALID_ID,
                 "The message's id can'bt invalid for response");
    response.set_from(raft->self_node_.id);
    response.set_type(request.type());
    response.set_term(raft->term());
    return response;
  }

  KRAFT_INLINE static void SendVoteRequestAll(Raft *raft)
  {
    const auto last_entry_info = raft->log_get_last_entry_meta_cb_();
    Request req;
    req.set_type(MSG_VOTE);
    KRAFT_ASSERT(raft->self_node_.id != INVALID_ID,
                 "The VoteRequest id can't be invalid");
    req.set_from(raft->self_node_.id);

    // Used for checking whether the log is up-to-date or not
    req.set_log_index(last_entry_info.index);
    req.set_log_term(last_entry_info.term);

    req.set_term(raft->term());
    for (auto &peer_node : raft->peer_nodes_) {
      // FIXME voting
      if (peer_node.IsActive()) {
        req.set_to(peer_node.id);
        KRAFT_ASSERT1(raft->send_message_cb_);
        raft->send_message_cb_(&req, peer_node.ctx);
      }
    }
  }

  KRAFT_INLINE static Entry MakeEntry(Raft *raft, EntryType type,
                                      void const *data, u64 n)
  {
    Entry entry;
    entry.set_type(type);
    entry.set_term(raft->term());
    entry.set_index(raft->entry_index_counter_++);
    entry.set_data(data, n);
    return entry;
  }

  KRAFT_INLINE static Error AppendLogEntry(Raft *raft, EntryType type,
                                           void const *data, u64 n)
  {
    KRAFT_ASSERT(raft->IsLeader(), "Only leader can send AE request");

    Entry last_entry = MakeEntry(raft, type, data, n);
    const auto last_entry_index = last_entry.index();
    if (!raft->log_append_entry_cb_(last_entry)) {
      Log(raft, "Failed to log entry, stop continue append log");
      errcode = E_LOG_APPEND_ENTRY;
      return MakeMsgErrorf("Failed to log entry [%llu]", (ull)last_entry_index);
    }

    KRAFT_ASSERT(last_entry.term() == raft->term(),
                 "The appened log entry must has current term");
    Request request = MakeRequest(raft, MSG_AE);
    request.set_commit(raft->commit_);

    // FIXME send size
    for (u64 i = 0; i < raft->peer_nodes_.size(); ++i) {
      auto &node = raft->peer_nodes_[i];
      // FIXME voting
      if (!node.IsActive()) {
        continue;
      }

      request.set_to(node.id);
      u64 start_idx = raft->next_indices_[i];
      KRAFT_ASSERT(start_idx >= 1, "The next index must >= 1");

      // log index previous new logs
      request.set_log_index(start_idx - 1);
      // FIXME cache term?
      const auto prev_entry_term = raft->log_get_entry_meta_cb_(start_idx - 1);
      request.set_log_term(prev_entry_term);

      request.mutable_entries()->Reserve(last_entry_index - start_idx + 1);
      for (; start_idx <= last_entry_index; ++start_idx) {
        request.mutable_entries()->Add();
        Entry const *p_entry = *(--request.mutable_entries()->pointer_end());
        // auto request.mutable_entries()->Add();
        if (!raft->log_get_entry_cb_(start_idx, &p_entry)) {
          errcode = E_LOG_GET_ENTRY;
          return MakeMsgErrorf("Failed to get log entry in %llu", start_idx);
        }
      }
      raft->send_message_cb_(&request, node.ctx);
    }

    return MakeSuccess();
  }

  KRAFT_INLINE static Error FillAppendEntriesRequest(Raft *raft,
                                                     Request *request,
                                                     u64 start_idx,
                                                     u64 last_entry_index)
  {
    KRAFT_ASSERT(raft->IsLeader(), "Only leader can fill AE request");

    request->set_log_index(start_idx - 1);
    // FIXME cache term?
    auto prev_entry_term = raft->log_get_entry_meta_cb_(start_idx - 1);
    request->set_log_term(prev_entry_term);
    request->mutable_entries()->Reserve(last_entry_index - start_idx + 1);
    for (; start_idx <= last_entry_index; ++start_idx) {
      request->mutable_entries()->Add();
      Entry const *p_entry = *(--request->mutable_entries()->pointer_end());
      if (!raft->log_get_entry_cb_(start_idx, &p_entry)) {
        errcode = E_LOG_GET_ENTRY;
        return MakeMsgErrorf("Failed to get log entry in %llu", start_idx);
      }
    }
    return MakeSuccess();
  }

  KRAFT_INLINE static Error AppendConfLogEntry(Raft *raft, ConfChangeType type,
                                               u64 id, void const *conf_ctx,
                                               u64 conf_ctx_len)
  {
    auto conf_chg =
        MakeConfChange(raft, CONF_CHANGE_ADD_NODE, id, conf_ctx, conf_ctx_len);
    auto conf_chg_data = conf_chg.SerializeAsString();
    return AppendLogEntry(raft, ENTRY_CONF, conf_chg_data.data(),
                          conf_chg_data.size());
  }

  KRAFT_INLINE static Error ApplyEntries(Raft *raft)
  {
    KRAFT_ASSERT(raft->last_applied_ <= raft->commit_,
                 "The last applied index must be <= commit index");

    Entry const *apply_entry = nullptr;
    for (u64 i = raft->last_applied_ + 1; i < raft->commit_; ++i) {
      if (!raft->log_get_entry_cb_(i, &apply_entry)) {
        errcode = E_LOG_GET_ENTRY;
        return MakeMsgErrorf(
            "Failed to get log entry in %llu when apply entries", i);
      }

      // User should implement how to apply conf change entry and
      // normal entry(user-defined data)
      if (!raft->log_apply_entry_cb_(apply_entry)) {
        errcode = E_LOG_APPLY;
        return MakeMsgErrorf("Failed to apply log entry in %llu", i);
      }

      // FIXME Support roll back or allow partial applied?
      raft->last_applied_++;
    }

    Log(raft, "Apply successfully, applied index = %llu", raft->last_applied_);
    KRAFT_ASSERT(raft->last_applied_ == raft->commit_,
                 "applied index must be same with commit index after apply "
                 "successfully");
    return MakeSuccess();
  }

  // To expand, make LogState() as a single function call
  KRAFT_INLINE static bool LogState(Raft *raft)
  {
    if (!raft->log_set_state_(raft->persistent_state_)) {
      Log(raft,
          "Failed to log state, old state is: (term: %llu, voted_for: %llu, "
          "id: %llu)",
          raft->term(), raft->voted_for(), raft->id());
      return false;
    }
    return true;
  }

  KRAFT_INLINE static Error IncrementCommitIndex(Raft *raft)
  {
    /* Leader Rule 4 */

    auto last_entry_meta = raft->log_get_last_entry_meta_cb_();

    // The last entry has the maximum index, its term is old term entry
    if (last_entry_meta.term != raft->term()) return MakeSuccess();

    for (u64 test_index = last_entry_meta.index; test_index > raft->commit_;) {
      bool is_majority = false;
      u64 match_num = 1; // self
      for (u64 i = 0; i < raft->peer_nodes_.size(); ++i) {
        if (!raft->peer_nodes_[i].IsActive()) continue;

        if (raft->match_indices_[i] >= test_index) {
          match_num++;

          is_majority = match_num > ((raft->peer_nodes_.size() + 1) >> 1);
          if (is_majority) break;
        }
      }

      if (is_majority) {
        Log(raft, "Update commit: %llu", raft->commit_);
        raft->commit_ = test_index;
        break;
      }

      --test_index;

      auto entry_term = raft->log_get_entry_meta_cb_(test_index);
      if (entry_term != raft->term()) {
        break;
      }
    }

    return MakeSuccess();
  }

  KRAFT_INLINE static Error AppendNoOpLogEntry(Raft *raft)
  {
    return AppendLogEntry(raft, ENTRY_NORMAL, nullptr, 0);
  }

  // Should be call only once
  KRAFT_INLINE static void SetId(Raft *raft, u64 id)
  {
    raft->persistent_state_.set_id(id);
    LogState(raft);
  }

  KRAFT_INLINE static void SetVotedFor(Raft *raft, u64 id)
  {
    raft->persistent_state_.set_voted_for(id);
    LogState(raft);
  }

  KRAFT_INLINE static void SetTerm(Raft *raft, u64 term)
  {
    raft->persistent_state_.set_term(term);
    LogState(raft);
  }
};
