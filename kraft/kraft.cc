// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#include "kraft/kraft.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>

#include "error_code.h"

using namespace kraft;
using namespace kerror;

#define CHECK_MESSAGE_(msg_)                                                   \
  decltype(peer_node_map_)::iterator node_iter;                                \
  do {                                                                         \
    if ((msg_).to() != self_node_.id) {                                        \
      errcode = E_INCORRECT_TO_NODE;                                           \
      return MakeNoInfoError();                                                \
    }                                                                          \
                                                                               \
    node_iter = peer_node_map_.find((msg_).from());                            \
    if (node_iter == peer_node_map_.end()) {                                   \
      errcode = E_INCORRECT_FROM_NODE;                                         \
      return MakeNoInfoError();                                                \
    }                                                                          \
  } while (0)

/*--------------------------------------------------*/
/* Impl helpers                                     */
/*--------------------------------------------------*/

void Raft::Log(char const *fmt, ...) const
{
  if (!log_cb_) return;

  char buf[4096];

  va_list args;
  va_start(args, fmt);
  const auto writen = vsprintf(buf, fmt, args);
  if (writen < 0) return;

  log_cb_(buf, writen);
  va_end(args);
}

auto Raft::MakeConfChange(ConfChangeType conf_type, u64 id,
                          void const *conf_ctx, size_t conf_ctx_len)
    -> ConfChange
{
  ConfChange conf_chg;
  conf_chg.set_type(conf_type);
  conf_chg.set_node_id(id);
  conf_chg.set_context(conf_ctx, conf_ctx_len);
  return conf_chg;
}

void Raft::InitNextIndices() noexcept
{
  KRAFT_ASSERT1(log_get_last_entry_meta_cb_);
  const auto last_log_index = log_get_last_entry_meta_cb_().index;
  for (auto &idx : next_indices_) {
    idx = last_log_index + 1;
  }
}

void Raft::InitMatchIndices() noexcept
{
  for (auto &idx : match_indices_) {
    idx = 0;
  }
}

bool Raft::CheckResponseTerm(Response const &response)
{
  // Peer's term is newer than me,
  // indicates I'm might a stale leader/candidate.
  if (response.term() > term()) {
    SetTerm(response.term());
    if (!IsFollower()) BecomeFollower();
    return true; // to stop process, update state
  }
  return false;
}

bool Raft::CheckRequestTerm(Request &request, Response *response, void *ctx)
{
  // AppendEntriest & RequestVote Receiver Impl Rule 1
  if (request.term() < term()) {
    response->set_success(false);
    response->set_term(term());
    Log("Peer term < current term");
    send_message_cb_(response, ctx);
    return true; // to stop process, peer is a stale candidate/leader
  }

  // new leader has later term is OK.
  if (request.term() > term()) {
    Log("Peer term > current term");
    SetTerm(request.term());
  }
  return false;
}

bool Raft::IsLogUptodate(u64 term, u64 index) const noexcept
{
  // Compare last entry term instead of current term since current term
  // may greater than last entry term.
  // e.g.
  // current term = 13(candidate also)
  // request term = 12
  // last entry term = 8
  const auto last_entry_meta = log_get_last_entry_meta_cb_();
  return term > last_entry_meta.term ||
         (term == last_entry_meta.term && index > last_entry_meta.index);
}

auto Raft::MakeRequest(MessageType type) const -> Request
{
  Request request;

  KRAFT_ASSERT(self_node_.id != INVALID_ID,
               "The message's id can't be invalid for request");
  request.set_from(self_node_.id);
  request.set_type(type);
  request.set_term(term());
  return request;
}

auto Raft::MakeResponse(Request const &request) const -> Response
{
  Response response;

  KRAFT_ASSERT(self_node_.id != INVALID_ID,
               "The message's id can'bt invalid for response");
  response.set_from(self_node_.id);
  response.set_type(request.type());
  response.set_term(term());
  return response;
}

