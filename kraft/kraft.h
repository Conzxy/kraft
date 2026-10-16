// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_H__
#define _KRAFT_H__

#include <cstdint>
#include <functional>
#include <vector>

#include "kraft.pb.h"
#include "kraft/macro.h"
#include "kraft/type.h"

#include <kerror/kerror.h>

namespace kraft {

using kerror::Error;

// Dummy type to distinguish the request
// and response of raft message
//
// The usage is explained by user
using Request = Message;
using Response = Message;

// A monotonic clock, in milliseconds.
//
// The library never reads the wall clock by itself: the host injects the
// clock so a Raft instance can share the host's time source, and so tests
// can drive the state machine deterministically.
using Clock = std::function<u64()>;

// The clock used when the host injects nothing.
u64 SteadyClockMs() noexcept;

// NOTE: HB: Heart Beat
enum NodeFlag : u8 {
  NODE_NONE = 0,   // INVALID state
  NODE_ACTIVE = 1, // HB Request is OK
  NODE_VOTING = (1 << 1),
  NODE_RECV_HEARTBEAT = (1 << 2),
};

struct Node {
  u64 id = -1;
  void *ctx = nullptr; // context
  u8 flag = (NODE_ACTIVE | NODE_VOTING);

  Node() = default;

  // Just used for perfect forwarding.
  Node(u64 id_, void *ctx_, u8 flag_ = NODE_NONE) noexcept
    : id(id_)
    , ctx(ctx_)
    , flag(flag_)
  {
  }

  KRAFT_INLINE bool IsActive() const noexcept { return flag & NODE_ACTIVE; }

  KRAFT_INLINE bool IsVoting() const noexcept { return flag & NODE_VOTING; }

  KRAFT_INLINE bool IsRecvHeartBeat() const noexcept
  {
    return flag & NODE_RECV_HEARTBEAT;
  }
};

/**
 * Represents a Raft Instance
 */
class Raft {
 public:
  // FIXME init state?
  enum class RaftRole : u8 {
    FOLLOWER = 0,
    CANDIDATE,
    LEADER,
  };

  explicit Raft(size_t peer_node_num = 0);
  ~Raft() noexcept;

  // FIXME move operation
  KRAFT_DISABLE_MOVE(Raft);
  KRAFT_DISABLE_COPY(Raft);

  /*--------------------------------------------------*/
  /* Node control                                     */
  /*--------------------------------------------------*/

  bool AddPeerNode(u64 id, void *user_ctx);
  bool AddSelfNode(u64 id, void *user_ctx = nullptr);

  /**
   * \brief
   *
   * \param user_ctx
   *
   * \return
   *
   * \errno
   */
  Error AskNodeId(void *user_ctx);
  Error RecvAskNodeResponse(Response &response, void *user_ctx);

  /*--------------------------------------------------*/
  /* Host-provided environment                        */
  /*--------------------------------------------------*/

  // Install the clock the raft instance reads. Without one, SteadyClockMs()
  // is used. The clock must be monotonic and never go backwards.
  void SetClock(Clock clock) noexcept { clock_ = std::move(clock); }

  KRAFT_INLINE u64 NowMs() const noexcept
  {
    return clock_ ? clock_() : SteadyClockMs();
  }

  /**
   * \brief Whether every storage/IO callback the state machine needs is
   * installed.
   *
   * An instance is constructed before its host can install the callbacks, so
   * the periodic entry points stay no-ops until this becomes true. The host
   * can therefore start driving the instance without having to order
   * "install everything" before "construct" or "start".
   */
  KRAFT_INLINE bool IsReady() const noexcept
  {
    return log_append_entry_cb_ && log_set_state_ &&
           log_get_last_entry_meta_cb_ && log_get_entry_meta_cb_ &&
           log_truncate_after_cb_ && log_get_entry_cb_ && log_apply_entry_cb_ &&
           send_message_cb_;
  }

  /*--------------------------------------------------*/
  /* Periodic operation                               */
  /*--------------------------------------------------*/

  void Periodic();
  void ElectionPeriodic();
  void HeartBeatPeriodic();

  /*--------------------------------------------------*/
  /* State machine                                    */
  /*--------------------------------------------------*/

  Error AppendLog(void const *data, size_t n);
  Error AppendConfChangeLog(u64 id, void const *conf_ctx, size_t n);

  Error RecvVoteResponse(Response &response);
  Error RecvVoteRequest(Request &request);
  Error RecvHeartBeatResponse(Response &response);
  Error RecvHeartBeatRequest(Request &request);
  Error RecvAppendEntriesResponse(Response &response);
  Error RecvAppendEntriesRequest(Request &request);

