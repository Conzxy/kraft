// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#include "kraft_impl.h"

#define CHECK_MESSAGE_(msg_)                                                   \
  decltype(peer_node_map_)::iterator node_iter;                                \
  do {                                                                         \
    if ((msg_).to() != self_node_.id) {                                        \
      return E_INCORRECT_TO_NODE;                                              \
    }                                                                          \
                                                                               \
    node_iter = peer_node_map_.find((msg_).from());                            \
    if (node_iter == peer_node_map_.end()) {                                   \
      return E_INCORRECT_FROM_NODE;                                            \
    }                                                                          \
  } while (0)

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

Raft::~Raft() noexcept
{
  RaftImpl::Log(this, "Raft is destoryed");
}

bool Raft::AddPeerNode(u64 id, void *user_ctx, void *conf_ctx,
                       size_t conf_ctx_len)
{
  peer_nodes_.emplace_back(id, user_ctx, 0);
  auto iter_res = peer_node_map_.emplace(id, peer_nodes_.size() - 1);
  if (!iter_res.second) {
    return false;
  }
  if (IsLeader()) {
    return E_OK == RaftImpl::AppendConfLogEntry(this, CONF_CHANGE_ADD_NODE, id,
                                                conf_ctx, conf_ctx_len);
  }
  return true;
}

bool Raft::AddSelfNode(u64 id, void *user_ctx, void *conf_ctx,
                       size_t conf_ctx_len)
{
  self_node_.id = id;
  self_node_.ctx = user_ctx;
  if (IsLeader()) {
    return E_OK == RaftImpl::AppendConfLogEntry(this, CONF_CHANGE_ADD_NODE, id,
                                                conf_ctx, conf_ctx_len);
  }

  SetId(id);
  return true;
}

void Raft::Periodic()
{
  if (0 == peer_nodes_.size() && !IsLeader()) {
    BecomeLeader();
  }

  if (IsLeader()) {
    if (!RaftImpl::IncrementCommitIndex(this)) {
      return;
    }

    if (!RaftImpl::ApplyEntries(this)) {
      return;
    }
  }
}

void Raft::ElectionPeriodic()
{
  BecomeCandidate();
}

void Raft::HeartBeatPeriodic()
{
  auto request = RaftImpl::MakeRequest(this, MSG_HEART_BEAT);
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

  SetTerm(term() + 1);
  RaftImpl::Log(this, "Become candidate of term: %llu", term());

  KRAFT_ASSERT(voted_num_ == 0, "Vote number must be 0 when become candidate");
  voted_num_++;

  RaftImpl::SendVoteRequestAll(this);

  // User should reset timer after this be called
}

void Raft::BecomeLeader()
{
  KRAFT_ASSERT(!IsFollower() && !Isleader(),
               "Can't convert follwer/leader to leader");
  state_ = RaftState::LEADER;
  RaftImpl::Log(this, "Voted num = %llu", voted_num_);
  RaftImpl::Log(this, "Become leader of term %llu", term());

  // reinitialize
  RaftImpl::InitNextIndices(this);
  RaftImpl::InitMatchIndices(this);

  // &Leader Rule1
  HeartBeatPeriodic();
}

void Raft::BecomeFollower()
{
  KRAFT_ASSERT(!IsFollower(), "Can't conver follower to follower");
  state_ = RaftState::FOLLOWER;
  RaftImpl::Log(this, "Become follower of term: %llu", term());

  // reset election state
  voted_num_ = 0;
  SetVotedFor(-1);
}

auto Raft::RecvVoteRequest(Request &request) -> ErrorCode
{
  // Ignore leader
  if (IsLeader()) {
    return E_OK;
  }

  CHECK_MESSAGE_(request);

  Response response;
  response.set_to(request.from());
  response.set_from(self_node_.id);
  response.set_type(request.type());

  // First check term
  // RequestVote RPC - Receiver Impl rule 1
  auto ctx = peer_nodes_[node_iter->second].ctx;
  if (RaftImpl::CheckRequestTerm(this, request, &response, ctx)) {
    return E_OK;
  }

  // If self node don't vote any node or
  // the voted node don't receive the response,
  // consider vote it and send response.
  if ((voted_for() == INVALID_VOTED_FOR || voted_for() == request.from()) &&
      RaftImpl::IsLogUptodate(this, request.log_term(), request.log_index()))
  {
    SetVotedFor(request.from());
    response.set_success(true);
  } else {
    KRAFT_ASSERT(voted_for() != INVALID_VOTED_FOR &&
                     peer_node_map_.find(voted_for) != peer_node_map_.end(),
                 "Voted for is an invalid node id");
    response.set_success(false);
  }

  send_message_cb_(&response, ctx);
  return E_OK;
}