void Raft::SendVoteRequestAll() const
{
  const auto last_entry_info = log_get_last_entry_meta_cb_();
  Request req;
  req.set_type(MSG_VOTE);
  KRAFT_ASSERT(self_node_.id != INVALID_ID,
               "The VoteRequest id can't be invalid");
  req.set_from(self_node_.id);

  // Used for checking whether the log is up-to-date or not
  req.set_log_index(last_entry_info.index);
  req.set_log_term(last_entry_info.term);

  req.set_term(term());
  for (auto &peer_node : peer_nodes_) {
    // FIXME voting
    if (peer_node.IsActive()) {
      req.set_to(peer_node.id);
      KRAFT_ASSERT1(send_message_cb_);
      send_message_cb_(&req, peer_node.ctx);
    }
  }
}

auto Raft::MakeEntry(EntryType type, void const *data, u64 n) -> Entry
{
  Entry entry;
  entry.set_type(type);
  entry.set_term(term());
  entry.set_index(entry_index_counter_++);
  entry.set_data(data, n);
  return entry;
}

auto Raft::AppendLogEntry(EntryType type, void const *data, u64 n) -> Error
{
  KRAFT_ASSERT(IsLeader(), "Only leader can send AE request");

  Entry last_entry = MakeEntry(type, data, n);
  const auto last_entry_index = last_entry.index();
  if (!log_append_entry_cb_(last_entry)) {
    Log("Failed to log entry, stop continue append log");
    errcode = E_LOG_APPEND_ENTRY;
    return MakeMsgErrorf("Failed to log entry [%llu]", (ull)last_entry_index);
  }

  KRAFT_ASSERT(last_entry.term() == term(),
               "The appened log entry must has current term");
  Request request = MakeRequest(MSG_AE);
  request.set_commit(commit_);

  // FIXME send size
  for (u64 i = 0; i < peer_nodes_.size(); ++i) {
    auto &node = peer_nodes_[i];
    // FIXME voting
    if (!node.IsActive()) {
      continue;
    }

    request.set_to(node.id);
    u64 start_idx = next_indices_[i];
    KRAFT_ASSERT(start_idx >= 1, "The next index must >= 1");

    // log index previous new logs
    request.set_log_index(start_idx - 1);
    // FIXME cache term?
    const auto prev_entry_term = log_get_entry_meta_cb_(start_idx - 1);
    request.set_log_term(prev_entry_term);

    request.mutable_entries()->Reserve(last_entry_index - start_idx + 1);
    for (; start_idx <= last_entry_index; ++start_idx) {
      request.mutable_entries()->Add();
      Entry const *p_entry = *(--request.mutable_entries()->pointer_end());
      // auto request.mutable_entries()->Add();
      if (!log_get_entry_cb_(start_idx, &p_entry)) {
        errcode = E_LOG_GET_ENTRY;
        return MakeMsgErrorf("Failed to get log entry in %llu", start_idx);
      }
    }
    send_message_cb_(&request, node.ctx);
  }

  return MakeSuccess();
}

auto Raft::FillAppendEntriesRequest(Request *request, u64 start_idx,
                                    u64 last_entry_index) const -> Error
{
  KRAFT_ASSERT(IsLeader(), "Only leader can fill AE request");

  request->set_log_index(start_idx - 1);
  // FIXME cache term?
  auto prev_entry_term = log_get_entry_meta_cb_(start_idx - 1);
  request->set_log_term(prev_entry_term);
  request->mutable_entries()->Reserve(last_entry_index - start_idx + 1);
  for (; start_idx <= last_entry_index; ++start_idx) {
    request->mutable_entries()->Add();
    Entry const *p_entry = *(--request->mutable_entries()->pointer_end());
    if (!log_get_entry_cb_(start_idx, &p_entry)) {
      errcode = E_LOG_GET_ENTRY;
      return MakeMsgErrorf("Failed to get log entry in %llu", start_idx);
    }
  }
  return MakeSuccess();
}

