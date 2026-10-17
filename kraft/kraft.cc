// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#include "kraft/kraft.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>

#include "error_code.h"
#include "kraft.pb.h"

using namespace kraft;
using namespace kerror;

namespace {

char const *RoleName(Raft::RaftRole role) noexcept
{
  switch (role) {
    case Raft::RaftRole::FOLLOWER:
      return "FOLLOWER";
    case Raft::RaftRole::CANDIDATE:
      return "CANDIDATE";
    case Raft::RaftRole::LEADER:
      return "LEADER";
  }
  return "UNKNOWN";
}

// A reply carries the MSG_*_RES counterpart of the request it answers instead
// of echoing the request type: the host dispatches on one flat envelope, and
// every receive path can whitelist the exact type it accepts.
MessageType ResponseTypeOf(MessageType request_type) noexcept
{
  switch (request_type) {
    case MSG_VOTE:
      return MSG_VOTE_RES;
    case MSG_AE:
      return MSG_AE_RES;
    case MSG_HEART_BEAT:
      return MSG_HEART_BEAT_RES;
    case MSG_ASK_ID:
      return MSG_ASK_ID_RES;
    case MSG_NO_OP:
      return MSG_NO_OP_RES;
    default:
      return request_type;
  }
}

bool IsMessageTypeIn(MessageType type,
                     std::initializer_list<MessageType> types) noexcept
{
  for (auto t : types) {
    if (t == type) return true;
  }
  return false;
}

} // namespace

namespace kraft {

u64 SteadyClockMs() noexcept
{
  using namespace std::chrono;
  const auto elapsed = steady_clock::now().time_since_epoch();
  return static_cast<u64>(duration_cast<milliseconds>(elapsed).count());
}

} // namespace kraft

// The single entry check of every receive path: the type must be one this
// path accepts, the message must be addressed to this instance, and the
// sender must be a known peer. `node_iter` is declared in the caller's scope
// so the caller can reach the sender's index and context.
#define CHECK_MESSAGE_(msg_, ...)                                              \
  decltype(peer_node_map_)::iterator node_iter;                                \
  do {                                                                         \
    if (!IsMessageTypeIn((msg_).type(), {__VA_ARGS__})) {                      \
      errcode = E_INCORRECT_MESSAGE_TYPE;                                      \
      return MakeNoInfoError();                                                \
    }                                                                          \
                                                                               \
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
  const auto writen = vsnprintf(buf, sizeof(buf), fmt, args);
  if (writen > 0) {
    const auto n = static_cast<size_t>(writen);
    log_cb_(buf, n < sizeof(buf) ? n : sizeof(buf) - 1);
  }

  va_end(args);
}