auto Raft::RecvVoteResponse(Response &response) -> ErrorCode
{
  if (IsLeader()) {
    return E_OK;
  }
  // Not E_OK
  if (!IsCandidate()) return E_INCORRECT_STATE;
  KRAFT_ASSERT(response.has_term() && response.has_success(),
               "response must has the 'term' and 'success' fields");

  auto node_iter = peer_node_map_.find(response.from());
  if (node_iter == peer_node_map_.end()) {
    return E_NODE_NONEXISTS;
  }

  if (response.success()) {
    voted_num_++;
    if (IsMajorityVoted()) {
      BecomeLeader();
    }
    return E_OK;
  }

  KRAFT_ASSERT1(!response.success());

  SetTerm(KRAFT_MAX(term(), response.term()));
  return E_OK;
}

auto Raft::RecvAppendEntriesResponse(Response &response) -> ErrorCode
{
  if (!IsLeader()) return E_INCORRECT_STATE;
  CHECK_MESSAGE_(response);

  auto &node = peer_nodes_[node_iter->second];

  if (RaftImpl::CheckResponseTerm(this, response)) {
    return E_OK;
  }

  switch (response.type()) {
    case MSG_HEART_BEAT: {
      node.flag |= NODE_RECV_HARTBEAT;
    } break;

    case MSG_AE: {
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

        auto request = RaftImpl::MakeRequest(this, MSG_AE);
        request.set_to(peer_id);
        request.set_commit(commit_);
        auto last_entry_meta = log_get_last_entry_meta_cb_();
        RaftImpl::FillAppendEntriesRequest(
            this, &request, next_indices_[peer_id], last_entry_meta.index);
        RaftImpl::Log(this, "Retry send AE request [%llu, %llu]",
                      next_indices_[peer_id], last_entry_meta);
        send_message_cb_(&request, peer_nodes_[node_iter->second].ctx);
      }
    } break;

    default:
      return E_INVALID_RESPONSE;
  }

  leader_id_ = response.from();
  return E_OK;
}

auto Raft::RecvHeartBeatRequest(Request &request) -> ErrorCode {}

auto Raft::RecvAppendEntriesRequest(Request &request) -> ErrorCode
{
  KRAFT_ASSERT(!IsLeader(), "Leader receive AppendEntries Request");

  CHECK_MESSAGE_(request);
  auto &node = peer_nodes_[node_iter->second];

  Response response;
  if (RaftImpl::CheckRequestTerm(this, request, &response, node.ctx)) {
    return E_OK;
  }

  response.set_from(self_node_.id);
  response.set_to(request.to());
  response.set_term(term());
  switch (request.type()) {
    case MSG_HEART_BEAT: {
      response.set_success(true);
    } break;

    case MSG_AE: {
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
            log_append_entry_cb_(request.entries()[i]);
            need_check_term = false;
          } else if (check_term != request.entries()[i].term()) {
            if (!log_truncate_after_cb_(i)) {
              return E_TRUNCATE;
            }
          }
        } else {
          log_append_entry_cb_(request.entries()[i]);
        }

        // AE Impl &4
        if (request.commit() > commit_) {
          commit_ =
              KRAFT_MIN(request.commit(),
                        request.entries()[request.entries_size() - 1].index());
        }
      }
    }

    default:
      return E_INVALID_REQUEST;
  }

  // After type check OK
  if (IsCandidate()) {
    RaftImpl::Log(
        this,
        "Candidate receive AppendEntriest Request from current leader: %llu",
        request.term());
    BecomeFollower();
  }

  send_message_cb_(&response, node.ctx);
  return E_OK;
}

auto Raft::AppendLog(void const *data, size_t n) -> ErrorCode
{
  return RaftImpl::AppendLogEntry(this, ENTRY_NORMAL, data, n);
}

auto Raft::AskNodeId(void *user_ctx) -> ErrorCode
{
  Request request = RaftImpl::MakeRequest(this, MSG_ASK_ID);
  // request.set_commit(commit_);
  send_message_cb_(&request, user_ctx);

  return E_OK;
}

auto Raft::RecvAskNodeResponse(Response &response, void *user_ctx) -> ErrorCode
{
  if (response.to() != self_node_.id) {
    return E_INVALID_RESPONSE;
  }

  auto node_iter = peer_node_map_.find(response.from());
  if (node_iter != peer_node_map_.end()) {
    // Has added, ignore this response
    // Maybe a duplicate message since network problem.
    return E_OK;
  }

  peer_nodes_.emplace_back(response.from(), user_ctx);
  if (!peer_node_map_.emplace(response.from(), peer_nodes_.size() - 1).second) {
    return E_ADD_PEER_NODE;
  }
  return E_OK;
}

void Raft::SetId(u64 id)
{
  persistent_state_.set_id(id);
  RaftImpl::LogState(this);
}

void Raft::SetVotedFor(u64 id)
{
  persistent_state_.set_voted_for(id);
  RaftImpl::LogState(this);
}

void Raft::SetTerm(u64 term)
{
  persistent_state_.set_term(term);
  RaftImpl::LogState(this);
}