auto Raft::AppendConfLogEntry(ConfChangeType type, u64 id, void const *conf_ctx,
                              u64 conf_ctx_len) -> Error
{
  auto conf_chg =
      MakeConfChange(CONF_CHANGE_ADD_NODE, id, conf_ctx, conf_ctx_len);
  auto conf_chg_data = conf_chg.SerializeAsString();
  return AppendLogEntry(ENTRY_CONF, conf_chg_data.data(), conf_chg_data.size());
}

auto Raft::ApplyEntries() -> Error
{
  KRAFT_ASSERT(last_applied_ <= commit_,
               "The last applied index must be <= commit index");

  Entry const *apply_entry = nullptr;
  for (u64 i = last_applied_ + 1; i < commit_; ++i) {
    if (!log_get_entry_cb_(i, &apply_entry)) {
      errcode = E_LOG_GET_ENTRY;
      return MakeMsgErrorf("Failed to get log entry in %llu when apply entries",
                           i);
    }

    // User should implement how to apply conf change entry and
    // normal entry(user-defined data)
    if (!log_apply_entry_cb_(apply_entry)) {
      errcode = E_LOG_APPLY;
      return MakeMsgErrorf("Failed to apply log entry in %llu", i);
    }

    // FIXME Support roll back or allow partial applied?
    last_applied_++;
  }

  Log("Apply successfully, applied index = %llu", last_applied_);
  KRAFT_ASSERT(last_applied_ == commit_,
               "applied index must be same with commit index after apply "
               "successfully");
  return MakeSuccess();
}

// To expand, make LogState() as a single function call
bool Raft::LogState() const
{
  if (!log_set_state_(persistent_state_)) {
    Log("Failed to log state, old state is: (term: %llu, voted_for: %llu, "
        "id: %llu)",
        term(), voted_for(), id());
    return false;
  }
  return true;
}

auto Raft::IncrementCommitIndex() -> Error
{
  /* Leader Rule 4 */

  auto last_entry_meta = log_get_last_entry_meta_cb_();

  // The last entry has the maximum index, its term is old term entry
  if (last_entry_meta.term != term()) return MakeSuccess();

  for (u64 test_index = last_entry_meta.index; test_index > commit_;) {
    bool is_majority = false;
    u64 match_num = 1; // self
    for (u64 i = 0; i < peer_nodes_.size(); ++i) {
      if (!peer_nodes_[i].IsActive()) continue;

      if (match_indices_[i] >= test_index) {
        match_num++;

        is_majority = match_num > ((peer_nodes_.size() + 1) >> 1);
        if (is_majority) break;
      }
    }

    if (is_majority) {
      Log("Update commit: %llu", commit_);
      commit_ = test_index;
      break;
    }

    --test_index;

    auto entry_term = log_get_entry_meta_cb_(test_index);
    if (entry_term != term()) {
      break;
    }
  }

  return MakeSuccess();
}

auto Raft::AppendNoOpLogEntry() -> Error
{
  return AppendLogEntry(ENTRY_NORMAL, nullptr, 0);
}

// Should be call only once
void Raft::SetId(u64 id)
{
  persistent_state_.set_id(id);
  LogState();
}

void Raft::SetVotedFor(u64 id)
{
  persistent_state_.set_voted_for(id);
  LogState();
}

void Raft::SetTerm(u64 term)
{
  persistent_state_.set_term(term);
  LogState();
}

/*--------------------------------------------------*/
/* Public API                                       */
/*--------------------------------------------------*/