  KRAFT_INLINE bool IsLeader() const noexcept
  {
    return role_ == RaftRole::LEADER;
  }
  KRAFT_INLINE bool IsFollower() const noexcept
  {
    return role_ == RaftRole::FOLLOWER;
  }
  KRAFT_INLINE bool IsCandidate() const noexcept
  {
    return role_ == RaftRole::CANDIDATE;
  }

  /*--------------------------------------------------*/
  /* State getter                                     */
  /*--------------------------------------------------*/

  // These getters are the read-only view of the raft instance.
  // The host needs them to drive the raft instance, e.g. comparing
  // a peer's term with term() decides whether the election timer
  // should be reset.

  KRAFT_INLINE RaftRole role() const noexcept { return role_; }

  KRAFT_INLINE u64 term() const noexcept { return persistent_state_.term(); }

  KRAFT_INLINE u64 voted_for() const noexcept
  {
    return persistent_state_.voted_for();
  }

  KRAFT_INLINE u64 id() const noexcept { return persistent_state_.id(); }

  KRAFT_INLINE u64 commit_index() const noexcept { return commit_; }

  KRAFT_INLINE u64 last_applied() const noexcept { return last_applied_; }

  KRAFT_INLINE u64 leader_id() const noexcept { return leader_id_; }

  /**
   * \brief The last time (in ms, on the injected clock) at which this
   * instance was contacted by the leader it currently recognizes, or
   * granted its vote to a candidate.
   *
   * This is the stamp an election-timeout driver compares against its own
   * deadline, so that a follower resets its timer exactly when the Raft
   * rules say it must, without the driver having to re-derive those rules
   * from the message stream.
   *
   * \return 0 if the instance has never been contacted.
   */
  KRAFT_INLINE u64 last_leader_contact_ms() const noexcept
  {
    return last_leader_contact_ms_;
  }

  /**
   * \brief The index of the last entry of the local log.
   *
   * \return INVALID_INDEX if the log storage callback is not installed yet.
   */
  KRAFT_INLINE u64 last_log_index() const noexcept
  {
    if (!log_get_last_entry_meta_cb_) {
      return INVALID_INDEX;
    }
    return log_get_last_entry_meta_cb_().index;
  }

  /**
   * \brief The term of the last entry of the local log.
   *
   * \return INVALID_TERM if the log storage callback is not installed yet.
   */
  KRAFT_INLINE u64 last_log_term() const noexcept
  {
    if (!log_get_last_entry_meta_cb_) {
      return INVALID_TERM;
    }
    return log_get_last_entry_meta_cb_().term;
  }

  /**
   * \brief The host context of the node this instance currently recognizes
   * as the leader, so that a follower can redirect a client to it.
   *
   * \return nullptr if no leader is known, or if this instance is the leader
   * itself in a single-node cluster (the leader is not in peer_node_map_).
   */
  KRAFT_INLINE void *leader_context() const noexcept
  {
    if (leader_id_ == INVALID_ID) {
      return nullptr;
    }
    if (leader_id_ == self_node_.id) {
      return self_node_.ctx;
    }

    auto node_iter = peer_node_map_.find(leader_id_);
    if (node_iter == peer_node_map_.end()) {
      return nullptr;
    }
    return peer_nodes_[node_iter->second].ctx;
  }

 private:
  /*--------------------------------------------------*/
  /* State transition(defined in kraft.cc)            */
  /*--------------------------------------------------*/

  void BecomeLeader();
  void BecomeFollower();
  void BecomeCandidate();

  // The single place where state_ / commit_ change, so that the host
  // callbacks fire on every transition, no matter which path caused it.
  void SetRole(RaftRole state);
  void SetCommit(u64 commit);

  // Called whenever this instance accepts a message from the leader whose
  // term it currently follows (a valid AppendEntries/heartbeat), or grants
  // its vote to a candidate. Both reset the election timer.
  void RecordLeaderContact() noexcept
  {
    last_leader_contact_ms_ = NowMs();
  }

  KRAFT_INLINE bool IsMajorityVoted() const noexcept
  {
    return voted_num_ > ((peer_nodes_.size() + 1) >> 1);
  }

 private:
  /*--------------------------------------------------*/
  /* Impl helpers(defined in kraft.cc)                */
  /*--------------------------------------------------*/

  void Log(char const *fmt, ...) const;

  // Extract and log an Error without tripping kerror's unchecked-error
  // abort: info() marks the error checked.
  void LogError(char const *what, Error const &err) const;

  static ConfChange MakeConfChange(ConfChangeType conf_type, u64 id,
                                   void const *conf_ctx, size_t conf_ctx_len);

  void InitNextIndices() noexcept;
  void InitMatchIndices() noexcept;

