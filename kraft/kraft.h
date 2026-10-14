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

// NOTE: HB: Heart Beat
enum NodeFlag : u8 {
  NODE_NONE = 0,   // INVALID state
  NODE_ACTIVE = 1, // HB Request is OK
  NODE_VOTING = (1 << 1),
  NODE_RECV_HARTBEAT = (1 << 2),
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
    return flag & NODE_RECV_HARTBEAT;
  }
};

/**
 * Represents a Raft Instance
 */
class Raft {
 public:
  // FIXME init state?
  enum class RaftState : u8 {
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

  Error RecvVoteResponse(Response &rsp);
  Error RecvVoteRequest(Request &req);
  Error RecvHeartBeatResponse(Response &req);
  Error RecvHeartBeatRequest(Response &rep);
  Error RecvAppendEntriesResponse(Response &rep);
  Error RecvAppendEntriesRequest(Request &req);

  KRAFT_INLINE bool IsLeader() const noexcept
  {
    return state_ == RaftState::LEADER;
  }
  KRAFT_INLINE bool IsFollower() const noexcept
  {
    return state_ == RaftState::FOLLOWER;
  }
  KRAFT_INLINE bool IsCandidate() const noexcept
  {
    return state_ == RaftState::CANDIDATE;
  }

 private:
  /*--------------------------------------------------*/
  /* State getter                                     */
  /*--------------------------------------------------*/

  void BecomeLeader();
  void BecomeFollower();
  void BecomeCandidate();

  KRAFT_INLINE RaftState state() const noexcept { return state_; }

  KRAFT_INLINE u64 term() const noexcept { return persistent_state_.term(); }

  KRAFT_INLINE u64 voted_for() const noexcept
  {
    return persistent_state_.voted_for();
  }

  KRAFT_INLINE u64 id() const noexcept { return persistent_state_.id(); }

  KRAFT_INLINE bool IsMajorityVoted() const noexcept
  {
    return voted_num_ > ((peer_nodes_.size() + 1) >> 1);
  }

  KRAFT_INLINE u64 leader_id() const noexcept { return leader_id_; }

  KRAFT_INLINE void *leader_context() noexcept
  {
    if (leader_id_ == INVALID_INDEX) {
      return nullptr;
    }

    auto node_iter = peer_node_map_.find(leader_id_);
    KRAFT_ASSERT(node_iter != peer_node_map_.end(),
                 "The leader id must be a existed node");
    KRAFT_ASSERT(node_iter->first == peer_nodes_[node_iter->second].id, "");
    return peer_nodes_[node_iter->second].ctx;
  }

 private:
  /*--------------------------------------------------*/
  /* Impl helpers(defined in kraft.cc)                */
  /*--------------------------------------------------*/

  void Log(char const *fmt, ...) const;

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

  bool LogState() const;
  void SetId(u64 id);
  void SetVotedFor(u64 id);
  void SetTerm(u64 term);

 private:
  std::vector<Node> peer_nodes_;
  HashMap<u64, u64> peer_node_map_; // NodeId -> NodeIndex
  Node self_node_;

  // raft state
  RaftState state_;
  PersistentState persistent_state_;
  u64 entry_index_counter_;

  // For redirect
  u64 leader_id_;

  u64 commit_;
  u64 last_applied_;

  // candidate state
  u64 voted_num_;
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
  /* IO Callback(Send/Recv Message)                   */
  /*--------------------------------------------------*/

  using SendCallback =
      std::function<void(::google::protobuf::Message *msg, void *user_ctx)>;
  SendCallback send_message_cb_;
};

} // namespace kraft

#endif