Raft::Raft(size_t peer_node_num)
{
  entry_index_counter_ = 1;
  leader_id_ = -1;
  commit_ = 0;
  last_applied_ = 0;
  voted_num_ = 0;

  peer_nodes_.reserve(peer_node_num);
  peer_node_map_.reserve(peer_node_num);

  persistent_state_.set_id(-1);
  persistent_state_.set_term(0);
  persistent_state_.set_voted_for(-1);

  next_indices_.resize(peer_node_num);
  match_indices_.resize(peer_node_num);

  // Follwer don't initialize next_indices and match_indices
  BecomeFollower();

  KRAFT_ASSERT1(log_append_entry_cb_);
  KRAFT_ASSERT1(log_get_last_entry_meta_cb_);
  KRAFT_ASSERT1(log_get_entry_meta_cb_);
  KRAFT_ASSERT1(log_truncate_after_cb_);
  KRAFT_ASSERT1(log_get_entry_cb_);
  KRAFT_ASSERT1(log_apply_entry_cb_);
  KRAFT_ASSERT1(log_set_state_);
  KRAFT_ASSERT1(send_message_cb_);
}

Raft::~Raft() noexcept { Log("Raft is destoryed"); }

bool Raft::AddPeerNode(u64 id, void *user_ctx)
{
  peer_nodes_.emplace_back(id, user_ctx, 0);
  auto iter_res = peer_node_map_.emplace(id, peer_nodes_.size() - 1);
  if (!iter_res.second) {
    return false;
  }
  return true;
}

bool Raft::AddSelfNode(u64 id, void *user_ctx)
{
  self_node_.id = id;
  self_node_.ctx = user_ctx;

  SetId(id);
  return true;
}

void Raft::Periodic()
{
  // if (0 == peer_nodes_.size() && !IsLeader()) {
  //   BecomeLeader();
  // }

  if (IsLeader()) {
    if (!IncrementCommitIndex()) {
      return;
    }
  }

  if (!ApplyEntries()) {
    return;
  }
}

void Raft::ElectionPeriodic() { BecomeCandidate(); }

/* Leader Rule 1 */
void Raft::HeartBeatPeriodic()
{
  auto request = MakeRequest(MSG_HEART_BEAT);
  for (auto &node : peer_nodes_) {
    if (node.IsRecvHeartBeat()) {
      node.flag |= NODE_ACTIVE;
    }
    request.set_to(node.id);
    send_message_cb_(&request, node.ctx);
    node.flag &= ~NODE_RECV_HARTBEAT;
  }
}

void Raft::BecomeCandidate()
{
  KRAFT_ASSERT(!IsLeader() && !IsCandidate(),
               "Can't convert leader/candidate to candidate");
  state_ = RaftState::CANDIDATE;

  // Increment term and start a new election
  SetTerm(term() + 1);
  Log("Become candidate of term: %llu", term());

  KRAFT_ASSERT(voted_num_ == 0, "Vote number must be 0 when become candidate");

  // vote myself
  // since you also as count by majority
  voted_num_++;

  SendVoteRequestAll();

  // User should reset timer after this be called
}

void Raft::BecomeLeader()
{
  KRAFT_ASSERT(!IsFollower() && !IsLeader(),
               "Can't convert follwer/leader to leader");
  state_ = RaftState::LEADER;
  Log("Voted num = %llu", voted_num_);
  Log("Become leader of term %llu", term());

  // reinitialize is necessary
  // replicate is based on leader current log
  InitNextIndices();
  InitMatchIndices();

  // Leader not a follower
  // avoid persist the voted_for state
  SetVotedFor(-1);

  // Leader Rule 1
  HeartBeatPeriodic();

  //
  if (!AppendNoOpLogEntry()) {
    Log("Failed to log no-op entry");
  }
}

void Raft::BecomeFollower()
{
  KRAFT_ASSERT(!IsFollower(), "Can't conver follower to follower");
  state_ = RaftState::FOLLOWER;
  Log("Become follower of term: %llu", term());

  // reset election state
  voted_num_ = 0;
  SetVotedFor(-1);
}