  bool CheckResponseTerm(Response const &response);
  bool CheckRequestTerm(Request &request, Response *response, void *ctx);
  bool IsLogUptodate(u64 term, u64 index) const noexcept;

  Request MakeRequest(MessageType type) const;
  Response MakeResponse(Request const &request) const;
  void SendVoteRequestAll() const;

  Entry MakeEntry(EntryType type, void const *data, u64 n);
  Error AppendLogEntry(EntryType type, void const *data, u64 n);
  Error FillAppendEntriesRequest(Request *request, u64 start_idx,
                                 u64 last_entry_index) const;
  Error AppendConfLogEntry(ConfChangeType type, u64 id, void const *conf_ctx,
                           u64 conf_ctx_len);
  Error AppendNoOpLogEntry();
  Error ApplyEntries();
  Error IncrementCommitIndex();

  // Leader -> follower replication. Picks AppendEntries when the peer is
  // behind, a bare heartbeat otherwise, and reports why nothing was sent.
  Error SendAppendEntriesTo(u64 node_idx);
  Error SendHeartBeatTo(u64 node_idx);

  // The single replication core shared by RecvHeartBeatRequest() and
  // RecvAppendEntriesRequest(): an AppendEntries carrying no entries is a
  // heartbeat, so both message types take the same path.
  Error HandleAppendEntriesRequest(Request &request);

  Error HandleAppendEntriesResponse(Response const &response, u64 node_idx);

  // The index the leader should retry from after a rejected AppendEntries,
  // derived from the follower's hint.
  std::tuple<u64, u64> ComputeConflictHint(u64 prev_log_index, u64 reject_term) const noexcept;

  bool LogState() const;
  void SetId(u64 id);
  void SetVotedFor(u64 id);
  void SetTerm(u64 new_term);

 private:
  std::vector<Node> peer_nodes_;
  HashMap<u64, u64> peer_node_map_; // NodeId -> NodeIndex
  Node self_node_;

  // raft state
  RaftRole role_ = RaftRole::FOLLOWER;
  PersistentState persistent_state_;

  // For redirect
  u64 leader_id_;

  u64 commit_;
  u64 last_applied_;

  // Host-provided time source and the last moment the leader was heard from
  Clock clock_;
  u64 last_leader_contact_ms_ = 0;

  // candidate state
  u64 voted_num_;
  // Ids of the peers that granted their vote in the current election, so a
  // duplicate or replayed response cannot be counted twice.
  std::vector<u64> granted_voters_;
  // leader state
  std::vector<u64> next_indices_;
  std::vector<u64> match_indices_;

  // no replicated log entries member.
  // The raft instance will call the callback to append log entry to storage layer.
  // The implementation of storage layer is managed by user, 
  // and the raft instance will call the callback to append log entry to storage layer.
 public:
  /*--------------------------------------------------*/
  /* Log Callback(for debug)                          */
  /*--------------------------------------------------*/

  std::function<void(char const *data, size_t n)> log_cb_;
  std::function<void()> flush_cb_;

  /*--------------------------------------------------*/
  /* Log storage Engine Callback(for log entry)       */
  /*--------------------------------------------------*/

  // Log storage engine interface.
  // I use callback as it is more flexible than virtual function.
  //
  // The performance of std::function<> and virtual function is likely
  // same(Release mode).
  // YOU don't care the performance problem.

  // Storage layer
  std::function<bool(Entry &entry)> log_append_entry_cb_;
  std::function<bool(PersistentState const &state)> log_set_state_;
  std::function<EntryMeta()> log_get_last_entry_meta_cb_;
  std::function<u64(u64 index)> log_get_entry_meta_cb_;
  std::function<bool(u64 index)> log_truncate_after_cb_;

  // User must ensure this point is valid when call this function
  // even though in multithread environment
  // \param entry is pointer to a const entry
  std::function<bool(u64 index, Entry const **entry)> log_get_entry_cb_;

  // Application layer
  std::function<bool(Entry const *)> log_apply_entry_cb_;

  /*--------------------------------------------------*/
  /* Host Notification Callback                       */
  /*--------------------------------------------------*/

  // The host observes the instance through these instead of polling
  // IsLeader()/commit_index() on its own cadence.
  //
  // on_role_change_cb_ fires after state_ has changed, so calling
  // state()/leader_id() inside the callback observes the new state.
  std::function<void(RaftRole)> on_role_change_cb_;

  // Fires after commit_ advanced, with the new commit index.
  std::function<void(u64)> on_commit_index_cb_;

  /*--------------------------------------------------*/
  /* IO Callback(Send/Recv Message)                   */
  /*--------------------------------------------------*/

  using SendCallback =
      std::function<void(::google::protobuf::Message *msg, void *user_ctx)>;
  SendCallback send_message_cb_;
};

} // namespace kraft

#endif