void Raft::LogError(char const *what, Error const &err) const
{
  // info() marks the error as checked, which is not optional here: kerror
  // aborts the process when an error is destroyed unchecked.
  auto *info = err.info();
  if (info) {
    // GetMessage() moves the message out, so it is called exactly once.
    auto msg = info->GetMessage();
    Log("%s: %s (errcode = %d)", what, msg.c_str(), errcode);
    return;
  }
  Log("%s: (no detail) errcode = %d", what, errcode);
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
  // Every peer starts from the end of the leader's log: whether it is up to
  // date is decided by the AppendEntries consistency check, not by guessing.
  const u64 last_index = last_log_index();
  for (auto &idx : next_indices_) {
    idx = last_index + 1;
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
  // A peer's term is newer than mine, so I am a stale leader/candidate.
  if (response.term() > term()) {
    SetTerm(response.term());
    BecomeFollower();
    return true; // to stop process, update state
  }
  return false;
}

bool Raft::CheckRequestTerm(Request &request, Response *response, void *ctx)
{
  // AppendEntries & RequestVote receiver rule 1
  if (request.term() < term()) {
    response->set_success(false);
    response->set_term(term());
    Log("Peer term (%llu) < current term (%llu)", (ull)request.term(),
        (ull)term());
    KRAFT_ASSERT1(send_message_cb_);
    send_message_cb_(response, ctx);
    return true; // to stop process, peer is a stale candidate/leader
  }

  // A newer term means the peer is the leader/candidate of that term.
  if (request.term() > term()) {
    Log("Peer term (%llu) > current term (%llu)", (ull)request.term(),
        (ull)term());
    SetTerm(request.term());
    // The reply was built before the term changed.
    response->set_term(term());
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
  // The log is up-to-date if its last entry has a higher term than the
  // request's, or if the terms are equal and its last entry has an equal or
  // higher index than the request's.
  return term > last_entry_meta.term ||
         (term == last_entry_meta.term && index >= last_entry_meta.index);
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
               "The message's id can't be invalid for response");
  response.set_from(self_node_.id);
  // `to` is a required field: a response that never sets it cannot even be
  // serialized, so it must be filled in here rather than by each caller.
  response.set_to(request.from());
  response.set_type(ResponseTypeOf(request.type()));
  response.set_term(term());
  return response;
}

void Raft::SendVoteRequestAll() const
{
  const auto last_entry_meta = log_get_last_entry_meta_cb_();
  Request request = MakeRequest(MSG_VOTE);

  // Used for checking whether the log is up-to-date or not
  request.set_log_index(last_entry_meta.index);
  request.set_log_term(last_entry_meta.term);

  for (auto &peer_node : peer_nodes_) {
    // FIXME should check peer_node.flags & NODE_ACTIVE
    request.set_to(peer_node.id);
    KRAFT_ASSERT1(send_message_cb_);
    send_message_cb_(&request, peer_node.ctx);
  }
}

auto Raft::MakeEntry(EntryType type, void const *data, u64 n) -> Entry
{
  Entry entry;
  entry.set_type(type);
  entry.set_term(term());
  // The log is the single source of truth for the index. A counter kept in
  // the instance drifts as soon as the log is truncated by a conflicting
  // AppendEntries or replayed from storage on restart.
  entry.set_index(last_log_index() + 1);
  if (data != nullptr && n > 0) {
    entry.set_data(data, n);
  }
  return entry;
}

auto Raft::AppendLogEntry(EntryType type, void const *data, u64 n) -> Error
{
  if (!IsLeader()) {
    errcode = E_NOT_ONLY_LEADER;
    return MakeMsgError("Only the leader can append an entry");
  }

  Entry entry = MakeEntry(type, data, n);
  const u64 entry_index = entry.index();
  if (!log_append_entry_cb_(entry)) {
    errcode = E_LOG_APPEND_ENTRY;
    return MakeMsgErrorf("Failed to log entry [%llu]", (ull)entry_index);
  }

  Log("Appended entry %llu of term %llu", (ull)entry_index, (ull)term());

  // Replicate immediately instead of waiting for the next heartbeat.
  for (size_t i = 0; i < peer_nodes_.size(); ++i) {
    auto err = SendAppendEntriesTo(i);
    if (err) {
      LogError("Failed to replicate the new entry", err);
      return err;
    }
  }

  return MakeSuccess();
}

auto Raft::FillAppendEntriesRequest(Request *request, u64 start_idx,
                                    u64 last_entry_index) const -> Error
{
  KRAFT_ASSERT(start_idx >= 1, "The next index must >= 1");

  // The request is reused across peers, so the entries built for the previous
  // peer must not leak into this one.
  request->clear_entries();

  // Index 0 is the sentinel of an empty log and must have term 0.
  // FIXME should append empty entry to log when the log is empty, 
  // so that the first entry's index is 1.
  request->set_log_index(start_idx - 1);
  request->set_log_term(log_get_entry_meta_cb_(start_idx - 1));

  if (start_idx > last_entry_index) {
    return MakeSuccess(); // nothing to replicate, a bare heartbeat
  }

  if (request->type() == MSG_AE) {
    request->mutable_entries()->Reserve(
        static_cast<int>(last_entry_index - start_idx + 1));
    for (u64 index = start_idx; index <= last_entry_index; ++index) {
      // The storage layer hands back a pointer to its own entry,
      // so the entry must be copied into the request
      Entry const *p_entry = nullptr;
      if (!log_get_entry_cb_(index, &p_entry)) {
        errcode = E_LOG_GET_ENTRY;
        return MakeMsgErrorf("Failed to get log entry in %llu", (ull)index);
      }
      if (p_entry == nullptr) {
        errcode = E_LOG_GET_ENTRY;
        return MakeMsgErrorf("The log entry in %llu is null", (ull)index);
      }
      *request->mutable_entries()->Add() = *p_entry;
    }
  }
  return MakeSuccess();
}

auto Raft::AppendConfLogEntry(ConfChangeType type, u64 id, void const *conf_ctx,
                              u64 conf_ctx_len) -> Error
{
  auto conf_chg = MakeConfChange(type, id, conf_ctx, conf_ctx_len);
  auto conf_chg_data = conf_chg.SerializeAsString();
  return AppendLogEntry(ENTRY_CONF, conf_chg_data.data(), conf_chg_data.size());
}

auto Raft::ApplyEntries() -> Error
{
  if (last_applied_ >= commit_) {
    return MakeSuccess();
  }

  Entry const *apply_entry = nullptr;
  for (u64 index = last_applied_ + 1; index <= commit_; ++index) {
    if (!log_get_entry_cb_(index, &apply_entry)) {
      errcode = E_LOG_GET_ENTRY;
      return MakeMsgErrorf("Failed to get log entry in %llu when apply entries",
                           (ull)index);
    }
    if (apply_entry == nullptr) {
      errcode = E_LOG_GET_ENTRY;
      return MakeMsgErrorf("The log entry in %llu is null when apply entries",
                           (ull)index);
    }

    // The host decides how to apply a conf-change entry and a normal
    // entry(user-defined data).
    if (!log_apply_entry_cb_(apply_entry)) {
      errcode = E_LOG_APPLY;
      return MakeMsgErrorf("Failed to apply log entry in %llu", (ull)index);
    }

    // Advance one entry at a time: a failure part-way through must not skip
    // the entries that already reached the application.
    last_applied_ = index;
  }

  Log("Apply successfully, applied index = %llu", (ull)last_applied_);
  return MakeSuccess();
}

// To expand, make LogState() as a single function call
bool Raft::LogState() const
{
  if (!log_set_state_) {
    // The instance exists before its host installs the callbacks; nothing to
    // persist yet, and calling an empty std::function would throw.
    Log("No persistent-state callback installed, state is not persisted");
    return false;
  }

  if (!log_set_state_(persistent_state_)) {
    Log("Failed to log state, old state is: (term: %llu, voted_for: %llu, "
        "id: %llu)",
        (ull)term(), (ull)voted_for(), (ull)id());
    return false;
  }
  return true;
}

auto Raft::IncrementCommitIndex() -> Error
{
  /* Leader Rule 4 */
  if (!IsLeader()) {
    return MakeSuccess();
  }

  const auto last_entry_meta = log_get_last_entry_meta_cb_();

  // The last entry has the maximum index, its term is old term entry
  if (last_entry_meta.term != term() || last_entry_meta.index <= commit_) {
    return MakeSuccess();
  }

  // The leader itself is the first replica of every entry it holds.
  const u64 majority = ((peer_nodes_.size() + 1) >> 1) + 1;

  for (u64 test_index = last_entry_meta.index; test_index > commit_;
       --test_index)
  {
    u64 match_num = 1; // self
    for (size_t i = 0; i < peer_nodes_.size(); ++i) {
      if (match_indices_[i] >= test_index) {
        ++match_num;
      }
    }

    if (match_num >= majority) {
      // Rule 4 only lets the leader commit an entry of its own term; older
      // entries commit implicitly once such an entry does.
      SetCommit(test_index);
      break;
    }

    // Walking further back would leave the entries of the current term.
    if (test_index == 1) break; // FIXME
    if (log_get_entry_meta_cb_(test_index - 1) != term()) break;
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

void Raft::SetTerm(u64 new_term)
{
  if (new_term == this->term()) return;

  persistent_state_.set_term(new_term);

  // A new term invalidates the vote of the previous one and any election
  // this instance was running. voted_for is cleared together with the term in
  // the same persisted state, otherwise a crash in between could let this
  // instance vote twice in one term.
  persistent_state_.set_voted_for(INVALID_VOTED_FOR);
  voted_num_ = 0;
  granted_voters_.clear();

  LogState();
}

void Raft::SetRole(RaftRole role)
{
  if (role_ == role) return;

  role_ = role;
  Log("State changed to %s of term %llu", RoleName(role_), (ull)term());

  if (on_role_change_cb_) {
    on_role_change_cb_(role_);
  }
}

void Raft::SetCommit(u64 commit)
{
  // The commit index never moves backwards.
  if (commit <= commit_) return;

  commit_ = commit;
  Log("Commit index advanced to %llu", (ull)commit_);

  if (on_commit_index_cb_) {
    on_commit_index_cb_(commit_);
  }
}

/*--------------------------------------------------*/
/* Public API                                       */
/*--------------------------------------------------*/

Raft::Raft(size_t peer_node_num)
{
  leader_id_ = INVALID_ID;
  commit_ = 0;
  last_applied_ = 0;
  voted_num_ = 0;
  last_leader_contact_ms_ = 0;
  // role_ is initialized to FOLLOWER by its default member initializer.

  // peer_node_num is a capacity hint only. The index vectors stay exactly as
  // long as peer_nodes_, so an index into one of them is always valid in all
  // of them, no matter in which order the host adds its nodes.
  peer_nodes_.reserve(peer_node_num);
  peer_node_map_.reserve(peer_node_num);
  next_indices_.reserve(peer_node_num);
  match_indices_.reserve(peer_node_num);
  granted_voters_.reserve(peer_node_num + 1);

  persistent_state_.set_id(INVALID_ID);
  persistent_state_.set_term(0);
  persistent_state_.set_voted_for(INVALID_VOTED_FOR);

  // No LogState() here: the callbacks are installed by the host after
  // construction, and IsReady() keeps the periodic entry points quiet until
  // they are.
}

Raft::~Raft() noexcept { Log("Raft is destoryed"); }

bool Raft::AddPeerNode(u64 id, void *user_ctx)
{
  if (id == INVALID_ID || id == self_node_.id) {
    errcode = E_ADD_PEER_NODE;
    return false;
  }

  // Insert into the map first: it owns the id -> index mapping, and a
  // duplicate that made it into peer_nodes_ would desync every vector that is
  // indexed alongside it.
  auto iter_res = peer_node_map_.emplace(id, peer_nodes_.size());
  if (!iter_res.second) {
    errcode = E_ADD_PEER_NODE;
    return false;
  }

  // Every added peer is an active voter for good: classic Raft never removes
  // a voter because it is temporarily unreachable, it only stops counting its
  // replication progress towards the commit index.
  peer_nodes_.emplace_back(id, user_ctx, NODE_ACTIVE | NODE_VOTING);

  const u64 last_index = last_log_index();
  next_indices_.push_back(last_index == INVALID_INDEX ? 1 : last_index + 1);
  match_indices_.push_back(0);
  return true;
}

bool Raft::AddSelfNode(u64 id, void *user_ctx)
{
  if (id == INVALID_ID) {
    errcode = E_INVALID_REQUEST;
    return false;
  }

  if (self_node_.id != INVALID_ID) {
    // The identity is part of the persisted state; rewriting it would let one
    // instance impersonate another across a restart.
    errcode = E_INCORRECT_STATE;
    return false;
  }

  self_node_.id = id;
  self_node_.ctx = user_ctx;

  SetId(id);
  return true;
}

auto Raft::Recover(u64 self_id) -> Error
{
  if (self_id == INVALID_ID) {
    errcode = E_INVALID_REQUEST;
    return MakeMsgError("The self id to recover as must be a valid id");
  }

  if (!log_get_state_cb_) {
    errcode = E_INCORRECT_STATE;
    return MakeMsgError("No persistent-state read callback is installed, "
                        "the state can't be recovered");
  }

  if (recover_done_) {
    errcode = E_INCORRECT_STATE;
    return MakeMsgError("The persistent state has already been recovered");
  }

  if (self_node_.id != INVALID_ID) {
    errcode = E_INCORRECT_STATE;
    return MakeMsgError("AddSelfNode() ran before Recover(): it persists the "
                        "identity it was given, so the state to recover has "
                        "already been overwritten");
  }

  // Anything already driven was driven on the constructor's fresh state, and
  // a recovered term would contradict the decisions it made in the meantime.
  if (term() != 0 || commit_ != 0 || last_applied_ != 0) {
    errcode = E_INCORRECT_STATE;
    return MakeMsgError("The instance has already been driven, it's too late "
                        "to recover its persistent state");
  }

  PersistentState persisted;
  if (!log_get_state_cb_(&persisted)) {
    // A node that never ran before: nothing was persisted, so the fresh state
    // the constructor installed is the right one. Not an error.
    recover_done_ = true;
    Log("No persistent state to recover, starting from a fresh state");
    return MakeSuccess();
  }

  if (persisted.id() != self_id) {
    errcode = E_INCORRECT_STATE;
    return MakeMsgErrorf("The persisted state belongs to node %llu, not to "
                         "node %llu",
                         (ull)persisted.id(), (ull)self_id);
  }

  // Assign instead of SetTerm()/SetId(): recovery is not a state transition,
  // so it must not fire a notification callback, and there is nothing to
  // persist, the recovered state being what is already on disk.
  persistent_state_ = persisted;
  recover_done_ = true;

  Log("Recovered the persistent state: term = %llu, voted_for = %llu, "
      "id = %llu",
      (ull)term(), (ull)voted_for(), (ull)id());
  return MakeSuccess();
}

void Raft::Periodic()
{
  if (!IsReady()) return;

  if (IsLeader()) {
    auto err = IncrementCommitIndex();
    if (err) {
      LogError("Failed to advance the commit index", err);
    }
  }

  auto err = ApplyEntries();
  if (err) {
    LogError("Failed to apply the committed entries", err);
  }
}

void Raft::ElectionPeriodic()
{
  if (!IsReady()) return;

  // A leader does not run elections; its own liveness is what the peers time.
  if (IsLeader()) return;

  BecomeCandidate();
}

/* Leader Rule 1 */
void Raft::HeartBeatPeriodic()
{
  if (!IsReady()) return;
  if (!IsLeader()) return;

  for (size_t i = 0; i < peer_nodes_.size(); ++i) {
    auto err = SendAppendEntriesTo(i, MSG_HEART_BEAT);
    auto &node = peer_nodes_[i];
    // FIXME
    if (node.IsRecvHeartBeat()) {
      node.flag |= NODE_ACTIVE;
    }
    if (err) {
      LogError("Failed to send heartbeat", err);
    }
    node.flag &= ~NODE_RECV_HEARTBEAT;
  }
}

auto Raft::SendAppendEntriesTo(u64 node_idx, MessageType type) -> Error
{
  KRAFT_ASSERT(IsLeader(), "Only the leader can replicate");
  KRAFT_ASSERT(node_idx < peer_nodes_.size(), "Invalid peer node index");
  if (type != MSG_AE && type != MSG_HEART_BEAT) {
    errcode = E_INCORRECT_MESSAGE_TYPE;
    return MakeMsgError("The message type must be MSG_AE or MSG_HEART_BEAT");
  }

  auto &node = peer_nodes_[node_idx];
  const u64 last_index = last_log_index();

  // A peer that is behind gets the entries it is missing; a peer that is
  // caught up only needs the leader's commit index.
  Request request = MakeRequest(type);
  request.set_to(node.id);
  request.set_commit(commit_);

  auto err =
      FillAppendEntriesRequest(&request, next_indices_[node_idx], last_index);
  if (err) {
    LogError("Failed to fill the AppendEntries request", err);
    return err;
  }

  KRAFT_ASSERT1(send_message_cb_);
  send_message_cb_(&request, node.ctx);
  return MakeSuccess();
}

void Raft::BecomeCandidate()
{
  // Raft §5.2: a follower that hears nothing from the leader starts a new
  // election at term + 1. The term and the self-vote are persisted in one
  // update because the vote is only meaningful together with its term.
  persistent_state_.set_term(term() + 1);
  persistent_state_.set_voted_for(self_node_.id);
  voted_num_ = 0;
  granted_voters_.clear();
  LogState();

  // Cleared before the role-change callback fires, like BecomeLeader() sets
  // leader_id_ before it: a host that reads leader_id() from inside the
  // callback must not see the leader of the term this candidacy just left.
  leader_id_ = INVALID_ID;
  SetRole(RaftRole::CANDIDATE);

  Log("Become candidate of term: %llu", (ull)term());

  // Vote for self, which is also the first vote counted by majority.
  granted_voters_.push_back(self_node_.id);
  voted_num_ = granted_voters_.size();

  // A single-node cluster elects itself without sending anything.
  if (IsMajorityVoted()) {
    BecomeLeader();
    return;
  }

  SendVoteRequestAll();
}

void Raft::BecomeLeader()
{
  KRAFT_ASSERT(!IsLeader(), "Can't convert a leader to a leader");

  // Reinitialized before the role-change callback fires: a host that appends
  // an entry from inside the callback must see the leader's bookkeeping.
  InitNextIndices();
  InitMatchIndices();
  leader_id_ = self_node_.id;

  SetRole(RaftRole::LEADER);
  Log("Voted num = %llu", (ull)voted_num_);
  Log("Become leader of term %llu", (ull)term());

  // Leader Rule 1: an entry of the leader's own term is what carries the
  // commit index forward (Raft §5.4.2), so append one before replicating.
  auto err = AppendNoOpLogEntry();
  if (err) {
    LogError("Failed to log no-op entry", err);
  }

  // Announce the new leadership to the peers.
  HeartBeatPeriodic();
}

void Raft::BecomeFollower()
{
  // Cleared before the role-change callback fires, so that a host reading
  // role()/leader_id() inside it observes the new state, as kraft.h promises.
  leader_id_ = INVALID_ID;

  // Idempotent: several paths step down, and only one of them changes state.
  SetRole(RaftRole::FOLLOWER);

  // Reset the election state of a previous candidacy. voted_for is *not*
  // reset: it is per term, and it is written by SetTerm()/BecomeCandidate().
  voted_num_ = 0;
  granted_voters_.clear();
}

/*--------------------------------------------------*/
/* Receive paths                                    */
/*--------------------------------------------------*/

// Follower/Candidate
auto Raft::RecvVoteRequest(Request &request) -> Error
{
  // A leader never grants a vote without stepping down first.
  if (IsLeader()) {
    return MakeSuccess();
  }

  CHECK_MESSAGE_(request, MSG_VOTE);

  auto &node = peer_nodes_[node_iter->second];
  Response response = MakeResponse(request);
  response.set_success(false);

  // RequestVote receiver rule 1: reject a stale term, telling the candidate
  // the current one.
  if (CheckRequestTerm(request, &response, node.ctx)) {
    return MakeSuccess();
  }

  // A candidate that hears another candidate of its own term keeps its own
  // candidacy; only a newer term (handled above) demotes it.

  const bool is_votable =
      voted_for() == INVALID_VOTED_FOR || voted_for() == request.from();
  if (is_votable && IsLogUptodate(request.log_term(), request.log_index())) {
    SetVotedFor(request.from());
    response.set_success(true);
    // Granting a vote resets the election timer: otherwise the candidate
    // would time out again in the middle of the election it just joined.
    RecordLeaderContact();
  }

  KRAFT_ASSERT1(send_message_cb_);
  send_message_cb_(&response, node.ctx);
  return MakeSuccess();
}

auto Raft::RecvVoteResponse(Response &response) -> Error
{
  // The election is over for this instance, either way.
  // Indicates the role has changed to follower or leader, ignore it.
  if (!IsCandidate()) {
    return MakeSuccess();
  }

  CHECK_MESSAGE_(response, MSG_VOTE_RES);

  auto &node = peer_nodes_[node_iter->second];

  if (CheckResponseTerm(response)) {
    return MakeSuccess();
  }

  // 检测消息不应用assert(has_success())，不清楚对端会塞什么
  if (!response.has_success() || !response.success()) {
    return MakeSuccess();
  }

  // Count each voter at most once: a replayed or forged duplicate must not
  // manufacture a majority.
  for (auto voter : granted_voters_) {
    if (voter == node.id) {
      return MakeSuccess();
    }
  }
  granted_voters_.push_back(node.id);
  voted_num_ = granted_voters_.size();
  Log("Receive the vote of %llu, voted num = %llu", (ull)node.id,
      (ull)voted_num_);

  if (IsMajorityVoted()) {
    BecomeLeader();
  }
  return MakeSuccess();
}

auto Raft::RecvHeartBeatResponse(Response &response) -> Error
{
  // A reply that arrives after this instance stopped being the leader has
  // nothing to say, and is not an error.
  // 在发和收心跳之间，可能有其他节点发起选举，导致本节点降级为follower
  if (!IsLeader()) {
    return MakeSuccess();
  }

  CHECK_MESSAGE_(response, MSG_HEART_BEAT_RES);

  if (CheckResponseTerm(response)) {
    return MakeSuccess();
  }

  return HandleAppendEntriesResponse(response, node_iter->second);
}

auto Raft::RecvHeartBeatRequest(Request &request) -> Error
{
  return HandleAppendEntriesRequest(request);
}

auto Raft::RecvAppendEntriesResponse(Response &response) -> Error
{
  if (!IsLeader()) {
    return MakeSuccess();
  }

  CHECK_MESSAGE_(response, MSG_AE_RES);

  if (CheckResponseTerm(response)) {
    return MakeSuccess();
  }

  return HandleAppendEntriesResponse(response, node_iter->second);
}

auto Raft::RecvAppendEntriesRequest(Request &request) -> Error
{
  return HandleAppendEntriesRequest(request);
}

// Leader
auto Raft::HandleAppendEntriesResponse(Response const &response, u64 node_idx)
    -> Error
{
  auto &node = peer_nodes_[node_idx];

  // Nothing to learn from a reply without a verdict: leave the peer's next
  // index untouched so the next heartbeat retries.
  if (!response.has_success()) {
    return MakeSuccess();
  }

  if (response.success()) {
    if (response.type() == MSG_AE_RES) {
      // The follower's hint is the last index its log now agrees with the
      // leader on, so match/next follow from it instead of from a guess.
      const u64 matched = response.hint();
      // 避免乱序到达的消息导致match_index回退
      // HeartBeat也有可能同步日志(见下面)
      match_indices_[node_idx] = KRAFT_MAX(matched, match_indices_[node_idx]);
      next_indices_[node_idx] = KRAFT_MAX(next_indices_[node_idx], matched + 1);
      Log("Peer %llu matched through %llu", (ull)node.id, (ull)matched);
    } else if (response.type() == MSG_HEART_BEAT_RES) {
      // Send remaining entries if the follower is behind, otherwise it is a bare
      // heartbeat that only updates the commit index.
      if (next_indices_[node_idx] <= last_log_index()) {
        auto err = SendAppendEntriesTo(node_idx);
        if (err) {
          LogError("Failed to send AppendEntries to peer", err);
          return err;
        }
      }
    }
    return MakeSuccess();
  }

  // The follower rejected the request; its hint is the index to retry from,
  // clamped so it always moves backwards and never leaves the log.
  const u64 last_index = last_log_index();
  const auto [ hint_index, _ ] = ComputeConflictHint(response.hint(), response.log_term());
  u64 next_index = KRAFT_MAX(hint_index, static_cast<u64>(1));
  if (next_index > last_index + 1) {
    next_index = last_index + 1;
  }
  next_indices_[node_idx] = next_index;
  Log("Peer %llu rejected the AppendEntries, retry from %llu", (ull)node.id,
      (ull)next_index);

  // Retry immediately instead of waiting for the next heartbeat.
  return SendAppendEntriesTo(node_idx);
}

// follower/candidate
auto Raft::HandleAppendEntriesRequest(Request &request) -> Error
{
  CHECK_MESSAGE_(request, MSG_AE, MSG_HEART_BEAT);

  auto &node = peer_nodes_[node_iter->second];
  Response response = MakeResponse(request);
  response.set_success(false);

  // Receiver rule 1: reply false to a stale term without touching the log.
  if (CheckRequestTerm(request, &response, node.ctx)) {
    return MakeSuccess();
  }

  // The sender's term is current, so it is the leader of this term and any
  // candidacy of this instance is over (Raft §5.2).
  BecomeFollower();
  leader_id_ = request.from();
  // A valid AppendEntries/heartbeat of the current term resets the election
  // timer, whether or not the log check below passes.
  RecordLeaderContact();

  const u64 prev_index = request.log_index();
  const u64 entries_size = static_cast<u64>(request.entries_size());

  // Receiver rule 2: the entry preceding the new ones must match. Index 0 is
  // the sentinel of an empty log and has term 0 on both sides.
  // 检查prevLogEntry是否存在，且term是否匹配
  const u64 term_in_prev_index = log_get_entry_meta_cb_(prev_index);
  if (prev_index > last_log_index() || term_in_prev_index != request.log_term()) {
    const auto [ hint_index, hint_term ] = ComputeConflictHint(prev_index, request.log_term());
    response.set_hint(hint_index);
    response.set_log_term(hint_term);
    Log("Reject the AppendEntries of %llu: no entry at %llu of term %llu",
        (ull)node.id, (ull)prev_index, (ull)request.log_term());
    KRAFT_ASSERT1(send_message_cb_);
    send_message_cb_(&response, node.ctx);
    return MakeSuccess();
  }

  if (request.type() == MSG_AE) {
    // Rules 3 and 4: skip the entries already in the log, then truncate the
    // first one that conflicts and append everything from there on.
    u64 index = prev_index;
    for (int i = 0; i < request.entries_size(); ++i) {
      Entry const &entry = request.entries(i);
      ++index;

      const u64 local_term = log_get_entry_meta_cb_(index);
      if (local_term != INVALID_TERM && local_term == entry.term()) {
        continue; // an old entry that is already replicated, keep it
      }

      if (local_term != INVALID_TERM) {
        // Rule 3: delete the conflicting entry and everything after it. The
        // callback keeps the log *through* the given index.
        // FIXME index?
        if (!log_truncate_after_cb_(index - 1)) {
          errcode = E_TRUNCATE;
          return MakeMsgErrorf("Failed to truncate the log after %llu",
                              (ull)(index - 1));
        }
      }

      for (int j = i; j < request.entries_size(); ++j) {
        // The storage layer may take ownership of the entry, hence the
        // non-const access, but the wire message itself is not modified.
        Entry &append_entry = *request.mutable_entries(j);
        if (!log_append_entry_cb_(append_entry)) {
          errcode = E_LOG_APPEND_ENTRY;
          return MakeMsgErrorf("Failed to append the log entry at index %llu",
                              (ull)append_entry.index());
        }
      }
      break;
    }
  } 
  // Rule 5: advance the commit index, but never past the last index this
  // request proved the leader holds.
  Log("AppendEntries accepted from %llu, prev_index = %llu, entries_size = %llu",
      (ull)node.id, (ull)prev_index, (ull)entries_size);
  // A heartbeat carries no entries, so the last index it proves is prev_index
  // itself; using the local log's tail instead would let a stale entry beyond
  // prev_index be committed (Raft §5.3).
  const u64 last_new_index = prev_index + entries_size;
  // commit不能回滚，只能单调递增
  if (request.commit() > commit_) {
    // 当日志同步还没跟得上leader和其他大多数节点时，
    // 不能简单设置commit = request.commit()，否则可能提交了旧日志或者对应日志根本不存在
    // last_new_index 到 last log index 之间的日志可能是旧的日志，不能提交，因此取
    // 当前已复制的日志Index（last_new_index < request.commit()）是安全的
    SetCommit(KRAFT_MIN(request.commit(), last_new_index));
  }
  response.set_hint(last_new_index);

  // On success the hint is the index the follower's log now agrees with the
  // leader on.
  response.set_success(true);
  response.set_term(term());
  KRAFT_ASSERT1(send_message_cb_);
  send_message_cb_(&response, node.ctx);
  return MakeSuccess();
}

auto Raft::ComputeConflictHint(u64 prev_log_index, u64 peer_reject_term) const noexcept -> std::tuple<u64, u64>
{
  const u64 last_index = last_log_index();

  auto hint_index = KRAFT_MIN(prev_log_index, last_index);
  while (hint_index > 1 && log_get_entry_meta_cb_(hint_index - 1) > peer_reject_term) {
    --hint_index;
  }
  return { hint_index, log_get_entry_meta_cb_(hint_index)};
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
  // `to` is a required field and the responder is not known yet, so the
  // message is addressed to nobody and the responder answers the sender.
  request.set_to(INVALID_ID);
  KRAFT_ASSERT1(send_message_cb_);
  send_message_cb_(&request, user_ctx);

  return MakeSuccess();
}

auto Raft::RecvAskNodeResponse(Response &response, void *user_ctx) -> Error
{
  if (response.type() != MSG_ASK_ID_RES) {
    errcode = E_INCORRECT_MESSAGE_TYPE;
    return MakeNoInfoError();
  }

  if (response.to() != self_node_.id) {
    errcode = E_INVALID_RESPONSE;
    return MakeNoInfoError();
  }

  if (response.from() == self_node_.id || response.from() == INVALID_ID) {
    errcode = E_INVALID_RESPONSE;
    return MakeNoInfoError();
  }

  // Has added, ignore this response
  // Maybe a duplicate message since network problem.
  if (peer_node_map_.find(response.from()) != peer_node_map_.end()) {
    return MakeSuccess();
  }

  if (!AddPeerNode(response.from(), user_ctx)) {
    return MakeNoInfoError(); // errcode is set by AddPeerNode()
  }
  return MakeSuccess();
}