// Follower
auto Raft::RecvVoteRequest(Request &request) -> Error
{
  // Ignore leader
  if (IsLeader()) {
    return MakeSuccess();
  }

  CHECK_MESSAGE_(request);

  Response response;
  response.set_to(request.from());
  response.set_from(self_node_.id);
  response.set_type(request.type());

  // First check term
  // RequestVote RPC - Receiver Impl rule 1
  auto ctx = peer_nodes_[node_iter->second].ctx;

  // CheckRequestTerm has handle this request
  // return OK is ok.
  if (CheckRequestTerm(request, &response, ctx)) {
    return MakeSuccess();
  }

  // If self node don't vote any node or
  // the voted node don't receive the response(peer send request again),
  // consider vote it and send response.
  if ((voted_for() == INVALID_VOTED_FOR || voted_for() == request.from()) &&
      IsLogUptodate(request.log_term(), request.log_index()))
  {
    SetVotedFor(request.from());
    response.set_success(true);
  } else {
    KRAFT_ASSERT(voted_for() != INVALID_VOTED_FOR &&
                     peer_node_map_.find(voted_for()) != peer_node_map_.end(),
                 "Voted for is an invalid node id");
    response.set_success(false);
  }

  send_message_cb_(&response, ctx);
  return MakeSuccess();
}

auto Raft::RecvVoteResponse(Response &response) -> Error
{
  // The node has become leader
  // ignore the vote response.
  if (IsLeader()) {
    return MakeSuccess();
  }

  // Start a new election
  if (!IsCandidate()) {
    KRAFT_ASSERT1(voted_for() == INVALID_VOTED_FOR);
    KRAFT_ASSERT1(voted_num_ == 0);
    // errcode = E_INCORRECT_STATE;
    return MakeSuccess();
  }
  KRAFT_ASSERT(response.has_success(),
               "response must has the 'success' fields");

  auto node_iter = peer_node_map_.find(response.from());
  if (node_iter == peer_node_map_.end()) {
    errcode = E_NODE_NONEXISTS;
    return MakeNoInfoError();
  }

  if (response.success()) {
    voted_num_++;
    if (IsMajorityVoted()) {
      BecomeLeader();
    }
    return MakeSuccess();
  }

  KRAFT_ASSERT1(!response.success());

  SetTerm(KRAFT_MAX(term(), response.term()));
  return MakeSuccess();
}

auto Raft::RecvHeartBeatResponse(Response &response) -> Error
{
  if (!IsLeader()) {
    errcode = E_INCORRECT_STATE;
    return MakeSuccess();
  }

  CHECK_MESSAGE_(response);

  auto &node = peer_nodes_[node_iter->second];

  if (CheckResponseTerm(response)) {
    return MakeSuccess();
  }

  leader_id_ = response.from();
  node.flag |= NODE_RECV_HARTBEAT;
  return MakeSuccess();
}

auto Raft::RecvHeartBeatRequest(Request &request) -> Error
{
  KRAFT_ASSERT(!IsLeader(), "Leader receive AppendEntries Request");
  if (IsCandidate()) {
    Log("Candidate receive Heart Beat Request from current leader: %llu",
        request.term());
    BecomeFollower();
  }

  Response response;
  CHECK_MESSAGE_(response);

  auto &node = peer_nodes_[node_iter->second];

  if (CheckRequestTerm(request, &response, node.ctx)) {
    return MakeSuccess();
  }

  response.set_success(true);
  response.set_from(self_node_.id);
  response.set_to(request.to());
  response.set_term(term());
  send_message_cb_(&response, node.ctx);
  return MakeSuccess();
}

auto Raft::RecvAppendEntriesResponse(Response &response) -> Error
{
  if (!IsLeader()) {
    errcode = E_INCORRECT_STATE;
    return MakeSuccess();
  }

  CHECK_MESSAGE_(response);

  // auto &node = peer_nodes_[node_iter->second];

  if (CheckResponseTerm(response)) {
    return MakeSuccess();
  }

  auto peer_id = response.from();
  if (response.success()) {
    // Peer has log these entries logs to its log
    auto entries_num = response.hint();
    next_indices_[peer_id] = entries_num;
    match_indices_[peer_id] = entries_num - 1;
  } else {
    auto conflict_num = response.hint();
    match_indices_[peer_id] = next_indices_[peer_id] - conflict_num;
    next_indices_[peer_id] -= conflict_num;

    auto request = MakeRequest(MSG_AE);
    request.set_to(peer_id);
    request.set_commit(commit_);
    auto last_entry_meta = log_get_last_entry_meta_cb_();
    FillAppendEntriesRequest(&request, next_indices_[peer_id],
                             last_entry_meta.index);
    Log("Retry send AE request [%llu, %llu]", next_indices_[peer_id],
        last_entry_meta);
    send_message_cb_(&request, peer_nodes_[node_iter->second].ctx);
  }

  leader_id_ = response.from();
  return MakeSuccess();
}

auto Raft::RecvAppendEntriesRequest(Request &request) -> Error
{
  KRAFT_ASSERT(!IsLeader(), "Leader receive AppendEntries Request");
  if (IsCandidate()) {
    Log("Candidate receive AppendEntriest Request from current leader: %llu",
        request.term());
    BecomeFollower();
  }

  CHECK_MESSAGE_(request);

  auto &node = peer_nodes_[node_iter->second];

  Response response;
  if (CheckRequestTerm(request, &response, node.ctx)) {
    return MakeSuccess();
  }

  response.set_from(self_node_.id);
  response.set_to(request.to());
  response.set_term(term());

  // AE impl Rule 2
  auto check_term = log_get_entry_meta_cb_(request.log_index());
  if (check_term != request.log_term()) {
    response.set_success(false);
  }

  const u64 entry_num = request.entries_size();
  bool need_check_term = true;
  for (u64 i = request.log_index() + 1; i < entry_num; ++i) {
    if (need_check_term) {
      check_term = log_get_entry_meta_cb_(i);
      if (check_term == INVALID_TERM) {
        // AE impl Rule 4
        log_append_entry_cb_(const_cast<Entry &>(request.entries()[i]));
        need_check_term = false;
      } else if (check_term != request.entries()[i].term()) {
        // AE impl Rule 3
        if (!log_truncate_after_cb_(i)) {
          errcode = E_TRUNCATE;
          return MakeMsgErrorf("Failed to truncate the log in %llu\n", (ull)i);
        }
      }
    } else {
      log_append_entry_cb_(const_cast<Entry &>(request.entries()[i]));
    }

    // AE Impl Rule 5
    if (request.commit() > commit_) {
      commit_ =
          KRAFT_MIN(request.commit(),
                    request.entries()[request.entries_size() - 1].index());
    }
  }

  send_message_cb_(&response, node.ctx);
  return MakeSuccess();
}

auto Raft::AppendLog(void const *data, size_t n) -> Error
{
  return AppendLogEntry(ENTRY_NORMAL, data, n);
}

auto Raft::AppendConfChangeLog(u64 id, void const *conf_ctx, size_t n) -> Error
{
  return AppendConfLogEntry(CONF_CHANGE_ADD_NODE, id, conf_ctx, n);
}

auto Raft::AskNodeId(void *user_ctx) -> Error
{
  Request request = MakeRequest(MSG_ASK_ID);
  // request.set_commit(commit_);
  send_message_cb_(&request, user_ctx);

  return MakeSuccess();
}

auto Raft::RecvAskNodeResponse(Response &response, void *user_ctx) -> Error
{
  if (response.to() != self_node_.id) {
    errcode = E_INVALID_RESPONSE;
    return MakeNoInfoError();
  }

  auto node_iter = peer_node_map_.find(response.from());
  if (node_iter != peer_node_map_.end()) {
    // Has added, ignore this response
    // Maybe a duplicate message since network problem.
    return MakeSuccess();
  }

  peer_nodes_.emplace_back(response.from(), user_ctx);
  if (!peer_node_map_.emplace(response.from(), peer_nodes_.size() - 1).second) {
    errcode = E_ADD_PEER_NODE;
    return MakeNoInfoError();
  }
  return MakeSuccess();
}
